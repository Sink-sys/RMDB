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

#include <assert.h>

#include <atomic>
#include <array>
#include <functional>
#include <exception>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <utility>
#include <vector>
#include <unordered_set>

#include "bitmap.h"
#include "common/context.h"
#include "common/types.h"
#include "rm_defs.h"

class RmManager;
class RmRecordPageCursor;

/* 对表数据文件中的页面进行封装 */
struct RmPageHandle {
    const RmFileHdr *file_hdr;  // 当前页面所在文件的文件头指针
    Page *page;                 // 页面的实际数据，包括页面存储的数据、元信息等
    RmPageHdr *page_hdr;        // page->data的第一部分，存储页面元信息，指针指向首地址，长度为sizeof(RmPageHdr)
    char *bitmap;               // page->data的第二部分，存储页面的bitmap，指针指向首地址，长度为file_hdr->bitmap_size
    char *slots;                // page->data的第三部分，存储表的记录，指针指向首地址，每个slot的长度为file_hdr->record_size

    RmPageHandle(const RmFileHdr *fhdr_, Page *page_) : file_hdr(fhdr_), page(page_) {
        page_hdr = reinterpret_cast<RmPageHdr *>(page->get_data() + page->OFFSET_PAGE_HDR);
        bitmap = page->get_data() + sizeof(RmPageHdr) + page->OFFSET_PAGE_HDR;
        slots = bitmap + file_hdr->bitmap_size;
    }

    // 返回指定slot_no的slot存储收地址
    char* get_slot(int slot_no) const {
        return slots + slot_no * file_hdr->record_size;  // slots的首地址 + slot个数 * 每个slot的大小(每个record的大小)
    }
};

/* 每个RmFileHandle对应一个表的数据文件，里面有多个page，每个page的数据封装在RmPageHandle中 */
class RmFileHandle {
    friend class RmScan;
    friend class RmManager;
    friend class RmRecordPageCursor;

   private:
    DiskManager *disk_manager_;
    BufferPoolManager *buffer_pool_manager_;
    int fd_;        // 打开文件后产生的文件句柄
    RmFileHdr file_hdr_;    // 文件头，维护当前表文件的元数据
    // Protects file_hdr_ and the on-page free-list links. Tuple bytes and
    // record-page bitmaps are protected by the corresponding page latch.
    mutable std::shared_mutex file_latch_;
    static constexpr size_t kInsertLaneCount = 32;
    struct alignas(64) InsertLane {
        std::mutex latch;
        std::atomic<page_id_t> page_no{RM_NO_PAGE};
    };
    std::array<InsertLane, kInsertLaneCount> insert_lanes_;
    // New physical pages must be installed into the contiguous file header in
    // allocation order. Existing lane-owned pages insert independently.
    std::mutex page_allocation_latch_;
    static constexpr size_t kPendingInsertShardCount = 64;
    struct alignas(64) PendingInsertShard {
        mutable std::mutex latch;
        std::unordered_set<rmdb::u64> entries;
    };
    std::array<PendingInsertShard, kPendingInsertShardCount> pending_insert_shards_;
    std::atomic<rmdb::u32> pending_insert_count_{0};

    static rmdb::u64 pending_key(const Rid &rid) {
        return (static_cast<rmdb::u64>(static_cast<rmdb::u32>(rid.page_no)) << 32) |
               static_cast<rmdb::u32>(rid.slot_no);
    }
    static size_t pending_shard_index(rmdb::u64 key) {
        return static_cast<size_t>((key ^ (key >> 32)) & (kPendingInsertShardCount - 1));
    }
    PendingInsertShard &pending_shard(rmdb::u64 key) {
        return pending_insert_shards_[pending_shard_index(key)];
    }
    const PendingInsertShard &pending_shard(rmdb::u64 key) const {
        return pending_insert_shards_[pending_shard_index(key)];
    }
    bool is_pending_insert_unlocked(const Rid &rid) const {
        if (pending_insert_count_.load(std::memory_order_acquire) == 0) {
            return false;
        }
        const rmdb::u64 key = pending_key(rid);
        const auto &shard = pending_shard(key);
        std::lock_guard<std::mutex> guard(shard.latch);
        return shard.entries.find(key) != shard.entries.end();
    }

   public:
    enum class ConditionalDeleteResult { RECORD_MISSING, CONDITION_FAILED, DELETED };

    RmFileHandle(DiskManager *disk_manager, BufferPoolManager *buffer_pool_manager, int fd)
        : disk_manager_(disk_manager), buffer_pool_manager_(buffer_pool_manager), fd_(fd) {
        // 注意：这里从磁盘中读出文件描述符为fd的文件的file_hdr，读到内存中
        // 这里实际就是初始化file_hdr，只不过是从磁盘中读出进行初始化
        // init file_hdr_
        disk_manager_->read_page(fd, RM_FILE_HDR_PAGE, (char *)&file_hdr_, sizeof(file_hdr_));
        // disk_manager管理的fd对应的文件中，设置从file_hdr_.num_pages开始分配page_no
        disk_manager_->set_fd2pageno(fd, file_hdr_.num_pages);
    }

    RmFileHdr get_file_hdr() const {
        std::shared_lock<std::shared_mutex> guard(file_latch_);
        return file_hdr_;
    }
    int num_pages_snapshot() const {
        std::shared_lock<std::shared_mutex> guard(file_latch_);
        return file_hdr_.num_pages;
    }
    int GetFd() const { return fd_; }

    std::shared_lock<std::shared_mutex> acquire_shared_latch() const {
        return std::shared_lock<std::shared_mutex>(file_latch_);
    }

    void flush_header();

    void flush() {
        flush_header();
        buffer_pool_manager_->flush_all_pages(fd_);
    }

    void rebuild_file_hdr_from_disk();

    /* 判断指定位置上是否已经存在一条记录，通过Bitmap来判断 */
    bool is_record(const Rid &rid) const {
        validate_record_page_no(rid.page_no);
        ReadPageGuard page_guard = fetch_page_read_unchecked(rid.page_no);
        RmPageHandle page_handle(&file_hdr_, page_guard.get_page());
        bool exists = rid.slot_no >= 0 && rid.slot_no < file_hdr_.num_records_per_page &&
                      Bitmap::is_set(page_handle.bitmap, rid.slot_no) && !is_pending_insert_unlocked(rid);
        return exists;  // page的slot_no位置上是否有record
    }

    std::unique_ptr<RmRecord> get_record(const Rid &rid, Context *context) const;

    bool read_record(const Rid &rid, RmRecord *record, Context *context) const;

    template <typename Fn>
    bool with_record_slot_fast(const Rid &rid, Fn &&fn) const {
        validate_record_page_no(rid.page_no);
        ReadPageGuard page_guard = fetch_page_read_unchecked(rid.page_no);
        RmPageHandle page_handle(&file_hdr_, page_guard.get_page());
        if (rid.slot_no < 0 || rid.slot_no >= file_hdr_.num_records_per_page ||
            !Bitmap::is_set(page_handle.bitmap, rid.slot_no) || is_pending_insert_unlocked(rid)) {
            return false;
        }
        return static_cast<bool>(std::forward<Fn>(fn)(page_handle.get_slot(rid.slot_no)));
    }

    bool with_record_slot(const Rid &rid, const std::function<bool(const char *slot)> &fn) const;

    Rid insert_record(char *buf, Context *context, PageId *modified_page_id = nullptr, bool defer_unpin = false,
                      Page **modified_page = nullptr, BufferAccessStrategy *strategy = nullptr,
                      bool defer_visibility = false);

    void publish_pending_insert(const Rid &rid);
    void rollback_pending_insert(const Rid &rid);

    template <typename FillRecordFn>
    size_t bulk_insert_records(FillRecordFn &&fill_record, BufferAccessStrategy *strategy = nullptr) {
        release_insert_lanes_to_free_list();
        std::unique_lock<std::shared_mutex> guard(file_latch_);
        const RmFileHdr initial_file_hdr = file_hdr_;
        std::vector<char> record(static_cast<size_t>(file_hdr_.record_size));
        Page *page = nullptr;
        PageWriteLatchGuard page_latch;
        RmPageHdr *page_hdr = nullptr;
        char *bitmap = nullptr;
        char *slots = nullptr;
        int next_slot = file_hdr_.num_records_per_page;

        auto release_current_page = [&]() {
            if (page != nullptr) {
                page_latch.Drop();
                unpin_page_handle_fast(page, true);
                page = nullptr;
                page_hdr = nullptr;
                bitmap = nullptr;
                slots = nullptr;
                next_slot = file_hdr_.num_records_per_page;
            }
        };

        auto pin_free_page = [&]() {
            auto page_handle = create_page_handle(strategy);
            page = page_handle.page;
            page_latch = PageWriteLatchGuard(page);
            page_hdr = page_handle.page_hdr;
            bitmap = page_handle.bitmap;
            slots = page_handle.slots;
            next_slot = Bitmap::first_bit(false, bitmap, file_hdr_.num_records_per_page);
            assert(next_slot < file_hdr_.num_records_per_page);
        };

        size_t inserted = 0;
        try {
            while (fill_record(record.data())) {
                if (page == nullptr) {
                    pin_free_page();
                }
                char *slot = slots + next_slot * file_hdr_.record_size;
                Bitmap::set(bitmap, next_slot);
                memcpy(slot, record.data(), file_hdr_.record_size);
                ++page_hdr->num_records;
                ++inserted;

                if (page_hdr->num_records == file_hdr_.num_records_per_page) {
                    file_hdr_.first_free_page_no = page_hdr->next_free_page_no;
                    page_hdr->next_free_page_no = RM_NO_PAGE;
                    release_current_page();
                } else {
                    next_slot = Bitmap::next_bit(false, bitmap, file_hdr_.num_records_per_page, next_slot);
                    assert(next_slot < file_hdr_.num_records_per_page);
                }
            }
            release_current_page();
        } catch (...) {
            std::exception_ptr load_error = std::current_exception();
            release_current_page();
            if (initial_file_hdr.num_pages == RM_FIRST_RECORD_PAGE) {
                for (page_id_t page_no = RM_FIRST_RECORD_PAGE; page_no < file_hdr_.num_pages; ++page_no) {
                    if (!buffer_pool_manager_->delete_page(PageId{fd_, page_no})) {
                        throw InternalError("failed to discard bulk load page during rollback");
                    }
                }
                disk_manager_->truncate_file(fd_, static_cast<rmdb::i64>(initial_file_hdr.num_pages) * PAGE_SIZE);
                file_hdr_ = initial_file_hdr;
                disk_manager_->set_fd2pageno(fd_, file_hdr_.num_pages);
                // file_latch_ is already held exclusively by this bulk-load rollback.
                // flush_header() would try to acquire it again and abort the server.
                disk_manager_->write_page(fd_, RM_FILE_HDR_PAGE, reinterpret_cast<char *>(&file_hdr_),
                                          sizeof(file_hdr_));
                disk_manager_->sync_file(fd_);
            }
            std::rethrow_exception(load_error);
        }
        return inserted;
    }

    void reset_empty_bulk_load() {
        release_insert_lanes_to_free_list();
        std::unique_lock<std::shared_mutex> guard(file_latch_);
        for (page_id_t page_no = RM_FIRST_RECORD_PAGE; page_no < file_hdr_.num_pages; ++page_no) {
            if (!buffer_pool_manager_->delete_page(PageId{fd_, page_no})) {
                throw InternalError("failed to discard bulk load page during rollback");
            }
        }
        disk_manager_->truncate_file(fd_, static_cast<rmdb::i64>(RM_FIRST_RECORD_PAGE) * PAGE_SIZE);
        file_hdr_.num_pages = RM_FIRST_RECORD_PAGE;
        file_hdr_.first_free_page_no = RM_NO_PAGE;
        disk_manager_->set_fd2pageno(fd_, file_hdr_.num_pages);
        // file_latch_ is already held exclusively by this rollback path.
        disk_manager_->write_page(fd_, RM_FILE_HDR_PAGE, reinterpret_cast<char *>(&file_hdr_), sizeof(file_hdr_));
        disk_manager_->sync_file(fd_);
    }

    void insert_record(const Rid &rid, char *buf, PageId *modified_page_id = nullptr,
                       bool defer_unpin = false, Page **modified_page = nullptr);

    void delete_record(const Rid &rid, Context *context, PageId *modified_page_id = nullptr,
                       bool defer_unpin = false, Page **modified_page = nullptr);

    /**
     * Delete a slot only after condition succeeds while the heap-page write
     * latch is held. The condition may lock and update external tuple metadata;
     * this gives MVCC GC one atomic heap-page -> version-metadata lock order and
     * prevents RID reuse between validation and physical deletion.
     */
    ConditionalDeleteResult delete_record_if(
        const Rid &rid, const std::function<bool()> &condition,
        PageId *modified_page_id = nullptr, bool defer_unpin = false,
        Page **modified_page = nullptr);

    void update_record(const Rid &rid, char *buf, Context *context, PageId *modified_page_id = nullptr,
                       bool defer_unpin = false, Page **modified_page = nullptr);

    RmPageHandle create_new_page_handle(BufferAccessStrategy *strategy = nullptr);

    RmPageHandle fetch_page_handle(int page_no, BufferAccessStrategy *strategy = nullptr) const;

    BufferPoolManager *get_buffer_pool_manager() const { return buffer_pool_manager_; }

   private:
    void validate_record_page_no(int page_no) const;

    ReadPageGuard fetch_page_read_unchecked(int page_no, BufferAccessStrategy *strategy = nullptr) const;

    WritePageGuard fetch_page_write_unchecked(int page_no, BufferAccessStrategy *strategy = nullptr) const;

    RmPageHandle fetch_page_handle_unchecked(int page_no, BufferAccessStrategy *strategy = nullptr) const;

    RmPageHandle create_new_page_handle_unlocked(BufferAccessStrategy *strategy = nullptr);

    RmPageHandle create_page_handle(BufferAccessStrategy *strategy = nullptr);

    InsertLane &insert_lane_for_current_thread() {
        const size_t hash = std::hash<std::thread::id>{}(std::this_thread::get_id());
        return insert_lanes_[hash & (kInsertLaneCount - 1)];
    }

    RmPageHandle claim_insert_page(InsertLane &lane, BufferAccessStrategy *strategy);

    void release_insert_lanes_to_free_list();

    void release_page_handle(RmPageHandle &page_handle);

    void unpin_page_handle_fast(Page *page, bool is_dirty) const {
        PageId page_id = page->get_page_id();
        if (!buffer_pool_manager_->unpin_page_fast(page, page_id, is_dirty)) {
            buffer_pool_manager_->unpin_page(page_id, is_dirty);
        }
    }
};

class RmRecordPageCursor {
   public:
    RmRecordPageCursor() = default;

    explicit RmRecordPageCursor(const RmFileHandle *file_handle) : file_handle_(file_handle) {}

    RmRecordPageCursor(const RmRecordPageCursor &) = delete;
    RmRecordPageCursor &operator=(const RmRecordPageCursor &) = delete;
    RmRecordPageCursor(RmRecordPageCursor &&) = delete;
    RmRecordPageCursor &operator=(RmRecordPageCursor &&) = delete;

    ~RmRecordPageCursor() { reset(); }

    void bind(const RmFileHandle *file_handle) {
        if (file_handle_ == file_handle) {
            return;
        }
        reset();
        file_handle_ = file_handle;
    }

    void reset() {
        current_guard_.Drop();
        current_page_ = nullptr;
        current_page_no_ = RM_NO_PAGE;
        file_hdr_ = nullptr;
        bitmap_ = nullptr;
        slots_ = nullptr;
    }

    const char *get_slot(const Rid &rid) {
        if (file_handle_ == nullptr) {
            return nullptr;
        }
        if (rid.page_no != current_page_no_) {
            reset();
            file_handle_->validate_record_page_no(rid.page_no);
            current_guard_ = file_handle_->fetch_page_read_unchecked(rid.page_no);
            current_page_ = current_guard_.get_page();
            RmPageHandle handle(&file_handle_->file_hdr_, current_page_);
            current_page_no_ = rid.page_no;
            file_hdr_ = handle.file_hdr;
            bitmap_ = handle.bitmap;
            slots_ = handle.slots;
        }
        if (rid.slot_no < 0 || rid.slot_no >= file_hdr_->num_records_per_page ||
            !Bitmap::is_set(bitmap_, rid.slot_no)) {
            return nullptr;
        }
        return slots_ + rid.slot_no * file_hdr_->record_size;
    }

    bool read_record(const Rid &rid, RmRecord *record) {
        const char *slot = get_slot(rid);
        if (slot == nullptr) {
            return false;
        }
        record->ResizeAndCopy(slot, file_hdr_->record_size);
        return true;
    }

    template <typename Fn>
    bool with_slot(const Rid &rid, Fn &&fn) {
        const char *slot = get_slot(rid);
        if (slot == nullptr) {
            return false;
        }
        return static_cast<bool>(std::forward<Fn>(fn)(slot));
    }

   private:
    const RmFileHandle *file_handle_ = nullptr;
    ReadPageGuard current_guard_;
    Page *current_page_ = nullptr;
    int current_page_no_ = RM_NO_PAGE;
    const RmFileHdr *file_hdr_ = nullptr;
    char *bitmap_ = nullptr;
    char *slots_ = nullptr;
};
