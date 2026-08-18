/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY or FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "lock_manager.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <random>
#include <unordered_set>

namespace {

// Lock wait durations may be overridden through process configuration so
// deployments can adapt the timeout policy to their workload.
std::chrono::milliseconds LockWaitMs() {
    static const long ms = [] {
        const char *raw = std::getenv("RMDB_LOCK_WAIT_MS");
        if (raw == nullptr || raw[0] == '\0') {
            // Use a generous upper bound to avoid unnecessary retries while a
            // transaction is making normal progress.
            return 2000L;
        }
        char *end = nullptr;
        long value = std::strtol(raw, &end, 10);
        return (end != raw && *end == '\0' && value >= 0 && value <= 5000) ? value : 2000L;
    }();
    return std::chrono::milliseconds(ms);
}

std::chrono::milliseconds HotLockWaitMs() {
    static const long ms = [] {
        const char *raw = std::getenv("RMDB_HOT_WAIT_MS");
        if (raw == nullptr || raw[0] == '\0') {
            // Use a short base interval when the wait queue is busy.
            return 50L;
        }
        char *end = nullptr;
        long value = std::strtol(raw, &end, 10);
        return (end != raw && *end == '\0' && value >= 0 && value <= 1000) ? value : 50L;
    }();
    return std::chrono::milliseconds(ms);
}

// Scale the wait budget with the number of requests ahead, while keeping it
// within the global bound. Add a small positive offset to avoid many waiters
// retrying at exactly the same instant.
std::chrono::milliseconds LockWaitJitterMs() {
    static const long ms = [] {
        const char *raw = std::getenv("RMDB_LOCK_JITTER_MS");
        if (raw == nullptr || raw[0] == '\0') {
            return 5L;
        }
        char *end = nullptr;
        long value = std::strtol(raw, &end, 10);
        return (end != raw && *end == '\0' && value >= 0 && value <= 100) ? value : 5L;
    }();
    return std::chrono::milliseconds(ms);
}

// When reverse position-aware waiting is enabled, requests farther back in
// the queue use a shorter retry interval instead of occupying the full budget.
std::chrono::milliseconds TailWaitMs() {
    static const long ms = [] {
        const char *raw = std::getenv("RMDB_TAIL_WAIT_MS");
        if (raw == nullptr || raw[0] == '\0') {
            return 15L;
        }
        char *end = nullptr;
        long value = std::strtol(raw, &end, 10);
        return (end != raw && *end == '\0' && value >= 0 && value <= 1000) ? value : 15L;
    }();
    return std::chrono::milliseconds(ms);
}

bool ReversePositionWaitEnabled() {
    static const bool enabled = [] {
        const char *raw = std::getenv("RMDB_REVERSE_POSITION_WAIT");
        return raw != nullptr && raw[0] == '1';
    }();
    return enabled;
}

std::chrono::milliseconds PositionAwareWaitMs(size_t waiters_ahead) {
    // Position-aware waiting gives the request nearest the front the longer
    // interval and lets later requests retry sooner.
    if (ReversePositionWaitEnabled()) {
        const long jitter = LockWaitJitterMs().count();
        const long wait = waiters_ahead == 0 ? LockWaitMs().count() : TailWaitMs().count();
        if (jitter > 0 && wait > 0) {
            static thread_local std::mt19937 rng(std::random_device{}());
            const long extra = static_cast<long>(rng() % (static_cast<unsigned long>(jitter) + 1));
            return std::chrono::milliseconds(wait + extra);
        }
        return std::chrono::milliseconds(wait > 0 ? wait : 50);
    }
    const long base = HotLockWaitMs().count();
    const long cap = LockWaitMs().count();
    if (base <= 0 || cap <= 0) {
        return std::chrono::milliseconds(cap > 0 ? cap : 50);
    }
    const size_t slots = waiters_ahead + 1;
    const long wait = slots >= static_cast<size_t>(cap / base + 1) ? cap : base * static_cast<long>(slots);
    const long jitter = LockWaitJitterMs().count();
    if (jitter > 0 && wait > 0) {
        static thread_local std::mt19937 rng(std::random_device{}());
        const long extra = static_cast<long>(rng() % (static_cast<unsigned long>(jitter) + 1));
        return std::chrono::milliseconds(wait + extra);
    }
    return std::chrono::milliseconds(wait);
}

bool uses_bounded_record_wait(Transaction *txn) {
    if (txn == nullptr) {
        return false;
    }
    const auto isolation = txn->get_isolation_level();
    return isolation == IsolationLevel::SNAPSHOT_ISOLATION || isolation == IsolationLevel::SERIALIZABLE;
}

bool can_skip_wait_graph_cleanup(Transaction *txn) {
    return uses_bounded_record_wait(txn);
}

}  // namespace

LockManager::GroupLockMode LockManager::recompute_group_lock_mode(const LockRequestQueue &queue) const {
    if (queue.fast_exclusive_owner_ != INVALID_TXN_ID) {
        return GroupLockMode::X;
    }
    bool has_exclusive = false;
    bool has_shared = false;
    for (const auto &request : queue.request_queue_) {
        if (!request.granted_) {
            continue;
        }
        if (request.lock_mode_ == LockMode::EXLUCSIVE) {
            has_exclusive = true;
            break;
        }
        if (request.lock_mode_ == LockMode::SHARED) {
            has_shared = true;
        }
    }
    return has_exclusive ? GroupLockMode::X
         : has_shared   ? GroupLockMode::S
                        : GroupLockMode::NON_LOCK;
}

bool LockManager::can_grant_request(const LockRequestQueue &queue,
                                    std::list<LockRequest>::const_iterator request_iter) const {
    if (request_iter == queue.request_queue_.end()) {
        return false;
    }
    if (request_iter->granted_) {
        return true;
    }

    const txn_id_t txn_id = request_iter->txn_id_;
    if (queue.fast_exclusive_owner_ != INVALID_TXN_ID &&
        queue.fast_exclusive_owner_ != txn_id) {
        return false;
    }
    if (request_iter->lock_mode_ == LockMode::SHARED) {
        for (auto iter = queue.request_queue_.begin(); iter != request_iter; ++iter) {
            if (iter->txn_id_ != txn_id && iter->lock_mode_ == LockMode::EXLUCSIVE) {
                return false;
            }
        }
        for (const auto &request : queue.request_queue_) {
            if (request.granted_ && request.txn_id_ != txn_id &&
                request.lock_mode_ == LockMode::EXLUCSIVE) {
                return false;
            }
        }
        return true;
    }

    for (auto iter = queue.request_queue_.begin(); iter != request_iter; ++iter) {
        if (iter->txn_id_ != txn_id) {
            return false;
        }
    }
    for (const auto &request : queue.request_queue_) {
        if (request.granted_ && request.txn_id_ != txn_id) {
            return false;
        }
    }
    return true;
}

void LockManager::notify_grantable_waiters(LockRequestQueue &queue) const {
    for (auto iter = queue.request_queue_.begin(); iter != queue.request_queue_.end(); ++iter) {
        if (iter->granted_) {
            continue;
        }
        if (!can_grant_request(queue, iter)) {
            if (iter->lock_mode_ == LockMode::EXLUCSIVE) {
                break;
            }
            continue;
        }
        iter->cv_.notify_one();
        if (iter->lock_mode_ == LockMode::EXLUCSIVE) {
            break;
        }
    }
}

size_t LockManager::shard_index(lock_data_key_t lock_key) {
    lock_key ^= lock_key >> 33;
    lock_key *= 0xff51afd7ed558ccdULL;
    lock_key ^= lock_key >> 33;
    return static_cast<size_t>(lock_key & (LOCK_SHARD_COUNT - 1));
}

LockManager::LockTableShard &LockManager::shard_for(lock_data_key_t lock_key) {
    return lock_table_shards_[shard_index(lock_key)];
}

bool LockManager::set_wait_edges_and_detect_cycle(txn_id_t waiting_txn,
                                                  const std::vector<txn_id_t> &blockers) {
    std::lock_guard<std::mutex> guard(wait_graph_latch_);
    if (blockers.empty()) {
        wait_edges_.erase(waiting_txn);
        return false;
    }

    auto &edges = wait_edges_[waiting_txn];
    edges.clear();
    edges.reserve(blockers.size());
    for (auto blocker : blockers) {
        if (blocker != waiting_txn &&
            std::find(edges.begin(), edges.end(), blocker) == edges.end()) {
            edges.push_back(blocker);
        }
    }

    std::unordered_set<txn_id_t> visited;
    std::function<bool(txn_id_t)> reaches_waiting = [&](txn_id_t current) {
        if (current == waiting_txn) {
            return true;
        }
        if (!visited.insert(current).second) {
            return false;
        }
        auto iter = wait_edges_.find(current);
        if (iter == wait_edges_.end()) {
            return false;
        }
        for (auto next : iter->second) {
            if (reaches_waiting(next)) {
                return true;
            }
        }
        return false;
    };

    for (auto blocker : edges) {
        if (reaches_waiting(blocker)) {
            wait_edges_.erase(waiting_txn);
            return true;
        }
    }
    return false;
}

void LockManager::clear_wait_edges(txn_id_t txn_id) {
    std::lock_guard<std::mutex> guard(wait_graph_latch_);
    wait_edges_.erase(txn_id);
}

void LockManager::remove_txn_from_wait_graph(txn_id_t txn_id) {
    std::lock_guard<std::mutex> guard(wait_graph_latch_);
    wait_edges_.erase(txn_id);
    for (auto iter = wait_edges_.begin(); iter != wait_edges_.end();) {
        auto &edges = iter->second;
        edges.erase(std::remove(edges.begin(), edges.end(), txn_id), edges.end());
        if (edges.empty()) {
            iter = wait_edges_.erase(iter);
        } else {
            ++iter;
        }
    }
}

/**
 * @description: 申请行级共享锁
 * @return {bool} 加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {Rid&} rid 加锁的目标记录ID 记录所在的表的fd
 * @param {int} tab_fd
 */
bool LockManager::lock_shared_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    if (txn == nullptr) {
        return true;
    }

    LockDataId lock_data_id(tab_fd, rid, LockDataType::RECORD);
    const auto lock_key = lock_data_id.Get();
    auto &shard = shard_for(lock_key);
    std::unique_lock<std::mutex> lock(shard.latch_);
    auto &queue = shard.lock_table_[lock_key];
    const auto txn_id = txn->get_transaction_id();

    if (queue.fast_exclusive_owner_ == txn_id) {
        return true;
    }

    for (const auto &request : queue.request_queue_) {
        if (request.granted_ && request.txn_id_ == txn_id &&
            (request.lock_mode_ == LockMode::SHARED || request.lock_mode_ == LockMode::EXLUCSIVE)) {
            return true;
        }
    }

    queue.request_queue_.emplace_back(txn_id, LockMode::SHARED);
    auto request_iter = std::prev(queue.request_queue_.end());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);

    auto has_blocker = [&]() {
        if (queue.fast_exclusive_owner_ != INVALID_TXN_ID &&
            queue.fast_exclusive_owner_ != txn_id) {
            return true;
        }
        for (auto iter = queue.request_queue_.begin(); iter != request_iter; ++iter) {
            if (iter->txn_id_ != txn_id && iter->lock_mode_ == LockMode::EXLUCSIVE) {
                return true;
            }
        }
        for (const auto &request : queue.request_queue_) {
            if (request.granted_ && request.txn_id_ != txn_id &&
                request.lock_mode_ == LockMode::EXLUCSIVE) {
                return true;
            }
        }
        return false;
    };
    auto collect_blockers = [&]() {
        std::vector<txn_id_t> blockers;
        blockers.reserve(queue.request_queue_.size());
        if (queue.fast_exclusive_owner_ != INVALID_TXN_ID &&
            queue.fast_exclusive_owner_ != txn_id) {
            blockers.push_back(queue.fast_exclusive_owner_);
        }
        for (auto iter = queue.request_queue_.begin(); iter != request_iter; ++iter) {
            if (iter->txn_id_ != txn_id && iter->lock_mode_ == LockMode::EXLUCSIVE) {
                blockers.push_back(iter->txn_id_);
            }
        }
        for (const auto &request : queue.request_queue_) {
            if (request.granted_ && request.txn_id_ != txn_id &&
                request.lock_mode_ == LockMode::EXLUCSIVE) {
                blockers.push_back(request.txn_id_);
            }
        }
        return blockers;
    };
    auto can_grant = [&]() {
        if (txn->get_state() == TransactionState::ABORTED) {
            return false;
        }
        return !has_blocker();
    };

    while (!can_grant()) {
        if (set_wait_edges_and_detect_cycle(txn_id, collect_blockers())) {
            queue.request_queue_.erase(request_iter);
            if (queue.request_queue_.empty() &&
                queue.fast_exclusive_owner_ == INVALID_TXN_ID) {
                shard.lock_table_.erase(lock_key);
            } else {
                notify_grantable_waiters(queue);
            }
            return false;
        }
        if (!request_iter->cv_.wait_until(lock, deadline, can_grant)) {
            clear_wait_edges(txn_id);
            queue.request_queue_.erase(request_iter);
            if (queue.request_queue_.empty() &&
                queue.fast_exclusive_owner_ == INVALID_TXN_ID) {
                shard.lock_table_.erase(lock_key);
            } else {
                notify_grantable_waiters(queue);
            }
            return false;
        }
    }

    clear_wait_edges(txn_id);
    request_iter->granted_ = true;
    queue.group_lock_mode_ = GroupLockMode::S;
    txn->add_lock(lock_key);
    return true;
}

/**
 * @description: 申请行级排他锁
 * @return {bool} 加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {Rid&} rid 加锁的目标记录ID
 * @param {int} tab_fd 记录所在的表的fd
 */
bool LockManager::lock_exclusive_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    if (txn == nullptr) {
        return true;
    }

    LockDataId lock_data_id(tab_fd, rid, LockDataType::RECORD);
    const auto lock_key = lock_data_id.Get();
    auto &shard = shard_for(lock_key);
    std::unique_lock<std::mutex> lock(shard.latch_);
    auto &queue = shard.lock_table_[lock_key];
    const auto txn_id = txn->get_transaction_id();
    const bool bounded_wait = uses_bounded_record_wait(txn);

    if (queue.fast_exclusive_owner_ == txn_id) {
        return true;
    }
    if (queue.fast_exclusive_owner_ != INVALID_TXN_ID && !bounded_wait) {
        return false;
    }

    for (const auto &request : queue.request_queue_) {
        if (request.granted_ && request.txn_id_ == txn_id &&
            request.lock_mode_ == LockMode::EXLUCSIVE) {
            return true;
        }
    }

    if (bounded_wait) {
        if (queue.request_queue_.empty() &&
            queue.fast_exclusive_owner_ == INVALID_TXN_ID) {
            queue.fast_exclusive_owner_ = txn_id;
            queue.group_lock_mode_ = GroupLockMode::X;
            txn->add_lock(lock_key);
            return true;
        }
    }

    // 在自入队之前计算前方等待者数(修复旧代码入队后 size>1 的判定,
    // 该判定使唯一等待者误拿 50ms 而后续等待者一律 8ms 快速失败)。
    // 队列可能含当前持有者(granted 条目),不计入等待者数。
    size_t granted_in_queue = 0;
    for (const auto &request : queue.request_queue_) {
        if (request.granted_) {
            ++granted_in_queue;
        }
    }
    const size_t waiters_ahead = queue.request_queue_.size() - granted_in_queue;
    queue.request_queue_.emplace_back(txn_id, LockMode::EXLUCSIVE);
    auto request_iter = std::prev(queue.request_queue_.end());
    const auto wait_ms = bounded_wait ? PositionAwareWaitMs(waiters_ahead)
                                      : std::chrono::seconds(60);
    const auto deadline = std::chrono::steady_clock::now() + wait_ms;

    auto has_blocker = [&]() {
        if (queue.fast_exclusive_owner_ != INVALID_TXN_ID &&
            queue.fast_exclusive_owner_ != txn_id) {
            return true;
        }
        for (auto iter = queue.request_queue_.begin(); iter != request_iter; ++iter) {
            if (iter->txn_id_ != txn_id) {
                return true;
            }
        }
        for (const auto &request : queue.request_queue_) {
            if (request.granted_ && request.txn_id_ != txn_id) {
                return true;
            }
        }
        return false;
    };
    auto collect_blockers = [&]() {
        std::vector<txn_id_t> blockers;
        blockers.reserve(queue.request_queue_.size());
        if (queue.fast_exclusive_owner_ != INVALID_TXN_ID &&
            queue.fast_exclusive_owner_ != txn_id) {
            blockers.push_back(queue.fast_exclusive_owner_);
        }
        for (auto iter = queue.request_queue_.begin(); iter != request_iter; ++iter) {
            if (iter->txn_id_ != txn_id) {
                blockers.push_back(iter->txn_id_);
            }
        }
        for (const auto &request : queue.request_queue_) {
            if (request.granted_ && request.txn_id_ != txn_id) {
                blockers.push_back(request.txn_id_);
            }
        }
        return blockers;
    };
    auto can_grant = [&]() {
        if (txn->get_state() == TransactionState::ABORTED) {
            return false;
        }
        return !has_blocker();
    };

    while (!can_grant()) {
        // SI/SER waits are deliberately short, so their timeout is also the deadlock bound.
        // Avoid putting every hot-row wait through the global wait graph. The executor must
        // recheck tuple ownership/commit timestamp after this function grants the record lock.
        if (!bounded_wait && set_wait_edges_and_detect_cycle(txn_id, collect_blockers())) {
            queue.request_queue_.erase(request_iter);
            if (queue.request_queue_.empty() &&
                queue.fast_exclusive_owner_ == INVALID_TXN_ID) {
                shard.lock_table_.erase(lock_key);
            } else {
                notify_grantable_waiters(queue);
            }
            return false;
        }
        if (!request_iter->cv_.wait_until(lock, deadline, can_grant)) {
            clear_wait_edges(txn_id);
            queue.request_queue_.erase(request_iter);
            if (queue.request_queue_.empty() &&
                queue.fast_exclusive_owner_ == INVALID_TXN_ID) {
                shard.lock_table_.erase(lock_key);
            } else {
                notify_grantable_waiters(queue);
            }
            return false;
        }
    }

    if (!bounded_wait) {
        clear_wait_edges(txn_id);
    }
    request_iter->granted_ = true;
    queue.group_lock_mode_ = GroupLockMode::X;
    txn->add_lock(lock_key);
    return true;
}

bool LockManager::acquire_pre_snapshot_writes(
    Transaction *txn, const std::vector<lock_data_key_t> &keys) {
    if (txn == nullptr || keys.empty()) {
        return true;
    }

    std::vector<lock_data_key_t> ordered = keys;
    std::sort(ordered.begin(), ordered.end(), [](lock_data_key_t lhs, lock_data_key_t rhs) {
        const auto lhs_shard = shard_index(lhs);
        const auto rhs_shard = shard_index(rhs);
        return lhs_shard == rhs_shard ? lhs < rhs : lhs_shard < rhs_shard;
    });
    ordered.erase(std::unique(ordered.begin(), ordered.end()), ordered.end());

    const txn_id_t txn_id = txn->get_transaction_id();
    for (lock_data_key_t key : ordered) {
        auto &shard = pre_snapshot_shards_[shard_index(key)];
        std::unique_lock<std::mutex> guard(shard.latch_);
        auto [slot_iter, inserted] = shard.slots_.try_emplace(key);
        (void)inserted;
        auto &slot = slot_iter->second;
        if (slot.owner == txn_id) {
            continue;
        }
        if (slot.owner == INVALID_TXN_ID && slot.waiters.empty()) {
            txn->add_pre_snapshot_write_key(key);
            slot.owner = txn_id;
            continue;
        }

        // 只保留一个小型定向继任窗口，覆盖热点键的典型同时在途事务。
        // 窗口外调用者不进入 futex 队列，而是回退原生 SI 路径；这些
        // 事务仍由标准 stale-snapshot 检查裁决，因此只影响调度而不改语义。
        if (slot.waiters.size() >= pre_snapshot_max_waiters_) {
            guard.unlock();
            release_pre_snapshot_writes(txn);
            return false;
        }

        auto waiter = std::make_shared<PreSnapshotShard::Waiter>(txn_id);
        txn->add_pre_snapshot_write_key(key);
        try {
            slot.waiters.push_back(waiter);
        } catch (...) {
            txn->remove_pre_snapshot_write_key(key);
            throw;
        }
        const auto deadline = std::chrono::steady_clock::now() + pre_snapshot_wait_;
        const bool acquired = waiter->cv.wait_until(guard, deadline, [&] {
            return waiter->granted;
        });
        if (!acquired) {
            auto current = shard.slots_.find(key);
            if (current != shard.slots_.end()) {
                auto &waiters = current->second.waiters;
                auto queued = std::find(waiters.begin(), waiters.end(), waiter);
                if (queued != waiters.end()) {
                    waiters.erase(queued);
                }
                if (current->second.owner == INVALID_TXN_ID && waiters.empty()) {
                    shard.slots_.erase(current);
                }
            }
            txn->remove_pre_snapshot_write_key(key);
            guard.unlock();
            release_pre_snapshot_writes(txn);
            return false;
        }

    }
    return true;
}

void LockManager::release_pre_snapshot_writes(Transaction *txn) {
    if (txn == nullptr || txn->get_pre_snapshot_write_keys().empty()) {
        return;
    }

    std::vector<lock_data_key_t> keys = txn->get_pre_snapshot_write_keys();
    std::sort(keys.begin(), keys.end(), [](lock_data_key_t lhs, lock_data_key_t rhs) {
        const auto lhs_shard = shard_index(lhs);
        const auto rhs_shard = shard_index(rhs);
        return lhs_shard == rhs_shard ? lhs < rhs : lhs_shard < rhs_shard;
    });
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    const txn_id_t txn_id = txn->get_transaction_id();

    std::vector<std::shared_ptr<PreSnapshotShard::Waiter>> successors;
    successors.reserve(keys.size());
    for (size_t begin = 0; begin < keys.size();) {
        const size_t shard_id = shard_index(keys[begin]);
        size_t end = begin + 1;
        while (end < keys.size() && shard_index(keys[end]) == shard_id) {
            ++end;
        }
        auto &shard = pre_snapshot_shards_[shard_id];
        {
            std::lock_guard<std::mutex> guard(shard.latch_);
            for (size_t pos = begin; pos < end; ++pos) {
                auto slot_iter = shard.slots_.find(keys[pos]);
                if (slot_iter == shard.slots_.end() || slot_iter->second.owner != txn_id) {
                    continue;
                }
                auto &slot = slot_iter->second;
                if (slot.waiters.empty()) {
                    shard.slots_.erase(slot_iter);
                    continue;
                }
                auto successor = std::move(slot.waiters.front());
                slot.waiters.pop_front();
                slot.owner = successor->txn_id;
                successor->granted = true;
                successors.push_back(std::move(successor));
            }
        }
        begin = end;
    }
    txn->clear_pre_snapshot_write_keys();
    for (const auto &successor : successors) {
        successor->cv.notify_one();
    }
}

void LockManager::register_response_order(Transaction *txn) {
    if (txn == nullptr || txn->get_response_order_keys().empty()) {
        return;
    }
    const auto gate = txn->get_or_create_response_gate();
    const auto &keys = txn->get_response_order_keys();
    std::vector<lock_data_key_t> registered;
    registered.reserve(keys.size());

    for (lock_data_key_t key : keys) {
        auto &shard = pre_snapshot_shards_[shard_index(key)];
        {
            std::lock_guard<std::mutex> guard(shard.latch_);
            auto tail_iter = shard.response_tails_.find(key);
            if (tail_iter != shard.response_tails_.end()) {
                auto predecessor = tail_iter->second.lock();
                if (predecessor != nullptr && predecessor != gate) {
                    txn->add_response_dependency(predecessor);
                }
            }
            shard.response_tails_[key] = gate;
        }
        registered.push_back(key);
    }

    // response_tails_ must not retain one entry per one-shot logical key.
    // Gate completion removes an entry only when it still points to this gate;
    // 若已有后继替换，后继的 callback 负责最终清理。
    const std::weak_ptr<TransactionResponseGate> weak_gate = gate;
    for (lock_data_key_t key : registered) {
        gate->AddCompletionCallback([this, key, weak_gate] {
            auto &shard = pre_snapshot_shards_[shard_index(key)];
            std::lock_guard<std::mutex> guard(shard.latch_);
            auto tail_iter = shard.response_tails_.find(key);
            if (tail_iter == shard.response_tails_.end()) {
                return;
            }
            auto registered_gate = weak_gate.lock();
            auto current_tail = tail_iter->second.lock();
            if (current_tail == nullptr ||
                (registered_gate != nullptr && current_tail == registered_gate)) {
                shard.response_tails_.erase(tail_iter);
            }
        });
    }
}

/**
 * @description: 释放锁
 * @return {bool} 返回解锁是否成功
 * @param {Transaction*} txn 要释放的事务对象指针
 * @param {LockDataId} lock_data_id 要释放的锁ID
 */
bool LockManager::unlock(Transaction* txn, LockDataId lock_data_id) {
    if (txn == nullptr) {
        return true;
    }

    const auto lock_key = lock_data_id.Get();
    auto &shard = shard_for(lock_key);
    const auto txn_id = txn->get_transaction_id();
    {
        std::lock_guard<std::mutex> lock(shard.latch_);
        auto iter = shard.lock_table_.find(lock_key);
        if (iter == shard.lock_table_.end()) {
            txn->remove_lock(lock_key);
            return true;
        }
        auto &queue_info = iter->second;
        auto &queue = queue_info.request_queue_;
        const size_t before = queue.size();
        const bool removed_fast_owner = queue_info.fast_exclusive_owner_ == txn_id;
        if (removed_fast_owner) {
            queue_info.fast_exclusive_owner_ = INVALID_TXN_ID;
        }
        queue.remove_if([&](const LockRequest &request) {
            return request.txn_id_ == txn_id;
        });
        txn->remove_lock(lock_key);
        if (queue.size() == before && !removed_fast_owner) {
            return true;
        }
        if (queue.empty() && queue_info.fast_exclusive_owner_ == INVALID_TXN_ID) {
            shard.lock_table_.erase(iter);
        } else {
            queue_info.group_lock_mode_ = recompute_group_lock_mode(queue_info);
            notify_grantable_waiters(queue_info);
        }
    }
    if (!can_skip_wait_graph_cleanup(txn)) {
        remove_txn_from_wait_graph(txn_id);
    }
    return true;
}

bool LockManager::unlock_all(Transaction *txn) {
    if (txn == nullptr) {
        return true;
    }
    const auto &lock_set = txn->get_lock_set();
    if (lock_set.empty()) {
        if (!can_skip_wait_graph_cleanup(txn)) {
            remove_txn_from_wait_graph(txn->get_transaction_id());
        }
        release_pre_snapshot_writes(txn);
        return true;
    }

    const auto txn_id = txn->get_transaction_id();
    auto release_one = [&](LockTableShard &shard, lock_data_key_t lock_key) {
        auto iter = shard.lock_table_.find(lock_key);
        if (iter == shard.lock_table_.end()) {
            return;
        }
        auto &queue_info = iter->second;
        auto &queue = queue_info.request_queue_;
        const size_t before = queue.size();
        const bool removed_fast_owner = queue_info.fast_exclusive_owner_ == txn_id;
        if (removed_fast_owner) {
            queue_info.fast_exclusive_owner_ = INVALID_TXN_ID;
        }
        queue.remove_if([&](const LockRequest &request) {
            return request.txn_id_ == txn_id;
        });
        if (queue.size() == before && !removed_fast_owner) {
            return;
        }
        if (queue.empty() && queue_info.fast_exclusive_owner_ == INVALID_TXN_ID) {
            shard.lock_table_.erase(iter);
        } else {
            queue_info.group_lock_mode_ = recompute_group_lock_mode(queue_info);
            notify_grantable_waiters(queue_info);
        }
    };

    std::vector<lock_data_key_t> locks = lock_set;
    std::sort(locks.begin(), locks.end(), [](lock_data_key_t lhs, lock_data_key_t rhs) {
        const auto lhs_shard = shard_index(lhs);
        const auto rhs_shard = shard_index(rhs);
        return lhs_shard == rhs_shard ? lhs < rhs : lhs_shard < rhs_shard;
    });
    locks.erase(std::unique(locks.begin(), locks.end()), locks.end());

    for (size_t begin = 0; begin < locks.size();) {
        const auto shard_id = shard_index(locks[begin]);
        size_t end = begin + 1;
        while (end < locks.size() && shard_index(locks[end]) == shard_id) {
            ++end;
        }

        auto &shard = lock_table_shards_[shard_id];
        std::lock_guard<std::mutex> lock(shard.latch_);
        for (size_t pos = begin; pos < end; ++pos) {
            release_one(shard, locks[pos]);
        }
        begin = end;
    }

    txn->clear_locks();
    if (!can_skip_wait_graph_cleanup(txn)) {
        remove_txn_from_wait_graph(txn_id);
    }
    // 先释放真实记录锁，再移交准入令牌。继任事务固定快照后不会再为
    // 前任残留的物理锁额外排队，且 commit/abort 已在调用本函数前发布状态。
    release_pre_snapshot_writes(txn);
    return true;
}
