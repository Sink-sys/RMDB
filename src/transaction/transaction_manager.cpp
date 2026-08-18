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
#include "record/rm_file_handle.h"
#include "system/sm_manager.h"
#include "transaction_page_state.h"

#include <algorithm>
#include <array>
#include <functional>
#include <utility>

namespace {

constexpr size_t kTableVersionCacheSize = 8;

using rmdb::transaction_page_state::BumpVisibilityEpoch;
using rmdb::transaction_page_state::ClearDirtySlotIfEmpty;
using rmdb::transaction_page_state::MarkDirtySlot;
using rmdb::transaction_page_state::AddActiveMeta;
using rmdb::transaction_page_state::AddDeletedMeta;
using rmdb::transaction_page_state::AddUncommittedMeta;
using rmdb::transaction_page_state::AddVersionLink;
using rmdb::transaction_page_state::AdvanceMaxCommittedMetaTs;
using rmdb::transaction_page_state::RemoveActiveMeta;
using rmdb::transaction_page_state::RemoveDeletedMeta;
using rmdb::transaction_page_state::RemoveUncommittedMeta;
using rmdb::transaction_page_state::RemoveVersionLink;

struct TableVersionCacheEntry {
    TransactionManager *manager{nullptr};
    rmdb::u64 manager_id{0};
    rmdb::u64 epoch{0};
    std::string table_name;
    std::shared_ptr<TransactionManager::TableVersionInfo> table_info;
};

thread_local std::array<TableVersionCacheEntry, kTableVersionCacheSize> table_version_cache;

TableVersionCacheEntry &table_version_cache_entry(const std::string &table_name) {
    return table_version_cache[std::hash<std::string>{}(table_name) & (kTableVersionCacheSize - 1)];
}

// 单条目线程本地缓存:GC 线程按表批量处理退役元组,同表连续命中。
struct TableIdCacheEntry {
    TransactionManager *manager{nullptr};
    rmdb::u64 manager_id{0};
    rmdb::u64 epoch{0};
    rmdb::u32 table_id{0};
    std::shared_ptr<TransactionManager::TableVersionInfo> table_info;
};
thread_local TableIdCacheEntry table_id_cache;

void init_page_dirty_slots(SmManager *sm_manager, const std::string &tab_name,
                           const std::shared_ptr<TransactionManager::PageVersionInfo> &page_info) {
    if (sm_manager == nullptr || page_info == nullptr) {
        return;
    }
    auto fh_iter = sm_manager->fhs_.find(tab_name);
    if (fh_iter == sm_manager->fhs_.end() || fh_iter->second == nullptr) {
        return;
    }
    int slot_count = fh_iter->second->get_file_hdr().num_records_per_page;
    if (slot_count <= 0) {
        return;
    }
    page_info->InitDirtySlots(static_cast<rmdb::u32>(slot_count));
}
}

bool TransactionManager::UpdateTupleMeta(const std::string &tab_name, Rid rid, std::optional<TupleMeta> meta,
                                         std::function<bool(std::optional<TupleMeta>)> &&check) {
    auto table_info = GetOrCreateTableVersionInfo(tab_name);
    std::shared_ptr<PageVersionInfo> page_info = GetOrCreatePageVersionInfoOnTable(table_info, tab_name, rid.page_no);

    std::unique_lock<std::shared_mutex> page_lock(page_info->mutex_);
    slot_offset_t slot = static_cast<slot_offset_t>(rid.slot_no);
    if (rid.slot_no < 0 || !page_info->CanTrackSlot(slot)) {
        throw InternalError("Tuple metadata slot is out of range");
    }
    std::optional<TupleMeta> current_meta = std::nullopt;
    if (const TupleMeta *stored_meta = page_info->GetTupleMeta(slot); stored_meta != nullptr) {
        current_meta = *stored_meta;
    }
    if (check != nullptr && !check(current_meta)) {
        return false;
    }
    const bool was_deleted = current_meta.has_value() && current_meta->is_deleted_;
    const bool will_be_deleted = meta.has_value() && meta->is_deleted_;
    const bool was_uncommitted = current_meta.has_value() && current_meta->ts_ >= TXN_START_ID;
    const bool will_be_uncommitted = meta.has_value() && meta->ts_ >= TXN_START_ID;
    bool changed = current_meta.has_value() != meta.has_value();
    if (!changed && current_meta.has_value() && meta.has_value()) {
        changed = current_meta->ts_ != meta->ts_ || current_meta->is_deleted_ != meta->is_deleted_;
    }
    if (!current_meta.has_value() && meta.has_value()) {
        AddActiveMeta(page_info);
    } else if (current_meta.has_value() && !meta.has_value()) {
        RemoveActiveMeta(page_info);
    }
    if (!was_uncommitted && will_be_uncommitted) {
        AddUncommittedMeta(page_info);
    } else if (was_uncommitted && !will_be_uncommitted) {
        RemoveUncommittedMeta(page_info);
    }
    if (meta.has_value() && meta->ts_ < TXN_START_ID) {
        AdvanceMaxCommittedMetaTs(page_info, meta->ts_);
    }
    if (!was_deleted && will_be_deleted) {
        AddDeletedMeta(page_info);
    }
    if (meta.has_value()) {
        MarkDirtySlot(table_info, page_info, rid);
        page_info->SetTupleMeta(slot, *meta);
    } else {
        page_info->ClearTupleMeta(slot);
        ClearDirtySlotIfEmpty(table_info, page_info, rid);
        page_info->ReleaseTupleMetaStorageIfEmpty();
    }
    if (was_deleted && !will_be_deleted) {
        RemoveDeletedMeta(page_info);
    }
    if (changed) {
        BumpVisibilityEpoch(page_info);
    }
    return true;
}

std::optional<TupleMeta> TransactionManager::GetTupleMeta(const std::string &tab_name, Rid rid) {
    auto table_info = GetTableVersionInfo(tab_name);
    auto page_info = GetPageVersionInfoOnTable(table_info, rid.page_no);
    if (page_info == nullptr) {
        return std::nullopt;
    }

    std::shared_lock<std::shared_mutex> page_lock(page_info->mutex_);
    if (rid.slot_no < 0) {
        return std::nullopt;
    }
    const TupleMeta *meta = page_info->GetTupleMeta(static_cast<slot_offset_t>(rid.slot_no));
    return meta == nullptr ? std::nullopt : std::optional<TupleMeta>(*meta);
}

TupleMeta TransactionManager::GetTupleMetaOrDefault(const std::string &tab_name, Rid rid) {
    auto meta = GetTupleMeta(tab_name, rid);
    if (meta.has_value()) {
        return *meta;
    }
    return TupleMeta{0, false};
}

std::shared_ptr<TransactionManager::PageVersionInfo> TransactionManager::GetPageVersionInfo(
    const std::string &tab_name, page_id_t page_no) {
    return GetPageVersionInfoOnTable(GetTableVersionInfo(tab_name), page_no);
}

std::shared_ptr<TransactionManager::TableVersionInfo> TransactionManager::GetTableVersionInfo(
    const std::string &tab_name) {
    rmdb::u64 epoch = version_state_.epoch.load(std::memory_order_acquire);
    auto &cached = table_version_cache_entry(tab_name);
    if (cached.manager == this && cached.manager_id == version_state_.cache_id && cached.epoch == epoch &&
        cached.table_name == tab_name) {
        return cached.table_info;
    }

    std::shared_lock<std::shared_mutex> version_lock(version_state_.mutex);
    auto table_iter = version_state_.tables.find(tab_name);
    std::shared_ptr<TableVersionInfo> table_info =
        table_iter == version_state_.tables.end() ? nullptr : table_iter->second;
    cached = TableVersionCacheEntry{this, version_state_.cache_id, epoch, tab_name, table_info};
    return table_info;
}

std::shared_ptr<TransactionManager::TableVersionInfo> TransactionManager::GetOrCreateTableVersionInfo(
    const std::string &tab_name) {
    rmdb::u64 epoch = version_state_.epoch.load(std::memory_order_acquire);
    auto &cached = table_version_cache_entry(tab_name);
    if (cached.manager == this && cached.manager_id == version_state_.cache_id && cached.epoch == epoch &&
        cached.table_name == tab_name && cached.table_info != nullptr) {
        return cached.table_info;
    }
    {
        std::shared_lock<std::shared_mutex> version_lock(version_state_.mutex);
        auto table_iter = version_state_.tables.find(tab_name);
        if (table_iter != version_state_.tables.end() && table_iter->second != nullptr) {
            cached = TableVersionCacheEntry{
                this, version_state_.cache_id, epoch, tab_name, table_iter->second};
            return table_iter->second;
        }
    }
    std::unique_lock<std::shared_mutex> version_lock(version_state_.mutex);
    auto &table_info = version_state_.tables[tab_name];
    if (table_info == nullptr) {
        table_info = std::make_shared<TableVersionInfo>();
        table_info->table_id_ = version_state_.next_table_id.fetch_add(1, std::memory_order_relaxed);
        table_info->table_name_ = tab_name;
        version_state_.tables_by_id.emplace(table_info->table_id_, table_info);
        epoch = version_state_.epoch.fetch_add(1, std::memory_order_acq_rel) + 1;
    }
    cached = TableVersionCacheEntry{this, version_state_.cache_id, epoch, tab_name, table_info};
    return table_info;
}

std::shared_ptr<TransactionManager::TableVersionInfo> TransactionManager::GetTableVersionInfoById(rmdb::u32 table_id) {
    if (table_id == 0) {
        return nullptr;
    }
    auto &cached = table_id_cache;
    if (cached.manager == this && cached.manager_id == version_state_.cache_id &&
        cached.epoch == version_state_.epoch.load(std::memory_order_acquire) &&
        cached.table_id == table_id) {
        return cached.table_info;
    }
    std::shared_lock<std::shared_mutex> version_lock(version_state_.mutex);
    auto iter = version_state_.tables_by_id.find(table_id);
    std::shared_ptr<TableVersionInfo> table_info =
        iter == version_state_.tables_by_id.end() ? nullptr : iter->second;
    cached = TableIdCacheEntry{this, version_state_.cache_id,
                               version_state_.epoch.load(std::memory_order_relaxed), table_id, table_info};
    return table_info;
}

std::shared_ptr<TransactionManager::PageVersionInfo> TransactionManager::GetPageVersionInfoOnTable(
    const std::shared_ptr<TableVersionInfo> &table_info, page_id_t page_no) {
    return GetPageVersionInfoOnTable(table_info, page_no, nullptr);
}

std::shared_ptr<TransactionManager::PageVersionInfo> TransactionManager::GetPageVersionInfoOnTable(
    const std::shared_ptr<TableVersionInfo> &table_info, page_id_t page_no, rmdb::u64 *page_map_epoch) {
    if (table_info == nullptr) {
        if (page_map_epoch != nullptr) {
            *page_map_epoch = 0;
        }
        return nullptr;
    }
    if (table_info->tracks_exact_dirty_page(page_no) && !table_info->is_page_dirty_exact(page_no)) {
        if (page_map_epoch != nullptr) {
            *page_map_epoch = table_info->page_map_epoch_.load(std::memory_order_acquire);
        }
        return nullptr;
    }
    if (table_info->tracks_exact_dirty_page(page_no)) {
        auto *dense_page_info = table_info->lookup_page_info_dense(page_no);
        if (dense_page_info != nullptr) {
            if (page_map_epoch != nullptr) {
                *page_map_epoch = table_info->page_map_epoch_.load(std::memory_order_acquire);
            }
            return std::shared_ptr<PageVersionInfo>(table_info, dense_page_info);
        }
    }
    if (!table_info->maybe_has_page_version_info(page_no)) {
        if (page_map_epoch != nullptr) {
            *page_map_epoch = table_info->page_map_epoch_.load(std::memory_order_acquire);
        }
        return nullptr;
    }
    std::shared_lock<std::shared_mutex> table_lock(table_info->mutex_);
    if (page_map_epoch != nullptr) {
        *page_map_epoch = table_info->page_map_epoch_.load(std::memory_order_acquire);
    }
    auto page_iter = table_info->pages_.find(page_no);
    if (page_iter == table_info->pages_.end()) {
        return nullptr;
    }
    return page_iter->second;
}

TransactionManager::PageVersionInfo *TransactionManager::GetPageVersionInfoOnTableRaw(
    const std::shared_ptr<TableVersionInfo> &table_info, page_id_t page_no, rmdb::u64 *page_map_epoch) {
    if (table_info == nullptr) {
        if (page_map_epoch != nullptr) {
            *page_map_epoch = 0;
        }
        return nullptr;
    }
    if (table_info->tracks_exact_dirty_page(page_no) && !table_info->is_page_dirty_exact(page_no)) {
        if (page_map_epoch != nullptr) {
            *page_map_epoch = table_info->page_map_epoch_.load(std::memory_order_acquire);
        }
        return nullptr;
    }
    if (table_info->tracks_exact_dirty_page(page_no)) {
        auto *dense_page_info = table_info->lookup_page_info_dense(page_no);
        if (dense_page_info != nullptr) {
            if (page_map_epoch != nullptr) {
                *page_map_epoch = table_info->page_map_epoch_.load(std::memory_order_acquire);
            }
            return dense_page_info;
        }
    }
    if (!table_info->maybe_has_page_version_info(page_no)) {
        if (page_map_epoch != nullptr) {
            *page_map_epoch = table_info->page_map_epoch_.load(std::memory_order_acquire);
        }
        return nullptr;
    }
    std::shared_lock<std::shared_mutex> table_lock(table_info->mutex_);
    if (page_map_epoch != nullptr) {
        *page_map_epoch = table_info->page_map_epoch_.load(std::memory_order_acquire);
    }
    auto page_iter = table_info->pages_.find(page_no);
    if (page_iter == table_info->pages_.end()) {
        return nullptr;
    }
    return page_iter->second.get();
}

std::shared_ptr<TransactionManager::PageVersionInfo> TransactionManager::GetOrCreatePageVersionInfo(
    const std::string &tab_name, page_id_t page_no) {
    auto table_info = GetOrCreateTableVersionInfo(tab_name);
    return GetOrCreatePageVersionInfoOnTable(table_info, tab_name, page_no);
}

std::shared_ptr<TransactionManager::PageVersionInfo> TransactionManager::GetOrCreatePageVersionInfoOnTable(
    const std::shared_ptr<TableVersionInfo> &table_info, const std::string &tab_name, page_id_t page_no) {
    table_info->mark_page_version_info(page_no);
    if (table_info->tracks_exact_dirty_page(page_no)) {
        if (auto *dense_page_info = table_info->lookup_page_info_dense(page_no); dense_page_info != nullptr) {
            return std::shared_ptr<PageVersionInfo>(table_info, dense_page_info);
        }
    }
    {
        std::shared_lock<std::shared_mutex> table_lock(table_info->mutex_);
        auto page_iter = table_info->pages_.find(page_no);
        if (page_iter != table_info->pages_.end() && page_iter->second != nullptr) {
            return page_iter->second;
        }
    }

    std::unique_lock<std::shared_mutex> table_lock(table_info->mutex_);
    auto &page_info = table_info->pages_[page_no];
    if (page_info == nullptr) {
        auto new_page_info = std::make_shared<PageVersionInfo>();
        init_page_dirty_slots(sm_manager_, tab_name, new_page_info);
        page_info = std::move(new_page_info);
        table_info->publish_page_info_dense(page_no, page_info.get());
        table_info->page_map_epoch_.fetch_add(1, std::memory_order_acq_rel);
    }
    return page_info;
}

void TransactionManager::RetainUndoReference(const UndoLink &link) {
    if (!link.IsValid()) {
        return;
    }
    transaction_registry_.WithTransactionShared(link.prev_txn_, [&](Transaction *txn) {
        if (txn == nullptr) {
            throw InternalError("Cannot retain missing undo transaction");
        }
        txn->RetainUndoReference();
    });
}

void TransactionManager::ReleaseUndoReference(const UndoLink &link) {
    if (!link.IsValid()) {
        return;
    }
    txn_id_t ready_txn_id = INVALID_TXN_ID;
    transaction_registry_.WithTransactionShared(link.prev_txn_, [&](Transaction *txn) {
        if (txn == nullptr) {
            throw InternalError("Cannot release missing undo transaction");
        }
        if (txn->ReleaseUndoReference() && txn->gc_ready() &&
            txn->TryMarkUndoReadyEnqueued()) {
            ready_txn_id = txn->get_transaction_id();
        }
    });
    // Publish after dropping the registry shard lock. The ready queue is
    // sharded and GC drains each shard before taking any registry/page lock.
    if (ready_txn_id != INVALID_TXN_ID) {
        PublishReadyTransaction(ready_txn_id);
    }
}

void TransactionManager::RetainUndoReference(const UndoLink &link, Transaction *self) {
    if (!link.IsValid()) {
        return;
    }
    // 自链:调用线程拥有该事务,不可能被 GC 回收,直接原子计数。
    if (self != nullptr && link.prev_txn_ == self->get_transaction_id()) {
        self->RetainUndoReference();
        return;
    }
    RetainUndoReference(link);
}

void TransactionManager::ReleaseUndoReference(const UndoLink &link, Transaction *self) {
    if (!link.IsValid()) {
        return;
    }
    if (self != nullptr && link.prev_txn_ == self->get_transaction_id()) {
        if (self->ReleaseUndoReference() && self->gc_ready() &&
            self->TryMarkUndoReadyEnqueued()) {
            PublishReadyTransaction(self->get_transaction_id());
        }
        return;
    }
    ReleaseUndoReference(link);
}

bool TransactionManager::UpdateUndoLink(const std::string &tab_name, Rid rid, std::optional<UndoLink> prev_link,
                                        std::function<bool(std::optional<UndoLink>)> &&check, Transaction *self) {
    auto table_info = GetOrCreateTableVersionInfo(tab_name);
    std::shared_ptr<PageVersionInfo> page_info = GetOrCreatePageVersionInfoOnTable(table_info, tab_name, rid.page_no);

    std::unique_lock<std::shared_mutex> page_lock(page_info->mutex_);
    slot_offset_t slot = static_cast<slot_offset_t>(rid.slot_no);
    if (rid.slot_no < 0 || !page_info->CanTrackSlot(slot)) {
        throw InternalError("Version metadata slot is out of range");
    }
    VersionUndoLink *stored_version = page_info->GetMutableVersion(slot);
    std::optional<UndoLink> current_link = std::nullopt;
    if (stored_version != nullptr && stored_version->prev_.IsValid()) {
        current_link = stored_version->prev_;
    }
    if (check != nullptr && !check(current_link)) {
        return false;
    }

    const UndoLink old_link = stored_version == nullptr ? UndoLink{} : stored_version->prev_;
    const UndoLink new_link = prev_link.has_value() ? *prev_link : UndoLink{};
    const bool link_changed = old_link != new_link;
    if (link_changed && new_link.IsValid()) {
        RetainUndoReference(new_link, self);
    }

    bool changed = false;
    bool mutation_succeeded = false;
    try {
        if (prev_link.has_value()) {
            changed = stored_version == nullptr || stored_version->prev_ != *prev_link;
            MarkDirtySlot(table_info, page_info, rid);
            if (stored_version == nullptr) {
                page_info->SetVersion(slot, VersionUndoLink{*prev_link, false});
                AddVersionLink(page_info);
            } else {
                stored_version->prev_ = *prev_link;
            }
            if (changed) {
                BumpVisibilityEpoch(page_info);
            }
            mutation_succeeded = true;
            if (link_changed && old_link.IsValid()) {
                ReleaseUndoReference(old_link, self);
            }
            return true;
        }

        if (stored_version == nullptr) {
            mutation_succeeded = true;
            return true;
        }
        if (stored_version->in_progress_) {
            changed = stored_version->prev_.IsValid();
            stored_version->prev_ = UndoLink{};
        } else {
            changed = true;
            page_info->ClearVersion(slot);
            RemoveVersionLink(page_info);
            ClearDirtySlotIfEmpty(table_info, page_info, rid);
            page_info->ReleaseVersionStorageIfEmpty();
        }
        if (changed) {
            BumpVisibilityEpoch(page_info);
        }
        mutation_succeeded = true;
        if (link_changed && old_link.IsValid()) {
            ReleaseUndoReference(old_link, self);
        }
        return true;
    } catch (...) {
        if (!mutation_succeeded && link_changed && new_link.IsValid()) {
            ReleaseUndoReference(new_link, self);
        }
        throw;
    }
}

bool TransactionManager::UpdateVersionLink(const std::string &tab_name, Rid rid,
                                           std::optional<VersionUndoLink> prev_version,
                                           std::function<bool(std::optional<VersionUndoLink>)> &&check,
                                           Transaction *self) {
    auto table_info = GetOrCreateTableVersionInfo(tab_name);
    std::shared_ptr<PageVersionInfo> page_info = GetOrCreatePageVersionInfoOnTable(table_info, tab_name, rid.page_no);

    std::unique_lock<std::shared_mutex> page_lock(page_info->mutex_);
    slot_offset_t slot = static_cast<slot_offset_t>(rid.slot_no);
    if (rid.slot_no < 0 || !page_info->CanTrackSlot(slot)) {
        throw InternalError("Version metadata slot is out of range");
    }
    std::optional<VersionUndoLink> current_version = std::nullopt;
    VersionUndoLink *stored_version = page_info->GetMutableVersion(slot);
    if (stored_version != nullptr) {
        current_version = *stored_version;
    }
    if (check != nullptr && !check(current_version)) {
        return false;
    }

    const UndoLink old_link = stored_version == nullptr ? UndoLink{} : stored_version->prev_;
    const UndoLink new_link = prev_version.has_value() ? prev_version->prev_ : UndoLink{};
    const bool link_changed = old_link != new_link;
    if (link_changed && new_link.IsValid()) {
        RetainUndoReference(new_link, self);
    }

    bool changed = false;
    bool mutation_succeeded = false;
    try {
        if (prev_version.has_value()) {
            changed = stored_version == nullptr || *stored_version != *prev_version;
            MarkDirtySlot(table_info, page_info, rid);
            if (stored_version == nullptr) {
                AddVersionLink(page_info);
            }
            page_info->SetVersion(slot, *prev_version);
        } else {
            if (stored_version != nullptr) {
                changed = true;
                page_info->ClearVersion(slot);
                RemoveVersionLink(page_info);
                ClearDirtySlotIfEmpty(table_info, page_info, rid);
                page_info->ReleaseVersionStorageIfEmpty();
            }
        }
        if (changed) {
            BumpVisibilityEpoch(page_info);
        }
        mutation_succeeded = true;
        if (link_changed && old_link.IsValid()) {
            ReleaseUndoReference(old_link, self);
        }
        return true;
    } catch (...) {
        if (!mutation_succeeded && link_changed && new_link.IsValid()) {
            ReleaseUndoReference(new_link, self);
        }
        throw;
    }
}

void TransactionManager::InstallTupleVersion(const std::string &tab_name, Rid rid,
                                              const VersionUndoLink &version, const TupleMeta &meta,
                                              Transaction *self) {
    auto table_info = GetOrCreateTableVersionInfo(tab_name);
    auto page_info = GetOrCreatePageVersionInfoOnTable(table_info, tab_name, rid.page_no);
    std::unique_lock<std::shared_mutex> page_lock(page_info->mutex_);
    slot_offset_t slot = static_cast<slot_offset_t>(rid.slot_no);
    if (rid.slot_no < 0 || !page_info->CanTrackSlot(slot)) {
        throw InternalError("Version metadata slot is out of range");
    }

    // Allocate both sparse stores before retaining or publishing anything so
    // allocation failure cannot leave a half-installed tuple version.
    page_info->EnsureVersionStorage();
    page_info->EnsureTupleMetaStorage();
    VersionUndoLink *stored_version = page_info->GetMutableVersion(slot);
    const TupleMeta *stored_meta = page_info->GetTupleMeta(slot);
    UndoLink old_link = stored_version == nullptr ? UndoLink{} : stored_version->prev_;
    bool link_changed = old_link != version.prev_;
    if (link_changed && version.prev_.IsValid()) {
        RetainUndoReference(version.prev_, self);
    }

    MarkDirtySlot(table_info, page_info, rid);
    if (stored_version == nullptr) {
        AddVersionLink(page_info);
    }
    page_info->SetVersion(slot, version);

    bool had_meta = stored_meta != nullptr;
    bool was_uncommitted = had_meta && stored_meta->ts_ >= TXN_START_ID;
    bool was_deleted = had_meta && stored_meta->is_deleted_;
    bool will_be_uncommitted = meta.ts_ >= TXN_START_ID;
    if (!had_meta) {
        AddActiveMeta(page_info);
    }
    if (!was_uncommitted && will_be_uncommitted) {
        AddUncommittedMeta(page_info);
    } else if (was_uncommitted && !will_be_uncommitted) {
        RemoveUncommittedMeta(page_info);
    }
    if (!was_deleted && meta.is_deleted_) {
        AddDeletedMeta(page_info);
    } else if (was_deleted && !meta.is_deleted_) {
        RemoveDeletedMeta(page_info);
    }
    if (meta.ts_ < TXN_START_ID) {
        AdvanceMaxCommittedMetaTs(page_info, meta.ts_);
    }
    page_info->SetTupleMeta(slot, meta);
    BumpVisibilityEpoch(page_info);
    if (link_changed && old_link.IsValid()) {
        ReleaseUndoReference(old_link, self);
    }
}

std::optional<UndoLink> TransactionManager::GetUndoLink(const std::string &tab_name, Rid rid) {
    auto version_link = GetVersionLink(tab_name, rid);
    if (!version_link.has_value() || !version_link->prev_.IsValid()) {
        return std::nullopt;
    }
    return version_link->prev_;
}

std::optional<VersionUndoLink> TransactionManager::GetVersionLink(const std::string &tab_name, Rid rid) {
    auto table_info = GetTableVersionInfo(tab_name);
    auto page_info = GetPageVersionInfoOnTable(table_info, rid.page_no);
    if (page_info == nullptr) {
        return std::nullopt;
    }

    std::shared_lock<std::shared_mutex> page_lock(page_info->mutex_);
    if (rid.slot_no < 0) {
        return std::nullopt;
    }
    const VersionUndoLink *version = page_info->GetVersion(static_cast<slot_offset_t>(rid.slot_no));
    return version == nullptr ? std::nullopt : std::optional<VersionUndoLink>(*version);
}

std::optional<UndoLog> TransactionManager::GetUndoLogOptional(UndoLink link) {
    if (!link.IsValid()) {
        return std::nullopt;
    }
    return transaction_registry_.WithTransactionShared(link.prev_txn_, [&](Transaction *txn)
                                                -> std::optional<UndoLog> {
        if (txn == nullptr) {
            return std::nullopt;
        }
        if (link.prev_log_idx_ < 0 || static_cast<size_t>(link.prev_log_idx_) >= txn->GetUndoLogNum()) {
            return std::nullopt;
        }
        return txn->GetUndoLog(link.prev_log_idx_);
    });
}

UndoLink TransactionManager::AppendUndoLog(const std::string &tab_name, Rid rid,
                                           Transaction *txn, UndoLog log) {
    if (txn == nullptr) {
        throw InternalError("Cannot append undo log without a transaction");
    }
    auto table_info = GetTableVersionInfo(tab_name);
    auto page_info = GetPageVersionInfoOnTable(table_info, rid.page_no);
    std::shared_lock<std::shared_mutex> page_lock;
    if (page_info != nullptr && rid.slot_no >= 0) {
        page_lock = std::shared_lock<std::shared_mutex>(page_info->mutex_);
        const auto slot = static_cast<slot_offset_t>(rid.slot_no);
        const VersionUndoLink *stored_version = page_info->GetVersion(slot);
        log.prev_version_ = stored_version == nullptr ? UndoLink{} : stored_version->prev_;
    } else {
        log.prev_version_ = {};
    }

    const UndoLink predecessor = log.prev_version_;
    const bool cross_transaction = predecessor.IsValid() &&
                                   predecessor.prev_txn_ != txn->get_transaction_id();
    if (cross_transaction) {
        // The page's ownership edge remains live while its latch is held, so
        // the registry entry cannot disappear before this retain completes.
        RetainUndoReference(predecessor);
    }
    try {
        return txn->AppendUndoLog(std::move(log));
    } catch (...) {
        if (cross_transaction) {
            ReleaseUndoReference(predecessor);
        }
        throw;
    }
}

UndoLog TransactionManager::GetUndoLog(UndoLink link) {
    auto undo_log = GetUndoLogOptional(link);
    if (!undo_log.has_value()) {
        throw InternalError("Undo log not found");
    }
    return *undo_log;
}

bool TransactionManager::PeekUndoLogMeta(UndoLink link, timestamp_t *ts, bool *is_deleted,
                                         UndoLink *prev_version) {
    bool found = false;
    transaction_registry_.WithTransactionShared(link.prev_txn_, [&](Transaction *txn) {
        if (txn == nullptr) return;
        txn->WithUndoLog(static_cast<size_t>(link.prev_log_idx_), [&](const UndoLog &undo_log) {
            found = true;
            if (ts != nullptr) *ts = undo_log.ts_;
            if (is_deleted != nullptr) *is_deleted = undo_log.is_deleted_;
            if (prev_version != nullptr) *prev_version = undo_log.prev_version_;
        });
    });
    return found;
}

timestamp_t TransactionManager::GetWatermark() {
    return lifecycle_state_.running_txns.GetWatermark();
}

void TransactionManager::PublishReadyTransaction(txn_id_t txn_id) {
    auto &shard = gc_state_.ready_shards[TransactionReadyShardFor(txn_id)];
    std::lock_guard<std::mutex> ready_lock(shard.mutex);
    shard.transactions.push_back(txn_id);
    gc_state_.ready_transaction_count.fetch_add(1, std::memory_order_release);
}

void TransactionManager::EraseTableState(const std::string &tab_name) {
    snapshot_index_history_.EraseTable(tab_name);
    rmdb::u32 erased_table_id = 0;
    std::unique_lock<std::shared_mutex> guard(version_state_.mutex);
    auto table_iter = version_state_.tables.find(tab_name);
    if (table_iter != version_state_.tables.end()) {
        if (table_iter->second != nullptr) {
            erased_table_id = table_iter->second->table_id_;
            version_state_.tables_by_id.erase(table_iter->second->table_id_);
        }
        version_state_.tables.erase(table_iter);
        version_state_.epoch.fetch_add(1, std::memory_order_acq_rel);
    }
    guard.unlock();
    if (erased_table_id != 0) {
        std::vector<std::unique_lock<std::mutex>> delete_locks;
        delete_locks.reserve(gc_state_.logical_delete_shards.size());
        for (auto &shard : gc_state_.logical_delete_shards) {
            delete_locks.emplace_back(shard.mutex);
        }
        for (auto &shard : gc_state_.logical_delete_shards) {
            for (auto iter = shard.deletes.begin(); iter != shard.deletes.end();) {
                if (iter->first.table_id == erased_table_id) {
                    iter = shard.deletes.erase(iter);
                } else {
                    ++iter;
                }
            }
        }
    }
}
