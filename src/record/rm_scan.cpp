/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "rm_scan.h"

/**
 * @brief 初始化file_handle和rid
 * @param file_handle
 */
namespace {
constexpr size_t kDefaultRmScanRingSize = 128;
}

RmScan::RmScan(const RmFileHandle *file_handle, BufferAccessClass access_class, size_t ring_size)
    : file_handle_(file_handle),
      access_strategy_(access_class,
                       access_class == BufferAccessClass::Default
                           ? 0
                           : (ring_size == 0 ? kDefaultRmScanRingSize : ring_size)),
      use_access_strategy_(access_class != BufferAccessClass::Default) {
    scan_page_limit_ = file_handle_->num_pages_snapshot();
    rid_ = {RM_FIRST_RECORD_PAGE, -1};
    next();
}

RmScan::~RmScan() { release_current_page(); }

void RmScan::release_current_page() {
    if (current_page_ != nullptr) {
        PageId page_id = current_page_->get_page_id();
        if (!file_handle_->buffer_pool_manager_->unpin_page_fast(current_page_, page_id, false)) {
            file_handle_->buffer_pool_manager_->unpin_page(page_id, false);
        }
        current_page_ = nullptr;
    }
    current_page_no_ = RM_NO_PAGE;
    current_page_hdr_ = nullptr;
    current_bitmap_ = nullptr;
    current_slots_ = nullptr;
    current_slot_ = nullptr;
    reset_current_bitmap_word();
}

void RmScan::reset_current_bitmap_word() {
    current_bitmap_word_ = 0;
    current_bitmap_word_base_slot_ = 0;
}

bool RmScan::pin_next_non_empty_page(int page_no) {
    release_current_page();
    while (page_no < scan_page_limit_) {
        auto page_handle =
            file_handle_->fetch_page_handle_unchecked(page_no, use_access_strategy_ ? &access_strategy_ : nullptr);
        PageReadLatchGuard page_guard(page_handle.page);
        if (page_handle.page_hdr->num_records > 0) {
            current_page_ = page_handle.page;
            current_page_no_ = page_no;
            current_page_hdr_ = page_handle.page_hdr;
            current_bitmap_ = page_handle.bitmap;
            current_slots_ = page_handle.slots;
            reset_current_bitmap_word();
            return true;
        }
        page_guard.Drop();
        PageId page_id = page_handle.page->get_page_id();
        if (!file_handle_->buffer_pool_manager_->unpin_page_fast(page_handle.page, page_id, false)) {
            file_handle_->buffer_pool_manager_->unpin_page(page_id, false);
        }
        ++page_no;
    }
    return false;
}

bool RmScan::next_slot_in_current_page(int after_slot_no, int *slot_no) {
    int max_slots = file_handle_->file_hdr_.num_records_per_page;
    int min_slot = std::max(0, after_slot_no + 1);
    while (min_slot < max_slots) {
        while (current_bitmap_word_ != 0) {
            int bit = static_cast<int>(__builtin_ctzll(current_bitmap_word_));
            current_bitmap_word_ &= current_bitmap_word_ - 1;
            int candidate = current_bitmap_word_base_slot_ + bit;
            Rid candidate_rid{current_page_no_, candidate};
            if (candidate >= min_slot && candidate < max_slots && Bitmap::is_set(current_bitmap_, candidate) &&
                !file_handle_->is_pending_insert_unlocked(candidate_rid)) {
                *slot_no = candidate;
                return true;
            }
            min_slot = std::max(min_slot, candidate + 1);
        }

        int word_base = (min_slot / 64) * 64;
        current_bitmap_word_base_slot_ = word_base;
        current_bitmap_word_ = Bitmap::aligned_word_bits(true, current_bitmap_, max_slots, word_base);
        int offset = min_slot - word_base;
        if (offset > 0) {
            current_bitmap_word_ &= ~((rmdb::u64{1} << offset) - 1);
        }
        if (current_bitmap_word_ == 0) {
            min_slot = word_base + 64;
        }
    }
    return false;
}

/**
 * @brief 找到文件中下一个存放了记录的位置
 */
void RmScan::next() {
    int page_no = rid_.page_no;
    int slot_no = rid_.slot_no;

    while (page_no < scan_page_limit_) {
        if (current_page_ == nullptr || current_page_no_ != page_no) {
            if (!pin_next_non_empty_page(page_no)) {
                break;
            }
            page_no = current_page_no_;
            slot_no = -1;
        }

        PageReadLatchGuard page_guard(current_page_);
        if (current_page_hdr_->num_records == 0) {
            ++page_no;
            slot_no = -1;
            page_guard.Drop();
            release_current_page();
            continue;
        }

        int next_slot = file_handle_->file_hdr_.num_records_per_page;
        if (next_slot_in_current_page(slot_no, &next_slot)) {
            rid_ = {page_no, next_slot};
            current_slot_ = current_slots_ + next_slot * file_handle_->file_hdr_.record_size;
            return;
        }

        ++page_no;
        slot_no = -1;
        page_guard.Drop();
        release_current_page();
    }

    release_current_page();
    rid_ = {scan_page_limit_, -1};
}

/**
 * @brief ​ 判断是否到达文件末尾
 */
bool RmScan::is_end() const { return rid_.page_no >= scan_page_limit_; }

/**
 * @brief RmScan内部存放的rid
 */
Rid RmScan::rid() const { return rid_; }

bool RmScan::acquire_current_slot_lease(PageReadLatchGuard *guard, const char **slot) const {
    if (guard == nullptr || slot == nullptr || is_end() || current_page_ == nullptr || current_slot_ == nullptr ||
        rid_.page_no != current_page_no_) {
        return false;
    }
    PageReadLatchGuard page_guard(current_page_);
    if (rid_.slot_no < 0 || rid_.slot_no >= file_handle_->file_hdr_.num_records_per_page ||
        !Bitmap::is_set(current_bitmap_, rid_.slot_no) || file_handle_->is_pending_insert_unlocked(rid_)) {
        return false;
    }
    *slot = current_slot_;
    *guard = std::move(page_guard);
    return true;
}
