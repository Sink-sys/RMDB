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

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <unordered_set>

#include "common/common.h"
#include "transaction/txn_defs.h"
#include "record/rm_defs.h"
#include "system/sm_meta.h"

/** 表示此tuple的前一个版本的链接 */
struct UndoLink {
    /* 之前的版本可以在其中的事务中找到 */
    txn_id_t prev_txn_{INVALID_TXN_ID};
    /* 在 `prev_txn_` 中前一个版本的日志索引 */
    int prev_log_idx_{0};

    friend auto operator==(const UndoLink& a, const UndoLink& b) {
        return a.prev_txn_ == b.prev_txn_ && a.prev_log_idx_ == b.prev_log_idx_;
    }

    friend auto operator!=(const UndoLink& a, const UndoLink& b) {
        return !(a == b);
    }

    /* 是否实际指向一条撤销日志。 */
    bool IsValid() const {
        return prev_txn_ != INVALID_TXN_ID;
    }
};

struct UndoLog {
    /* 此日志是否为删除标记 */
    bool is_deleted_{false};
    /* 旧元组的完整字节镜像；shared_ptr 允许版本链读者跨执行器生命周期持有。 */
    std::shared_ptr<RmRecord> tuple_image_{nullptr};
    /* 定长列增量:packed delta bytes。
     * layout: u16 num_deltas; per delta: u16 col_offset u16 col_len u8 data[col_len]。
     * tuple_image_ == nullptr && !delta_.empty() 时激活。 */
    std::vector<rmdb::u8> delta_;
    /* 此撤销日志的时间戳 */
    timestamp_t ts_{INVALID_TS};
    /* 撤销日志的前一个版本 */
    UndoLink prev_version_{};
};

// A logical-key successor may execute and group-commit before its predecessor's
// socket response is sent.  The response gate preserves that per-key response
// order without keeping record locks or WAL flushes on the critical path.
class TransactionResponseGate {
public:
    void Wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&] { return completed_; });
    }

    void Complete() {
        std::vector<std::function<void()>> callbacks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (completed_) {
                return;
            }
            completed_ = true;
            callbacks.swap(completion_callbacks_);
        }
        cv_.notify_all();
        for (auto &callback : callbacks) {
            callback();
        }
    }

    void AddCompletionCallback(std::function<void()> callback) {
        bool run_now = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (completed_) {
                run_now = true;
            } else {
                completion_callbacks_.push_back(std::move(callback));
            }
        }
        if (run_now) {
            callback();
        }
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool completed_{false};
    std::vector<std::function<void()>> completion_callbacks_;
};

class Transaction {
public:
    enum class UndoGcLinkResult { MISSING, CONTINUE, BOUNDARY };

    struct PredicateRead {
        std::string tab_name;
        std::vector<ColMeta> cols;
        std::vector<Condition> conds;
    };
    struct SerializableWriteInfo {
        std::string tab_name;
        Rid rid;
        std::vector<ColMeta> cols;
        std::shared_ptr<RmRecord> old_record;
        std::shared_ptr<RmRecord> new_record;
    };
    explicit Transaction(txn_id_t txn_id,
                         IsolationLevel isolation_level = IsolationLevel::SNAPSHOT_ISOLATION)
        : isolation_level_(isolation_level), txn_id_(txn_id) {}

    ~Transaction() = default;

    inline txn_id_t get_transaction_id() const {
        return txn_id_;
    }
    inline void set_transaction_id(txn_id_t txn_id) {
        txn_id_ = txn_id;
    }

    inline std::thread::id get_thread_id() const {
        return thread_id_;
    }

    inline void set_txn_mode(bool txn_mode) {
        txn_mode_ = txn_mode;
    }
    inline bool get_txn_mode() const {
        return txn_mode_;
    }

    inline void set_lightweight_read_only(bool value) {
        lightweight_read_only_ = value;
    }
    inline bool is_lightweight_read_only() const {
        return lightweight_read_only_;
    }

    inline void set_start_ts(timestamp_t start_ts) {
        start_ts_ = start_ts;
    }
    inline timestamp_t get_start_ts() const {
        return start_ts_;
    }

    inline IsolationLevel get_isolation_level() const {
        return isolation_level_;
    }
    inline void set_isolation_level(IsolationLevel isolation_level) {
        isolation_level_ = isolation_level;
    }

    inline bool watermark_registered() const {
        return watermark_registered_;
    }
    inline void set_watermark_registered(bool registered) {
        watermark_registered_ = registered;
    }

    inline bool gc_ready() const {
        return gc_ready_.load(std::memory_order_acquire);
    }
    inline void set_gc_ready(bool ready) {
        gc_ready_.store(ready, std::memory_order_release);
    }

    inline bool admission_active() const {
        return admission_active_;
    }
    inline void set_admission_active(bool active) {
        admission_active_ = active;
    }

    inline bool ssi_metadata_released() const {
        return ssi_metadata_released_.load(std::memory_order_acquire);
    }
    inline void set_ssi_metadata_released(bool released) {
        ssi_metadata_released_.store(released, std::memory_order_release);
    }

    inline TransactionState get_state() const {
        return state_.load(std::memory_order_acquire);
    }
    inline void set_state(TransactionState state) {
        state_.store(state, std::memory_order_release);
    }

    inline lsn_t get_prev_lsn() const {
        return prev_lsn_.load(std::memory_order_acquire);
    }
    inline void set_prev_lsn(lsn_t prev_lsn) {
        prev_lsn_.store(prev_lsn, std::memory_order_release);
    }

    inline lsn_t get_first_lsn() const {
        return first_lsn_.load(std::memory_order_acquire);
    }
    inline void set_first_lsn(lsn_t first_lsn) {
        first_lsn_.store(first_lsn, std::memory_order_release);
    }

    // The greatest WAL byte on which this transaction's snapshot may depend.
    // A read-only transaction has no commit record of its own, so commit must
    // explicitly wait for this frontier before reporting success.  A writer's
    // later commit record covers the same dependency through sequential WAL.
    inline lsn_t get_snapshot_durability_lsn() const {
        return snapshot_durability_lsn_.load(std::memory_order_acquire);
    }
    inline void set_snapshot_durability_lsn(lsn_t lsn) {
        snapshot_durability_lsn_.store(lsn, std::memory_order_release);
    }

    inline std::shared_ptr<TransactionResponseGate> get_or_create_response_gate() {
        if (response_gate_ == nullptr) {
            response_gate_ = std::make_shared<TransactionResponseGate>();
        }
        return response_gate_;
    }
    inline std::shared_ptr<TransactionResponseGate> get_response_gate() const {
        return response_gate_;
    }
    inline void add_response_dependency(
        const std::shared_ptr<TransactionResponseGate> &dependency) {
        if (dependency == nullptr || dependency == response_gate_) {
            return;
        }
        if (std::find(response_dependencies_.begin(), response_dependencies_.end(), dependency) ==
            response_dependencies_.end()) {
            response_dependencies_.push_back(dependency);
        }
    }
    inline void wait_for_response_dependencies() {
        for (const auto &dependency : response_dependencies_) {
            dependency->Wait();
        }
        std::vector<std::shared_ptr<TransactionResponseGate>>().swap(response_dependencies_);
    }
    inline void set_response_gate_deferred(bool deferred) {
        response_gate_deferred_ = deferred;
    }
    inline bool response_gate_deferred() const {
        return response_gate_deferred_;
    }
    inline void complete_response_gate() {
        if (response_gate_ != nullptr) {
            response_gate_->Complete();
        }
    }

    inline std::vector<WriteRecord>& get_write_set() {
        return write_set_;
    }
    inline const std::vector<WriteRecord>& get_write_set() const {
        return write_set_;
    }

    template <typename... Args>
    inline void emplace_write_record(Args&&... args) {
        write_set_.emplace_back(std::forward<Args>(args)...);
    }

    inline const std::vector<lock_data_key_t>& get_lock_set() const {
        return lock_set_;
    }

    inline void add_lock(lock_data_key_t lock_key) {
        lock_set_.push_back(lock_key);
    }

    inline void remove_lock(lock_data_key_t lock_key) {
        lock_set_.erase(std::remove(lock_set_.begin(), lock_set_.end(), lock_key), lock_set_.end());
    }

    inline void clear_locks() {
        // 事务结束(commit/abort)后容量无保留价值:释放 capacity,
        // 避免 GC 滞留事务按峰值持锁数保留内存(积压时线性放大)。
        std::vector<lock_data_key_t>().swap(lock_set_);
    }

    // 持锁行例外(Phase 1):本事务是否持有 (tab_fd, rid) 的记录级 X 锁。
    // 用于读可见性与写冲突检查的行级放行；锁先于读可避免热行写写冲突
    // 被误判为陈旧快照写入。
    inline bool holds_record_lock(int tab_fd, const Rid &rid) const {
        if (lock_set_.empty()) {
            return false;
        }
        lock_data_key_t key = LockDataId(tab_fd, rid, LockDataType::RECORD).Get();
        return std::find(lock_set_.begin(), lock_set_.end(), key) != lock_set_.end();
    }

    // 在 read_ts 固定前获得的逻辑写准入令牌。它们仅负责安排热点写者
    // 的开始顺序；真正的可见性和写写冲突仍由 MVCC 元数据检查决定。
    inline const std::vector<lock_data_key_t> &get_pre_snapshot_write_keys() const {
        return pre_snapshot_write_keys_;
    }
    inline void add_pre_snapshot_write_key(lock_data_key_t key) {
        pre_snapshot_write_keys_.push_back(key);
    }
    inline void remove_pre_snapshot_write_key(lock_data_key_t key) {
        auto it = std::find(pre_snapshot_write_keys_.begin(), pre_snapshot_write_keys_.end(), key);
        if (it != pre_snapshot_write_keys_.end()) {
            pre_snapshot_write_keys_.erase(it);
        }
    }
    inline void clear_pre_snapshot_write_keys() {
        std::vector<lock_data_key_t>().swap(pre_snapshot_write_keys_);
    }

    inline void set_response_order_keys(std::vector<lock_data_key_t> keys) {
        std::sort(keys.begin(), keys.end());
        keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
        response_order_keys_ = std::move(keys);
        if (!response_order_keys_.empty()) {
            (void)get_or_create_response_gate();
        }
    }
    inline const std::vector<lock_data_key_t> &get_response_order_keys() const {
        return response_order_keys_;
    }

    // 仅当该锁在 locks_at_entry(本语句入口)之前已持有才返回 true。
    // 区分"预锁"(前序语句获取,盲相对写,可放行)与"语句内新锁"
    // (如 CAS 谓词写,保持原 stale-snapshot 语义)。
    inline bool holds_record_lock_since(int tab_fd, const Rid &rid, size_t locks_at_entry) const {
        if (lock_set_.empty() || locks_at_entry == 0) {
            return false;
        }
        lock_data_key_t key = LockDataId(tab_fd, rid, LockDataType::RECORD).Get();
        const size_t scan_limit = lock_set_.size() < locks_at_entry ? lock_set_.size() : locks_at_entry;
        for (size_t i = 0; i < scan_limit; ++i) {
            if (lock_set_[i] == key) {
                return true;
            }
        }
        return false;
    }

    inline void add_read_record(const std::string& tab_name, Rid rid) {
        read_records_[tab_name].push_back(rid);
    }

    inline const std::unordered_map<std::string, std::vector<Rid>>& get_read_records() const {
        return read_records_;
    }

    inline void add_predicate_read(std::string tab_name,
                                   std::vector<ColMeta> cols,
                                   std::vector<Condition> conds) {
        predicate_reads_.push_back({std::move(tab_name), std::move(cols), std::move(conds)});
    }

    inline const std::vector<PredicateRead>& get_predicate_reads() const {
        return predicate_reads_;
    }

    inline void upsert_serializable_write(std::string tab_name,
                                          Rid rid,
                                          std::vector<ColMeta> cols,
                                          std::shared_ptr<RmRecord> old_record,
                                          std::shared_ptr<RmRecord> new_record) {
        serializable_writes_[serializable_write_key(tab_name, rid)] = {std::move(tab_name),
                                                                       rid,
                                                                       std::move(cols),
                                                                       std::move(old_record),
                                                                       std::move(new_record)};
    }

    inline const std::unordered_map<std::string, SerializableWriteInfo>& get_serializable_writes()
        const {
        return serializable_writes_;
    }

    inline void add_rw_dependency(txn_id_t txn_id) {
        rw_dependencies_.insert(txn_id);
    }

    inline const std::unordered_set<txn_id_t>& get_rw_dependencies() const {
        return rw_dependencies_;
    }

    inline void remove_rw_dependency(txn_id_t txn_id) {
        rw_dependencies_.erase(txn_id);
    }

    inline void clear_serializable_state() {
        // 事务结束后 SSI 元数据无保留价值:释放容量(含 ColMeta/Condition 负载)。
        std::unordered_map<std::string, std::vector<Rid>>().swap(read_records_);
        std::vector<PredicateRead>().swap(predicate_reads_);
        std::unordered_set<txn_id_t>().swap(rw_dependencies_);
        std::unordered_map<std::string, SerializableWriteInfo>().swap(serializable_writes_);
    }

    inline timestamp_t get_read_ts() const {
        return read_ts_;
    }
    inline void set_read_ts(timestamp_t read_ts) {
        read_ts_ = read_ts;
    }
    inline timestamp_t get_commit_ts() const {
        return commit_ts_;
    }
    inline void set_commit_ts(timestamp_t commit_ts) {
        commit_ts_ = commit_ts;
    }

    inline void clear_write_records() {
        // 提交尾部(索引维护完成后)不再需要写集:swap 释放旧行镜像等
        // 负载与容器 capacity,GC 滞留期间不按峰值写集保留内存。
        std::vector<WriteRecord>().swap(write_set_);
    }

    /** 修改现有的撤销日志 */
    inline auto ModifyUndoLog(int log_idx, UndoLog new_log) {
        std::scoped_lock<std::mutex> lck(latch_);
        undo_logs_[log_idx] = std::move(new_log);
    }

    /** @return 此事务中撤销日志的索引 */
    inline auto AppendUndoLog(UndoLog log) -> UndoLink {
        std::scoped_lock<std::mutex> lck(latch_);
        undo_logs_.emplace_back(std::move(log));
        return {txn_id_, static_cast<int>(undo_logs_.size() - 1)};
    }
    inline auto GetUndoLog(size_t log_id) -> UndoLog {
        std::scoped_lock<std::mutex> lck(latch_);
        return undo_logs_[log_id];
    }

    /** 锁内只读访问撤销日志:消除版本链读取每步的 UndoLog 值拷贝
     * (vector<bool>/vector<Value> 拷贝 + shared_ptr 原子递增),fn 在
     * 事务 latch 作用域内收到 const 引用。 */
    template <typename Fn>
    inline void WithUndoLog(size_t log_id, Fn &&fn) {
        std::scoped_lock<std::mutex> lck(latch_);
        if (log_id < undo_logs_.size()) {
            fn(undo_logs_[log_id]);
        }
    }

    /** @return 撤销日志的数量 */
    inline auto GetUndoLogNum() -> size_t {
        std::scoped_lock<std::mutex> lck(latch_);
        return undo_logs_.size();
    }

    inline std::vector<UndoLog> TakeUndoLogs() {
        std::scoped_lock<std::mutex> lck(latch_);
        std::vector<UndoLog> logs;
        logs.swap(undo_logs_);
        return logs;
    }

    /**
     * Inspect one immutable committed undo edge for GC.  Once this log's
     * materialized version is visible to the oldest active snapshot, versions
     * behind it are unreachable by every active snapshot and the predecessor
     * ownership edge can be detached.
     */
    inline UndoGcLinkResult InspectOrCutUndoPredecessor(size_t log_id, timestamp_t watermark,
                                                        UndoLink *next, UndoLink *released) {
        std::scoped_lock<std::mutex> lck(latch_);
        if (next != nullptr) *next = {};
        if (released != nullptr) *released = {};
        if (log_id >= undo_logs_.size()) {
            return UndoGcLinkResult::MISSING;
        }
        UndoLog &log = undo_logs_[log_id];
        if (log.ts_ <= watermark) {
            if (released != nullptr) *released = log.prev_version_;
            log.prev_version_ = {};
            return UndoGcLinkResult::BOUNDARY;
        }
        if (next != nullptr) *next = log.prev_version_;
        return UndoGcLinkResult::CONTINUE;
    }

    inline void RetainUndoReference() {
        undo_ref_count_.fetch_add(1, std::memory_order_acq_rel);
    }

    /** Release one owner and report whether this was the last reference. */
    inline bool ReleaseUndoReference() {
        rmdb::i64 current = undo_ref_count_.load(std::memory_order_acquire);
        while (true) {
            if (current <= 0) {
                throw InternalError("Transaction undo reference count underflow");
            }
            if (undo_ref_count_.compare_exchange_weak(
                    current, current - 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
                return current == 1;
            }
        }
    }

    inline rmdb::i64 GetUndoReferenceCount() const {
        return undo_ref_count_.load(std::memory_order_acquire);
    }

    inline bool TryMarkUndoReadyEnqueued() {
        bool expected = false;
        return undo_ready_enqueued_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_acquire);
    }

    inline void ClearUndoReadyEnqueued() {
        undo_ready_enqueued_.store(false, std::memory_order_release);
    }

private:
    static std::string serializable_write_key(const std::string& tab_name, const Rid& rid) {
        return tab_name + "#" + std::to_string(rid.page_no) + ":" + std::to_string(rid.slot_no);
    }

    bool txn_mode_{false};  // false 表示单条 SQL 的隐式事务，true 表示由 BEGIN 显式开启
    bool lightweight_read_only_{false};
    std::atomic<TransactionState> state_{
        TransactionState::DEFAULT};   // 可被 checkpoint/SSI/GC 并发观察
    IsolationLevel isolation_level_;  // 事务的隔离级别，默认隔离级别为快照隔离
    bool watermark_registered_{false};
    std::atomic<bool> gc_ready_{false};
    bool admission_active_{false};
    std::atomic<bool> ssi_metadata_released_{true};
    std::thread::id thread_id_{std::this_thread::get_id()};  // 创建事务的线程
    std::atomic<lsn_t> prev_lsn_{INVALID_LSN};  // checkpoint 可并发读取的事务 WAL 链尾
    std::atomic<lsn_t> first_lsn_{INVALID_LSN}; // WAL 回收必须保留的事务链起点
    // 快照可见提交的 WAL 持久化依赖；不改变 read_ts/SI 语义，只约束成功 ACK。
    std::atomic<lsn_t> snapshot_durability_lsn_{INVALID_LSN};
    std::shared_ptr<TransactionResponseGate> response_gate_;
    std::vector<std::shared_ptr<TransactionResponseGate>> response_dependencies_;
    bool response_gate_deferred_{false};
    txn_id_t txn_id_;                   // 事务的ID，唯一标识符
    timestamp_t start_ts_{INVALID_TS};  // 事务开始时间戳；begin() 成功前保持无效值

    std::vector<WriteRecord> write_set_;          // 事务包含的所有写操作，首次写入时才分配
    std::vector<lock_data_key_t> lock_set_;       // 事务申请的所有锁，首次加锁时才分配
    std::vector<lock_data_key_t> pre_snapshot_write_keys_;  // read_ts 前取得的逻辑写准入令牌
    // 所有候选逻辑写键（包括准入超时后的原生 SI 回退）用于按真实提交顺序
    // 串联客户端响应；与准入令牌的生命周期独立。
    std::vector<lock_data_key_t> response_order_keys_;

    std::atomic<timestamp_t> read_ts_{0};
    /** 提交时间戳 */
    std::atomic<timestamp_t> commit_ts_{INVALID_TS};
    std::unordered_map<std::string, std::vector<Rid>> read_records_;
    std::vector<PredicateRead> predicate_reads_;
    std::unordered_set<txn_id_t> rw_dependencies_;
    std::unordered_map<std::string, SerializableWriteInfo> serializable_writes_;
    /** 仍指向本事务 undo 日志的页头链接和跨事务链接数量。 */
    std::atomic<rmdb::i64> undo_ref_count_{0};
    /** A zero-reference transaction may have at most one ready-queue entry. */
    std::atomic<bool> undo_ready_enqueued_{false};
    /**
     * @brief 存储撤销日志。
     * 其他撤销日志/表堆将存储 (txn_id, index)
     * 对，因此只能向此vector中追加内容或就地更新内容，而不能删除任何内容。
     */
    std::vector<UndoLog> undo_logs_;
    /** 用于访问事务级撤销日志的锁。 */
    std::mutex latch_;
};
