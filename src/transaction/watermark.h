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
#include <mutex>
#include <set>

#include "transaction/transaction.h"


/**
 * @brief 追踪所有的读时间戳
 *
 */
class Watermark {
public:
  explicit Watermark(timestamp_t commit_ts) : commit_ts_(commit_ts), watermark_(commit_ts) {}

  void AddTxn(timestamp_t read_ts);

  void RemoveTxn(timestamp_t read_ts);

  /** 在一个临界区内发布提交前沿并移除已结束事务。 */
  void FinishTxn(timestamp_t read_ts, timestamp_t commit_ts);

  timestamp_t GetWatermark();

  /** 诊断:当前登记的快照数(活跃事务 + 残留泄漏)。 */
  size_t Count() {
    std::lock_guard<std::mutex> guard(latch_);
    return current_reads_.size();
  }

  mutable timestamp_t commit_ts_;

  // 原子水印:GetWatermark 无锁读(GC 每 2ms),写路径在 latch_ 内更新。
  std::atomic<timestamp_t> watermark_{0};

  std::multiset<timestamp_t> current_reads_;

private:
  std::mutex latch_;
};
