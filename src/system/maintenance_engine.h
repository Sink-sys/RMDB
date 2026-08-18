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
#include <functional>
#include <thread>

#include "common/config.h"

class TransactionManager;
class BufferPoolManager;
class DiskManager;
class LogManager;

/* 进程级后台维护组件:单个线程按时间片统一调度
     1. GC(墓碑 + finished 事务回收)       每 2ms
     2. 脏页刷盘                           每 50ms
     3. 动态 fuzzy checkpoint/WAL 回收    每 2s 检查水位
   相比多个独立后台线程,单线程避免互相挤占 CPU/锁,统一节流与生命周期。 */
class MaintenanceEngine {
   public:
    MaintenanceEngine(TransactionManager *txn_manager, BufferPoolManager *buffer_pool_manager,
                      DiskManager *disk_manager, LogManager *log_manager,
                      std::function<void()> checkpoint_callback = {});
    ~MaintenanceEngine();

    MaintenanceEngine(const MaintenanceEngine &) = delete;
    MaintenanceEngine &operator=(const MaintenanceEngine &) = delete;

    void Start();
    void Stop();

   private:
    void Loop();

    TransactionManager *txn_manager_;
    BufferPoolManager *buffer_pool_manager_;
    DiskManager *disk_manager_;
    LogManager *log_manager_;
    std::function<void()> checkpoint_callback_;

    std::thread thread_;
    std::atomic<bool> stop_{false};
    lsn_t last_checkpoint_attempt_lsn_{INVALID_LSN};
    std::chrono::steady_clock::time_point last_checkpoint_attempt_at_{};

    // 刷盘轮转游标(跨 tick 持续扫描缓冲池)。
    size_t next_frame_{0};
};
