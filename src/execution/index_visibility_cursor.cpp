#include "index_visibility_cursor.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

#include "common/index_runtime.h"
#include "common/reusable_flat_u64_set.h"
#include "record/rm_file_handle.h"
#include "transaction/transaction.h"
#include "transaction/transaction_manager.h"

namespace {

class SnapshotIndexEntryCursor {
   public:
    void bind(TransactionManager *txn_mgr, Transaction *txn, std::string tab_name) {
        txn_mgr_ = txn_mgr;
        txn_ = txn;
        tab_name_ = std::move(tab_name);
        table_info_ = txn_mgr_ == nullptr ? nullptr : txn_mgr_->GetOrCreateTableVersionInfo(tab_name_);
        reset();
    }

    void reset() {
        current_page_no_ = RM_NO_PAGE;
        page_info_ = nullptr;
        current_cache_ = nullptr;
        current_page_clean_visible_ = false;
        last_tuple_hint_.Reset();
        ++cache_generation_;
        if (cache_generation_ == 0) {
            for (auto &entry : page_cache_) {
                entry.generation = 0;
            }
            cache_generation_ = 1;
        }
    }

    TransactionManager::IndexEntryVisibilityState classify(const Rid &rid) {
        last_tuple_hint_.Reset();
        if (txn_mgr_ == nullptr) {
            return TransactionManager::IndexEntryVisibilityState::CURRENT_KEY_VISIBLE;
        }
        if (rid.page_no != current_page_no_ || current_cache_ == nullptr) {
            current_page_no_ = rid.page_no;
            current_cache_ = lookup_page_info(rid.page_no);
            page_info_ = current_cache_ == nullptr ? nullptr : current_cache_->page_info;
        } else if (page_info_ == nullptr && table_info_ != nullptr) {
            bool page_may_need_info = table_info_->tracks_exact_dirty_page(rid.page_no)
                                          ? table_info_->is_page_dirty_exact(rid.page_no)
                                          : table_info_->maybe_has_page_version_info(rid.page_no);
            if (page_may_need_info) {
                current_cache_ = lookup_page_info(rid.page_no);
                page_info_ = current_cache_ == nullptr ? nullptr : current_cache_->page_info;
            }
        }
        if (page_info_ == nullptr || current_cache_ == nullptr) {
            current_page_clean_visible_ = true;
            return TransactionManager::IndexEntryVisibilityState::CURRENT_KEY_VISIBLE;
        }
        if (rid.slot_no >= 0 && page_info_->IsSlotClean(static_cast<slot_offset_t>(rid.slot_no))) {
            current_page_clean_visible_ = false;
            return TransactionManager::IndexEntryVisibilityState::CURRENT_KEY_VISIBLE;
        }
        current_page_clean_visible_ = page_clean_visible(*current_cache_);
        if (current_page_clean_visible_) {
            return TransactionManager::IndexEntryVisibilityState::CURRENT_KEY_VISIBLE;
        }
        return txn_mgr_->ClassifySnapshotIndexEntryOnPage(page_info_, rid, txn_, &last_tuple_hint_);
    }

    bool current_page_clean_visible() const { return current_page_clean_visible_; }

    const std::shared_ptr<TransactionManager::TableVersionInfo> &table_info() const {
        return table_info_;
    }

    bool current_page_still_clean_visible(page_id_t page_no) {
        if (!current_page_clean_visible_ || page_no != current_page_no_ || current_cache_ == nullptr) {
            return false;
        }
        if (page_info_ == nullptr) {
            if (table_info_ == nullptr) {
                return true;
            }
            if (table_info_->tracks_exact_dirty_page(page_no)) {
                return !table_info_->is_page_dirty_exact(page_no);
            }
            return !table_info_->maybe_has_page_version_info(page_no);
        }
        current_page_clean_visible_ = page_clean_visible(*current_cache_);
        return current_page_clean_visible_;
    }

   private:
    static constexpr size_t kPageCacheSize = 512;

    struct CachedPageInfo {
        rmdb::u64 generation = 0;
        page_id_t page_no = RM_NO_PAGE;
        bool valid = false;
        rmdb::u64 page_map_epoch = 0;
        TransactionManager::PageVersionInfo *page_info = nullptr;
        rmdb::u64 visibility_epoch = 0;
        bool clean_cached = false;
        bool clean_visible = false;
    };

    static void clear_cached_page_state(CachedPageInfo &entry) {
        entry.visibility_epoch = 0;
        entry.clean_cached = false;
        entry.clean_visible = false;
    }

    CachedPageInfo *lookup_page_info(page_id_t page_no) {
        const auto slot = static_cast<size_t>(page_no) & (kPageCacheSize - 1);
        auto &entry = page_cache_[slot];
        if (entry.generation == cache_generation_ && entry.valid && entry.page_no == page_no) {
            if (entry.page_info == nullptr) {
                if (table_info_ == nullptr) {
                    return &entry;
                }
                if (table_info_->tracks_exact_dirty_page(page_no)) {
                    if (!table_info_->is_page_dirty_exact(page_no)) {
                        return &entry;
                    }
                } else if (!table_info_->maybe_has_page_version_info(page_no)) {
                    return &entry;
                }
            } else {
                return &entry;
            }
            if (table_info_ != nullptr && !table_info_->tracks_exact_dirty_page(page_no)) {
                rmdb::u64 current_epoch = table_info_->page_map_epoch_.load(std::memory_order_acquire);
                if (entry.page_map_epoch == current_epoch) {
                    return &entry;
                }
            }
        }
        clear_cached_page_state(entry);
        entry.generation = cache_generation_;
        entry.page_no = page_no;
        entry.valid = true;
        entry.page_map_epoch = table_info_ == nullptr
                                   ? 0
                                   : table_info_->page_map_epoch_.load(std::memory_order_acquire);
        entry.page_info = nullptr;
        if (table_info_ == nullptr) {
            return &entry;
        }
        if (table_info_->tracks_exact_dirty_page(page_no)) {
            if (!table_info_->is_page_dirty_exact(page_no)) {
                return &entry;
            }
        } else if (!table_info_->maybe_has_page_version_info(page_no)) {
            return &entry;
        }
        rmdb::u64 page_map_epoch = 0;
        entry.page_info = txn_mgr_->GetPageVersionInfoOnTableRaw(table_info_, page_no, &page_map_epoch);
        entry.page_map_epoch = page_map_epoch;
        return &entry;
    }

    bool page_clean_visible(CachedPageInfo &entry) {
        if (entry.page_info == nullptr) {
            return true;
        }
        rmdb::u64 epoch_before = entry.page_info->visibility_epoch_.load(std::memory_order_acquire);
        if (entry.clean_cached && entry.visibility_epoch == epoch_before) {
            return entry.clean_visible;
        }
        bool clean = txn_mgr_->IsSnapshotPageCleanVisible(entry.page_info, txn_);
        rmdb::u64 epoch_after = entry.page_info->visibility_epoch_.load(std::memory_order_acquire);
        if (epoch_before != epoch_after) {
            entry.clean_cached = false;
            entry.clean_visible = false;
            entry.visibility_epoch = epoch_after;
            return false;
        }
        entry.clean_cached = true;
        entry.clean_visible = clean;
        entry.visibility_epoch = epoch_after;
        return clean;
    }

    TransactionManager *txn_mgr_ = nullptr;
    Transaction *txn_ = nullptr;
    std::string tab_name_;
    std::shared_ptr<TransactionManager::TableVersionInfo> table_info_;
    page_id_t current_page_no_ = RM_NO_PAGE;
    TransactionManager::PageVersionInfo *page_info_ = nullptr;
    std::array<CachedPageInfo, kPageCacheSize> page_cache_;
    rmdb::u64 cache_generation_ = 1;
    CachedPageInfo *current_cache_ = nullptr;
    bool current_page_clean_visible_ = false;
    TransactionManager::IndexEntryTupleHint last_tuple_hint_;
};

rmdb::u64 rid_key(const Rid &rid) {
    return (static_cast<rmdb::u64>(static_cast<rmdb::u32>(rid.page_no)) << 32) |
           static_cast<rmdb::u32>(rid.slot_no);
}

}  // namespace

class VisibleIndexCursor::Impl {
   public:
    void bind(TransactionManager *txn_mgr, Transaction *txn, std::string tab_name,
              const std::vector<std::string> *index_col_names, const IndexMeta *index_meta,
              RmFileHandle *file_handle) {
        txn_mgr_ = txn_mgr;
        txn_ = txn;
        tab_name_ = std::move(tab_name);
        index_col_names_ = index_col_names;
        index_meta_ = index_meta;
        snapshot_entry_.bind(txn_mgr_, txn_, tab_name_);
        heap_page_cursor_.bind(file_handle);
        visible_key_scratch_.resize(index_meta_ == nullptr ? 0 : index_meta_->col_tot_len);
        reset();
    }

    void reset() {
        snapshot_entry_.reset();
        heap_page_cursor_.reset();
        history_loaded_ = false;
        history_rids_.clear();
        history_keys_.clear();
        dedup_keys_.clear();
        current_rid_keys_.clear();
    }

    void finish() { heap_page_cursor_.reset(); }

    void reset_for_pool() {
        reset();
        txn_mgr_ = nullptr;
        txn_ = nullptr;
        tab_name_.clear();
        index_col_names_ = nullptr;
        index_meta_ = nullptr;
        heap_page_cursor_.bind(nullptr);
        visible_key_scratch_.clear();
    }

    const std::vector<Rid> &history_rids() {
        load_history();
        return history_rids_;
    }

    void record_current_rid(const Rid &rid) {
        current_rid_keys_.push_back(rid_key(rid));
    }

    bool is_historical(const Rid &rid) {
        load_history();
        return history_keys_.contains(rid_key(rid));
    }

    void deduplicate_and_append_history(std::vector<Rid> *candidates) {
        if (candidates == nullptr) {
            return;
        }
        load_history();
        dedup_keys_.clear();
        dedup_keys_.reserve(candidates->size() + history_rids_.size());
        size_t output = 0;
        for (const auto &rid : *candidates) {
            if (!dedup_keys_.insert(rid_key(rid))) {
                continue;
            }
            (*candidates)[output++] = rid;
        }
        candidates->resize(output);
        for (const auto &rid : history_rids_) {
            if (dedup_keys_.insert(rid_key(rid))) {
                candidates->push_back(rid);
            }
        }
    }

    VisibleIndexCursor::ReadResult read_current(const Rid &rid, const char *index_key, RmRecord *out_record,
                                                bool materialize_index_only, bool require_matching_key) {
        auto state = snapshot_entry_.classify(rid);
        if (state == TransactionManager::IndexEntryVisibilityState::INVISIBLE) {
            return {};
        }
        if (state == TransactionManager::IndexEntryVisibilityState::CURRENT_KEY_VISIBLE && !materialize_index_only) {
            return {VisibleIndexCursor::ReadState::IndexOnly, false};
        }
        if (!read_visible(rid, out_record)) {
            return {VisibleIndexCursor::ReadState::Invisible, true};
        }
        if (require_matching_key && !record_matches_index_key(rid, index_key, *out_record)) {
            return {VisibleIndexCursor::ReadState::Invisible, true};
        }
        return {VisibleIndexCursor::ReadState::Materialized, true};
    }

    VisibleIndexCursor::ReadResult read_visible_entry(const Rid &rid, RmRecord *out_record) {
        if (!read_visible(rid, out_record)) {
            return {VisibleIndexCursor::ReadState::Invisible, true};
        }
        return {VisibleIndexCursor::ReadState::Materialized, true};
    }

    bool current_page_clean_visible() const { return snapshot_entry_.current_page_clean_visible(); }

    bool current_page_still_clean_visible(page_id_t page_no) {
        return snapshot_entry_.current_page_still_clean_visible(page_no);
    }

   private:
    void load_history() {
        if (history_loaded_) {
            return;
        }
        history_loaded_ = true;
        if (txn_mgr_ == nullptr || txn_ == nullptr || index_meta_ == nullptr) {
            return;
        }
        auto candidates = txn_mgr_->LookupSnapshotIndexHistory(tab_name_, *index_meta_, txn_->get_read_ts());
        std::sort(current_rid_keys_.begin(), current_rid_keys_.end());
        current_rid_keys_.erase(std::unique(current_rid_keys_.begin(), current_rid_keys_.end()),
                                current_rid_keys_.end());
        history_rids_.reserve(candidates.size());
        history_keys_.reserve(candidates.size());
        for (const auto &rid : candidates) {
            const rmdb::u64 key = rid_key(rid);
            // 已在 current index 处理过的 RID 不再从 history 重复补充。
            if (history_keys_.insert(key) &&
                !std::binary_search(current_rid_keys_.begin(), current_rid_keys_.end(), key)) {
                history_rids_.push_back(rid);
            }
        }
    }

    bool read_visible(const Rid &rid, RmRecord *out_record) {
        return txn_mgr_ != nullptr && out_record != nullptr &&
               txn_mgr_->GetVisibleTupleInto(tab_name_, snapshot_entry_.table_info(), rid, txn_, out_record,
                                             nullptr, &heap_page_cursor_);
    }

    bool record_matches_index_key(const Rid &rid, const char *index_key, const RmRecord &record) {
        if (index_meta_ == nullptr || index_key == nullptr || record.data == nullptr) {
            return false;
        }
        char *visible_key = rmdb::build_index_key_into(*index_meta_, record.data, rid, &visible_key_scratch_);
        return std::memcmp(visible_key, index_key, index_meta_->col_tot_len) == 0;
    }

    TransactionManager *txn_mgr_{nullptr};
    Transaction *txn_{nullptr};
    std::string tab_name_;
    const std::vector<std::string> *index_col_names_{nullptr};
    const IndexMeta *index_meta_{nullptr};
    SnapshotIndexEntryCursor snapshot_entry_;
    RmRecordPageCursor heap_page_cursor_;
    std::string visible_key_scratch_;
    bool history_loaded_{false};
    std::vector<Rid> history_rids_;
    rmdb::ReusableFlatU64Set history_keys_;
    rmdb::ReusableFlatU64Set dedup_keys_;
    // current index 已处理过的 RID(record_current_rid 记录),history 加载时据此去重。
    // current 扫描期间只追加，首次加载 history 时一次性排序去重，避免逐 RID
    // unordered_set 节点分配、rehash 与哈希开销。
    std::vector<rmdb::u64> current_rid_keys_;
};

std::unique_ptr<VisibleIndexCursor::Impl> VisibleIndexCursor::acquire_impl() {
    auto &pool = impl_pool();
    if (pool.empty()) {
        return std::make_unique<Impl>();
    }
    auto impl = std::move(pool.back());
    pool.pop_back();
    return impl;
}

void VisibleIndexCursor::release_impl(std::unique_ptr<Impl> impl) {
    if (impl == nullptr) {
        return;
    }
    impl->reset_for_pool();
    auto &pool = impl_pool();
    if (pool.size() < 8) {
        pool.push_back(std::move(impl));
    }
}

std::vector<std::unique_ptr<VisibleIndexCursor::Impl>> &VisibleIndexCursor::impl_pool() {
    thread_local std::vector<std::unique_ptr<Impl>> pool;
    return pool;
}

VisibleIndexCursor::VisibleIndexCursor() : impl_(acquire_impl()) {}

VisibleIndexCursor::VisibleIndexCursor(TransactionManager *txn_mgr, Transaction *txn, std::string tab_name,
                                       const std::vector<std::string> *index_col_names,
                                       const IndexMeta *index_meta, RmFileHandle *file_handle)
    : VisibleIndexCursor() {
    bind(txn_mgr, txn, std::move(tab_name), index_col_names, index_meta, file_handle);
}

VisibleIndexCursor::~VisibleIndexCursor() { release_impl(std::move(impl_)); }
VisibleIndexCursor::VisibleIndexCursor(VisibleIndexCursor &&) noexcept = default;
VisibleIndexCursor &VisibleIndexCursor::operator=(VisibleIndexCursor &&other) noexcept {
    if (this != &other) {
        release_impl(std::move(impl_));
        impl_ = std::move(other.impl_);
    }
    return *this;
}

void VisibleIndexCursor::bind(TransactionManager *txn_mgr, Transaction *txn, std::string tab_name,
                              const std::vector<std::string> *index_col_names, const IndexMeta *index_meta,
                              RmFileHandle *file_handle) {
    impl_->bind(txn_mgr, txn, std::move(tab_name), index_col_names, index_meta, file_handle);
}

void VisibleIndexCursor::reset() { impl_->reset(); }
void VisibleIndexCursor::finish() { impl_->finish(); }
const std::vector<Rid> &VisibleIndexCursor::history_rids() { return impl_->history_rids(); }
void VisibleIndexCursor::record_current_rid(const Rid &rid) { impl_->record_current_rid(rid); }
bool VisibleIndexCursor::is_historical(const Rid &rid) { return impl_->is_historical(rid); }
void VisibleIndexCursor::deduplicate_and_append_history(std::vector<Rid> *candidates) {
    impl_->deduplicate_and_append_history(candidates);
}
VisibleIndexCursor::ReadResult VisibleIndexCursor::read_current(const Rid &rid, const char *index_key,
                                                                RmRecord *out_record,
                                                                bool materialize_index_only,
                                                                bool require_matching_key) {
    return impl_->read_current(rid, index_key, out_record, materialize_index_only, require_matching_key);
}
VisibleIndexCursor::ReadResult VisibleIndexCursor::read_visible_entry(const Rid &rid, RmRecord *out_record) {
    return impl_->read_visible_entry(rid, out_record);
}
bool VisibleIndexCursor::current_page_clean_visible() const { return impl_->current_page_clean_visible(); }
bool VisibleIndexCursor::current_page_still_clean_visible(page_id_t page_no) {
    return impl_->current_page_still_clean_visible(page_no);
}
