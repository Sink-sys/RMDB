#pragma once

#include "common/common.h"
#include "common/types.h"
#include "statement_checkpoint_gate.h"
#include "watermark.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <shared_mutex>

struct TransactionLifecycleState {
    // 热路径计数器:每笔 begin/commit 多次原子 RMW,独占一条缓存行,
    // 避免与锁/CV 内部状态、Watermark 形成伪共享。
    alignas(64) std::atomic<txn_id_t> next_txn_id{0};
    std::atomic<timestamp_t> next_timestamp{0};
    std::atomic<rmdb::u64> next_commit_ticket{0};
    std::atomic<rmdb::u64> next_publish_ticket{0};  // 发布序号；锁内推进，等待者可无锁自旋观察
    std::atomic<bool> admission_blocked{false};
    std::atomic<size_t> active_transaction_count{0};
    // 已发布可见版本所依赖的最大 WAL 字节。发布时先推进它、再以 release
    // 推进 last_commit_ts；begin 先读 read_ts、再读此值，最多保守多等，
    // 不会遗漏其快照已观察到的提交依赖。
    std::atomic<lsn_t> last_published_commit_lsn{INVALID_LSN};
    std::atomic<timestamp_t> last_commit_ts{0};
    // 锁/CV/水位与计数器分线,避免锁操作写放大热计数器缓存行。
    alignas(64) std::mutex commit_publish_mutex;
    std::condition_variable commit_publish_cv;
    StatementCheckpointGate checkpoint_gate;
    mutable std::mutex admission_mutex;
    std::condition_variable admission_cv;
    // 日常路径(无 WAL reset 阻塞)下 active 计数与 blocked 标志均为原子操作,
    // 不触碰 mutex/cv;仅在 drain 阻塞期间才走持锁慢路径。
    Watermark running_txns{0};
    // 注册表中活跃的 SERIALIZABLE 事务数:GC 每 2ms 构造
    // SerializableMetadataRetention 需遍历全注册表(O(N)),SI 负载(计数为 0)
    // 时跳过构造,消除每 tick 的全表遍历。
    std::atomic<size_t> active_serializable_count_{0};
};
