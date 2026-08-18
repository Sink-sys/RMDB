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

#include <atomic>
#include <chrono>
#include <array>
#include <condition_variable>
#include <deque>
#include <unordered_map>
#include <optional>
#include <functional>
#include <thread>
#include <memory>
#include <shared_mutex>
#include <utility>
#include <vector>

#include "transaction.h"
#include "transaction_registry.h"
#include "transaction_gc_state.h"
#include "transaction_lifecycle_state.h"
#include "transaction_version_state.h"
#include "transaction_version_storage.h"
#include "snapshot_index_history_store.h"
#include "watermark.h"
#include "recovery/log_manager.h"
#include "concurrency/lock_manager.h"
#include "system/sm_manager.h"
#include "common/exception.h"
#include "common/index_runtime.h"
#include "common/types.h"

/* 系统采用的并发控制算法，当前题目中要求两阶段封锁并发控制算法 */
enum class ConcurrencyMode { TWO_PHASE_LOCKING = 0, BASIC_TO, MVCC };


class RmRecordPageCursor;
class TransactionManager;

class TransactionDrainGuard {
   public:
    TransactionDrainGuard() = default;
    ~TransactionDrainGuard();

    TransactionDrainGuard(const TransactionDrainGuard &) = delete;
    TransactionDrainGuard &operator=(const TransactionDrainGuard &) = delete;

    TransactionDrainGuard(TransactionDrainGuard &&other) noexcept;
    TransactionDrainGuard &operator=(TransactionDrainGuard &&other) noexcept;

    explicit operator bool() const { return manager_ != nullptr; }

   private:
    friend class TransactionManager;
    explicit TransactionDrainGuard(TransactionManager *manager) : manager_(manager) {}
    void reset();

    TransactionManager *manager_{nullptr};
};
class TransactionManager{
public:
    enum class UniqueKeyConflictResult { NONE, ABORT, FAILURE };
    enum class IndexEntryVisibilityState { CURRENT_KEY_VISIBLE, INVISIBLE, NEEDS_HEAP };
    using PageVersionInfo = TransactionPageVersionInfo;
    using TableVersionInfo = TransactionTableVersionInfo;
    struct IndexEntryTupleHint {
        bool valid = false;
        page_id_t page_no = INVALID_PAGE_ID;
        int slot_no = -1;
        PageVersionInfo *page_info = nullptr;
        rmdb::u64 visibility_epoch = 0;
        bool has_meta = false;
        TupleMeta meta{0, false};
        bool has_version_link = false;
        VersionUndoLink version_link{};

        void Reset() {
            valid = false;
            page_no = INVALID_PAGE_ID;
            slot_no = -1;
            page_info = nullptr;
            visibility_epoch = 0;
            has_meta = false;
            meta = TupleMeta{0, false};
            has_version_link = false;
            version_link = VersionUndoLink{};
        }

        bool Matches(const Rid &rid) const {
            return valid && page_no == rid.page_no && slot_no == rid.slot_no && page_info != nullptr;
        }
    };

    explicit TransactionManager(LockManager *lock_manager, SmManager *sm_manager,
                             ConcurrencyMode concurrency_mode = ConcurrencyMode::TWO_PHASE_LOCKING) {
        sm_manager_ = sm_manager;
        lock_manager_ = lock_manager;
        concurrency_mode_ = concurrency_mode;
    }
    
    ~TransactionManager();

    Transaction* begin(Transaction* txn, LogManager* log_manager, IsolationLevel isolation_level,
                       const std::vector<lock_data_key_t> &pre_snapshot_write_keys = {});

    Transaction* begin_read_only(IsolationLevel isolation_level, bool reserve_txn_id = false);

    void PromoteToWrite(Transaction *txn, LogManager *log_manager);

    void commit(Transaction* txn, LogManager* log_manager);

    void abort(Transaction* txn, LogManager* log_manager);

    void PhysicalizeCommittedDeletes();

    bool UpdateTupleMeta(const std::string &tab_name, Rid rid, std::optional<TupleMeta> meta,
                         std::function<bool(std::optional<TupleMeta>)> &&check = nullptr);

    std::optional<TupleMeta> GetTupleMeta(const std::string &tab_name, Rid rid);

    TupleMeta GetTupleMetaOrDefault(const std::string &tab_name, Rid rid);

    std::shared_ptr<PageVersionInfo> GetPageVersionInfo(const std::string &tab_name, page_id_t page_no);
    std::shared_ptr<TableVersionInfo> GetTableVersionInfo(const std::string &tab_name);
    std::shared_ptr<TableVersionInfo> GetTableVersionInfoById(rmdb::u32 table_id);
    std::shared_ptr<TableVersionInfo> GetOrCreateTableVersionInfo(const std::string &tab_name);
    std::shared_ptr<PageVersionInfo> GetPageVersionInfoOnTable(
        const std::shared_ptr<TableVersionInfo> &table_info, page_id_t page_no);
    std::shared_ptr<PageVersionInfo> GetPageVersionInfoOnTable(
        const std::shared_ptr<TableVersionInfo> &table_info, page_id_t page_no, rmdb::u64 *page_map_epoch);
    PageVersionInfo *GetPageVersionInfoOnTableRaw(
        const std::shared_ptr<TableVersionInfo> &table_info, page_id_t page_no, rmdb::u64 *page_map_epoch = nullptr);

    std::optional<RmRecord> GetVisibleTuple(const std::string &tab_name, const Rid &rid, Transaction *txn,
                                            TupleMeta *visible_meta = nullptr);

    bool GetVisibleTupleInto(const std::string &tab_name, const Rid &rid, Transaction *txn, RmRecord *out_record,
                            TupleMeta *visible_meta = nullptr, RmRecordPageCursor *page_cursor = nullptr);

    bool GetVisibleTupleInto(const std::string &tab_name,
                            const std::shared_ptr<TableVersionInfo> &table_info,
                            const Rid &rid, Transaction *txn, RmRecord *out_record,
                            TupleMeta *visible_meta = nullptr, RmRecordPageCursor *page_cursor = nullptr);

    IndexEntryVisibilityState ClassifySnapshotIndexEntryOnPage(
        PageVersionInfo *page_info, const Rid &rid, Transaction *txn,
        IndexEntryTupleHint *hint = nullptr);
    IndexEntryVisibilityState ClassifySnapshotIndexEntryState(bool has_meta,
                                                                  const TupleMeta &meta,
                                                                  bool has_version_link,
                                                                  Transaction *txn) const;
    bool IsSnapshotPageCleanVisible(const PageVersionInfo *page_info, Transaction *txn) const;

    void EnsureWriteConflictFree(Transaction *txn, const std::string &tab_name, const Rid &rid);

    void EnsureWriteConflictFree(Transaction *txn, const TupleMeta &current_meta);

    void EnsureKeyConflictFree(Transaction *txn, const std::string &tab_name, const TabMeta &tab,
                               const RmRecord &record, const Rid *self_rid = nullptr,
                               const std::vector<rmdb::IndexBinding> *prebound_bindings = nullptr);

    UniqueKeyConflictResult ClassifyUniqueIndexConflict(Transaction *txn, const std::string &tab_name,
                                                        const IndexMeta &index, const char *key,
                                                        const std::vector<Rid> &matches,
                                                        const Rid *self_rid = nullptr);

    void RecordSnapshotIndexRetirement(const std::string &tab_name, const IndexMeta &index,
                                       const Rid &rid, timestamp_t retire_ts);
    std::vector<Rid> LookupSnapshotIndexHistory(const std::string &tab_name, const IndexMeta &index,
                                                timestamp_t read_ts);
    void EraseSnapshotIndexHistory(const std::string &tab_name, const IndexMeta &index);
    void EraseSnapshotIndexHistory(const std::string &tab_name);
    void ClearSnapshotIndexHistory();
    void EraseTableState(const std::string &tab_name);

    void RecordSerializableRead(Transaction *txn, const std::string &tab_name, const Rid &rid);

    void RecordSerializablePredicateRead(Transaction *txn, const std::string &tab_name,
                                         const std::vector<ColMeta> &cols,
                                         const std::vector<Condition> &conds);

    void RecordSerializableWrite(Transaction *txn, const std::string &tab_name, const Rid &rid,
                                 const RmRecord *old_record, const RmRecord *new_record,
                                 const std::vector<ColMeta> *cols);

    ConcurrencyMode get_concurrency_mode() { return concurrency_mode_; }

    void set_concurrency_mode(ConcurrencyMode concurrency_mode) { concurrency_mode_ = concurrency_mode; }

    LockManager* get_lock_manager() { return lock_manager_; }

    struct CheckpointTxnTableSnapshot {
        std::vector<CheckpointTxnEntry> entries;
        lsn_t oldest_first_lsn{INVALID_LSN};
    };

    CheckpointTxnTableSnapshot CollectCheckpointTxnTable(Transaction *exclude_txn = nullptr);

    std::vector<CheckpointDirtyPageInfo> CollectCheckpointLogicalDeletePages();

    void UntrackLogicalDelete(const std::string &tab_name, const Rid &rid, txn_id_t txn_id);

    TransactionDrainGuard BlockNewTransactionsAndWait();

    TransactionDrainGuard TryBlockNewTransactionsIfIdle();

    TransactionDrainGuard BlockNewTransactionsAndWaitFor(std::chrono::milliseconds timeout);

    size_t ActiveTransactionCount() const;

    txn_id_t NextTransactionId() const {
        return lifecycle_state_.next_txn_id.load(std::memory_order_acquire);
    }

    size_t RegisteredTransactionCount() const {
        return transaction_registry_.Size();
    }

    StatementCheckpointGate::ReadGuard EnterStatementExecution() {
        return lifecycle_state_.checkpoint_gate.ReadEnter();
    }

    StatementCheckpointGate::WriteGuard EnterCheckpointExecution() {
        return lifecycle_state_.checkpoint_gate.WriteEnter();
    }

    /**
     * @description: 获取事务ID为txn_id的事务对象
     * @return {Transaction*} 事务对象的指针
     * @param {txn_id_t} txn_id 事务ID
     */    
    Transaction* get_transaction(txn_id_t txn_id) {
        return transaction_registry_.GetThreadOwned(txn_id);
    }

    void EnsureNextTransactionIdAtLeast(txn_id_t next_txn_id) {
        txn_id_t current = lifecycle_state_.next_txn_id.load(std::memory_order_relaxed);
        while (current < next_txn_id &&
               !lifecycle_state_.next_txn_id.compare_exchange_weak(
                   current, next_txn_id, std::memory_order_relaxed, std::memory_order_relaxed)) {
        }
    }
    /** ------------------------以下函数仅可能在MVCC当中使用------------------------------------------*/

    /**
    * @brief 更新一个撤销链接，该链接将表堆元组与第一个撤销日志连接起来。
    * 在更新之前，将调用 `check` 函数以确保有效性。
    */
    bool UpdateUndoLink(const std::string &tab_name, Rid rid, std::optional<UndoLink> prev_link,
                        std::function<bool(std::optional<UndoLink>)> &&check = nullptr,
                        Transaction *self = nullptr);

    /**
     * @brief 更新一个撤销链接，该链接将表堆元组与第一个撤销日志连接起来。
     * 在更新之前，将调用 `check` 函数以确保有效性。
     */
    bool UpdateVersionLink(const std::string &tab_name, Rid rid, std::optional<VersionUndoLink> prev_version,
                           std::function<bool(std::optional<VersionUndoLink>)> &&check = nullptr,
                           Transaction *self = nullptr);

    void InstallTupleVersion(const std::string &tab_name, Rid rid, const VersionUndoLink &version,
                             const TupleMeta &meta, Transaction *self = nullptr);

    /** @brief 获取表堆元组的第一个撤销日志。 */
    std::optional<UndoLink> GetUndoLink(const std::string &tab_name, Rid rid);

    /** @brief 获取表堆元组的第一个撤销日志。*/
    std::optional<VersionUndoLink> GetVersionLink(const std::string &tab_name, Rid rid);

    /** @brief 访问事务撤销日志缓冲区并获取撤销日志。如果事务不存在，返回 nullopt。
     * 如果索引超出范围仍然会抛出异常。 */
    std::optional<UndoLog> GetUndoLogOptional(UndoLink link);

    /**
     * Append an undo log whose predecessor is read and retained atomically
     * under the tuple version-page latch. This transfers a stable ownership
     * edge from the page head into the new undo record before GC may clear it.
     */
    UndoLink AppendUndoLog(const std::string &tab_name, Rid rid, Transaction *txn, UndoLog log);

    /** @brief 访问事务撤销日志缓冲区并获取撤销日志。除非访问当前事务缓冲区，
     * 否则应该始终调用此函数以获取撤销日志，而不是手动检索事务 shared_ptr 并访问缓冲区。 */
    UndoLog GetUndoLog(UndoLink link);

    /** @brief 零拷贝读取撤销日志的元数据(ts_/is_deleted_/prev_version_),
     * 供 abort 回滚等只读标量的路径使用,避免整份 UndoLog 值拷贝。 */
    bool PeekUndoLogMeta(UndoLink link, timestamp_t *ts, bool *is_deleted, UndoLink *prev_version);

    /** @brief 获取系统中的最低读时间戳。 */
    timestamp_t GetWatermark();

    /** @brief 垃圾回收。仅在所有事务都未访问时调用。 */
    void GarbageCollection();


private:
    friend class TransactionDrainGuard;

    // 快路径:原子递增活跃计数,仅当 drain 阻塞置位时才转入持锁等待。
    void ReserveTransactionAdmission();

    // 后台 GC:提交路径只入队,实际回收由 MaintenanceEngine(进程级后台
    // 组件)按固定 2ms tick 调度 GarbageCollection() 执行。

    using RetiredTuple = TransactionRetiredTuple;

    ConcurrencyMode concurrency_mode_;      // 事务使用的并发控制算法，目前只需要考虑2PL
    SmManager *sm_manager_;
    LockManager *lock_manager_;
    TransactionRegistry transaction_registry_;
    TransactionLifecycleState lifecycle_state_;
    TransactionVersionState version_state_;
    rmdb::SnapshotIndexHistoryStore snapshot_index_history_;

    std::shared_ptr<PageVersionInfo> GetOrCreatePageVersionInfo(const std::string &tab_name, page_id_t page_no);
    std::shared_ptr<PageVersionInfo> GetOrCreatePageVersionInfoOnTable(
        const std::shared_ptr<TableVersionInfo> &table_info, const std::string &tab_name, page_id_t page_no);
    void RetainUndoReference(const UndoLink &link);
    void ReleaseUndoReference(const UndoLink &link);

    /** 带调用者事务的版本:自链(link 指向调用者自己的撤销日志)直接更新引用计数,
     *  免去一次 registry 查找;跨事务链接仍走注册表。 */
    void RetainUndoReference(const UndoLink &link, Transaction *self);
    void ReleaseUndoReference(const UndoLink &link, Transaction *self);
    void PublishReadyTransaction(txn_id_t txn_id);
    void EnqueueRetiredTuples(timestamp_t commit_ts, std::vector<RetiredTuple> tuples);
    void UntrackLogicalDelete(rmdb::u32 table_id, const Rid &rid, txn_id_t txn_id);
    bool ReclaimRetiredTuple(timestamp_t commit_ts, timestamp_t watermark,
                             const RetiredTuple &retired);
    void QueueFinishedTransactionForGc(Transaction *txn);
    void GarbageCollectFinishedTransactions();
    void FinishTransactionAdmission(Transaction *txn);
    void CancelTransactionAdmissionReservation();
    void ReleaseTransactionDrain();

    TransactionGcState gc_state_;
};
