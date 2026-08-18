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

#include "common/config.h"
#include "common/types.h"

#include <shared_mutex>

#include "common/binary.h"

/**
 * @description: 存储层每个Page的id的声明
 */
struct PageId {
    int fd;  //  Page所在的磁盘文件开启后的文件描述符, 来定位打开的文件在内存中的位置
    page_id_t page_no = INVALID_PAGE_ID;

    friend bool operator==(const PageId &x, const PageId &y) { return x.fd == y.fd && x.page_no == y.page_no; }
    bool operator<(const PageId& x) const {
        if(fd < x.fd) return true;
        return page_no < x.page_no;
    }

    std::string toString() {
        return "{fd: " + std::to_string(fd) + " page_no: " + std::to_string(page_no) + "}"; 
    }

    inline rmdb::i64 Get() const {
        return (static_cast<rmdb::i64>(fd << 16) | page_no);
    }
};

// PageId的自定义哈希算法, 用于构建unordered_map<PageId, frame_id_t, PageIdHash>
struct PageIdHash {
    size_t operator()(const PageId &x) const { return (x.fd << 16) | x.page_no; }
};

template <>
struct std::hash<PageId> {
    size_t operator()(const PageId &obj) const { return std::hash<rmdb::i64>()(obj.Get()); }
};

/**
 * @description: Page类声明, Page是RMDB数据块的单位、是负责数据操作Record模块的操作对象，
 * Page对象在磁盘上有文件存储, 若在Buffer中则有帧偏移, 并非特指Buffer或Disk上的数据
 */
class alignas(64) Page {
    friend class BufferPoolManager;

   public:
    
    Page() { reset_memory(); }

    ~Page() = default;

    PageId get_page_id() const { return id_; }

    inline char *get_data() { return data_; }

    bool is_dirty() const { return is_dirty_; }

    rmdb::u64 dirty_epoch() const { return dirty_epoch_; }

    static constexpr size_t OFFSET_PAGE_START = 0;
    static constexpr size_t OFFSET_LSN = 0;
    static constexpr size_t OFFSET_PAGE_HDR = sizeof(lsn_t);

    inline lsn_t get_page_lsn() { return rmdb::load_unaligned<lsn_t>(get_data() + OFFSET_LSN); }

    inline void set_page_lsn(lsn_t page_lsn) { memcpy(get_data() + OFFSET_LSN, &page_lsn, sizeof(lsn_t)); }

    void r_latch() { page_latch_.lock_shared(); }

    void r_unlatch() { page_latch_.unlock_shared(); }

    void w_latch() { page_latch_.lock(); }

    void w_unlatch() { page_latch_.unlock(); }

   private:
    void reset_memory() { memset(data_, OFFSET_PAGE_START, PAGE_SIZE); }  // 将data_的PAGE_SIZE个字节填充为0

    /** Frequently accessed frame metadata stays together in the first cache line. */
    PageId id_;
    bool is_dirty_ = false;
    std::atomic<int> pin_count_{0};
    std::atomic<rmdb::u64> generation_{0};
    rmdb::u64 dirty_epoch_{0};
    lsn_t dpt_rec_lsn_{INVALID_LSN};
    std::atomic<bool> is_replacing_{false};
    std::shared_mutex page_latch_;

    /** The persisted page payload starts on its own cache-line boundary. */
    alignas(64) char data_[PAGE_SIZE] = {};
};

static_assert(alignof(Page) >= 64, "buffer frames must be cache-line aligned");
