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
#include <functional>
#include <memory>

#include "common/config.h"
#include "common/types.h"
#include "defs.h"
#include "record/rm_defs.h"

/* 标识事务状态 */
enum class TransactionState { DEFAULT, GROWING, SHRINKING, COMMITTED, ABORTED };

/* 系统的隔离级别 */
enum class IsolationLevel { SNAPSHOT_ISOLATION, SERIALIZABLE };

/* 事务写操作类型，包括插入、删除、更新三种操作 */
enum class WType { INSERT_TUPLE = 0, DELETE_TUPLE, UPDATE_TUPLE};

/**
 * @brief 事务的写操作记录，用于事务的回滚
 * INSERT
 * --------------------------------
 * | wtype | tab_name | tuple_rid |
 * --------------------------------
 * DELETE / UPDATE
 * ----------------------------------------------
 * | wtype | tab_name | tuple_rid | tuple_value |
 * ----------------------------------------------
 */
class WriteRecord {
   public:
    WriteRecord() = default;

    // constructor for insert operation
    WriteRecord(WType wtype, const std::string &tab_name, const Rid &rid)
        : wtype_(wtype), tab_name_(tab_name), rid_(rid) {}

    // constructor for delete & update operation
    WriteRecord(WType wtype, const std::string &tab_name, const Rid &rid, const RmRecord &record,
                bool index_keys_changed = true)
        : wtype_(wtype), tab_name_(tab_name), rid_(rid), record_(std::make_shared<RmRecord>(record)),
          index_keys_changed_(index_keys_changed) {}

    WriteRecord(WType wtype, const std::string &tab_name, const Rid &rid, std::shared_ptr<RmRecord> record,
                bool index_keys_changed = true)
        : wtype_(wtype), tab_name_(tab_name), rid_(rid), record_(std::move(record)),
          index_keys_changed_(index_keys_changed) {}

    WriteRecord(const WriteRecord &) = default;
    WriteRecord &operator=(const WriteRecord &) = default;
    WriteRecord(WriteRecord &&) noexcept = default;
    WriteRecord &operator=(WriteRecord &&) noexcept = default;
    ~WriteRecord() = default;

    inline RmRecord &GetRecord() {
        assert(record_ != nullptr);
        return *record_;
    }

    inline Rid &GetRid() { return rid_; }

    inline WType &GetWriteType() { return wtype_; }

    inline std::string &GetTableName() { return tab_name_; }

    inline bool IndexKeysChanged() const { return index_keys_changed_; }

    void SetLogPosition(lsn_t lsn, lsn_t prev_lsn) {
        log_lsn_ = lsn;
        log_prev_lsn_ = prev_lsn;
    }

    [[nodiscard]] lsn_t GetLogLsn() const { return log_lsn_; }

    [[nodiscard]] lsn_t GetLogPrevLsn() const { return log_prev_lsn_; }

   private:
    WType wtype_;
    std::string tab_name_;
    Rid rid_;
    std::shared_ptr<RmRecord> record_;
    bool index_keys_changed_{true};
    lsn_t log_lsn_{INVALID_LSN};
    lsn_t log_prev_lsn_{INVALID_LSN};
};

/* 多粒度锁，加锁对象的类型，包括记录和表 */
enum class LockDataType { TABLE = 0, RECORD = 1 };

using lock_data_key_t = rmdb::u64;

/**
 * @description: 加锁对象的唯一标识
 */
class LockDataId {
   public:
    /* 表级锁 */
    LockDataId(int fd, LockDataType type) {
        assert(type == LockDataType::TABLE);
        fd_ = fd;
        type_ = type;
        rid_.page_no = -1;
        rid_.slot_no = -1;
    }

    /* 行级锁 */
    LockDataId(int fd, const Rid &rid, LockDataType type) {
        assert(type == LockDataType::RECORD);
        fd_ = fd;
        rid_ = rid;
        type_ = type;
    }

    inline lock_data_key_t Get() const {
        const auto fd_key = static_cast<lock_data_key_t>(static_cast<rmdb::u32>(fd_) & 0x7fffU);
        if (type_ == LockDataType::TABLE) {
            // fd_
            return fd_key << 48;
        }
        // fd_, rid_.page_no, rid.slot_no
        return (static_cast<lock_data_key_t>(1) << 63) |
               (fd_key << 48) |
               (static_cast<lock_data_key_t>(static_cast<rmdb::u32>(rid_.page_no)) << 16) |
               static_cast<lock_data_key_t>(static_cast<rmdb::u16>(rid_.slot_no));
    }

    bool operator==(const LockDataId &other) const {
        if (type_ != other.type_) return false;
        if (fd_ != other.fd_) return false;
        return rid_ == other.rid_;
    }
    int fd_;
    Rid rid_;
    LockDataType type_;
};

template <>
struct std::hash<LockDataId> {
    size_t operator()(const LockDataId &obj) const { return std::hash<lock_data_key_t>()(obj.Get()); }
};

/* 事务回滚原因 */
enum class AbortReason { LOCK_ON_SHIRINKING = 0, UPGRADE_CONFLICT, DEADLOCK_PREVENTION };

enum class AbortSubReason {
    UNKNOWN = 0,
    RECORD_LOCK_CONFLICT,
    UNCOMMITTED_WRITE_CONFLICT,
    STALE_SNAPSHOT_WRITE_CONFLICT,
    UNIQUE_KEY_CONFLICT,
    TARGET_NOT_VISIBLE,
    SSI_DANGEROUS_STRUCTURE,
};

inline const char *AbortSubReasonName(AbortSubReason reason) {
    switch (reason) {
        case AbortSubReason::RECORD_LOCK_CONFLICT: return "record_lock_conflict";
        case AbortSubReason::UNCOMMITTED_WRITE_CONFLICT: return "uncommitted_write_conflict";
        case AbortSubReason::STALE_SNAPSHOT_WRITE_CONFLICT: return "stale_snapshot_write_conflict";
        case AbortSubReason::UNIQUE_KEY_CONFLICT: return "unique_key_conflict";
        case AbortSubReason::TARGET_NOT_VISIBLE: return "target_not_visible";
        case AbortSubReason::SSI_DANGEROUS_STRUCTURE: return "ssi_dangerous_structure";
        case AbortSubReason::UNKNOWN: return "unknown";
    }
    return "unknown";
}

/* 事务回滚异常，在rmdb.cpp中进行处理 */
class TransactionAbortException : public std::exception {
    txn_id_t txn_id_;
    AbortReason abort_reason_;
    AbortSubReason abort_sub_reason_;

   public:
    explicit TransactionAbortException(txn_id_t txn_id, AbortReason abort_reason,
                                       AbortSubReason abort_sub_reason = AbortSubReason::UNKNOWN)
        : txn_id_(txn_id), abort_reason_(abort_reason), abort_sub_reason_(abort_sub_reason) {}

    txn_id_t get_transaction_id() { return txn_id_; }
    AbortReason GetAbortReason() { return abort_reason_; }
    AbortSubReason GetAbortSubReason() const { return abort_sub_reason_; }
    std::string GetInfo() {
        const std::string detail = " [subreason=" + std::string(AbortSubReasonName(abort_sub_reason_)) + "]\n";
        switch (abort_reason_) {
            case AbortReason::LOCK_ON_SHIRINKING: {
                return "Transaction " + std::to_string(txn_id_) +
                       " aborted because it cannot request locks on SHRINKING phase" + detail;
            } break;

            case AbortReason::UPGRADE_CONFLICT: {
                return "Transaction " + std::to_string(txn_id_) +
                       " aborted because another transaction is waiting for upgrading" + detail;
            } break;

            case AbortReason::DEADLOCK_PREVENTION: {
                return "Transaction " + std::to_string(txn_id_) + " aborted for deadlock prevention" + detail;
            } break;

            default: {
                return "Transaction aborted\n";
            } break;
        }
    }
};
