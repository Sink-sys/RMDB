#pragma once

#include <algorithm>

#include "common/runtime_feedback.h"
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "execution_common.h"
#include "index/ix.h"
#include "system/sm.h"

class SortMergeJoinExecutor : public AbstractExecutor {
   private:
    struct StoredRecord {
        RmRecord record;
    };

    std::unique_ptr<AbstractExecutor> left_;
    std::unique_ptr<AbstractExecutor> right_;
    size_t left_len_ = 0;
    size_t right_len_ = 0;
    size_t len_ = 0;
    std::vector<ColMeta> cols_;
    std::vector<Condition> fed_conds_;
    std::vector<CompiledCondition> compiled_conds_;
    bool compiled_conds_valid_ = false;
    std::vector<ExecutionJoinKeyPart> key_parts_;
    // 排序合并分组已按字节证明键列相等,逐对只需求值非键(残余)条件。
    std::vector<Condition> residual_fed_conds_;
    std::vector<CompiledCondition> compiled_residual_conds_;
    bool compiled_residual_ok_ = false;
    bool has_residual_conds_ = false;

    std::vector<StoredRecord> left_records_;
    std::vector<StoredRecord> right_records_;
    std::vector<size_t> left_order_;
    std::vector<size_t> right_order_;
    size_t left_pos_ = 0;
    size_t right_pos_ = 0;
    size_t left_group_begin_ = 0;
    size_t left_group_end_ = 0;
    size_t right_group_begin_ = 0;
    size_t right_group_end_ = 0;
    size_t left_pair_pos_ = 0;
    size_t right_pair_pos_ = 0;
    size_t current_left_idx_ = 0;
    size_t current_right_idx_ = 0;

    bool isend_ = false;
    bool has_current_ = false;
    mutable RmRecord current_record_;
    TupleView current_view_;
    std::vector<const char *> current_cells_;
    std::vector<const char *> eval_cells_;
    rmdb::RuntimeJoinFeedbackCollector feedback_;

    void materialize_child(AbstractExecutor *child, size_t tuple_len, std::vector<StoredRecord> *records) {
        records->clear();
        const auto &child_cols = child->cols();
        child->beginTuple();
        for (; !child->is_end(); child->nextTuple()) {
            auto tuple = child->ReadTupleView();
            if (!tuple) {
                continue;
            }
            StoredRecord stored;
            stored.record.Resize(static_cast<int>(tuple_len));
            materialize_tuple_view(*tuple, child_cols, &stored.record, tuple_len);
            records->push_back(std::move(stored));
        }
    }

    int compare_left_records(size_t lhs_idx, size_t rhs_idx) const {
        const auto &lhs = left_records_[lhs_idx].record;
        const auto &rhs = left_records_[rhs_idx].record;
        for (const auto &part : key_parts_) {
            int cmp = compare_value(lhs.data + part.left_col.offset, rhs.data + part.left_col.offset,
                                    part.left_col.type, part.left_col.len);
            if (cmp != 0) {
                return cmp;
            }
        }
        return 0;
    }

    int compare_right_records(size_t lhs_idx, size_t rhs_idx) const {
        const auto &lhs = right_records_[lhs_idx].record;
        const auto &rhs = right_records_[rhs_idx].record;
        for (const auto &part : key_parts_) {
            int cmp = compare_value(lhs.data + part.right_col.offset, rhs.data + part.right_col.offset,
                                    part.right_col.type, part.right_col.len);
            if (cmp != 0) {
                return cmp;
            }
        }
        return 0;
    }

    int compare_left_right(size_t left_idx, size_t right_idx) const {
        const auto &left = left_records_[left_idx].record;
        const auto &right = right_records_[right_idx].record;
        for (const auto &part : key_parts_) {
            int cmp = compare_value(left.data + part.left_col.offset, right.data + part.right_col.offset,
                                    part.left_col.type, part.left_col.len);
            if (cmp != 0) {
                return cmp;
            }
        }
        return 0;
    }

    bool is_key_condition(const Condition &cond) const {
        // 与 BuildEquiJoinKeyParts 的消费判定保持一致:跨输入两侧的列列 OP_EQ。
        if (cond.is_rhs_val || cond.op != OP_EQ) {
            return false;
        }
        size_t left_idx = 0;
        size_t right_idx = 0;
        const ColMeta *left_col = FindExecutorColumn(left_->cols(), cond.lhs_col, &left_idx);
        const ColMeta *right_col = FindExecutorColumn(right_->cols(), cond.rhs_col, &right_idx);
        if (left_col == nullptr || right_col == nullptr) {
            left_col = FindExecutorColumn(left_->cols(), cond.rhs_col, &left_idx);
            right_col = FindExecutorColumn(right_->cols(), cond.lhs_col, &right_idx);
            if (left_col == nullptr || right_col == nullptr) {
                return false;
            }
        }
        return left_col->type == right_col->type && left_col->len == right_col->len;
    }

    bool pair_passes_conditions(const RmRecord &left_record, const RmRecord &right_record) {
        feedback_.add_candidate_pairs();
        if (!has_residual_conds_) {
            return true;
        }
        const auto &left_cols = left_->cols();
        const auto &right_cols = right_->cols();
        eval_cells_.resize(left_cols.size() + right_cols.size());
        size_t out_idx = 0;
        for (const auto &col : left_cols) {
            eval_cells_[out_idx++] = left_record.data + col.offset;
        }
        for (const auto &col : right_cols) {
            eval_cells_[out_idx++] = right_record.data + col.offset;
        }
        TupleView view;
        view.record = nullptr;
        view.cells = &eval_cells_;
        return compiled_residual_ok_ ? eval_compiled_conds_view(view, compiled_residual_conds_)
                                     : eval_conds_view(cols_, view, residual_fed_conds_);
    }

    template <typename Compare>
    size_t group_end(const std::vector<size_t> &order, size_t begin, Compare compare) const {
        size_t end = begin + 1;
        while (end < order.size() && compare(order[begin], order[end]) == 0) {
            ++end;
        }
        return end;
    }

    void build_sorted_inputs() {
        materialize_child(left_.get(), left_len_, &left_records_);
        materialize_child(right_.get(), right_len_, &right_records_);
        feedback_.add_left_rows(left_records_.size());
        feedback_.add_right_rows(right_records_.size());
        left_order_.clear();
        right_order_.clear();
        if (left_records_.empty() || right_records_.empty()) {
            return;
        }

        left_order_.resize(left_records_.size());
        right_order_.resize(right_records_.size());
        for (size_t i = 0; i < left_order_.size(); ++i) {
            left_order_[i] = i;
        }
        for (size_t i = 0; i < right_order_.size(); ++i) {
            right_order_[i] = i;
        }
        std::stable_sort(left_order_.begin(), left_order_.end(), [&](size_t lhs, size_t rhs) {
            return compare_left_records(lhs, rhs) < 0;
        });
        std::stable_sort(right_order_.begin(), right_order_.end(), [&](size_t lhs, size_t rhs) {
            return compare_right_records(lhs, rhs) < 0;
        });
    }

    bool find_next_equal_group() {
        while (left_pos_ < left_order_.size() && right_pos_ < right_order_.size()) {
            int cmp = compare_left_right(left_order_[left_pos_], right_order_[right_pos_]);
            if (cmp < 0) {
                left_pos_ = group_end(left_order_, left_pos_, [&](size_t lhs, size_t rhs) {
                    return compare_left_records(lhs, rhs);
                });
                continue;
            }
            if (cmp > 0) {
                right_pos_ = group_end(right_order_, right_pos_, [&](size_t lhs, size_t rhs) {
                    return compare_right_records(lhs, rhs);
                });
                continue;
            }

            left_group_begin_ = left_pos_;
            left_group_end_ = group_end(left_order_, left_pos_, [&](size_t lhs, size_t rhs) {
                return compare_left_records(lhs, rhs);
            });
            right_group_begin_ = right_pos_;
            right_group_end_ = group_end(right_order_, right_pos_, [&](size_t lhs, size_t rhs) {
                return compare_right_records(lhs, rhs);
            });
            left_pair_pos_ = left_group_begin_;
            right_pair_pos_ = right_group_begin_;
            left_pos_ = left_group_end_;
            right_pos_ = right_group_end_;
            return true;
        }
        return false;
    }

    bool advance_to_match() {
        while (true) {
            while (left_pair_pos_ < left_group_end_) {
                while (right_pair_pos_ < right_group_end_) {
                    size_t left_idx = left_order_[left_pair_pos_];
                    size_t right_idx = right_order_[right_pair_pos_++];
                    if (pair_passes_conditions(left_records_[left_idx].record, right_records_[right_idx].record)) {
                        feedback_.add_rows_output();
                        current_left_idx_ = left_idx;
                        current_right_idx_ = right_idx;
                        set_current_from_pair();
                        return true;
                    }
                }
                ++left_pair_pos_;
                right_pair_pos_ = right_group_begin_;
            }
            if (!find_next_equal_group()) {
                return false;
            }
        }
    }

    void set_current_from_pair() {
        const auto &left_record = left_records_[current_left_idx_].record;
        const auto &right_record = right_records_[current_right_idx_].record;
        const auto &left_cols = left_->cols();
        const auto &right_cols = right_->cols();
        current_cells_.resize(left_cols.size() + right_cols.size());
        size_t out_idx = 0;
        for (const auto &col : left_cols) {
            current_cells_[out_idx++] = left_record.data + col.offset;
        }
        for (const auto &col : right_cols) {
            current_cells_[out_idx++] = right_record.data + col.offset;
        }
        current_view_.record = nullptr;
        current_view_.cells = &current_cells_;
        has_current_ = true;
    }

   public:
    SortMergeJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right,
                          std::vector<Condition> conds,
                          std::shared_ptr<rmdb::RuntimeNodeFeedback> runtime_feedback = nullptr) {
        left_ = std::move(left);
        right_ = std::move(right);
        left_len_ = left_->tupleLen();
        right_len_ = right_->tupleLen();
        len_ = left_len_ + right_len_;
        cols_ = left_->cols();
        auto right_cols = right_->cols();
        for (auto &col : right_cols) {
            col.offset += static_cast<int>(left_len_);
        }
        cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());
        fed_conds_ = std::move(conds);
        compiled_conds_valid_ = compile_conds(cols_, fed_conds_, &compiled_conds_);
        key_parts_ = BuildEquiJoinKeyParts(left_->cols(), right_->cols(), fed_conds_);
        for (const auto &cond : fed_conds_) {
            if (!is_key_condition(cond)) {
                residual_fed_conds_.push_back(cond);
            }
        }
        has_residual_conds_ = !residual_fed_conds_.empty();
        if (has_residual_conds_) {
            compiled_residual_ok_ = compile_conds(cols_, residual_fed_conds_, &compiled_residual_conds_);
        }
        current_record_.Resize(static_cast<int>(len_));
        feedback_.bind(std::move(runtime_feedback));
    }

    void beginTuple() override {
        feedback_.begin();
        isend_ = false;
        has_current_ = false;
        left_pos_ = 0;
        right_pos_ = 0;
        left_group_begin_ = 0;
        left_group_end_ = 0;
        right_group_begin_ = 0;
        right_group_end_ = 0;
        left_pair_pos_ = 0;
        right_pair_pos_ = 0;
        build_sorted_inputs();
        if (!find_next_equal_group() || !advance_to_match()) {
            isend_ = true;
            feedback_.flush();
            return;
        }
    }

    void nextTuple() override {
        if (isend_) {
            return;
        }
        has_current_ = false;
        if (!advance_to_match()) {
            isend_ = true;
            feedback_.flush();
            return;
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        if (!has_current_) {
            return nullptr;
        }
        auto rec = std::make_unique<RmRecord>(static_cast<int>(len_));
        materialize_tuple_view(current_view_, cols_, rec.get(), len_);
        return rec;
    }

    const TupleView *CurrentTupleView() const override { return has_current_ ? &current_view_ : nullptr; }

    const RmRecord *CurrentTuple() const override {
        if (!has_current_) {
            return nullptr;
        }
        materialize_tuple_view(current_view_, cols_, &current_record_, len_);
        return &current_record_;
    }

    bool is_end() const override { return isend_; }
    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta> &cols() const override { return cols_; }
    std::string getType() override { return "SortMergeJoinExecutor"; }
    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }
    Rid &rid() override { return _abstract_rid; }
};
