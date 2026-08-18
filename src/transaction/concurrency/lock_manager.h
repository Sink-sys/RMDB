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

#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "transaction/transaction.h"

class LockManager {
    enum class LockMode { SHARED, EXLUCSIVE };

    /* 用于标识加锁队列中排他性最强的锁类型，例如加锁队列中有SHARED和EXLUSIVE两个加锁操作，则该队列的锁模式为X */
    enum class GroupLockMode { NON_LOCK, S, X };

    /* 事务的加锁申请 */
    class LockRequest {
    public:
        LockRequest(txn_id_t txn_id, LockMode lock_mode)
            : txn_id_(txn_id), lock_mode_(lock_mode), granted_(false) {}

        txn_id_t txn_id_;   // 申请加锁的事务ID
        LockMode lock_mode_;    // 事务申请加锁的类型
        bool granted_;          // 该事务是否已经被赋予锁
        std::condition_variable cv_;
    };

    /* 数据项上的加锁队列 */
    class LockRequestQueue {
    public:
        std::list<LockRequest> request_queue_;  // 加锁队列
        txn_id_t fast_exclusive_owner_ = INVALID_TXN_ID;
        GroupLockMode group_lock_mode_ = GroupLockMode::NON_LOCK;   // 加锁队列的锁模式
    };

public:
    explicit LockManager(
        std::chrono::milliseconds pre_snapshot_wait = std::chrono::milliseconds(12),
        size_t pre_snapshot_max_waiters = 3)
        : pre_snapshot_wait_(pre_snapshot_wait),
          pre_snapshot_max_waiters_(pre_snapshot_max_waiters) {}

    ~LockManager() {}

    bool lock_shared_on_record(Transaction* txn, const Rid& rid, int tab_fd);

    bool lock_exclusive_on_record(Transaction* txn, const Rid& rid, int tab_fd);

    // 在事务选取 SI 快照前获取逻辑写键。每键只保留有界的定向继任窗口，
    // 更深竞争或短等待超时时回退到原生 SI，不形成 convoy。无论是否获得
    // 准入，后续记录锁、版本可见性及写写冲突检查都保持不变。
    bool acquire_pre_snapshot_writes(Transaction *txn,
                                     const std::vector<lock_data_key_t> &keys);
    void release_pre_snapshot_writes(Transaction *txn);
    // 在仍持有真实记录锁时，按每个逻辑写键的实际提交顺序串联响应 gate。
    // 准入回退事务同样注册，因此响应顺序不依赖有界等待窗口是否命中。
    void register_response_order(Transaction *txn);

    bool unlock(Transaction* txn, LockDataId lock_data_id);

    bool unlock_all(Transaction *txn);

    static size_t shard_index(lock_data_key_t lock_key);

private:
    static constexpr size_t LOCK_SHARD_COUNT = 128;

    class alignas(64) LockTableShard {
    public:
        std::mutex latch_;
        std::unordered_map<lock_data_key_t, LockRequestQueue> lock_table_;
    };

    class alignas(64) PreSnapshotShard {
    public:
        struct Waiter {
            explicit Waiter(txn_id_t id) : txn_id(id) {}

            txn_id_t txn_id;
            bool granted{false};
            std::condition_variable cv;
        };
        struct Slot {
            txn_id_t owner{INVALID_TXN_ID};
            std::deque<std::shared_ptr<Waiter>> waiters;
        };

        std::mutex latch_;
        std::unordered_map<lock_data_key_t, Slot> slots_;
        std::unordered_map<lock_data_key_t, std::weak_ptr<TransactionResponseGate>>
            response_tails_;
    };

    GroupLockMode recompute_group_lock_mode(const LockRequestQueue &queue) const;
    bool can_grant_request(const LockRequestQueue &queue,
                           std::list<LockRequest>::const_iterator request_iter) const;
    void notify_grantable_waiters(LockRequestQueue &queue) const;
    LockTableShard &shard_for(lock_data_key_t lock_key);
    bool set_wait_edges_and_detect_cycle(txn_id_t waiting_txn, const std::vector<txn_id_t> &blockers);
    void clear_wait_edges(txn_id_t txn_id);
    void remove_txn_from_wait_graph(txn_id_t txn_id);

    std::array<LockTableShard, LOCK_SHARD_COUNT> lock_table_shards_;
    std::array<PreSnapshotShard, LOCK_SHARD_COUNT> pre_snapshot_shards_;
    const std::chrono::milliseconds pre_snapshot_wait_;
    const size_t pre_snapshot_max_waiters_;
    std::mutex wait_graph_latch_;
    std::unordered_map<txn_id_t, std::vector<txn_id_t>> wait_edges_;
};
