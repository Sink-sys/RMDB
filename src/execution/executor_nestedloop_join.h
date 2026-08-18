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
#include "common/runtime_feedback.h"
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class NestedLoopJoinExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> left_;    // 左儿子节点（需要join的表）
    std::unique_ptr<AbstractExecutor> right_;   // 右儿子节点（需要join的表）
    size_t len_;                                // join后获得的每条记录的长度
    std::vector<ColMeta> cols_;                 // join后获得的记录的字段

    std::vector<Condition> fed_conds_;          // join条件
    std::vector<CompiledCondition> compiled_conds_;
    bool compiled_conds_valid_ = false;
    bool isend;
    bool has_current_ = false;
    mutable RmRecord current_record_;
    TupleView current_view_;
    std::vector<const char *> current_cells_;
    rmdb::RuntimeJoinFeedbackCollector feedback_;

    bool set_current_from_children() {
        auto left_view = left_->CurrentTupleView();
        auto right_view = right_->CurrentTupleView();
        if (left_view == nullptr || right_view == nullptr) {
            return false;
        }
        feedback_.add_right_rows();
        feedback_.add_candidate_pairs();
        const auto &left_cols = left_->cols();
        const auto &right_cols = right_->cols();
        current_cells_.resize(left_cols.size() + right_cols.size());
        size_t out_idx = 0;
        for (size_t i = 0; i < left_cols.size(); ++i) {
            current_cells_[out_idx++] = left_view->cell_at(left_cols[i], i);
        }
        for (size_t i = 0; i < right_cols.size(); ++i) {
            current_cells_[out_idx++] = right_view->cell_at(right_cols[i], i);
        }
        current_view_.record = nullptr;
        current_view_.cells = &current_cells_;
        bool passes = compiled_conds_valid_ ? eval_compiled_conds_view(current_view_, compiled_conds_)
                                            : eval_conds_view(cols_, current_view_, fed_conds_);
        if (passes) {
            feedback_.add_rows_output();
        }
        return passes;
    }

   public:
    NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right, 
                           std::vector<Condition> conds,
                           std::shared_ptr<rmdb::RuntimeNodeFeedback> runtime_feedback = nullptr) {
        left_ = std::move(left);
        right_ = std::move(right);
        len_ = left_->tupleLen() + right_->tupleLen();
        cols_ = left_->cols();
        auto right_cols = right_->cols();
        for (auto &col : right_cols) {
            col.offset += left_->tupleLen();
        }

        cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());
        isend = false;
        current_record_.Resize(static_cast<int>(len_));
        fed_conds_ = std::move(conds);
        compiled_conds_valid_ = compile_conds(cols_, fed_conds_, &compiled_conds_);
        feedback_.bind(std::move(runtime_feedback));

    }

    void beginTuple() override {
        feedback_.begin();
        isend = false;
        has_current_ = false;
        left_->beginTuple();
        right_->beginTuple();
        if (!left_->is_end()) {
            feedback_.add_left_rows();
        }
        while (!left_->is_end()) {
            while (!right_->is_end()) {
                if (set_current_from_children()) {
                    has_current_ = true;
                    return;
                }
                right_->nextTuple();
            }
            left_->nextTuple();
            if (!left_->is_end()) {
                feedback_.add_left_rows();
            }
            right_->beginTuple();
        }
        isend = true;
        feedback_.flush();
    }

    void nextTuple() override {
        if (isend) {
            return;
        }
        has_current_ = false;
        right_->nextTuple();
        while (!left_->is_end()) {
            while (!right_->is_end()) {
                if (set_current_from_children()) {
                    has_current_ = true;
                    return;
                }
                right_->nextTuple();
            }
            left_->nextTuple();
            if (!left_->is_end()) {
                feedback_.add_left_rows();
            }
            right_->beginTuple();
        }
        isend = true;
        feedback_.flush();
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

    bool is_end() const override { return isend; }
    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta> &cols() const override { return cols_; }
    std::string getType() override { return "NestedLoopJoinExecutor"; }
    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }
    Rid &rid() override { return _abstract_rid; }
};
