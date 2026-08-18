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

#include <utility>

#include "common/types.h"
#include "rm_file_handle.h"

class RmScan : public RecScan {
    const RmFileHandle *file_handle_;
    Rid rid_;
    Page *current_page_ = nullptr;
    int current_page_no_ = RM_NO_PAGE;
    RmPageHdr *current_page_hdr_ = nullptr;
    const char *current_bitmap_ = nullptr;
    const char *current_slots_ = nullptr;
    const char *current_slot_ = nullptr;
    rmdb::u64 current_bitmap_word_ = 0;
    int current_bitmap_word_base_slot_ = 0;
    int scan_page_limit_ = RM_FIRST_RECORD_PAGE;
    BufferAccessStrategy access_strategy_;
    bool use_access_strategy_ = false;

    void release_current_page();
    void reset_current_bitmap_word();
    bool pin_next_non_empty_page(int page_no);
    bool next_slot_in_current_page(int after_slot_no, int *slot_no);

public:
    RmScan(const RmFileHandle *file_handle, BufferAccessClass access_class = BufferAccessClass::Default,
           size_t ring_size = 0);

    ~RmScan() override;

    void next() override;

    bool is_end() const override;

    Rid rid() const override;

    bool acquire_current_slot_lease(PageReadLatchGuard *guard, const char **slot) const;

    template <typename Fn>
    bool with_current_slot(Fn &&fn) const {
        if (is_end() || current_page_ == nullptr || current_slot_ == nullptr || rid_.page_no != current_page_no_) {
            return false;
        }
        PageReadLatchGuard guard(current_page_);
        if (rid_.slot_no < 0 || rid_.slot_no >= file_handle_->file_hdr_.num_records_per_page ||
            !Bitmap::is_set(current_bitmap_, rid_.slot_no) || file_handle_->is_pending_insert_unlocked(rid_)) {
            return false;
        }
        return static_cast<bool>(std::forward<Fn>(fn)(current_slot_));
    }
};
