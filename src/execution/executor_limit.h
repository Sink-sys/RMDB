#pragma once

#include "executor_abstract.h"

class LimitExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    int limit_;
    int produced_;

   public:
    LimitExecutor(std::unique_ptr<AbstractExecutor> prev, int limit) {
        prev_ = std::move(prev);
        limit_ = limit;
        produced_ = 0;
    }

    void beginTuple() override {
        produced_ = 0;
        prev_->beginTuple();
    }

    void nextTuple() override {
        produced_++;
        prev_->nextTuple();
    }

    bool is_end() const override { return produced_ >= limit_ || prev_->is_end(); }
    size_t tupleLen() const override { return prev_->tupleLen(); }
    const std::vector<ColMeta> &cols() const override { return prev_->cols(); }
    std::string getType() override { return "LimitExecutor"; }
    ColMeta get_col_offset(const TabCol &target) override { return prev_->get_col_offset(target); }
    Rid &rid() override { return _abstract_rid; }
    std::unique_ptr<RmRecord> Next() override { return prev_->Next(); }
    const TupleView *CurrentTupleView() const override { return prev_->CurrentTupleView(); }
    const RmRecord *CurrentTuple() const override { return prev_->CurrentTuple(); }
};
