/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "transaction/watermark.h"


auto Watermark::AddTxn(timestamp_t read_ts) -> void {
    std::lock_guard<std::mutex> guard(latch_);
    current_reads_.insert(read_ts);
    if (read_ts < watermark_.load(std::memory_order_relaxed)) {
        watermark_.store(read_ts, std::memory_order_relaxed);
    }
}

auto Watermark::RemoveTxn(timestamp_t read_ts) -> void {
    std::lock_guard<std::mutex> guard(latch_);
    auto iter = current_reads_.find(read_ts);
    if (iter == current_reads_.end()) {
        return;
    }
    current_reads_.erase(iter);
    watermark_.store(current_reads_.empty() ? commit_ts_ : *current_reads_.begin(),
                     std::memory_order_relaxed);
}

auto Watermark::FinishTxn(timestamp_t read_ts, timestamp_t commit_ts) -> void {
    std::lock_guard<std::mutex> guard(latch_);
    // 提交前沿只前移:延迟到达的旧提交(从 publish 临界区外调用)不得让水印回退。
    if (commit_ts > commit_ts_) {
        commit_ts_ = commit_ts;
    }
    auto iter = current_reads_.find(read_ts);
    if (iter != current_reads_.end()) {
        current_reads_.erase(iter);
    }
    watermark_.store(current_reads_.empty() ? commit_ts_ : *current_reads_.begin(),
                     std::memory_order_relaxed);
}

auto Watermark::GetWatermark() -> timestamp_t {
    // 无锁原子读:GC 每 2ms 调用,消除与事务 begin/end(写锁)的锁竞争。
    return watermark_.load(std::memory_order_acquire);
}
