#pragma once

#include "common/runtime_feedback.h"
#include "executor_abstract.h"

class SemiJoinExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> left_;
    std::unique_ptr<AbstractExecutor> right_;
    std::vector<Condition> conds_;
    std::vector<ColMeta> cols_;
    std::vector<ColMeta> combined_cols_;
    std::vector<CompiledCondition> compiled_conds_;
    bool compiled_conds_valid_ = false;
    size_t len_;
    std::vector<std::unique_ptr<RmRecord>> tuples_;
    size_t cursor_;
    TupleView combined_view_;
    std::vector<const char *> combined_cells_;
    rmdb::RuntimeJoinFeedbackCollector feedback_;

   public:
    SemiJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right,
                     std::vector<Condition> conds,
                     std::shared_ptr<rmdb::RuntimeNodeFeedback> runtime_feedback = nullptr) {
        left_ = std::move(left);
        right_ = std::move(right);
        conds_ = std::move(conds);
        cols_ = left_->cols();
        combined_cols_ = cols_;
        auto right_cols = right_->cols();
        for (auto &col : right_cols) {
            col.offset += left_->tupleLen();
            combined_cols_.push_back(col);
        }
        compiled_conds_valid_ = compile_conds(combined_cols_, conds_, &compiled_conds_);
        len_ = left_->tupleLen();
        cursor_ = 0;
        feedback_.bind(std::move(runtime_feedback));
    }

    void beginTuple() override {
        feedback_.begin();
        tuples_.clear();
        for (left_->beginTuple(); !left_->is_end(); left_->nextTuple()) {
            auto left_rec = left_->ReadTupleView();
            if (!left_rec) {
                break;
            }
            feedback_.add_left_rows();
            bool matched = false;
            for (right_->beginTuple(); !right_->is_end(); right_->nextTuple()) {
                auto right_rec = right_->ReadTupleView();
                if (!right_rec) {
                    break;
                }
                feedback_.add_right_rows();
                feedback_.add_candidate_pairs();
                const auto &left_cols = left_->cols();
                const auto &right_cols = right_->cols();
                combined_cells_.resize(left_cols.size() + right_cols.size());
                size_t out_idx = 0;
                for (size_t i = 0; i < left_cols.size(); ++i) {
                    combined_cells_[out_idx++] = left_rec->cell_at(left_cols[i], i);
                }
                for (size_t i = 0; i < right_cols.size(); ++i) {
                    combined_cells_[out_idx++] = right_rec->cell_at(right_cols[i], i);
                }
                combined_view_.record = nullptr;
                combined_view_.cells = &combined_cells_;
                bool pass = compiled_conds_valid_ ? eval_compiled_conds_view(combined_view_, compiled_conds_)
                                                  : eval_conds_view(combined_cols_, combined_view_, conds_);
                if (pass) {
                    matched = true;
                    break;
                }
            }
            if (matched) {
                feedback_.add_rows_output();
                auto rec = std::make_unique<RmRecord>(static_cast<int>(len_));
                materialize_tuple_view(*left_rec, cols_, rec.get(), len_);
                tuples_.push_back(std::move(rec));
            }
        }
        cursor_ = 0;
        feedback_.flush();
    }

    void nextTuple() override {
        if (cursor_ < tuples_.size()) {
            cursor_++;
        }
    }

    bool is_end() const override { return cursor_ >= tuples_.size(); }
    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta> &cols() const override { return cols_; }
    std::string getType() override { return "SemiJoinExecutor"; }
    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }
    Rid &rid() override { return _abstract_rid; }
    std::unique_ptr<RmRecord> Next() override {
        if (cursor_ >= tuples_.size()) {
            return nullptr;
        }
        auto rec = std::make_unique<RmRecord>(static_cast<int>(len_));
        memcpy(rec->data, tuples_[cursor_]->data, len_);
        return rec;
    }
    const RmRecord *CurrentTuple() const override {
        return cursor_ >= tuples_.size() ? nullptr : tuples_[cursor_].get();
    }
};
