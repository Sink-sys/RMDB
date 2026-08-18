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

#include "serializable_metadata_retention.h"
#include "transaction_page_state.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <unordered_map>
#include <vector>

namespace {

constexpr size_t kTxnGcBatchLimit = 512;
constexpr size_t kTxnGcRetryReserve = 64;
constexpr size_t kRetiredTupleGcBatchLimit = 4096;

using rmdb::transaction_page_state::BumpVisibilityEpoch;
using rmdb::transaction_page_state::ClearDirtySlotIfEmpty;
using rmdb::transaction_page_state::RemoveActiveMeta;
using rmdb::transaction_page_state::RemoveDeletedMeta;
using rmdb::transaction_page_state::RemoveVersionLink;
using rmdb::atomic_counter::DecrementPositive;
using rmdb::atomic_counter::Increment;

}  // namespace

void TransactionManager::EnqueueRetiredTuples(timestamp_t commit_ts, std::vector<RetiredTuple> tuples) {
    if (tuples.empty()) {
        return;
    }
    std::lock_guard<std::mutex> gc_lock(gc_state_.queue_mutex);
    size_t inserted_count = 0;
    for (auto &tuple : tuples) {
        const TransactionLogicalDeleteKey key{tuple.table_id, tuple.rid};
        auto [iter, inserted] = gc_state_.retired_candidates.try_emplace(
            key, TransactionRetiredCandidate{commit_ts, std::move(tuple)});
        if (inserted) {
            gc_state_.retired_candidate_queue.push_back(key);
            ++inserted_count;
        } else if (commit_ts >= iter->second.commit_ts) {
            iter->second = TransactionRetiredCandidate{commit_ts, std::move(tuple)};
        }
    }
    if (inserted_count > 0) {
        Increment(gc_state_.retired_tuple_count, inserted_count, std::memory_order_release);
    }
}

bool TransactionManager::ReclaimRetiredTuple(timestamp_t commit_ts, timestamp_t watermark,
                                             const RetiredTuple &retired) {
    auto complete = [&] {
        if (retired.delete_lsn != INVALID_LSN) {
            UntrackLogicalDelete(retired.table_id, retired.rid, retired.txn_id);
        }
        return true;
    };
    auto table_info = GetTableVersionInfoById(retired.table_id);
    if (table_info == nullptr) {
        return complete();
    }
    auto page_info = GetPageVersionInfoOnTable(table_info, retired.rid.page_no);
    if (page_info == nullptr) {
        return complete();
    }

    if (retired.rid.slot_no < 0) {
        return complete();
    }
    const slot_offset_t slot = static_cast<slot_offset_t>(retired.rid.slot_no);

    UndoLink released_link{};
    auto clear_retired_state = [&](const VersionUndoLink *stored_version, bool is_deleted) {
        BumpVisibilityEpoch(page_info);
        page_info->ClearTupleMeta(slot);
        RemoveActiveMeta(page_info);
        if (is_deleted) {
            RemoveDeletedMeta(page_info);
        }

        if (stored_version != nullptr) {
            released_link = stored_version->prev_;
            page_info->ClearVersion(slot);
            RemoveVersionLink(page_info);
        }
        ClearDirtySlotIfEmpty(table_info, page_info, retired.rid);
        page_info->ReleaseTupleMetaStorageIfEmpty();
        page_info->ReleaseVersionStorageIfEmpty();
        BumpVisibilityEpoch(page_info);
    };

    bool needs_physical_delete = false;
    bool keep_candidate = false;
    {
        std::unique_lock<std::shared_mutex> page_lock(page_info->mutex_);
        const TupleMeta *stored_meta = page_info->GetTupleMeta(slot);
        if (stored_meta == nullptr) {
            return complete();
        }

        const VersionUndoLink *stored_version = page_info->GetVersion(slot);
        if (stored_meta->ts_ >= TXN_START_ID ||
            (stored_version != nullptr && stored_version->in_progress_)) {
            return false;
        }
        // A concurrently committed successor publishes its own deduplicated
        // candidate. This extracted generation can finish; an in-progress
        // successor above was retained because it may still abort.
        if (stored_meta->ts_ != commit_ts) {
            return complete();
        }

        needs_physical_delete = stored_meta->is_deleted_;
        if (stored_meta->ts_ > watermark) {
            // The latest version is still too new for the oldest active
            // snapshot, so its page root must stay.  The suffix at and behind
            // the first version visible at the watermark is nevertheless
            // unreachable by every active snapshot and can be severed now.
            if (page_info->ActiveVersionWalkers() != 0) {
                return false;
            }
            UndoLink cursor = stored_version == nullptr ? UndoLink{} : stored_version->prev_;
            while (cursor.IsValid()) {
                UndoLink next;
                UndoLink cut_link;
                Transaction::UndoGcLinkResult result = Transaction::UndoGcLinkResult::MISSING;
                transaction_registry_.WithTransactionShared(cursor.prev_txn_, [&](Transaction *txn) {
                    if (txn == nullptr) return;
                    result = txn->InspectOrCutUndoPredecessor(
                        static_cast<size_t>(cursor.prev_log_idx_), watermark, &next, &cut_link);
                });
                if (result == Transaction::UndoGcLinkResult::MISSING) {
                    throw InternalError("Undo link is missing while pruning a retired tuple");
                }
                if (result == Transaction::UndoGcLinkResult::BOUNDARY) {
                    released_link = cut_link;
                    break;
                }
                cursor = next;
            }
            keep_candidate = true;
            needs_physical_delete = false;
        } else if (!needs_physical_delete) {
            clear_retired_state(stored_version, false);
        } else if (retired.delete_lsn == INVALID_LSN) {
            return false;
        }
    }

    if (needs_physical_delete) {
        if (sm_manager_ == nullptr || table_info->table_name_.empty()) {
            return false;
        }
        auto fh_iter = sm_manager_->fhs_.find(table_info->table_name_);
        if (fh_iter == sm_manager_->fhs_.end() || fh_iter->second == nullptr) {
            return false;
        }

        bool physical_delete_failed = false;
        try {
            // Acquire the heap-page writer first, then recheck and clear MVCC
            // metadata under page_info->mutex_. This matches visibility's
            // heap-reader -> metadata-reader order and makes delete + RID reuse
            // atomic without waiting for a globally idle admission boundary.
            PageId modified_page_id{};
            Page *modified_page = nullptr;
            auto delete_result = fh_iter->second->delete_record_if(
                retired.rid,
                [&] {
                    std::unique_lock<std::shared_mutex> page_lock(page_info->mutex_);
                    const TupleMeta *stored_meta = page_info->GetTupleMeta(slot);
                    if (stored_meta == nullptr || stored_meta->ts_ != commit_ts ||
                        stored_meta->ts_ >= TXN_START_ID || !stored_meta->is_deleted_) {
                        return false;
                    }
                    const VersionUndoLink *stored_version = page_info->GetVersion(slot);
                    if (stored_version != nullptr && stored_version->in_progress_) {
                        return false;
                    }
                    clear_retired_state(stored_version, true);
                    return true;
                },
                retired.delete_lsn == INVALID_LSN ? nullptr : &modified_page_id,
                retired.delete_lsn != INVALID_LSN,
                retired.delete_lsn == INVALID_LSN ? nullptr : &modified_page);

            if (delete_result == RmFileHandle::ConditionalDeleteResult::CONDITION_FAILED) {
                return false;
            }
            if (delete_result == RmFileHandle::ConditionalDeleteResult::RECORD_MISSING) {
                // A prior retry may have completed the physical bitmap update
                // before its WAL finalization reported failure. If this is
                // still the same tombstone, finish its metadata/reference
                // cleanup; otherwise the RID has already moved on.
                std::unique_lock<std::shared_mutex> page_lock(page_info->mutex_);
                const TupleMeta *stored_meta = page_info->GetTupleMeta(slot);
                if (stored_meta != nullptr && stored_meta->ts_ == commit_ts &&
                    stored_meta->ts_ < TXN_START_ID && stored_meta->is_deleted_) {
                    const VersionUndoLink *stored_version = page_info->GetVersion(slot);
                    if (stored_version == nullptr || !stored_version->in_progress_) {
                        clear_retired_state(stored_version, true);
                    }
                }
            }
            if (delete_result == RmFileHandle::ConditionalDeleteResult::DELETED &&
                retired.delete_lsn != INVALID_LSN) {
                BufferPoolManager *bpm = sm_manager_->get_bpm();
                if (!bpm->finalize_page_write_fast(modified_page, modified_page_id,
                                                   retired.delete_lsn, true) &&
                    !bpm->finalize_page_write(modified_page_id, retired.delete_lsn, true)) {
                    throw InternalError("failed to finalize physical DELETE page");
                }
            }
        } catch (...) {
            physical_delete_failed = true;
        }
        if (physical_delete_failed) {
            if (released_link.IsValid()) {
                ReleaseUndoReference(released_link);
                released_link = {};
            }
            return false;
        }
    }

    if (released_link.IsValid()) {
        ReleaseUndoReference(released_link);
    }
    if (keep_candidate) {
        return false;
    }
    return complete();
}

std::vector<CheckpointDirtyPageInfo> TransactionManager::CollectCheckpointLogicalDeletePages() {
    struct LogicalPageKey {
        rmdb::u64 table_hash;
        page_id_t page_no;

        bool operator==(const LogicalPageKey &other) const {
            return table_hash == other.table_hash && page_no == other.page_no;
        }
    };
    struct LogicalPageKeyHash {
        size_t operator()(const LogicalPageKey &key) const noexcept {
            size_t seed = std::hash<rmdb::u64>{}(key.table_hash);
            return seed ^ (std::hash<page_id_t>{}(key.page_no) + 0x9e3779b9U +
                           (seed << 6U) + (seed >> 2U));
        }
    };

    std::vector<std::pair<TransactionLogicalDeleteKey, TransactionLogicalDelete>> deletes;
    {
        // Checkpoint needs one point-in-time view across all shards. Lock them
        // in array order; Track/Untrack take only one shard and cannot invert
        // this order.
        std::vector<std::unique_lock<std::mutex>> locks;
        locks.reserve(gc_state_.logical_delete_shards.size());
        size_t total = 0;
        for (auto &shard : gc_state_.logical_delete_shards) {
            locks.emplace_back(shard.mutex);
            total += shard.deletes.size();
        }
        deletes.reserve(total);
        for (const auto &shard : gc_state_.logical_delete_shards) {
            deletes.insert(deletes.end(), shard.deletes.begin(), shard.deletes.end());
        }
    }

    std::unordered_map<LogicalPageKey, lsn_t, LogicalPageKeyHash> oldest_by_page;
    oldest_by_page.reserve(deletes.size());
    for (const auto &[key, logical_delete] : deletes) {
        if (logical_delete.rec_lsn == INVALID_LSN) {
            continue;
        }
        auto table_info = GetTableVersionInfoById(key.table_id);
        if (table_info == nullptr || table_info->table_name_.empty()) {
            continue;
        }
        LogicalPageKey page_key{HashTableName(table_info->table_name_), key.rid.page_no};
        auto [iter, inserted] = oldest_by_page.emplace(page_key, logical_delete.rec_lsn);
        if (!inserted && logical_delete.rec_lsn < iter->second) {
            iter->second = logical_delete.rec_lsn;
        }
    }

    std::vector<CheckpointDirtyPageInfo> pages;
    pages.reserve(oldest_by_page.size());
    for (const auto &[key, rec_lsn] : oldest_by_page) {
        pages.push_back(CheckpointDirtyPageInfo{key.table_hash, key.page_no, rec_lsn});
    }
    std::sort(pages.begin(), pages.end(), [](const auto &lhs, const auto &rhs) {
        if (lhs.table_hash_ != rhs.table_hash_) return lhs.table_hash_ < rhs.table_hash_;
        return lhs.page_no_ < rhs.page_no_;
    });
    return pages;
}

void TransactionManager::UntrackLogicalDelete(const std::string &tab_name, const Rid &rid,
                                              txn_id_t txn_id) {
    auto table_info = GetTableVersionInfo(tab_name);
    if (table_info == nullptr) {
        return;
    }
    const TransactionLogicalDeleteKey key{table_info->table_id_, rid};
    auto &shard = gc_state_.logical_delete_shards[TransactionLogicalDeleteShardFor(key)];
    std::lock_guard<std::mutex> delete_lock(shard.mutex);
    auto iter = shard.deletes.find(key);
    if (iter != shard.deletes.end() && iter->second.txn_id == txn_id) {
        shard.deletes.erase(iter);
    }
}

void TransactionManager::UntrackLogicalDelete(rmdb::u32 table_id, const Rid &rid, txn_id_t txn_id) {
    const TransactionLogicalDeleteKey key{table_id, rid};
    auto &shard = gc_state_.logical_delete_shards[TransactionLogicalDeleteShardFor(key)];
    std::lock_guard<std::mutex> delete_lock(shard.mutex);
    auto iter = shard.deletes.find(key);
    if (iter != shard.deletes.end() && iter->second.txn_id == txn_id) {
        shard.deletes.erase(iter);
    }
}

TransactionManager::~TransactionManager() = default;

void TransactionManager::QueueFinishedTransactionForGc(Transaction *txn) {
    if (txn == nullptr) {
        return;
    }
    if (!txn->gc_ready() ||
        (txn->get_state() != TransactionState::COMMITTED &&
         txn->get_state() != TransactionState::ABORTED)) {
        return;
    }
    // A transaction with references is not also placed in a polling FIFO:
    // its final 1->0 transition is the exact wake-up event. Publishing every
    // finished transaction to both queues leaves stale ids behind whenever
    // ready work is continuous, making the queue itself grow without bound.
    if (txn->GetUndoReferenceCount() == 0 && txn->TryMarkUndoReadyEnqueued()) {
        PublishReadyTransaction(txn->get_transaction_id());
    }
}

void TransactionManager::GarbageCollectFinishedTransactions() {
    // First priority: exact 1->0 notifications. Drain short, independently
    // locked shards so a release performed under a page lock never waits for
    // the general GC queue mutex.
    std::deque<txn_id_t> candidates;
    size_t retry_reserve = 0;
    {
        std::lock_guard<std::mutex> gc_lock(gc_state_.queue_mutex);
        retry_reserve = std::min(gc_state_.finished_transactions.size(), kTxnGcRetryReserve);
    }
    size_t ready_remaining = kTxnGcBatchLimit - retry_reserve;
    const size_t first_shard = gc_state_.next_ready_shard;
    size_t scanned_shards = 0;
    while (ready_remaining > 0 && scanned_shards < gc_state_.ready_shards.size()) {
        const size_t shard_index = (first_shard + scanned_shards) % gc_state_.ready_shards.size();
        auto &shard = gc_state_.ready_shards[shard_index];
        size_t drained = 0;
        {
            std::lock_guard<std::mutex> ready_lock(shard.mutex);
            while (ready_remaining > 0 && !shard.transactions.empty()) {
                candidates.push_back(shard.transactions.front());
                shard.transactions.pop_front();
                --ready_remaining;
                ++drained;
            }
        }
        if (drained > 0) {
            gc_state_.ready_transaction_count.fetch_sub(drained, std::memory_order_acq_rel);
        }
        ++scanned_shards;
    }
    gc_state_.next_ready_shard = (first_shard + scanned_shards) % gc_state_.ready_shards.size();

    {
        std::lock_guard<std::mutex> gc_lock(gc_state_.queue_mutex);
        // The finished queue is only for exceptional retries (for example an
        // SSI transaction whose dependency metadata is not releasable yet).
        size_t attempts = std::min(gc_state_.finished_transactions.size(),
                                   kTxnGcBatchLimit - candidates.size());
        for (size_t i = 0; i < attempts; ++i) {
            candidates.push_back(gc_state_.finished_transactions.front());
            gc_state_.finished_transactions.pop_front();
        }
    }
    if (candidates.empty()) return;

    std::vector<Transaction *> releasable;
    std::deque<txn_id_t> retry;
    // Only active SERIALIZABLE transactions require the global exclusion used
    // to serialize SSI dependencies; snapshot-isolation transactions use the
    // sharded lock view and do not queue behind that global lock.
    const bool need_ssi =
        lifecycle_state_.active_serializable_count_.load(std::memory_order_relaxed) > 0;
    auto process_candidates = [&](TransactionRegistry::AllView transactions) {
        std::optional<SerializableMetadataRetention> ssi_retention;
        if (need_ssi) {
            ssi_retention.emplace(transactions);
        }

        while (!candidates.empty()) {
            txn_id_t txn_id = candidates.front();
            candidates.pop_front();

            auto *txn = transactions.Find(txn_id);
            if (txn == nullptr) continue;
            if (!txn->gc_ready() ||
                (txn->get_state() != TransactionState::COMMITTED &&
                 txn->get_state() != TransactionState::ABORTED)) {
                txn->ClearUndoReadyEnqueued();
                continue;
            }

            if (txn->GetUndoReferenceCount() == 0 && txn->GetUndoLogNum() > 0) {
                auto undo_logs = txn->TakeUndoLogs();
                for (const auto &undo_log : undo_logs) {
                    const UndoLink &predecessor = undo_log.prev_version_;
                    if (!predecessor.IsValid() || predecessor.prev_txn_ == txn_id) {
                        continue;
                    }
                    auto *predecessor_txn = transactions.Find(predecessor.prev_txn_);
                    if (predecessor_txn == nullptr) {
                        throw InternalError("Undo predecessor transaction is missing during GC");
                    }
                    const bool became_zero = predecessor_txn->ReleaseUndoReference();
                    // Drain a newly exposed predecessor in this same pass.  A hot
                    // row may carry thousands of committed versions; deferring one
                    // predecessor per 2 ms maintenance tick makes the chain grow
                    // faster than GC can peel it even when every page root is gone.
                    if (became_zero && predecessor_txn->gc_ready() &&
                        predecessor_txn->TryMarkUndoReadyEnqueued()) {
                        candidates.push_back(predecessor.prev_txn_);
                    }
                }
            }

            if (ssi_retention.has_value()) {
                ssi_retention->ReleaseIfSafe(txn_id, txn, transactions);
            }

            bool undo_released = txn->GetUndoReferenceCount() == 0 && txn->GetUndoLogNum() == 0;
            if (undo_released && txn->ssi_metadata_released()) {
                if (txn->get_isolation_level() == IsolationLevel::SERIALIZABLE) {
                    lifecycle_state_.active_serializable_count_.fetch_sub(1, std::memory_order_relaxed);
                }
                releasable.push_back(txn);
                transactions.Erase(txn_id);
            } else {
                txn->ClearUndoReadyEnqueued();
                retry.push_back(txn_id);
            }
        }
    };
    if (need_ssi) {
        transaction_registry_.WithAllExclusive(process_candidates);
    } else {
        auto view = transaction_registry_.ShardsView();
        process_candidates(view);
    }

    if (!retry.empty()) {
        std::lock_guard<std::mutex> gc_lock(gc_state_.queue_mutex);
        while (!retry.empty()) {
            gc_state_.finished_transactions.push_back(retry.front());
            retry.pop_front();
        }
    }

    for (auto *txn : releasable) {
        delete txn;
    }
}

void TransactionManager::GarbageCollection() {
    std::unique_lock<std::mutex> run_lock(gc_state_.run_mutex, std::try_to_lock);
    if (!run_lock.owns_lock()) {
        return;
    }

    // Empty-queue fast path avoids watermark/index/registry work.
    {
        std::lock_guard<std::mutex> gc_lock(gc_state_.queue_mutex);
        if (gc_state_.retired_candidate_queue.empty() &&
            gc_state_.finished_transactions.empty() &&
            gc_state_.ready_transaction_count.load(std::memory_order_acquire) == 0) {
            return;
        }
    }

    const timestamp_t watermark = GetWatermark();
    snapshot_index_history_.Purge(watermark);
    std::vector<TransactionRetiredCandidate> ready;
    {
        std::lock_guard<std::mutex> gc_lock(gc_state_.queue_mutex);
        const size_t attempts = std::min(gc_state_.retired_candidate_queue.size(),
                                         kRetiredTupleGcBatchLimit);
        ready.reserve(attempts);
        for (size_t i = 0; i < attempts; ++i) {
            TransactionLogicalDeleteKey key = gc_state_.retired_candidate_queue.front();
            gc_state_.retired_candidate_queue.pop_front();
            auto iter = gc_state_.retired_candidates.find(key);
            if (iter == gc_state_.retired_candidates.end()) {
                continue;
            }
            ready.push_back(std::move(iter->second));
            gc_state_.retired_candidates.erase(iter);
            DecrementPositive(gc_state_.retired_tuple_count, "Retired tuple count", size_t{1},
                              std::memory_order_release, std::memory_order_relaxed);
        }
    }

    std::vector<TransactionRetiredCandidate> retry;
    for (auto &candidate : ready) {
        if (!ReclaimRetiredTuple(candidate.commit_ts, watermark, candidate.tuple)) {
            retry.push_back(std::move(candidate));
        }
    }
    if (!retry.empty()) {
        std::lock_guard<std::mutex> gc_lock(gc_state_.queue_mutex);
        size_t inserted_count = 0;
        for (auto &candidate : retry) {
            const TransactionLogicalDeleteKey key{
                candidate.tuple.table_id, candidate.tuple.rid};
            auto [iter, inserted] = gc_state_.retired_candidates.try_emplace(
                key, std::move(candidate));
            if (inserted) {
                gc_state_.retired_candidate_queue.push_back(key);
                ++inserted_count;
            } else if (candidate.commit_ts >= iter->second.commit_ts) {
                iter->second = std::move(candidate);
            }
        }
        if (inserted_count > 0) {
            Increment(gc_state_.retired_tuple_count, inserted_count, std::memory_order_release);
        }
    }

    GarbageCollectFinishedTransactions();
}

void TransactionManager::PhysicalizeCommittedDeletes() {
    if (sm_manager_ == nullptr) {
        return;
    }

    std::vector<std::pair<std::string, std::shared_ptr<TableVersionInfo>>> tables;
    {
        std::shared_lock<std::shared_mutex> version_lock(version_state_.mutex);
        tables.reserve(version_state_.tables.size());
        for (const auto &table_entry : version_state_.tables) {
            if (table_entry.second != nullptr) {
                tables.push_back(table_entry);
            }
        }
    }

    std::vector<std::pair<std::string, Rid>> deletes;
    for (const auto &table_entry : tables) {
        const auto &tab_name = table_entry.first;
        const auto &table_info = table_entry.second;
        std::shared_lock<std::shared_mutex> table_lock(table_info->mutex_);
        for (const auto &page_entry : table_info->pages_) {
            auto page_info = page_entry.second;
            if (page_info == nullptr) {
                continue;
            }
            std::shared_lock<std::shared_mutex> page_lock(page_info->mutex_);
            // 无已提交删除标记的页面无需逐个槽位扫描（消除 reset 停顿中的全表横扫）
            if (page_info->deleted_count_.load(std::memory_order_acquire) == 0) {
                continue;
            }
            for (slot_offset_t slot = 0; slot < page_info->slot_count_; ++slot) {
                const TupleMeta *meta = page_info->GetTupleMeta(slot);
                if (meta != nullptr && meta->is_deleted_ && meta->ts_ < TXN_START_ID) {
                    deletes.push_back({tab_name, Rid{page_entry.first, static_cast<int>(slot)}});
                }
            }
        }
    }

    for (const auto &entry : deletes) {
        const std::string &tab_name = entry.first;
        Rid rid = entry.second;
        auto fh_iter = sm_manager_->fhs_.find(tab_name);
        if (fh_iter == sm_manager_->fhs_.end() || fh_iter->second == nullptr) {
            throw InternalError("Cannot physicalize DELETE for unopened table " + tab_name);
        }
        if (fh_iter->second->is_record(rid)) {
            fh_iter->second->delete_record(rid, nullptr);
        }
        UpdateTupleMeta(tab_name, rid, std::nullopt);
        UpdateVersionLink(tab_name, rid, std::nullopt);
    }
}
