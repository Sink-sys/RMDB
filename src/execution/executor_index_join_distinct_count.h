#pragma once

#include <cstring>
#include <memory>
#include <utility>

#include "executor_abstract.h"
#include "executor_index_nestedloop_join.h"

class IndexJoinDistinctCountExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<IndexNestedLoopJoinExecutor> join_;
    TabCol target_;
    std::vector<ColMeta> cols_;
    RmRecord output_{static_cast<int>(sizeof(int))};
    bool is_end_ = true;

   public:
    IndexJoinDistinctCountExecutor(std::unique_ptr<IndexNestedLoopJoinExecutor> join, TabCol target,
                                   const TabCol &output_col)
        : join_(std::move(join)), target_(std::move(target)) {
        cols_.push_back({"", output_col.col_name, TYPE_INT, static_cast<int>(sizeof(int)), 0, false});
    }

    void beginTuple() override {
        const int count = join_->CountDistinctRightInt(target_);
        rmdb::store_unaligned<int>(output_.data, count);
        is_end_ = false;
    }

    void nextTuple() override { is_end_ = true; }
    bool is_end() const override { return is_end_; }
    size_t tupleLen() const override { return sizeof(int); }
    const std::vector<ColMeta> &cols() const override { return cols_; }
    std::string getType() override { return "IndexJoinDistinctCountExecutor"; }
    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }
    Rid &rid() override { return _abstract_rid; }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end_) {
            return nullptr;
        }
        return std::make_unique<RmRecord>(output_);
    }

    const RmRecord *CurrentTuple() const override { return is_end_ ? nullptr : &output_; }
};
