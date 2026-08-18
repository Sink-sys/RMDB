#pragma once

#include <array>
#include <atomic>
#include <cassert>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/checked_atomic_counter.h"
#include "common/types.h"
#include "transaction.h"

/// 版本链中的第一个撤销链接，将表堆元组链接到撤销日志。
struct VersionUndoLink {
    /** 版本链中的下一个版本。 */
    UndoLink prev_;
    bool in_progress_{false};

    friend auto operator==(const VersionUndoLink &a, const VersionUndoLink &b) {
        return a.prev_ == b.prev_ && a.in_progress_ == b.in_progress_;
    }

    friend auto operator!=(const VersionUndoLink &a, const VersionUndoLink &b) { return !(a == b); }

    inline static std::optional<VersionUndoLink> FromOptionalUndoLink(std::optional<UndoLink> undo_link) {
        if (undo_link.has_value()) {
            return VersionUndoLink{*undo_link};
        }
        return std::nullopt;
    }
};
struct TransactionPageVersionInfo {
    std::shared_mutex mutex_;
    // Readers release the page latch before walking undo logs.  This short-lived
    // pin prevents GC from severing an interior chain edge while such a walker
    // still relies on the original ownership closure.
    std::atomic<rmdb::u32> active_version_walkers_{0};
    std::atomic<int> active_meta_count_{0};
    std::atomic<int> uncommitted_meta_count_{0};
    std::atomic<int> deleted_count_{0};
    std::atomic<int> version_link_count_{0};
    std::atomic<timestamp_t> max_committed_meta_ts_{0};
    std::atomic<rmdb::u64> visibility_epoch_{1};
    std::atomic<rmdb::u32> dirty_slot_count_{0};
    rmdb::u32 slot_count_{0};
    rmdb::u32 slot_word_count_{0};
    std::unique_ptr<std::atomic<rmdb::u64>[]> dirty_slot_words_;
    rmdb::u32 dirty_slot_word_count_{0};
    std::unique_ptr<rmdb::u64[]> tuple_meta_valid_words_;
    std::unique_ptr<TupleMeta[]> tuple_meta_values_;
    std::unique_ptr<rmdb::u64[]> version_valid_words_;
    std::unique_ptr<VersionUndoLink[]> version_values_;

    void BeginVersionWalk() {
        rmdb::atomic_counter::Increment(active_version_walkers_, rmdb::u32{1},
                                        std::memory_order_acq_rel);
    }

    void EndVersionWalk() {
        rmdb::atomic_counter::DecrementPositive(
            active_version_walkers_, "Active version walker count", rmdb::u32{1},
            std::memory_order_acq_rel, std::memory_order_acquire);
    }

    rmdb::u32 ActiveVersionWalkers() const {
        return active_version_walkers_.load(std::memory_order_acquire);
    }

    void InitDirtySlots(rmdb::u32 slot_count) {
        if (slot_count == 0 || dirty_slot_words_ != nullptr) {
            return;
        }
        rmdb::u32 word_count = (slot_count + 63) / 64;
        auto words = std::make_unique<std::atomic<rmdb::u64>[]>(word_count);
        for (rmdb::u32 i = 0; i < word_count; ++i) {
            words[i].store(0, std::memory_order_relaxed);
        }
        slot_count_ = slot_count;
        slot_word_count_ = word_count;
        dirty_slot_word_count_ = word_count;
        dirty_slot_words_ = std::move(words);
    }

    bool CanTrackSlot(slot_offset_t slot_no) const {
        return slot_no < slot_count_;
    }

    static rmdb::u64 SlotBit(slot_offset_t slot_no) {
        return rmdb::u64{1} << (slot_no & 63);
    }

    void EnsureTupleMetaStorage() {
        if (tuple_meta_values_ != nullptr) {
            return;
        }
        tuple_meta_valid_words_ = std::make_unique<rmdb::u64[]>(slot_word_count_);
        tuple_meta_values_ = std::make_unique<TupleMeta[]>(slot_count_);
    }

    bool HasTupleMeta(slot_offset_t slot_no) const {
        if (!CanTrackSlot(slot_no) || tuple_meta_valid_words_ == nullptr) {
            return false;
        }
        return (tuple_meta_valid_words_[slot_no / 64] & SlotBit(slot_no)) != 0;
    }

    const TupleMeta *GetTupleMeta(slot_offset_t slot_no) const {
        return HasTupleMeta(slot_no) ? &tuple_meta_values_[slot_no] : nullptr;
    }

    TupleMeta *GetMutableTupleMeta(slot_offset_t slot_no) {
        return HasTupleMeta(slot_no) ? &tuple_meta_values_[slot_no] : nullptr;
    }

    bool SetTupleMeta(slot_offset_t slot_no, const TupleMeta &meta) {
        assert(CanTrackSlot(slot_no));
        EnsureTupleMetaStorage();
        rmdb::u64 &word = tuple_meta_valid_words_[slot_no / 64];
        rmdb::u64 bit = SlotBit(slot_no);
        bool existed = (word & bit) != 0;
        tuple_meta_values_[slot_no] = meta;
        word |= bit;
        return existed;
    }

    bool ClearTupleMeta(slot_offset_t slot_no) {
        if (!HasTupleMeta(slot_no)) {
            return false;
        }
        tuple_meta_valid_words_[slot_no / 64] &= ~SlotBit(slot_no);
        return true;
    }

    void ReleaseTupleMetaStorageIfEmpty() {
        if (active_meta_count_.load(std::memory_order_relaxed) == 0) {
            tuple_meta_values_.reset();
            tuple_meta_valid_words_.reset();
        }
    }

    void EnsureVersionStorage() {
        if (version_values_ != nullptr) {
            return;
        }
        version_valid_words_ = std::make_unique<rmdb::u64[]>(slot_word_count_);
        version_values_ = std::make_unique<VersionUndoLink[]>(slot_count_);
    }

    bool HasVersion(slot_offset_t slot_no) const {
        if (!CanTrackSlot(slot_no) || version_valid_words_ == nullptr) {
            return false;
        }
        return (version_valid_words_[slot_no / 64] & SlotBit(slot_no)) != 0;
    }

    const VersionUndoLink *GetVersion(slot_offset_t slot_no) const {
        return HasVersion(slot_no) ? &version_values_[slot_no] : nullptr;
    }

    VersionUndoLink *GetMutableVersion(slot_offset_t slot_no) {
        return HasVersion(slot_no) ? &version_values_[slot_no] : nullptr;
    }

    bool SetVersion(slot_offset_t slot_no, const VersionUndoLink &version) {
        assert(CanTrackSlot(slot_no));
        EnsureVersionStorage();
        rmdb::u64 &word = version_valid_words_[slot_no / 64];
        rmdb::u64 bit = SlotBit(slot_no);
        bool existed = (word & bit) != 0;
        version_values_[slot_no] = version;
        word |= bit;
        return existed;
    }

    bool ClearVersion(slot_offset_t slot_no) {
        if (!HasVersion(slot_no)) {
            return false;
        }
        version_valid_words_[slot_no / 64] &= ~SlotBit(slot_no);
        return true;
    }

    void ReleaseVersionStorageIfEmpty() {
        if (version_link_count_.load(std::memory_order_relaxed) == 0) {
            version_values_.reset();
            version_valid_words_.reset();
        }
    }

    bool CanTrackDirtySlot(slot_offset_t slot_no) const {
        rmdb::u32 word_idx = static_cast<rmdb::u32>(slot_no / 64);
        return dirty_slot_words_ != nullptr && word_idx < dirty_slot_word_count_;
    }

    bool MarkDirtySlot(slot_offset_t slot_no) {
        rmdb::u32 word_idx = static_cast<rmdb::u32>(slot_no / 64);
        if (!CanTrackDirtySlot(slot_no)) {
            return false;
        }
        rmdb::u64 bit = 1ull << (slot_no & 63);
        rmdb::u64 old = dirty_slot_words_[word_idx].fetch_or(bit, std::memory_order_acq_rel);
        if ((old & bit) != 0) {
            return false;
        }
        rmdb::atomic_counter::Increment(dirty_slot_count_, rmdb::u32{1}, std::memory_order_acq_rel);
        return true;
    }

    bool ClearDirtySlot(slot_offset_t slot_no) {
        rmdb::u32 word_idx = static_cast<rmdb::u32>(slot_no / 64);
        if (!CanTrackDirtySlot(slot_no)) {
            return false;
        }
        rmdb::u64 bit = 1ull << (slot_no & 63);
        rmdb::u64 old = dirty_slot_words_[word_idx].fetch_and(~bit, std::memory_order_acq_rel);
        if ((old & bit) == 0) {
            return false;
        }
        rmdb::atomic_counter::DecrementPositive(dirty_slot_count_, "Dirty page slot count", rmdb::u32{1},
                                                std::memory_order_acq_rel, std::memory_order_acquire);
        return true;
    }

    bool IsSlotClean(slot_offset_t slot_no) const {
        rmdb::u32 word_idx = static_cast<rmdb::u32>(slot_no / 64);
        if (dirty_slot_words_ == nullptr || word_idx >= dirty_slot_word_count_) {
            return false;
        }
        rmdb::u64 bit = 1ull << (slot_no & 63);
        return (dirty_slot_words_[word_idx].load(std::memory_order_acquire) & bit) == 0;
    }

    rmdb::u32 DirtySlotCount() const {
        return dirty_slot_count_.load(std::memory_order_acquire);
    }
};

struct TransactionTableVersionInfo {
    static constexpr page_id_t kExactDirtyPageLimit = 1 << 20;
    static constexpr size_t kExactDirtyPageWordCount = static_cast<size_t>(kExactDirtyPageLimit) / 64;
    static constexpr size_t kDensePageChunkBits = 10;
    static constexpr size_t kDensePageChunkSize = 1 << kDensePageChunkBits;
    static constexpr size_t kDensePageChunkMask = kDensePageChunkSize - 1;
    static constexpr size_t kDensePageChunkCount =
        static_cast<size_t>(kExactDirtyPageLimit) / kDensePageChunkSize;

    struct PageInfoChunk {
        std::array<std::atomic<TransactionPageVersionInfo *>, kDensePageChunkSize> pages_{};

        PageInfoChunk() {
            for (auto &page : pages_) {
                page.store(nullptr, std::memory_order_relaxed);
            }
        }
    };

    TransactionTableVersionInfo() {
        for (auto &word : dirty_page_filter_) {
            word.store(0, std::memory_order_relaxed);
        }
        for (auto &word : exact_dirty_pages_) {
            word.store(0, std::memory_order_relaxed);
        }
        for (auto &chunk : page_info_chunks_) {
            chunk.store(nullptr, std::memory_order_relaxed);
        }
    }

    std::shared_mutex mutex_;
    // 稳定表标识:创建时分配(version_state_.next_table_id),退役元组以
    // u32 引用表;table_name 供物理删除等按名查找路径使用。
    rmdb::u32 table_id_{0};
    std::string table_name_;
    std::atomic<rmdb::u64> page_map_epoch_{0};
    std::atomic<rmdb::u32> dirty_page_count_{0};
    std::atomic<rmdb::u64> dirty_slot_total_{0};
    std::array<std::atomic<rmdb::u64>, 1024> dirty_page_filter_{};
    std::array<std::atomic<rmdb::u64>, kExactDirtyPageWordCount> exact_dirty_pages_{};
    std::array<std::atomic<PageInfoChunk *>, kDensePageChunkCount> page_info_chunks_{};
    std::vector<std::unique_ptr<PageInfoChunk>> page_info_chunk_owners_;
    std::unordered_map<page_id_t, std::shared_ptr<TransactionPageVersionInfo>> pages_;

    bool maybe_has_page_version_info(page_id_t page_no) const {
        rmdb::u64 hash = static_cast<rmdb::u64>(static_cast<rmdb::u32>(page_no)) * 11400714819323198485ull;
        size_t bucket = static_cast<size_t>(hash & (dirty_page_filter_.size() - 1));
        rmdb::u64 bit = 1ull << ((hash >> 10) & 63);
        return (dirty_page_filter_[bucket].load(std::memory_order_acquire) & bit) != 0;
    }

    void mark_page_version_info(page_id_t page_no) {
        rmdb::u64 hash = static_cast<rmdb::u64>(static_cast<rmdb::u32>(page_no)) * 11400714819323198485ull;
        size_t bucket = static_cast<size_t>(hash & (dirty_page_filter_.size() - 1));
        rmdb::u64 bit = 1ull << ((hash >> 10) & 63);
        dirty_page_filter_[bucket].fetch_or(bit, std::memory_order_release);
    }

    bool tracks_exact_dirty_page(page_id_t page_no) const {
        return page_no >= 0 && page_no < kExactDirtyPageLimit;
    }

    bool is_page_dirty_exact(page_id_t page_no) const {
        if (!tracks_exact_dirty_page(page_no)) {
            return true;
        }
        size_t word_idx = static_cast<size_t>(page_no) / 64;
        rmdb::u64 bit = 1ull << (static_cast<rmdb::u32>(page_no) & 63);
        return (exact_dirty_pages_[word_idx].load(std::memory_order_acquire) & bit) != 0;
    }

    bool mark_page_dirty_exact(page_id_t page_no) {
        if (!tracks_exact_dirty_page(page_no)) {
            return false;
        }
        size_t word_idx = static_cast<size_t>(page_no) / 64;
        rmdb::u64 bit = 1ull << (static_cast<rmdb::u32>(page_no) & 63);
        rmdb::u64 old = exact_dirty_pages_[word_idx].fetch_or(bit, std::memory_order_acq_rel);
        if ((old & bit) != 0) {
            return false;
        }
        rmdb::atomic_counter::Increment(dirty_page_count_, rmdb::u32{1}, std::memory_order_acq_rel);
        return true;
    }

    bool clear_page_dirty_exact(page_id_t page_no) {
        if (!tracks_exact_dirty_page(page_no)) {
            return false;
        }
        size_t word_idx = static_cast<size_t>(page_no) / 64;
        rmdb::u64 bit = 1ull << (static_cast<rmdb::u32>(page_no) & 63);
        rmdb::u64 old = exact_dirty_pages_[word_idx].fetch_and(~bit, std::memory_order_acq_rel);
        if ((old & bit) == 0) {
            return false;
        }
        rmdb::atomic_counter::DecrementPositive(dirty_page_count_, "Dirty table page count", rmdb::u32{1},
                                                std::memory_order_acq_rel, std::memory_order_acquire);
        return true;
    }

    TransactionPageVersionInfo *lookup_page_info_dense(page_id_t page_no) const {
        if (!tracks_exact_dirty_page(page_no)) {
            return nullptr;
        }
        size_t chunk_idx = static_cast<size_t>(page_no) >> kDensePageChunkBits;
        size_t chunk_offset = static_cast<size_t>(page_no) & kDensePageChunkMask;
        auto *chunk = page_info_chunks_[chunk_idx].load(std::memory_order_acquire);
        if (chunk == nullptr) {
            return nullptr;
        }
        return chunk->pages_[chunk_offset].load(std::memory_order_acquire);
    }

    void publish_page_info_dense(page_id_t page_no, TransactionPageVersionInfo *page_info) {
        if (!tracks_exact_dirty_page(page_no) || page_info == nullptr) {
            return;
        }
        size_t chunk_idx = static_cast<size_t>(page_no) >> kDensePageChunkBits;
        size_t chunk_offset = static_cast<size_t>(page_no) & kDensePageChunkMask;
        auto *chunk = page_info_chunks_[chunk_idx].load(std::memory_order_acquire);
        if (chunk == nullptr) {
            auto owner = std::make_unique<PageInfoChunk>();
            chunk = owner.get();
            page_info_chunk_owners_.push_back(std::move(owner));
            page_info_chunks_[chunk_idx].store(chunk, std::memory_order_release);
        }
        chunk->pages_[chunk_offset].store(page_info, std::memory_order_release);
    }

    rmdb::u32 DirtyPageCount() const {
        return dirty_page_count_.load(std::memory_order_acquire);
    }

    rmdb::u64 DirtySlotTotal() const {
        return dirty_slot_total_.load(std::memory_order_acquire);
    }
};
