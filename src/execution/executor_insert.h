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
#include <cmath>
#include "common/index_runtime.h"
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "recovery/log_manager.h"
#include "system/sm.h"
#include "transaction/transaction_manager.h"

class InsertExecutor : public AbstractExecutor {
   private:
    const TabMeta *tab_ = nullptr;  // 表的元数据
    std::vector<Value> values_;     // 需要插入的数据
    RmFileHandle *fh_;              // 表的数据文件句柄
    int record_size_ = 0;
    std::string tab_name_;          // 表名称
    rmdb::u64 table_hash_{0};
    Rid rid_;                       // 插入的位置，由于系统默认插入时不指定位置，因此当前rid_在插入后才赋值
    SmManager *sm_manager_;
    std::vector<rmdb::IndexBinding> index_bindings_;
    std::vector<std::string> index_key_scratch_;
    BufferAccessStrategy cold_write_strategy_{BufferAccessClass::ColdWrite};
    bool use_cold_write_strategy_ = false;
    struct InsertedIndexKey {
        IxIndexHandle *ih;
        std::string key;
    };

   public:
    InsertExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<Value> values, Context *context,
                   std::shared_ptr<const PlanRuntimeCache> runtime_cache = nullptr) {
        sm_manager_ = sm_manager;
        tab_ = &sm_manager_->db_.get_table(tab_name);
        values_ = std::move(values);
        tab_name_ = tab_name;
        table_hash_ = HashTableName(tab_name_);
        if (values_.size() != tab_->cols.size()) {
            throw InvalidValueCountError();
        }
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        record_size_ = fh_->get_file_hdr().record_size;
        context_ = context;
        index_bindings_ = rmdb::bind_table_indexes(sm_manager_, tab_name_, *tab_);
        index_key_scratch_.resize(index_bindings_.size());
        for (size_t i = 0; i < index_bindings_.size(); ++i) {
            index_key_scratch_[i].resize(index_bindings_[i].meta->col_tot_len);
        }
        use_cold_write_strategy_ = context_ != nullptr && context_->txn_ == nullptr &&
                                   context_->txn_mgr_ == nullptr && context_->log_mgr_ == nullptr;
    };

    std::unique_ptr<RmRecord> Next() override {
        if (context_ != nullptr && context_->txn_mgr_ != nullptr) {
            context_->txn_mgr_->PromoteToWrite(context_->txn_, context_->log_mgr_);
        }
        // Make record buffer
        RmRecord rec(record_size_);
        for (size_t i = 0; i < values_.size(); i++) {
            auto &col = tab_->cols[i];
            auto &val = values_[i];
            if (col.type == TYPE_FLOAT && val.type == TYPE_INT) {
                val.set_float(static_cast<float>(val.int_val));
            } else if (col.type != val.type) {
                throw IncompatibleTypeError(coltype2str(col.type), coltype2str(val.type));
            }
            if (col.type == TYPE_FLOAT && !std::isfinite(val.float_val)) {
                throw RMDBError("FLOAT value must be finite");
            }
            val.init_raw(col.len);
            memcpy(rec.data + col.offset, val.raw->data, col.len);
        }
        if (context_ != nullptr && context_->txn_mgr_ != nullptr) {
            context_->txn_mgr_->EnsureKeyConflictFree(context_ == nullptr ? nullptr : context_->txn_,
                                                      tab_name_, *tab_, rec, nullptr, &index_bindings_);
        }
        for (size_t i = 0; i < index_bindings_.size(); ++i) {
            const auto &binding = index_bindings_[i];
            const auto &index = *binding.meta;
            if (!index.unique) {
                continue;
            }
            auto *ih = binding.ih;
            char *key = rmdb::build_index_key_into(index, rec.data, &index_key_scratch_[i]);
            std::vector<Rid> result;
            if (ih->get_value(key, &result, context_->txn_) && !result.empty()) {
                auto conflict = context_->txn_mgr_ == nullptr
                                    ? TransactionManager::UniqueKeyConflictResult::FAILURE
                                    : context_->txn_mgr_->ClassifyUniqueIndexConflict(
                                          context_ == nullptr ? nullptr : context_->txn_, tab_name_, index, key,
                                          result);
                if (conflict == TransactionManager::UniqueKeyConflictResult::ABORT) {
                    throw TransactionAbortException(context_->txn_->get_transaction_id(),
                                                    AbortReason::DEADLOCK_PREVENTION,
                                                    AbortSubReason::UNIQUE_KEY_CONFLICT);
                }
                throw RMDBError("Duplicate key on table " + tab_name_);
            }
        }
        PageId modified_page_id{};
        Page *modified_page = nullptr;
        bool defer_page_unpin = context_ != nullptr && context_->log_mgr_ != nullptr && context_->txn_ != nullptr;
        bool page_finalized = !defer_page_unpin;
        bool write_record_appended = false;
        bool insert_pending_visibility = false;
        std::vector<InsertedIndexKey> inserted_index_keys;
        auto rollback_inserted_index_keys = [&]() {
            Transaction *txn = context_ == nullptr ? nullptr : context_->txn_;
            for (auto iter = inserted_index_keys.rbegin(); iter != inserted_index_keys.rend(); ++iter) {
                iter->ih->delete_entry(iter->key.data(), txn);
            }
            inserted_index_keys.clear();
        };
        auto forget_insert_write_record = [&]() {
            if (context_ == nullptr || context_->txn_ == nullptr || !write_record_appended) {
                return;
            }
            auto &write_set = context_->txn_->get_write_set();
            if (write_set.empty()) {
                return;
            }
            WriteRecord &write_record = write_set.back();
            if (write_record.GetWriteType() == WType::INSERT_TUPLE &&
                write_record.GetTableName() == tab_name_ && write_record.GetRid() == rid_) {
                write_set.pop_back();
                write_record_appended = false;
            }
        };
        // Insert into record file
        bool defer_visibility = context_ != nullptr && context_->txn_ != nullptr && context_->txn_mgr_ != nullptr;
        rid_ = fh_->insert_record(rec.data, context_, defer_page_unpin ? &modified_page_id : nullptr,
                                  defer_page_unpin, defer_page_unpin ? &modified_page : nullptr,
                                  use_cold_write_strategy_ ? &cold_write_strategy_ : nullptr, defer_visibility);
        insert_pending_visibility = defer_visibility;
        try {
            if (context_ != nullptr && context_->txn_ != nullptr) {
                context_->txn_mgr_->UpdateTupleMeta(
                    tab_name_, rid_, TupleMeta{TXN_START_ID + context_->txn_->get_transaction_id(), false});
                fh_->publish_pending_insert(rid_);
                insert_pending_visibility = false;
                context_->txn_->emplace_write_record(WType::INSERT_TUPLE, tab_name_, rid_);
                write_record_appended = true;
            }
            // Insert into index
            for (size_t i = 0; i < index_bindings_.size(); ++i) {
                const auto &binding = index_bindings_[i];
                const auto &index = *binding.meta;
                auto *ih = binding.ih;
                char *key = rmdb::build_index_key_into(index, rec.data, rid_, &index_key_scratch_[i]);
                auto outcome = ih->insert_entry(key, rid_, context_->txn_);
                if (outcome.result == IxInsertResult::kInserted) {
                    inserted_index_keys.push_back(InsertedIndexKey{ih, std::string(key, index.col_tot_len)});
                } else if (outcome.result == IxInsertResult::kDuplicate) {
                    if (!index.unique) {
                        continue;
                    }
                    std::vector<Rid> result;
                    ih->get_value(key, &result, context_->txn_);
                    auto conflict = context_->txn_mgr_ == nullptr
                                        ? TransactionManager::UniqueKeyConflictResult::FAILURE
                                        : context_->txn_mgr_->ClassifyUniqueIndexConflict(
                                              context_ == nullptr ? nullptr : context_->txn_, tab_name_, index, key,
                                              result);
                    if (conflict == TransactionManager::UniqueKeyConflictResult::ABORT) {
                        throw TransactionAbortException(context_->txn_->get_transaction_id(),
                                                        AbortReason::DEADLOCK_PREVENTION,
                                                        AbortSubReason::UNIQUE_KEY_CONFLICT);
                    }
                    throw RMDBError("Duplicate key on table " + tab_name_);
                }
            }
            if (context_ != nullptr && context_->txn_mgr_ != nullptr) {
                context_->txn_mgr_->RecordSerializableWrite(context_->txn_, tab_name_, rid_, nullptr, &rec,
                                                            &tab_->cols);
            }
            if (context_ != nullptr && context_->log_mgr_ != nullptr && context_->txn_ != nullptr) {
                InsertLogRecord log_record(context_->txn_->get_transaction_id(), rec, rid_, table_hash_);
                log_record.prev_lsn_ = context_->txn_->get_prev_lsn();
                lsn_t lsn = context_->log_mgr_->add_log_to_buffer(&log_record);
                context_->txn_->get_write_set().back().SetLogPosition(lsn, log_record.prev_lsn_);
                context_->txn_->set_prev_lsn(lsn);
                if (!sm_manager_->get_bpm()->finalize_page_write_fast(modified_page, modified_page_id, lsn, true)) {
                    sm_manager_->get_bpm()->finalize_page_write(modified_page_id, lsn, true);
                }
                page_finalized = true;
            }
        } catch (...) {
            if (defer_page_unpin && !page_finalized) {
                if (!sm_manager_->get_bpm()->unpin_page_fast(modified_page, modified_page_id, true)) {
                    sm_manager_->get_bpm()->unpin_page(modified_page_id, true);
                }
            }
            rollback_inserted_index_keys();
            forget_insert_write_record();
            try {
                if (insert_pending_visibility) {
                    fh_->rollback_pending_insert(rid_);
                    insert_pending_visibility = false;
                } else {
                    fh_->delete_record(rid_, nullptr);
                }
            } catch (...) {
            }
            if (context_ != nullptr && context_->txn_mgr_ != nullptr) {
                context_->txn_mgr_->UpdateTupleMeta(tab_name_, rid_, std::nullopt);
                context_->txn_mgr_->UpdateVersionLink(tab_name_, rid_, std::nullopt, nullptr,
                                                       context_->txn_);
            }
            throw;
        }
        return nullptr;
    }
    Rid &rid() override { return rid_; }
};
