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
#include <condition_variable>
#include <array>
#include <exception>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <iostream>
#include "log_defs.h"
#include "common/config.h"
#include "system/sm_meta.h"
#include "common/types.h"
#include "record/rm_defs.h"

/* 日志记录对应操作的类型。显式值属于持久化日志格式，不可重排。 */
enum class LogType : rmdb::i32 {
    kUpdate = 0,
    kInsert = 1,
    kDelete = 2,
    kBegin = 3,
    kCommit = 4,
    kAbort = 5,
    kCheckpoint = 6,
    kEnd = 7,
    kClr = 8,
    kBeginCheckpoint = 9,
    kEndCheckpoint = 10,
};

inline constexpr auto kLogTypeNames = std::to_array<std::string_view>({
    "UPDATE",
    "INSERT",
    "DELETE",
    "BEGIN",
    "COMMIT",
    "ABORT",
    "CHECKPOINT",
    "END",
    "CLR",
    "BEGIN_CHECKPOINT",
    "END_CHECKPOINT",
});

static_assert(kLogTypeNames.size() == static_cast<rmdb::usize>(LogType::kEndCheckpoint) + 1);

inline constexpr std::string_view log_type_name(LogType type) noexcept {
    const auto index = static_cast<rmdb::usize>(type);
    return index < kLogTypeNames.size() ? kLogTypeNames[index] : "UNKNOWN";
}

static_assert(sizeof(LogType) == sizeof(rmdb::i32));

class LogRecord {
public:
    virtual ~LogRecord() = default;

    LogType log_type_;         /* 日志对应操作的类型 */
    lsn_t lsn_;                /* 当前日志的lsn */
    rmdb::u32 log_tot_len_;     /* 整个日志记录的长度 */
    txn_id_t log_tid_;         /* 创建当前日志的事务ID */
    lsn_t prev_lsn_;           /* 事务创建的前一条日志记录的lsn，用于undo */

    // 把日志记录序列化到dest中
    virtual void serialize (char* dest) const {
        memcpy(dest + OFFSET_LOG_TYPE, &log_type_, sizeof(LogType));
        memcpy(dest + OFFSET_LSN, &lsn_, sizeof(lsn_t));
        memcpy(dest + OFFSET_LOG_TOT_LEN, &log_tot_len_, sizeof(rmdb::u32));
        memcpy(dest + OFFSET_LOG_TID, &log_tid_, sizeof(txn_id_t));
        memcpy(dest + OFFSET_PREV_LSN, &prev_lsn_, sizeof(lsn_t));
    }
    // 从src中反序列化出一条日志记录
    virtual void deserialize(const char* src) {
        memcpy(&log_type_, src + OFFSET_LOG_TYPE, sizeof(LogType));
        memcpy(&lsn_, src + OFFSET_LSN, sizeof(lsn_t));
        memcpy(&log_tot_len_, src + OFFSET_LOG_TOT_LEN, sizeof(rmdb::u32));
        memcpy(&log_tid_, src + OFFSET_LOG_TID, sizeof(txn_id_t));
        memcpy(&prev_lsn_, src + OFFSET_PREV_LSN, sizeof(lsn_t));
    }
    // used for debug
    virtual void format_print() {
        std::cout << "log type in father_function: " << log_type_name(log_type_) << "\n";
        std::cout << "Print Log Record:\n";
        std::cout << "log_type_: " << log_type_name(log_type_) << "\n";
        std::cout << "lsn: " << lsn_ << "\n";
        std::cout << "log_tot_len: " << log_tot_len_ << "\n";
        std::cout << "log_tid: " << log_tid_ << "\n";
        std::cout << "prev_lsn: " << prev_lsn_ << "\n";
    }
};

class BeginLogRecord: public LogRecord {
public:
    BeginLogRecord() {
        log_type_ = LogType::kBegin;
        lsn_ = INVALID_LSN;
        log_tot_len_ = LOG_HEADER_SIZE;
        log_tid_ = INVALID_TXN_ID;
        prev_lsn_ = INVALID_LSN;
    }
    BeginLogRecord(txn_id_t txn_id) : BeginLogRecord() {
        log_tid_ = txn_id;
    }
    // 序列化Begin日志记录到dest中
    void serialize(char* dest) const override {
        LogRecord::serialize(dest);
    }
    // 从src中反序列化出一条Begin日志记录
    void deserialize(const char* src) override {
        LogRecord::deserialize(src);   
    }
    virtual void format_print() override {
        std::cout << "log type in son_function: " << log_type_name(log_type_) << "\n";
        LogRecord::format_print();
    }
};

class CommitLogRecord: public LogRecord {
public:
    CommitLogRecord() {
        log_type_ = LogType::kCommit;
        lsn_ = INVALID_LSN;
        log_tot_len_ = LOG_HEADER_SIZE;
        log_tid_ = INVALID_TXN_ID;
        prev_lsn_ = INVALID_LSN;
    }
    CommitLogRecord(txn_id_t txn_id) : CommitLogRecord() {
        log_tid_ = txn_id;
    }
    void serialize(char* dest) const override {
        LogRecord::serialize(dest);
    }
    void deserialize(const char* src) override {
        LogRecord::deserialize(src);
    }
    void format_print() override {
        printf("commit record\n");
        LogRecord::format_print();
    }
};

class AbortLogRecord: public LogRecord {
public:
    AbortLogRecord() {
        log_type_ = LogType::kAbort;
        lsn_ = INVALID_LSN;
        log_tot_len_ = LOG_HEADER_SIZE;
        log_tid_ = INVALID_TXN_ID;
        prev_lsn_ = INVALID_LSN;
    }
    AbortLogRecord(txn_id_t txn_id) : AbortLogRecord() {
        log_tid_ = txn_id;
    }
    void serialize(char* dest) const override {
        LogRecord::serialize(dest);
    }
    void deserialize(const char* src) override {
        LogRecord::deserialize(src);
    }
    void format_print() override {
        printf("abort record\n");
        LogRecord::format_print();
    }
};

class EndLogRecord: public LogRecord {
public:
    EndLogRecord() {
        log_type_ = LogType::kEnd;
        lsn_ = INVALID_LSN;
        log_tot_len_ = LOG_HEADER_SIZE;
        log_tid_ = INVALID_TXN_ID;
        prev_lsn_ = INVALID_LSN;
    }
    explicit EndLogRecord(txn_id_t txn_id) : EndLogRecord() { log_tid_ = txn_id; }
};

class BeginCheckpointLogRecord: public LogRecord {
public:
    BeginCheckpointLogRecord() {
        log_type_ = LogType::kBeginCheckpoint;
        lsn_ = INVALID_LSN;
        log_tot_len_ = LOG_HEADER_SIZE;
        log_tid_ = INVALID_TXN_ID;
        prev_lsn_ = INVALID_LSN;
    }
};

struct CheckpointTxnInfo {
    txn_id_t txn_id_{INVALID_TXN_ID};
    lsn_t last_lsn_{INVALID_LSN};
};

class CheckpointLogRecord: public LogRecord {
public:
    CheckpointLogRecord() {
        log_type_ = LogType::kCheckpoint;
        lsn_ = INVALID_LSN;
        log_tot_len_ = LOG_HEADER_SIZE;
        log_tid_ = INVALID_TXN_ID;
        prev_lsn_ = INVALID_LSN;
        active_txn_count_ = 0;
    }
    CheckpointLogRecord(const std::vector<CheckpointTxnInfo>& active_txns) : CheckpointLogRecord() {
        active_txn_count_ = static_cast<int>(active_txns.size());
        active_txns_ = active_txns;
        log_tot_len_ += sizeof(int);
        if (active_txn_count_ > 0) {
            log_tot_len_ += active_txn_count_ * (sizeof(txn_id_t) + sizeof(lsn_t));
        }
    }
    void serialize(char* dest) const override {
        LogRecord::serialize(dest);
        int offset = OFFSET_LOG_DATA;
        memcpy(dest + offset, &active_txn_count_, sizeof(int));
        offset += sizeof(int);
        for (int i = 0; i < active_txn_count_; ++i) {
            memcpy(dest + offset, &active_txns_[i].txn_id_, sizeof(txn_id_t));
            offset += sizeof(txn_id_t);
            memcpy(dest + offset, &active_txns_[i].last_lsn_, sizeof(lsn_t));
            offset += sizeof(lsn_t);
        }
    }
    void deserialize(const char* src) override {
        LogRecord::deserialize(src);
        int offset = OFFSET_LOG_DATA;
        memcpy(&active_txn_count_, src + offset, sizeof(int));
        offset += sizeof(int);
        active_txns_.resize(active_txn_count_);
        for (int i = 0; i < active_txn_count_; ++i) {
            memcpy(&active_txns_[i].txn_id_, src + offset, sizeof(txn_id_t));
            offset += sizeof(txn_id_t);
            memcpy(&active_txns_[i].last_lsn_, src + offset, sizeof(lsn_t));
            offset += sizeof(lsn_t);
        }
    }
    void format_print() override {
        printf("checkpoint record\n");
        LogRecord::format_print();
        printf("active_txn_count: %d\n", active_txn_count_);
    }
    int active_txn_count_;
    std::vector<CheckpointTxnInfo> active_txns_;   // 检查点时刻活跃事务及其最近日志位置
};

enum class RecoveryTxnStatus : rmdb::u8 {
    kRunning = 0,
    kCommitting = 1,
    kAborting = 2,
};

struct CheckpointTxnEntry {
    txn_id_t txn_id_{INVALID_TXN_ID};
    RecoveryTxnStatus status_{RecoveryTxnStatus::kRunning};
    lsn_t last_lsn_{INVALID_LSN};
};

struct CheckpointDirtyPageInfo {
    rmdb::u64 table_hash_{0};
    page_id_t page_no_{INVALID_PAGE_ID};
    lsn_t rec_lsn_{INVALID_LSN};
};

class EndCheckpointLogRecord: public LogRecord {
public:
    EndCheckpointLogRecord() {
        log_type_ = LogType::kEndCheckpoint;
        lsn_ = INVALID_LSN;
        log_tot_len_ = LOG_HEADER_SIZE + sizeof(lsn_t) + sizeof(txn_id_t) + 2 * sizeof(rmdb::u32);
        log_tid_ = INVALID_TXN_ID;
        prev_lsn_ = INVALID_LSN;
    }

    EndCheckpointLogRecord(lsn_t begin_lsn, txn_id_t next_txn_id,
                           std::vector<CheckpointTxnEntry> txn_table,
                           std::vector<CheckpointDirtyPageInfo> dirty_pages)
        : EndCheckpointLogRecord() {
        begin_lsn_ = begin_lsn;
        next_txn_id_ = next_txn_id;
        txn_table_ = std::move(txn_table);
        dirty_pages_ = std::move(dirty_pages);
        log_tot_len_ += static_cast<rmdb::u32>(
            txn_table_.size() * (sizeof(txn_id_t) + sizeof(RecoveryTxnStatus) + sizeof(lsn_t)) +
            dirty_pages_.size() * (sizeof(rmdb::u64) + sizeof(page_id_t) + sizeof(lsn_t)));
    }

    void serialize(char *dest) const override {
        LogRecord::serialize(dest);
        size_t offset = OFFSET_LOG_DATA;
        memcpy(dest + offset, &begin_lsn_, sizeof(begin_lsn_));
        offset += sizeof(begin_lsn_);
        memcpy(dest + offset, &next_txn_id_, sizeof(next_txn_id_));
        offset += sizeof(next_txn_id_);
        const rmdb::u32 txn_count = static_cast<rmdb::u32>(txn_table_.size());
        memcpy(dest + offset, &txn_count, sizeof(txn_count));
        offset += sizeof(txn_count);
        for (const auto &entry : txn_table_) {
            memcpy(dest + offset, &entry.txn_id_, sizeof(entry.txn_id_));
            offset += sizeof(entry.txn_id_);
            memcpy(dest + offset, &entry.status_, sizeof(entry.status_));
            offset += sizeof(entry.status_);
            memcpy(dest + offset, &entry.last_lsn_, sizeof(entry.last_lsn_));
            offset += sizeof(entry.last_lsn_);
        }
        const rmdb::u32 dirty_count = static_cast<rmdb::u32>(dirty_pages_.size());
        memcpy(dest + offset, &dirty_count, sizeof(dirty_count));
        offset += sizeof(dirty_count);
        for (const auto &entry : dirty_pages_) {
            memcpy(dest + offset, &entry.table_hash_, sizeof(entry.table_hash_));
            offset += sizeof(entry.table_hash_);
            memcpy(dest + offset, &entry.page_no_, sizeof(entry.page_no_));
            offset += sizeof(entry.page_no_);
            memcpy(dest + offset, &entry.rec_lsn_, sizeof(entry.rec_lsn_));
            offset += sizeof(entry.rec_lsn_);
        }
    }

    void deserialize(const char *src) override {
        LogRecord::deserialize(src);
        size_t offset = OFFSET_LOG_DATA;
        memcpy(&begin_lsn_, src + offset, sizeof(begin_lsn_));
        offset += sizeof(begin_lsn_);
        memcpy(&next_txn_id_, src + offset, sizeof(next_txn_id_));
        offset += sizeof(next_txn_id_);
        rmdb::u32 txn_count = 0;
        memcpy(&txn_count, src + offset, sizeof(txn_count));
        offset += sizeof(txn_count);
        txn_table_.resize(txn_count);
        for (auto &entry : txn_table_) {
            memcpy(&entry.txn_id_, src + offset, sizeof(entry.txn_id_));
            offset += sizeof(entry.txn_id_);
            memcpy(&entry.status_, src + offset, sizeof(entry.status_));
            offset += sizeof(entry.status_);
            memcpy(&entry.last_lsn_, src + offset, sizeof(entry.last_lsn_));
            offset += sizeof(entry.last_lsn_);
        }
        rmdb::u32 dirty_count = 0;
        memcpy(&dirty_count, src + offset, sizeof(dirty_count));
        offset += sizeof(dirty_count);
        dirty_pages_.resize(dirty_count);
        for (auto &entry : dirty_pages_) {
            memcpy(&entry.table_hash_, src + offset, sizeof(entry.table_hash_));
            offset += sizeof(entry.table_hash_);
            memcpy(&entry.page_no_, src + offset, sizeof(entry.page_no_));
            offset += sizeof(entry.page_no_);
            memcpy(&entry.rec_lsn_, src + offset, sizeof(entry.rec_lsn_));
            offset += sizeof(entry.rec_lsn_);
        }
    }

    lsn_t begin_lsn_{INVALID_LSN};
    txn_id_t next_txn_id_{0};
    std::vector<CheckpointTxnEntry> txn_table_;
    std::vector<CheckpointDirtyPageInfo> dirty_pages_;
};

// DML 日志用 FNV-1a 64 位表名哈希替代全字符串:每条记录省 ~20-30 字节,
// 且不依赖表注册顺序(恢复侧按哈希查表)。
inline rmdb::u64 HashTableName(const std::string &table_name) {
    rmdb::u64 hash = 1469598103934665603ULL;
    for (unsigned char ch : table_name) {
        hash ^= ch;
        hash *= 1099511628211ULL;
    }
    return hash;
}

class InsertLogRecord: public LogRecord {
public:
    InsertLogRecord() {
        log_type_ = LogType::kInsert;
        lsn_ = INVALID_LSN;
        log_tot_len_ = LOG_HEADER_SIZE;
        log_tid_ = INVALID_TXN_ID;
        prev_lsn_ = INVALID_LSN;
    }
    InsertLogRecord(txn_id_t txn_id, const RmRecord& insert_value, const Rid& rid,
                    rmdb::u64 table_hash)
        : InsertLogRecord() {
        log_tid_ = txn_id;
        insert_value_view_ = &insert_value;
        rid_ = rid;
        log_tot_len_ += sizeof(int);
        log_tot_len_ += insert_value.size;
        log_tot_len_ += sizeof(Rid);
        table_hash_ = table_hash;
        log_tot_len_ += sizeof(rmdb::u64);
    }
    InsertLogRecord(txn_id_t txn_id, const RmRecord& insert_value, const Rid& rid,
                    const std::string& table_name)
        : InsertLogRecord(txn_id, insert_value, rid, HashTableName(table_name)) {}

    // 把insert日志记录序列化到dest中
    void serialize(char* dest) const override {
        LogRecord::serialize(dest);
        int offset = OFFSET_LOG_DATA;
        const RmRecord &insert_value = insert_value_view_ == nullptr ? insert_value_ : *insert_value_view_;
        memcpy(dest + offset, &insert_value.size, sizeof(int));
        offset += sizeof(int);
        memcpy(dest + offset, insert_value.data, insert_value.size);
        offset += insert_value.size;
        memcpy(dest + offset, &rid_, sizeof(Rid));
        offset += sizeof(Rid);
        memcpy(dest + offset, &table_hash_, sizeof(rmdb::u64));
        offset += sizeof(rmdb::u64);
    }
    // 从src中反序列化出一条Insert日志记录
    void deserialize(const char* src) override {
        insert_value_view_ = nullptr;
        LogRecord::deserialize(src);
        int offset = OFFSET_LOG_DATA;
        int record_size = 0;
        memcpy(&record_size, src + offset, sizeof(int));
        offset += sizeof(int);
        insert_value_.ResizeAndCopy(src + offset, record_size);
        offset += record_size;
        memcpy(&rid_, src + offset, sizeof(Rid));
        offset += sizeof(Rid);
        memcpy(&table_hash_, src + offset, sizeof(rmdb::u64));
        offset += sizeof(rmdb::u64);
    }
    void format_print() override {
        printf("insert record\n");
        LogRecord::format_print();
        const RmRecord &insert_value = insert_value_view_ == nullptr ? insert_value_ : *insert_value_view_;
        printf("insert_value: %s\n", insert_value.data);
        printf("insert rid: %d, %d\n", rid_.page_no, rid_.slot_no);
        printf("table hash: %llu\n", static_cast<unsigned long long>(table_hash_));
    }

    RmRecord insert_value_;     // 插入的记录
    const RmRecord *insert_value_view_{nullptr};  // add_log_to_buffer同步序列化期间借用
    Rid rid_;                   // 记录插入的位置
    rmdb::u64 table_hash_{0};  // 表名 FNV-1a 哈希(序列化用)
};

class DeleteLogRecord: public LogRecord {
public:
    DeleteLogRecord() {
        log_type_ = LogType::kDelete;
        lsn_ = INVALID_LSN;
        log_tot_len_ = LOG_HEADER_SIZE;
        log_tid_ = INVALID_TXN_ID;
        prev_lsn_ = INVALID_LSN;
    }
    DeleteLogRecord(txn_id_t txn_id, const RmRecord& old_record, const Rid& rid,
                    rmdb::u64 table_hash)
        : DeleteLogRecord() {
        log_tid_ = txn_id;
        old_record_view_ = &old_record;
        rid_ = rid;
        log_tot_len_ += sizeof(int) + old_record.size;
        log_tot_len_ += sizeof(Rid);
        table_hash_ = table_hash;
        log_tot_len_ += sizeof(rmdb::u64);
    }
    DeleteLogRecord(txn_id_t txn_id, const RmRecord& old_record, const Rid& rid,
                    const std::string& table_name)
        : DeleteLogRecord(txn_id, old_record, rid, HashTableName(table_name)) {}
    void serialize(char* dest) const override {
        LogRecord::serialize(dest);
        int offset = OFFSET_LOG_DATA;
        const RmRecord &old_record = old_record_view_ == nullptr ? old_record_ : *old_record_view_;
        memcpy(dest + offset, &old_record.size, sizeof(int));
        offset += sizeof(int);
        memcpy(dest + offset, old_record.data, old_record.size);
        offset += old_record.size;
        memcpy(dest + offset, &rid_, sizeof(Rid));
        offset += sizeof(Rid);
        memcpy(dest + offset, &table_hash_, sizeof(rmdb::u64));
        offset += sizeof(rmdb::u64);
    }
    void deserialize(const char* src) override {
        old_record_view_ = nullptr;
        LogRecord::deserialize(src);
        int offset = OFFSET_LOG_DATA;
        int record_size = 0;
        memcpy(&record_size, src + offset, sizeof(int));
        offset += sizeof(int);
        old_record_.ResizeAndCopy(src + offset, record_size);
        offset += record_size;
        memcpy(&rid_, src + offset, sizeof(Rid));
        offset += sizeof(Rid);
        memcpy(&table_hash_, src + offset, sizeof(rmdb::u64));
        offset += sizeof(rmdb::u64);
    }
    void format_print() override {
        printf("delete record\n");
        LogRecord::format_print();
        printf("delete rid: %d, %d\n", rid_.page_no, rid_.slot_no);
        printf("table hash: %llu\n", static_cast<unsigned long long>(table_hash_));
    }
    RmRecord old_record_;       // 被删除的记录（用于undo）
    const RmRecord *old_record_view_{nullptr};  // add_log_to_buffer同步序列化期间借用
    Rid rid_;                   // 记录的位置
    rmdb::u64 table_hash_{0};  // 表名 FNV-1a 哈希(序列化用)
};

// UPDATE logs use column-level deltas: only changed columns (old and new
// values) are recorded, while unchanged columns are reconstructed from the
// current data page during recovery.
struct UpdateColumnDelta {
    rmdb::u16 col_index{0};
    std::string old_value;  // recovery 反序列化后的自有字节
    std::string new_value;
    std::string_view old_value_view;
    std::string_view new_value_view;
    bool borrowed{false};

    std::string_view OldValue() const noexcept {
        return borrowed ? old_value_view : std::string_view(old_value);
    }

    std::string_view NewValue() const noexcept {
        return borrowed ? new_value_view : std::string_view(new_value);
    }
};

inline constexpr rmdb::u16 kFullRecordUpdateDelta =
    static_cast<rmdb::u16>(~static_cast<rmdb::u16>(0));

class UpdateLogRecord: public LogRecord {
public:
    UpdateLogRecord() {
        log_type_ = LogType::kUpdate;
        lsn_ = INVALID_LSN;
        log_tot_len_ = LOG_HEADER_SIZE;
        log_tid_ = INVALID_TXN_ID;
        prev_lsn_ = INVALID_LSN;
    }
    UpdateLogRecord(txn_id_t txn_id, const RmRecord& old_record, const RmRecord& new_record,
                    const Rid& rid, rmdb::u64 table_hash, const std::vector<ColMeta> *cols)
        : UpdateLogRecord() {
        log_tid_ = txn_id;
        rid_ = rid;
        table_hash_ = table_hash;
        record_size_ = old_record.size;
        if (cols != nullptr) {
            for (size_t i = 0; i < cols->size(); ++i) {
                const ColMeta &col = (*cols)[i];
                const size_t col_end = static_cast<size_t>(col.offset) +
                                       static_cast<size_t>(col.len);
                if (col.offset < 0 || col.len < 0 ||
                    col_end > static_cast<size_t>(old_record.size) ||
                    col_end > static_cast<size_t>(new_record.size)) {
                    continue;
                }
                if (memcmp(old_record.data + col.offset, new_record.data + col.offset, col.len) == 0) {
                    continue;
                }
                UpdateColumnDelta delta;
                delta.col_index = static_cast<rmdb::u16>(i);
                delta.old_value_view = std::string_view(old_record.data + col.offset, col.len);
                delta.new_value_view = std::string_view(new_record.data + col.offset, col.len);
                delta.borrowed = true;
                deltas_.push_back(std::move(delta));
            }
        }
        if (deltas_.empty() && cols == nullptr) {
            // 无列元数据时使用显式整行哨兵。若列元数据存在且新旧行相同，
            // 0 个 delta 就是完整的 no-op redo，不需要复制整行。
            UpdateColumnDelta delta;
            delta.col_index = kFullRecordUpdateDelta;
            delta.old_value_view = std::string_view(old_record.data, old_record.size);
            delta.new_value_view = std::string_view(new_record.data, new_record.size);
            delta.borrowed = true;
            deltas_.push_back(std::move(delta));
        }
        log_tot_len_ += sizeof(rmdb::u16);
        for (const auto &delta : deltas_) {
            log_tot_len_ += sizeof(rmdb::u16) + sizeof(rmdb::u32) + delta.OldValue().size() +
                            sizeof(rmdb::u32) + delta.NewValue().size();
        }
        log_tot_len_ += sizeof(Rid) + sizeof(rmdb::u64);
    }
    UpdateLogRecord(txn_id_t txn_id, const RmRecord& old_record, const RmRecord& new_record,
                    const Rid& rid, const std::string& table_name,
                    const std::vector<ColMeta> *cols)
        : UpdateLogRecord(txn_id, old_record, new_record, rid,
                          HashTableName(table_name), cols) {}
    void serialize(char* dest) const override {
        LogRecord::serialize(dest);
        int offset = OFFSET_LOG_DATA;
        rmdb::u16 delta_count = static_cast<rmdb::u16>(deltas_.size());
        memcpy(dest + offset, &delta_count, sizeof(delta_count));
        offset += sizeof(delta_count);
        for (const auto &delta : deltas_) {
            memcpy(dest + offset, &delta.col_index, sizeof(delta.col_index));
            offset += sizeof(delta.col_index);
            const std::string_view old_value = delta.OldValue();
            rmdb::u32 old_len = static_cast<rmdb::u32>(old_value.size());
            memcpy(dest + offset, &old_len, sizeof(old_len));
            offset += sizeof(old_len);
            memcpy(dest + offset, old_value.data(), old_len);
            offset += old_len;
            const std::string_view new_value = delta.NewValue();
            rmdb::u32 new_len = static_cast<rmdb::u32>(new_value.size());
            memcpy(dest + offset, &new_len, sizeof(new_len));
            offset += sizeof(new_len);
            memcpy(dest + offset, new_value.data(), new_len);
            offset += new_len;
        }
        memcpy(dest + offset, &rid_, sizeof(Rid));
        offset += sizeof(Rid);
        memcpy(dest + offset, &table_hash_, sizeof(rmdb::u64));
        offset += sizeof(rmdb::u64);
    }
    void deserialize(const char* src) override {
        LogRecord::deserialize(src);
        int offset = OFFSET_LOG_DATA;
        rmdb::u16 delta_count = 0;
        memcpy(&delta_count, src + offset, sizeof(delta_count));
        offset += sizeof(delta_count);
        deltas_.clear();
        deltas_.reserve(delta_count);
        for (rmdb::u16 i = 0; i < delta_count; ++i) {
            UpdateColumnDelta delta;
            memcpy(&delta.col_index, src + offset, sizeof(delta.col_index));
            offset += sizeof(delta.col_index);
            rmdb::u32 old_len = 0;
            memcpy(&old_len, src + offset, sizeof(old_len));
            offset += sizeof(old_len);
            delta.old_value.assign(src + offset, old_len);
            offset += old_len;
            rmdb::u32 new_len = 0;
            memcpy(&new_len, src + offset, sizeof(new_len));
            offset += sizeof(new_len);
            delta.new_value.assign(src + offset, new_len);
            offset += new_len;
            deltas_.push_back(std::move(delta));
        }
        memcpy(&rid_, src + offset, sizeof(Rid));
        offset += sizeof(Rid);
        memcpy(&table_hash_, src + offset, sizeof(rmdb::u64));
        offset += sizeof(rmdb::u64);
    }
    void format_print() override {
        printf("update record\n");
        LogRecord::format_print();
        printf("update rid: %d, %d\n", rid_.page_no, rid_.slot_no);
        printf("table hash: %llu delta columns: %zu\n", static_cast<unsigned long long>(table_hash_),
               deltas_.size());
    }
    std::vector<UpdateColumnDelta> deltas_;   // 变化列的旧值/新值
    Rid rid_;                                 // 记录的位置
    rmdb::u64 table_hash_{0};                 // 表名 FNV-1a 哈希(序列化用)
    int record_size_{0};                      // 记录总字节数(重建时校验)
};

// CLR 保存被补偿 DML 的完整原始字节。恢复重启时 REDO CLR 可直接重放其逆操作，
// UNDO 则沿 undo_next_lsn_ 跳过已经完成的补偿，从而保证恢复过程本身可重启。
class ClrLogRecord: public LogRecord {
public:
    ClrLogRecord() {
        log_type_ = LogType::kClr;
        lsn_ = INVALID_LSN;
        log_tot_len_ = LOG_HEADER_SIZE + sizeof(lsn_t) + sizeof(rmdb::u32);
        log_tid_ = INVALID_TXN_ID;
        prev_lsn_ = INVALID_LSN;
    }

    ClrLogRecord(txn_id_t txn_id, lsn_t undo_next_lsn, const LogRecord &compensated)
        : ClrLogRecord() {
        log_tid_ = txn_id;
        undo_next_lsn_ = undo_next_lsn;
        // add_log_to_buffer serializes synchronously while the compensated record
        // is alive. Borrow it here to avoid a heap allocation and a full DML copy
        // for every runtime abort; deserialized recovery CLRs still own bytes.
        compensated_record_ = &compensated;
        log_tot_len_ += compensated.log_tot_len_;
    }

    void serialize(char *dest) const override {
        LogRecord::serialize(dest);
        size_t offset = OFFSET_LOG_DATA;
        memcpy(dest + offset, &undo_next_lsn_, sizeof(undo_next_lsn_));
        offset += sizeof(undo_next_lsn_);
        const rmdb::u32 compensated_len = compensated_record_ == nullptr
                                                ? static_cast<rmdb::u32>(compensated_log_.size())
                                                : compensated_record_->log_tot_len_;
        memcpy(dest + offset, &compensated_len, sizeof(compensated_len));
        offset += sizeof(compensated_len);
        if (compensated_len > 0) {
            if (compensated_record_ != nullptr) {
                compensated_record_->serialize(dest + offset);
            } else {
                memcpy(dest + offset, compensated_log_.data(), compensated_len);
            }
        }
    }

    void deserialize(const char *src) override {
        LogRecord::deserialize(src);
        size_t offset = OFFSET_LOG_DATA;
        memcpy(&undo_next_lsn_, src + offset, sizeof(undo_next_lsn_));
        offset += sizeof(undo_next_lsn_);
        rmdb::u32 compensated_len = 0;
        memcpy(&compensated_len, src + offset, sizeof(compensated_len));
        offset += sizeof(compensated_len);
        compensated_record_ = nullptr;
        compensated_log_.assign(src + offset, src + offset + compensated_len);
    }

    lsn_t undo_next_lsn_{INVALID_LSN};
    std::vector<char> compensated_log_;
    const LogRecord *compensated_record_{nullptr};
};

/* 单个日志缓冲区；LogManager 使用两个实例轮换接收前台写入和后台刷盘。 */

class LogBuffer {
public:
    LogBuffer() { 
        offset_ = 0; 
        memset(buffer_, 0, sizeof(buffer_));
    }

    [[nodiscard]] bool is_full(int append_size) const { return offset_ + append_size > LOG_BUFFER_SIZE; }

    void reset() { offset_ = 0; }

    char buffer_[LOG_BUFFER_SIZE + 1];
    int offset_;  // 下一条日志在缓冲区内的写入偏移
};

/* 日志管理器，负责把日志写入日志缓冲区，以及把日志缓冲区中的内容写入磁盘中 */
class LogManager {
public:
    explicit LogManager(DiskManager* disk_manager);
    ~LogManager();

    lsn_t add_log_to_buffer(LogRecord* log_record);
    void flush_log_to_disk();
    void flush_log_to_disk_until(lsn_t target_lsn);
    void flush_log_to_disk_until_group(lsn_t target_lsn);
    lsn_t get_log_file_offset();
    lsn_t get_persist_lsn();
    void reset_log_file_offset(lsn_t log_file_offset);

private:
    void request_flush_and_wait(lsn_t target_lsn, bool group_commit);
    void writer_loop() noexcept;
    void rethrow_writer_error_unlocked() const;

    std::mutex latch_;  // 保护双缓冲切换及以下刷盘状态
    std::condition_variable flush_cv_;
    std::condition_variable writer_cv_;
    // During group-commit waiting, release latch_ so new log records and waiters
    // can join the active batch. Only the writer waits on this condition variable;
    // other threads notify it when a flush, stop, or waiter-registration event
    // changes the predicate.
    std::mutex group_sleep_mutex_;
    std::condition_variable group_sleep_cv_;
    LogBuffer log_buffers_[2];  // active buffer 接收新日志，flush buffer 负责当前写盘
    LogBuffer *active_buffer_{&log_buffers_[0]};
    LogBuffer *flush_buffer_{&log_buffers_[1]};
    std::atomic<lsn_t> persist_lsn_{INVALID_LSN};  // 已持久化的最后一个字节偏移；原子可见，等待者可无锁自旋
    lsn_t active_start_offset_{0};  // active buffer 首字节对应的 WAL 文件偏移
    lsn_t next_log_offset_{0};      // 下一条日志记录的 WAL 文件偏移
    lsn_t flush_start_offset_{0};
    int flush_size_{0};
    bool flush_in_progress_{false};
    std::atomic<bool> immediate_flush_requested_{false};  // 聚合睡眠谓词在 latch_ 外读取
    std::atomic<bool> stop_writer_{false};
    // 当前 active buffer 已登记的 group flush 到达数；buffer swap 时归零。
    // 它不是仍在等待 I/O 的线程数，旧代 waiter 不会污染下一批。
    std::atomic<size_t> group_waiter_count_{0};
    // Arrival time of the oldest waiter in the active group-commit batch.
    // Small batches may extend the normal deadline for a short bootstrap window.
    std::chrono::steady_clock::time_point group_first_waiter_at_{};
    lsn_t requested_lsn_{INVALID_LSN};
    std::exception_ptr writer_error_;
    DiskManager* disk_manager_;
    std::thread writer_thread_;
}; 
