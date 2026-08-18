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
#include "execution/execution_common.h"
#include "record/rm_file_handle.h"
#include "record/rm_scan.h"

#include <algorithm>
#include <vector>

namespace {

rmdb::u64 snapshot_index_id(const IndexMeta &index) {
    if (index.index_id == 0) {
        throw InternalError("Snapshot index history requires a stable index id");
    }
    return index.index_id;
}

std::string index_key(const IndexMeta &index, const char *record_data) {
    std::string key;
    size_t key_size = static_cast<size_t>(index.logical_col_tot_len()) +
                      static_cast<size_t>(index.col_num) * 2;
    for (const auto &col : index.cols) {
        key_size += col.name.size();
    }
    key.reserve(key_size);
    for (int i = 0; i < index.col_num; ++i) {
        const auto &col = index.cols[i];
        key += col.name;
        key.push_back('\x1f');
        key.append(record_data + col.offset, col.len);
        key.push_back('\x1e');
    }
    return key;
}

std::string raw_index_key(const IndexMeta &index, const char *record_data) {
    std::string key(index.logical_col_tot_len(), '\0');
    rmdb::build_index_logical_key_into(index, record_data, key.data());
    return key;
}

const ColMeta *logical_identity_col(const TabMeta &tab) {
    auto id_col = std::find_if(tab.cols.begin(), tab.cols.end(), [](const ColMeta &col) {
        return col.name == "id";
    });
    if (id_col != tab.cols.end()) {
        return &*id_col;
    }
    if (tab.cols.size() == 1) {
        return &tab.cols.front();
    }
    return nullptr;
}

std::string logical_key(const TabMeta &tab, const RmRecord &record) {
    const ColMeta *col = logical_identity_col(tab);
    if (col == nullptr) {
        return {};
    }
    std::string key = "\x1dlogical\x1f";
    key.reserve(key.size() + col->name.size() + 1 + static_cast<size_t>(col->len));
    key += col->name;
    key.push_back('\x1f');
    key.append(record.data + col->offset, col->len);
    return key;
}

bool tuple_version_visible_to_txn(const TupleMeta &meta, Transaction *txn) {
    if (txn == nullptr) {
        return true;
    }
    if (meta.ts_ >= TXN_START_ID) {
        txn_id_t owner = meta.ts_ - TXN_START_ID;
        return owner == txn->get_transaction_id();
    }
    return meta.ts_ <= txn->get_read_ts();
}

bool keys_overlap(const std::vector<std::string> &lhs, const std::vector<std::string> &rhs) {
    for (const auto &key : lhs) {
        if (std::find(rhs.begin(), rhs.end(), key) != rhs.end()) {
            return true;
        }
    }
    return false;
}

bool is_logical_identity_key(const std::string &key) {
    static const std::string prefix = "\x1dlogical\x1f";
    return key.compare(0, prefix.size(), prefix) == 0;
}

bool nonlogical_keys_overlap(const std::vector<std::string> &lhs, const std::vector<std::string> &rhs) {
    for (const auto &key : lhs) {
        if (is_logical_identity_key(key)) {
            continue;
        }
        if (std::find(rhs.begin(), rhs.end(), key) != rhs.end()) {
            return true;
        }
    }
    return false;
}

}  // namespace

std::optional<RmRecord> TransactionManager::GetVisibleTuple(const std::string &tab_name, const Rid &rid,
                                                            Transaction *txn, TupleMeta *visible_meta) {
    RmRecord visible_record;
    if (!GetVisibleTupleInto(tab_name, rid, txn, &visible_record, visible_meta)) {
        return std::nullopt;
    }
    return visible_record;
}

bool TransactionManager::GetVisibleTupleInto(const std::string &tab_name, const Rid &rid, Transaction *txn,
                                             RmRecord *out_record, TupleMeta *visible_meta,
                                             RmRecordPageCursor *page_cursor) {
    return GetVisibleTupleInto(tab_name, GetTableVersionInfo(tab_name), rid, txn, out_record,
                               visible_meta, page_cursor);
}

bool TransactionManager::GetVisibleTupleInto(const std::string &tab_name,
                                             const std::shared_ptr<TableVersionInfo> &table_info,
                                             const Rid &rid, Transaction *txn, RmRecord *out_record,
                                             TupleMeta *visible_meta, RmRecordPageCursor *page_cursor) {
    bool found = page_cursor != nullptr
                     ? page_cursor->read_record(rid, out_record)
                     : sm_manager_->fhs_.at(tab_name)->read_record(rid, out_record, nullptr);
    if (!found) {
        return false;
    }

    TupleMeta base_meta{0, false};
    std::optional<VersionUndoLink> version_link;
    PageVersionInfo *page_info = GetPageVersionInfoOnTableRaw(table_info, rid.page_no);
    // 页锁只覆盖"读 meta + 链 + retain 首层"的瞬间。retain 后释放页锁:
    // 链存活由引用计数传递闭包保证——retain 首层 -> 首层日志存活 -> 其
    // prev_version_ 引用使下一层存活 -> ... 行走期间 GC/abort 无法释放
    // 任何链上日志。行走结束(含 found=false 路径)统一 release。
    UndoLink retained_link;
    bool retained = false;
    bool version_walk_registered = false;
    std::shared_lock<std::shared_mutex> page_lock;
    if (page_info != nullptr && rid.slot_no >= 0) {
        page_lock = std::shared_lock<std::shared_mutex>(page_info->mutex_);
        const auto slot = static_cast<slot_offset_t>(rid.slot_no);
        if (const TupleMeta *stored_meta = page_info->GetTupleMeta(slot); stored_meta != nullptr) {
            base_meta = *stored_meta;
        }
        if (const VersionUndoLink *stored_version = page_info->GetVersion(slot); stored_version != nullptr) {
            version_link = *stored_version;
        }
        if (version_link.has_value() && version_link->prev_.IsValid()) {
            retained_link = version_link->prev_;
            RetainUndoReference(retained_link);
            retained = true;
            page_info->BeginVersionWalk();
            version_walk_registered = true;
        }
        page_lock.unlock();
    }
    // Retain happens before dropping the page lock so GC cannot reclaim the
    // chain between reading the head and walking it.  Install the matching
    // release immediately: the current-version-visible branch below is also
    // an exit from this function and must not leak a transient reference.
    auto release_retained = rmdb::make_scope_exit([&] {
        if (retained) {
            ReleaseUndoReference(retained_link);
            retained = false;
        }
        if (version_walk_registered) {
            page_info->EndVersionWalk();
            version_walk_registered = false;
        }
    });
    // 持锁行例外:本事务持有该行记录级 X 锁时,基版本即所见版本
    // (锁在本次读之前由前序写语句获取，当前值可作为后续相对写的基准)。
    const bool locked_row_exception =
        txn != nullptr && !txn->get_lock_set().empty() &&
        txn->holds_record_lock(sm_manager_->fhs_.at(tab_name)->GetFd(), rid);
    if (locked_row_exception || tuple_version_visible_to_txn(base_meta, txn)) {
        if (visible_meta != nullptr) {
            *visible_meta = base_meta;
        }
        if (base_meta.is_deleted_) {
            return false;
        }
        return true;
    }

    // 定长列增量:行内逐层回退,零拷贝。在 WithUndoLog 回调内直接解码
    // undo_log.delta_ 字节流并 memcpy 到行偏移。recon 惰性初始化——仅在
    // 首次遇到 delta 层时才拷贝基行。无 vector 累积,无二次 registry 查找。
    RmRecord recon;
    bool have_delta = false;
    std::shared_ptr<RmRecord> image;
    while (version_link.has_value() && version_link->prev_.IsValid()) {
        const UndoLink link = version_link->prev_;
        bool found = false;
        bool visible = false;
        bool is_deleted = false;
        bool has_next = false;
        timestamp_t ts = INVALID_TS;
        UndoLink next_link;
        transaction_registry_.WithTransactionShared(link.prev_txn_, [&](Transaction *undo_owner) {
            if (undo_owner == nullptr) return;
            undo_owner->WithUndoLog(static_cast<size_t>(link.prev_log_idx_), [&](const UndoLog &undo_log) {
                found = true;
                ts = undo_log.ts_;
                is_deleted = undo_log.is_deleted_;
                TupleMeta undo_meta{ts, is_deleted};
                visible = tuple_version_visible_to_txn(undo_meta, txn);
                has_next = undo_log.prev_version_.IsValid();
                if (has_next) next_link = undo_log.prev_version_;
                if (visible && undo_log.tuple_image_ != nullptr) {
                    image = undo_log.tuple_image_;
                    return;
                }
                if (!undo_log.delta_.empty()) {
                    if (!have_delta) {
                        recon = *out_record;  // 首次 delta 层才拷贝基行
                        have_delta = true;
                    }
                    const auto &d = undo_log.delta_;
                    size_t pos = 0;
                    if (pos + 2 > d.size()) return;
                    rmdb::u16 num = static_cast<rmdb::u16>(d[pos]) | (static_cast<rmdb::u16>(d[pos + 1]) << 8);
                    pos += 2;
                    for (rmdb::u16 i = 0; i < num && pos + 4 <= d.size(); ++i) {
                        rmdb::u16 off = static_cast<rmdb::u16>(d[pos]) | (static_cast<rmdb::u16>(d[pos + 1]) << 8);
                        rmdb::u16 len = static_cast<rmdb::u16>(d[pos + 2]) | (static_cast<rmdb::u16>(d[pos + 3]) << 8);
                        pos += 4;
                        if (pos + len <= d.size()) {
                            memcpy(recon.data + off, &d[pos], len);
                            pos += len;
                        }
                    }
                }
            });
        });
        if (!found) {
            break;
        }
        if (visible) {
            if (visible_meta != nullptr) *visible_meta = TupleMeta{ts, is_deleted};
            if (is_deleted) return false;
            if (image != nullptr) {
                out_record->ResizeAndCopy(image->data, image->size);
                return true;
            }
            if (!have_delta) return true;
            out_record->ResizeAndCopy(recon.data, recon.size);
            return true;
        }
        if (!has_next) break;
        version_link = VersionUndoLink::FromOptionalUndoLink(next_link);
    }
    return false;
}

TransactionManager::IndexEntryVisibilityState TransactionManager::ClassifySnapshotIndexEntryOnPage(
    PageVersionInfo *page_info, const Rid &rid, Transaction *txn, IndexEntryTupleHint *hint) {
    if (hint != nullptr) {
        hint->Reset();
    }
    if (page_info == nullptr || IsSnapshotPageCleanVisible(page_info, txn)) {
        return IndexEntryVisibilityState::CURRENT_KEY_VISIBLE;
    }

    TupleMeta meta{INVALID_TS, false};
    bool has_meta = false;
    bool has_version_link = false;
    {
        std::shared_lock<std::shared_mutex> page_lock(page_info->mutex_);
        const TupleMeta *stored_meta = rid.slot_no < 0
                                           ? nullptr
                                           : page_info->GetTupleMeta(static_cast<slot_offset_t>(rid.slot_no));
        if (stored_meta != nullptr) {
            meta = *stored_meta;
            has_meta = true;
        }
        const VersionUndoLink *stored_version = rid.slot_no < 0
                                                    ? nullptr
                                                    : page_info->GetVersion(static_cast<slot_offset_t>(rid.slot_no));
        has_version_link = stored_version != nullptr;
        if (hint != nullptr) {
            hint->valid = true;
            hint->page_no = rid.page_no;
            hint->slot_no = rid.slot_no;
            hint->page_info = page_info;
            hint->visibility_epoch = page_info->visibility_epoch_.load(std::memory_order_acquire);
            hint->has_meta = has_meta;
            hint->meta = has_meta ? meta : TupleMeta{0, false};
            hint->has_version_link = has_version_link;
            if (has_version_link) {
                hint->version_link = *stored_version;
            }
        }
    }
    return ClassifySnapshotIndexEntryState(has_meta, meta, has_version_link, txn);
}

TransactionManager::IndexEntryVisibilityState TransactionManager::ClassifySnapshotIndexEntryState(
    bool has_meta, const TupleMeta &meta, bool has_version_link, Transaction *txn) const {
    if (!has_meta) {
        return has_version_link ? IndexEntryVisibilityState::NEEDS_HEAP
                                : IndexEntryVisibilityState::CURRENT_KEY_VISIBLE;
    }
    if (meta.ts_ >= TXN_START_ID) {
        txn_id_t owner = meta.ts_ - TXN_START_ID;
        if (txn != nullptr && owner == txn->get_transaction_id()) {
            if (meta.is_deleted_) {
                return IndexEntryVisibilityState::INVISIBLE;
            }
            return has_version_link ? IndexEntryVisibilityState::NEEDS_HEAP
                                    : IndexEntryVisibilityState::CURRENT_KEY_VISIBLE;
        }
        return has_version_link ? IndexEntryVisibilityState::NEEDS_HEAP
                                : IndexEntryVisibilityState::INVISIBLE;
    }
    timestamp_t read_ts = txn == nullptr ? lifecycle_state_.last_commit_ts.load() : txn->get_read_ts();
    if (meta.ts_ > read_ts) {
        return has_version_link ? IndexEntryVisibilityState::NEEDS_HEAP
                                : IndexEntryVisibilityState::INVISIBLE;
    }
    return meta.is_deleted_ ? IndexEntryVisibilityState::INVISIBLE
                            : IndexEntryVisibilityState::CURRENT_KEY_VISIBLE;
}

bool TransactionManager::IsSnapshotPageCleanVisible(const PageVersionInfo *page_info, Transaction *txn) const {
    if (page_info == nullptr) {
        return true;
    }
    timestamp_t read_ts = txn == nullptr ? lifecycle_state_.last_commit_ts.load() : txn->get_read_ts();
    return page_info->max_committed_meta_ts_.load(std::memory_order_relaxed) <= read_ts &&
           page_info->uncommitted_meta_count_.load(std::memory_order_relaxed) <= 0 &&
           page_info->deleted_count_.load(std::memory_order_relaxed) <= 0;
}

void TransactionManager::EnsureWriteConflictFree(Transaction *txn, const std::string &tab_name, const Rid &rid) {
    if (txn == nullptr) {
        return;
    }
    TupleMeta current_meta = GetTupleMetaOrDefault(tab_name, rid);
    EnsureWriteConflictFree(txn, current_meta);
}

void TransactionManager::EnsureWriteConflictFree(Transaction *txn, const TupleMeta &current_meta) {
    if (txn == nullptr) {
        return;
    }
    if (current_meta.ts_ >= TXN_START_ID &&
        current_meta.ts_ - TXN_START_ID != txn->get_transaction_id()) {
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION,
                                        AbortSubReason::UNCOMMITTED_WRITE_CONFLICT);
    }
    if (IsWriteWriteConflict(current_meta.ts_, txn)) {
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION,
                                        AbortSubReason::STALE_SNAPSHOT_WRITE_CONFLICT);
    }
}

void TransactionManager::EnsureKeyConflictFree(Transaction *txn, const std::string &tab_name, const TabMeta &tab,
                                               const RmRecord &record, const Rid *self_rid,
                                               const std::vector<rmdb::IndexBinding> *prebound_bindings) {
    if (txn == nullptr || sm_manager_ == nullptr) {
        return;
    }
    // 调用方(Insert/Update executor)已在构造期绑定索引,传入复用,避免每行
    // 重新做索引名拼接和 map 查找。
    std::vector<rmdb::IndexBinding> local_bindings;
    const std::vector<rmdb::IndexBinding> *bindings = prebound_bindings;
    if (bindings == nullptr) {
        local_bindings = rmdb::bind_table_indexes(sm_manager_, tab_name, tab);
        bindings = &local_bindings;
    }
    for (const auto &binding : *bindings) {
        const auto &index = *binding.meta;
        if (!index.unique) {
            continue;
        }
        std::string candidate_key = index_key(index, record.data);
        std::vector<Rid> matches;
        binding.ih->get_value(candidate_key.data(), &matches, txn);
        auto historical = LookupSnapshotIndexHistory(tab_name, index, txn->get_read_ts());
        matches.insert(matches.end(), historical.begin(), historical.end());
        auto conflict = ClassifyUniqueIndexConflict(txn, tab_name, index, candidate_key.data(), matches, self_rid);
        if (conflict == UniqueKeyConflictResult::ABORT) {
            throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION,
                                            AbortSubReason::UNIQUE_KEY_CONFLICT);
        }
    }

    const ColMeta *identity_col = logical_identity_col(tab);
    if (identity_col == nullptr) {
        return;
    }
    bool identity_indexed = std::any_of(tab.indexes.begin(), tab.indexes.end(), [&](const IndexMeta &index) {
        return index.unique && index.col_num == 1 && index.cols.front().name == identity_col->name;
    });
    if (identity_indexed) {
        return;
    }

    const std::vector<std::string> candidate_keys{logical_key(tab, record)};
    auto &fh = sm_manager_->fhs_.at(tab_name);
    for (RmScan scan(fh.get()); !scan.is_end(); scan.next()) {
        Rid rid = scan.rid();
        if (self_rid != nullptr && rid == *self_rid) {
            continue;
        }
        auto base_record = fh->get_record(rid, nullptr);
        TupleMeta base_meta = GetTupleMetaOrDefault(tab_name, rid);
        const std::vector<std::string> base_keys{logical_key(tab, *base_record)};
        auto visible_record = GetVisibleTuple(tab_name, rid, txn);
        bool uncommitted_other = base_meta.ts_ >= TXN_START_ID &&
                                 base_meta.ts_ - TXN_START_ID != txn->get_transaction_id();
        bool newer_committed = base_meta.ts_ < TXN_START_ID && base_meta.ts_ > txn->get_read_ts();
        bool base_visible = tuple_version_visible_to_txn(base_meta, txn);

        if (visible_record.has_value()) {
            const std::vector<std::string> visible_keys{logical_key(tab, *visible_record)};
            if (keys_overlap(candidate_keys, visible_keys)) {
                bool self_owned = base_meta.ts_ >= TXN_START_ID &&
                                  base_meta.ts_ - TXN_START_ID == txn->get_transaction_id();
                if (self_owned && !nonlogical_keys_overlap(candidate_keys, visible_keys)) {
                    continue;
                }
                if (!base_visible || uncommitted_other || newer_committed) {
                    throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION,
                                                    AbortSubReason::UNIQUE_KEY_CONFLICT);
                }
                continue;
            }
            if (keys_overlap(candidate_keys, base_keys) && (uncommitted_other || newer_committed)) {
                throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION,
                                                AbortSubReason::UNIQUE_KEY_CONFLICT);
            }
            continue;
        }

        if (!keys_overlap(candidate_keys, base_keys)) {
            continue;
        }
        if (uncommitted_other || newer_committed) {
            throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION,
                                            AbortSubReason::UNIQUE_KEY_CONFLICT);
        }
    }
}

void TransactionManager::RecordSnapshotIndexRetirement(const std::string &tab_name, const IndexMeta &index,
                                                        const Rid &rid, timestamp_t retire_ts) {
    snapshot_index_history_.Record(tab_name, snapshot_index_id(index), rid, retire_ts);
}

std::vector<Rid> TransactionManager::LookupSnapshotIndexHistory(const std::string &tab_name,
                                                                const IndexMeta &index,
                                                                timestamp_t read_ts) {
    return snapshot_index_history_.Lookup(tab_name, snapshot_index_id(index), read_ts);
}

void TransactionManager::EraseSnapshotIndexHistory(const std::string &tab_name, const IndexMeta &index) {
    snapshot_index_history_.EraseIndex(tab_name, snapshot_index_id(index));
}

void TransactionManager::EraseSnapshotIndexHistory(const std::string &tab_name) {
    snapshot_index_history_.EraseTable(tab_name);
}

void TransactionManager::ClearSnapshotIndexHistory() {
    snapshot_index_history_.Clear();
}

TransactionManager::UniqueKeyConflictResult TransactionManager::ClassifyUniqueIndexConflict(
    Transaction *txn, const std::string &tab_name, const IndexMeta &index, const char *key,
    const std::vector<Rid> &matches, const Rid *self_rid) {
    if (!index.unique) {
        return UniqueKeyConflictResult::NONE;
    }
    if (matches.empty()) {
        return UniqueKeyConflictResult::NONE;
    }
    std::string candidate_key(key, index.col_tot_len);
    if (txn == nullptr || sm_manager_ == nullptr) {
        for (const auto &rid : matches) {
            if (self_rid == nullptr || rid != *self_rid) {
                return UniqueKeyConflictResult::FAILURE;
            }
        }
        return UniqueKeyConflictResult::NONE;
    }

    auto &fh = sm_manager_->fhs_.at(tab_name);
    for (const auto &rid : matches) {
        if (self_rid != nullptr && rid == *self_rid) {
            continue;
        }
        auto base_record = fh->get_record(rid, nullptr);
        TupleMeta base_meta = GetTupleMetaOrDefault(tab_name, rid);
        auto visible_record = GetVisibleTuple(tab_name, rid, txn);

        bool uncommitted_other = base_meta.ts_ >= TXN_START_ID &&
                                 base_meta.ts_ - TXN_START_ID != txn->get_transaction_id();
        bool newer_committed = base_meta.ts_ < TXN_START_ID && base_meta.ts_ > txn->get_read_ts();
        std::string base_key = raw_index_key(index, base_record->data);
        bool base_visible = tuple_version_visible_to_txn(base_meta, txn);

        if (visible_record.has_value()) {
            std::string visible_key = raw_index_key(index, visible_record->data);
            if (visible_key == candidate_key) {
                if (base_visible) {
                    return UniqueKeyConflictResult::FAILURE;
                }
                return UniqueKeyConflictResult::ABORT;
            }
            if (base_key == candidate_key && (uncommitted_other || newer_committed)) {
                return UniqueKeyConflictResult::ABORT;
            }
            continue;
        }

        if (base_key == candidate_key && (uncommitted_other || newer_committed)) {
            return UniqueKeyConflictResult::ABORT;
        }
        return UniqueKeyConflictResult::NONE;
    }
    return UniqueKeyConflictResult::NONE;
}
