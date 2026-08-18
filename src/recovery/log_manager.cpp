/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include "log_manager.h"

namespace {
// Group commit ends when a waiter/byte threshold, deadline, immediate flush,
// or shutdown condition is reached. The deadline starts with the first waiter.
// Small batches may use a short bootstrap window so adjacent commits can join
// the same flush without extending the normal deadline indefinitely.
constexpr std::chrono::microseconds kBatchDeadlineUs{250};
constexpr std::chrono::microseconds kBootstrapDeadlineUs{1000};
constexpr size_t kTargetWaiters{8};
constexpr size_t kTargetWALBytes{64 * 1024};
constexpr size_t kBootstrapWaiters{4};
constexpr size_t kBootstrapWALBytes{16 * 1024};

inline void spin_pause() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    std::this_thread::yield();
#endif
}

bool PerAckBarrierEnabled() {
    // Optional per-ack sync for clients that require an acknowledgement barrier.
    // It is disabled by default because an extra fdatasync increases latency.
    static const bool enabled = [] {
        const char *raw = std::getenv("RMDB_PER_ACK_BARRIER");
        return raw != nullptr && (raw[0] == '1' || raw[0] == 'y' || raw[0] == 'Y' || raw[0] == 't' ||
                                  raw[0] == 'T' || raw[0] == 'o' || raw[0] == 'O');
    }();
    return enabled;
}

}

LogManager::LogManager(DiskManager *disk_manager) : disk_manager_(disk_manager) {
    persist_lsn_.store(INVALID_LSN, std::memory_order_relaxed);
    writer_thread_ = std::thread([this] { writer_loop(); });
}

LogManager::~LogManager() {
    try {
        flush_log_to_disk();
    } catch (...) {
        // WAL 写线程一旦失败便不可恢复，前台调用者已经收到该异常；析构仍须继续回收线程。
    }
    {
        std::lock_guard<std::mutex> lock(latch_);
        stop_writer_.store(true, std::memory_order_release);
        immediate_flush_requested_.store(true, std::memory_order_release);
    }
    writer_cv_.notify_one();
    group_sleep_cv_.notify_all();  // writer 可能正睡在组提交窗口内
    if (writer_thread_.joinable()) {
        writer_thread_.join();
    }
}

/**
 * @description: 添加日志记录到日志缓冲区中，并返回日志记录号
 * @param {LogRecord*} log_record 要写入缓冲区的日志记录
 * @return {lsn_t} 返回该日志的日志记录号（即该记录在日志文件中的字节偏移量）
 */
lsn_t LogManager::add_log_to_buffer(LogRecord* log_record) {
    std::unique_lock<std::mutex> lock(latch_);
    rethrow_writer_error_unlocked();
    if (log_record->log_tot_len_ > LOG_BUFFER_SIZE) {
        throw InternalError("WAL record exceeds log buffer size");
    }
    while (active_buffer_->is_full(log_record->log_tot_len_)) {
        immediate_flush_requested_.store(true, std::memory_order_release);
        writer_cv_.notify_one();
        group_sleep_cv_.notify_all();  // writer 可能正睡在组提交窗口内
        flush_cv_.wait(lock, [&] {
            return !active_buffer_->is_full(log_record->log_tot_len_) || writer_error_ != nullptr;
        });
        rethrow_writer_error_unlocked();
    }

    // LSN 就是记录在当前 WAL 文件中的起始字节偏移，既用于顺序读取，也用于按事务链回溯。
    log_record->lsn_ = active_start_offset_ + active_buffer_->offset_;
    log_record->serialize(active_buffer_->buffer_ + active_buffer_->offset_);
    active_buffer_->offset_ += log_record->log_tot_len_;
    next_log_offset_ = active_start_offset_ + active_buffer_->offset_;
    return log_record->lsn_;
}

/**
 * @description: 把日志缓冲区的内容刷到磁盘中，由于目前只设置了一个缓冲区，因此需要阻塞其他日志操作
 */
void LogManager::flush_log_to_disk() {
    lsn_t target_lsn = INVALID_LSN;
    {
        std::lock_guard<std::mutex> lock(latch_);
        target_lsn = next_log_offset_ - 1;
    }
    request_flush_and_wait(target_lsn, false);
}

void LogManager::flush_log_to_disk_until(lsn_t target_lsn) {
    request_flush_and_wait(target_lsn, false);
}

void LogManager::flush_log_to_disk_until_group(lsn_t target_lsn) {
    request_flush_and_wait(target_lsn, true);
    // 组提交通常由同一次持久化覆盖多个确认。需要独立确认屏障的客户端可通过
    // RMDB_PER_ACK_BARRIER=1 为每个确认追加一次同步。
    if (PerAckBarrierEnabled() && target_lsn != INVALID_LSN) {
        disk_manager_->sync_log_file();
    }
}

void LogManager::request_flush_and_wait(lsn_t target_lsn, bool group_commit) {
    // 强制刷盘/页淘汰可以先无锁自旋，避免完成通知后争抢 latch_。组提交则
    // 必须立即登记为 waiter；先自旋会消耗聚合窗口并把相邻提交拆成小批。
    if (!group_commit && target_lsn != INVALID_LSN &&
        target_lsn > persist_lsn_.load(std::memory_order_acquire)) {
        constexpr int kFlushSpinIterations = 400;  // ~40-80μs
        for (int i = 0; i < kFlushSpinIterations &&
                            target_lsn > persist_lsn_.load(std::memory_order_acquire);
             ++i) {
            spin_pause();
        }
    }
    std::unique_lock<std::mutex> lock(latch_);
    rethrow_writer_error_unlocked();
    if (target_lsn == INVALID_LSN || target_lsn <= persist_lsn_.load(std::memory_order_acquire)) {
        return;
    }
    // WAL 重置前已经保证全部数据页持久化，但页头仍可能保留上一代日志的 LSN。
    // 换出该页时若请求的偏移超出当前代末尾，应把它视作已经满足；继续等待会让写线程
    // 永远追赶一个当前 WAL 不可能到达的 requested_lsn_。
    if (target_lsn >= next_log_offset_) {
        return;
    }
    requested_lsn_ = std::max(requested_lsn_, target_lsn);
    if (group_commit) {
        if (target_lsn >= active_start_offset_) {
            // Count arrivals for the current active buffer only. Requests whose
            // target is already in flush_buffer wait for that I/O but must not
            // make the next generation look artificially full.
            if (group_waiter_count_.load(std::memory_order_relaxed) == 0) {
                // 空闲期后的第一个等待者:记录到达时刻,供聚合截止时间起算。
                group_first_waiter_at_ = std::chrono::steady_clock::now();
            }
            group_waiter_count_.fetch_add(1, std::memory_order_release);
        }
    } else {
        immediate_flush_requested_.store(true, std::memory_order_release);
    }
    writer_cv_.notify_one();
    group_sleep_cv_.notify_all();  // writer 可能正睡在组提交窗口内
    flush_cv_.wait(lock, [&] {
        return target_lsn <= persist_lsn_.load(std::memory_order_acquire) || writer_error_ != nullptr;
    });
    rethrow_writer_error_unlocked();
}

void LogManager::writer_loop() noexcept {
    std::unique_lock<std::mutex> lock(latch_);
    while (true) {
        writer_cv_.wait(lock, [&] {
            return stop_writer_.load(std::memory_order_relaxed) ||
                   immediate_flush_requested_.load(std::memory_order_relaxed) ||
                   (requested_lsn_ != INVALID_LSN &&
                    requested_lsn_ > persist_lsn_.load(std::memory_order_acquire));
        });
        if (writer_error_ != nullptr) {
            return;
        }
        if (stop_writer_.load(std::memory_order_relaxed) && active_buffer_->offset_ == 0) {
            return;
        }
        if (active_buffer_->offset_ == 0) {
            if (stop_writer_.load(std::memory_order_relaxed)) {
                return;
            }
            immediate_flush_requested_.store(false, std::memory_order_relaxed);
            continue;
        }

        if (!stop_writer_.load(std::memory_order_relaxed) &&
            !immediate_flush_requested_.load(std::memory_order_relaxed) &&
            group_waiter_count_.load(std::memory_order_relaxed) > 0) {
            // Wait for the current batch to reach a size or time limit.
            const auto agg_start = std::chrono::steady_clock::now();
            // Recompute the deadline after the bootstrap threshold changes.
            const auto batch_deadline = std::max(agg_start, group_first_waiter_at_ + kBatchDeadlineUs);
            while (true) {
                if (stop_writer_.load(std::memory_order_relaxed) ||
                    immediate_flush_requested_.load(std::memory_order_relaxed)) {
                    break;
                }
                if (group_waiter_count_.load(std::memory_order_relaxed) >= kTargetWaiters) {
                    break;
                }
                if (static_cast<size_t>(active_buffer_->offset_) >= kTargetWALBytes) {
                    break;
                }
                const auto waiters = group_waiter_count_.load(std::memory_order_relaxed);
                const auto active_bytes = static_cast<size_t>(active_buffer_->offset_);
                auto deadline = batch_deadline;
                const bool bootstrap_batch =
                    waiters < kBootstrapWaiters && active_bytes < kBootstrapWALBytes;
                if (bootstrap_batch) {
                    deadline = std::max(deadline, agg_start + kBootstrapDeadlineUs);
                }
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline) {
                    break;
                }
                lock.unlock();
                {
                    std::unique_lock<std::mutex> sleep_lock(group_sleep_mutex_);
                    // During the bootstrap window, wake at the bootstrap threshold;
                    // afterwards use the normal target. The predicate also avoids
                    // losing a notification between unlock and wait.
                    const size_t wake_waiters = bootstrap_batch ? kBootstrapWaiters : kTargetWaiters;
                    group_sleep_cv_.wait_for(sleep_lock, deadline - now, [&] {
                        return stop_writer_.load(std::memory_order_relaxed) ||
                               immediate_flush_requested_.load(std::memory_order_relaxed) ||
                               group_waiter_count_.load(std::memory_order_relaxed) >= wake_waiters;
                    });
                }
                lock.lock();
            }
        }

        std::swap(active_buffer_, flush_buffer_);
        flush_start_offset_ = active_start_offset_;
        flush_size_ = flush_buffer_->offset_;
        active_start_offset_ = flush_start_offset_ + flush_size_;
        // group_waiter_count_ is an arrival count for the buffer just swapped
        // out, not a live waiter count. New requests during this I/O belong to
        // the fresh active generation and start again from zero.
        group_waiter_count_.store(0, std::memory_order_release);
        flush_in_progress_ = true;
        immediate_flush_requested_.store(false, std::memory_order_relaxed);

        char *flush_data = flush_buffer_->buffer_;
        const int flush_size = flush_size_;
        const lsn_t flush_start = flush_start_offset_;
        lock.unlock();
        try {
            disk_manager_->write_log(flush_data, static_cast<size_t>(flush_size), flush_start);
            disk_manager_->sync_log();
        } catch (...) {
            lock.lock();
            writer_error_ = std::current_exception();
            flush_in_progress_ = false;
            flush_cv_.notify_all();
            return;
        }
        lock.lock();

        persist_lsn_.store(flush_start + flush_size - 1, std::memory_order_release);
        flush_buffer_->reset();
        flush_size_ = 0;
        flush_in_progress_ = false;
        if (requested_lsn_ <= persist_lsn_.load(std::memory_order_relaxed)) {
            requested_lsn_ = INVALID_LSN;
        }
        flush_cv_.notify_all();
    }
}

void LogManager::rethrow_writer_error_unlocked() const {
    if (writer_error_ != nullptr) {
        std::rethrow_exception(writer_error_);
    }
}

lsn_t LogManager::get_log_file_offset() {
    std::lock_guard<std::mutex> lock(latch_);
    return next_log_offset_;
}

lsn_t LogManager::get_persist_lsn() {
    // The writer publishes this watermark only after write+sync completes.
    // Readers need the watermark, not a consistent snapshot of buffer state;
    // taking the append latch here serializes BPM eviction with every WAL append.
    return persist_lsn_.load(std::memory_order_acquire);
}

void LogManager::reset_log_file_offset(lsn_t log_file_offset) {
    std::unique_lock<std::mutex> lock(latch_);
    rethrow_writer_error_unlocked();
    while (flush_in_progress_) {
        flush_cv_.wait(lock);
    }
    active_buffer_->reset();
    flush_buffer_->reset();
    active_start_offset_ = log_file_offset;
    next_log_offset_ = log_file_offset;
    persist_lsn_.store(log_file_offset > 0 ? log_file_offset - 1 : INVALID_LSN,
                       std::memory_order_release);
    requested_lsn_ = INVALID_LSN;
    immediate_flush_requested_.store(false, std::memory_order_relaxed);
    group_waiter_count_.store(0, std::memory_order_relaxed);
    group_first_waiter_at_ = std::chrono::steady_clock::time_point{};
}
