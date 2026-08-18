/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#pragma once

#include <utility>

#include "page.h"

class BufferPoolManager;

/**
 * A move-only read latch for a page whose pin is owned elsewhere.
 *
 * RmScan keeps the current heap page pinned while it advances.  Borrowed
 * tuple views only need to extend the page-latch lifetime, not acquire a
 * second buffer-pool pin for every tuple.
 */
class PageReadLatchGuard {
   public:
    PageReadLatchGuard() = default;
    explicit PageReadLatchGuard(Page *page) noexcept : page_(page) {
        if (page_ != nullptr) {
            page_->r_latch();
        }
    }
    PageReadLatchGuard(const PageReadLatchGuard &) = delete;
    PageReadLatchGuard &operator=(const PageReadLatchGuard &) = delete;

    PageReadLatchGuard(PageReadLatchGuard &&other) noexcept : page_(other.page_) { other.page_ = nullptr; }
    PageReadLatchGuard &operator=(PageReadLatchGuard &&other) noexcept {
        if (this != &other) {
            Drop();
            page_ = other.page_;
            other.page_ = nullptr;
        }
        return *this;
    }

    ~PageReadLatchGuard() { Drop(); }

    explicit operator bool() const noexcept { return page_ != nullptr; }
    void Drop() noexcept {
        if (page_ != nullptr) {
            page_->r_unlatch();
            page_ = nullptr;
        }
    }

   private:
    Page *page_ = nullptr;
};

/** Move-only write latch for a page whose buffer pin is owned elsewhere. */
class PageWriteLatchGuard {
   public:
    PageWriteLatchGuard() = default;
    explicit PageWriteLatchGuard(Page *page) noexcept : page_(page) {
        if (page_ != nullptr) {
            page_->w_latch();
        }
    }
    PageWriteLatchGuard(const PageWriteLatchGuard &) = delete;
    PageWriteLatchGuard &operator=(const PageWriteLatchGuard &) = delete;

    PageWriteLatchGuard(PageWriteLatchGuard &&other) noexcept : page_(other.page_) { other.page_ = nullptr; }
    PageWriteLatchGuard &operator=(PageWriteLatchGuard &&other) noexcept {
        if (this != &other) {
            Drop();
            page_ = other.page_;
            other.page_ = nullptr;
        }
        return *this;
    }

    ~PageWriteLatchGuard() { Drop(); }

    explicit operator bool() const noexcept { return page_ != nullptr; }
    void Drop() noexcept {
        if (page_ != nullptr) {
            page_->w_unlatch();
            page_ = nullptr;
        }
    }

   private:
    Page *page_ = nullptr;
};

/**
 * A move-only read lease for a buffer-pool page.
 *
 * The lease owns both the buffer pin and the page read latch.  Keeping these
 * lifetimes in one object makes it safe for execution-layer borrowed views to
 * outlive the helper that located a slot, while still allowing the page to be
 * evicted as soon as the view is destroyed.
 */
class ReadPageGuard {
   public:
    ReadPageGuard() = default;
    ReadPageGuard(const ReadPageGuard &) = delete;
    ReadPageGuard &operator=(const ReadPageGuard &) = delete;

    ReadPageGuard(ReadPageGuard &&other) noexcept { move_from(std::move(other)); }
    ReadPageGuard &operator=(ReadPageGuard &&other) noexcept {
        if (this != &other) {
            Drop();
            move_from(std::move(other));
        }
        return *this;
    }

    ~ReadPageGuard() { Drop(); }

    explicit operator bool() const noexcept { return page_ != nullptr; }
    Page *get_page() const noexcept { return page_; }
    const char *get_data() const noexcept { return page_ == nullptr ? nullptr : page_->get_data(); }
    PageId page_id() const noexcept { return page_id_; }

    void Drop() noexcept;

   private:
    friend class BufferPoolManager;
    ReadPageGuard(BufferPoolManager *manager, PageId page_id, Page *page) noexcept
        : manager_(manager), page_id_(page_id), page_(page) {}

    void move_from(ReadPageGuard &&other) noexcept {
        manager_ = other.manager_;
        page_id_ = other.page_id_;
        page_ = other.page_;
        other.manager_ = nullptr;
        other.page_ = nullptr;
        other.page_id_ = PageId{-1, INVALID_PAGE_ID};
    }

    BufferPoolManager *manager_ = nullptr;
    PageId page_id_{-1, INVALID_PAGE_ID};
    Page *page_ = nullptr;
};

/**
 * A move-only write lease for a buffer-pool page.
 *
 * A write lease is dirty only when MarkDirty() is called.  This keeps read
 * paths from accidentally extending the WAL/page-dirty boundary.
 */
class WritePageGuard {
   public:
    WritePageGuard() = default;
    WritePageGuard(const WritePageGuard &) = delete;
    WritePageGuard &operator=(const WritePageGuard &) = delete;

    WritePageGuard(WritePageGuard &&other) noexcept { move_from(std::move(other)); }
    WritePageGuard &operator=(WritePageGuard &&other) noexcept {
        if (this != &other) {
            Drop();
            move_from(std::move(other));
        }
        return *this;
    }

    ~WritePageGuard() { Drop(); }

    explicit operator bool() const noexcept { return page_ != nullptr; }
    Page *get_page() const noexcept { return page_; }
    char *get_data() const noexcept { return page_ == nullptr ? nullptr : page_->get_data(); }
    PageId page_id() const noexcept { return page_id_; }

    void MarkDirty() noexcept { dirty_ = true; }
    bool is_dirty() const noexcept { return dirty_; }
    Page *ReleaseLatchKeepPinned() noexcept;
    void Drop() noexcept;

   private:
    friend class BufferPoolManager;
    WritePageGuard(BufferPoolManager *manager, PageId page_id, Page *page) noexcept
        : manager_(manager), page_id_(page_id), page_(page) {}

    void move_from(WritePageGuard &&other) noexcept {
        manager_ = other.manager_;
        page_id_ = other.page_id_;
        page_ = other.page_;
        dirty_ = other.dirty_;
        other.manager_ = nullptr;
        other.page_ = nullptr;
        other.page_id_ = PageId{-1, INVALID_PAGE_ID};
        other.dirty_ = false;
    }

    BufferPoolManager *manager_ = nullptr;
    PageId page_id_{-1, INVALID_PAGE_ID};
    Page *page_ = nullptr;
    bool dirty_ = false;
};
