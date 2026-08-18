#pragma once

#include "common/common.h"
#include "record/rm_defs.h"

#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct TransactionRetiredTuple {
    // 稳定表标识:退役元组以 u32 引用表,替代 std::string tab_name,
    // 消除每条目的独立堆分配(约 60B+churn/条目)。
    rmdb::u32 table_id;
    Rid rid;
    txn_id_t txn_id{INVALID_TXN_ID};
    bool is_deleted{false};
    // A committed logical DELETE remains recoverable until GC physically removes
    // the heap slot.  Checkpoints retain this WAL position in the DPT meanwhile.
    lsn_t delete_lsn{INVALID_LSN};
};

struct TransactionLogicalDeleteKey {
    rmdb::u32 table_id{0};
    Rid rid{};

    bool operator==(const TransactionLogicalDeleteKey &other) const {
        return table_id == other.table_id && rid == other.rid;
    }
};

struct TransactionLogicalDeleteKeyHash {
    size_t operator()(const TransactionLogicalDeleteKey &key) const noexcept {
        size_t seed = std::hash<rmdb::u32>{}(key.table_id);
        seed ^= std::hash<int>{}(key.rid.page_no) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
        return seed ^ (std::hash<int>{}(key.rid.slot_no) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U));
    }
};

struct TransactionLogicalDelete {
    txn_id_t txn_id{INVALID_TXN_ID};
    lsn_t rec_lsn{INVALID_LSN};
};

struct TransactionRetiredCandidate {
    timestamp_t commit_ts{INVALID_TS};
    TransactionRetiredTuple tuple{};
};

constexpr size_t kTransactionLogicalDeleteShardCount = 64;
constexpr size_t kTransactionReadyShardCount = 64;

struct alignas(64) TransactionLogicalDeleteShard {
    std::mutex mutex;
    std::unordered_map<TransactionLogicalDeleteKey, TransactionLogicalDelete,
                       TransactionLogicalDeleteKeyHash> deletes;
};

inline size_t TransactionLogicalDeleteShardFor(const TransactionLogicalDeleteKey &key) {
    static_assert((kTransactionLogicalDeleteShardCount &
                   (kTransactionLogicalDeleteShardCount - 1)) == 0,
                  "logical DELETE shard count must be a power of two");
    return TransactionLogicalDeleteKeyHash{}(key) &
           (kTransactionLogicalDeleteShardCount - 1);
}

struct alignas(64) TransactionReadyShard {
    std::mutex mutex;
    std::deque<txn_id_t> transactions;
};

inline size_t TransactionReadyShardFor(txn_id_t txn_id) {
    static_assert((kTransactionReadyShardCount & (kTransactionReadyShardCount - 1)) == 0,
                  "ready shard count must be a power of two");
    return std::hash<txn_id_t>{}(txn_id) & (kTransactionReadyShardCount - 1);
}

struct TransactionGcState {
    std::mutex run_mutex;
    std::mutex queue_mutex;
    // At most one outstanding cleanup responsibility exists for each RID.
    // A newer commit replaces the candidate in place while the queue keeps a
    // single key, so hot rows cannot create an unbounded retry-token stream.
    std::unordered_map<TransactionLogicalDeleteKey, TransactionRetiredCandidate,
                       TransactionLogicalDeleteKeyHash> retired_candidates;
    std::deque<TransactionLogicalDeleteKey> retired_candidate_queue;
    std::atomic<size_t> retired_tuple_count{0};
    // Only exceptional retry work (currently SSI retention) lives here.
    // Ordinary finished transactions are published exactly once when their
    // undo reference count reaches zero.
    std::deque<txn_id_t> finished_transactions;
    std::array<TransactionReadyShard, kTransactionReadyShardCount> ready_shards;
    std::atomic<size_t> ready_transaction_count{0};
    size_t next_ready_shard{0};
    std::array<TransactionLogicalDeleteShard, kTransactionLogicalDeleteShardCount>
        logical_delete_shards;
    std::chrono::steady_clock::time_point last_gc_trigger{};
};
