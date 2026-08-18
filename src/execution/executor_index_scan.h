/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include <algorithm>
#include <utility>

#include "common/index_runtime.h"
#include "common/sql_template_cache.h"
#include "common/types.h"
#include "execution_common.h"
#include "index_range_traversal.h"
#include "index_visibility_cursor.h"
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"
#include "transaction/transaction_manager.h"

class IndexScanExecutor : public AbstractExecutor {
   private:
    static CompOp swap_comparison_op(CompOp op) {
        switch (op) {
            case OP_EQ: return OP_EQ;
            case OP_NE: return OP_NE;
            case OP_LT: return OP_GT;
            case OP_GT: return OP_LT;
            case OP_LE: return OP_GE;
            case OP_GE: return OP_LE;
        }
        return op;
    }

    std::string tab_name_;                      // 表名称
    std::string visible_name_;
    const TabMeta *tab_ = nullptr;              // 表的元数据
    RmFileHandle *fh_;                          // 表的数据文件句柄
    std::vector<ColMeta> owned_cols_;           // 投影输出列；完整schema直接借用runtime cache
    std::vector<ColMeta> owned_full_cols_;
    std::vector<ColMeta> owned_output_source_cols_;
    const std::vector<ColMeta> *cols_ = nullptr;
    const std::vector<ColMeta> *full_cols_ = nullptr;
    const std::vector<ColMeta> *output_source_cols_ = nullptr;
    std::vector<ColMeta> index_key_cols_;
    std::vector<const char *> current_cells_;
    std::vector<int> index_output_offsets_;
    size_t len_;                                // 选取出来的一条记录的长度
    size_t full_len_ = 0;
    std::vector<Condition> fed_conds_;          // 扫描条件，和conds_字段相同
    std::vector<CompiledCondition> compiled_fed_conds_;  // 构造期编译一次,避免逐行线性 get_col
    bool compiled_conds_ok_ = false;

    std::vector<std::string> index_col_names_;  // index scan涉及到的索引包含的字段
    IndexMeta owned_index_meta_;
    const IndexMeta *index_meta_ = nullptr;     // 缓存计划的索引元数据可直接借用
    IxIndexHandle *ih_{nullptr};

    Rid rid_;
    const std::vector<Rid> *history_rids_ = nullptr;
    size_t history_cursor_ = 0;
    mutable RmRecord current_record_;
    VisibleTupleRef current_index_key_;
    TupleView current_view_;
    std::unique_ptr<IxScan> index_scan_;
    Rid point_rid_;
    bool point_rid_valid_ = false;
    bool point_consumed_ = false;
    bool at_end_ = true;
    bool current_is_history_ = false;
    bool index_only_ = false;
    bool projected_output_ = false;
    bool compiled_unique_point_ = false;
    std::shared_ptr<const rmdb::CompiledAccessProgram> access_program_;
    std::shared_ptr<const PlanRuntimeCache> runtime_cache_;
    rmdb::u64 access_program_schema_epoch_ = 0;
    bool access_program_matches_ = false;
    rmdb::IndexRangeSpec range_spec_;
    IndexRangeTraversal range_traversal_;

    VisibleIndexCursor visible_index_cursor_;

    SmManager *sm_manager_;
    rmdb::RuntimeScanFeedbackCollector feedback_;
    
    void reset_data_page() {
        visible_index_cursor_.finish();
    }
    
    void record_current_read() {
        if (context_ != nullptr && context_->txn_mgr_ != nullptr) {
            context_->txn_mgr_->RecordSerializableRead(context_->txn_, tab_name_, rid_);
        }
    }

    void compile_index_key_cols() {
        index_key_cols_.clear();
        int key_offset = 0;
        for (const auto &index_col : index_meta_->cols) {
            ColMeta col = index_col;
            col.tab_name = visible_name_;
            col.offset = key_offset;
            index_key_cols_.push_back(col);
            key_offset += index_col.len;
        }
    }

    bool key_conds_match(const char *key) const {
        if (range_spec_.all_conditions_consumed) {
            return true;
        }
        if (access_program_matches_) {
            return access_program_->EvaluateIndexKey(key, fed_conds_);
        }
        RmRecord key_record = RmRecord::borrow(key, index_meta_->logical_col_tot_len());
        return eval_conds(index_key_cols_, &key_record, fed_conds_);
    }

    bool residual_conds_match(const RmRecord *rec) const {
        if (compiled_conds_ok_) {
            return eval_compiled_conds_record(rec, compiled_fed_conds_);
        }
        return eval_conds(*full_cols_, rec, fed_conds_);
    }

    void set_current_from_heap() {
        if (!projected_output_) {
            current_view_.record = &current_record_;
            current_view_.cells = nullptr;
            return;
        }
        current_cells_.resize(output_source_cols_->size());
        for (size_t i = 0; i < output_source_cols_->size(); ++i) {
            current_cells_[i] = current_record_.data + (*output_source_cols_)[i].offset;
        }
        current_view_.record = nullptr;
        current_view_.cells = &current_cells_;
    }

    void set_current_from_index_key(const char *key) {
        current_index_key_ = VisibleTupleRef::IndexKeyBorrowed(key, index_meta_->logical_col_tot_len());
        if (!current_index_key_) {
            throw RMDBError("unable to lease index leaf for key view");
        }
        key = current_index_key_.data();
        current_cells_.resize(output_source_cols_->size());
        for (size_t i = 0; i < output_source_cols_->size(); ++i) {
            current_cells_[i] = key + index_output_offsets_[i];
        }
        current_view_.record = nullptr;
        current_view_.cells = &current_cells_;
    }

    bool advance_current_index() {
        while (index_scan_ != nullptr && !index_scan_->is_end()) {
            current_index_key_ = {};
            const char *key = index_scan_->key();
            // 扫描结束边界已由 IxScan 按逻辑 key 判定;此处前缀复核是防御检查。
            if (!range_traversal_.matches_prefix(key)) {
                break;
            }
            feedback_.add_index_entries();
            rid_ = index_scan_->rid();
            // 不再按 history 跳过 current 条目:先完整扫描 current index,
            // history 在扫描结束后加载,并按 record_current_rid 去重补充。
            // 旧快照下"提交删除但索引项尚未移除"的窗口行不会被漏掉。
            auto visible = visible_index_cursor_.read_current(rid_, key, &current_record_, !index_only_, true);
            feedback_.add_heap_fetches(visible.heap_fetched ? 1 : 0);
            if (!visible) {
                index_scan_->next();
                continue;
            }
            feedback_.add_rows_visible();
            if (visible.state == VisibleIndexCursor::ReadState::IndexOnly) {
                if (!key_conds_match(key)) {
                    index_scan_->next();
                    continue;
                }
                feedback_.add_index_only_rows();
                set_current_from_index_key(key);
            } else {
                // 范围已消费全部条件时,索引区间内的行天然满足条件,无需逐行求值。
                if (!range_spec_.all_conditions_consumed && !residual_conds_match(&current_record_)) {
                    index_scan_->next();
                    continue;
                }
                set_current_from_heap();
            }
            feedback_.add_rows_output();
            current_is_history_ = false;
            visible_index_cursor_.record_current_rid(rid_);
            record_current_read();
            return true;
        }
        index_scan_.reset();
        return false;
    }

    bool advance_unique_point() {
        if (!point_rid_valid_ || point_consumed_) {
            return false;
        }
        point_consumed_ = true;
        {
            current_index_key_ = {};
            rid_ = point_rid_;
            const char *key = range_spec_.lower_key.data();
            feedback_.add_index_entries();
            auto visible = visible_index_cursor_.read_current(rid_, key, &current_record_, !index_only_, true);
            feedback_.add_heap_fetches(visible.heap_fetched ? 1 : 0);
            if (!visible) {
                return false;
            }
            feedback_.add_rows_visible();
            if (visible.state == VisibleIndexCursor::ReadState::IndexOnly) {
                if (!key_conds_match(key)) {
                    return false;
                }
                feedback_.add_index_only_rows();
                set_current_from_index_key(key);
            } else {
                if (!range_spec_.all_conditions_consumed && !residual_conds_match(&current_record_)) {
                    return false;
                }
                set_current_from_heap();
            }
            feedback_.add_rows_output();
            current_is_history_ = false;
            visible_index_cursor_.record_current_rid(rid_);
            record_current_read();
            return true;
        }
    }

    bool advance_history() {
        while (history_rids_ != nullptr && history_cursor_ < history_rids_->size()) {
            rid_ = (*history_rids_)[history_cursor_];
            auto visible = visible_index_cursor_.read_visible_entry(rid_, &current_record_);
            feedback_.add_heap_fetches(visible.heap_fetched ? 1 : 0);
            if (!visible) {
                ++history_cursor_;
                continue;
            }
            feedback_.add_rows_visible();
            // history 行的 rid 不在索引区间内,必须保留条件求值。
            if (!residual_conds_match(&current_record_)) {
                ++history_cursor_;
                continue;
            }
            feedback_.add_rows_output();
            set_current_from_heap();
            current_is_history_ = true;
            record_current_read();
            return true;
        }
        return false;
    }

    void advance_index_cursor() {
        at_end_ = false;
        if ((compiled_unique_point_ ? advance_unique_point() : advance_current_index())) {
            return;
        }
        // current index 扫描结束:此时才加载 snapshot-index history(对已返回
        // current RID 去重),补上扫描期间并发提交删除/改键的旧行。延迟加载
        // 避免"history 已缓存但 current 项已被移除"的旧快照漏行。
        if (history_rids_ == nullptr) {
            history_rids_ = &visible_index_cursor_.history_rids();
            history_cursor_ = 0;
        }
        if (advance_history()) {
            return;
        }
        at_end_ = true;
        reset_data_page();
        feedback_.flush();
    }

   public:
    IndexScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds,
                    std::vector<std::string> index_col_names, Context *context, std::string visible_name = "",
                    const std::vector<TabCol> &required_cols = {},
                    std::shared_ptr<const PlanRuntimeCache> runtime_cache = nullptr,
                    std::shared_ptr<rmdb::RuntimeNodeFeedback> runtime_feedback = nullptr) {
        sm_manager_ = sm_manager;
        context_ = context;
        feedback_.bind(std::move(runtime_feedback), rmdb::RuntimeNodeKind::kIndexScan);
        tab_name_ = std::move(tab_name);
        visible_name_ = visible_name.empty() ? tab_name_ : std::move(visible_name);
        runtime_cache_ = std::move(runtime_cache);
        const bool use_cache = runtime_cache_ != nullptr && runtime_cache_->has_table;
        tab_ = &sm_manager_->db_.get_table(tab_name_);
        fed_conds_ = std::move(conds);
        // index_no_ = index_no;
        index_col_names_ = std::move(index_col_names);
        if (runtime_cache_ != nullptr && runtime_cache_->has_index) {
            index_meta_ = &runtime_cache_->index_meta;
        } else {
            owned_index_meta_ = *(tab_->get_index_meta(index_col_names_, true));
            index_meta_ = &owned_index_meta_;
        }
        access_program_schema_epoch_ = rmdb::sql_template_schema_epoch();
        if (runtime_cache_ != nullptr) {
            access_program_ = runtime_cache_->access_program;
        }
        access_program_matches_ = access_program_ != nullptr &&
                                  access_program_->Matches(*index_meta_, fed_conds_.size(),
                                                           access_program_schema_epoch_);
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        ih_ = rmdb::resolve_index_handle(sm_manager_, tab_name_, *index_meta_);
        visible_index_cursor_.bind(context_ == nullptr ? nullptr : context_->txn_mgr_,
                                   context_ == nullptr ? nullptr : context_->txn_, tab_name_,
                                   &index_col_names_, index_meta_, fh_);
        if (use_cache) {
            full_cols_ = &runtime_cache_->full_cols;
            full_len_ = runtime_cache_->full_len;
        } else {
            owned_full_cols_ = tab_->cols;
            for (auto &col : owned_full_cols_) {
                col.tab_name = visible_name_;
            }
            full_cols_ = &owned_full_cols_;
            full_len_ = full_cols_->back().offset + full_cols_->back().len;
        }
        for (auto &cond : fed_conds_) {
            if (cond.lhs_col.tab_name != visible_name_) {
                // lhs is on other table, now rhs must be on this table
                assert(!cond.is_rhs_val && cond.rhs_col.tab_name == visible_name_);
                // swap lhs and rhs
                std::swap(cond.lhs_col, cond.rhs_col);
                cond.op = swap_comparison_op(cond.op);
            }
        }
        compiled_conds_ok_ = compile_conds(*full_cols_, fed_conds_, &compiled_fed_conds_);
        if (required_cols.empty()) {
            cols_ = full_cols_;
            output_source_cols_ = full_cols_;
            len_ = full_len_;
        } else if (runtime_cache_ != nullptr && runtime_cache_->has_projection) {
            cols_ = &runtime_cache_->projected_cols;
            output_source_cols_ = &runtime_cache_->projected_source_cols;
            len_ = runtime_cache_->projected_len;
            projected_output_ = true;
        } else {
            build_column_projection(*full_cols_, required_cols, &owned_cols_, &owned_output_source_cols_, &len_);
            if (owned_cols_.empty()) {
                cols_ = full_cols_;
                output_source_cols_ = full_cols_;
                len_ = full_len_;
            } else {
                cols_ = &owned_cols_;
                output_source_cols_ = &owned_output_source_cols_;
                projected_output_ = true;
            }
        }
        compile_index_key_cols();
        index_only_ = rmdb::index_covers_conditions(*index_meta_, fed_conds_, visible_name_);
        for (const auto &col : *output_source_cols_) {
            index_only_ = index_only_ && rmdb::index_covers_col(*index_meta_, col.name);
        }
        if (index_only_) {
            bool bound = access_program_ != nullptr &&
                         access_program_->BindCoveringOutput(*index_meta_, *output_source_cols_,
                                                             &index_output_offsets_);
            if (!bound) {
                for (const auto &col : *output_source_cols_) {
                    auto key_col = rmdb::find_index_logical_col(*index_meta_, col.name);
                    if (!key_col) throw RMDBError("covering index column disappeared");
                    index_output_offsets_.push_back(key_col.offset);
                }
            }
        }
        bool compiled_range = access_program_matches_ &&
                              access_program_->MaterializeRange(*index_meta_, fed_conds_, &range_spec_);
        if (!compiled_range) {
            range_spec_ = rmdb::build_index_range_spec(*index_meta_, fed_conds_, visible_name_);
        }
        compiled_unique_point_ = range_spec_.exact_unique_key && access_program_matches_ &&
                                 access_program_->exact_unique_key();
        range_traversal_.bind(ih_, &range_spec_);
    }

    void beginTuple() override {
        feedback_.begin();
        index_scan_.reset();
        current_view_ = {};
        current_index_key_ = {};
        if (context_ != nullptr && context_->txn_mgr_ != nullptr) {
            context_->txn_mgr_->RecordSerializablePredicateRead(context_->txn_, tab_name_, *full_cols_, fed_conds_);
        }
        visible_index_cursor_.reset();
        // history 延迟到 current index 扫描结束后加载,避免一次性缓存导致漏行。
        history_rids_ = nullptr;
        history_cursor_ = 0;
        point_rid_ = {};
        point_rid_valid_ = false;
        point_consumed_ = false;
        current_is_history_ = false;
        if (compiled_unique_point_) {
            int *hint = nullptr;
            if (context_ != nullptr && context_->index_hints_ != nullptr) {
                hint = &(*context_->index_hints_)[ih_];
                if (*hint == 0) {
                    *hint = IX_NO_PAGE;   // 首次使用: 无 hint(0 是文件头页,非节点)
                }
            }
            point_rid_valid_ = ih_->get_unique_value(range_spec_.lower_key.data(), &point_rid_,
                                                     context_ == nullptr ? nullptr : context_->txn_, hint);
        } else {
            index_scan_ = range_traversal_.open_scan();
        }
        advance_index_cursor();
    }

    void nextTuple() override {
        if (at_end_) {
            return;
        }
        if (current_is_history_) {
            ++history_cursor_;
        } else if (compiled_unique_point_) {
            point_consumed_ = true;
        } else if (index_scan_ != nullptr && !index_scan_->is_end()) {
            current_index_key_ = {};
            index_scan_->next();
        }
        advance_index_cursor();
    }

    std::unique_ptr<RmRecord> Next() override {
        if (at_end_) {
            return nullptr;
        }
        auto rec = std::make_unique<RmRecord>(static_cast<int>(len_));
        materialize_tuple_view(*CurrentTupleView(), *cols_, rec.get(), len_);
        return rec;
    }

    const RmRecord *CurrentTuple() const override {
        if (at_end_) {
            return nullptr;
        }
        if (current_view_.record == &current_record_) {
            return &current_record_;
        }
        materialize_tuple_view(*CurrentTupleView(), *cols_, &current_record_, len_);
        return &current_record_;
    }

    const TupleView *CurrentTupleView() const override {
        if (at_end_) {
            return nullptr;
        }
        if (current_view_.cells != nullptr || current_view_.record != nullptr) {
            return &current_view_;
        }
        return nullptr;
    }

    bool is_end() const override { return at_end_; }
    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta> &cols() const override { return *cols_; }
    std::string getType() override { return "IndexScanExecutor"; }
    ColMeta get_col_offset(const TabCol &target) override { return *get_col(*cols_, target); }
    Rid &rid() override { return rid_; }
};
