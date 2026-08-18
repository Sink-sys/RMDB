#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <utility>

// 语句级 checkpoint 闸门:替代每语句一次的 shared_mutex。
//
// 读者(语句执行)走原子计数快路径,日常零锁;写者(WAL reset / SQL
// CHECKPOINT)置位 writer 标志后等待读者归零。readers_ 与 writer_ 使用
// seq_cst 保证跨两个原子的"先递增、后复查":递增若排在 writer 置位之前,
// writer 会观察到该读者;否则读者会观察到 writer 并撤销增量等待。
class StatementCheckpointGate {
   public:
    class ReadGuard {
       public:
        ReadGuard() = default;
        explicit ReadGuard(StatementCheckpointGate *gate) : gate_(gate) {}
        ReadGuard(const ReadGuard &) = delete;
        ReadGuard &operator=(const ReadGuard &) = delete;
        ReadGuard(ReadGuard &&other) noexcept : gate_(other.gate_) { other.gate_ = nullptr; }
        ReadGuard &operator=(ReadGuard &&other) noexcept {
            if (this != &other) {
                Release();
                gate_ = other.gate_;
                other.gate_ = nullptr;
            }
            return *this;
        }
        ~ReadGuard() { Release(); }

       private:
        void Release() {
            if (gate_ != nullptr) {
                gate_->ReadExit();
                gate_ = nullptr;
            }
        }
        StatementCheckpointGate *gate_ = nullptr;
    };

    class WriteGuard {
       public:
        WriteGuard() = default;
        WriteGuard(StatementCheckpointGate *gate, std::unique_lock<std::mutex> writer_lock)
            : gate_(gate), writer_lock_(std::move(writer_lock)) {}
        WriteGuard(const WriteGuard &) = delete;
        WriteGuard &operator=(const WriteGuard &) = delete;
        WriteGuard(WriteGuard &&other) noexcept
            : gate_(other.gate_), writer_lock_(std::move(other.writer_lock_)) {
            other.gate_ = nullptr;
        }
        WriteGuard &operator=(WriteGuard &&other) noexcept {
            if (this != &other) {
                Release();
                gate_ = other.gate_;
                writer_lock_ = std::move(other.writer_lock_);
                other.gate_ = nullptr;
            }
            return *this;
        }
        ~WriteGuard() { Release(); }

       private:
        void Release() {
            if (gate_ != nullptr) {
                gate_->WriteExit();
                gate_ = nullptr;
            }
            if (writer_lock_.owns_lock()) {
                writer_lock_.unlock();
            }
        }
        StatementCheckpointGate *gate_ = nullptr;
        std::unique_lock<std::mutex> writer_lock_;
    };

    ReadGuard ReadEnter() {
        for (;;) {
            readers_.fetch_add(1, std::memory_order_seq_cst);
            if (!writer_.load(std::memory_order_seq_cst)) {
                return ReadGuard(this);
            }
            ReadExit();
            WaitForWriterClear();
        }
    }

    WriteGuard WriteEnter() {
        std::unique_lock<std::mutex> writer_lock(writer_mutex_);
        writer_.store(true, std::memory_order_seq_cst);
        std::unique_lock<std::mutex> lock(wait_mutex_);
        wait_cv_.wait(lock, [&] { return readers_.load(std::memory_order_seq_cst) == 0; });
        return WriteGuard(this, std::move(writer_lock));
    }

   private:
    void ReadExit() {
        // 最后一个读者退出且写者在等待时才通知,避免每次退出都唤醒。
        if (readers_.fetch_sub(1, std::memory_order_seq_cst) == 1) {
            if (writer_.load(std::memory_order_seq_cst)) {
                std::lock_guard<std::mutex> lock(wait_mutex_);
                wait_cv_.notify_all();
            }
        }
    }

    void WriteExit() {
        writer_.store(false, std::memory_order_seq_cst);
        std::lock_guard<std::mutex> lock(wait_mutex_);
        wait_cv_.notify_all();
    }

    void WaitForWriterClear() {
        std::unique_lock<std::mutex> lock(wait_mutex_);
        wait_cv_.wait(lock, [&] { return !writer_.load(std::memory_order_seq_cst); });
    }

    std::atomic<std::uint32_t> readers_{0};
    std::atomic<bool> writer_{false};
    std::mutex writer_mutex_;
    std::mutex wait_mutex_;
    std::condition_variable wait_cv_;
};
