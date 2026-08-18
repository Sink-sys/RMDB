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
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class ProjectionExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;        // 投影节点的儿子节点
    std::vector<ColMeta> cols_;                     // 需要投影的字段
    size_t len_;                                    // 字段总长度
    std::vector<size_t> sel_idxs_;
    mutable RmRecord current_record_;
    mutable TupleView current_view_;
    mutable std::vector<const char *> current_cells_;

   public:
    ProjectionExecutor(std::unique_ptr<AbstractExecutor> prev, const std::vector<TabCol> &sel_cols) {
        prev_ = std::move(prev);

        size_t curr_offset = 0;
        auto &prev_cols = prev_->cols();
        for (auto &sel_col : sel_cols) {
            auto pos = get_col(prev_cols, sel_col);
            sel_idxs_.push_back(pos - prev_cols.begin());
            auto col = *pos;
            col.offset = curr_offset;
            curr_offset += col.len;
            cols_.push_back(col);
        }
        len_ = curr_offset;
    }

    void beginTuple() override { prev_->beginTuple(); }

    void nextTuple() override { prev_->nextTuple(); }

    std::unique_ptr<RmRecord> Next() override {
        auto tuple = prev_->ReadTupleView();
        if (!tuple) {
            return nullptr;
        }
        auto rec = std::make_unique<RmRecord>(static_cast<int>(len_));
        auto &prev_cols = prev_->cols();
        for (size_t i = 0; i < sel_idxs_.size(); ++i) {
            const auto &prev_col = prev_cols[sel_idxs_[i]];
            memcpy(rec->data + cols_[i].offset, tuple->cell_at(prev_col, sel_idxs_[i]), prev_col.len);
        }
        return rec;
    }

    const TupleView *CurrentTupleView() const override {
        auto prev_view = prev_->CurrentTupleView();
        if (prev_view == nullptr) {
            return nullptr;
        }
        auto &prev_cols = prev_->cols();
        current_cells_.resize(sel_idxs_.size());
        for (size_t i = 0; i < sel_idxs_.size(); ++i) {
            const auto &prev_col = prev_cols[sel_idxs_[i]];
            current_cells_[i] = prev_view->cell_at(prev_col, sel_idxs_[i]);
        }
        current_view_.record = nullptr;
        current_view_.cells = &current_cells_;
        return &current_view_;
    }

    const RmRecord *CurrentTuple() const override {
        auto view = CurrentTupleView();
        if (view == nullptr) {
            return nullptr;
        }
        materialize_tuple_view(*view, cols_, &current_record_, len_);
        return &current_record_;
    }

    bool is_end() const override { return prev_->is_end(); }
    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta> &cols() const override { return cols_; }
    std::string getType() override { return "ProjectionExecutor"; }
    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }
    Rid &rid() override { return _abstract_rid; }
};
