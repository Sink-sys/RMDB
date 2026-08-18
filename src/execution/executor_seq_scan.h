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
#include "execution_common.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "record/rm_scan.h"
#include "system/sm.h"
#include "transaction/transaction_manager.h"

class SeqScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;              // 表的名称
    RmFileHandle *fh_;                  // 表的数据文件句柄
    std::vector<ColMeta> owned_cols_;
    const std::vector<ColMeta> *cols_ = nullptr;
    size_t len_;                        // scan后生成的每条记录的长度
    size_t full_len_ = 0;
    int record_size_ = 0;
    std::vector<Condition> fed_conds_;  // 同conds_，两个字段相同
    std::vector<CompiledCondition> compiled_output_conds_;
    bool compiled_output_conds_valid_ = false;

    Rid rid_;
    std::unique_ptr<RmScan> scan_;      // table_iterator
    mutable RmRecord current_record_;
    VisibleTupleRef current_visible_;
    TupleView current_view_;
    bool at_end_ = true;
    BufferAccessClass scan_access_class_ = BufferAccessClass::Default;

    SmManager *sm_manager_;
    rmdb::RuntimeScanFeedbackCollector feedback_;
    std::shared_ptr<TransactionManager::TableVersionInfo> table_version_info_;
    std::shared_ptr<const PlanRuntimeCache> runtime_cache_;

    bool try_borrow_current() {
        if (context_ == nullptr || context_->txn_mgr_ == nullptr || scan_ == nullptr) {
            return false;
        }
        PageReadLatchGuard page_latch;
        const char *slot = nullptr;
        if (!scan_->acquire_current_slot_lease(&page_latch, &slot)) {
            return false;
        }
        auto *page_info =
            context_->txn_mgr_->GetPageVersionInfoOnTableRaw(table_version_info_, rid_.page_no);
        if (!context_->txn_mgr_->IsSnapshotPageCleanVisible(page_info, context_->txn_)) {
            return false;
        }
        current_visible_ = VisibleTupleRef::Borrowed(std::move(page_latch), slot, record_size_);
        current_view_.record = nullptr;
        current_view_.cells = nullptr;
        current_view_.raw_data = current_visible_.data();
        current_view_.raw_size = current_visible_.size();
        return static_cast<bool>(current_visible_);
    }

    bool eval_current_record_conds() const {
        if (compiled_output_conds_valid_) {
            return eval_compiled_conds_record(&current_record_, compiled_output_conds_);
        }
        return eval_conds(*cols_, &current_record_, fed_conds_);
    }

    void record_current_read() {
        if (context_ != nullptr && context_->txn_mgr_ != nullptr) {
            context_->txn_mgr_->RecordSerializableRead(context_->txn_, tab_name_, rid_);
        }
    }

    void advance_to_next_visible() {
        at_end_ = true;
        while (scan_ != nullptr && !scan_->is_end()) {
            current_visible_ = {};
            current_view_ = {};
            feedback_.add_rows_scanned();
            rid_ = scan_->rid();
            if (try_borrow_current()) {
                feedback_.add_rows_visible();
                bool matches = compiled_output_conds_valid_
                                   ? eval_compiled_conds_view(current_view_, compiled_output_conds_)
                                   : eval_conds_view(*cols_, current_view_, fed_conds_);
                if (!matches) {
                    current_visible_ = {};
                    scan_->next();
                    continue;
                }
                feedback_.add_rows_output();
                at_end_ = false;
                record_current_read();
                return;
            }
            if (context_->txn_mgr_->GetVisibleTupleInto(tab_name_, table_version_info_, rid_,
                                                        context_->txn_, &current_record_)) {
                feedback_.add_rows_visible();
                feedback_.add_heap_fetches();
                if (!eval_current_record_conds()) {
                    scan_->next();
                    continue;
                }
                feedback_.add_rows_output();
                current_view_.record = &current_record_;
                current_view_.cells = nullptr;
                at_end_ = false;
                record_current_read();
                return;
            }
            scan_->next();
        }
        feedback_.flush();
    }

   public:
    SeqScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds, Context *context,
                    std::string visible_name = "", std::vector<TabCol> required_cols = {},
                    std::shared_ptr<const PlanRuntimeCache> runtime_cache = nullptr,
                    std::shared_ptr<rmdb::RuntimeNodeFeedback> runtime_feedback = nullptr) {
        sm_manager_ = sm_manager;
        feedback_.bind(std::move(runtime_feedback), rmdb::RuntimeNodeKind::kSeqScan);
        tab_name_ = std::move(tab_name);
        std::string visible = visible_name.empty() ? tab_name_ : std::move(visible_name);
        fed_conds_ = std::move(conds);
        runtime_cache_ = std::move(runtime_cache);
        const bool use_cache = runtime_cache_ != nullptr && runtime_cache_->has_table;
        const TabMeta &tab = sm_manager_->db_.get_table(tab_name_);
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        record_size_ = fh_->get_file_hdr().record_size;
        context_ = context;
        if (context_ != nullptr && context_->txn_mgr_ != nullptr) {
            table_version_info_ = context_->txn_mgr_->GetOrCreateTableVersionInfo(tab_name_);
        }
        if (use_cache) {
            cols_ = &runtime_cache_->full_cols;
            full_len_ = runtime_cache_->full_len;
        } else {
            owned_cols_ = tab.cols;
            for (auto &col : owned_cols_) {
                col.tab_name = visible;
            }
            cols_ = &owned_cols_;
            full_len_ = cols_->back().offset + cols_->back().len;
        }
        (void)required_cols;
        len_ = full_len_;

        compiled_output_conds_valid_ = compile_conds(*cols_, fed_conds_, &compiled_output_conds_);
        scan_access_class_ = fh_->get_file_hdr().num_pages >= 64 ? BufferAccessClass::BulkRead
                                                                  : BufferAccessClass::Default;
    }

    void beginTuple() override {
        feedback_.begin();
        current_visible_ = {};
        current_view_ = {};
        if (context_ != nullptr && context_->txn_mgr_ != nullptr) {
            context_->txn_mgr_->RecordSerializablePredicateRead(context_->txn_, tab_name_, *cols_, fed_conds_);
        }
        scan_ = std::make_unique<RmScan>(fh_, scan_access_class_);
        advance_to_next_visible();
    }

    void nextTuple() override {
        if (scan_ == nullptr || at_end_) {
            return;
        }
        current_visible_ = {};
        current_view_ = {};
        scan_->next();
        advance_to_next_visible();
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
        if (current_view_.cells != nullptr || current_view_.record != nullptr || current_view_.raw_data != nullptr) {
            return &current_view_;
        }
        return nullptr;
    }

    bool is_end() const override { return at_end_; }
    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta> &cols() const override { return *cols_; }
    std::string getType() override { return "SeqScanExecutor"; }
    ColMeta get_col_offset(const TabCol &target) override { return *get_col(*cols_, target); }
    Rid &rid() override { return rid_; }
};
