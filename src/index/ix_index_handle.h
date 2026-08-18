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

#include "ix_defs.h"
#include "common/types.h"
#include "transaction/transaction.h"
#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <memory>
#include <shared_mutex>
#include <string>
#include <utility>

class IxScan;
class IxIndexHandle;

enum class Operation { FIND = 0, INSERT, DELETE };  // 三种操作：查找、插入、删除

enum class LatchMode { None = 0, Shared, Exclusive };

// 逻辑 key 边界:索引范围扫描的起止以索引 key 表达,而非物理 Iid。
// 物理 (page_no, slot_no) 在叶页插入/删除/分裂后会失效;key 边界在每次
// 进入叶页(shared latch 保护)时重新计算,不受槽位移动影响。
//   None       = 无边界:下限从首叶开始,上限扫到索引末尾
//   LowerBound = 第一个 key >= bound 的槽位
//   UpperBound = 第一个 key > bound 的槽位
enum class IxBoundMode { None, LowerBound, UpperBound };

struct IxScanBounds {
    std::string lower_key;
    std::string upper_key;
    IxBoundMode lower_mode = IxBoundMode::None;
    IxBoundMode upper_mode = IxBoundMode::None;
};

enum class IxInsertResult { kInserted, kDuplicate };

struct IxInsertOutcome {
    IxInsertResult result;
    page_id_t page_no;
};

// Compact bulk-load storage: fixed-width keys live in one byte arena, RIDs in
// one dense array, and sorting permutes 32-bit ordinals instead of heap-owning
// std::string objects. The prepared order is consumed without copying keys.
class IxBulkLoadEntries {
   public:
    explicit IxBulkLoadEntries(size_t key_size);

    void reserve(size_t entry_count);
    void append(const char *key, const Rid &rid);
    size_t size() const { return rids_.size(); }
    bool empty() const { return rids_.empty(); }
    void release();

   private:
    friend class IxIndexHandle;

    const char *insertion_key(size_t storage_index) const;
    const char *prepared_key(size_t sorted_index) const;
    Rid prepared_rid(size_t sorted_index) const;

    size_t key_size_;
    std::vector<char> keys_;
    std::vector<Rid> rids_;
    std::vector<rmdb::u32> order_;
};

inline int ix_compare(const char *a, const char *b, ColType type, int col_len) {
    switch (type) {
        case TYPE_INT: {
            int ia = rmdb::load_unaligned<int>(a);
            int ib = rmdb::load_unaligned<int>(b);
            return (ia < ib) ? -1 : ((ia > ib) ? 1 : 0);
        }
        case TYPE_FLOAT: {
            float fa = rmdb::load_unaligned<float>(a);
            float fb = rmdb::load_unaligned<float>(b);
            return (fa < fb) ? -1 : ((fa > fb) ? 1 : 0);
        }
        case TYPE_STRING:
            return memcmp(a, b, col_len);
        default:
            throw InternalError("Unexpected data type");
    }
}

inline int ix_compare(const char* a, const char* b, const std::vector<ColType>& col_types, const std::vector<int>& col_lens) {
    int offset = 0;
    for(size_t i = 0; i < col_types.size(); ++i) {
        int res = ix_compare(a + offset, b + offset, col_types[i], col_lens[i]);
        if(res != 0) return res;
        offset += col_lens[i];
    }
    return 0;
}

/* 管理B+树中的每个节点 */
class IxNodeHandle {
    friend class IxIndexHandle;
    friend class IxScan;

   private:
    const IxFileHdr *file_hdr;      // 节点所在文件的头部信息
    Page *page;                     // 存储节点的页面
    IxPageHdr *page_hdr;            // page->data的第一部分，指针指向首地址，长度为sizeof(IxPageHdr)
    char *keys;                     // page->data的第二部分，指针指向首地址，长度为file_hdr->keys_size，每个key的长度为file_hdr->col_len
    char *rid_bytes;                // page->data的第三部分，不保证按Rid对齐

   public:
    IxNodeHandle() = default;

    IxNodeHandle(const IxFileHdr *file_hdr_, Page *page_) : file_hdr(file_hdr_), page(page_) {
        page_hdr = reinterpret_cast<IxPageHdr *>(page->get_data());
        keys = page->get_data() + sizeof(IxPageHdr);
        rid_bytes = keys + file_hdr->keys_size_;
    }

    int get_size() const { return page_hdr->num_key; }

    void set_size(int size) { page_hdr->num_key = size; }

    int get_physical_key_capacity() const { return file_hdr->btree_order_ + 1; }

    int get_max_size() const { return file_hdr->btree_order_; }

    int get_min_size() const { return get_max_size() / 2; }

    int key_at(int i) const { return rmdb::load_unaligned<int>(get_key(i)); }

    /* 得到第i个孩子结点的page_no */
    page_id_t value_at(int i) const { return get_rid(i).page_no; }

    page_id_t get_page_no() const { return page->get_page_id().page_no; }

    PageId get_page_id() const { return page->get_page_id(); }

    page_id_t get_next_leaf() const { return page_hdr->next_leaf; }

    page_id_t right_link() const { return page_hdr->next_leaf; }

    page_id_t get_prev_leaf() const { return page_hdr->prev_leaf; }

    page_id_t get_parent_page_no() const { return page_hdr->parent; }

    bool is_leaf_page() const { return page_hdr->is_leaf; }

    bool is_root_page() const { return get_parent_page_no() == INVALID_PAGE_ID; }

    bool is_tombstone() const {
        return page_hdr->next_free_page_no == IX_PAGE_TOMBSTONE ||
               page_hdr->next_free_page_no == IX_PAGE_DRAINING;
    }

    bool is_draining() const { return page_hdr->next_free_page_no == IX_PAGE_DRAINING; }

    bool is_free_page() const { return page_hdr->parent == IX_PAGE_FREE; }

    void mark_tombstone() { page_hdr->next_free_page_no = IX_PAGE_TOMBSTONE; }

    void mark_draining() { page_hdr->next_free_page_no = IX_PAGE_DRAINING; }

    void clear_tombstone() { page_hdr->next_free_page_no = IX_NO_PAGE; }

    void mark_free_page(page_id_t next_free_page_no) {
        page_hdr->next_free_page_no = next_free_page_no;
        page_hdr->parent = IX_PAGE_FREE;
        page_hdr->num_key = 0;
        page_hdr->is_leaf = true;
        page_hdr->prev_leaf = IX_NO_PAGE;
        page_hdr->next_leaf = IX_NO_PAGE;
    }

    void set_next_leaf(page_id_t page_no) { page_hdr->next_leaf = page_no; }

    void set_right_link(page_id_t page_no) { page_hdr->next_leaf = page_no; }

    void set_prev_leaf(page_id_t page_no) { page_hdr->prev_leaf = page_no; }

    void set_parent_page_no(page_id_t parent) { page_hdr->parent = parent; }

    char *get_key(int key_idx) const { return keys + key_idx * file_hdr->col_tot_len_; }

    char *high_key() const {
        assert(get_max_size() < get_physical_key_capacity());
        return get_key(get_max_size());
    }

    char *get_rid_bytes(int rid_idx) const { return rid_bytes + rid_idx * sizeof(Rid); }

    Rid get_rid(int rid_idx) const { return rmdb::load_unaligned<Rid>(get_rid_bytes(rid_idx)); }

    void set_key(int key_idx, const char *key) { memcpy(keys + key_idx * file_hdr->col_tot_len_, key, file_hdr->col_tot_len_); }

    void set_high_key(const char *key) { memcpy(high_key(), key, file_hdr->col_tot_len_); }

    void set_rid(int rid_idx, const Rid &rid) { rmdb::store_unaligned(get_rid_bytes(rid_idx), rid); }

    int lower_bound(const char *target) const;

    int upper_bound(const char *target) const;

    bool has_high_key() const;

    bool key_belongs_here(const char *key) const;

    void assert_valid() const;


    int find_child_index(page_id_t child_page_no) const {
        for (int rid_idx = 0; rid_idx < page_hdr->num_key; rid_idx++) {
            if (get_rid(rid_idx).page_no == child_page_no) {
                return rid_idx;
            }
        }
        return -1;
    }

    void insert_pairs(int pos, const char *key, const void *rid_data, int n);

    page_id_t internal_lookup(const char *key) const;

    bool leaf_lookup(const char *key, Rid *value);

    int insert(const char *key, const Rid &value);

    // 用于在结点中的指定位置插入单个键值对
    void insert_pair(int pos, const char *key, const Rid &rid) { insert_pairs(pos, key, &rid, 1); }

    void erase_pair(int pos);

    int remove(const char *key);

    /**
     * @brief 由parent调用，寻找child，返回child在parent中的rid_idx∈[0,page_hdr->num_key)
     * @param child
     * @return int
     */
    int find_child(const IxNodeHandle *child) const {
        assert(child != nullptr);
        int rid_idx = find_child_index(child->get_page_no());
        assert(rid_idx >= 0);
        return rid_idx;
    }
};

class IxNodeHandleGuard {
   public:
    IxNodeHandleGuard() = default;

    IxNodeHandleGuard(BufferPoolManager *buffer_pool_manager, const IxFileHdr *file_hdr, Page *page,
                      LatchMode latch_mode = LatchMode::None)
        : buffer_pool_manager_(buffer_pool_manager), page_(page), node_(file_hdr, page) {
        latch(latch_mode);
    }

    ~IxNodeHandleGuard() { reset(); }

    IxNodeHandleGuard(const IxNodeHandleGuard &) = delete;
    IxNodeHandleGuard &operator=(const IxNodeHandleGuard &) = delete;

    IxNodeHandleGuard(IxNodeHandleGuard &&other) noexcept { move_from(std::move(other)); }

    IxNodeHandleGuard &operator=(IxNodeHandleGuard &&other) noexcept {
        if (this != &other) {
            reset();
            move_from(std::move(other));
        }
        return *this;
    }

    explicit operator bool() const { return page_ != nullptr; }

    IxNodeHandle *operator->() {
        assert(page_ != nullptr);
        return &node_;
    }

    const IxNodeHandle *operator->() const {
        assert(page_ != nullptr);
        return &node_;
    }

    IxNodeHandle &get() {
        assert(page_ != nullptr);
        return node_;
    }

    const IxNodeHandle &get() const {
        assert(page_ != nullptr);
        return node_;
    }

    void mark_dirty() {
        assert(page_ != nullptr);
        dirty_ = true;
    }

    void unlock() {
        if (page_ == nullptr || latch_mode_ == LatchMode::None) {
            return;
        }
        if (latch_mode_ == LatchMode::Shared) {
            page_->r_unlatch();
        } else {
            page_->w_unlatch();
        }
        latch_mode_ = LatchMode::None;
    }

    LatchMode latch_mode() const { return latch_mode_; }

    PageId page_id() const {
        assert(page_ != nullptr);
        return page_->get_page_id();
    }

    void reset() {
        if (page_ == nullptr) {
            return;
        }
        if (latch_mode_ != LatchMode::None) {
            node_.assert_valid();
        }
        unlock();
        PageId page_id = page_->get_page_id();
        bool ok = buffer_pool_manager_->unpin_page_fast(page_, page_id, dirty_);
        if (!ok) {
            ok = buffer_pool_manager_->unpin_page(page_id, dirty_);
        }
        assert(ok);
        clear();
    }

   private:
    void latch(LatchMode latch_mode) {
        assert(page_ != nullptr || latch_mode == LatchMode::None);
        if (latch_mode == LatchMode::Shared) {
            page_->r_latch();
        } else if (latch_mode == LatchMode::Exclusive) {
            page_->w_latch();
        }
        latch_mode_ = latch_mode;
    }

    void move_from(IxNodeHandleGuard &&other) {
        buffer_pool_manager_ = other.buffer_pool_manager_;
        page_ = other.page_;
        node_ = other.node_;
        dirty_ = other.dirty_;
        latch_mode_ = other.latch_mode_;
        other.clear();
    }

    void clear() {
        buffer_pool_manager_ = nullptr;
        page_ = nullptr;
        node_ = IxNodeHandle();
        dirty_ = false;
        latch_mode_ = LatchMode::None;
    }

    BufferPoolManager *buffer_pool_manager_ = nullptr;
    Page *page_ = nullptr;
    IxNodeHandle node_;
    bool dirty_ = false;
    LatchMode latch_mode_ = LatchMode::None;
};

class IxReadGuard {
   public:
    IxReadGuard() = default;
    explicit IxReadGuard(const IxIndexHandle *index);
    ~IxReadGuard();

    IxReadGuard(const IxReadGuard &) = delete;
    IxReadGuard &operator=(const IxReadGuard &) = delete;

    IxReadGuard(IxReadGuard &&other) noexcept { move_from(std::move(other)); }
    IxReadGuard &operator=(IxReadGuard &&other) noexcept {
        if (this != &other) {
            reset();
            move_from(std::move(other));
        }
        return *this;
    }

    explicit operator bool() const { return index_ != nullptr; }
    void reset();

   private:
    void move_from(IxReadGuard &&other) {
        index_ = other.index_;
        reader_ticket_ = other.reader_ticket_;
        other.index_ = nullptr;
        other.reader_ticket_ = UINT16_MAX;
    }

    const IxIndexHandle *index_{nullptr};
    rmdb::u16 reader_ticket_{UINT16_MAX};
};

/* B+树 */
class IxIndexHandle {
    friend class IxScan;
    friend class IxManager;
    friend class IxReadGuard;

   private:
    DiskManager *disk_manager_;
    BufferPoolManager *buffer_pool_manager_;
    int fd_;                                    // 存储B+树的文件
    IxFileHdr* file_hdr_;                       // 存了root_page，但其初始化为2（第0页存FILE_HDR_PAGE，第1页存LEAF_HEADER_PAGE）
    mutable std::shared_mutex tree_latch_;
    mutable std::mutex reclamation_latch_;
    mutable std::condition_variable reclamation_cv_;
    static constexpr size_t kReaderShardCount = 64;
    struct alignas(64) ReaderShard {
        std::array<std::atomic<rmdb::u32>, 2> active{};
    };
    mutable std::array<ReaderShard, kReaderShardCount> reader_shards_{};
    mutable std::atomic<rmdb::u64> reader_epoch_{0};
    mutable std::atomic<rmdb::u32> reclamation_waiters_{0};
    // 根页号原子缓存:root 只在分裂时变化,读路径每次树下降都查
    // root_page_no();缓存避免每次下降的 tree_latch_ 共享锁。分裂瞬间
    // 读到旧 root 也安全(旧 root 变为新 root 的子节点,下降仍可达叶子)。
    mutable std::atomic<page_id_t> cached_root_{IX_NO_PAGE};

   public:
    IxIndexHandle(DiskManager *disk_manager, BufferPoolManager *buffer_pool_manager, int fd);

    ~IxIndexHandle() { delete file_hdr_; }

    void flush_header() {
        char *data = new char[file_hdr_->tot_len_];
        file_hdr_->serialize(data);
        disk_manager_->write_page(fd_, IX_FILE_HDR_PAGE, data, file_hdr_->tot_len_);
        delete[] data;
    }

    void flush() {
        flush_header();
        buffer_pool_manager_->flush_all_pages(fd_);
    }

    // for search
    // leaf_hint(可选): batch 级 leaf 页提示。入参为上次点查的 leaf 页号,命中
    // (tombstone 链 + leaf + 左边界 + key_belongs_here 全校验)则免去 root->leaf
    // 下降;未命中正常下降并回写新页号。每次使用都重新 fetch + 校验,页被并发
    // split/merge/复用后自动失效,正确性等价于无 hint 路径。
    bool get_value(const char *key, std::vector<Rid> *result, Transaction *transaction,
                   page_id_t *leaf_hint = nullptr);
    bool get_unique_value(const char *key, Rid *result, Transaction *transaction,
                          page_id_t *leaf_hint = nullptr);

    void get_all_rids(std::vector<Rid> *result) const;

    void get_prefix_values(const char *prefix_key, int prefix_len, std::vector<Rid> *result) const;

    // for insert
    IxInsertOutcome insert_entry(const char *key, const Rid &value, Transaction *transaction);

    // Bulk-load: build entire B+ tree from index entries.
    void prepare_bulk_load(IxBulkLoadEntries &entries, bool enforce_unique = true) const;
    void bulk_load(IxBulkLoadEntries &entries, bool enforce_unique = true);
    void bulk_load_prepared(const IxBulkLoadEntries &entries);

    IxNodeHandleGuard split(IxNodeHandleGuard &node);

    void insert_into_parent(IxNodeHandleGuard &old_node, const char *key, IxNodeHandleGuard &new_node,
                            Transaction *transaction, std::vector<page_id_t> *ancestors = nullptr);

    // for delete
    bool delete_entry(const char *key, Transaction *transaction);

    bool predecessor(const char *upper_key, std::string *key, Rid *rid, bool inclusive = false) const;

    // 比较两个索引 key(全 key 长度),返回 <0 / 0 / >0。供执行层在扫描中
    // 对逻辑边界做复核。
    int compare_keys(const char *a, const char *b) const { return file_hdr_->compare_key(a, b); }

    std::unique_ptr<IxScan> create_scan(const IxScanBounds &bounds) const;

    IxReadGuard make_read_guard() const;

   private:
    // 按 lower key 定位起始叶页 + 槽位,返回 shared-latched 叶页 guard。
    // strict=false → 第一个 key >= target;strict=true → 第一个 key > target。
    // 索引为空或 key 超出所有 key 时返回空 guard。seek 到叶页后 guard 不释放,
    // 由 IxScan 持有,避免"seek 之后、扫描建立之前"的并发窗口。
    std::pair<IxNodeHandleGuard, int> seek_lower_internal(const char *key, bool strict) const;

    // 辅助函数
    page_id_t root_page_no() const;

    void update_root_page_no(page_id_t root);

    void update_leaf_bounds(page_id_t first_leaf, page_id_t last_leaf);

    void update_last_leaf_page_no(page_id_t last_leaf);

    bool is_empty() const { return root_page_no() == IX_NO_PAGE; }

    rmdb::u16 enter_read() const;

    void leave_read(rmdb::u16 reader_ticket) const;


    static size_t reader_shard_for_current_thread();

    // batch leaf hint 尝试: fetch hint 页 -> tombstone 链 -> leaf && 左边界 &&
    // key_belongs_here(high_key) 全校验。任一校验失败返回空 guard,调用方正常
    // 下降。页被并发 split/merge/复用后自动失效。
    IxNodeHandleGuard try_hinted_leaf(page_id_t hint_page, const char *key) const;

    IxNodeHandleGuard fetch_node_guard(int page_no, LatchMode latch_mode = LatchMode::None,
                                       BufferAccessStrategy *strategy = nullptr) const;

    IxNodeHandleGuard create_node_guard(LatchMode latch_mode = LatchMode::None,
                                        BufferAccessStrategy *strategy = nullptr);

    IxNodeHandleGuard pop_free_node_guard(LatchMode latch_mode);


    IxNodeHandleGuard fetch_parent_for_child(page_id_t parent_page_no, page_id_t child_page_no) const;

    IxNodeHandleGuard find_leaf_page_for_write(const char *key, std::vector<page_id_t> *ancestors) const;




    page_id_t safe_next_leaf(const IxNodeHandle &node) const;

    IxNodeHandleGuard fetch_next_live_leaf(page_id_t page_no, LatchMode latch_mode) const;

    std::pair<IxNodeHandleGuard, bool> find_leaf_page_guard(const char *key, Operation operation, Transaction *transaction,
                                                            bool find_first = false,
                                                            std::vector<page_id_t> *ancestors = nullptr) const;

    // for index test
    Rid get_rid(const Iid &iid) const;

    void collect_all_rids(std::vector<Rid> *result) const;
};
