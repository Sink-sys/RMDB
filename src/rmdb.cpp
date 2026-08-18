/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <iostream>
#include <memory>
#include <mutex>
#include <poll.h>
#include <readline/history.h>
#include <readline/readline.h>
#include <signal.h>
#include <string>
#include <string_view>
#include <stop_token>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <atomic>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "errors.h"
#include "common/plan_template_cache.h"
#include "common/query_template_cache.h"
#include "common/session_point_program.h"
#include "common/scope_exit.h"
#include "common/tcp_command_framer.h"
#include "common/types.h"
#include "optimizer/optimizer.h"
#include "recovery/log_recovery.h"
#include "system/maintenance_engine.h"
#include "optimizer/plan.h"
#include "optimizer/planner.h"
#include "portal.h"
#include "analyze/analyze.h"
#include "parser/parser.h"

#define SOCK_PORT 8765
#define MAX_CONN_LIMIT 64

static_assert(std::atomic<bool>::is_always_lock_free,
              "信号处理函数只能使用无锁原子操作发布退出请求");
static std::atomic<bool> shutdown_requested{false};
static volatile sig_atomic_t server_listen_fd = -1;

namespace {

class SessionPlanTemplateHotCache {
   public:
    using Key = rmdb::plan_template_detail::PlanTemplateKey;
    using Value = std::shared_ptr<const rmdb::plan_template_detail::PlanTemplate>;

    Value lookup(const Key &key, rmdb::u64 schema_epoch, rmdb::u64 feedback_generation) const {
        for (const auto &slot : slots_) {
            if (slot.valid && slot.key == key && slot.schema_epoch == schema_epoch &&
                slot.feedback_generation == feedback_generation && slot.templ != nullptr) {
                return slot.templ;
            }
        }
        return nullptr;
    }

    void store(Key key, rmdb::u64 schema_epoch, rmdb::u64 feedback_generation, Value templ) {
        auto &slot = slots_[next_++ % slots_.size()];
        slot.key = std::move(key);
        slot.schema_epoch = schema_epoch;
        slot.feedback_generation = feedback_generation;
        slot.templ = std::move(templ);
        slot.valid = true;
    }

   private:
    struct Slot {
        Key key;
        rmdb::u64 schema_epoch = 0;
        rmdb::u64 feedback_generation = 0;
        Value templ;
        bool valid = false;
    };

    std::array<Slot, 256> slots_{};
    size_t next_ = 0;
};

class SessionPointProgramCache {
   public:
    using Key = rmdb::plan_template_detail::PlanTemplateKey;

    std::shared_ptr<Plan> lookup_or_create(
        const Key &key, rmdb::u64 schema_epoch, rmdb::u64 feedback_generation,
        const std::shared_ptr<const rmdb::plan_template_detail::PlanTemplate> &templ,
        const rmdb::SqlTemplateCandidate &candidate, Context *context) {
        for (auto &slot : slots_) {
            if (!slot.valid || !(slot.key == key) || slot.schema_epoch != schema_epoch ||
                slot.feedback_generation != feedback_generation || slot.program == nullptr) {
                continue;
            }
            if (!slot.program->Rebind(candidate, context)) {
                slot = Slot{};
                return nullptr;
            }
            return slot.program->plan();
        }

        auto program = rmdb::SessionPointProgram::Create(templ, candidate, context);
        if (program == nullptr) {
            return nullptr;
        }
        auto &slot = slots_[next_++ % slots_.size()];
        slot.key = key;
        slot.schema_epoch = schema_epoch;
        slot.feedback_generation = feedback_generation;
        slot.program = std::move(program);
        slot.valid = true;
        return slot.program->plan();
    }

   private:
    struct Slot {
        Key key;
        rmdb::u64 schema_epoch = 0;
        rmdb::u64 feedback_generation = 0;
        std::unique_ptr<rmdb::SessionPointProgram> program;
        bool valid = false;
    };

    std::array<Slot, 256> slots_{};
    size_t next_ = 0;
};

constexpr size_t kMaxBufferPoolPages = 720896;
constexpr size_t kMaxSqlCommandBytes = BUFFER_LENGTH - 1;
constexpr size_t kMaxWirePayloadBytes = 1024 * 1024;
constexpr size_t kMaxWireDiagnosticBytes = 64 * 1024;

size_t configured_buffer_pool_pages() {
    const char *raw = std::getenv("RMDB_BUFFER_POOL_PAGES");
    if (raw == nullptr || raw[0] == '\0') {
        return BUFFER_POOL_SIZE;
    }
    for (const char *cursor = raw; *cursor != '\0'; ++cursor) {
        if (!std::isdigit(static_cast<unsigned char>(*cursor))) {
            throw InternalError("RMDB_BUFFER_POOL_PAGES must be a positive integer");
        }
    }
    char *end = nullptr;
    errno = 0;
    unsigned long long parsed = std::strtoull(raw, &end, 10);
    if (errno != 0 || end == raw || *end != '\0' || parsed == 0 || parsed > kMaxBufferPoolPages) {
        throw InternalError("RMDB_BUFFER_POOL_PAGES must be between 1 and " +
                            std::to_string(kMaxBufferPoolPages));
    }
    return static_cast<size_t>(parsed);
}

std::string trim_copy(std::string str) {
    auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
    str.erase(str.begin(), std::find_if_not(str.begin(), str.end(), is_space));
    str.erase(std::find_if_not(str.rbegin(), str.rend(), is_space).base(), str.end());
    return str;
}

std::string lowercase_copy(std::string str) {
    std::transform(str.begin(), str.end(), str.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return str;
}

enum class ReceiveCommandStatus {
    kCommand,
    kPeerClosed,
    kTooLarge,
    kError,
};

ReceiveCommandStatus ReceiveCommand(int fd, rmdb::TcpCommandFramer *framer, std::string *command) {
    if (framer == nullptr || command == nullptr) {
        return ReceiveCommandStatus::kError;
    }
    char recv_buffer[4096];
    while (true) {
        switch (framer->Next(command)) {
            case rmdb::TcpFrameStatus::kCommand:
                return ReceiveCommandStatus::kCommand;
            case rmdb::TcpFrameStatus::kTooLarge:
                return ReceiveCommandStatus::kTooLarge;
            case rmdb::TcpFrameStatus::kNeedMore:
                break;
        }

        ssize_t received = recv(fd, recv_buffer, sizeof(recv_buffer), 0);
        if (received == 0) {
            return ReceiveCommandStatus::kPeerClosed;
        }
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            return ReceiveCommandStatus::kError;
        }
        framer->Append(recv_buffer, static_cast<size_t>(received));
    }
}

bool SendAll(int fd, const char *data, size_t size) {
    size_t sent = 0;
    while (sent < size) {
        ssize_t result = send(fd, data + sent, size - sent, MSG_NOSIGNAL);
        if (result > 0) {
            sent += static_cast<size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

bool SendAllV(int fd, const struct iovec *buffers, size_t buffer_count) {
    // Wire responses contain at most a fixed header and one payload. Keep a
    // writable iovec copy because a short send must advance the active range.
    if (buffers == nullptr || buffer_count == 0 || buffer_count > 2) {
        return false;
    }
    std::array<struct iovec, 2> pending{};
    size_t pending_count = 0;
    for (size_t i = 0; i < buffer_count; ++i) {
        if (buffers[i].iov_len != 0) {
            pending[pending_count++] = buffers[i];
        }
    }
    if (pending_count == 0) {
        return true;
    }

    size_t first = 0;
    while (first < pending_count) {
        struct msghdr message {};
        message.msg_iov = pending.data() + first;
        message.msg_iovlen = pending_count - first;
        ssize_t result = sendmsg(fd, &message, MSG_NOSIGNAL);
        if (result > 0) {
            size_t sent = static_cast<size_t>(result);
            while (first < pending_count && sent >= pending[first].iov_len) {
                sent -= pending[first].iov_len;
                ++first;
            }
            if (first < pending_count && sent != 0) {
                auto *base = static_cast<char *>(pending[first].iov_base);
                pending[first].iov_base = base + sent;
                pending[first].iov_len -= sent;
            }
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

void RearmTcpQuickAck(int fd) noexcept {
#ifdef TCP_QUICKACK
    int quickack = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &quickack, sizeof(quickack));
#else
    (void)fd;
#endif
}

bool ReadExact(int fd, void *data, size_t size) {
    auto *bytes = static_cast<char *>(data);
    size_t received = 0;
    while (received < size) {
        ssize_t result = recv(fd, bytes + received, size - received, 0);
        if (result > 0) {
            received += static_cast<size_t>(result);
            // Handshake and legacy reads are cold paths. Preserve the old
            // fragmented-client protection here; Wire v3 below rearms only
            // when a complete request frame is not already available.
            RearmTcpQuickAck(fd);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

class WireInputBuffer {
   public:
    explicit WireInputBuffer(int fd) : fd_(fd) {}

    bool ReadExact(void *data, size_t size) {
        auto *dst = static_cast<rmdb::u8 *>(data);
        size_t copied = 0;
        while (copied < size) {
            if (begin_ != end_) {
                const size_t take = std::min(size - copied, end_ - begin_);
                std::memcpy(dst + copied, buffer_.data() + begin_, take);
                begin_ += take;
                copied += take;
                if (begin_ == end_) {
                    begin_ = 0;
                    end_ = 0;
                }
                continue;
            }

            const size_t remaining = size - copied;
            if (remaining >= buffer_.size()) {
                ssize_t result = recv(fd_, dst + copied, remaining, 0);
                if (result > 0) {
                    copied += static_cast<size_t>(result);
                    if (copied < size) {
                        RearmTcpQuickAck(fd_);
                    }
                    continue;
                }
                if (result < 0 && errno == EINTR) {
                    continue;
                }
                return false;
            }

            ssize_t result = recv(fd_, buffer_.data(), buffer_.size(), 0);
            if (result > 0) {
                begin_ = 0;
                end_ = static_cast<size_t>(result);
                if (end_ < size - copied) {
                    RearmTcpQuickAck(fd_);
                }
                continue;
            }
            if (result < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }
        return true;
    }

    size_t Available() const { return end_ - begin_; }
    void RearmQuickAck() const { RearmTcpQuickAck(fd_); }

   private:
    static constexpr size_t kBufferBytes = 64U * 1024U;
    int fd_;
    std::array<rmdb::u8, kBufferBytes> buffer_{};
    size_t begin_{0};
    size_t end_{0};
};

rmdb::u16 LoadBe16(const rmdb::u8 *data) {
    return static_cast<rmdb::u16>((static_cast<rmdb::u16>(data[0]) << 8U) | data[1]);
}

rmdb::u32 LoadBe32(const rmdb::u8 *data) {
    return (static_cast<rmdb::u32>(data[0]) << 24U) |
           (static_cast<rmdb::u32>(data[1]) << 16U) |
           (static_cast<rmdb::u32>(data[2]) << 8U) |
           static_cast<rmdb::u32>(data[3]);
}


class WireBuffer {
   public:
    void Clear() { data_.clear(); }
    void Reserve(size_t capacity) {
        if (capacity > kMaxWirePayloadBytes) {
            throw RMDBError("wire payload exceeds 1 MiB");
        }
        data_.reserve(capacity);
    }
    const char *Data() const { return reinterpret_cast<const char *>(data_.data()); }
    size_t Size() const { return data_.size(); }

    void PutU8(rmdb::u8 value) {
        ReserveAppend(1);
        data_.push_back(value);
    }
    void PutU16(rmdb::u16 value) {
        ReserveAppend(2);
        data_.push_back(static_cast<rmdb::u8>(value >> 8U));
        data_.push_back(static_cast<rmdb::u8>(value));
    }
    void PutU32(rmdb::u32 value) {
        ReserveAppend(4);
        data_.push_back(static_cast<rmdb::u8>(value >> 24U));
        data_.push_back(static_cast<rmdb::u8>(value >> 16U));
        data_.push_back(static_cast<rmdb::u8>(value >> 8U));
        data_.push_back(static_cast<rmdb::u8>(value));
    }
    void PutU64(rmdb::u64 value) {
        PutU32(static_cast<rmdb::u32>(value >> 32U));
        PutU32(static_cast<rmdb::u32>(value));
    }
    void PutBytes(const char *data, size_t size) {
        ReserveAppend(size);
        if (size == 0) {
            return;
        }
        const auto *begin = reinterpret_cast<const rmdb::u8 *>(data);
        data_.insert(data_.end(), begin, begin + size);
    }
    void PatchU16(size_t offset, rmdb::u16 value) {
        RequirePatch(offset, 2);
        data_[offset] = static_cast<rmdb::u8>(value >> 8U);
        data_[offset + 1] = static_cast<rmdb::u8>(value);
    }
    void PatchU32(size_t offset, rmdb::u32 value) {
        RequirePatch(offset, 4);
        data_[offset] = static_cast<rmdb::u8>(value >> 24U);
        data_[offset + 1] = static_cast<rmdb::u8>(value >> 16U);
        data_[offset + 2] = static_cast<rmdb::u8>(value >> 8U);
        data_[offset + 3] = static_cast<rmdb::u8>(value);
    }

   private:
    void ReserveAppend(size_t size) const {
        if (size > kMaxWirePayloadBytes || data_.size() > kMaxWirePayloadBytes - size) {
            throw RMDBError("wire payload exceeds 1 MiB");
        }
    }
    void RequirePatch(size_t offset, size_t size) const {
        if (size > data_.size() || offset > data_.size() - size) {
            throw RMDBError("invalid wire payload patch");
        }
    }

    std::vector<rmdb::u8> data_;
};

class WireReader {
   public:
    explicit WireReader(const std::vector<rmdb::u8> &data) : data_(data) {}

    rmdb::u8 ReadU8() {
        Require(1);
        return data_[offset_++];
    }
    rmdb::u16 ReadU16() {
        Require(2);
        rmdb::u16 value = LoadBe16(data_.data() + offset_);
        offset_ += 2;
        return value;
    }
    rmdb::u32 ReadU32() {
        Require(4);
        rmdb::u32 value = LoadBe32(data_.data() + offset_);
        offset_ += 4;
        return value;
    }
    std::string ReadString(size_t size) {
        Require(size);
        std::string value(reinterpret_cast<const char *>(data_.data() + offset_), size);
        offset_ += size;
        return value;
    }
    // 有界 view(不拷贝): 指向调用方 payload(EXEC_BATCH 的 frame.payload),
    // 批循环全程有效。消费方需拷贝时才拷贝(如 BindPreparedCandidate)。
    std::string_view ReadStringView(size_t size) {
        Require(size);
        std::string_view value(reinterpret_cast<const char *>(data_.data() + offset_), size);
        offset_ += size;
        return value;
    }
    bool Empty() const { return offset_ == data_.size(); }

   private:
    void Require(size_t size) const {
        if (size > data_.size() || offset_ > data_.size() - size) {
            throw RMDBError("truncated wire payload");
        }
    }

    const std::vector<rmdb::u8> &data_;
    size_t offset_{0};
};

bool SendWireFrame(int fd, rmdb::u8 tag, const WireBuffer &payload) {
    if (payload.Size() > kMaxWirePayloadBytes) {
        return false;
    }
    rmdb::u8 header[8] = {
        static_cast<rmdb::u8>(payload.Size() >> 24U),
        static_cast<rmdb::u8>(payload.Size() >> 16U),
        static_cast<rmdb::u8>(payload.Size() >> 8U),
        static_cast<rmdb::u8>(payload.Size()),
        tag,
        0,
        0,
        0,
    };
    const std::array<struct iovec, 2> buffers{{
        {header, sizeof(header)},
        {const_cast<char *>(payload.Data()), payload.Size()},
    }};
    return SendAllV(fd, buffers.data(), payload.Size() == 0 ? 1U : buffers.size());
}

void CompleteResponseGatesAfterClientProgress(
    int fd, const std::vector<std::shared_ptr<TransactionResponseGate>> &gates) noexcept {
    if (gates.empty()) {
        return;
    }
    // EXEC_BATCH is request/response ordered. Readability of the next frame
    // provides a bounded opportunity to release gates after a terminal response.
    constexpr int kClientProgressGraceMs = 50;
    struct pollfd descriptor {
        fd, POLLIN, 0
    };
    int poll_result;
    do {
        poll_result = ::poll(&descriptor, 1, kClientProgressGraceMs);
    } while (poll_result < 0 && errno == EINTR);
    for (const auto &gate : gates) {
        if (gate != nullptr) {
            gate->Complete();
        }
    }
}

bool SendWireDiagnostic(int fd, rmdb::u8 tag, const std::string &diagnostic) {
    WireBuffer payload;
    payload.PutBytes(diagnostic.data(), std::min(diagnostic.size(), kMaxWireDiagnosticBytes));
    return SendWireFrame(fd, tag, payload);
}

struct WireFrame {
    rmdb::u8 tag{0};
    rmdb::u8 flags{0};
    rmdb::u16 reserved{0};
    std::vector<rmdb::u8> payload;
};

enum class WireReceiveStatus {
    Frame,
    PeerClosed,
    TooLarge,
};

WireReceiveStatus ReceiveWireFrame(WireInputBuffer *input, WireFrame *frame) {
    rmdb::u8 header[8];
    if (input == nullptr || !input->ReadExact(header, sizeof(header))) {
        return WireReceiveStatus::PeerClosed;
    }
    rmdb::u32 payload_size = LoadBe32(header);
    if (payload_size > kMaxWirePayloadBytes) {
        return WireReceiveStatus::TooLarge;
    }
    frame->tag = header[4];
    frame->flags = header[5];
    frame->reserved = LoadBe16(header + 6);
    frame->payload.resize(payload_size);
    if (payload_size > 0) {
        // Normal clients write one complete request frame. Rearm QUICKACK only
        // for genuine fragmentation, not once per header and payload recv.
        if (input->Available() < payload_size) {
            input->RearmQuickAck();
        }
        if (!input->ReadExact(frame->payload.data(), payload_size)) {
            return WireReceiveStatus::PeerClosed;
        }
    }
    return WireReceiveStatus::Frame;
}

enum class ConnectionProtocol {
    Legacy,
    WireV3,
    Closed,
};

ConnectionProtocol InitializeConnectionProtocol(int fd, rmdb::TcpCommandFramer *legacy_framer) {
    rmdb::u8 prefix[4];
    if (!ReadExact(fd, prefix, sizeof(prefix))) {
        return ConnectionProtocol::Closed;
    }
    if (std::memcmp(prefix, "RMDB", 4) != 0) {
        legacy_framer->Append(reinterpret_cast<const char *>(prefix), sizeof(prefix));
        return ConnectionProtocol::Legacy;
    }
    rmdb::u8 version[4];
    if (!ReadExact(fd, version, sizeof(version))) {
        return ConnectionProtocol::Closed;
    }
    if (LoadBe16(version) != 3 || LoadBe16(version + 2) != 0) {
        return ConnectionProtocol::Closed;
    }
    rmdb::u8 handshake[8] = {'R', 'M', 'D', 'B', version[0], version[1], version[2], version[3]};
    return SendAll(fd, reinterpret_cast<const char *>(handshake), sizeof(handshake))
               ? ConnectionProtocol::WireV3
               : ConnectionProtocol::Closed;
}

enum class FastCommand {
    None,
    Begin,
    Commit,
    Rollback,
    Abort,
    SetIsolationSnapshot,
    SetIsolationSerializable,
};

FastCommand parse_fast_command(const char *raw_sql) {
    std::string stmt = trim_copy(raw_sql == nullptr ? "" : std::string(raw_sql));
    if (!stmt.empty() && stmt.back() == ';') {
        stmt.pop_back();
        stmt = trim_copy(stmt);
    }
    stmt = lowercase_copy(stmt);
    if (stmt == "begin") {
        return FastCommand::Begin;
    }
    if (stmt == "commit") {
        return FastCommand::Commit;
    }
    if (stmt == "rollback") {
        return FastCommand::Rollback;
    }
    if (stmt == "abort") {
        return FastCommand::Abort;
    }
    if (stmt == "set transaction isolation level snapshot isolation") {
        return FastCommand::SetIsolationSnapshot;
    }
    if (stmt == "set transaction isolation level serializable") {
        return FastCommand::SetIsolationSerializable;
    }
    return FastCommand::None;
}

}  // namespace

std::unique_ptr<DiskManager> disk_manager;
std::unique_ptr<BufferPoolManager> buffer_pool_manager;
std::unique_ptr<RmManager> rm_manager;
std::unique_ptr<IxManager> ix_manager;
std::unique_ptr<SmManager> sm_manager;
std::unique_ptr<LockManager> lock_manager;
std::unique_ptr<TransactionManager> txn_manager;
std::unique_ptr<Planner> planner;
std::unique_ptr<Optimizer> optimizer;
std::unique_ptr<QlManager> ql_manager;
std::unique_ptr<LogManager> log_manager;
std::unique_ptr<RecoveryManager> recovery;
std::unique_ptr<Portal> portal;
std::unique_ptr<Analyze> analyze;
std::unique_ptr<MaintenanceEngine> maintenance_engine;
std::mutex client_registry_mutex;
std::unordered_set<int> active_client_fds;


void InitializeManagers(size_t buffer_pool_pages) {
    disk_manager = std::make_unique<DiskManager>();
    buffer_pool_manager = std::make_unique<BufferPoolManager>(buffer_pool_pages, disk_manager.get());
    rm_manager = std::make_unique<RmManager>(disk_manager.get(), buffer_pool_manager.get());
    ix_manager = std::make_unique<IxManager>(disk_manager.get(), buffer_pool_manager.get());
    sm_manager =
        std::make_unique<SmManager>(disk_manager.get(), buffer_pool_manager.get(), rm_manager.get(), ix_manager.get());
    lock_manager = std::make_unique<LockManager>();
    txn_manager = std::make_unique<TransactionManager>(lock_manager.get(), sm_manager.get());
    planner = std::make_unique<Planner>(sm_manager.get());
    optimizer = std::make_unique<Optimizer>(planner.get());
    ql_manager = std::make_unique<QlManager>(sm_manager.get(), txn_manager.get(), nullptr);
    log_manager = std::make_unique<LogManager>(disk_manager.get());
    buffer_pool_manager->set_log_manager(log_manager.get());
    recovery = std::make_unique<RecoveryManager>(disk_manager.get(), buffer_pool_manager.get(), sm_manager.get(),
                                                 log_manager.get());
    portal = std::make_unique<Portal>(sm_manager.get());
    analyze = std::make_unique<Analyze>(sm_manager.get());
}

void PersistRecoveredState(bool derived_indexes) {
    log_manager->flush_log_to_disk();
    disk_manager->sync_log();
    std::vector<int> durable_fds;
    durable_fds.reserve(sm_manager->fhs_.size());
    for (auto &entry : sm_manager->fhs_) {
        entry.second->flush_header();
        durable_fds.push_back(entry.second->GetFd());
    }
    buffer_pool_manager->flush_pages_for_fds(durable_fds);
    // 崩溃恢复重建出的索引完全派生自已经持久化的 heap。clean marker 在正常
    // close_db 前始终不存在，因此这里不等待索引落盘也能保证二次崩溃时安全重建。
    if (derived_indexes) {
        for (int fd : durable_fds) {
            disk_manager->sync_file(fd);
        }
    } else {
        for (auto &entry : sm_manager->ihs_) {
            entry.second->flush();
        }
        disk_manager->sync_all_data_files();
    }
    sm_manager->flush_meta();
    int meta_fd = disk_manager->get_file_fd(DB_META_NAME);
    disk_manager->sync_file(meta_fd);
    disk_manager->close_file(meta_fd);
    // ARIES pageLSNs are absolute WAL positions. Reusing offset zero would make a
    // durable page from an older generation incorrectly skip newer REDO records.
    // Online checkpoints reclaim only safe whole WAL segments. Recovery itself
    // preserves the absolute LSN namespace and never resets it after a crash.
}





void sigint_handler(int signo) {
    (void)signo;
    shutdown_requested.store(true, std::memory_order_relaxed);
    int listen_fd = server_listen_fd;
    if (listen_fd >= 0) {
        close(listen_fd);
        server_listen_fd = -1;
    }
}

// 判断当前正在执行的是显式事务还是单条SQL语句的事务，并更新事务ID
void SetTransaction(txn_id_t *txn_id, Context *context, IsolationLevel session_isolation,
                    bool implicit_read_only = false) {
    if (*txn_id == INVALID_TXN_ID) {
        context->txn_ = nullptr;
    } else if (context->txn_ == nullptr || context->txn_->get_transaction_id() != *txn_id) {
        context->txn_ = txn_manager->get_transaction(*txn_id);
    }
    if(context->txn_ == nullptr || context->txn_->get_state() == TransactionState::COMMITTED ||
        context->txn_->get_state() == TransactionState::ABORTED) {
        std::vector<lock_data_key_t> pre_snapshot_write_keys;
        pre_snapshot_write_keys.swap(context->pre_snapshot_write_keys_);
        if (implicit_read_only && pre_snapshot_write_keys.empty() &&
            session_isolation == IsolationLevel::SNAPSHOT_ISOLATION) {
            context->txn_ = txn_manager->begin_read_only(session_isolation);
        } else {
            context->txn_ = txn_manager->begin(nullptr, context->log_mgr_, session_isolation,
                                               pre_snapshot_write_keys);
        }
        *txn_id = context->txn_->get_transaction_id();
        context->txn_->set_txn_mode(false);
    }
}

void BindExistingTransaction(txn_id_t txn_id, Context *context) {
    if (txn_id == INVALID_TXN_ID) {
        context->txn_ = nullptr;
        return;
    }
    if (context->txn_ == nullptr || context->txn_->get_transaction_id() != txn_id) {
        context->txn_ = txn_manager->get_transaction(txn_id);
    }
    if (context->txn_ == nullptr || context->txn_->get_state() == TransactionState::COMMITTED ||
        context->txn_->get_state() == TransactionState::ABORTED) {
        context->txn_ = nullptr;
        return;
    }
}

void AbortSessionTransaction(
    txn_id_t *txn_id, Transaction **session_txn,
    std::vector<std::shared_ptr<TransactionResponseGate>> *response_gates = nullptr) noexcept {
    if (txn_id == nullptr || *txn_id == INVALID_TXN_ID) {
        if (session_txn != nullptr) {
            *session_txn = nullptr;
        }
        return;
    }
    try {
        auto statement_guard = txn_manager->EnterStatementExecution();
        Transaction *txn = session_txn == nullptr ? nullptr : *session_txn;
        if (txn == nullptr || txn->get_transaction_id() != *txn_id) {
            txn = txn_manager->get_transaction(*txn_id);
        }
        if (txn != nullptr && txn->get_state() == TransactionState::GROWING) {
            if (response_gates != nullptr && !txn->response_gate_deferred()) {
                auto gate = txn->get_response_gate();
                if (gate != nullptr) {
                    txn->set_response_gate_deferred(true);
                    response_gates->push_back(std::move(gate));
                }
            }
            txn_manager->abort(txn, log_manager.get());
        }
    } catch (const std::exception &error) {
        std::cerr << "Failed to abort disconnected transaction " << *txn_id << ": " << error.what() << "\n";
    } catch (...) {
        std::cerr << "Failed to abort disconnected transaction " << *txn_id << "\n";
    }
    *txn_id = INVALID_TXN_ID;
    if (session_txn != nullptr) {
        *session_txn = nullptr;
    }
}

class SessionTransactionGuard {
   public:
    SessionTransactionGuard(txn_id_t *txn_id, Transaction **session_txn)
        : txn_id_(txn_id), session_txn_(session_txn) {}
    SessionTransactionGuard(const SessionTransactionGuard &) = delete;
    SessionTransactionGuard &operator=(const SessionTransactionGuard &) = delete;
    ~SessionTransactionGuard() { AbortSessionTransaction(txn_id_, session_txn_); }

   private:
    txn_id_t *txn_id_;
    Transaction **session_txn_;
};

void AbortStatementTransaction(Context *context, txn_id_t *txn_id) {
    auto statement_guard = txn_manager->EnterStatementExecution();
    Transaction *txn = context == nullptr ? nullptr : context->txn_;
    if (txn == nullptr && txn_id != nullptr && *txn_id != INVALID_TXN_ID) {
        txn = txn_manager->get_transaction(*txn_id);
    }
    if (txn != nullptr && txn->get_state() == TransactionState::GROWING) {
        if (context != nullptr) {
            context->DeferResponseGate(txn);
        }
        txn_manager->abort(txn, log_manager.get());
    }
    if (context != nullptr) {
        context->txn_ = nullptr;
    }
    if (txn_id != nullptr) {
        *txn_id = INVALID_TXN_ID;
    }
}

bool ExecuteFastCommand(FastCommand command, txn_id_t *txn_id, Context *context,
                        IsolationLevel *session_isolation) {
    if (command == FastCommand::None) {
        return false;
    }

    if (command == FastCommand::Begin) {
        BindExistingTransaction(*txn_id, context);
        if (context->txn_ == nullptr) {
            std::vector<lock_data_key_t> pre_snapshot_write_keys;
            pre_snapshot_write_keys.swap(context->pre_snapshot_write_keys_);
            context->txn_ = txn_manager->begin(nullptr, context->log_mgr_, *session_isolation,
                                               pre_snapshot_write_keys);
        }
    }
    auto statement_guard = txn_manager->EnterStatementExecution();
    switch (command) {
        case FastCommand::Begin:
            context->txn_->set_txn_mode(true);
            *txn_id = context->txn_->get_transaction_id();
            break;
        case FastCommand::Commit:
            BindExistingTransaction(*txn_id, context);
            if (context->txn_ != nullptr) {
                context->DeferResponseGate(context->txn_);
                txn_manager->commit(context->txn_, context->log_mgr_);
                context->txn_ = nullptr;
            }
            *txn_id = INVALID_TXN_ID;
            break;
        case FastCommand::Rollback:
        case FastCommand::Abort:
            BindExistingTransaction(*txn_id, context);
            if (context->txn_ != nullptr) {
                context->DeferResponseGate(context->txn_);
                txn_manager->abort(context->txn_, context->log_mgr_);
                context->txn_ = nullptr;
            }
            *txn_id = INVALID_TXN_ID;
            break;
        case FastCommand::SetIsolationSnapshot:
        case FastCommand::SetIsolationSerializable:
            *session_isolation = command == FastCommand::SetIsolationSerializable
                                     ? IsolationLevel::SERIALIZABLE
                                     : IsolationLevel::SNAPSHOT_ISOLATION;
            BindExistingTransaction(*txn_id, context);
            break;
        case FastCommand::None:
            break;
    }
    return true;
}

void RunPlan(const std::shared_ptr<Plan> &plan, txn_id_t *txn_id, Context *context) {
    std::shared_ptr<PortalStmt> portalStmt = portal->start(plan, context);
    portal->run(portalStmt, ql_manager.get(), txn_id, context);
    portal->drop();
    if (context->txn_ != nullptr && context->txn_->get_txn_mode() == false &&
        context->txn_->get_state() == TransactionState::GROWING) {
        context->DeferResponseGate(context->txn_);
        txn_manager->commit(context->txn_, context->log_mgr_);
        context->txn_ = nullptr;
        *txn_id = INVALID_TXN_ID;
    }
}

enum class StatementStatus {
    Ok,
    TransactionAbort,
    Error,
};

struct StatementExecutionResult {
    StatementStatus status{StatementStatus::Error};
    bool is_query{false};
    bool legacy_abort_response{false};
    uint64_t affected_rows{0};
    std::string diagnostic;
};

StatementExecutionResult ExecuteSqlStatement(const char *data_recv, txn_id_t *txn_id, Context *context,
                                             IsolationLevel *session_isolation,
                                             SessionPlanTemplateHotCache *session_plan_cache,
                                             SessionPointProgramCache *session_point_program_cache,
                                             const rmdb::SqlTemplateCandidate *prepared_candidate = nullptr,
                                             const rmdb::query_template_detail::QueryTemplate *prepared_query_template = nullptr,
                                             const rmdb::plan_template_detail::PlanTemplate *prepared_plan_template = nullptr,
                                             rmdb::u64 prepared_schema_epoch = 0,
                                             rmdb::u64 prepared_feedback_generation = 0,
                                             IsolationLevel prepared_isolation = IsolationLevel::SNAPSHOT_ISOLATION,
                                             rmdb::SessionPointProgram *prepared_program = nullptr) {
    rmdb::StatementArena::Get().Reset();
    StatementExecutionResult result;
    try {
        FastCommand fast_command = parse_fast_command(data_recv);
        if (!ExecuteFastCommand(fast_command, txn_id, context, session_isolation)) {
            std::shared_ptr<Query> query;
            auto parsed_template_candidate = prepared_candidate == nullptr
                                                 ? rmdb::make_sql_template_candidate(data_recv)
                                                 : std::optional<rmdb::SqlTemplateCandidate>();
            const rmdb::SqlTemplateCandidate *template_candidate =
                prepared_candidate == nullptr
                    ? (parsed_template_candidate.has_value() ? &*parsed_template_candidate : nullptr)
                    : prepared_candidate;
            bool plan_template_candidate = prepared_candidate == nullptr && template_candidate != nullptr &&
                                           rmdb::plan_template_cacheable_session(*session_isolation);
            bool ran_plan_template = false;
            if (prepared_candidate != nullptr && prepared_plan_template != nullptr &&
                rmdb::plan_template_cacheable_session(*session_isolation) &&
                prepared_schema_epoch == rmdb::sql_template_schema_epoch() &&
                prepared_feedback_generation == rmdb::workload_feedback_generation() &&
                prepared_isolation == *session_isolation) {
                std::shared_ptr<Plan> plan;
                if (prepared_program != nullptr && prepared_program->Rebind(*prepared_candidate, context)) {
                    // 直达路径:复用 PREPARE 时克隆的计划骨架,仅重新绑定参数。
                    plan = prepared_program->plan();
                } else {
                    plan = rmdb::plan_template_detail::materialize_template(
                        *prepared_plan_template, *prepared_candidate, context);
                }
                if (plan != nullptr &&
                    (prepared_schema_epoch != rmdb::sql_template_schema_epoch() ||
                     prepared_feedback_generation != rmdb::workload_feedback_generation())) {
                    plan = nullptr;
                }
                if (plan != nullptr) {
                    result.is_query = plan->tag == T_select;
                    SetTransaction(txn_id, context, *session_isolation, result.is_query);
                    auto statement_guard = txn_manager->EnterStatementExecution();
                    RunPlan(plan, txn_id, context);
                    ran_plan_template = true;
                }
            }
            if (plan_template_candidate) {
                const rmdb::u64 schema_epoch = rmdb::sql_template_schema_epoch();
                const rmdb::u64 feedback_generation = rmdb::workload_feedback_generation();
                SessionPlanTemplateHotCache::Key hot_key{template_candidate->key, *session_isolation};
                auto plan_template = session_plan_cache->lookup(hot_key, schema_epoch, feedback_generation);
                if (plan_template == nullptr) {
                        plan_template = rmdb::lookup_plan_template_definition(*template_candidate, *session_isolation);
                    if (plan_template != nullptr) {
                        session_plan_cache->store(hot_key, schema_epoch, feedback_generation, plan_template);
                    }
                }
                auto plan = plan_template == nullptr
                                ? nullptr
                                : session_point_program_cache->lookup_or_create(
                                      hot_key, schema_epoch, feedback_generation, plan_template,
                                      *template_candidate, context);
                if (plan == nullptr && plan_template != nullptr) {
                    plan = rmdb::plan_template_detail::materialize_template(
                        *plan_template, *template_candidate, context);
                }
                if (plan != nullptr &&
                    (rmdb::sql_template_schema_epoch() != schema_epoch ||
                     rmdb::workload_feedback_generation() != feedback_generation)) {
                    plan = nullptr;
                }
                if (plan != nullptr) {
                    result.is_query = plan->tag == T_select;
                    SetTransaction(txn_id, context, *session_isolation, result.is_query);
                    auto statement_guard = txn_manager->EnterStatementExecution();
                    RunPlan(plan, txn_id, context);
                    ran_plan_template = true;
                }
            }
            if (!ran_plan_template && query == nullptr && template_candidate != nullptr) {
                query = rmdb::lookup_query_template(*template_candidate, context);
            }

            if (!ran_plan_template && query == nullptr && prepared_candidate != nullptr &&
                prepared_query_template != nullptr) {
                query = rmdb::query_template_detail::materialize_template(
                    *prepared_query_template, *prepared_candidate, context);
            }

            if (!ran_plan_template && query == nullptr) {
                if (prepared_candidate != nullptr) {
                    throw RMDBError("prepared statement template could not be materialized");
                }
                auto parse_result = rmdb::ParseSql(data_recv);
                if (parse_result.status == 0) {
                    if (parse_result.tree != nullptr) {
                        query = analyze->do_analyze(parse_result.tree);
                        if (template_candidate != nullptr) {
                            rmdb::store_query_template(*template_candidate, parse_result.tree, query);
                        }
                    }
                } else {
                    result.status = StatementStatus::Error;
                    result.diagnostic = parse_result.error.empty() ? "parse error" : std::move(parse_result.error);
                    return result;
                }
            }

            if (!ran_plan_template && query != nullptr) {
                bool is_checkpoint_stmt =
                    std::dynamic_pointer_cast<ast::CreateCheckpoint>(query->parse) != nullptr ||
                    query->kind == StmtKind::CreateCheckpoint;
                bool is_txn_end_stmt =
                    std::dynamic_pointer_cast<ast::TxnCommit>(query->parse) ||
                    std::dynamic_pointer_cast<ast::TxnAbort>(query->parse) ||
                    std::dynamic_pointer_cast<ast::TxnRollback>(query->parse) ||
                    query->kind == StmtKind::TxnCommit ||
                    query->kind == StmtKind::TxnAbort ||
                    query->kind == StmtKind::TxnRollback;
                bool is_set_isolation_stmt =
                    std::dynamic_pointer_cast<ast::SetTransactionIsolation>(query->parse) != nullptr ||
                    query->kind == StmtKind::SetTransactionIsolation;
                bool is_catalog_stmt = query->kind == StmtKind::CreateTable ||
                                       query->kind == StmtKind::DropTable ||
                                       query->kind == StmtKind::CreateIndex ||
                                       query->kind == StmtKind::DropIndex;
                {
                    StatementCheckpointGate::ReadGuard statement_guard;
                    StatementCheckpointGate::WriteGuard catalog_guard;
                    if (is_catalog_stmt) {
                        catalog_guard = txn_manager->EnterCheckpointExecution();
                        if (!is_set_isolation_stmt) {
                            SetTransaction(txn_id, context, *session_isolation,
                                           query->kind == StmtKind::Select);
                        }
                    } else {
                        if (is_checkpoint_stmt) {
                            // Fuzzy checkpoint shares the gate with DML and only excludes DDL.
                        } else if (is_txn_end_stmt) {
                            BindExistingTransaction(*txn_id, context);
                        } else if (!is_set_isolation_stmt) {
                            SetTransaction(txn_id, context, *session_isolation,
                                           query->kind == StmtKind::Select);
                        }
                        statement_guard = txn_manager->EnterStatementExecution();
                    }
                    std::shared_ptr<Plan> plan = optimizer->plan_query(query, context);
                    result.is_query = plan->tag == T_select;
                    if (plan_template_candidate) {
                        rmdb::store_plan_template(*template_candidate, query, plan, *session_isolation);
                    }
                    RunPlan(plan, txn_id, context);
                }
            }
        }
        result.status = StatementStatus::Ok;
        result.affected_rows = context->affected_rows;
        return result;
    } catch (TransactionAbortException &error) {
        AbortStatementTransaction(context, txn_id);
        result.status = StatementStatus::TransactionAbort;
        result.legacy_abort_response = true;
        result.diagnostic = error.GetInfo();
        return result;
    } catch (RMDBError &error) {
        std::cerr << error.what() << std::endl;
        Transaction *error_txn = context->txn_;
        if (error_txn == nullptr && *txn_id != INVALID_TXN_ID) {
            error_txn = txn_manager->get_transaction(*txn_id);
        }
        result.legacy_abort_response = error_txn != nullptr && error_txn->get_txn_mode();
        AbortStatementTransaction(context, txn_id);
        result.status = StatementStatus::Error;
        result.diagnostic = error.what();
        return result;
    }
}

rmdb::u8 WireTypeFor(ColType type) {
    switch (type) {
        case TYPE_INT: return 0x01;
        case TYPE_FLOAT: return 0x02;
        case TYPE_STRING: return 0x03;
    }
    throw RMDBError("unsupported wire column type");
}

struct BoundParameter {
    rmdb::u8 type{0};
    bool present{false};
    rmdb::i32 int_value{0};
    float float_value{0.0f};
    // Bounded view into the EXEC_BATCH frame payload; valid for the batch lifetime.
    std::string_view string_value;
};

class BoundParameterSpan {
   public:
    BoundParameterSpan() = default;
    BoundParameterSpan(const BoundParameter *data, size_t size) : data_(data), size_(size) {}

    const BoundParameter &operator[](size_t index) const { return data_[index]; }
    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }

   private:
    const BoundParameter *data_{nullptr};
    size_t size_{0};
};

struct PreparedColumn {
    std::string name;
    ColType type{TYPE_INT};
};

struct PointSelectProgram;    // 定义见下(批循环直通执行器)
struct PreparedStatement;

// 点 SELECT 直通执行器编译声明(定义见批循环执行器处;形状不支持返回 nullptr)
std::unique_ptr<PointSelectProgram> CompilePointSelect(const PreparedStatement &statement,
                                                       const std::shared_ptr<Plan> &plan,
                                                       SmManager *sm_manager);

struct PreparedStatement {
    rmdb::u16 id{0};
    bool is_query{false};
    std::vector<rmdb::u8> parameter_types;
    std::string exemplar_sql;
    std::vector<rmdb::u16> marker_parameters;
    std::vector<std::pair<std::string, rmdb::u16>> predicate_markers;
    std::vector<rmdb::u16> marker_literal_indexes;
    struct TemplateLiteral {
        rmdb::SqlTemplateLiteralType type{rmdb::SqlTemplateLiteralType::kInt};
        int int_value{0};
        float float_value{0.0F};
        std::string string_value;
    };
    std::vector<TemplateLiteral> template_literals;
    bool has_template{false};
    std::shared_ptr<const rmdb::query_template_detail::QueryTemplate> query_template;
    std::shared_ptr<const rmdb::plan_template_detail::PlanTemplate> plan_template;
    rmdb::u64 schema_epoch{0};
    rmdb::u64 feedback_generation{0};
    IsolationLevel plan_isolation{IsolationLevel::SNAPSHOT_ISOLATION};
    std::vector<PreparedColumn> columns;
    // 点 SELECT 直通执行器(PREPARE 时编译;nullptr = 形状不支持,走现有路径)
    std::shared_ptr<PointSelectProgram> point_select;
    // 预编译的唯一点 DML 直达程序:PREPARE 时克隆一次计划骨架,每次执行仅
    // Rebind 绑定参数,跳过逐次 materialize_template 的整树克隆。
    std::shared_ptr<rmdb::SessionPointProgram> program;
    // 通用结构特征：唯一点 UPDATE 且每个 SET 都是同列自引用加减。
    // 具体标识符只作为逻辑键命名空间，不参与是否启用的条件判断。
    bool pre_snapshot_admission{false};
    std::string admission_relation;
};

struct PreparedRequestStatement {
    rmdb::u16 id{0};
    bool is_query{false};
    std::vector<rmdb::u8> parameter_types;
    std::string sql;
};

using PreparedDictionary = std::unordered_map<rmdb::u16, PreparedStatement>;

class PreparedAstCompiler {
   public:
    PreparedAstCompiler(const std::vector<rmdb::u8> &parameter_types, PreparedStatement *prepared)
        : parameter_types_(parameter_types), prepared_(prepared), seen_(parameter_types.size(), false) {}

    rmdb::SqlTemplateCandidate Compile(const std::shared_ptr<ast::TreeNode> &root) {
        if (auto insert = std::dynamic_pointer_cast<ast::InsertStmt>(root)) {
            for (auto &value : insert->vals) AddValue(&value);
        } else if (auto update = std::dynamic_pointer_cast<ast::UpdateStmt>(root)) {
            bool self_relative_add_sub = !update->set_clauses.empty();
            for (auto &set_clause : update->set_clauses) {
                if (set_clause->op != ast::SET_OP_ADD && set_clause->op != ast::SET_OP_SUB) {
                    self_relative_add_sub = false;
                } else if (!set_clause->rhs_is_col ||
                           set_clause->col_name != set_clause->rhs_col_name) {
                    self_relative_add_sub = false;
                }
                if (!set_clause->rhs_is_col || set_clause->op != ast::SET_OP_ASSIGN) {
                    AddValue(&set_clause->val);
                }
            }
            AddConditions(update->conds);
            if (self_relative_add_sub && !prepared_->predicate_markers.empty()) {
                prepared_->pre_snapshot_admission = true;
                prepared_->admission_relation = update->tab_name;
            }
        } else if (auto del = std::dynamic_pointer_cast<ast::DeleteStmt>(root)) {
            AddConditions(del->conds);
        } else if (auto select = std::dynamic_pointer_cast<ast::SelectStmt>(root)) {
            AddConditions(select->conds);
            for (auto &having : select->having_conds) AddValue(&having->rhs);
            AddConditions(select->semi_conds);
            if (select->has_limit) AddIntLiteral(select->limit);
        } else if (!parameter_types_.empty()) {
            throw RMDBError("parameters are only supported in prepared DML statements");
        }
        if (std::find(seen_.begin(), seen_.end(), false) != seen_.end()) {
            throw RMDBError("prepared parameter markers must form a dense set");
        }
        candidate_.key.literal_count = static_cast<rmdb::u16>(candidate_.literals.size());
        return std::move(candidate_);
    }

   private:
    void AddConditions(std::vector<std::shared_ptr<ast::BinaryExpr>> &conditions) {
        for (auto &condition : conditions) {
            auto value = std::dynamic_pointer_cast<ast::Value>(condition->rhs);
            if (value == nullptr) continue;
            auto parameter = std::dynamic_pointer_cast<ast::ParameterRef>(value);
            std::string column;
            if (parameter != nullptr && condition->lhs != nullptr) {
                column = condition->lhs->tab_name.empty()
                             ? condition->lhs->col_name
                             : condition->lhs->tab_name + "." + condition->lhs->col_name;
            }
            AddValue(&value, column);
            condition->rhs = std::move(value);
        }
    }

    void AddValue(std::shared_ptr<ast::Value> *value, const std::string &predicate_column = {}) {
        if (value == nullptr || *value == nullptr) throw RMDBError("invalid prepared SQL literal");
        if (auto parameter = std::dynamic_pointer_cast<ast::ParameterRef>(*value)) {
            if (parameter->ordinal == 0 || parameter->ordinal > parameter_types_.size()) {
                throw RMDBError("prepared parameter marker out of range");
            }
            rmdb::u16 parameter_index = static_cast<rmdb::u16>(parameter->ordinal - 1U);
            rmdb::u8 parameter_type = parameter_types_[parameter_index];
            seen_[parameter_index] = true;
            prepared_->marker_parameters.push_back(parameter_index);
            prepared_->marker_literal_indexes.push_back(static_cast<rmdb::u16>(candidate_.literals.size()));
            if (!predicate_column.empty()) {
                prepared_->predicate_markers.emplace_back(predicate_column, parameter_index);
            }
            rmdb::SqlTemplateLiteral literal;
            if (parameter_type == 0x01) {
                *value = std::make_shared<ast::IntLit>(0);
                literal.type = rmdb::SqlTemplateLiteralType::kInt;
            } else if (parameter_type == 0x02) {
                *value = std::make_shared<ast::FloatLit>(0.0F);
                literal.type = rmdb::SqlTemplateLiteralType::kFloat;
            } else if (parameter_type == 0x03) {
                *value = std::make_shared<ast::StringLit>("");
                literal.type = rmdb::SqlTemplateLiteralType::kString;
            } else {
                throw RMDBError("unknown prepared parameter type");
            }
            PushLiteral(std::move(literal));
            return;
        }
        if (auto int_lit = std::dynamic_pointer_cast<ast::IntLit>(*value)) {
            AddIntLiteral(int_lit->val);
        } else if (auto float_lit = std::dynamic_pointer_cast<ast::FloatLit>(*value)) {
            rmdb::SqlTemplateLiteral literal;
            literal.type = rmdb::SqlTemplateLiteralType::kFloat;
            literal.float_val = float_lit->val < 0.0F ? -float_lit->val : float_lit->val;
            PushLiteral(std::move(literal));
        } else if (auto string_lit = std::dynamic_pointer_cast<ast::StringLit>(*value)) {
            rmdb::SqlTemplateLiteral literal;
            literal.type = rmdb::SqlTemplateLiteralType::kString;
            literal.str_val = string_lit->val;
            PushLiteral(std::move(literal));
        } else {
            throw RMDBError("unsupported prepared SQL literal");
        }
    }

    void AddIntLiteral(int value) {
        if (value == std::numeric_limits<int>::min()) {
            throw RMDBError("prepared integer literal is out of range");
        }
        rmdb::SqlTemplateLiteral literal;
        literal.type = rmdb::SqlTemplateLiteralType::kInt;
        literal.int_val = value < 0 ? -value : value;
        PushLiteral(std::move(literal));
    }

    void PushLiteral(rmdb::SqlTemplateLiteral literal) {
        if (!candidate_.literals.push_back(std::move(literal))) {
            throw RMDBError("too many prepared SQL literals");
        }
    }

    const std::vector<rmdb::u8> &parameter_types_;
    PreparedStatement *prepared_;
    std::vector<bool> seen_;
    rmdb::SqlTemplateCandidate candidate_;
};

std::vector<PreparedColumn> PreparedQueryColumns(const std::shared_ptr<Plan> &plan, Context *context) {
    auto portal_statement = portal->start(plan, context);
    if (portal_statement == nullptr || portal_statement->tag != PORTAL_ONE_SELECT ||
        portal_statement->root == nullptr) {
        throw RMDBError("prepared query did not produce a query plan");
    }
    std::vector<PreparedColumn> columns;
    columns.reserve(portal_statement->sel_cols.size());
    const auto &root_columns = portal_statement->root->cols();
    for (const auto &selected : portal_statement->sel_cols) {
        auto column = portal_statement->root->get_col(root_columns, selected);
        const auto &name = selected.output_name.empty() ? selected.col_name : selected.output_name;
        columns.push_back(PreparedColumn{name, column->type});
    }
    portal->drop();
    return columns;
}

PreparedStatement PrepareOneStatement(const PreparedRequestStatement &request, Context *context,
                                      IsolationLevel session_isolation) {
    PreparedStatement prepared;
    prepared.id = request.id;
    prepared.is_query = request.is_query;
    prepared.parameter_types = request.parameter_types;
    prepared.exemplar_sql = request.sql;

    FastCommand fast_command = parse_fast_command(prepared.exemplar_sql.c_str());
    if (fast_command != FastCommand::None) {
        if (prepared.is_query || !prepared.parameter_types.empty()) {
            throw RMDBError("prepared transaction control must be a parameterless command");
        }
        return prepared;
    }

    auto parse_result = rmdb::ParseSql(request.sql);
    if (parse_result.status != 0 || parse_result.tree == nullptr) {
        throw RMDBError(parse_result.error.empty() ? "prepared SQL parse failed" : parse_result.error);
    }
    auto candidate = PreparedAstCompiler(request.parameter_types, &prepared).Compile(parse_result.tree);
    auto query = analyze->do_analyze(parse_result.tree);
    prepared.query_template =
        rmdb::query_template_detail::make_template(candidate, parse_result.tree, query);
    if (prepared.query_template != nullptr) {
        prepared.has_template = true;
        prepared.template_literals.reserve(candidate.literals.size());
        for (size_t i = 0; i < candidate.literals.size(); ++i) {
            const auto &literal = candidate.literals[i];
            PreparedStatement::TemplateLiteral stored;
            stored.type = literal.type;
            stored.int_value = literal.int_val;
            stored.float_value = literal.float_val;
            stored.string_value.assign(literal.str_val.data(), literal.str_val.size());
            prepared.template_literals.push_back(std::move(stored));
        }
    } else if (!request.parameter_types.empty()) {
        throw RMDBError("parameterized prepared SQL cannot create an immutable query template");
    }
    auto statement_guard = txn_manager->EnterStatementExecution();
    auto plan = optimizer->plan_query(query, context);
    bool actual_query = plan->tag == T_select;
    if (actual_query != prepared.is_query) {
        throw RMDBError("prepared result_kind does not match SQL");
    }
    if (prepared.has_template && candidate.literals.size() != 0 &&
        rmdb::plan_template_cacheable_session(session_isolation)) {
        prepared.schema_epoch = rmdb::sql_template_schema_epoch();
        prepared.feedback_generation = rmdb::workload_feedback_generation();
        prepared.plan_isolation = session_isolation;
        prepared.plan_template = rmdb::plan_template_detail::make_template(
            candidate, prepared.query_template, plan, prepared.feedback_generation);
        // 唯一点 DML 语句可创建直达程序;其余形状返回 nullptr,执行时回退
        // 到逐次 materialize_template。
        prepared.program = rmdb::SessionPointProgram::Create(prepared.plan_template, candidate, context);
        if (prepared.program != nullptr && !prepared.parameter_types.empty() &&
            !prepared.program->ConfigurePreparedBindings(prepared.parameter_types.size(),
                                                         prepared.marker_literal_indexes,
                                                         prepared.marker_parameters)) {
            prepared.program.reset();
        }
    }
    // SessionPointProgram 只接受由唯一索引完整等值定位的单行 DML。
    // 范围 UPDATE 或计划退化时关闭准入，保留原 SI 路径作为通用兜底。
    if (prepared.program == nullptr || prepared.predicate_markers.empty()) {
        prepared.pre_snapshot_admission = false;
        prepared.admission_relation.clear();
    }
    if (prepared.is_query) {
        prepared.columns = PreparedQueryColumns(plan, context);
        if (prepared.columns.empty()) {
            throw RMDBError("prepared query must project at least one column");
        }
        // 点 SELECT 直通执行器编译(仅 program 有效时;形状不支持返回 nullptr,
        // 批循环退化为现有 Rebind + RunPlan 路径)。
        if (prepared.program != nullptr) {
            prepared.point_select = CompilePointSelect(prepared, prepared.program->plan(), sm_manager.get());
        }
    }
    return prepared;
}

std::vector<PreparedRequestStatement> DecodePrepareSet(const WireFrame &frame) {
    WireReader reader(frame.payload);
    rmdb::u16 statement_count = reader.ReadU16();
    if (statement_count == 0 || statement_count > 256) {
        throw RMDBError("PREPARE_SET statement_count must be 1..256");
    }
    std::vector<PreparedRequestStatement> requests;
    requests.reserve(statement_count);
    std::unordered_map<rmdb::u16, bool> ids;
    for (rmdb::u16 i = 0; i < statement_count; ++i) {
        PreparedRequestStatement request;
        request.id = reader.ReadU16();
        if (request.id == 0 || !ids.emplace(request.id, true).second) {
            throw RMDBError("PREPARE_SET statement ids must be nonzero and unique");
        }
        rmdb::u8 result_kind = reader.ReadU8();
        if (result_kind > 1) {
            throw RMDBError("invalid PREPARE_SET result_kind");
        }
        request.is_query = result_kind == 1;
        rmdb::u16 parameter_count = reader.ReadU16();
        request.parameter_types.reserve(parameter_count);
        for (rmdb::u16 parameter = 0; parameter < parameter_count; ++parameter) {
            rmdb::u8 type = reader.ReadU8();
            if (type < 0x01 || type > 0x03) {
                throw RMDBError("invalid PREPARE_SET parameter type");
            }
            request.parameter_types.push_back(type);
        }
        rmdb::u32 sql_size = reader.ReadU32();
        if (sql_size == 0) {
            throw RMDBError("prepared SQL must be non-empty");
        }
        request.sql = reader.ReadString(sql_size);
        if (request.sql.find('\0') != std::string::npos) {
            throw RMDBError("prepared SQL contains NUL");
        }
        requests.push_back(std::move(request));
    }
    if (!reader.Empty()) {
        throw RMDBError("trailing PREPARE_SET payload bytes");
    }
    return requests;
}

WireBuffer EncodePrepareOk(const std::vector<PreparedRequestStatement> &requests,
                           const PreparedDictionary &dictionary) {
    WireBuffer payload;
    payload.PutU16(static_cast<rmdb::u16>(requests.size()));
    for (const auto &request : requests) {
        const auto &prepared = dictionary.at(request.id);
        payload.PutU16(prepared.id);
        payload.PutU16(static_cast<rmdb::u16>(prepared.columns.size()));
        for (const auto &column : prepared.columns) {
            if (column.name.empty() || column.name.size() > UINT16_MAX) {
                throw RMDBError("invalid prepared result column name");
            }
            payload.PutU16(static_cast<rmdb::u16>(column.name.size()));
            payload.PutBytes(column.name.data(), column.name.size());
            payload.PutU8(WireTypeFor(column.type));
        }
    }
    return payload;
}

void AppendWireCell(WireBuffer *payload, const ColMeta &column, const char *cell) {
    payload->PutU8(1);
    if (column.type == TYPE_INT) {
        payload->PutU32(static_cast<rmdb::u32>(rmdb::load_unaligned<int>(cell)));
    } else if (column.type == TYPE_FLOAT) {
        rmdb::u32 bits = 0;
        float value = rmdb::load_unaligned<float>(cell);
        std::memcpy(&bits, &value, sizeof(bits));
        payload->PutU32(bits);
    } else if (column.type == TYPE_STRING) {
        const void *nul = std::memchr(cell, '\0', static_cast<size_t>(column.len));
        size_t length = nul == nullptr ? static_cast<size_t>(column.len)
                                       : static_cast<const char *>(nul) - cell;
        payload->PutU32(static_cast<rmdb::u32>(length));
        payload->PutBytes(cell, length);
    } else {
        throw RMDBError("unsupported wire cell type");
    }
}

class StreamQueryResultWriter : public QueryResultWriter {
   public:
    explicit StreamQueryResultWriter(int fd) : fd_(fd) {}

    void BeginResult(const std::vector<ColMeta> &columns) override {
        if (columns.empty() || columns.size() > UINT16_MAX) {
            throw RMDBError("invalid query column count");
        }
        WireBuffer payload;
        payload.PutU16(static_cast<rmdb::u16>(columns.size()));
        for (const auto &column : columns) {
            if (column.name.empty() || column.name.size() > UINT16_MAX) {
                throw RMDBError("invalid wire column name");
            }
            payload.PutU16(static_cast<rmdb::u16>(column.name.size()));
            payload.PutBytes(column.name.data(), column.name.size());
            payload.PutU8(WireTypeFor(column.type));
        }
        SendOrThrow(0x01, payload);
        began_ = true;
    }

    void WriteRow(const std::vector<ColMeta> &columns,
                  const std::vector<const char *> &cells) override {
        if (!began_ || columns.size() != cells.size()) {
            throw RMDBError("wire row does not match result schema");
        }
        WireBuffer payload;
        for (size_t i = 0; i < columns.size(); ++i) {
            AppendWireCell(&payload, columns[i], cells[i]);
        }
        SendOrThrow(0x02, payload);
    }

    void EndResult(rmdb::u64 row_count) override {
        if (!began_) {
            throw RMDBError("wire result ended before META");
        }
        WireBuffer payload;
        payload.PutU64(row_count);
        SendOrThrow(0x11, payload);
        ended_ = true;
    }

    bool ended() const { return ended_; }

   private:
    void SendOrThrow(rmdb::u8 tag, const WireBuffer &payload) {
        if (!SendWireFrame(fd_, tag, payload)) {
            throw RMDBError("wire response write failed");
        }
    }

    int fd_;
    bool began_{false};
    bool ended_{false};
};

// 点 SELECT 直通执行器: PREPARE 时从 plan 编译(索引句柄 + key 列参数映射 +
// 输出列),执行时 key 组装 -> get_unique_value(P2 leaf-hint) -> 快照可见性 ->
// 直接写结果,免去 portal/executor 构造与 select_from 框架。仅支持 int/float
// 定长键与纯表列投影;其余形状编译失败,批循环退化为现有路径。
struct PointSelectProgram {
    IxIndexHandle *ih_ = nullptr;
    RmFileHandle *fh_ = nullptr;
    std::string tab_name_;
    struct KeyCol {
        rmdb::u16 param_index;   // operation.parameters 序号(predicate_markers)
        int key_offset;
        int len;
    };
    std::vector<KeyCol> key_cols_;
    int logical_key_len_ = 0;    // 逻辑键长(列顺序拼接)
    int key_total_len_ = 0;      // 含 rid 后缀(非 unique 索引)
    bool unique_ = false;
    std::vector<ColMeta> out_cols_;

    bool Execute(BoundParameterSpan params, Transaction *txn, QueryResultWriter *writer,
                 int *hint, TransactionManager *txn_mgr) const;
};

std::unique_ptr<PointSelectProgram> CompilePointSelect(const PreparedStatement &statement,
                                                              const std::shared_ptr<Plan> &plan,
                                                              SmManager *sm_manager) {
    auto dml = std::dynamic_pointer_cast<DMLPlan>(plan);
    if (dml == nullptr || dml->tag != T_select) {
        return nullptr;
    }
    std::shared_ptr<Plan> access = dml->subplan_;
    if (auto projection = std::dynamic_pointer_cast<ProjectionPlan>(access)) {
        access = projection->subplan_;
    }
    auto scan = std::dynamic_pointer_cast<ScanPlan>(access);
    if (scan == nullptr || scan->tag != T_IndexScan || scan->runtime_cache_ == nullptr ||
        !scan->runtime_cache_->has_index) {
        return nullptr;
    }
    const IndexMeta &index = scan->runtime_cache_->index_meta;
    auto program = std::make_unique<PointSelectProgram>();
    program->tab_name_ = dml->tab_name_;
    program->ih_ = rmdb::resolve_index_handle(sm_manager, dml->tab_name_, index);
    program->fh_ = sm_manager->fhs_.at(dml->tab_name_).get();
    program->unique_ = index.unique;
    // key 列: index.cols 顺序,列名 -> predicate_markers 的参数序号。
    // 仅支持定长 4 字节键(int/float);string 键或缺失映射退化。
    int key_offset = 0;
    for (int i = 0; i < index.col_num; ++i) {
        const std::string &col_name = index.cols[i].name;
        auto marker_iter = std::find_if(statement.predicate_markers.begin(),
                                        statement.predicate_markers.end(),
                                        [&](const std::pair<std::string, rmdb::u16> &entry) {
                                            return entry.first == col_name;
                                        });
        if (marker_iter == statement.predicate_markers.end()) {
            return nullptr;
        }
        rmdb::u16 param_index = marker_iter->second;
        if (param_index >= statement.parameter_types.size() ||
            statement.parameter_types[param_index] != 0x01 &&
                statement.parameter_types[param_index] != 0x02) {
            return nullptr;   // 仅 int/float 键
        }
        if (index.cols[i].len != 4) {
            return nullptr;
        }
        program->key_cols_.push_back(PointSelectProgram::KeyCol{param_index, key_offset, 4});
        key_offset += 4;
    }
    program->logical_key_len_ = key_offset;
    program->key_total_len_ = key_offset + (index.unique ? 0 : 8);
    if (program->key_cols_.empty() || key_offset != index.logical_col_tot_len()) {
        return nullptr;
    }
    // 输出列: prepared.columns(显示名+类型) -> tab.cols 的 offset/len。
    const TabMeta &tab = sm_manager->db_.get_table(dml->tab_name_);
    program->out_cols_.reserve(statement.columns.size());
    for (const auto &prepared_col : statement.columns) {
        auto col_iter = std::find_if(tab.cols.begin(), tab.cols.end(),
                                     [&](const ColMeta &col) { return col.name == prepared_col.name; });
        if (col_iter == tab.cols.end()) {
            return nullptr;   // 别名/表达式列: 退化
        }
        ColMeta out = *col_iter;
        out.name = prepared_col.name;
        program->out_cols_.push_back(out);
    }
    if (program->out_cols_.empty()) {
        return nullptr;
    }
    return program;
}

    bool PointSelectProgram::Execute(BoundParameterSpan params, Transaction *txn,
                                     QueryResultWriter *writer, int *hint, TransactionManager *txn_mgr) const {
        // key 组装: 列顺序拼接参数值; 非 unique 索引补 0 rid 后缀。
        std::string key(static_cast<size_t>(key_total_len_), '\0');
        for (const auto &kc : key_cols_) {
            if (kc.param_index >= params.size() || !params[kc.param_index].present) {
                return false;
            }
            const BoundParameter &param = params[kc.param_index];
            if (param.type == 0x01) {
                memcpy(key.data() + kc.key_offset, &param.int_value, 4);
            } else if (param.type == 0x02) {
                memcpy(key.data() + kc.key_offset, &param.float_value, 4);
            } else {
                return false;
            }
        }
        if (!unique_) {
            memset(key.data() + logical_key_len_, 0, 8);
        }
        auto write_empty = [&]() {
            writer->BeginResult(out_cols_);
            writer->EndResult(0);
        };
        Rid rid;
        if (!ih_->get_unique_value(key.data(), &rid, txn, hint)) {
            write_empty();   // 未找到: 空结果
            return true;
        }
        RmRecord record;
        TupleMeta meta;
        if (txn_mgr == nullptr || !txn_mgr->GetVisibleTupleInto(tab_name_, rid, txn, &record, &meta)) {
            write_empty();   // 快照不可见(并发删除): 空结果
            return true;
        }
        writer->BeginResult(out_cols_);
        std::vector<const char *> cells(out_cols_.size());
        for (size_t i = 0; i < out_cols_.size(); ++i) {
            cells[i] = record.data + out_cols_[i].offset;
        }
        writer->WriteRow(out_cols_, cells);
        writer->EndResult(1);
        return true;
    }

struct BatchOperation {
    const PreparedStatement *statement{nullptr};
    size_t parameter_offset{0};
    size_t parameter_count{0};

    BoundParameterSpan Parameters(const std::vector<BoundParameter> &parameters) const {
        if (parameter_count == 0) {
            return {};
        }
        return BoundParameterSpan(parameters.data() + parameter_offset, parameter_count);
    }
};

struct DecodedBatch {
    std::vector<BatchOperation> operations;
    std::vector<BoundParameter> parameters;
};

class BatchResponseArena {
   public:
    void BeginSuccess(rmdb::u16 operation_count, size_t reserve_hint) {
        payload_.Clear();
        payload_.Reserve(std::min(kMaxWirePayloadBytes, std::max<size_t>(reserve_hint, 256U)));
        payload_.PutU16(operation_count);
        payload_.PutU8(0);
        payload_.PutU16(UINT16_MAX);
        payload_.PutU32(0);
        result_count_offset_ = payload_.Size();
        payload_.PutU16(0);
        result_count_ = 0;
        success_active_ = true;
    }

    size_t BeginQuery(rmdb::u16 operation_index) {
        if (!success_active_ || result_count_ == UINT16_MAX) {
            throw RMDBError("invalid batch result state");
        }
        payload_.PutU16(operation_index);
        size_t row_count_offset = payload_.Size();
        payload_.PutU32(0);
        ++result_count_;
        return row_count_offset;
    }

    void EndQuery(size_t row_count_offset, rmdb::u32 row_count) {
        if (!success_active_) {
            throw RMDBError("invalid batch result state");
        }
        payload_.PatchU32(row_count_offset, row_count);
    }

    void FinishSuccess() {
        if (!success_active_) {
            throw RMDBError("invalid batch result state");
        }
        payload_.PatchU16(result_count_offset_, result_count_);
    }

    void BeginFailure(rmdb::u16 executed_operations, rmdb::u8 status,
                      rmdb::u16 failed_operation, std::string_view diagnostic) {
        payload_.Clear();
        payload_.PutU16(executed_operations);
        payload_.PutU8(status);
        payload_.PutU16(failed_operation);
        size_t diagnostic_size = std::min(diagnostic.size(), kMaxWireDiagnosticBytes);
        payload_.PutU32(static_cast<rmdb::u32>(diagnostic_size));
        payload_.PutBytes(diagnostic.data(), diagnostic_size);
        payload_.PutU16(0);
        success_active_ = false;
    }

    WireBuffer *MutablePayload() { return &payload_; }
    const WireBuffer &Payload() const { return payload_; }

   private:
    WireBuffer payload_;
    size_t result_count_offset_{0};
    rmdb::u16 result_count_{0};
    bool success_active_{false};
};

class BatchQueryResultWriter : public QueryResultWriter {
   public:
    BatchQueryResultWriter(rmdb::u16 operation_index, const PreparedStatement *statement,
                           BatchResponseArena *arena)
        : operation_index_(operation_index), statement_(statement), arena_(arena) {}

    void BeginResult(const std::vector<ColMeta> &columns) override {
        if (began_ || statement_ == nullptr || columns.size() != statement_->columns.size()) {
            throw RMDBError("batch query schema column count mismatch");
        }
        for (size_t i = 0; i < columns.size(); ++i) {
            if (columns[i].type != statement_->columns[i].type) {
                throw RMDBError("batch query schema type mismatch");
            }
        }
        row_count_offset_ = arena_->BeginQuery(operation_index_);
        began_ = true;
    }

    void WriteRow(const std::vector<ColMeta> &columns,
                  const std::vector<const char *> &cells) override {
        if (!began_ || ended_ || columns.size() != cells.size()) {
            throw RMDBError("invalid batch query row");
        }
        for (size_t i = 0; i < columns.size(); ++i) {
            AppendWireCell(arena_->MutablePayload(), columns[i], cells[i]);
        }
        if (row_count_ == UINT32_MAX) {
            throw RMDBError("batch query row count overflow");
        }
        ++row_count_;
    }

    void EndResult(rmdb::u64 row_count) override {
        if (!began_ || row_count != row_count_) {
            throw RMDBError("batch query row count mismatch");
        }
        arena_->EndQuery(row_count_offset_, row_count_);
        ended_ = true;
    }

    bool ended() const { return ended_; }

   private:
    rmdb::u16 operation_index_;
    const PreparedStatement *statement_;
    BatchResponseArena *arena_;
    size_t row_count_offset_{0};
    rmdb::u32 row_count_{0};
    bool began_{false};
    bool ended_{false};
};

DecodedBatch DecodeBatch(const WireFrame &frame, const PreparedDictionary &dictionary) {
    WireReader reader(frame.payload);
    rmdb::u16 operation_count = reader.ReadU16();
    if (operation_count == 0 || operation_count > 256) {
        throw RMDBError("EXEC_BATCH operation_count must be 1..256");
    }
    DecodedBatch batch;
    batch.operations.reserve(operation_count);
    batch.parameters.reserve(static_cast<size_t>(operation_count) * 8U);
    for (rmdb::u16 operation_index = 0; operation_index < operation_count; ++operation_index) {
        rmdb::u16 statement_id = reader.ReadU16();
        auto found = dictionary.find(statement_id);
        if (found == dictionary.end()) {
            throw RMDBError("EXEC_BATCH references unknown statement id");
        }
        BatchOperation operation;
        operation.statement = &found->second;
        operation.parameter_offset = batch.parameters.size();
        operation.parameter_count = found->second.parameter_types.size();
        for (rmdb::u8 type : found->second.parameter_types) {
            BoundParameter parameter;
            parameter.type = type;
            rmdb::u8 present = reader.ReadU8();
            if (present > 1) {
                throw RMDBError("invalid typed parameter present byte");
            }
            parameter.present = present == 1;
            if (parameter.present) {
                if (type == 0x01) {
                    parameter.int_value = static_cast<rmdb::i32>(reader.ReadU32());
                } else if (type == 0x02) {
                    rmdb::u32 bits = reader.ReadU32();
                    std::memcpy(&parameter.float_value, &bits, sizeof(bits));
                    if (!std::isfinite(parameter.float_value)) {
                        throw RMDBError("FLOAT32 parameter must be finite");
                    }
                } else if (type == 0x03) {
                    parameter.string_value = reader.ReadStringView(reader.ReadU32());
                }
            }
            batch.parameters.push_back(std::move(parameter));
        }
        batch.operations.push_back(operation);
    }
    if (!reader.Empty()) {
        throw RMDBError("trailing EXEC_BATCH payload bytes");
    }
    return batch;
}

lock_data_key_t ComputePreSnapshotWriteKey(const PreparedStatement &statement,
                                           BoundParameterSpan parameters) {
    rmdb::u64 hash = 1469598103934665603ULL;
    auto mix_byte = [&](rmdb::u8 byte) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    };
    auto mix_u64 = [&](rmdb::u64 value) {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            mix_byte(static_cast<rmdb::u8>((value >> shift) & 0xffU));
        }
    };
    auto mix_text = [&](std::string_view value) {
        mix_u64(static_cast<rmdb::u64>(value.size()));
        for (unsigned char byte : value) {
            mix_byte(byte);
        }
    };

    mix_text(statement.admission_relation);
    auto markers = statement.predicate_markers;
    std::sort(markers.begin(), markers.end(), [](const auto &lhs, const auto &rhs) {
        return lhs.first == rhs.first ? lhs.second < rhs.second : lhs.first < rhs.first;
    });
    for (const auto &[column, parameter_index] : markers) {
        mix_text(column);
        if (parameter_index >= parameters.size()) {
            mix_byte(0xffU);
            continue;
        }
        const BoundParameter &parameter = parameters[parameter_index];
        mix_byte(parameter.type);
        mix_byte(parameter.present ? 1U : 0U);
        if (!parameter.present) {
            continue;
        }
        if (parameter.type == 0x01) {
            mix_u64(static_cast<rmdb::u32>(parameter.int_value));
        } else if (parameter.type == 0x02) {
            rmdb::u32 bits = 0;
            static_assert(sizeof(bits) == sizeof(parameter.float_value));
            std::memcpy(&bits, &parameter.float_value, sizeof(bits));
            mix_u64(bits);
        } else if (parameter.type == 0x03) {
            mix_text(parameter.string_value);
        }
    }
    return static_cast<lock_data_key_t>(hash);
}

bool BoundParameterToLiteral(const BoundParameter &parameter, rmdb::SqlTemplateLiteral *literal) {
    if (!parameter.present || literal == nullptr) {
        return false;
    }
    if (parameter.type == 0x01) {
        literal->type = rmdb::SqlTemplateLiteralType::kInt;
        literal->int_val = parameter.int_value;
    } else if (parameter.type == 0x02) {
        literal->type = rmdb::SqlTemplateLiteralType::kFloat;
        literal->float_val = parameter.float_value;
    } else if (parameter.type == 0x03) {
        literal->type = rmdb::SqlTemplateLiteralType::kString;
        literal->str_val = parameter.string_value;
    } else {
        return false;
    }
    return true;
}

bool BindPreparedProgram(const PreparedStatement &statement, BoundParameterSpan parameters,
                         Context *context) {
    if (statement.program == nullptr || parameters.size() != statement.parameter_types.size()) {
        return false;
    }
    return statement.program->RebindPrepared(
        parameters.size(),
        [&](size_t parameter_index, rmdb::SqlTemplateLiteral *literal) {
            return parameter_index < parameters.size() &&
                   BoundParameterToLiteral(parameters[parameter_index], literal);
        },
        context);
}

rmdb::SqlTemplateCandidate BindPreparedCandidate(const PreparedStatement &statement,
                                                 BoundParameterSpan parameters) {
    if (!statement.has_template || parameters.size() != statement.parameter_types.size()) {
        throw RMDBError("prepared statement template is unavailable");
    }
    rmdb::SqlTemplateCandidate candidate;
    candidate.key.literal_count = static_cast<rmdb::u16>(statement.template_literals.size());
    size_t marker = 0;
    for (size_t literal_index = 0; literal_index < statement.template_literals.size(); ++literal_index) {
        const auto &stored = statement.template_literals[literal_index];
        rmdb::SqlTemplateLiteral literal;
        literal.type = stored.type;
        literal.int_val = stored.int_value;
        literal.float_val = stored.float_value;
        literal.str_val = stored.string_value;
        if (marker < statement.marker_literal_indexes.size() &&
            statement.marker_literal_indexes[marker] == literal_index) {
            rmdb::u16 parameter_index = statement.marker_parameters[marker++];
            const BoundParameter &parameter = parameters[parameter_index];
            if (!parameter.present) {
                throw RMDBError("NULL prepared parameters are not supported by this schema");
            }
            if (!BoundParameterToLiteral(parameter, &literal)) {
                throw RMDBError("invalid prepared parameter type");
            }
        }
        if (!candidate.literals.push_back(std::move(literal))) {
            throw RMDBError("too many prepared SQL literals");
        }
    }
    if (marker != statement.marker_literal_indexes.size()) {
        throw RMDBError("prepared statement marker mapping is inconsistent");
    }
    return candidate;
}

bool SendUnexpectedBatchFailure(int fd, txn_id_t *txn_id, Transaction **session_txn,
                                rmdb::u16 failed_operation, rmdb::u8 status,
                                const char *diagnostic, BatchResponseArena *arena,
                                std::vector<std::shared_ptr<TransactionResponseGate>>
                                    *response_gates) noexcept {
    AbortSessionTransaction(txn_id, session_txn, response_gates);
    try {
        arena->BeginFailure(failed_operation, status, failed_operation,
                            diagnostic == nullptr ? "batch execution failed" : diagnostic);
        return SendWireFrame(fd, 0x15, arena->Payload());
    } catch (...) {
        return false;
    }
}

bool HandlePrepareSetRequest(int fd, const WireFrame &frame, PreparedDictionary *dictionary,
                             txn_id_t txn_id, IsolationLevel session_isolation) {
    if (txn_id != INVALID_TXN_ID) {
        return SendWireDiagnostic(fd, 0x13, "PREPARE_SET is not allowed during an active transaction");
    }
    Transaction *planning_txn = nullptr;
    try {
        auto requests = DecodePrepareSet(frame);
        planning_txn = txn_manager->begin(nullptr, log_manager.get(), session_isolation);
        StatementScratch scratch;
        auto data_send = std::make_unique<char[]>(BUFFER_LENGTH);
        int offset = 0;
        Context planning_context(lock_manager.get(), log_manager.get(), txn_manager.get(), planning_txn,
                                 data_send.get(), &offset, &session_isolation, &scratch);
        PreparedDictionary replacement;
        replacement.reserve(requests.size());
        for (const auto &request : requests) {
            PreparedStatement prepared;
            try {
                prepared = PrepareOneStatement(request, &planning_context, session_isolation);
            } catch (RMDBError &error) {
                throw RMDBError("prepared statement " + std::to_string(request.id) + ": " + error.what());
            }
            replacement.emplace(prepared.id, std::move(prepared));
            scratch.reset();
        }
        {
            auto statement_guard = txn_manager->EnterStatementExecution();
            txn_manager->commit(planning_txn, log_manager.get());
        }
        planning_txn = nullptr;
        WireBuffer response = EncodePrepareOk(requests, replacement);
        *dictionary = std::move(replacement);
        return SendWireFrame(fd, 0x14, response);
    } catch (RMDBError &error) {
        if (planning_txn != nullptr && planning_txn->get_state() == TransactionState::GROWING) {
            auto statement_guard = txn_manager->EnterStatementExecution();
            txn_manager->abort(planning_txn, log_manager.get());
        }
        return SendWireDiagnostic(fd, 0x13, error.what());
    }
}

bool HandleBatchRequestImpl(int fd, const WireFrame &frame, const PreparedDictionary &dictionary,
                            char *data_send, int *offset, txn_id_t *txn_id, Transaction **session_txn,
                            IsolationLevel *session_isolation, StatementScratch *statement_scratch,
                            SessionPlanTemplateHotCache *session_plan_cache,
                            SessionPointProgramCache *session_point_program_cache,
                            rmdb::u16 *operation_progress, BatchResponseArena *response_arena,
                            std::vector<std::shared_ptr<TransactionResponseGate>> *response_gates) {
    DecodedBatch batch;
    try {
        batch = DecodeBatch(frame, dictionary);
    } catch (RMDBError &error) {
        if (*txn_id != INVALID_TXN_ID || *session_txn != nullptr) {
            Context abort_context(lock_manager.get(), log_manager.get(), txn_manager.get(), *session_txn,
                                  data_send, offset, session_isolation, statement_scratch,
                                  nullptr, nullptr, response_gates);
            AbortStatementTransaction(&abort_context, txn_id);
            *session_txn = nullptr;
        }
        return SendWireDiagnostic(fd, 0x13, error.what());
    }
    const auto &operations = batch.operations;
    response_arena->BeginSuccess(static_cast<rmdb::u16>(operations.size()), frame.payload.size());
    // Batch-level index leaf hints let consecutive point lookups reuse the
    // previous leaf page. Hints survive across statements and are validated on use.
    std::unordered_map<const IxIndexHandle *, int> batch_index_hints;
    // 仅在新显式事务的首批中扫描可静态证明的唯一点自引用写。先取得
    // 全部逻辑键再执行 BEGIN，确保 read_ts 晚于同键前任的最终状态。
    std::vector<lock_data_key_t> batch_pre_snapshot_writes;
    if (*txn_id == INVALID_TXN_ID && *session_txn == nullptr) {
        for (const BatchOperation &operation : operations) {
            const PreparedStatement &statement = *operation.statement;
            if (!statement.pre_snapshot_admission) {
                continue;
            }
            batch_pre_snapshot_writes.push_back(ComputePreSnapshotWriteKey(
                statement, operation.Parameters(batch.parameters)));
        }
    }
    for (rmdb::u16 operation_index = 0; operation_index < operations.size(); ++operation_index) {
        *operation_progress = operation_index;
        const BatchOperation &operation = operations[operation_index];
        const PreparedStatement &statement = *operation.statement;
        BoundParameterSpan parameters = operation.Parameters(batch.parameters);
        *offset = 0;
        data_send[0] = '\0';
        statement_scratch->reset();

        BatchQueryResultWriter result_writer(operation_index, &statement, response_arena);
        Context statement_context(lock_manager.get(), log_manager.get(), txn_manager.get(), *session_txn,
                                  data_send, offset, session_isolation, statement_scratch,
                                  statement.is_query ? &result_writer : nullptr, &batch_index_hints,
                                  response_gates);
        if (!batch_pre_snapshot_writes.empty() && *txn_id == INVALID_TXN_ID &&
            *session_txn == nullptr) {
            statement_context.pre_snapshot_write_keys_ = batch_pre_snapshot_writes;
        }
        StatementExecutionResult execution;
        try {
            if (statement.has_template) {
                bool ran_direct = false;
                // 直通入口: 唯一点 DML(program 非空)且模板校验通过时,跳过
                // ExecuteSqlStatement 框架(parse_fast_command/template
                // 缓存查找/异常框架),直接 Rebind + SetTransaction + RunPlan。
                // 行为与 ExecuteSqlStatement 的 prepared 直达路径一致;Rebind 失败
                // 或模板校验不过退化为完整路径。
                if (statement.program != nullptr && statement.plan_template != nullptr &&
                    rmdb::plan_template_cacheable_session(*session_isolation) &&
                    statement.schema_epoch == rmdb::sql_template_schema_epoch() &&
                    statement.feedback_generation == rmdb::workload_feedback_generation() &&
                    statement.plan_isolation == *session_isolation &&
                    BindPreparedProgram(statement, parameters, &statement_context)) {
                    if (statement.point_select != nullptr) {
                        // P3-2: 点 SELECT 直通执行器——免 portal/executor/select_from。
                        // Execute 返回 false(参数形状异常)时退化为 RunPlan 路径。
                        try {
                            execution.is_query = true;
                            SetTransaction(txn_id, &statement_context, *session_isolation, true);
                            auto statement_guard = txn_manager->EnterStatementExecution();
                            int *hint = &batch_index_hints[statement.point_select->ih_];
                            if (*hint == 0) {
                                *hint = IX_NO_PAGE;
                            }
                            if (statement.point_select->Execute(parameters, statement_context.txn_,
                                                                &result_writer, hint, txn_manager.get())) {
                                ran_direct = true;
                                execution.status = StatementStatus::Ok;
                            }
                        } catch (TransactionAbortException &error) {
                            AbortStatementTransaction(&statement_context, txn_id);
                            execution.status = StatementStatus::TransactionAbort;
                            execution.legacy_abort_response = true;
                            execution.diagnostic = error.GetInfo();
                            ran_direct = true;
                        } catch (RMDBError &error) {
                            AbortStatementTransaction(&statement_context, txn_id);
                            execution.status = StatementStatus::Error;
                            execution.diagnostic = error.what();
                            ran_direct = true;
                        }
                    }
                    if (!ran_direct) {
                        ran_direct = true;
                        try {
                            execution.is_query = statement.is_query;
                            SetTransaction(txn_id, &statement_context, *session_isolation, statement.is_query);
                            auto statement_guard = txn_manager->EnterStatementExecution();
                            RunPlan(statement.program->plan(), txn_id, &statement_context);
                            execution.status = StatementStatus::Ok;
                        } catch (TransactionAbortException &error) {
                            AbortStatementTransaction(&statement_context, txn_id);
                            execution.status = StatementStatus::TransactionAbort;
                            execution.legacy_abort_response = true;
                            execution.diagnostic = error.GetInfo();
                        } catch (RMDBError &error) {
                            AbortStatementTransaction(&statement_context, txn_id);
                            execution.status = StatementStatus::Error;
                            execution.diagnostic = error.what();
                        }
                    }
                }
                if (!ran_direct) {
                    rmdb::SqlTemplateCandidate candidate = BindPreparedCandidate(statement, parameters);
                    execution = ExecuteSqlStatement(
                        statement.exemplar_sql.c_str(), txn_id, &statement_context, session_isolation,
                        session_plan_cache, session_point_program_cache, &candidate,
                        statement.query_template.get(), statement.plan_template.get(), statement.schema_epoch,
                        statement.feedback_generation, statement.plan_isolation, statement.program.get());
                }
            } else {
                if (!parameters.empty()) {
                    throw RMDBError("prepared command parameters cannot be bound");
                }
                execution = ExecuteSqlStatement(
                    statement.exemplar_sql.c_str(), txn_id, &statement_context, session_isolation,
                    session_plan_cache, session_point_program_cache, nullptr, nullptr, nullptr, 0, 0,
                    IsolationLevel::SNAPSHOT_ISOLATION);
            }
        } catch (RMDBError &error) {
            AbortStatementTransaction(&statement_context, txn_id);
            execution.status = StatementStatus::Error;
            execution.diagnostic = error.what();
        }
        *session_txn = statement_context.txn_;

        bool result_mismatch = execution.status == StatementStatus::Ok &&
                               (execution.is_query != statement.is_query ||
                                (statement.is_query && !result_writer.ended()));
        if (result_mismatch) {
            AbortStatementTransaction(&statement_context, txn_id);
            *session_txn = nullptr;
            execution.status = StatementStatus::Error;
            execution.diagnostic = "prepared result_kind does not match execution result";
        }
        if (execution.status != StatementStatus::Ok) {
            if (*txn_id != INVALID_TXN_ID || *session_txn != nullptr) {
                AbortStatementTransaction(&statement_context, txn_id);
                *session_txn = nullptr;
            }
            rmdb::u8 status = execution.status == StatementStatus::TransactionAbort ? 1 : 2;
            response_arena->BeginFailure(operation_index, status, operation_index, execution.diagnostic);
            return SendWireFrame(fd, 0x15, response_arena->Payload());
        }
    }

    response_arena->FinishSuccess();
    return SendWireFrame(fd, 0x15, response_arena->Payload());
}

bool HandleBatchRequest(int fd, const WireFrame &frame, const PreparedDictionary &dictionary,
                        char *data_send, int *offset, txn_id_t *txn_id, Transaction **session_txn,
                        IsolationLevel *session_isolation, StatementScratch *statement_scratch,
                        SessionPlanTemplateHotCache *session_plan_cache,
                        SessionPointProgramCache *session_point_program_cache,
                        BatchResponseArena *response_arena) noexcept {
    rmdb::u16 operation_progress = 0;
    std::vector<std::shared_ptr<TransactionResponseGate>> response_gates;
    auto response_gate_guard = rmdb::make_scope_exit([&] {
        CompleteResponseGatesAfterClientProgress(fd, response_gates);
    });
    try {
        return HandleBatchRequestImpl(fd, frame, dictionary, data_send, offset, txn_id, session_txn,
                                      session_isolation, statement_scratch, session_plan_cache,
                                      session_point_program_cache, &operation_progress, response_arena,
                                      &response_gates);
    } catch (TransactionAbortException &error) {
        return SendUnexpectedBatchFailure(fd, txn_id, session_txn, operation_progress, 1,
                                          "transaction aborted", response_arena, &response_gates);
    } catch (const std::exception &error) {
        return SendUnexpectedBatchFailure(fd, txn_id, session_txn, operation_progress, 2, error.what(),
                                          response_arena, &response_gates);
    } catch (...) {
        return SendUnexpectedBatchFailure(fd, txn_id, session_txn, operation_progress, 2,
                                          "unknown batch execution failure", response_arena,
                                          &response_gates);
    }
}

void HandleWireV3Connection(int fd, char *data_send, int *offset, txn_id_t *txn_id,
                            Transaction **session_txn, IsolationLevel *session_isolation,
                            StatementScratch *statement_scratch,
                            SessionPlanTemplateHotCache *session_plan_cache,
                            SessionPointProgramCache *session_point_program_cache) {
    WireInputBuffer input(fd);
    WireFrame frame;
    PreparedDictionary dictionary;
    BatchResponseArena batch_response_arena;
    while (true) {
        WireReceiveStatus receive_status = ReceiveWireFrame(&input, &frame);
        if (receive_status == WireReceiveStatus::PeerClosed) {
            return;
        }
        if (receive_status == WireReceiveStatus::TooLarge) {
            SendWireDiagnostic(fd, 0x13, "frame payload exceeds 1 MiB");
            return;
        }
        if (frame.reserved != 0) {
            SendWireDiagnostic(fd, 0x13, "reserved frame field must be zero");
            continue;
        }
        if (frame.tag == 0x21) {
            if (frame.flags != 0) {
                SendWireDiagnostic(fd, 0x13, "PREPARE_SET flags must be zero");
                continue;
            }
            if (!HandlePrepareSetRequest(fd, frame, &dictionary, *txn_id, *session_isolation)) {
                return;
            }
            continue;
        }
        if (frame.tag == 0x22) {
            if (frame.flags != 0x01) {
                SendWireDiagnostic(fd, 0x13, "EXEC_BATCH requires AUTO_ABORT");
                continue;
            }
            if (!HandleBatchRequest(fd, frame, dictionary, data_send, offset, txn_id, session_txn,
                                    session_isolation, statement_scratch, session_plan_cache,
                                    session_point_program_cache, &batch_response_arena)) {
                return;
            }
            continue;
        }
        if (frame.tag != 0x20) {
            SendWireDiagnostic(fd, 0x13, "unsupported request tag");
            continue;
        }
        if (frame.flags != 0) {
            SendWireDiagnostic(fd, 0x13, "EXEC_STREAM flags must be zero");
            continue;
        }
        if (frame.payload.empty() ||
            std::find(frame.payload.begin(), frame.payload.end(), static_cast<rmdb::u8>(0)) != frame.payload.end()) {
            SendWireDiagnostic(fd, 0x13, "EXEC_STREAM requires non-empty SQL without NUL");
            continue;
        }

        std::string sql(reinterpret_cast<const char *>(frame.payload.data()), frame.payload.size());
        *offset = 0;
        data_send[0] = '\0';
        statement_scratch->reset();
        StreamQueryResultWriter result_writer(fd);
        Context statement_context(lock_manager.get(), log_manager.get(), txn_manager.get(), *session_txn,
                                  data_send, offset, session_isolation, statement_scratch, &result_writer);
        StatementExecutionResult execution = ExecuteSqlStatement(
            sql.c_str(), txn_id, &statement_context, session_isolation, session_plan_cache,
            session_point_program_cache);
        *session_txn = statement_context.txn_;

        if (execution.status == StatementStatus::Ok) {
            if (execution.is_query) {
                if (!result_writer.ended()) {
                    SendWireDiagnostic(fd, 0x13, "query did not produce a complete result");
                }
            } else {
                WireBuffer empty;
                if (!SendWireFrame(fd, 0x10, empty)) {
                    return;
                }
            }
        } else {
            rmdb::u8 tag = execution.status == StatementStatus::TransactionAbort ? 0x12 : 0x13;
            if (!SendWireDiagnostic(fd, tag, execution.diagnostic)) {
                return;
            }
        }
    }
}

void client_handler(int fd) {
    struct RegistryGuard {
        int fd;
        ~RegistryGuard() {
            std::lock_guard<std::mutex> guard(client_registry_mutex);
            active_client_fds.erase(fd);
        }
    } registry_guard{fd};
    rmdb::TcpCommandFramer command_framer(kMaxSqlCommandBytes);
    std::string command;
    // 需要返回给客户端的结果
    auto data_send = std::make_unique<char[]>(BUFFER_LENGTH);
    // 需要返回给客户端的结果的长度
    int offset = 0;
    // 记录客户端当前正在执行的事务ID
    txn_id_t txn_id = INVALID_TXN_ID;
    Transaction *session_txn = nullptr;
    SessionTransactionGuard transaction_guard(&txn_id, &session_txn);
    IsolationLevel session_isolation = IsolationLevel::SNAPSHOT_ISOLATION;
    StatementScratch statement_scratch;
    SessionPlanTemplateHotCache session_plan_cache;
    SessionPointProgramCache session_point_program_cache;

    ConnectionProtocol protocol = InitializeConnectionProtocol(fd, &command_framer);
    if (protocol == ConnectionProtocol::WireV3) {
        try {
            HandleWireV3Connection(fd, data_send.get(), &offset, &txn_id, &session_txn, &session_isolation,
                                   &statement_scratch, &session_plan_cache, &session_point_program_cache);
        } catch (const std::exception &error) {
            std::cerr << "Wire connection failed: " << error.what() << '\n';
        } catch (...) {
            std::cerr << "Wire connection failed with an unknown exception\n";
        }
        close(fd);
        return;
    }
    if (protocol == ConnectionProtocol::Closed) {
        close(fd);
        return;
    }

    while (true) {
        ReceiveCommandStatus receive_status = ReceiveCommand(fd, &command_framer, &command);
        if (receive_status != ReceiveCommandStatus::kCommand) {
            if (receive_status == ReceiveCommandStatus::kTooLarge) {
                static constexpr char failure[] = "failure\n";
                SendAll(fd, failure, sizeof(failure));
            }
            break;
        }
        const char *data_recv = command.c_str();

        if (strcmp(data_recv, "exit") == 0) {
            break;
        }
        if (strcmp(data_recv, "crash") == 0) {
            std::cout << "Server crash" << std::endl;
            exit(1);
        }

        offset = 0;
        data_send[0] = '\0';
        statement_scratch.reset();

        // 开启事务，初始化系统所需的上下文信息（包括事务对象指针、锁管理器指针、日志管理器指针、存放结果的buffer、记录结果长度的变量）
        Context statement_context(lock_manager.get(), log_manager.get(), txn_manager.get(), session_txn, data_send.get(),
                                  &offset, &session_isolation, &statement_scratch);
        Context *context = &statement_context;

        StatementExecutionResult execution = ExecuteSqlStatement(
            data_recv, &txn_id, context, &session_isolation, &session_plan_cache, &session_point_program_cache);
        if (execution.status != StatementStatus::Ok) {
            std::string str = execution.legacy_abort_response ? "abort\n" : "failure\n";
            memcpy(data_send.get(), str.c_str(), str.length());
            data_send[str.length()] = '\0';
            offset = str.length();
        }
        session_txn = context->txn_;
        // future TODO: 格式化 sql_handler.result, 传给客户端
        // send result with fixed format, use protobuf in the future
        if (offset < 0 || offset >= BUFFER_LENGTH) {
            break;
        }
        data_send[static_cast<size_t>(offset)] = '\0';
        if (!SendAll(fd, data_send.get(), static_cast<size_t>(offset) + 1)) {
            break;
        }
    }

    close(fd);           // close a file descriptor.
}

void start_server() {
    int sockfd_server;
    int fd_temp;
    struct sockaddr_in s_addr_in {};

    // 初始化连接
    sockfd_server = socket(AF_INET, SOCK_STREAM, 0);  // ipv4,TCP
    assert(sockfd_server != -1);
    int val = 1;
    setsockopt(sockfd_server, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    // before bind(), set the attr of structure sockaddr.
    memset(&s_addr_in, 0, sizeof(s_addr_in));
    s_addr_in.sin_family = AF_INET;
    s_addr_in.sin_addr.s_addr = htonl(INADDR_ANY);
    s_addr_in.sin_port = htons(SOCK_PORT);
    fd_temp = bind(sockfd_server, (struct sockaddr *)(&s_addr_in), sizeof(s_addr_in));
    if (fd_temp == -1) {
        std::cout << "Bind error!" << std::endl;
        exit(1);
    }

    fd_temp = listen(sockfd_server, MAX_CONN_LIMIT);
    if (fd_temp == -1) {
        std::cout << "Listen error!" << std::endl;
        exit(1);
    }
    server_listen_fd = sockfd_server;

    // 进程级后台维护组件:统一调度 GC(2ms)/脏页刷盘(50ms)/WAL 回收。
    maintenance_engine = std::make_unique<MaintenanceEngine>(txn_manager.get(),
                                                             buffer_pool_manager.get(),
                                                             disk_manager.get(),
                                                             log_manager.get(), [] {
        auto statement_guard = txn_manager->EnterStatementExecution();
        ql_manager->create_fuzzy_checkpoint(log_manager.get());
    });
    maintenance_engine->Start();

    std::vector<std::jthread> client_threads;

    while (!shutdown_requested.load(std::memory_order_relaxed)) {
        struct sockaddr_in s_addr_client {};
        int client_length = sizeof(s_addr_client);

        // accept 会一直阻塞；SIGINT 处理函数通过关闭监听 fd 使其立即返回。
        int sockfd = accept(sockfd_server, (struct sockaddr *)(&s_addr_client), (socklen_t *)(&client_length));
        if (sockfd == -1) {
            if (shutdown_requested.load(std::memory_order_relaxed) || errno == EBADF || errno == EINVAL) {
                break;
            }
            if (errno != EINTR) {
                std::cerr << "Accept error: " << strerror(errno) << std::endl;
            }
            continue;
        }
        int tcp_nodelay = 1;
        if (setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &tcp_nodelay, sizeof(tcp_nodelay)) != 0) {
            std::cerr << "TCP_NODELAY error: " << strerror(errno) << std::endl;
            close(sockfd);
            continue;
        }

        // 和客户端建立连接，并开启一个线程负责处理客户端请求
        {
            std::lock_guard<std::mutex> guard(client_registry_mutex);
            active_client_fds.insert(sockfd);
        }
        try {
            client_threads.emplace_back(client_handler, sockfd);
        } catch (const std::system_error &error) {
            {
                std::lock_guard<std::mutex> guard(client_registry_mutex);
                active_client_fds.erase(sockfd);
            }
            close(sockfd);
            std::cerr << "Failed to start client thread: " << error.what() << std::endl;
            break;
        }

    }

    if (server_listen_fd == sockfd_server) {
        server_listen_fd = -1;
        close(sockfd_server);
    }
    std::cout << "Try to close all client connections.\n";
    {
        std::lock_guard<std::mutex> guard(client_registry_mutex);
        for (int fd : active_client_fds) {
            shutdown(fd, SHUT_RDWR);
        }
    }
    // 每个 jthread 析构时都会 join；必须先等所有会话退出，再停止后台维护组件
    // （组件持有 txn_manager/bpm 指针），最后释放事务、日志和存储管理器。
    client_threads.clear();
    maintenance_engine.reset();
    log_manager->flush_log_to_disk();
    txn_manager->PhysicalizeCommittedDeletes();
    sm_manager->close_db();
    std::cout << " DB has been closed.\n";
    std::cout << "Server shuts down." << std::endl;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        // 需要指定数据库名称
        std::cerr << "Usage: " << argv[0] << " <database>" << std::endl;
        exit(1);
    }

    signal(SIGINT, sigint_handler);
    try {
        InitializeManagers(configured_buffer_pool_pages());
        std::cout << "\n"
                     "  _____  __  __ _____  ____  \n"
                     " |  __ \\|  \\/  |  __ \\|  _ \\ \n"
                     " | |__) | \\  / | |  | | |_) |\n"
                     " |  _  /| |\\/| | |  | |  _ < \n"
                     " | | \\ \\| |  | | |__| | |_) |\n"
                     " |_|  \\_\\_|  |_|_____/|____/ \n"
                     "\n"
                     "Welcome to RMDB!\n"
                     "Type 'help;' for help.\n"
                     "\n";
        // Database name is passed by args
        std::string db_name = argv[1];
        if (!sm_manager->is_dir(db_name)) {
            // Database not found, create a new one
            sm_manager->create_db(db_name);
        }
        // Open database
        auto recovery_started = std::chrono::steady_clock::now();
        sm_manager->open_db(db_name);
        auto recovery_opened = std::chrono::steady_clock::now();
        const lsn_t log_end_lsn = disk_manager->get_log_end_lsn();
        log_manager->reset_log_file_offset(log_end_lsn);

        // recovery database
        recovery->analyze();
        if (recovery->scan_end_lsn() < log_end_lsn) {
            // A crash may leave a physically extended but incomplete final WAL
            // record. New CLRs must overwrite that tail; appending after it would
            // make every later restart stop before the recovery-generated WAL.
            disk_manager->truncate_log_tail(recovery->scan_end_lsn());
            disk_manager->sync_log();
            log_manager->reset_log_file_offset(recovery->scan_end_lsn());
        }
        txn_manager->EnsureNextTransactionIdAtLeast(recovery->next_txn_id());
        auto recovery_analyzed = std::chrono::steady_clock::now();
        recovery->redo();
        auto recovery_redone = std::chrono::steady_clock::now();
        recovery->undo();
        auto recovery_undone = std::chrono::steady_clock::now();
        const bool derived_indexes = sm_manager->recovery_required();
        sm_manager->rebuild_indexes_after_recovery();
        auto recovery_indexed = std::chrono::steady_clock::now();
        PersistRecoveredState(derived_indexes);
        auto recovery_persisted = std::chrono::steady_clock::now();
        auto elapsed_ms = [](auto begin, auto end) {
            return std::chrono::duration<double, std::milli>(end - begin).count();
        };
        std::cerr << "Recovery startup timing: open_ms=" << elapsed_ms(recovery_started, recovery_opened)
                  << " analyze_ms=" << elapsed_ms(recovery_opened, recovery_analyzed)
                  << " redo_ms=" << elapsed_ms(recovery_analyzed, recovery_redone)
                  << " undo_ms=" << elapsed_ms(recovery_redone, recovery_undone)
                  << " index_ms=" << elapsed_ms(recovery_undone, recovery_indexed)
                  << " persist_ms=" << elapsed_ms(recovery_indexed, recovery_persisted)
                  << " total_ms=" << elapsed_ms(recovery_started, recovery_persisted) << "\n";
        
        // 开启服务端，开始接受客户端连接
        start_server();
    } catch (RMDBError &e) {
        std::cerr << e.what() << std::endl;
        exit(1);
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
        exit(1);
    }
    return 0;
}
