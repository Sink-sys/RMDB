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
#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cassert>
#include <atomic>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "disk_manager.h"
#include "errors.h"
#include "page.h"
#include "page_guard.h"
#include "common/types.h"
#include "replacer/lru_replacer.h"
#include "replacer/replacer.h"

class LogManager;

enum class BufferAccessClass : rmdb::u8 {
    Default = 0,
    BulkRead,
    ColdWrite,
    IndexBuild,
    Hot
};

// Heap pages persist an ARIES pageLSN in their first eight bytes.  Derived
// index pages start with IxPageHdr at byte zero and must never be interpreted
// through the generic pageLSN API.
enum class PageWalPolicy : rmdb::u8 {
    WalProtected = 0,
    DerivedIndex
};

struct BufferAccessStrategy {
    explicit BufferAccessStrategy(BufferAccessClass access_class = BufferAccessClass::Default,
                                  size_t ring_size = 0)
        : cls(access_class), ring(ring_size, INVALID_FRAME_ID) {}

    BufferAccessClass cls{BufferAccessClass::Default};
    std::vector<frame_id_t> ring;
    size_t hand{0};
};

class BufferPoolManager {
   private:
    size_t pool_size_;  // 缓冲池可容纳的页帧数量。
    std::unique_ptr<Page[]> pages_;  // 由 BufferPoolManager 独占的连续页帧数组。
    struct alignas(64) PageTableShard {
        std::mutex latch;
        std::unordered_map<PageId, frame_id_t, PageIdHash> table;
    };
    std::unique_ptr<PageTableShard[]> page_table_shards_;
    size_t page_table_shard_count_{1};
    std::list<frame_id_t> free_list_;   // 空闲帧编号的链表
    DiskManager *disk_manager_;
    LogManager *log_manager_{nullptr};
    std::unique_ptr<Replacer> replacer_;  // 由 BufferPoolManager 独占的页帧替换器。
    std::mutex free_list_latch_;
    std::mutex slow_path_latch_;
    struct alignas(64) PageLoadShard {
        std::mutex latch;
    };
    std::unique_ptr<PageLoadShard[]> page_load_shards_;
    std::unique_ptr<std::mutex[]> frame_latches_;
    enum class FrameEvictState : rmdb::u8 { kPinned, kPending, kInReplacer };
    std::vector<FrameEvictState> frame_evict_states_;
    std::vector<rmdb::u8> pending_evictable_queued_;
    std::vector<frame_id_t> pending_evictable_frames_;
    std::mutex pending_evictable_latch_;
    std::vector<rmdb::u8> frame_access_class_;
    std::array<std::atomic<rmdb::u8>, DiskManager::MAX_FD> fd_page_wal_policies_;
    // ARIES DPT只跟踪带有效WAL LSN的heap页；派生索引页和无日志元数据页不伪造recLSN。
    // DML finalization is a hot path, so independent heap pages must not share one
    // process-wide mutex merely to preserve their first-dirty recLSN.
    static constexpr size_t kDirtyPageTableShardCount = 64;
    struct alignas(64) DirtyPageTableShard {
        mutable std::mutex latch;
        std::unordered_map<PageId, lsn_t, PageIdHash> pages;
    };
    std::array<DirtyPageTableShard, kDirtyPageTableShardCount> dirty_page_table_shards_;

    static constexpr page_id_t kDenseFramePageLimit = 1 << 20;
    static constexpr size_t kDenseFrameChunkBits = 12;
    static constexpr size_t kDenseFrameChunkSize = 1 << kDenseFrameChunkBits;
    static constexpr size_t kDenseFrameChunkCount =
        static_cast<size_t>(kDenseFramePageLimit) / kDenseFrameChunkSize;

    struct DenseFrameChunk {
        std::array<std::atomic<frame_id_t>, kDenseFrameChunkSize> frames;

        DenseFrameChunk() {
            for (auto &frame : frames) {
                frame.store(INVALID_FRAME_ID, std::memory_order_relaxed);
            }
        }
    };

    struct DenseFdDirectory {
        std::mutex latch;
        // chunks 是供命中热路径无锁读取的发布视图；对象本身由 owned_chunks 持有。
        // owned_chunks 只记录实际创建的块，避免为 MAX_FD 个目录预留一整套 unique_ptr 数组。
        std::array<std::atomic<DenseFrameChunk *>, kDenseFrameChunkCount> chunks;
        std::vector<std::unique_ptr<DenseFrameChunk>> owned_chunks;

        DenseFdDirectory() {
            for (auto &chunk : chunks) {
                chunk.store(nullptr, std::memory_order_relaxed);
            }
        }

        DenseFdDirectory(const DenseFdDirectory &) = delete;
        DenseFdDirectory &operator=(const DenseFdDirectory &) = delete;

        ~DenseFdDirectory() = default;
    };

    std::unique_ptr<DenseFdDirectory[]> dense_frame_dirs_;

   public:
    BufferPoolManager(size_t pool_size, DiskManager *disk_manager)
        : pool_size_(pool_size),
          pages_(std::make_unique<Page[]>(pool_size)),
          disk_manager_(disk_manager),
          replacer_(std::make_unique<LRUReplacer>(pool_size)) {
        page_table_shard_count_ = pool_size_ < 64 ? 1 : 64;
        page_table_shards_ = std::make_unique<PageTableShard[]>(page_table_shard_count_);
        page_load_shards_ = std::make_unique<PageLoadShard[]>(page_table_shard_count_);
        dense_frame_dirs_ = std::make_unique<DenseFdDirectory[]>(DiskManager::MAX_FD);
        frame_latches_ = std::make_unique<std::mutex[]>(pool_size_);
        frame_evict_states_.assign(pool_size_, FrameEvictState::kPinned);
        pending_evictable_queued_.assign(pool_size_, 0);
        frame_access_class_.assign(pool_size_, static_cast<rmdb::u8>(BufferAccessClass::Default));
        for (auto &policy : fd_page_wal_policies_) {
            policy.store(static_cast<rmdb::u8>(PageWalPolicy::WalProtected),
                         std::memory_order_relaxed);
        }
        // 初始化时，所有的page都在free_list_中
        for (size_t i = 0; i < pool_size_; ++i) {
            free_list_.emplace_back(static_cast<frame_id_t>(i));  // static_cast转换数据类型
        }
    }

    ~BufferPoolManager() = default;

    /**
     * @description: 将目标页面标记为脏页
     * @param {Page*} page 脏页
     */
    static void mark_dirty(Page* page) {
        page->is_dirty_ = true;
        ++page->dirty_epoch_;
    }

    void set_log_manager(LogManager *log_manager) { log_manager_ = log_manager; }

    void set_fd_page_wal_policy(int fd, PageWalPolicy policy);

   public:
    Page* fetch_page(PageId page_id, BufferAccessStrategy *strategy = nullptr);

    /**
     * 将页帧 pin 与读锁合并成一个只可移动的租约；租约析构前页面不能被淘汰。
     */
    ReadPageGuard fetch_page_read(PageId page_id, BufferAccessStrategy *strategy = nullptr);

    /**
     * 将页帧 pin 与写锁合并成一个只可移动的租约；析构时先释放锁，再归还 pin。
     */
    WritePageGuard fetch_page_write(PageId page_id, BufferAccessStrategy *strategy = nullptr);

    bool unpin_page(PageId page_id, bool is_dirty);

    bool unpin_page_fast(Page *page, PageId expected_page_id, bool is_dirty);

    bool flush_page(PageId page_id);

    Page* new_page(PageId* page_id, BufferAccessStrategy *strategy = nullptr);

    bool delete_page(PageId page_id);

    size_t flush_all_pages(int fd = -1);

    // 一次遍历 buffer pool，刷回属于给定文件集合的所有脏页。
    size_t flush_pages_for_fds(const std::vector<int> &fds);

    size_t flush_unpinned_pages_batch(size_t max_pages, size_t max_frames, size_t *next_frame,
                                      bool *pass_complete, bool include_derived_pages = true);

    bool set_page_lsn(PageId page_id, lsn_t page_lsn);

    bool finalize_page_write(PageId page_id, lsn_t page_lsn, bool is_dirty = true);

    bool finalize_page_write_fast(Page *page, PageId expected_page_id, lsn_t page_lsn, bool is_dirty = true);

    std::unordered_map<PageId, lsn_t, PageIdHash> snapshot_dirty_page_table() const;

   private:
    enum class VictimSource { kFreeList, kReplacer };

    struct FlushCandidate {
        PageId page_id;
        frame_id_t frame_id;
        lsn_t page_lsn;
        rmdb::u64 dirty_epoch;
    };

    size_t page_table_shard_for(const PageId &page_id) const;

    std::unordered_map<PageId, frame_id_t, PageIdHash> &page_table_for(const PageId &page_id);

    std::mutex &page_table_latch_for(const PageId &page_id);

    std::mutex &page_load_latch_for(const PageId &page_id);

    bool find_victim_page(frame_id_t* frame_id);

    bool find_victim_page(frame_id_t* frame_id, VictimSource *source);

    bool find_wal_safe_victim_page(frame_id_t *frame_id, VictimSource *source);

    frame_id_t frame_id_for_page(Page *page) const;

    Page *fetch_page_from_cache(PageId page_id, BufferAccessClass access_class);

    Page *fetch_page_from_dense(PageId page_id, BufferAccessClass access_class);

    void remember_fetch_cache(PageId page_id, frame_id_t frame_id, rmdb::u64 generation);

    DenseFrameChunk *dense_chunk_for(PageId page_id, bool create);

    frame_id_t lookup_dense_frame(PageId page_id);

    void publish_dense_frame(PageId page_id, frame_id_t frame_id);

    void clear_dense_frame(PageId page_id, frame_id_t expected_frame_id);

    void initialize_frame_for_page(Page *page, PageId page_id, bool bump_generation);

    // 调用者必须已持有 frame latch。若要改变 PageId 到 frame 的映射，还必须取得对应的
    // page-table shard latch；所有映射切换都遵守这一顺序，避免不同路径形成锁顺序环。
    Page *pin_frame_locked(PageId page_id, frame_id_t frame_id, BufferAccessClass access_class);

    void begin_frame_transition_locked(PageId old_page_id, frame_id_t frame_id);

    void begin_unmapped_frame_transition_locked(frame_id_t frame_id);

    void publish_frame_mapping_locked(PageId page_id, frame_id_t frame_id,
                                      BufferAccessClass access_class);

    void mark_frame_pinned_locked(frame_id_t frame_id);

    void mark_frame_evictable_locked(frame_id_t frame_id);

    void mark_frame_reclaimed_locked(frame_id_t frame_id);

    bool try_publish_frame_to_replacer_locked(frame_id_t frame_id);

    void reset_frame_to_free_locked(frame_id_t frame_id);

    void drain_pending_evictable();

    void update_page(Page* page, PageId new_page_id, frame_id_t new_frame_id);

    void ensure_wal_before_page_flush(Page *page);

    lsn_t page_wal_lsn(Page *page) const;

    void note_dirty_page(Page *page, PageId page_id, lsn_t rec_lsn);

    void clear_dirty_page(Page *page, PageId page_id);

    static size_t dirty_page_table_shard_for(const PageId &page_id);

    size_t flush_pages_matching_fds(const std::vector<rmdb::u8> *fd_mask);

    static BufferAccessClass access_class_from_strategy(BufferAccessStrategy *strategy);

    void set_frame_access_class(frame_id_t frame_id, BufferAccessClass access_class);

    void note_frame_access(frame_id_t frame_id, BufferAccessClass access_class);

    bool access_class_uses_ring(BufferAccessClass access_class) const;

    bool access_class_is_cold(BufferAccessClass access_class) const;

    bool try_reuse_strategy_frame(PageId page_id, BufferAccessStrategy *strategy, frame_id_t *frame_id);

    void attach_frame_to_strategy_ring(BufferAccessStrategy *strategy, frame_id_t frame_id);
};
