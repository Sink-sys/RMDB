/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "transaction_manager.h"

#include "common/index_runtime.h"
#include "common/scope_exit.h"
#include "record/rm_file_handle.h"
#include "transaction_page_state.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using rmdb::transaction_page_state::BumpVisibilityEpoch;
using rmdb::transaction_page_state::MarkDirtySlot;
using rmdb::transaction_page_state::AddActiveMeta;
using rmdb::transaction_page_state::AddDeletedMeta;
using rmdb::transaction_page_state::AdvanceMaxCommittedMetaTs;
using rmdb::transaction_page_state::RemoveDeletedMeta;
using rmdb::transaction_page_state::RemoveUncommittedMeta;

// 轻量只读/延迟 BEGIN 事务不在注册表里,连接断开时会话 guard 可能
// 没有可查的注册表项,导致其 read_ts 永远钉住 watermark,版本回收
// 停摆并使 RSS 持续增长。thread_local 追踪本线程在途的轻量只读事务，
// 线程退出时统一 abort(提交/abort 时移除)。
struct ReadOnlyTxnTracker {
    struct Entry {
        TransactionManager *manager;
        Transaction *txn;
    };
    std::vector<Entry> entries;

    void Add(TransactionManager *manager, Transaction *txn) {
        entries.push_back(Entry{manager, txn});
    }

    void Remove(Transaction *txn) {
        for (size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].txn == txn) {
                entries.erase(entries.begin() + static_cast<long>(i));
                return;
            }
        }
    }

    ~ReadOnlyTxnTracker() {
        while (!entries.empty()) {
            Entry entry = entries.back();
            entries.pop_back();
            if (entry.txn == nullptr || entry.manager == nullptr ||
                entry.txn->get_state() != TransactionState::GROWING) {
                continue;
            }
            try {
                entry.manager->abort(entry.txn, nullptr);
            } catch (...) {
                // 线程退出路径的兜底:失败只能放弃,不能抛出。
            }
        }
    }
};

thread_local ReadOnlyTxnTracker t_read_only_tracker;

inline void spin_pause() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    std::this_thread::yield();
#endif
}

void delete_index_entries_bound(const std::vector<rmdb::IndexBinding> &bindings, const RmRecord &record,
                                const Rid &rid, Transaction *txn) {
    std::vector<std::string> key_scratch(bindings.size());
    for (size_t i = 0; i < bindings.size(); ++i) {
        const auto &binding = bindings[i];
        key_scratch[i].resize(binding.meta->col_tot_len);
        char *key = rmdb::build_index_key_into(*binding.meta, record.data, rid, &key_scratch[i]);
        binding.ih->delete_entry(key, txn);
    }
}

void delete_index_entries(SmManager *sm_manager, const TabMeta &tab, const std::string &tab_name,
                          const RmRecord &record, const Rid &rid, Transaction *txn) {
    auto bindings = rmdb::bind_table_indexes(sm_manager, tab_name, tab);
    delete_index_entries_bound(bindings, record, rid, txn);
}

void delete_changed_old_index_entries_bound(const std::vector<rmdb::IndexBinding> &bindings,
                                            const RmRecord &old_record, const RmRecord &new_record, const Rid &rid,
                                            Transaction *txn) {
    std::vector<std::string> old_key_scratch(bindings.size());
    std::vector<std::string> new_key_scratch(bindings.size());
    for (size_t i = 0; i < bindings.size(); ++i) {
        const auto &binding = bindings[i];
        old_key_scratch[i].resize(binding.meta->col_tot_len);
        new_key_scratch[i].resize(binding.meta->col_tot_len);
        char *old_key = rmdb::build_index_key_into(*binding.meta, old_record.data, rid, &old_key_scratch[i]);
        char *new_key = rmdb::build_index_key_into(*binding.meta, new_record.data, rid, &new_key_scratch[i]);
        if (memcmp(old_key, new_key, binding.meta->col_tot_len) != 0) {
            binding.ih->delete_entry(old_key, txn);
        }
    }
}

void delete_changed_old_index_entries(SmManager *sm_manager, const TabMeta &tab, const std::string &tab_name,
                                      const RmRecord &old_record, const RmRecord &new_record, const Rid &rid,
                                      Transaction *txn) {
    auto bindings = rmdb::bind_table_indexes(sm_manager, tab_name, tab);
    delete_changed_old_index_entries_bound(bindings, old_record, new_record, rid, txn);
}

void delete_changed_new_index_entries_bound(const std::vector<rmdb::IndexBinding> &bindings,
                                            const RmRecord &old_record, const RmRecord &new_record, const Rid &rid,
                                            Transaction *txn) {
    std::vector<std::string> old_key_scratch(bindings.size());
    std::vector<std::string> new_key_scratch(bindings.size());
    for (size_t i = 0; i < bindings.size(); ++i) {
        const auto &binding = bindings[i];
        old_key_scratch[i].resize(binding.meta->col_tot_len);
        new_key_scratch[i].resize(binding.meta->col_tot_len);
        char *old_key = rmdb::build_index_key_into(*binding.meta, old_record.data, rid, &old_key_scratch[i]);
        char *new_key = rmdb::build_index_key_into(*binding.meta, new_record.data, rid, &new_key_scratch[i]);
        if (memcmp(old_key, new_key, binding.meta->col_tot_len) != 0) {
            binding.ih->delete_entry(new_key, txn);
        }
    }
}

void release_locks(LockManager *lock_manager, Transaction *txn) {
    if (lock_manager == nullptr || txn == nullptr) {
        return;
    }
    lock_manager->unlock_all(txn);
}

}  // namespace

TransactionDrainGuard::~TransactionDrainGuard() {
    reset();
}

TransactionDrainGuard::TransactionDrainGuard(TransactionDrainGuard &&other) noexcept : manager_(other.manager_) {
    other.manager_ = nullptr;
}

TransactionDrainGuard &TransactionDrainGuard::operator=(TransactionDrainGuard &&other) noexcept {
    if (this != &other) {
        reset();
        manager_ = other.manager_;
        other.manager_ = nullptr;
    }
    return *this;
}

void TransactionDrainGuard::reset() {
    if (manager_ != nullptr) {
        manager_->ReleaseTransactionDrain();
        manager_ = nullptr;
    }
}

void TransactionManager::ReserveTransactionAdmission() {
    auto &state = lifecycle_state_;
    // 快路径:先原子递增,再检查阻塞标志。递增若发生在 drain 置位之前,
    // drain 的 count==0 等待会观察到该增量并等待其撤销/结束,不会漏计;
    // 若阻塞已置位,撤销增量并转入持锁慢路径等待。
    state.active_transaction_count.fetch_add(1, std::memory_order_acq_rel);
    if (!state.admission_blocked.load(std::memory_order_acquire)) {
        return;
    }
    state.active_transaction_count.fetch_sub(1, std::memory_order_acq_rel);
    std::unique_lock<std::mutex> admission_lock(state.admission_mutex);
    // 唤醒可能在等待 count==0 的 drain(我们的撤销可能使其归零)。
    state.admission_cv.notify_all();
    state.admission_cv.wait(admission_lock, [&] {
        return !state.admission_blocked.load(std::memory_order_acquire);
    });
    // 持锁递增:drain 的 blocked 置位与 count 检查在同一把锁下,不会交错。
    state.active_transaction_count.fetch_add(1, std::memory_order_acq_rel);
}

Transaction *TransactionManager::begin(Transaction* txn, LogManager* log_manager,
                                       IsolationLevel isolation_level,
                                       const std::vector<lock_data_key_t> &pre_snapshot_write_keys) {
    ReserveTransactionAdmission();

    std::unique_ptr<Transaction> owned_txn;
    bool serializable_registered = false;
    try {
        if (txn == nullptr) {
            owned_txn = std::make_unique<Transaction>(lifecycle_state_.next_txn_id++, isolation_level);
            txn = owned_txn.get();
        } else {
            txn->set_isolation_level(isolation_level);
        }
        txn->set_state(TransactionState::GROWING);
        txn->set_start_ts(lifecycle_state_.next_timestamp++);
        txn->set_response_order_keys(pre_snapshot_write_keys);
        // 热点写者在固定 SI 快照前尝试取得单继任准入。获得者的
        // read_ts 包含前任提交；竞争过深或短等待超时则直接回退到原生
        // SI，仍由后续标准写冲突检查裁决，不在 BEGIN 人为制造 abort。
        if (lock_manager_ != nullptr && !pre_snapshot_write_keys.empty()) {
            (void)lock_manager_->acquire_pre_snapshot_writes(txn, pre_snapshot_write_keys);
        }
        txn->set_read_ts(lifecycle_state_.last_commit_ts.load(std::memory_order_acquire));
        txn->set_snapshot_durability_lsn(
            lifecycle_state_.last_published_commit_lsn.load(std::memory_order_acquire));
        txn->set_commit_ts(INVALID_TS);
        txn->set_watermark_registered(false);
        txn->set_gc_ready(false);
        txn->set_admission_active(true);
        txn->set_ssi_metadata_released(isolation_level != IsolationLevel::SERIALIZABLE);
        if (isolation_level == IsolationLevel::SERIALIZABLE) {
            lifecycle_state_.active_serializable_count_.fetch_add(1, std::memory_order_relaxed);
            serializable_registered = true;
        }
        lifecycle_state_.running_txns.AddTxn(txn->get_read_ts());
        txn->set_watermark_registered(true);

        if (log_manager != nullptr) {
            BeginLogRecord log_record(txn->get_transaction_id());
            const lsn_t begin_lsn = log_manager->add_log_to_buffer(&log_record);
            txn->set_first_lsn(begin_lsn);
            txn->set_prev_lsn(begin_lsn);
        }
        transaction_registry_.Insert(txn);
        owned_txn.release();
        return txn;
    } catch (...) {
        if (serializable_registered) {
            lifecycle_state_.active_serializable_count_.fetch_sub(1, std::memory_order_relaxed);
        }
        if (txn != nullptr) {
            if (txn->watermark_registered()) {
                lifecycle_state_.running_txns.RemoveTxn(txn->get_read_ts());
                txn->set_watermark_registered(false);
            }
            release_locks(lock_manager_, txn);
            txn->set_admission_active(false);
        }
        CancelTransactionAdmissionReservation();
        throw;
    }
}

Transaction *TransactionManager::begin_read_only(
    IsolationLevel isolation_level, bool reserve_txn_id) {
    if (isolation_level != IsolationLevel::SNAPSHOT_ISOLATION) {
        throw InternalError("Lightweight read-only transactions require snapshot isolation");
    }
    ReserveTransactionAdmission();
    std::unique_ptr<Transaction> txn;
    try {
        const txn_id_t txn_id = reserve_txn_id ? lifecycle_state_.next_txn_id++ : INVALID_TXN_ID;
        txn = std::make_unique<Transaction>(txn_id, isolation_level);
        timestamp_t read_ts = lifecycle_state_.last_commit_ts.load(std::memory_order_acquire);
        txn->set_state(TransactionState::GROWING);
        txn->set_start_ts(read_ts);
        txn->set_read_ts(read_ts);
        txn->set_snapshot_durability_lsn(
            lifecycle_state_.last_published_commit_lsn.load(std::memory_order_acquire));
        txn->set_commit_ts(INVALID_TS);
        txn->set_lightweight_read_only(true);
        txn->set_watermark_registered(false);
        txn->set_gc_ready(false);
        txn->set_admission_active(true);
        txn->set_ssi_metadata_released(true);
        lifecycle_state_.running_txns.AddTxn(read_ts);
        txn->set_watermark_registered(true);
        t_read_only_tracker.Add(this, txn.get());
        return txn.release();
    } catch (...) {
        if (txn != nullptr) {
            if (txn->watermark_registered()) {
                lifecycle_state_.running_txns.RemoveTxn(txn->get_read_ts());
                txn->set_watermark_registered(false);
            }
            release_locks(lock_manager_, txn.get());
            txn->set_admission_active(false);
        }
        CancelTransactionAdmissionReservation();
        throw;
    }
}

void TransactionManager::PromoteToWrite(Transaction *txn, LogManager *log_manager) {
    if (txn == nullptr || !txn->is_lightweight_read_only()) {
        return;
    }
    if (txn->get_isolation_level() != IsolationLevel::SNAPSHOT_ISOLATION ||
        txn->get_state() != TransactionState::GROWING) {
        throw InternalError("Only a growing SI snapshot can be promoted to write");
    }
    txn_id_t txn_id = txn->get_transaction_id();
    if (txn_id == INVALID_TXN_ID) {
        txn_id = lifecycle_state_.next_txn_id++;
    }
    lsn_t begin_lsn = INVALID_LSN;
    if (log_manager != nullptr) {
        BeginLogRecord log_record(txn_id);
        begin_lsn = log_manager->add_log_to_buffer(&log_record);
    }
    txn->set_transaction_id(txn_id);
    txn->set_first_lsn(begin_lsn);
    txn->set_prev_lsn(begin_lsn);
    transaction_registry_.Insert(txn);
    txn->set_lightweight_read_only(false);
    t_read_only_tracker.Remove(txn);
}

TransactionDrainGuard TransactionManager::BlockNewTransactionsAndWait() {
    std::unique_lock<std::mutex> admission_lock(lifecycle_state_.admission_mutex);
    lifecycle_state_.admission_cv.wait(admission_lock, [&] {
        return !lifecycle_state_.admission_blocked.load(std::memory_order_acquire);
    });
    // 置位后,快路径 begin 的"递增→复查"会观察到阻塞并撤销/等待;
    // 置位前已递增的活跃事务都会计入下面的 count==0 等待。
    lifecycle_state_.admission_blocked.store(true, std::memory_order_release);
    lifecycle_state_.admission_cv.wait(admission_lock, [&] {
        return lifecycle_state_.active_transaction_count.load(std::memory_order_acquire) == 0;
    });
    return TransactionDrainGuard(this);
}

TransactionDrainGuard TransactionManager::TryBlockNewTransactionsIfIdle() {
    std::lock_guard<std::mutex> admission_lock(lifecycle_state_.admission_mutex);
    if (lifecycle_state_.admission_blocked.load(std::memory_order_acquire) ||
        lifecycle_state_.active_transaction_count.load(std::memory_order_acquire) != 0) {
        return {};
    }
    lifecycle_state_.admission_blocked.store(true, std::memory_order_release);
    return TransactionDrainGuard(this);
}

TransactionDrainGuard TransactionManager::BlockNewTransactionsAndWaitFor(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> admission_lock(lifecycle_state_.admission_mutex);
    if (lifecycle_state_.admission_blocked.load(std::memory_order_acquire)) {
        return {};
    }
    lifecycle_state_.admission_blocked.store(true, std::memory_order_release);
    if (!lifecycle_state_.admission_cv.wait_for(admission_lock, timeout, [&] {
            return lifecycle_state_.active_transaction_count.load(std::memory_order_acquire) == 0;
        })) {
        lifecycle_state_.admission_blocked.store(false, std::memory_order_release);
        admission_lock.unlock();
        lifecycle_state_.admission_cv.notify_all();
        return {};
    }
    return TransactionDrainGuard(this);
}

size_t TransactionManager::ActiveTransactionCount() const {
    return lifecycle_state_.active_transaction_count.load(std::memory_order_acquire);
}

void TransactionManager::FinishTransactionAdmission(Transaction *txn) {
    if (txn == nullptr || !txn->admission_active()) {
        return;
    }
    txn->set_admission_active(false);
    CancelTransactionAdmissionReservation();
}

void TransactionManager::CancelTransactionAdmissionReservation() {
    // 原子递减;仅在 drain 阻塞期间才持锁唤醒等待者。
    size_t prior = lifecycle_state_.active_transaction_count.load(std::memory_order_acquire);
    while (true) {
        if (prior == 0) {
            throw InternalError("Transaction admission count underflow");
        }
        if (lifecycle_state_.active_transaction_count.compare_exchange_weak(
                prior, prior - 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
            break;
        }
    }
    if (lifecycle_state_.admission_blocked.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> admission_lock(lifecycle_state_.admission_mutex);
        lifecycle_state_.admission_cv.notify_all();
    }
}

void TransactionManager::ReleaseTransactionDrain() {
    std::lock_guard<std::mutex> admission_lock(lifecycle_state_.admission_mutex);
    lifecycle_state_.admission_blocked.store(false, std::memory_order_release);
    lifecycle_state_.admission_cv.notify_all();
}

TransactionManager::CheckpointTxnTableSnapshot
TransactionManager::CollectCheckpointTxnTable(Transaction *exclude_txn) {
    return transaction_registry_.WithAllShared([&](const TransactionRegistry::AllView &transactions) {
        CheckpointTxnTableSnapshot snapshot;
        snapshot.entries.reserve(transactions.Size());
        transactions.ForEach([&](txn_id_t txn_id, Transaction *txn) {
            if (txn == nullptr || txn == exclude_txn) {
                return;
            }
            // lastLSN 必须先于状态读取。若 COMMIT/ABORT 正在并发推进，
            // GROWING 快照可以保守包含事务；状态日志会在 analysis 扫描中覆盖它。
            const lsn_t last_lsn = txn->get_prev_lsn();
            if (txn->get_state() == TransactionState::GROWING) {
                const lsn_t first_lsn = txn->get_first_lsn();
                if (first_lsn == INVALID_LSN) {
                    throw InternalError("active write transaction has no first WAL LSN");
                }
                snapshot.entries.push_back(
                    CheckpointTxnEntry{txn_id, RecoveryTxnStatus::kRunning, last_lsn});
                if (snapshot.oldest_first_lsn == INVALID_LSN ||
                    first_lsn < snapshot.oldest_first_lsn) {
                    snapshot.oldest_first_lsn = first_lsn;
                }
            }
        });
        return snapshot;
    });
}

void TransactionManager::commit(Transaction* txn, LogManager* log_manager) {
    if (txn == nullptr) {
        return;
    }
    if (txn->is_lightweight_read_only()) {
        // Early-lock-release writers may already be visible to this snapshot
        // while their group flush is in flight.  A read-only transaction has
        // no later WAL record to carry that dependency, so enforce it here.
        const lsn_t durability_lsn = txn->get_snapshot_durability_lsn();
        if (log_manager != nullptr && durability_lsn != INVALID_LSN) {
            log_manager->flush_log_to_disk_until_group(durability_lsn);
        }
        timestamp_t commit_ts = lifecycle_state_.last_commit_ts.load(std::memory_order_acquire);
        txn->set_commit_ts(commit_ts);
        txn->set_state(TransactionState::COMMITTED);
        if (txn->watermark_registered()) {
            lifecycle_state_.running_txns.FinishTxn(txn->get_read_ts(), commit_ts);
            txn->set_watermark_registered(false);
        }
        release_locks(lock_manager_, txn);
        txn->wait_for_response_dependencies();
        FinishTransactionAdmission(txn);
        t_read_only_tracker.Remove(txn);
        if (!txn->response_gate_deferred()) {
            txn->complete_response_gate();
        }
        delete txn;
        return;
    }
    bool admission_finished = false;
    auto completion_guard = rmdb::make_scope_exit([&] {
        if (!admission_finished &&
            (txn->get_state() == TransactionState::COMMITTED || txn->get_state() == TransactionState::ABORTED)) {
            FinishTransactionAdmission(txn);
            if (!txn->response_gate_deferred()) {
                txn->complete_response_gate();
            }
        }
    });
    auto &write_set = txn->get_write_set();
    const bool has_writes = !write_set.empty();
    lsn_t durability_lsn = txn->get_snapshot_durability_lsn();

    struct PendingCommitTuple {
        std::string tab_name;
        rmdb::u32 table_id;
        Rid rid;
        bool is_deleted;
        bool publish_version;
        lsn_t delete_lsn;
        size_t write_sequence;
    };
    struct PendingCommitSlot {
        slot_offset_t slot;
        bool is_deleted;
        bool publish_version;
    };
    struct CommitPageBatch {
        std::string tab_name;
        page_id_t page_no;
        std::shared_ptr<TableVersionInfo> table_info;
        std::shared_ptr<PageVersionInfo> page_info;
        std::vector<PendingCommitSlot> slots;
    };

    std::vector<PendingCommitTuple> pending_tuples;
    pending_tuples.reserve(write_set.size());
    // 本事务触达的稳定表 id 缓存(通常 1-4 张表),避免逐写记录查表。
    std::unordered_map<std::string, rmdb::u32> table_id_cache;
    auto table_id_of = [&](const std::string &tab_name) -> rmdb::u32 {
        auto cache_iter = table_id_cache.find(tab_name);
        if (cache_iter != table_id_cache.end()) {
            return cache_iter->second;
        }
        auto table_info = GetOrCreateTableVersionInfo(tab_name);
        rmdb::u32 id = table_info == nullptr ? 0 : table_info->table_id_;
        table_id_cache.emplace(tab_name, id);
        return id;
    };
    size_t write_sequence = 0;
    for (auto &write_record : write_set) {
        const std::string &tab_name = write_record.GetTableName();
        const Rid &rid = write_record.GetRid();
        WType write_type = write_record.GetWriteType();
        pending_tuples.push_back(PendingCommitTuple{
            tab_name, table_id_of(tab_name), rid, write_type == WType::DELETE_TUPLE,
            write_type != WType::INSERT_TUPLE,
            write_type == WType::DELETE_TUPLE ? write_record.GetLogLsn() : INVALID_LSN,
            write_sequence++});
    }
    std::sort(pending_tuples.begin(), pending_tuples.end(), [](const auto &lhs, const auto &rhs) {
        if (lhs.tab_name != rhs.tab_name) {
            return lhs.tab_name < rhs.tab_name;
        }
        if (lhs.rid.page_no != rhs.rid.page_no) {
            return lhs.rid.page_no < rhs.rid.page_no;
        }
        if (lhs.rid.slot_no != rhs.rid.slot_no) {
            return lhs.rid.slot_no < rhs.rid.slot_no;
        }
        return lhs.write_sequence < rhs.write_sequence;
    });
    size_t compacted_count = 0;
    for (size_t i = 0; i < pending_tuples.size(); ++i) {
        PendingCommitTuple &pending = pending_tuples[i];
        if (compacted_count > 0 && pending_tuples[compacted_count - 1].tab_name == pending.tab_name &&
            pending_tuples[compacted_count - 1].rid == pending.rid) {
            PendingCommitTuple &compacted = pending_tuples[compacted_count - 1];
            compacted.is_deleted = pending.is_deleted;
            compacted.publish_version = compacted.publish_version || pending.publish_version;
            compacted.delete_lsn = pending.is_deleted ? pending.delete_lsn : INVALID_LSN;
            continue;
        }
        if (compacted_count != i) {
            pending_tuples[compacted_count] = std::move(pending);
        }
        ++compacted_count;
    }
    pending_tuples.resize(compacted_count);

    std::vector<CommitPageBatch> page_batches;
    for (const auto &pending : pending_tuples) {
        if (page_batches.empty() || page_batches.back().tab_name != pending.tab_name ||
            page_batches.back().page_no != pending.rid.page_no) {
            auto table_info = GetOrCreateTableVersionInfo(pending.tab_name);
            auto page_info = GetOrCreatePageVersionInfoOnTable(
                table_info, pending.tab_name, pending.rid.page_no);
            page_batches.push_back(CommitPageBatch{
                pending.tab_name, pending.rid.page_no, std::move(table_info), std::move(page_info), {}});
        }
        slot_offset_t slot = static_cast<slot_offset_t>(pending.rid.slot_no);
        if (pending.rid.slot_no < 0 || !page_batches.back().page_info->CanTrackSlot(slot)) {
            throw InternalError("Tuple metadata slot is out of range");
        }
        page_batches.back().slots.push_back(PendingCommitSlot{slot, pending.is_deleted, pending.publish_version});
    }

    if (log_manager != nullptr && has_writes) {
        CommitLogRecord log_record(txn->get_transaction_id());
        log_record.prev_lsn_ = txn->get_prev_lsn();
        lsn_t commit_lsn = log_manager->add_log_to_buffer(&log_record);
        txn->set_prev_lsn(commit_lsn);
        durability_lsn = commit_lsn + log_record.log_tot_len_ - 1;
    }

    struct PendingLogicalDelete {
        size_t shard_index;
        TransactionLogicalDeleteKey key;
        lsn_t rec_lsn;
    };
    std::vector<PendingLogicalDelete> logical_deletes;
    for (const auto &pending : pending_tuples) {
        if (!pending.is_deleted || pending.table_id == 0 || pending.delete_lsn == INVALID_LSN) {
            continue;
        }
        if (logical_deletes.empty()) {
            logical_deletes.reserve(pending_tuples.size());
        }
        TransactionLogicalDeleteKey key{pending.table_id, pending.rid};
        logical_deletes.push_back(PendingLogicalDelete{
            TransactionLogicalDeleteShardFor(key), key, pending.delete_lsn});
    }
    std::sort(logical_deletes.begin(), logical_deletes.end(), [](const auto &lhs, const auto &rhs) {
        return lhs.shard_index < rhs.shard_index;
    });
    for (size_t first = 0; first < logical_deletes.size();) {
        size_t last = first + 1;
        while (last < logical_deletes.size() &&
               logical_deletes[last].shard_index == logical_deletes[first].shard_index) {
            ++last;
        }
        auto &shard = gc_state_.logical_delete_shards[logical_deletes[first].shard_index];
        std::lock_guard<std::mutex> delete_lock(shard.mutex);
        for (size_t i = first; i < last; ++i) {
            const auto &pending = logical_deletes[i];
            auto [iter, inserted] = shard.deletes.emplace(
                pending.key,
                TransactionLogicalDelete{txn->get_transaction_id(), pending.rec_lsn});
            if (!inserted) {
                if (iter->second.txn_id != txn->get_transaction_id()) {
                    throw InternalError("logical DELETE RID is owned by another transaction");
                }
                iter->second.rec_lsn = pending.rec_lsn;
            }
        }
        first = last;
    }

    // 各事务可以并行准备互不相干的页版本，但提交票号负责串行化最终的可见性发布。
    // 只有更早票号都准备完毕后，读事务的快照水位才会前移，从而避免观察到提交序列中的空洞。
    rmdb::u64 publish_ticket = 0;
    timestamp_t commit_version = INVALID_TS;
    {
        // 在同一临界区内分配票号和逻辑时间戳。begin/abort 也可能消耗时间戳，
        // 但真正提交的版本仍须按票号对应的时间戳单调发布。
        std::lock_guard<std::mutex> lock(lifecycle_state_.commit_publish_mutex);
        publish_ticket = lifecycle_state_.next_commit_ticket.fetch_add(1, std::memory_order_relaxed);
        commit_version = lifecycle_state_.next_timestamp++;
    }
    txn->set_commit_ts(commit_version);
    for (auto &batch : page_batches) {
        std::unique_lock<std::shared_mutex> page_lock(batch.page_info->mutex_);
        BumpVisibilityEpoch(batch.page_info);
        // 未提交计数延迟到 max_committed 前移之后统一递减:借读快路径
        // (IsSnapshotPageCleanVisible)只要看到 uncommitted==0 就认为该槽位
        // 已提交可见,若先递减再写 meta/max,窗口内读者会把本快照之后才
        // 提交的新值当成可见值读出。先 SetTupleMeta + AdvanceMax,再递减
        // uncommitted,读者要么看到 uncommitted>0(走慢路径),要么看到
        // 完全发布后的状态。
        std::vector<slot_offset_t> deferred_uncommitted_slots;
        for (const auto &[slot, is_deleted, publish_version] : batch.slots) {
            TupleMeta *current_meta = batch.page_info->GetMutableTupleMeta(slot);
            bool had_meta = current_meta != nullptr;
            bool was_uncommitted = had_meta && current_meta->ts_ >= TXN_START_ID;
            bool was_deleted = had_meta && current_meta->is_deleted_;
            if (!had_meta) {
                Rid rid{batch.page_no, static_cast<int>(slot)};
                MarkDirtySlot(batch.table_info, batch.page_info, rid);
                AddActiveMeta(batch.page_info);
            }
            if (was_uncommitted) {
                deferred_uncommitted_slots.push_back(slot);
            }
            if (!was_deleted && is_deleted) {
                AddDeletedMeta(batch.page_info);
            } else if (was_deleted && !is_deleted) {
                RemoveDeletedMeta(batch.page_info);
            }
            batch.page_info->SetTupleMeta(slot, TupleMeta{commit_version, is_deleted});
            if (publish_version) {
                VersionUndoLink *version = batch.page_info->GetMutableVersion(slot);
                if (version == nullptr) {
                    continue;
                }
                version->in_progress_ = false;
            }
        }
        AdvanceMaxCommittedMetaTs(batch.page_info, commit_version);
        for (slot_offset_t slot : deferred_uncommitted_slots) {
            RemoveUncommittedMeta(batch.page_info);
        }
        BumpVisibilityEpoch(batch.page_info);
    }

    {
        // 快速路径：发布临界区极短（毫秒级 WAL 刷盘已在临界区外完成），等待者先无锁自旋观察
        // 发布序号，避免每次提交 notify_all 惊动所有等待者去抢同一把全局锁。
        constexpr int kPublishSpinIterations = 200;  // ~20-40μs
        rmdb::u64 next = lifecycle_state_.next_publish_ticket.load(std::memory_order_acquire);
        for (int i = 0; i < kPublishSpinIterations && next != publish_ticket; ++i) {
            spin_pause();
            next = lifecycle_state_.next_publish_ticket.load(std::memory_order_acquire);
        }
        std::unique_lock<std::mutex> lock(lifecycle_state_.commit_publish_mutex);
        lifecycle_state_.commit_publish_cv.wait(lock, [&] {
            return publish_ticket ==
                   lifecycle_state_.next_publish_ticket.load(std::memory_order_acquire);
        });
        // 事务对象在稍后的 GC 移交前仍由注册表管理。TransactionState 是原子状态，
        // 因此这里不获取注册表全局锁；SSI 图维护和 GC 可在各自的注册表保护下安全观察完成状态。
        txn->set_state(TransactionState::COMMITTED);
        // 串行 WAL 中，较大的 LSN 同时覆盖所有较早的提交依赖。必须先发布
        // 依赖前沿、再 release 发布 read_ts；begin 因而不会取得包含本版本
        // 却遗漏其 WAL 依赖的快照。
        if (durability_lsn != INVALID_LSN) {
            const lsn_t published = lifecycle_state_.last_published_commit_lsn.load(
                std::memory_order_relaxed);
            if (durability_lsn > published) {
                lifecycle_state_.last_published_commit_lsn.store(
                    durability_lsn, std::memory_order_relaxed);
            }
        }
        lifecycle_state_.last_commit_ts.store(commit_version, std::memory_order_release);
        lifecycle_state_.next_publish_ticket.store(publish_ticket + 1, std::memory_order_release);
    }
    lifecycle_state_.commit_publish_cv.notify_all();

    // 水印更新放回临界区外:GC 只要求水印是活跃快照读时间戳的保守下界,
    // 延迟推进只会让 GC 更保守,不会破坏可见性;commit_ts_ 在前沿处取 max 防回退。
    if (txn->watermark_registered()) {
        lifecycle_state_.running_txns.FinishTxn(txn->get_read_ts(), commit_version);
        txn->set_watermark_registered(false);
    }

    for (auto &write_record : write_set) {
        const std::string tab_name = write_record.GetTableName();
        Rid rid = write_record.GetRid();
        if (write_record.GetWriteType() == WType::DELETE_TUPLE) {
            RmRecord &old_record = write_record.GetRecord();
            auto bindings = rmdb::bind_table_indexes(sm_manager_, tab_name, sm_manager_->db_.get_table(tab_name));
            for (const auto &binding : bindings) {
                RecordSnapshotIndexRetirement(tab_name, *binding.meta, rid, txn->get_commit_ts());
            }
            delete_index_entries(sm_manager_, sm_manager_->db_.get_table(tab_name), tab_name, old_record, rid, txn);
        } else if (write_record.GetWriteType() == WType::UPDATE_TUPLE) {
            if (!write_record.IndexKeysChanged()) {
                continue;
            }
            RmRecord &old_record = write_record.GetRecord();
            auto current = sm_manager_->fhs_.at(tab_name)->get_record(rid, nullptr);
            auto bindings = rmdb::bind_table_indexes(sm_manager_, tab_name, sm_manager_->db_.get_table(tab_name));
            std::vector<std::string> old_key_scratch(bindings.size());
            std::vector<std::string> new_key_scratch(bindings.size());
            for (size_t i = 0; i < bindings.size(); ++i) {
                const auto &binding = bindings[i];
                old_key_scratch[i].resize(binding.meta->col_tot_len);
                new_key_scratch[i].resize(binding.meta->col_tot_len);
                char *old_key = rmdb::build_index_key_into(*binding.meta, old_record.data, rid, &old_key_scratch[i]);
                char *new_key = rmdb::build_index_key_into(*binding.meta, current->data, rid, &new_key_scratch[i]);
                if (memcmp(old_key, new_key, binding.meta->col_tot_len) != 0) {
                    RecordSnapshotIndexRetirement(tab_name, *binding.meta, rid, txn->get_commit_ts());
                }
            }
            delete_changed_old_index_entries(sm_manager_, sm_manager_->db_.get_table(tab_name), tab_name,
                                             old_record, *current, rid, txn);
        }
    }

    std::vector<RetiredTuple> retired_tuples;
    retired_tuples.reserve(pending_tuples.size());
    for (const auto &pending : pending_tuples) {
        retired_tuples.push_back(RetiredTuple{
            pending.table_id, pending.rid, txn->get_transaction_id(), pending.is_deleted,
            pending.is_deleted ? pending.delete_lsn : INVALID_LSN});
    }
    EnqueueRetiredTuples(txn->get_commit_ts(), std::move(retired_tuples));

    txn->clear_write_records();
    if (txn->get_isolation_level() != IsolationLevel::SERIALIZABLE) {
        txn->clear_serializable_state();
        txn->set_ssi_metadata_released(true);
    }
    // 仍持有真实记录锁，因此同一逻辑键在这里的注册顺序就是实际写入
    // 串行顺序；准入超时并回退原生 SI 的成功事务也不会绕过响应链。
    if (lock_manager_ != nullptr) {
        lock_manager_->register_response_order(txn);
    }
    release_locks(lock_manager_, txn);

    // Early lock release: COMMIT 已进入串行 WAL 且事务不再可能回滚后，先让
    // 后继取得热点记录并继续执行，再在返回成功前等待自身及全部前驱 WAL
    // 稳定。后继写事务的更大 commit LSN 自然携带该依赖；只读事务在上方
    // 使用 snapshot_durability_lsn 显式等待。这样只缩短持锁区间，不放宽
    // durability ACK、WAL-before-page 或 SI 的固定快照规则。
    if (log_manager != nullptr && durability_lsn != INVALID_LSN) {
        log_manager->flush_log_to_disk_until_group(durability_lsn);
    }
    // 记录锁和逻辑准入令牌均已释放，后继可以执行并加入同一次 group
    // flush。这里只约束客户端可观察的同键响应顺序，不把网络延迟重新
    // 放回锁持有区间。
    txn->wait_for_response_dependencies();
    FinishTransactionAdmission(txn);
    admission_finished = true;
    txn->set_gc_ready(true);
    QueueFinishedTransactionForGc(txn);
    if (!txn->response_gate_deferred()) {
        txn->complete_response_gate();
    }
}

/**
 * @description: 事务的终止（回滚）方法
 * @param {Transaction *} txn 需要回滚的事务
 * @param {LogManager} *log_manager 日志管理器指针
 */
void TransactionManager::abort(Transaction * txn, LogManager *log_manager) {
    if (txn == nullptr) {
        return;
    }
    if (txn->is_lightweight_read_only()) {
        txn->set_state(TransactionState::ABORTED);
        if (txn->watermark_registered()) {
            lifecycle_state_.running_txns.FinishTxn(
                txn->get_read_ts(), lifecycle_state_.last_commit_ts.load(std::memory_order_acquire));
            txn->set_watermark_registered(false);
        }
        release_locks(lock_manager_, txn);
        txn->wait_for_response_dependencies();
        FinishTransactionAdmission(txn);
        t_read_only_tracker.Remove(txn);
        if (!txn->response_gate_deferred()) {
            txn->complete_response_gate();
        }
        delete txn;
        return;
    }
    bool admission_finished = false;
    auto completion_guard = rmdb::make_scope_exit([&] {
        if (!admission_finished &&
            (txn->get_state() == TransactionState::COMMITTED || txn->get_state() == TransactionState::ABORTED)) {
            FinishTransactionAdmission(txn);
            if (!txn->response_gate_deferred()) {
                txn->complete_response_gate();
            }
        }
    });
    struct AbortTableCache {
        TabMeta *tab = nullptr;
        RmFileHandle *fh = nullptr;
        std::vector<rmdb::IndexBinding> index_bindings;
    };
    std::unordered_map<std::string, AbortTableCache> table_cache;
    auto get_table_cache = [&](const std::string &tab_name) -> AbortTableCache & {
        auto cache_iter = table_cache.find(tab_name);
        if (cache_iter != table_cache.end()) {
            return cache_iter->second;
        }
        AbortTableCache cache;
        cache.tab = &sm_manager_->db_.get_table(tab_name);
        cache.fh = sm_manager_->fhs_.at(tab_name).get();
        cache.index_bindings = rmdb::bind_table_indexes(sm_manager_, tab_name, *cache.tab);
        auto inserted = table_cache.emplace(tab_name, std::move(cache));
        return inserted.first->second;
    };
    auto append_clr = [&](WriteRecord &write_record, LogRecord &original,
                          bool completes_undo) -> lsn_t {
        if (log_manager == nullptr || write_record.GetLogLsn() == INVALID_LSN) {
            return INVALID_LSN;
        }
        original.lsn_ = write_record.GetLogLsn();
        original.prev_lsn_ = write_record.GetLogPrevLsn();
        // The final CLR is itself the durable completion marker. Its
        // undoNextLSN skips BEGIN because no action remains to undo. If a crash
        // happens before applying the compensation, REDO replays this CLR; if
        // it happens afterwards, recovery reaches INVALID without revisiting a
        // RID that a later transaction may already have reused.
        ClrLogRecord clr(txn->get_transaction_id(),
                         completes_undo ? INVALID_LSN : original.prev_lsn_, original);
        clr.prev_lsn_ = txn->get_prev_lsn();
        lsn_t clr_lsn = log_manager->add_log_to_buffer(&clr);
        txn->set_prev_lsn(clr_lsn);
        return clr_lsn;
    };
    auto finalize_heap_undo = [&](Page *page, PageId page_id, lsn_t clr_lsn) {
        BufferPoolManager *bpm = sm_manager_->get_bpm();
        if (clr_lsn != INVALID_LSN) {
            if (bpm->finalize_page_write_fast(page, page_id, clr_lsn, true) ||
                bpm->finalize_page_write(page_id, clr_lsn, true)) {
                return;
            }
            throw InternalError("failed to finalize transaction CLR page");
        }
        if (!bpm->unpin_page_fast(page, page_id, true) && !bpm->unpin_page(page_id, true)) {
            throw InternalError("failed to unpin transaction undo page");
        }
    };
    auto restore_tuple_version = [&](const std::string &tab_name, const Rid &rid) {
        auto version_link = GetVersionLink(tab_name, rid);
        if (version_link.has_value() && version_link->prev_.IsValid()) {
            timestamp_t undo_ts = INVALID_TS;
            bool undo_deleted = false;
            UndoLink undo_prev;
            if (PeekUndoLogMeta(version_link->prev_, &undo_ts, &undo_deleted, &undo_prev)) {
                UpdateTupleMeta(tab_name, rid, TupleMeta{undo_ts, undo_deleted});
                UpdateVersionLink(
                    tab_name, rid,
                    VersionUndoLink::FromOptionalUndoLink(
                        undo_prev.IsValid() ? std::optional<UndoLink>(undo_prev) : std::nullopt),
                    nullptr, txn);
                return;
            }
        }
        UpdateTupleMeta(tab_name, rid, std::nullopt);
        UpdateVersionLink(tab_name, rid, std::nullopt, nullptr, txn);
    };
    auto &write_set = txn->get_write_set();
    while (!write_set.empty()) {
        WriteRecord &write_record = write_set.back();
        const bool completes_undo = write_set.size() == 1;
        const std::string tab_name = write_record.GetTableName();
        auto &table = get_table_cache(tab_name);
        Rid rid = write_record.GetRid();

        if (write_record.GetWriteType() == WType::INSERT_TUPLE) {
            auto inserted = table.fh->get_record(rid, nullptr);
            InsertLogRecord original(txn->get_transaction_id(), *inserted, rid, tab_name);
            lsn_t clr_lsn = append_clr(write_record, original, completes_undo);
            delete_index_entries_bound(table.index_bindings, *inserted, rid, txn);
            PageId modified_page_id{};
            Page *modified_page = nullptr;
            table.fh->delete_record(rid, nullptr, &modified_page_id, true, &modified_page);
            finalize_heap_undo(modified_page, modified_page_id, clr_lsn);
            UpdateTupleMeta(tab_name, rid, std::nullopt);
            UpdateVersionLink(tab_name, rid, std::nullopt, nullptr, txn);
        } else if (write_record.GetWriteType() == WType::DELETE_TUPLE) {
            RmRecord &old_record = write_record.GetRecord();
            DeleteLogRecord original(txn->get_transaction_id(), old_record, rid, tab_name);
            lsn_t clr_lsn = append_clr(write_record, original, completes_undo);
            // DELETE 是运行时逻辑墓碑，但 repeat-history REDO 会物理删除 loser 的行。
            // 用 CLR LSN 重新写入旧镜像，使当前页和崩溃恢复走同一补偿语义。
            if (clr_lsn != INVALID_LSN) {
                PageId modified_page_id{};
                Page *modified_page = nullptr;
                table.fh->update_record(rid, old_record.data, nullptr, &modified_page_id, true,
                                        &modified_page);
                finalize_heap_undo(modified_page, modified_page_id, clr_lsn);
            }
            restore_tuple_version(tab_name, rid);
            UntrackLogicalDelete(tab_name, rid, txn->get_transaction_id());
        } else if (write_record.GetWriteType() == WType::UPDATE_TUPLE) {
            RmRecord &old_record = write_record.GetRecord();
            auto current = table.fh->get_record(rid, nullptr);
            UpdateLogRecord original(txn->get_transaction_id(), old_record, *current, rid, tab_name,
                                     &table.tab->cols);
            lsn_t clr_lsn = append_clr(write_record, original, completes_undo);
            if (write_record.IndexKeysChanged()) {
                delete_changed_new_index_entries_bound(table.index_bindings, old_record, *current, rid, txn);
            }
            PageId modified_page_id{};
            Page *modified_page = nullptr;
            table.fh->update_record(rid, old_record.data, nullptr, &modified_page_id, true,
                                    &modified_page);
            finalize_heap_undo(modified_page, modified_page_id, clr_lsn);
            restore_tuple_version(tab_name, rid);
        }
        write_set.pop_back();
    }
    // 回滚写集已排空:释放容器 capacity(旧行镜像等负载已随 pop 析构,
    // 但 vector capacity 会保留到 GC 删除事务,积压时线性放大)。
    txn->clear_write_records();
    auto finish_abort = [&] {
        txn->set_state(TransactionState::ABORTED);
        if (txn->watermark_registered()) {
            lifecycle_state_.running_txns.FinishTxn(
                txn->get_read_ts(), lifecycle_state_.last_commit_ts.load(std::memory_order_acquire));
            txn->set_watermark_registered(false);
        }
    };
    if (txn->get_isolation_level() == IsolationLevel::SERIALIZABLE) {
        transaction_registry_.WithAllExclusive([&](TransactionRegistry::AllView transactions) {
            txn->set_commit_ts(lifecycle_state_.next_timestamp++);
            finish_abort();
            transactions.ForEach([&](txn_id_t, Transaction *other) {
                if (other != nullptr && other != txn) {
                    other->remove_rw_dependency(txn->get_transaction_id());
                }
            });
            txn->clear_serializable_state();
            txn->set_ssi_metadata_released(true);
        });
    } else {
        // SI 事务不参与 SSI 依赖图。其状态为原子变量，检查点、SSI 和 GC 读者可直接观察终止状态，
        // 无须让每次热路径回滚都争用注册表全局写锁。
        txn->set_commit_ts(INVALID_TS);
        finish_abort();
        // SI 事务无 SSI 元数据,但必须标记 released:否则 GC 的 ReleaseIfSafe
        // 会对每个 aborted 事务做一次全注册表 ForEach(O(N)),abort 风暴下
        // GC 呈 O(N^2) 堆积,注册表膨胀并延迟版本链清理。
        txn->set_ssi_metadata_released(true);
    }
    // Runtime rollback needs neither a leading ABORT nor a trailing END: every
    // physical compensation is WAL-protected by its CLR, and the last CLR has
    // undoNextLSN=INVALID. Recovery may append status records after a crash,
    // while the normal abort path avoids two globally serialized WAL appends.
    release_locks(lock_manager_, txn);
    txn->wait_for_response_dependencies();
    FinishTransactionAdmission(txn);
    admission_finished = true;
    txn->set_gc_ready(true);
    QueueFinishedTransactionForGc(txn);
    if (!txn->response_gate_deferred()) {
        txn->complete_response_gate();
    }
}
