#pragma once

#include <cstring>

#include "common/index_runtime.h"
#include "common/types.h"
#include "execution_common.h"
#include "index_range_traversal.h"
#include "index_visibility_cursor.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"
#include "transaction/transaction_manager.h"

class CountIndexAggregateExecutor : public AbstractExecutor {
   private:
    struct CompiledKeyCond {
        int lhs_offset = 0;
        int rhs_offset = 0;
        int len = 0;
        CompiledComparator comparator = CompiledComparator::kInvalid;
        const char *rhs_value = nullptr;
        bool rhs_is_value = false;
    };

    SmManager *sm_manager_ = nullptr;
    std::string tab_name_;
    std::string visible_name_;
    const TabMeta *tab_ = nullptr;
    std::vector<Condition> conds_;
    std::vector<std::string> index_col_names_;
    IndexMeta index_meta_;
    RmFileHandle *fh_ = nullptr;
    IxIndexHandle *ih_ = nullptr;
    std::vector<ColMeta> index_key_cols_;
    std::vector<CompiledKeyCond> compiled_key_conds_;
    std::vector<ColMeta> full_cols_;
    std::vector<ColMeta> cols_;
    size_t len_ = sizeof(int);
    rmdb::IndexRangeSpec range_spec_;
    IndexRangeTraversal range_traversal_;
    VisibleIndexCursor visible_index_cursor_;
    RmRecord current_record_;
    RmRecord output_tuple_;
    size_t cursor_ = 0;
    bool has_tuple_ = false;
    rmdb::RuntimeScanFeedbackCollector feedback_;
    bool compiled_key_conds_valid_ = false;
    std::shared_ptr<const rmdb::CompiledAccessProgram> access_program_;

    void compile_index_key_cols() {
        index_key_cols_.clear();
        int key_offset = 0;
        for (const auto &index_col : index_meta_.cols) {
            ColMeta col = index_col;
            col.tab_name = visible_name_;
            col.offset = key_offset;
            index_key_cols_.push_back(col);
            key_offset += index_col.len;
        }
    }

    void compile_key_conds() {
        compiled_key_conds_.clear();
        compiled_key_conds_.reserve(conds_.size());
        compiled_key_conds_valid_ = true;
        for (const auto &cond : conds_) {
            auto lhs_col = get_col(index_key_cols_, cond.lhs_col);
            CompiledKeyCond compiled;
            compiled.lhs_offset = lhs_col->offset;
            compiled.len = lhs_col->len;
            compiled.comparator = compile_comparator(lhs_col->type, cond.op);
            if (compiled.comparator == CompiledComparator::kInvalid) {
                compiled_key_conds_.clear();
                compiled_key_conds_valid_ = false;
                return;
            }
            compiled.rhs_is_value = cond.is_rhs_val;
            if (cond.is_rhs_val) {
                if (cond.rhs_val.raw == nullptr) {
                    compiled_key_conds_.clear();
                    compiled_key_conds_valid_ = false;
                    return;
                }
                compiled.rhs_value = cond.rhs_val.raw->data;
            } else {
                auto rhs_col = get_col(index_key_cols_, cond.rhs_col);
                if (rhs_col->type != lhs_col->type || rhs_col->len != lhs_col->len) {
                    compiled_key_conds_.clear();
                    compiled_key_conds_valid_ = false;
                    return;
                }
                compiled.rhs_offset = rhs_col->offset;
            }
            compiled_key_conds_.push_back(compiled);
        }
    }

    bool eval_key_conds(const char *key) const {
        if (compiled_key_conds_valid_) {
            for (const auto &cond : compiled_key_conds_) {
                const char *lhs = key + cond.lhs_offset;
                const char *rhs = cond.rhs_is_value ? cond.rhs_value : key + cond.rhs_offset;
                if (!eval_compiled_comparator(cond.comparator, lhs, rhs, cond.len)) {
                    return false;
                }
            }
            return true;
        }
        RmRecord key_record = RmRecord::borrow(key, index_meta_.logical_col_tot_len());
        return eval_conds(index_key_cols_, &key_record, conds_);
    }

    bool key_conds_match(const char *key) const {
        return range_spec_.all_conditions_consumed || eval_key_conds(key);
    }

    bool entry_counts(const Rid &rid, const char *key) {
        auto visible = visible_index_cursor_.read_current(rid, key, &current_record_, false, true);
        feedback_.add_heap_fetches(visible.heap_fetched ? 1 : 0);
        if (!visible) {
            return false;
        }
        return key_conds_match(key);
    }

    int count_visible_entries() {
        int count = 0;
        IxReadGuard read_guard = ih_->make_read_guard();
        auto scan = range_traversal_.open_scan();
        page_id_t clean_page_no = RM_NO_PAGE;
        bool stop_scan = false;
        while (!stop_scan && scan != nullptr && !scan->is_end()) {
            int slot = scan->current_leaf_slot();
            const int leaf_end = scan->current_leaf_scan_end_slot();
            int processed = 0;
            while (slot < leaf_end) {
                const char *key = scan->current_leaf_key_at(slot);
                // 逻辑范围复核:即便 all_conditions_consumed 也不完全信任扫描
                // 结束边界(物理 Iid 在叶页槽位移动后会失效)。
                if (!range_traversal_.logical_range_contains(key)) {
                    stop_scan = true;
                    break;
                }
                feedback_.add_index_entries();
                const Rid rid = scan->current_leaf_rid_at(slot);
                processed++;

                if (rid.page_no == clean_page_no) {
                    bool clean_visible = visible_index_cursor_.current_page_still_clean_visible(rid.page_no);
                    if (clean_visible && range_spec_.all_conditions_consumed) {
                        // 整页快速计数：页面保持 clean 时，可直接统计同页条目。
                        // 并发写入会使页面状态失效；每次迭代重新检查，发现变化后
                        // 退出快速路径，剩余条目按逐条可见性路径处理。
                        count++;
                        slot++;
                        while (slot < leaf_end) {
                            const char *next_key = scan->current_leaf_key_at(slot);
                            if (!range_traversal_.logical_range_contains(next_key)) {
                                stop_scan = true;
                                break;
                            }
                            const Rid next_rid = scan->current_leaf_rid_at(slot);
                            if (next_rid.page_no != clean_page_no) {
                                break;
                            }
                            if (!visible_index_cursor_.current_page_still_clean_visible(rid.page_no)) {
                                break;
                            }
                            feedback_.add_index_entries();
                            count++;
                            processed++;
                            slot++;
                        }
                        if (stop_scan) {
                            break;
                        }
                        // 退出快速路径:若当前 slot 仍是本页条目(因 clean 检查
                        // 失败退出),必须走逐条慢路径——原逻辑此处直接跳过,
                        // 造成漏计数。跨页退出则由外层循环下一轮处理。
                        if (slot < leaf_end &&
                            scan->current_leaf_rid_at(slot).page_no == clean_page_no) {
                            const Rid fallback_rid = scan->current_leaf_rid_at(slot);
                            const char *fallback_key = scan->current_leaf_key_at(slot);
                            feedback_.add_index_entries();
                            if (entry_counts(fallback_rid, fallback_key)) {
                                count++;
                                visible_index_cursor_.record_current_rid(fallback_rid);
                            }
                            processed++;
                            slot++;
                        }
                        continue;
                    }
                    if (clean_visible && key_conds_match(key)) {
                        count++;
                        slot++;
                        continue;
                    }
                    // 页面状态已变化(原逻辑隐藏 bug:直接跳过漏计数)。
                    // 当前条目走逐条慢路径,与 MIN 可见性对齐。
                    if (entry_counts(rid, key)) {
                        count++;
                        visible_index_cursor_.record_current_rid(rid);
                    }
                    clean_page_no =
                        visible_index_cursor_.current_page_clean_visible() ? rid.page_no : RM_NO_PAGE;
                    slot++;
                    continue;
                }

                if (entry_counts(rid, key)) {
                    count++;
                    visible_index_cursor_.record_current_rid(rid);
                }
                clean_page_no = visible_index_cursor_.current_page_clean_visible() ? rid.page_no : RM_NO_PAGE;
                slot++;
            }
            if (!stop_scan) {
                scan->advance_current_leaf(processed);
            }
        }
        read_guard.reset();
        // snapshot-index history 在 current 扫描结束后才加载,并对已计数的 RID
        // 去重,补齐"history 提前缓存后 current 项被并发移除"的旧快照漏行。
        for (const auto &rid : visible_index_cursor_.history_rids()) {
            auto visible = visible_index_cursor_.read_visible_entry(rid, &current_record_);
            feedback_.add_heap_fetches(visible.heap_fetched ? 1 : 0);
            if (visible && eval_conds(full_cols_, &current_record_, conds_)) {
                count++;
            }
        }
        return count;
    }

    void write_count(int value) {
        output_tuple_ = RmRecord(static_cast<int>(len_));
        std::memcpy(output_tuple_.data, &value, sizeof(int));
    }

   public:
    CountIndexAggregateExecutor(SmManager *sm_manager,
                                std::string tab_name,
                                std::string visible_name,
                                std::vector<Condition> conds,
                                std::vector<std::string> index_col_names,
                                std::vector<TabCol> output_cols,
                                Context *context,
                                std::shared_ptr<const PlanRuntimeCache> runtime_cache = nullptr,
                                std::shared_ptr<rmdb::RuntimeNodeFeedback> runtime_feedback = nullptr) {
        sm_manager_ = sm_manager;
        context_ = context;
        feedback_.bind(std::move(runtime_feedback), rmdb::RuntimeNodeKind::kCountIndex);
        tab_name_ = std::move(tab_name);
        visible_name_ = visible_name.empty() ? tab_name_ : std::move(visible_name);
        conds_ = std::move(conds);
        index_col_names_ = std::move(index_col_names);

        const bool use_cache = runtime_cache != nullptr && runtime_cache->has_table;
        tab_ = &sm_manager_->db_.get_table(tab_name_);
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        if (use_cache) {
            full_cols_ = runtime_cache->full_cols;
        } else {
            full_cols_ = tab_->cols;
            for (auto &col : full_cols_) {
                col.tab_name = visible_name_;
            }
        }
        index_meta_ = runtime_cache != nullptr && runtime_cache->has_index
                          ? runtime_cache->index_meta
                          : *(tab_->get_index_meta(index_col_names_, true));
        ih_ = rmdb::resolve_index_handle(sm_manager_, tab_name_, index_meta_);
        compile_index_key_cols();
        compile_key_conds();
        if (runtime_cache != nullptr) access_program_ = runtime_cache->access_program;
        bool compiled_range = access_program_ != nullptr &&
                              access_program_->Matches(index_meta_, conds_.size(), rmdb::sql_template_schema_epoch()) &&
                              access_program_->MaterializeRange(index_meta_, conds_, &range_spec_);
        if (!compiled_range) {
            range_spec_ = rmdb::build_index_range_spec(index_meta_, conds_, visible_name_);
        }
        range_traversal_.bind(ih_, &range_spec_);
        visible_index_cursor_.bind(context_ == nullptr ? nullptr : context_->txn_mgr_,
                                   context_ == nullptr ? nullptr : context_->txn_, tab_name_,
                                   &index_col_names_, &index_meta_, fh_);

        ColMeta output_col;
        output_col.tab_name = "";
        output_col.name = output_cols.empty() ? "count" : output_cols[0].col_name;
        output_col.offset = 0;
        output_col.type = TYPE_INT;
        output_col.len = sizeof(int);
        cols_.push_back(output_col);
    }

    void beginTuple() override {
        feedback_.begin();
        cursor_ = 0;
        has_tuple_ = true;
        visible_index_cursor_.reset();
        int count = count_visible_entries();
        visible_index_cursor_.finish();
        feedback_.add_rows_visible(static_cast<rmdb::u64>(count));
        feedback_.add_rows_output(1);
        write_count(count);
        feedback_.flush();
    }

    void nextTuple() override {
        if (cursor_ == 0) {
            cursor_ = 1;
        }
    }

    bool is_end() const override { return !has_tuple_ || cursor_ > 0; }
    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta> &cols() const override { return cols_; }
    std::string getType() override { return "CountIndexAggregateExecutor"; }
    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }
    Rid &rid() override { return _abstract_rid; }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end()) {
            return nullptr;
        }
        auto rec = std::make_unique<RmRecord>(static_cast<int>(len_));
        std::memcpy(rec->data, output_tuple_.data, len_);
        return rec;
    }

    const RmRecord *CurrentTuple() const override {
        return is_end() ? nullptr : &output_tuple_;
    }
};
