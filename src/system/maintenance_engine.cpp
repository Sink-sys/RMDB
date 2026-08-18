/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "maintenance_engine.h"

#include <chrono>
#include <iostream>
#include <utility>

#include "storage/buffer_pool_manager.h"
#include "storage/disk_manager.h"
#include "transaction/transaction_manager.h"

namespace {
constexpr auto kTickInterval = std::chrono::milliseconds(2);
constexpr size_t kGcTicks = 1;             // GC 每 2ms
constexpr size_t kFlushTicks = 25;         // 刷盘每 50ms
// 0 = 禁用后台批量刷盘；脏页仍会在淘汰或显式 flush 时写回。
constexpr size_t kFlushPagesPerTick = 0;
// 保留宽扫描窗口,避免较稀疏的脏页因写回限额而长期不被发现。
constexpr size_t kFlushFramesPerTick = 32768;
constexpr auto kWalCheckpointInterval = std::chrono::seconds(2);
constexpr auto kWalCheckpointRetryInterval = std::chrono::seconds(10);
constexpr lsn_t kWalCheckpointBytes = 4 * DiskManager::WAL_SEGMENT_SIZE;
}  // namespace

MaintenanceEngine::MaintenanceEngine(TransactionManager *txn_manager,
                                     BufferPoolManager *buffer_pool_manager,
                                     DiskManager *disk_manager, LogManager *log_manager,
                                     std::function<void()> checkpoint_callback)
    : txn_manager_(txn_manager),
      buffer_pool_manager_(buffer_pool_manager),
      disk_manager_(disk_manager),
      log_manager_(log_manager),
      checkpoint_callback_(std::move(checkpoint_callback)) {}

MaintenanceEngine::~MaintenanceEngine() {
    Stop();
}

void MaintenanceEngine::Start() {
    if (thread_.joinable()) {
        return;
    }
    stop_.store(false, std::memory_order_relaxed);
    thread_ = std::thread([this] { Loop(); });
}

void MaintenanceEngine::Stop() {
    stop_.store(true, std::memory_order_release);
    if (thread_.joinable()) {
        thread_.join();
    }
}

void MaintenanceEngine::Loop() {
    size_t tick = 0;
    auto next_checkpoint_check = std::chrono::steady_clock::now() + kWalCheckpointInterval;
    while (!stop_.load(std::memory_order_relaxed)) {
        ++tick;
        if (tick % kGcTicks == 0 && txn_manager_ != nullptr) {
            try {
                txn_manager_->GarbageCollection();
            } catch (const std::exception &error) {
                std::cerr << "maintenance GC error: " << error.what() << "\n";
            }
        }
        if (tick % kFlushTicks == 0 && buffer_pool_manager_ != nullptr) {
            try {
                bool pass_complete = false;
                buffer_pool_manager_->flush_unpinned_pages_batch(kFlushPagesPerTick,
                                                                 kFlushFramesPerTick, &next_frame_,
                                                                 &pass_complete, false);
                if (pass_complete) {
                    next_frame_ = 0;
                }
            } catch (const std::exception &error) {
                std::cerr << "maintenance flush error: " << error.what() << "\n";
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_checkpoint_check && disk_manager_ != nullptr &&
            log_manager_ != nullptr && checkpoint_callback_) {
            next_checkpoint_check = now + kWalCheckpointInterval;
            try {
                const lsn_t first_lsn = disk_manager_->get_first_log_lsn();
                const lsn_t end_lsn = disk_manager_->get_log_end_lsn();
                const lsn_t retained_bytes = end_lsn - first_lsn;
                const bool enough_growth =
                    last_checkpoint_attempt_lsn_ == INVALID_LSN ||
                    end_lsn - last_checkpoint_attempt_lsn_ >= kWalCheckpointBytes;
                const bool retry_due =
                    last_checkpoint_attempt_lsn_ != INVALID_LSN &&
                    now - last_checkpoint_attempt_at_ >= kWalCheckpointRetryInterval;
                if (retained_bytes >= kWalCheckpointBytes && (enough_growth || retry_due)) {
                    // Record the attempt before entering the callback so repeated failures or
                    // an old active transaction cannot turn into a 2-second fsync storm.
                    last_checkpoint_attempt_lsn_ = end_lsn;
                    last_checkpoint_attempt_at_ = now;
                    checkpoint_callback_();
                    const lsn_t retained_after = disk_manager_->get_first_log_lsn();
                    // A long transaction or an unphysicalized logical DELETE can pin the
                    // reclaim horizon for minutes.  WAL growth alone must not make us copy
                    // and sort the same ATT/DPT every two seconds while that horizon is
                    // unchanged.  Successful segment progress keeps the normal cadence.
                    next_checkpoint_check = std::chrono::steady_clock::now() +
                                            (retained_after > first_lsn
                                                 ? kWalCheckpointInterval
                                                 : kWalCheckpointRetryInterval);
                }
            } catch (const std::exception &error) {
                std::cerr << "maintenance checkpoint error: " << error.what() << "\n";
                next_checkpoint_check =
                    std::chrono::steady_clock::now() + kWalCheckpointRetryInterval;
            }
        }
        std::this_thread::sleep_for(kTickInterval);
    }
}
