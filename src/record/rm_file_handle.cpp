/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "rm_file_handle.h"

#include <optional>

namespace {
void unpin_page_handle(BufferPoolManager *buffer_pool_manager, Page *page, bool is_dirty) {
    PageId page_id = page->get_page_id();
    if (!buffer_pool_manager->unpin_page_fast(page, page_id, is_dirty)) {
        buffer_pool_manager->unpin_page(page_id, is_dirty);
    }
}
}  // namespace

void RmFileHandle::flush_header() {
    release_insert_lanes_to_free_list();
    std::shared_lock<std::shared_mutex> header_guard(file_latch_);
    disk_manager_->write_page(fd_, RM_FILE_HDR_PAGE, reinterpret_cast<char *>(&file_hdr_), sizeof(file_hdr_));
}

void RmFileHandle::validate_record_page_no(int page_no) const {
    std::shared_lock<std::shared_mutex> guard(file_latch_);
    if (page_no <= RM_FILE_HDR_PAGE || page_no >= file_hdr_.num_pages) {
        throw PageNotExistError(disk_manager_->get_file_name(fd_), page_no);
    }
}

ReadPageGuard RmFileHandle::fetch_page_read_unchecked(int page_no, BufferAccessStrategy *strategy) const {
    ReadPageGuard guard = buffer_pool_manager_->fetch_page_read(PageId{fd_, page_no}, strategy);
    if (!guard) {
        throw InternalError("BufferPoolManager::fetch_page_read failed");
    }
    return guard;
}

WritePageGuard RmFileHandle::fetch_page_write_unchecked(int page_no, BufferAccessStrategy *strategy) const {
    WritePageGuard guard = buffer_pool_manager_->fetch_page_write(PageId{fd_, page_no}, strategy);
    if (!guard) {
        throw InternalError("BufferPoolManager::fetch_page_write failed");
    }
    return guard;
}

/**
 * @description: 获取当前表中记录号为rid的记录
 * @param {Rid&} rid 记录号，指定记录的位置
 * @param {Context*} context
 * @return {unique_ptr<RmRecord>} rid对应的记录对象指针
 */
std::unique_ptr<RmRecord> RmFileHandle::get_record(const Rid& rid, Context* context) const {
    validate_record_page_no(rid.page_no);
    ReadPageGuard page_guard = fetch_page_read_unchecked(rid.page_no);
    RmPageHandle page_handle(&file_hdr_, page_guard.get_page());
    if (rid.slot_no < 0 || rid.slot_no >= file_hdr_.num_records_per_page ||
        !Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }
    return std::make_unique<RmRecord>(file_hdr_.record_size, page_handle.get_slot(rid.slot_no));
}

bool RmFileHandle::read_record(const Rid& rid, RmRecord *record, Context* context) const {
    validate_record_page_no(rid.page_no);
    ReadPageGuard page_guard = fetch_page_read_unchecked(rid.page_no);
    RmPageHandle page_handle(&file_hdr_, page_guard.get_page());
    if (rid.slot_no < 0 || rid.slot_no >= file_hdr_.num_records_per_page ||
        !Bitmap::is_set(page_handle.bitmap, rid.slot_no) || is_pending_insert_unlocked(rid)) {
        return false;
    }
    record->ResizeAndCopy(page_handle.get_slot(rid.slot_no), file_hdr_.record_size);
    return true;
}

bool RmFileHandle::with_record_slot(const Rid& rid, const std::function<bool(const char *slot)> &fn) const {
    return with_record_slot_fast(rid, fn);
}

/**
 * @description: 在当前表中插入一条记录，不指定插入位置
 * @param {char*} buf 要插入的记录的数据
 * @param {Context*} context
 * @return {Rid} 插入的记录的记录号（位置）
 */
Rid RmFileHandle::insert_record(char* buf, Context* context, PageId *modified_page_id, bool defer_unpin,
                                Page **modified_page, BufferAccessStrategy *strategy, bool defer_visibility) {
    (void)context;
    auto &lane = insert_lane_for_current_thread();
    std::unique_lock<std::mutex> lane_guard(lane.latch);
    RmPageHandle page_handle = lane.page_no.load(std::memory_order_acquire) == RM_NO_PAGE
                                   ? claim_insert_page(lane, strategy)
                                   : fetch_page_handle_unchecked(lane.page_no.load(std::memory_order_relaxed), strategy);
    PageWriteLatchGuard page_latch(page_handle.page);
    int slot_no = Bitmap::first_bit(false, page_handle.bitmap, file_hdr_.num_records_per_page);
    if (slot_no >= file_hdr_.num_records_per_page) {
        throw InternalError("insert lane owns a full record page");
    }
    Rid rid{page_handle.page->get_page_id().page_no, slot_no};
    if (defer_visibility) {
        const rmdb::u64 key = pending_key(rid);
        auto &shard = pending_shard(key);
        std::lock_guard<std::mutex> pending_guard(shard.latch);
        if (shard.entries.insert(key).second) {
            pending_insert_count_.fetch_add(1, std::memory_order_release);
        }
    }
    Bitmap::set(page_handle.bitmap, slot_no);
    memcpy(page_handle.get_slot(slot_no), buf, file_hdr_.record_size);
    page_handle.page_hdr->num_records++;
    if (page_handle.page_hdr->num_records == file_hdr_.num_records_per_page) {
        lane.page_no.store(RM_NO_PAGE, std::memory_order_release);
    }
    if (modified_page_id != nullptr) {
        *modified_page_id = page_handle.page->get_page_id();
    }
    if (modified_page != nullptr) {
        *modified_page = page_handle.page;
    }
    page_latch.Drop();
    if (!defer_unpin) {
        unpin_page_handle(buffer_pool_manager_, page_handle.page, true);
    }
    return rid;
}

RmPageHandle RmFileHandle::claim_insert_page(InsertLane &lane, BufferAccessStrategy *strategy) {
    auto claim_free_head = [&]() -> std::optional<RmPageHandle> {
        std::unique_lock<std::shared_mutex> header_guard(file_latch_);
        while (file_hdr_.first_free_page_no != RM_NO_PAGE) {
            const page_id_t page_no = file_hdr_.first_free_page_no;
            auto page_handle = fetch_page_handle_unchecked(page_no, strategy);
            PageWriteLatchGuard page_latch(page_handle.page);
            page_id_t next_free_page_no = page_handle.page_hdr->next_free_page_no;
            if (next_free_page_no == page_no) {
                next_free_page_no = RM_NO_PAGE;
            }
            file_hdr_.first_free_page_no = next_free_page_no;
            page_handle.page_hdr->next_free_page_no = RM_NO_PAGE;
            const int free_slot = Bitmap::first_bit(false, page_handle.bitmap, file_hdr_.num_records_per_page);
            if (free_slot < file_hdr_.num_records_per_page) {
                lane.page_no.store(page_no, std::memory_order_release);
                page_latch.Drop();
                return page_handle;
            }
            page_handle.page_hdr->num_records = file_hdr_.num_records_per_page;
            page_latch.Drop();
            unpin_page_handle(buffer_pool_manager_, page_handle.page, true);
        }
        return std::nullopt;
    };

    if (auto free_page = claim_free_head(); free_page.has_value()) {
        return std::move(*free_page);
    }

    std::unique_lock<std::mutex> allocation_guard(page_allocation_latch_);
    if (auto free_page = claim_free_head(); free_page.has_value()) {
        return std::move(*free_page);
    }

    PageId page_id{fd_, INVALID_PAGE_ID};
    Page *page = buffer_pool_manager_->new_page(&page_id, strategy);
    if (page == nullptr) {
        throw InternalError("BufferPoolManager::new_page failed");
    }
    RmPageHandle page_handle(&file_hdr_, page);
    {
        PageWriteLatchGuard page_latch(page);
        page_handle.page_hdr->num_records = 0;
        page_handle.page_hdr->next_free_page_no = RM_NO_PAGE;
        Bitmap::init(page_handle.bitmap, file_hdr_.bitmap_size);
    }
    {
        std::unique_lock<std::shared_mutex> header_guard(file_latch_);
        if (page_id.page_no != file_hdr_.num_pages) {
            throw InternalError("record page allocation is not contiguous");
        }
        ++file_hdr_.num_pages;
        lane.page_no.store(page_id.page_no, std::memory_order_release);
    }
    return page_handle;
}

void RmFileHandle::release_insert_lanes_to_free_list() {
    std::array<std::unique_lock<std::mutex>, kInsertLaneCount> lane_guards;
    for (size_t i = 0; i < kInsertLaneCount; ++i) {
        lane_guards[i] = std::unique_lock<std::mutex>(insert_lanes_[i].latch);
    }
    std::unique_lock<std::shared_mutex> header_guard(file_latch_);
    for (auto &lane : insert_lanes_) {
        const page_id_t page_no = lane.page_no.exchange(RM_NO_PAGE, std::memory_order_acq_rel);
        if (page_no == RM_NO_PAGE) {
            continue;
        }
        auto page_handle = fetch_page_handle_unchecked(page_no);
        PageWriteLatchGuard page_latch(page_handle.page);
        if (page_handle.page_hdr->num_records >= file_hdr_.num_records_per_page) {
            throw InternalError("full insert lane page cannot rejoin the free list");
        }
        page_handle.page_hdr->next_free_page_no = file_hdr_.first_free_page_no;
        file_hdr_.first_free_page_no = page_no;
        page_latch.Drop();
        unpin_page_handle(buffer_pool_manager_, page_handle.page, true);
    }
}

void RmFileHandle::publish_pending_insert(const Rid &rid) {
    const rmdb::u64 key = pending_key(rid);
    auto &shard = pending_shard(key);
    std::lock_guard<std::mutex> guard(shard.latch);
    if (shard.entries.erase(key) != 0) {
        pending_insert_count_.fetch_sub(1, std::memory_order_release);
    }
}

void RmFileHandle::rollback_pending_insert(const Rid &rid) {
    std::unique_lock<std::shared_mutex> header_guard(file_latch_);
    if (rid.page_no <= RM_FILE_HDR_PAGE || rid.page_no >= file_hdr_.num_pages) {
        return;
    }
    WritePageGuard page_guard = fetch_page_write_unchecked(rid.page_no);
    RmPageHandle page_handle(&file_hdr_, page_guard.get_page());
    const rmdb::u64 key = pending_key(rid);
    auto &shard = pending_shard(key);
    std::unique_lock<std::mutex> pending_guard(shard.latch);
    if (shard.entries.find(key) == shard.entries.end()) {
        return;
    }
    if (rid.slot_no < 0 || rid.slot_no >= file_hdr_.num_records_per_page ||
        !Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        shard.entries.erase(key);
        pending_insert_count_.fetch_sub(1, std::memory_order_release);
        return;
    }
    bool was_full = page_handle.page_hdr->num_records == file_hdr_.num_records_per_page;
    Bitmap::reset(page_handle.bitmap, rid.slot_no);
    --page_handle.page_hdr->num_records;
    if (was_full) {
        page_handle.page_hdr->next_free_page_no = file_hdr_.first_free_page_no;
        file_hdr_.first_free_page_no = rid.page_no;
    }
    shard.entries.erase(key);
    pending_insert_count_.fetch_sub(1, std::memory_order_release);
    page_guard.MarkDirty();
}

/**
 * @description: 在当前表中的指定位置插入一条记录
 * @param {Rid&} rid 要插入记录的位置
 * @param {char*} buf 要插入记录的数据
 */
void RmFileHandle::insert_record(const Rid& rid, char* buf, PageId *modified_page_id,
                                 bool defer_unpin, Page **modified_page) {
    std::unique_lock<std::shared_mutex> header_guard(file_latch_);
    if (rid.page_no <= RM_FILE_HDR_PAGE || rid.page_no >= file_hdr_.num_pages) {
        throw PageNotExistError(disk_manager_->get_file_name(fd_), rid.page_no);
    }
    WritePageGuard page_guard = fetch_page_write_unchecked(rid.page_no);
    RmPageHandle page_handle(&file_hdr_, page_guard.get_page());
    if (rid.slot_no < 0 || rid.slot_no >= file_hdr_.num_records_per_page ||
        Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }
    bool was_full = page_handle.page_hdr->num_records == file_hdr_.num_records_per_page;
    Bitmap::set(page_handle.bitmap, rid.slot_no);
    memcpy(page_handle.get_slot(rid.slot_no), buf, file_hdr_.record_size);
    page_handle.page_hdr->num_records++;
    if (!was_full && page_handle.page_hdr->num_records == file_hdr_.num_records_per_page) {
        if (file_hdr_.first_free_page_no == rid.page_no) {
            file_hdr_.first_free_page_no = page_handle.page_hdr->next_free_page_no;
            page_handle.page_hdr->next_free_page_no = RM_NO_PAGE;
        }
    }
    if (modified_page_id != nullptr) {
        *modified_page_id = page_guard.page_id();
    }
    if (defer_unpin) {
        Page *page = page_guard.ReleaseLatchKeepPinned();
        if (modified_page != nullptr) {
            *modified_page = page;
        }
    } else {
        if (modified_page != nullptr) {
            *modified_page = page_guard.get_page();
        }
        page_guard.MarkDirty();
    }
}

/**
 * @description: 删除记录文件中记录号为rid的记录
 * @param {Rid&} rid 要删除的记录的记录号（位置）
 * @param {Context*} context
 */
void RmFileHandle::delete_record(const Rid& rid, Context* context, PageId *modified_page_id, bool defer_unpin,
                                 Page **modified_page) {
    validate_record_page_no(rid.page_no);
    WritePageGuard page_guard = fetch_page_write_unchecked(rid.page_no);
    RmPageHandle page_handle(&file_hdr_, page_guard.get_page());

    auto validate_slot = [&]() {
        if (rid.slot_no < 0 || rid.slot_no >= file_hdr_.num_records_per_page ||
            !Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
            throw RecordNotFoundError(rid.page_no, rid.slot_no);
        }
    };
    validate_slot();

    std::unique_lock<std::shared_mutex> header_guard;
    if (page_handle.page_hdr->num_records == file_hdr_.num_records_per_page) {
        page_guard.Drop();
        header_guard = std::unique_lock<std::shared_mutex>(file_latch_);
        page_guard = fetch_page_write_unchecked(rid.page_no);
        page_handle = RmPageHandle(&file_hdr_, page_guard.get_page());
        validate_slot();
    }

    bool was_full = page_handle.page_hdr->num_records == file_hdr_.num_records_per_page;
    Bitmap::reset(page_handle.bitmap, rid.slot_no);
    --page_handle.page_hdr->num_records;
    if (was_full) {
        release_page_handle(page_handle);
    }
    PageId page_id = page_guard.page_id();
    if (modified_page_id != nullptr) {
        *modified_page_id = page_id;
    }
    if (defer_unpin) {
        Page *page = page_guard.ReleaseLatchKeepPinned();
        if (modified_page != nullptr) {
            *modified_page = page;
        }
    } else {
        if (modified_page != nullptr) {
            *modified_page = page_guard.get_page();
        }
        page_guard.MarkDirty();
        page_guard.Drop();
    }
}

RmFileHandle::ConditionalDeleteResult RmFileHandle::delete_record_if(
    const Rid &rid, const std::function<bool()> &condition, PageId *modified_page_id,
    bool defer_unpin, Page **modified_page) {
    validate_record_page_no(rid.page_no);
    WritePageGuard page_guard = fetch_page_write_unchecked(rid.page_no);
    RmPageHandle page_handle(&file_hdr_, page_guard.get_page());

    auto slot_exists = [&]() {
        return rid.slot_no >= 0 && rid.slot_no < file_hdr_.num_records_per_page &&
               Bitmap::is_set(page_handle.bitmap, rid.slot_no);
    };
    if (!slot_exists()) {
        return ConditionalDeleteResult::RECORD_MISSING;
    }

    std::unique_lock<std::shared_mutex> header_guard;
    if (page_handle.page_hdr->num_records == file_hdr_.num_records_per_page) {
        page_guard.Drop();
        header_guard = std::unique_lock<std::shared_mutex>(file_latch_);
        page_guard = fetch_page_write_unchecked(rid.page_no);
        page_handle = RmPageHandle(&file_hdr_, page_guard.get_page());
        if (!slot_exists()) {
            return ConditionalDeleteResult::RECORD_MISSING;
        }
    }

    // The condition runs below the heap-page writer latch. Visibility readers
    // use heap-page reader -> version-page reader, so this is the same lock
    // order and cannot deadlock. A successful condition clears the matching
    // tombstone and its undo head before this latch allows RID reuse.
    if (!condition()) {
        return ConditionalDeleteResult::CONDITION_FAILED;
    }

    const bool was_full = page_handle.page_hdr->num_records == file_hdr_.num_records_per_page;
    Bitmap::reset(page_handle.bitmap, rid.slot_no);
    --page_handle.page_hdr->num_records;
    if (was_full) {
        release_page_handle(page_handle);
    }
    if (modified_page_id != nullptr) {
        *modified_page_id = page_guard.page_id();
    }
    if (defer_unpin) {
        Page *page = page_guard.ReleaseLatchKeepPinned();
        if (modified_page != nullptr) {
            *modified_page = page;
        }
    } else {
        if (modified_page != nullptr) {
            *modified_page = page_guard.get_page();
        }
        page_guard.MarkDirty();
        page_guard.Drop();
    }
    return ConditionalDeleteResult::DELETED;
}


/**
 * @description: 更新记录文件中记录号为rid的记录
 * @param {Rid&} rid 要更新的记录的记录号（位置）
 * @param {char*} buf 新记录的数据
 * @param {Context*} context
 */
void RmFileHandle::update_record(const Rid& rid, char* buf, Context* context, PageId *modified_page_id,
                                 bool defer_unpin, Page **modified_page) {
    validate_record_page_no(rid.page_no);
    WritePageGuard page_guard = fetch_page_write_unchecked(rid.page_no);
    RmPageHandle page_handle(&file_hdr_, page_guard.get_page());
    if (rid.slot_no < 0 || rid.slot_no >= file_hdr_.num_records_per_page ||
        !Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }
    memcpy(page_handle.get_slot(rid.slot_no), buf, file_hdr_.record_size);
    if (modified_page_id != nullptr) {
        *modified_page_id = page_guard.page_id();
    }
    if (defer_unpin) {
        Page *page = page_guard.ReleaseLatchKeepPinned();
        if (modified_page != nullptr) {
            *modified_page = page;
        }
    } else {
        if (modified_page != nullptr) {
            *modified_page = page_guard.get_page();
        }
        page_guard.MarkDirty();
    }
}

/**
 * 以下函数为辅助函数，仅提供参考，可以选择完成如下函数，也可以删除如下函数，在单元测试中不涉及如下函数接口的直接调用
*/
/**
 * @description: 获取指定页面的页面句柄
 * @param {int} page_no 页面号
 * @return {RmPageHandle} 指定页面的句柄
 */
RmPageHandle RmFileHandle::fetch_page_handle(int page_no, BufferAccessStrategy *strategy) const {
    validate_record_page_no(page_no);
    return fetch_page_handle_unchecked(page_no, strategy);
}

RmPageHandle RmFileHandle::fetch_page_handle_unchecked(int page_no, BufferAccessStrategy *strategy) const {
    Page *page = buffer_pool_manager_->fetch_page(PageId{fd_, page_no}, strategy);
    if (page == nullptr) {
        throw InternalError("BufferPoolManager::fetch_page failed");
    }
    return RmPageHandle(&file_hdr_, page);
}

/**
 * @description: 创建一个新的page handle
 * @return {RmPageHandle} 新的PageHandle
 */
RmPageHandle RmFileHandle::create_new_page_handle(BufferAccessStrategy *strategy) {
    std::unique_lock<std::shared_mutex> header_guard(file_latch_);
    return create_new_page_handle_unlocked(strategy);
}

RmPageHandle RmFileHandle::create_new_page_handle_unlocked(BufferAccessStrategy *strategy) {
    PageId page_id{fd_, INVALID_PAGE_ID};
    Page *page = buffer_pool_manager_->new_page(&page_id, strategy);
    if (page == nullptr) {
        throw InternalError("BufferPoolManager::new_page failed");
    }
    auto page_handle = RmPageHandle(&file_hdr_, page);
    PageWriteLatchGuard page_latch(page);
    page_handle.page_hdr->num_records = 0;
    page_handle.page_hdr->next_free_page_no = file_hdr_.first_free_page_no;
    Bitmap::init(page_handle.bitmap, file_hdr_.bitmap_size);
    file_hdr_.first_free_page_no = page_id.page_no;
    ++file_hdr_.num_pages;
    return page_handle;
}

/**
 * @brief 创建或获取一个空闲的page handle
 *
 * @return RmPageHandle 返回生成的空闲page handle
 * @note pin the page, remember to unpin it outside!
 */
RmPageHandle RmFileHandle::create_page_handle(BufferAccessStrategy *strategy) {
    while (file_hdr_.first_free_page_no != RM_NO_PAGE) {
        page_id_t page_no = file_hdr_.first_free_page_no;
        auto page_handle = fetch_page_handle_unchecked(page_no, strategy);
        if (Bitmap::first_bit(false, page_handle.bitmap, file_hdr_.num_records_per_page) <
            file_hdr_.num_records_per_page) {
            return page_handle;
        }

        page_id_t next_free_page_no = page_handle.page_hdr->next_free_page_no;
        if (next_free_page_no == page_no) {
            next_free_page_no = RM_NO_PAGE;
        }
        file_hdr_.first_free_page_no = next_free_page_no;
        page_handle.page_hdr->num_records = file_hdr_.num_records_per_page;
        page_handle.page_hdr->next_free_page_no = RM_NO_PAGE;
        unpin_page_handle(buffer_pool_manager_, page_handle.page, true);
    }
    return create_new_page_handle_unlocked(strategy);
}

/**
 * @description: 当一个页面从没有空闲空间的状态变为有空闲空间状态时，更新文件头和页头中空闲页面相关的元数据
 */
void RmFileHandle::release_page_handle(RmPageHandle&page_handle) {
    page_handle.page_hdr->next_free_page_no = file_hdr_.first_free_page_no;
    file_hdr_.first_free_page_no = page_handle.page->get_page_id().page_no;
}

void RmFileHandle::rebuild_file_hdr_from_disk() {
    std::unique_lock<std::shared_mutex> guard(file_latch_);
    rmdb::i64 file_size = disk_manager_->get_file_size(disk_manager_->get_file_name(fd_));
    int actual_num_pages =
        file_size <= 0 ? 1 : static_cast<int>((file_size + PAGE_SIZE - 1) / PAGE_SIZE);
    if (actual_num_pages < 1) {
        actual_num_pages = 1;
    }

    file_hdr_.num_pages = actual_num_pages;
    file_hdr_.first_free_page_no = RM_NO_PAGE;

    // Recovery only needs the record count and the persisted free-list link
    // from each page. Scan backwards so the successor of every free page is
    // already known, keeping memory bounded independently of the table size.
    constexpr size_t kScanBytes = 4 * 1024 * 1024;
    constexpr int kPagesPerScan = static_cast<int>(kScanBytes / PAGE_SIZE);
    std::vector<char> scan_buffer(kScanBytes);
    std::vector<rmdb::u8> dirty_pages(kPagesPerScan, 0);
    page_id_t next_free_page_no = RM_NO_PAGE;

    int chunk_end = actual_num_pages;
    while (chunk_end > RM_FIRST_RECORD_PAGE) {
        const int first_page = std::max(RM_FIRST_RECORD_PAGE, chunk_end - kPagesPerScan);
        const int page_count = chunk_end - first_page;
        const size_t bytes_to_read = static_cast<size_t>(page_count) * PAGE_SIZE;
        const rmdb::i64 file_offset = static_cast<rmdb::i64>(first_page) * PAGE_SIZE;
        const size_t bytes_read =
            disk_manager_->read_file_range(fd_, file_offset, scan_buffer.data(), bytes_to_read);
        if (bytes_read != bytes_to_read) {
            throw InternalError("short heap read in rebuild_file_hdr_from_disk");
        }
        std::fill(dirty_pages.begin(), dirty_pages.begin() + page_count, 0);
        for (int index = page_count - 1; index >= 0; --index) {
            char *page_data = scan_buffer.data() + static_cast<size_t>(index) * PAGE_SIZE;
            char *page_hdr_data = page_data + Page::OFFSET_PAGE_HDR;
            const int num_records =
                rmdb::load_unaligned<int>(page_hdr_data + offsetof(RmPageHdr, num_records));
            if (num_records < file_hdr_.num_records_per_page) {
                const page_id_t persisted_next = rmdb::load_unaligned<page_id_t>(page_hdr_data);
                if (persisted_next != next_free_page_no) {
                    rmdb::store_unaligned<page_id_t>(page_hdr_data, next_free_page_no);
                    dirty_pages[index] = 1;
                }
                next_free_page_no = first_page + index;
            }
        }

        // Neighboring stale pages are written with one contiguous pwrite. A
        // failed batch leaves a partially repaired but still recoverable list;
        // the next startup derives the chain again from record counts.
        int index = 0;
        while (index < page_count) {
            while (index < page_count && dirty_pages[index] == 0) {
                ++index;
            }
            const int run_begin = index;
            while (index < page_count && dirty_pages[index] != 0) {
                ++index;
            }
            if (run_begin < index) {
                const int run_pages = index - run_begin;
                if (run_pages == 1) {
                    const rmdb::i64 link_offset =
                        static_cast<rmdb::i64>(first_page + run_begin) * PAGE_SIZE +
                        Page::OFFSET_PAGE_HDR + offsetof(RmPageHdr, next_free_page_no);
                    disk_manager_->write_file_range(
                        fd_, link_offset,
                        scan_buffer.data() + static_cast<size_t>(run_begin) * PAGE_SIZE +
                            Page::OFFSET_PAGE_HDR + offsetof(RmPageHdr, next_free_page_no),
                        sizeof(page_id_t));
                } else {
                    disk_manager_->write_pages_batch(
                        fd_, first_page + run_begin,
                        scan_buffer.data() + static_cast<size_t>(run_begin) * PAGE_SIZE,
                        static_cast<size_t>(run_pages));
                }
            }
        }
        chunk_end = first_page;
    }
    file_hdr_.first_free_page_no = next_free_page_no;

    disk_manager_->set_fd2pageno(fd_, file_hdr_.num_pages);
}
