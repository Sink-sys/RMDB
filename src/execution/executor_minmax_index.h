#pragma once

#include <algorithm>
#include <cstring>
#include <limits>

#include "common/index_runtime.h"
#include "execution_common.h"
#include "index_range_traversal.h"
#include "index_visibility_cursor.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "record/rm_scan.h"
#include "system/sm.h"
#include "transaction/transaction_manager.h"

class MinMaxIndexAggregateExecutor : public AbstractExecutor {
   private:
    struct PrefixCond {
        int key_offset = 0;
        int len = 0;
        const char *rhs_value = nullptr;
    };

    std::string tab_name_;
    std::string visible_name_;
    TabMeta tab_;
    std::vector<Condition> conds_;
    std::vector<CompiledCondition> compiled_conds_;
    bool compiled_conds_ok_ = false;
    std::vector<std::string> index_col_names_;
    IndexMeta index_meta_;
    IxIndexHandle *ih_ = nullptr;
    RmFileHandle *fh_ = nullptr;
    std::vector<ColMeta> full_cols_;
    ColMeta agg_input_col_;
    ColMeta output_col_;
    std::vector<ColMeta> cols_;
    size_t len_ = 0;
    ast::AggType agg_type_ = ast::AGG_MIN;

    std::vector<PrefixCond> compiled_prefix_conds_;
    int equality_prefix_len_ = 0;
    std::string local_index_key_scratch_;
    std::string *index_key_scratch_ = &local_index_key_scratch_;
    rmdb::IndexRangeSpec range_spec_;
    IndexRangeTraversal range_traversal_;
    VisibleIndexCursor visible_index_cursor_;
    RmRecord current_record_;
    RmRecord output_tuple_;
    size_t cursor_ = 0;
    bool has_tuple_ = false;
    rmdb::RuntimeScanFeedbackCollector feedback_;
    std::shared_ptr<const rmdb::CompiledAccessProgram> access_program_;

    bool record_visible_result(const VisibleIndexCursor::ReadResult &visible) {
        feedback_.add_heap_fetches(visible.heap_fetched ? 1 : 0);
        if (visible) {
            feedback_.add_rows_visible();
            return true;
        }
        return false;
    }

    bool residual_conds_match(const RmRecord *rec) {
        if (compiled_conds_ok_) {
            return eval_compiled_conds_record(rec, compiled_conds_);
        }
        return eval_conds(full_cols_, rec, conds_);
    }

    void compile_index_prefix() {
        compiled_prefix_conds_.clear();
        equality_prefix_len_ = 0;
        int key_offset = 0;
        for (const auto &index_col : index_meta_.cols) {
            auto cond_it = std::find_if(conds_.begin(), conds_.end(), [&](const Condition &cond) {
                return cond.is_rhs_val && cond.op == OP_EQ && cond.rhs_val.raw != nullptr &&
                       cond.lhs_col.tab_name == visible_name_ && cond.lhs_col.col_name == index_col.name;
            });
            if (cond_it == conds_.end()) {
                break;
            }
            compiled_prefix_conds_.push_back({key_offset, index_col.len, cond_it->rhs_val.raw->data});
            key_offset += index_col.len;
            equality_prefix_len_ = key_offset;
        }
    }

    void write_empty_value() {
        output_tuple_ = RmRecord(static_cast<int>(len_));
        memset(output_tuple_.data, 0, len_);
    }

    void write_value(const char *slot) {
        output_tuple_ = RmRecord(static_cast<int>(len_));
        memcpy(output_tuple_.data + output_col_.offset, slot + agg_input_col_.offset, agg_input_col_.len);
    }

    bool value_greater_than_output(const char *slot) const {
        const char *lhs = slot + agg_input_col_.offset;
        const char *rhs = output_tuple_.data + output_col_.offset;
        if (agg_input_col_.type == TYPE_INT) {
            return rmdb::load_unaligned<int>(lhs) > rmdb::load_unaligned<int>(rhs);
        }
        if (agg_input_col_.type == TYPE_FLOAT) {
            return rmdb::load_unaligned<float>(lhs) > rmdb::load_unaligned<float>(rhs);
        }
        return std::memcmp(lhs, rhs, static_cast<size_t>(agg_input_col_.len)) > 0;
    }

    bool value_less_than_output(const char *slot) const {
        const char *lhs = slot + agg_input_col_.offset;
        const char *rhs = output_tuple_.data + output_col_.offset;
        if (agg_input_col_.type == TYPE_INT) {
            return rmdb::load_unaligned<int>(lhs) < rmdb::load_unaligned<int>(rhs);
        }
        if (agg_input_col_.type == TYPE_FLOAT) {
            return rmdb::load_unaligned<float>(lhs) < rmdb::load_unaligned<float>(rhs);
        }
        return std::memcmp(lhs, rhs, static_cast<size_t>(agg_input_col_.len)) < 0;
    }

    bool find_min_value() {
        std::fill(index_key_scratch_->begin(), index_key_scratch_->end(), '\0');
        for (const auto &prefix : compiled_prefix_conds_) {
            memcpy(index_key_scratch_->data() + prefix.key_offset, prefix.rhs_value, prefix.len);
        }
        int key_offset = equality_prefix_len_;
        for (int i = static_cast<int>(compiled_prefix_conds_.size()); i < index_meta_.col_num; ++i) {
            const auto &col = index_meta_.cols[i];
            if (col.type == TYPE_INT) {
                int min_value = std::numeric_limits<int>::min();
                memcpy(index_key_scratch_->data() + key_offset, &min_value, sizeof(int));
            } else if (col.type == TYPE_FLOAT) {
                float min_value = -std::numeric_limits<float>::max();
                memcpy(index_key_scratch_->data() + key_offset, &min_value, sizeof(float));
            }
            key_offset += col.len;
        }

        bool found = false;
        IxReadGuard read_guard = ih_->make_read_guard();
        auto scan = range_traversal_.open_scan();
        while (scan != nullptr && !scan->is_end()) {
            if (!range_traversal_.matches_prefix(scan->key())) {
                break;
            }
            feedback_.add_index_entries();
            Rid rid = scan->rid();
            auto visible = visible_index_cursor_.read_current(rid, scan->key(), &current_record_, true, true);
            if (record_visible_result(visible) &&
                (range_spec_.all_conditions_consumed || residual_conds_match(&current_record_))) {
                write_value(current_record_.data);
                found = true;
                break;
            }
            scan->next();
        }

        for (const auto &historical_rid : visible_index_cursor_.history_rids()) {
            auto visible = visible_index_cursor_.read_visible_entry(historical_rid, &current_record_);
            if (!record_visible_result(visible) || !residual_conds_match(&current_record_)) {
                continue;
            }
            if (!found || value_less_than_output(current_record_.data)) {
                write_value(current_record_.data);
                found = true;
            }
        }
        visible_index_cursor_.finish();
        return found;
    }

    bool find_max_value() {
        std::fill(index_key_scratch_->begin(), index_key_scratch_->end(), '\0');
        for (const auto &prefix : compiled_prefix_conds_) {
            memcpy(index_key_scratch_->data() + prefix.key_offset, prefix.rhs_value, prefix.len);
        }
        rmdb::fill_index_key_max_suffix(index_meta_, equality_prefix_len_, index_key_scratch_->data());

        bool found = false;
        bool inclusive = true;
        std::string candidate_key;
        Rid rid;
        while (ih_->predecessor(index_key_scratch_->data(), &candidate_key, &rid, inclusive)) {
            feedback_.add_index_entries();
            inclusive = false;
            if (!range_traversal_.matches_prefix(candidate_key.data())) {
                break;
            }
            *index_key_scratch_ = candidate_key;
            auto visible = visible_index_cursor_.read_current(
                rid, candidate_key.data(), &current_record_, true, true);
            if (record_visible_result(visible) &&
                (range_spec_.all_conditions_consumed || residual_conds_match(&current_record_))) {
                write_value(current_record_.data);
                found = true;
                break;
            }
        }

        for (const auto &historical_rid : visible_index_cursor_.history_rids()) {
            auto visible = visible_index_cursor_.read_visible_entry(historical_rid, &current_record_);
            if (!record_visible_result(visible) || !residual_conds_match(&current_record_)) {
                continue;
            }
            if (!found || value_greater_than_output(current_record_.data)) {
                write_value(current_record_.data);
                found = true;
            }
        }
        visible_index_cursor_.finish();
        return found;
    }

   public:
    MinMaxIndexAggregateExecutor(SmManager *sm_manager,
                                 std::string tab_name,
                                 std::string visible_name,
                                 std::vector<Condition> conds,
                                 std::vector<std::string> index_col_names,
                                 TabCol agg_col,
                                 ast::AggType agg_type,
                                 std::vector<TabCol> output_cols,
                                 Context *context,
                                 std::shared_ptr<const PlanRuntimeCache> runtime_cache = nullptr,
                                 std::shared_ptr<rmdb::RuntimeNodeFeedback> runtime_feedback = nullptr) {
        context_ = context;
        feedback_.bind(std::move(runtime_feedback), rmdb::RuntimeNodeKind::kMinMaxIndex);
        tab_name_ = std::move(tab_name);
        visible_name_ = visible_name.empty() ? tab_name_ : std::move(visible_name);
        conds_ = std::move(conds);
        index_col_names_ = std::move(index_col_names);
        agg_type_ = agg_type;

        const bool use_cache = runtime_cache != nullptr && runtime_cache->has_table;
        tab_ = sm_manager->db_.get_table(tab_name_);
        index_meta_ = runtime_cache != nullptr && runtime_cache->has_index
                          ? runtime_cache->index_meta
                          : *(tab_.get_index_meta(index_col_names_, true));
        ih_ = rmdb::resolve_index_handle(sm_manager, tab_name_, index_meta_);
        fh_ = sm_manager->fhs_.at(tab_name_).get();
        if (use_cache) {
            full_cols_ = runtime_cache->full_cols;
        } else {
            full_cols_ = tab_.cols;
            for (auto &col : full_cols_) {
                col.tab_name = visible_name_;
            }
        }
        agg_input_col_ = *get_col(full_cols_, agg_col);

        output_col_.tab_name = "";
        output_col_.name = output_cols.empty() ? agg_col.col_name : output_cols[0].col_name;
        output_col_.offset = 0;
        output_col_.type = agg_input_col_.type;
        output_col_.len = agg_input_col_.len;
        cols_.push_back(output_col_);
        len_ = static_cast<size_t>(output_col_.len);

        if (context_ != nullptr) {
            if (auto *scratch = context_->acquire_statement_string(index_meta_.col_tot_len); scratch != nullptr) {
                index_key_scratch_ = scratch;
            }
        }
        index_key_scratch_->resize(index_meta_.col_tot_len);
        visible_index_cursor_.bind(context_ == nullptr ? nullptr : context_->txn_mgr_,
                                   context_ == nullptr ? nullptr : context_->txn_, tab_name_,
                                   &index_col_names_, &index_meta_, fh_);
        compiled_conds_ok_ = compile_conds(full_cols_, conds_, &compiled_conds_);
        compile_index_prefix();
        if (runtime_cache != nullptr) access_program_ = runtime_cache->access_program;
        bool compiled_range = access_program_ != nullptr &&
                              access_program_->Matches(index_meta_, conds_.size(), rmdb::sql_template_schema_epoch()) &&
                              access_program_->MaterializeRange(index_meta_, conds_, &range_spec_);
        if (!compiled_range) {
            range_spec_ = rmdb::build_index_range_spec(index_meta_, conds_, visible_name_);
        }
        range_traversal_.bind(ih_, &range_spec_);
    }

    void beginTuple() override {
        feedback_.begin();
        cursor_ = 0;
        has_tuple_ = true;
        visible_index_cursor_.reset();
        if (agg_type_ != ast::AGG_MIN && agg_type_ != ast::AGG_MAX) {
            has_tuple_ = false;
            feedback_.flush();
            return;
        }
        if (!(agg_type_ == ast::AGG_MIN ? find_min_value() : find_max_value())) {
            write_empty_value();
        }
        feedback_.add_rows_output();
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
    std::string getType() override { return "MinMaxIndexAggregateExecutor"; }
    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }
    Rid &rid() override { return _abstract_rid; }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end()) {
            return nullptr;
        }
        auto rec = std::make_unique<RmRecord>(static_cast<int>(len_));
        memcpy(rec->data, output_tuple_.data, len_);
        return rec;
    }

    const RmRecord *CurrentTuple() const override {
        return is_end() ? nullptr : &output_tuple_;
    }
};
