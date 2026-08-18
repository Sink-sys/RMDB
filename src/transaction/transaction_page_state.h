#pragma once

#include "common/checked_atomic_counter.h"
#include "common/types.h"
#include "transaction_version_storage.h"

#include <atomic>
#include <memory>

namespace rmdb::transaction_page_state {

using rmdb::atomic_counter::DecrementPositive;
using rmdb::atomic_counter::Increment;

inline void AddActiveMeta(const std::shared_ptr<TransactionPageVersionInfo> &page_info) {
    Increment(page_info->active_meta_count_);
}

inline void RemoveActiveMeta(const std::shared_ptr<TransactionPageVersionInfo> &page_info) {
    DecrementPositive(page_info->active_meta_count_, "Active tuple metadata count");
}

inline void AddUncommittedMeta(const std::shared_ptr<TransactionPageVersionInfo> &page_info) {
    Increment(page_info->uncommitted_meta_count_);
}

inline void RemoveUncommittedMeta(const std::shared_ptr<TransactionPageVersionInfo> &page_info) {
    DecrementPositive(page_info->uncommitted_meta_count_, "Uncommitted tuple metadata count");
}

inline void AddDeletedMeta(const std::shared_ptr<TransactionPageVersionInfo> &page_info) {
    Increment(page_info->deleted_count_);
}

inline void RemoveDeletedMeta(const std::shared_ptr<TransactionPageVersionInfo> &page_info) {
    DecrementPositive(page_info->deleted_count_, "Deleted tuple metadata count");
}

inline void AddVersionLink(const std::shared_ptr<TransactionPageVersionInfo> &page_info) {
    Increment(page_info->version_link_count_);
}

inline void RemoveVersionLink(const std::shared_ptr<TransactionPageVersionInfo> &page_info) {
    DecrementPositive(page_info->version_link_count_, "Version link count");
}

inline void AdvanceMaxCommittedMetaTs(const std::shared_ptr<TransactionPageVersionInfo> &page_info,
                                      timestamp_t timestamp) {
    timestamp_t observed = page_info->max_committed_meta_ts_.load(std::memory_order_relaxed);
    while (timestamp > observed &&
           !page_info->max_committed_meta_ts_.compare_exchange_weak(
               observed, timestamp, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
}

inline void BumpVisibilityEpoch(const std::shared_ptr<TransactionPageVersionInfo> &page_info) {
    if (page_info != nullptr) {
        page_info->visibility_epoch_.fetch_add(1, std::memory_order_acq_rel);
    }
}

inline void MarkDirtySlot(const std::shared_ptr<TransactionTableVersionInfo> &table_info,
                          const std::shared_ptr<TransactionPageVersionInfo> &page_info, const Rid &rid) {
    if (page_info == nullptr || rid.slot_no < 0) {
        return;
    }
    slot_offset_t slot = static_cast<slot_offset_t>(rid.slot_no);
    if (!page_info->CanTrackDirtySlot(slot)) {
        if (table_info != nullptr) {
            table_info->mark_page_dirty_exact(rid.page_no);
        }
        return;
    }
    bool newly_dirty = page_info->MarkDirtySlot(slot);
    if (newly_dirty && table_info != nullptr) {
        Increment(table_info->dirty_slot_total_, rmdb::u64{1}, std::memory_order_acq_rel);
    }
    if (newly_dirty && page_info->DirtySlotCount() == 1 && table_info != nullptr) {
        table_info->mark_page_dirty_exact(rid.page_no);
    }
}

inline void ClearDirtySlotIfEmpty(const std::shared_ptr<TransactionTableVersionInfo> &table_info,
                                  const std::shared_ptr<TransactionPageVersionInfo> &page_info, const Rid &rid) {
    if (page_info == nullptr || rid.slot_no < 0) {
        return;
    }
    slot_offset_t slot = static_cast<slot_offset_t>(rid.slot_no);
    if (!page_info->CanTrackDirtySlot(slot)) {
        return;
    }
    if (!page_info->HasTupleMeta(slot) && !page_info->HasVersion(slot)) {
        bool cleared = page_info->ClearDirtySlot(slot);
        if (cleared && table_info != nullptr) {
            DecrementPositive(table_info->dirty_slot_total_, "Dirty slot count", rmdb::u64{1},
                              std::memory_order_acq_rel, std::memory_order_acquire);
        }
        if (cleared && page_info->DirtySlotCount() == 0 && table_info != nullptr) {
            table_info->clear_page_dirty_exact(rid.page_no);
        }
    }
}

}  // namespace rmdb::transaction_page_state
