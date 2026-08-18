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
#include "common/index_runtime.h"
#include "execution_common.h"
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "recovery/log_manager.h"
#include "system/sm.h"
#include "transaction/transaction_manager.h"

class DeleteExecutor : public AbstractExecutor {
   private:
    const TabMeta *tab_ = nullptr;  // 表的元数据
    std::vector<Condition> conds_;  // delete的条件
    RmFileHandle *fh_;              // 表的数据文件句柄
    std::shared_ptr<std::vector<Rid>> rids_;  // statement-local RID scratch
    std::string tab_name_;          // 表名称
    rmdb::u64 table_hash_{0};
    SmManager *sm_manager_;
    std::vector<rmdb::IndexBinding> index_bindings_;
    std::vector<std::string> index_key_scratch_;

   public:
    DeleteExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<Condition> conds,
                   std::shared_ptr<std::vector<Rid>> rids, Context *context,
                   std::shared_ptr<const PlanRuntimeCache> runtime_cache = nullptr) {
        sm_manager_ = sm_manager;
        tab_name_ = tab_name;
        table_hash_ = HashTableName(tab_name_);
        tab_ = &sm_manager_->db_.get_table(tab_name);
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        conds_ = conds;
        rids_ = std::move(rids);
        context_ = context;
        index_bindings_ = rmdb::bind_table_indexes(sm_manager_, tab_name_, *tab_);
        index_key_scratch_.resize(index_bindings_.size());
        for (size_t i = 0; i < index_bindings_.size(); ++i) {
            index_key_scratch_[i].resize(index_bindings_[i].meta->col_tot_len);
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        if (context_ != nullptr && context_->txn_mgr_ != nullptr) {
            context_->txn_mgr_->PromoteToWrite(context_->txn_, context_->log_mgr_);
        }
        for (auto &rid : *rids_) {
            if (context_ != nullptr && context_->lock_mgr_ != nullptr && context_->txn_ != nullptr &&
                !context_->lock_mgr_->lock_exclusive_on_record(context_->txn_, rid, fh_->GetFd())) {
                throw TransactionAbortException(context_->txn_->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION,
                                                AbortSubReason::RECORD_LOCK_CONFLICT);
            }
            std::unique_ptr<RmRecord> rec;
            try {
                rec = fh_->get_record(rid, context_);
            } catch (RMDBError &) {
                // 目标行已被并发删除/物理回收:SI 下视为不可见的写冲突,重试而非致命。
                throw TransactionAbortException(context_->txn_->get_transaction_id(),
                                                AbortReason::DEADLOCK_PREVENTION,
                                                AbortSubReason::TARGET_NOT_VISIBLE);
            }
            RmRecord *old_rec = rec.get();
            if (context_ != nullptr && context_->txn_mgr_ != nullptr) {
                context_->txn_mgr_->EnsureWriteConflictFree(context_->txn_, tab_name_, rid);
                auto visible_old = context_->txn_mgr_->GetVisibleTuple(tab_name_, rid, context_->txn_);
                if (!visible_old.has_value()) {
                    throw TransactionAbortException(context_->txn_->get_transaction_id(),
                                                    AbortReason::DEADLOCK_PREVENTION,
                                                    AbortSubReason::TARGET_NOT_VISIBLE);
                }
            }
            if (context_ != nullptr && context_->txn_mgr_ != nullptr) {
                context_->txn_mgr_->RecordSerializableWrite(context_->txn_, tab_name_, rid, old_rec, nullptr,
                                                            &tab_->cols);
            }
            if (context_ != nullptr && context_->txn_ != nullptr) {
                auto undo_image = std::shared_ptr<RmRecord>(std::move(rec));
                old_rec = undo_image.get();
                if (context_->txn_mgr_ != nullptr) {
                    TupleMeta old_meta = context_->txn_mgr_->GetTupleMetaOrDefault(tab_name_, rid);
                    UndoLog undo_log{old_meta.is_deleted_, undo_image, {}, old_meta.ts_, UndoLink{}};
                    UndoLink undo_link = context_->txn_mgr_->AppendUndoLog(
                        tab_name_, rid, context_->txn_, std::move(undo_log));
                    context_->txn_mgr_->UpdateVersionLink(tab_name_, rid, VersionUndoLink{undo_link, true},
                                                           nullptr, context_->txn_);
                    context_->txn_mgr_->UpdateTupleMeta(
                        tab_name_, rid, TupleMeta{TXN_START_ID + context_->txn_->get_transaction_id(), true});
                }
                context_->txn_->emplace_write_record(WType::DELETE_TUPLE, tab_name_, rid, undo_image);
            }
            if (context_ != nullptr && context_->log_mgr_ != nullptr && context_->txn_ != nullptr) {
                DeleteLogRecord log_record(context_->txn_->get_transaction_id(), *old_rec, rid, table_hash_);
                log_record.prev_lsn_ = context_->txn_->get_prev_lsn();
                lsn_t lsn = context_->log_mgr_->add_log_to_buffer(&log_record);
                context_->txn_->get_write_set().back().SetLogPosition(lsn, log_record.prev_lsn_);
                context_->txn_->set_prev_lsn(lsn);
            }
            if (context_ != nullptr) {
                context_->affected_rows++;
            }
        }
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};
