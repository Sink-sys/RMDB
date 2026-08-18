/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#pragma once

#include <memory>
#include <shared_mutex>
#include <utility>

#include "record/rm_defs.h"
#include "common/types.h"
#include "storage/page_guard.h"

/**
 * Lifetime-safe result of a visibility lookup.
 *
 * Borrowed references keep a buffer-page read lease alongside the raw tuple
 * bytes.  Historical versions are owned because they are reconstructed from
 * undo state and cannot safely point into a mutable page.  The type is kept
 * independent of any particular scan executor so SeqScan, IndexScan and
 * aggregate fast paths can share the same ownership contract.
 */
class VisibleTupleRef {
   public:
    enum class Kind : rmdb::u8 { Empty, BorrowedCurrent, IndexKeyView, OwnedHistorical };

    VisibleTupleRef() = default;
    VisibleTupleRef(const VisibleTupleRef &) = delete;
    VisibleTupleRef &operator=(const VisibleTupleRef &) = delete;
    VisibleTupleRef(VisibleTupleRef &&) noexcept = default;
    VisibleTupleRef &operator=(VisibleTupleRef &&) noexcept = default;

    static VisibleTupleRef Borrowed(ReadPageGuard page_guard, const char *data, int size) {
        return Borrowed(std::move(page_guard), {}, data, size);
    }

    static VisibleTupleRef Borrowed(ReadPageGuard page_guard,
                                    std::shared_lock<std::shared_mutex> record_guard,
                                    const char *data, int size) {
        VisibleTupleRef result;
        if (!page_guard || data == nullptr || size < 0) {
            return result;
        }
        result.page_guard_ = std::move(page_guard);
        result.record_guard_ = std::move(record_guard);
        result.data_ = data;
        result.size_ = size;
        result.kind_ = Kind::BorrowedCurrent;
        return result;
    }

    static VisibleTupleRef Borrowed(std::shared_lock<std::shared_mutex> record_guard, const char *data, int size) {
        VisibleTupleRef result;
        if (!record_guard.owns_lock() || data == nullptr || size < 0) {
            return result;
        }
        result.record_guard_ = std::move(record_guard);
        result.data_ = data;
        result.size_ = size;
        result.kind_ = Kind::BorrowedCurrent;
        return result;
    }

    // The owning RmScan keeps the page pinned. This lease only extends the
    // page read latch until the executor advances the scan.
    static VisibleTupleRef Borrowed(PageReadLatchGuard page_latch, const char *data, int size) {
        VisibleTupleRef result;
        if (!page_latch || data == nullptr || size < 0) {
            return result;
        }
        result.page_latch_ = std::move(page_latch);
        result.data_ = data;
        result.size_ = size;
        result.kind_ = Kind::BorrowedCurrent;
        return result;
    }

    static VisibleTupleRef IndexKey(ReadPageGuard page_guard, const char *data, int size) {
        VisibleTupleRef result = Borrowed(std::move(page_guard), data, size);
        if (result) {
            result.kind_ = Kind::IndexKeyView;
        }
        return result;
    }

    // The owning IxScan keeps its current leaf guard until nextTuple(). The
    // caller must consume this view before advancing that scan.
    static VisibleTupleRef IndexKeyBorrowed(const char *data, int size) {
        VisibleTupleRef result;
        if (data == nullptr || size < 0) {
            return result;
        }
        result.data_ = data;
        result.size_ = size;
        result.kind_ = Kind::IndexKeyView;
        return result;
    }

    static VisibleTupleRef Owned(std::unique_ptr<RmRecord> record) {
        VisibleTupleRef result;
        if (record == nullptr) {
            return result;
        }
        result.owner_ = std::move(record);
        result.data_ = result.owner_->data;
        result.size_ = result.owner_->size;
        result.kind_ = Kind::OwnedHistorical;
        return result;
    }

    explicit operator bool() const noexcept { return kind_ != Kind::Empty && data_ != nullptr; }
    Kind kind() const noexcept { return kind_; }
    bool borrowed() const noexcept { return kind_ == Kind::BorrowedCurrent || kind_ == Kind::IndexKeyView; }
    bool index_key() const noexcept { return kind_ == Kind::IndexKeyView; }
    bool owned() const noexcept { return kind_ == Kind::OwnedHistorical; }
    const char *data() const noexcept { return data_; }
    int size() const noexcept { return size_; }

   private:
    Kind kind_ = Kind::Empty;
    ReadPageGuard page_guard_;
    PageReadLatchGuard page_latch_;
    std::shared_lock<std::shared_mutex> record_guard_;
    std::unique_ptr<RmRecord> owner_;
    const char *data_ = nullptr;
    int size_ = 0;
};
