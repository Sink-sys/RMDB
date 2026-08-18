/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "log_recovery.h"
#include "common/config.h"
#include "record/rm_file_handle.h"
#include "recovery/checkpoint_master.h"
#include "system/sm_manager.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <queue>
#include <unistd.h>
#include <vector>

namespace {

constexpr size_t kMaxLogRecordSize = static_cast<size_t>(LOG_BUFFER_SIZE);

template <typename T>
bool ReadScalar(const char *data, size_t size, size_t offset, T *value) {
    if (value == nullptr || offset > size || sizeof(T) > size - offset) {
        return false;
    }
    memcpy(value, data + offset, sizeof(T));
    return true;
}

bool ConsumeBytes(size_t amount, size_t size, size_t *offset) {
    if (offset == nullptr || *offset > size || amount > size - *offset) {
        return false;
    }
    *offset += amount;
    return true;
}

bool ConsumeRecordImage(const char *data, size_t size, size_t *offset) {
    int record_size = 0;
    if (!ReadScalar(data, size, *offset, &record_size) ||
        !ConsumeBytes(sizeof(int), size, offset) || record_size <= 0 || record_size > RM_MAX_RECORD_SIZE) {
        return false;
    }
    return ConsumeBytes(static_cast<size_t>(record_size), size, offset);
}

bool ConsumeRidAndTableHash(const char *data, size_t size, size_t *offset) {
    if (!ConsumeBytes(sizeof(Rid), size, offset)) {
        return false;
    }
    return ConsumeBytes(sizeof(rmdb::u64), size, offset) && *offset == size;
}

bool ConsumeUpdateDeltaList(const char *data, size_t size, size_t *offset) {
    rmdb::u16 delta_count = 0;
    if (!ReadScalar(data, size, *offset, &delta_count) ||
        !ConsumeBytes(sizeof(delta_count), size, offset)) {
        return false;
    }
    for (rmdb::u16 i = 0; i < delta_count; ++i) {
        rmdb::u16 col_index = 0;
        if (!ReadScalar(data, size, *offset, &col_index) ||
            !ConsumeBytes(sizeof(col_index), size, offset)) {
            return false;
        }
        rmdb::u32 old_len = 0;
        if (!ReadScalar(data, size, *offset, &old_len) ||
            !ConsumeBytes(sizeof(old_len), size, offset) || old_len > RM_MAX_RECORD_SIZE ||
            !ConsumeBytes(old_len, size, offset)) {
            return false;
        }
        rmdb::u32 new_len = 0;
        if (!ReadScalar(data, size, *offset, &new_len) ||
            !ConsumeBytes(sizeof(new_len), size, offset) || new_len > RM_MAX_RECORD_SIZE ||
            !ConsumeBytes(new_len, size, offset)) {
            return false;
        }
    }
    return ConsumeRidAndTableHash(data, size, offset);
}

bool ValidateLogRecord(const char *data, size_t size, lsn_t expected_lsn) {
    if (data == nullptr || size < static_cast<size_t>(LOG_HEADER_SIZE) || size > kMaxLogRecordSize ||
        expected_lsn < 0) {
        return false;
    }

    LogType log_type;
    lsn_t stored_lsn = INVALID_LSN;
    rmdb::u32 total_len = 0;
    lsn_t prev_lsn = INVALID_LSN;
    if (!ReadScalar(data, size, OFFSET_LOG_TYPE, &log_type) ||
        !ReadScalar(data, size, OFFSET_LSN, &stored_lsn) ||
        !ReadScalar(data, size, OFFSET_LOG_TOT_LEN, &total_len) ||
        !ReadScalar(data, size, OFFSET_PREV_LSN, &prev_lsn) || total_len != size || stored_lsn != expected_lsn ||
        (prev_lsn != INVALID_LSN && (prev_lsn < 0 || prev_lsn >= stored_lsn))) {
        return false;
    }

    size_t offset = OFFSET_LOG_DATA;
    switch (log_type) {
        case LogType::kBegin:
        case LogType::kCommit:
        case LogType::kAbort:
        case LogType::kEnd:
        case LogType::kBeginCheckpoint:
            return size == static_cast<size_t>(LOG_HEADER_SIZE);
        case LogType::kCheckpoint: {
            int active_txn_count = 0;
            if (!ReadScalar(data, size, offset, &active_txn_count) ||
                !ConsumeBytes(sizeof(int), size, &offset) || active_txn_count < 0) {
                return false;
            }
            constexpr size_t entry_size = sizeof(txn_id_t) + sizeof(lsn_t);
            size_t remaining = size - offset;
            if (static_cast<size_t>(active_txn_count) > remaining / entry_size ||
                static_cast<size_t>(active_txn_count) * entry_size != remaining) {
                return false;
            }
            for (int i = 0; i < active_txn_count; ++i) {
                txn_id_t txn_id = INVALID_TXN_ID;
                lsn_t last_lsn = INVALID_LSN;
                if (!ReadScalar(data, size, offset, &txn_id) ||
                    !ConsumeBytes(sizeof(txn_id_t), size, &offset) ||
                    !ReadScalar(data, size, offset, &last_lsn) ||
                    !ConsumeBytes(sizeof(lsn_t), size, &offset) || txn_id == INVALID_TXN_ID ||
                    (last_lsn != INVALID_LSN && (last_lsn < 0 || last_lsn >= stored_lsn))) {
                    return false;
                }
            }
            return offset == size;
        }
        case LogType::kInsert:
        case LogType::kDelete:
            return ConsumeRecordImage(data, size, &offset) && ConsumeRidAndTableHash(data, size, &offset);
        case LogType::kUpdate:
            return ConsumeUpdateDeltaList(data, size, &offset);
        case LogType::kClr: {
            lsn_t undo_next_lsn = INVALID_LSN;
            rmdb::u32 compensated_len = 0;
            if (!ReadScalar(data, size, offset, &undo_next_lsn) ||
                !ConsumeBytes(sizeof(undo_next_lsn), size, &offset) ||
                (undo_next_lsn != INVALID_LSN && (undo_next_lsn < 0 || undo_next_lsn >= stored_lsn)) ||
                !ReadScalar(data, size, offset, &compensated_len) ||
                !ConsumeBytes(sizeof(compensated_len), size, &offset) ||
                compensated_len < static_cast<rmdb::u32>(LOG_HEADER_SIZE) ||
                compensated_len != size - offset) {
                return false;
            }
            LogType compensated_type;
            lsn_t compensated_lsn = INVALID_LSN;
            txn_id_t compensated_tid = INVALID_TXN_ID;
            if (!ReadScalar(data, size, offset + OFFSET_LOG_TYPE, &compensated_type) ||
                !ReadScalar(data, size, offset + OFFSET_LSN, &compensated_lsn) ||
                !ReadScalar(data, size, offset + OFFSET_LOG_TID, &compensated_tid) ||
                (compensated_type != LogType::kInsert && compensated_type != LogType::kDelete &&
                 compensated_type != LogType::kUpdate)) {
                return false;
            }
            txn_id_t outer_tid = INVALID_TXN_ID;
            if (!ReadScalar(data, size, OFFSET_LOG_TID, &outer_tid) || compensated_tid != outer_tid ||
                compensated_lsn < 0 || compensated_lsn >= stored_lsn) {
                return false;
            }
            return ValidateLogRecord(data + offset, compensated_len, compensated_lsn);
        }
        case LogType::kEndCheckpoint: {
            lsn_t begin_lsn = INVALID_LSN;
            txn_id_t next_txn_id = 0;
            rmdb::u32 txn_count = 0;
            if (!ReadScalar(data, size, offset, &begin_lsn) ||
                !ConsumeBytes(sizeof(begin_lsn), size, &offset) || begin_lsn < 0 || begin_lsn >= stored_lsn ||
                !ReadScalar(data, size, offset, &next_txn_id) ||
                !ConsumeBytes(sizeof(next_txn_id), size, &offset) || next_txn_id < 0 ||
                !ReadScalar(data, size, offset, &txn_count) ||
                !ConsumeBytes(sizeof(txn_count), size, &offset)) {
                return false;
            }
            constexpr size_t txn_entry_size =
                sizeof(txn_id_t) + sizeof(RecoveryTxnStatus) + sizeof(lsn_t);
            if (txn_count > (size - offset) / txn_entry_size) {
                return false;
            }
            for (rmdb::u32 i = 0; i < txn_count; ++i) {
                txn_id_t txn_id = INVALID_TXN_ID;
                RecoveryTxnStatus status = RecoveryTxnStatus::kRunning;
                lsn_t last_lsn = INVALID_LSN;
                if (!ReadScalar(data, size, offset, &txn_id) ||
                    !ConsumeBytes(sizeof(txn_id), size, &offset) ||
                    !ReadScalar(data, size, offset, &status) ||
                    !ConsumeBytes(sizeof(status), size, &offset) ||
                    !ReadScalar(data, size, offset, &last_lsn) ||
                    !ConsumeBytes(sizeof(last_lsn), size, &offset) || txn_id == INVALID_TXN_ID ||
                    static_cast<rmdb::u8>(status) > static_cast<rmdb::u8>(RecoveryTxnStatus::kAborting) ||
                    (last_lsn != INVALID_LSN && (last_lsn < 0 || last_lsn >= stored_lsn))) {
                    return false;
                }
            }
            rmdb::u32 dirty_count = 0;
            if (!ReadScalar(data, size, offset, &dirty_count) ||
                !ConsumeBytes(sizeof(dirty_count), size, &offset)) {
                return false;
            }
            constexpr size_t dirty_entry_size = sizeof(rmdb::u64) + sizeof(page_id_t) + sizeof(lsn_t);
            if (dirty_count != (size - offset) / dirty_entry_size ||
                static_cast<size_t>(dirty_count) * dirty_entry_size != size - offset) {
                return false;
            }
            for (rmdb::u32 i = 0; i < dirty_count; ++i) {
                rmdb::u64 table_hash = 0;
                page_id_t page_no = INVALID_PAGE_ID;
                lsn_t rec_lsn = INVALID_LSN;
                if (!ReadScalar(data, size, offset, &table_hash) ||
                    !ConsumeBytes(sizeof(table_hash), size, &offset) ||
                    !ReadScalar(data, size, offset, &page_no) ||
                    !ConsumeBytes(sizeof(page_no), size, &offset) ||
                    !ReadScalar(data, size, offset, &rec_lsn) ||
                    !ConsumeBytes(sizeof(rec_lsn), size, &offset) || table_hash == 0 || page_no < 0 ||
                    rec_lsn < 0 || rec_lsn >= stored_lsn) {
                    return false;
                }
            }
            return offset == size;
        }
        default:
            return false;
    }
}

// 根据持久化类型构造并反序列化一条日志；未知类型返回 nullptr，由上层将其视为无效 WAL 尾部。
static std::unique_ptr<LogRecord> ParseLogRecord(const char* src) {
    LogType log_type;
    memcpy(&log_type, src + OFFSET_LOG_TYPE, sizeof(LogType));
    std::unique_ptr<LogRecord> rec;
    switch (log_type) {
        case LogType::kBegin:
            rec = std::make_unique<BeginLogRecord>();
            break;
        case LogType::kCommit:
            rec = std::make_unique<CommitLogRecord>();
            break;
        case LogType::kAbort:
            rec = std::make_unique<AbortLogRecord>();
            break;
        case LogType::kInsert:
            rec = std::make_unique<InsertLogRecord>();
            break;
        case LogType::kDelete:
            rec = std::make_unique<DeleteLogRecord>();
            break;
        case LogType::kUpdate:
            rec = std::make_unique<UpdateLogRecord>();
            break;
        case LogType::kCheckpoint:
            rec = std::make_unique<CheckpointLogRecord>();
            break;
        case LogType::kEnd:
            rec = std::make_unique<EndLogRecord>();
            break;
        case LogType::kClr:
            rec = std::make_unique<ClrLogRecord>();
            break;
        case LogType::kBeginCheckpoint:
            rec = std::make_unique<BeginCheckpointLogRecord>();
            break;
        case LogType::kEndCheckpoint:
            rec = std::make_unique<EndCheckpointLogRecord>();
            break;
        default:
            return nullptr;
    }
    rec->deserialize(src);
    return rec;
}

enum class StreamReadStatus { kRecord, kEnd, kInvalid };

struct LogRecordView {
    const char *data{nullptr};
    size_t size{0};
    LogType log_type{LogType::kBegin};
    lsn_t lsn{INVALID_LSN};
    txn_id_t tid{INVALID_TXN_ID};
    lsn_t prev_lsn{INVALID_LSN};
};

class LogRecordStream {
public:
    LogRecordStream(DiskManager *disk_manager, lsn_t start_lsn, lsn_t end_lsn)
        : disk_manager_(disk_manager), buffer_(kMaxLogRecordSize + kReadChunkSize),
          buffer_start_lsn_(start_lsn), end_lsn_(end_lsn) {}

    StreamReadStatus Next(std::unique_ptr<LogRecord> *record) {
        if (record == nullptr) {
            return StreamReadStatus::kInvalid;
        }
        record->reset();
        LogRecordView view;
        const StreamReadStatus status = NextView(&view);
        if (status != StreamReadStatus::kRecord) {
            return status;
        }
        *record = ParseLogRecord(view.data);
        return *record == nullptr ? StreamReadStatus::kInvalid : StreamReadStatus::kRecord;
    }

    StreamReadStatus NextView(LogRecordView *record) {
        if (record == nullptr || disk_manager_ == nullptr || buffer_start_lsn_ < 0 ||
            end_lsn_ < buffer_start_lsn_) {
            return StreamReadStatus::kInvalid;
        }
        *record = LogRecordView{};
        lsn_t record_lsn = next_lsn();
        if (record_lsn >= end_lsn_) {
            return StreamReadStatus::kEnd;
        }
        if (!EnsureAvailable(LOG_HEADER_SIZE)) {
            return StreamReadStatus::kInvalid;
        }

        record_lsn = next_lsn();
        rmdb::u32 total_len = 0;
        memcpy(&total_len, buffer_.data() + position_ + OFFSET_LOG_TOT_LEN, sizeof(rmdb::u32));
        if (total_len < static_cast<rmdb::u32>(LOG_HEADER_SIZE) || total_len > kMaxLogRecordSize ||
            static_cast<lsn_t>(total_len) > end_lsn_ - record_lsn || !EnsureAvailable(total_len)) {
            return StreamReadStatus::kInvalid;
        }

        record_lsn = next_lsn();
        const char *record_data = buffer_.data() + position_;
        if (!ValidateLogRecord(record_data, total_len, record_lsn)) {
            return StreamReadStatus::kInvalid;
        }
        record->data = record_data;
        record->size = total_len;
        memcpy(&record->log_type, record_data + OFFSET_LOG_TYPE, sizeof(record->log_type));
        memcpy(&record->lsn, record_data + OFFSET_LSN, sizeof(record->lsn));
        memcpy(&record->tid, record_data + OFFSET_LOG_TID, sizeof(record->tid));
        memcpy(&record->prev_lsn, record_data + OFFSET_PREV_LSN, sizeof(record->prev_lsn));
        position_ += total_len;
        return StreamReadStatus::kRecord;
    }

    lsn_t next_lsn() const { return buffer_start_lsn_ + static_cast<lsn_t>(position_); }

private:
    bool EnsureAvailable(size_t needed) {
        if (valid_bytes_ - position_ >= needed) {
            return true;
        }
        if (position_ > 0) {
            size_t remaining = valid_bytes_ - position_;
            memmove(buffer_.data(), buffer_.data() + position_, remaining);
            buffer_start_lsn_ += static_cast<lsn_t>(position_);
            valid_bytes_ = remaining;
            position_ = 0;
        }
        while (valid_bytes_ < needed) {
            lsn_t read_lsn = buffer_start_lsn_ + static_cast<lsn_t>(valid_bytes_);
            if (read_lsn >= end_lsn_) {
                return false;
            }
            size_t room = buffer_.size() - valid_bytes_;
            size_t file_remaining = static_cast<size_t>(end_lsn_ - read_lsn);
            size_t read_size = std::min(kReadChunkSize, std::min(room, file_remaining));
            if (read_size == 0) {
                return false;
            }
            size_t bytes_read = disk_manager_->read_log(buffer_.data() + valid_bytes_, read_size, read_lsn);
            if (bytes_read == 0) {
                return false;
            }
            valid_bytes_ += bytes_read;
        }
        return true;
    }

    static constexpr size_t kReadChunkSize = 1024 * 1024;
    DiskManager *disk_manager_;
    std::vector<char> buffer_;
    lsn_t buffer_start_lsn_;
    lsn_t end_lsn_;
    size_t position_{0};
    size_t valid_bytes_{0};
};

class RandomLogReader {
public:
    RandomLogReader(DiskManager *disk_manager, lsn_t first_lsn, lsn_t end_lsn)
        : disk_manager_(disk_manager), first_lsn_(first_lsn), end_lsn_(end_lsn),
          cache_(kReadBlockSize) {}

    std::unique_ptr<LogRecord> Read(lsn_t lsn) {
        if (disk_manager_ == nullptr || lsn == INVALID_LSN || lsn < first_lsn_ ||
            lsn < 0 || end_lsn_ < 0 || lsn > end_lsn_ || end_lsn_ - lsn < LOG_HEADER_SIZE) {
            return nullptr;
        }
        char header[LOG_HEADER_SIZE];
        if (!CopyBytes(lsn, header, sizeof(header))) {
            return nullptr;
        }
        rmdb::u32 total_len = 0;
        memcpy(&total_len, header + OFFSET_LOG_TOT_LEN, sizeof(total_len));
        if (total_len < static_cast<rmdb::u32>(LOG_HEADER_SIZE) || total_len > kMaxLogRecordSize ||
            static_cast<lsn_t>(total_len) > end_lsn_ - lsn) {
            return nullptr;
        }
        record_buffer_.resize(total_len);
        if (!CopyBytes(lsn, record_buffer_.data(), total_len) ||
            !ValidateLogRecord(record_buffer_.data(), record_buffer_.size(), lsn)) {
            return nullptr;
        }
        return ParseLogRecord(record_buffer_.data());
    }

private:
    bool CopyBytes(lsn_t lsn, char *dest, size_t size) {
        size_t copied = 0;
        while (copied < size) {
            const lsn_t current = lsn + static_cast<lsn_t>(copied);
            if (cache_start_lsn_ == INVALID_LSN || current < cache_start_lsn_ ||
                current >= cache_start_lsn_ + static_cast<lsn_t>(cache_valid_bytes_)) {
                if (!LoadBlock(current)) {
                    return false;
                }
            }
            const size_t offset = static_cast<size_t>(current - cache_start_lsn_);
            const size_t chunk = std::min(size - copied, cache_valid_bytes_ - offset);
            memcpy(dest + copied, cache_.data() + offset, chunk);
            copied += chunk;
        }
        return true;
    }

    bool LoadBlock(lsn_t lsn) {
        if (lsn < first_lsn_ || lsn >= end_lsn_) {
            return false;
        }
        const lsn_t aligned = (lsn / static_cast<lsn_t>(kReadBlockSize)) *
                              static_cast<lsn_t>(kReadBlockSize);
        cache_start_lsn_ = std::max(first_lsn_, aligned);
        cache_valid_bytes_ = static_cast<size_t>(std::min<lsn_t>(
            static_cast<lsn_t>(kReadBlockSize), end_lsn_ - cache_start_lsn_));
        return cache_valid_bytes_ > 0 &&
               disk_manager_->read_log(cache_.data(), cache_valid_bytes_, cache_start_lsn_) ==
                   cache_valid_bytes_;
    }

    static constexpr size_t kReadBlockSize = 1024 * 1024;
    DiskManager *disk_manager_;
    lsn_t first_lsn_;
    lsn_t end_lsn_;
    std::vector<char> cache_;
    std::vector<char> record_buffer_;
    lsn_t cache_start_lsn_{INVALID_LSN};
    size_t cache_valid_bytes_{0};
};

static lsn_t FindTransactionFirstLsn(RandomLogReader *reader, txn_id_t txn_id,
                                     lsn_t last_lsn) {
    lsn_t current_lsn = last_lsn;
    while (current_lsn != INVALID_LSN) {
        auto record = reader == nullptr ? nullptr : reader->Read(current_lsn);
        if (record == nullptr || record->log_tid_ != txn_id) {
            throw InternalError("checkpoint transaction WAL chain is incomplete");
        }
        if (record->log_type_ == LogType::kBegin) {
            return record->lsn_;
        }
        current_lsn = record->prev_lsn_;
    }
    throw InternalError("checkpoint transaction WAL chain has no BEGIN");
}

RmFileHandle *FindOpenTable(SmManager *sm_manager, const std::string &table_name) {
    auto iter = sm_manager->fhs_.find(table_name);
    return iter == sm_manager->fhs_.end() ? nullptr : iter->second.get();
}

std::string FindTableNameByHash(SmManager *sm_manager, rmdb::u64 table_hash) {
    for (const auto &entry : sm_manager->all_tables()) {
        if (HashTableName(entry.first) == table_hash) {
            return entry.first;
        }
    }
    return {};
}

constexpr const char *kRecoverBucketPrefix = "db.recover.bucket.";

void RemoveStaleRecoverBuckets() {
    DIR *directory = opendir(".");
    if (directory == nullptr) {
        throw UnixError();
    }
    try {
        while (true) {
            errno = 0;
            dirent *entry = readdir(directory);
            if (entry == nullptr) {
                if (errno != 0) {
                    throw UnixError();
                }
                break;
            }
            const std::string name = entry->d_name;
            if (name.rfind(kRecoverBucketPrefix, 0) != 0) {
                continue;
            }
            const std::string suffix = name.substr(strlen(kRecoverBucketPrefix));
            if (suffix.empty() ||
                !std::all_of(suffix.begin(), suffix.end(), [](unsigned char ch) {
                    return ch >= '0' && ch <= '9';
                })) {
                continue;
            }
            while (unlink(name.c_str()) < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == ENOENT) {
                    break;
                }
                throw UnixError();
            }
        }
    } catch (...) {
        closedir(directory);
        throw;
    }
    if (closedir(directory) < 0) {
        throw UnixError();
    }
}

}  // namespace

std::unique_ptr<LogRecord> ParseLogRecordForRecovery(const char *data, size_t size,
                                                     lsn_t expected_lsn) {
    if (!ValidateLogRecord(data, size, expected_lsn)) {
        return nullptr;
    }
    return ParseLogRecord(data);
}

bool ReconcileCheckpointTxnTail(const LogRecord &last_record,
                                RecoveryTxnStatus *status) {
    if (status == nullptr) {
        return false;
    }
    switch (last_record.log_type_) {
        case LogType::kBegin:
        case LogType::kInsert:
        case LogType::kDelete:
        case LogType::kUpdate:
            return true;
        case LogType::kCommit:
            *status = RecoveryTxnStatus::kCommitting;
            return true;
        case LogType::kAbort:
        case LogType::kClr:
            *status = RecoveryTxnStatus::kAborting;
            return true;
        default:
            return false;
    }
}

static void EnsureRidPageExists(RmFileHandle *fh, BufferPoolManager *buffer_pool_manager, const Rid &rid) {
    while (rid.page_no >= fh->get_file_hdr().num_pages) {
        auto page_handle = fh->create_new_page_handle();
        buffer_pool_manager->unpin_page(page_handle.page->get_page_id(), true);
    }
}

struct RecoveryDmlIdentity {
    rmdb::u64 table_hash{0};
    Rid rid{};
};

static bool ExtractRecoveryDmlIdentity(const LogRecordView &record,
                                       RecoveryDmlIdentity *identity) {
    if (record.data == nullptr || identity == nullptr ||
        record.size < static_cast<size_t>(LOG_HEADER_SIZE)) {
        return false;
    }
    if (record.log_type == LogType::kClr) {
        size_t offset = OFFSET_LOG_DATA;
        rmdb::u32 compensated_len = 0;
        if (!ConsumeBytes(sizeof(lsn_t), record.size, &offset) ||
            !ReadScalar(record.data, record.size, offset, &compensated_len) ||
            !ConsumeBytes(sizeof(compensated_len), record.size, &offset) ||
            compensated_len != record.size - offset) {
            return false;
        }
        LogRecordView compensated;
        compensated.data = record.data + offset;
        compensated.size = compensated_len;
        if (!ReadScalar(compensated.data, compensated.size, OFFSET_LOG_TYPE,
                        &compensated.log_type)) {
            return false;
        }
        return ExtractRecoveryDmlIdentity(compensated, identity);
    }

    size_t offset = OFFSET_LOG_DATA;
    if (record.log_type == LogType::kInsert || record.log_type == LogType::kDelete) {
        int record_size = 0;
        if (!ReadScalar(record.data, record.size, offset, &record_size) ||
            !ConsumeBytes(sizeof(record_size), record.size, &offset) || record_size <= 0 ||
            !ConsumeBytes(static_cast<size_t>(record_size), record.size, &offset)) {
            return false;
        }
    } else if (record.log_type == LogType::kUpdate) {
        rmdb::u16 delta_count = 0;
        if (!ReadScalar(record.data, record.size, offset, &delta_count) ||
            !ConsumeBytes(sizeof(delta_count), record.size, &offset)) {
            return false;
        }
        for (rmdb::u16 i = 0; i < delta_count; ++i) {
            rmdb::u16 col_index = 0;
            rmdb::u32 old_len = 0;
            rmdb::u32 new_len = 0;
            if (!ReadScalar(record.data, record.size, offset, &col_index) ||
                !ConsumeBytes(sizeof(col_index), record.size, &offset) ||
                !ReadScalar(record.data, record.size, offset, &old_len) ||
                !ConsumeBytes(sizeof(old_len), record.size, &offset) ||
                !ConsumeBytes(old_len, record.size, &offset) ||
                !ReadScalar(record.data, record.size, offset, &new_len) ||
                !ConsumeBytes(sizeof(new_len), record.size, &offset) ||
                !ConsumeBytes(new_len, record.size, &offset)) {
                return false;
            }
        }
    } else {
        return false;
    }

    return ReadScalar(record.data, record.size, offset, &identity->rid) &&
           ConsumeBytes(sizeof(identity->rid), record.size, &offset) &&
           ReadScalar(record.data, record.size, offset, &identity->table_hash) &&
           ConsumeBytes(sizeof(identity->table_hash), record.size, &offset) &&
           offset == record.size && identity->table_hash != 0;
}

static std::unique_ptr<LogRecord> ParseCompensatedLog(const ClrLogRecord *clr) {
    if (clr == nullptr || clr->compensated_log_.size() < static_cast<size_t>(LOG_HEADER_SIZE)) {
        return nullptr;
    }
    lsn_t compensated_lsn = INVALID_LSN;
    memcpy(&compensated_lsn, clr->compensated_log_.data() + OFFSET_LSN, sizeof(compensated_lsn));
    return ParseLogRecordForRecovery(clr->compensated_log_.data(), clr->compensated_log_.size(),
                                     compensated_lsn);
}

static bool ExtractRecoveryDmlIdentity(const LogRecord *record, RecoveryDmlIdentity *identity) {
    if (record == nullptr || identity == nullptr) {
        return false;
    }
    switch (record->log_type_) {
        case LogType::kInsert: {
            const auto *insert = dynamic_cast<const InsertLogRecord *>(record);
            identity->table_hash = insert->table_hash_;
            identity->rid = insert->rid_;
            return identity->table_hash != 0;
        }
        case LogType::kDelete: {
            const auto *deletion = dynamic_cast<const DeleteLogRecord *>(record);
            identity->table_hash = deletion->table_hash_;
            identity->rid = deletion->rid_;
            return identity->table_hash != 0;
        }
        case LogType::kUpdate: {
            const auto *update = dynamic_cast<const UpdateLogRecord *>(record);
            identity->table_hash = update->table_hash_;
            identity->rid = update->rid_;
            return identity->table_hash != 0;
        }
        case LogType::kClr: {
            auto compensated = ParseCompensatedLog(dynamic_cast<const ClrLogRecord *>(record));
            return compensated != nullptr && ExtractRecoveryDmlIdentity(compensated.get(), identity);
        }
        default:
            return false;
    }
}

static bool PageNeedsRedo(RmFileHandle *fh, BufferPoolManager *buffer_pool_manager,
                          const Rid &rid, lsn_t record_lsn) {
    if (rid.page_no >= fh->num_pages_snapshot()) {
        return true;
    }
    const PageId page_id{fh->GetFd(), rid.page_no};
    Page *page = buffer_pool_manager->fetch_page(page_id);
    if (page == nullptr) {
        throw InternalError("failed to fetch heap page while checking pageLSN");
    }
    const lsn_t page_lsn = page->get_page_lsn();
    if (!buffer_pool_manager->unpin_page_fast(page, page_id, false) &&
        !buffer_pool_manager->unpin_page(page_id, false)) {
        throw InternalError("failed to unpin heap page while checking pageLSN");
    }
    return page_lsn == INVALID_LSN || page_lsn < record_lsn;
}

static void FinalizeRecoveryPage(BufferPoolManager *buffer_pool_manager, Page *page,
                                 PageId page_id, lsn_t record_lsn) {
    if (page != nullptr && buffer_pool_manager->finalize_page_write_fast(page, page_id, record_lsn, true)) {
        return;
    }
    if (!buffer_pool_manager->finalize_page_write(page_id, record_lsn, true)) {
        throw InternalError("failed to finalize recovery heap page");
    }
}

static void StampRecoveryPage(BufferPoolManager *buffer_pool_manager, PageId page_id,
                              lsn_t record_lsn) {
    Page *page = buffer_pool_manager->fetch_page(page_id);
    if (page == nullptr) {
        throw InternalError("failed to fetch recovery heap page");
    }
    FinalizeRecoveryPage(buffer_pool_manager, page, page_id, record_lsn);
}

static void ApplyRecoveryDml(const LogRecord *record, bool inverse, lsn_t effect_lsn,
                             RmFileHandle *fh, const TabMeta &tab,
                             BufferPoolManager *buffer_pool_manager) {
    if (record == nullptr || fh == nullptr) {
        throw InternalError("invalid recovery DML target");
    }

    RecoveryDmlIdentity identity;
    if (!ExtractRecoveryDmlIdentity(record, &identity)) {
        throw InternalError("recovery DML has no page identity");
    }
    const Rid rid = identity.rid;
    const PageId page_id{fh->GetFd(), rid.page_no};

    auto write_record = [&](const char *data) {
        EnsureRidPageExists(fh, buffer_pool_manager, rid);
        PageId modified_page_id{};
        Page *modified_page = nullptr;
        if (fh->is_record(rid)) {
            fh->update_record(rid, const_cast<char *>(data), nullptr, &modified_page_id, true,
                              &modified_page);
        } else {
            fh->insert_record(rid, const_cast<char *>(data), &modified_page_id, true, &modified_page);
        }
        FinalizeRecoveryPage(buffer_pool_manager, modified_page, modified_page_id, effect_lsn);
    };

    auto delete_record = [&] {
        if (rid.page_no >= fh->num_pages_snapshot()) {
            throw InternalError("recovery DELETE references a missing heap page");
        }
        if (!fh->is_record(rid)) {
            StampRecoveryPage(buffer_pool_manager, page_id, effect_lsn);
            return;
        }
        PageId modified_page_id{};
        Page *modified_page = nullptr;
        fh->delete_record(rid, nullptr, &modified_page_id, true, &modified_page);
        FinalizeRecoveryPage(buffer_pool_manager, modified_page, modified_page_id, effect_lsn);
    };

    switch (record->log_type_) {
        case LogType::kInsert: {
            const auto *insert = dynamic_cast<const InsertLogRecord *>(record);
            if (inverse) {
                delete_record();
            } else {
                write_record(insert->insert_value_.data);
            }
            break;
        }
        case LogType::kDelete: {
            const auto *deletion = dynamic_cast<const DeleteLogRecord *>(record);
            if (inverse) {
                write_record(deletion->old_record_.data);
            } else {
                delete_record();
            }
            break;
        }
        case LogType::kUpdate: {
            const auto *update = dynamic_cast<const UpdateLogRecord *>(record);
            if (rid.page_no >= fh->num_pages_snapshot() || !fh->is_record(rid)) {
                throw InternalError("recovery UPDATE references a missing record");
            }
            RmRecord current;
            if (!fh->read_record(rid, &current, nullptr)) {
                throw InternalError("failed to read recovery UPDATE target");
            }
            RmRecord changed(current);
            for (const auto &delta : update->deltas_) {
                const std::string_view value = inverse ? delta.OldValue() : delta.NewValue();
                const bool full_record =
                    delta.col_index == kFullRecordUpdateDelta ||
                    (static_cast<size_t>(delta.col_index) == tab.cols.size() &&
                     value.size() == static_cast<size_t>(changed.size));
                if (full_record) {
                    if (value.size() != static_cast<size_t>(changed.size)) {
                        throw InternalError("invalid recovery full-record UPDATE delta");
                    }
                    memcpy(changed.data, value.data(), value.size());
                    continue;
                }
                if (delta.col_index >= tab.cols.size() ||
                    value.size() != static_cast<size_t>(tab.cols[delta.col_index].len) ||
                    tab.cols[delta.col_index].offset + value.size() > static_cast<size_t>(changed.size)) {
                    throw InternalError("invalid recovery UPDATE delta");
                }
                memcpy(changed.data + tab.cols[delta.col_index].offset, value.data(), value.size());
            }
            PageId modified_page_id{};
            Page *modified_page = nullptr;
            fh->update_record(rid, changed.data, nullptr, &modified_page_id, true, &modified_page);
            FinalizeRecoveryPage(buffer_pool_manager, modified_page, modified_page_id, effect_lsn);
            break;
        }
        default:
            throw InternalError("unsupported recovery DML type");
    }
}

static void MaybeCrashAfterRecoveryClr(LogManager *log_manager) {
    static const rmdb::u64 crash_after = [] {
        const char *value = std::getenv("RMDB_TEST_CRASH_AFTER_RECOVERY_CLRS");
        if (value == nullptr || *value == '\0') {
            return rmdb::u64{0};
        }
        char *end = nullptr;
        unsigned long long parsed = std::strtoull(value, &end, 10);
        return end != value && *end == '\0' ? static_cast<rmdb::u64>(parsed) : rmdb::u64{0};
    }();
    if (crash_after == 0) {
        return;
    }
    static rmdb::u64 clr_count = 0;
    if (++clr_count == crash_after) {
        // Stabilize exactly this CLR prefix, then emulate a power loss before END.
        log_manager->flush_log_to_disk();
        // Test-only abrupt termination: no destructors, flushes, or clean marker.
        _exit(86);
    }
}

/**
 * @description: ARIES analysis rebuilds the transaction and dirty-page tables.
 * REDO performs a second sequential WAL pass, so analysis never materializes DML
 * records or writes recovery-only temporary files.
 */
void RecoveryManager::analyze() {
    txn_table_.clear();
    dirty_page_table_.clear();
    logical_delete_redo_table_.clear();
    scan_start_lsn_ = 0;
    scan_end_lsn_ = 0;
    next_txn_id_ = 0;
    RemoveStaleRecoverBuckets();

    const lsn_t file_size = disk_manager_->get_log_end_lsn();
    if (file_size <= 0) return;
    const lsn_t first_available_lsn = disk_manager_->get_first_log_lsn();
    RandomLogReader random_reader(disk_manager_, first_available_lsn, file_size);

    lsn_t checkpoint_begin_lsn = INVALID_LSN;
    lsn_t checkpoint_scan_lsn = INVALID_LSN;
    std::unordered_set<txn_id_t> checkpoint_txns;
    CheckpointMasterRecord master;
    const bool master_decoded = rmdb::checkpoint_master::Read(&master);
    const bool master_range_valid =
        master_decoded && master.end_checkpoint_lsn < file_size &&
        master.first_retained_lsn % DiskManager::WAL_SEGMENT_SIZE == 0 &&
        first_available_lsn <= master.first_retained_lsn;
    if (master_range_valid) {
        auto end_record = random_reader.Read(master.end_checkpoint_lsn);
        auto *checkpoint = end_record == nullptr
                               ? nullptr
                               : dynamic_cast<EndCheckpointLogRecord *>(end_record.get());
        if (checkpoint != nullptr && checkpoint->begin_lsn_ < checkpoint->lsn_ &&
            master.first_retained_lsn <= checkpoint->begin_lsn_) {
            auto begin_record = random_reader.Read(checkpoint->begin_lsn_);
            if (begin_record != nullptr && begin_record->log_type_ == LogType::kBeginCheckpoint) {
                bool snapshot_valid = true;
                std::unordered_map<txn_id_t, RecoveryTxnEntry> checkpoint_txn_table;
                std::unordered_map<RecoveryPageKey, lsn_t, RecoveryPageKeyHash> checkpoint_dpt;
                txn_id_t checkpoint_next_txn_id = checkpoint->next_txn_id_;
                for (const auto &entry : checkpoint->txn_table_) {
                    auto tail = random_reader.Read(entry.last_lsn_);
                    RecoveryTxnStatus status = entry.status_;
                    if (tail == nullptr || tail->log_tid_ != entry.txn_id_ ||
                        !ReconcileCheckpointTxnTail(*tail, &status)) {
                        snapshot_valid = false;
                        break;
                    }
                    auto [iter, inserted] = checkpoint_txn_table.emplace(
                        entry.txn_id_, RecoveryTxnEntry{status, entry.last_lsn_});
                    if (!inserted) {
                        snapshot_valid = false;
                        break;
                    }
                    if (entry.txn_id_ >= checkpoint_next_txn_id) {
                        checkpoint_next_txn_id = entry.txn_id_ + 1;
                    }
                }
                if (snapshot_valid) {
                    for (const auto &page : checkpoint->dirty_pages_) {
                        if (page.rec_lsn_ < master.first_retained_lsn) {
                            snapshot_valid = false;
                            break;
                        }
                        RecoveryPageKey key{page.table_hash_, page.page_no_};
                        auto [iter, inserted] = checkpoint_dpt.emplace(key, page.rec_lsn_);
                        if (!inserted && page.rec_lsn_ < iter->second) {
                            iter->second = page.rec_lsn_;
                        }
                    }
                }
                if (snapshot_valid) {
                    checkpoint_begin_lsn = checkpoint->begin_lsn_;
                    // Segment reclamation uses an aligned byte boundary, but WAL
                    // records may cross that boundary. Derive a real record LSN
                    // from the checkpoint contents instead of parsing a segment
                    // continuation as a log header.
                    checkpoint_scan_lsn = checkpoint_begin_lsn;
                    for (const auto &[txn_id, entry] : checkpoint_txn_table) {
                        checkpoint_scan_lsn = std::min(
                            checkpoint_scan_lsn,
                            FindTransactionFirstLsn(&random_reader, txn_id, entry.last_lsn));
                    }
                    next_txn_id_ = checkpoint_next_txn_id;
                    txn_table_ = std::move(checkpoint_txn_table);
                    dirty_page_table_ = std::move(checkpoint_dpt);
                    checkpoint_txns.reserve(txn_table_.size());
                    for (const auto &[txn_id, entry] : txn_table_) {
                        (void)entry;
                        checkpoint_txns.insert(txn_id);
                    }
                }
            }
        }
    }

    if (first_available_lsn > 0 && checkpoint_begin_lsn == INVALID_LSN) {
        throw InternalError("reclaimed WAL requires a valid fuzzy checkpoint master");
    }

    // Start at the earliest record needed by the ATT/DPT. The master segment
    // boundary is only a physical retention limit and may bisect a WAL record.
    scan_start_lsn_ = checkpoint_begin_lsn == INVALID_LSN ? 0 : checkpoint_scan_lsn;
    for (const auto &[page, rec_lsn] : dirty_page_table_) {
        (void)page;
        scan_start_lsn_ = std::min(scan_start_lsn_, rec_lsn);
    }
    scan_end_lsn_ = scan_start_lsn_;
    LogRecordStream stream(disk_manager_, scan_start_lsn_, file_size);
    while (true) {
        LogRecordView rec;
        StreamReadStatus status = stream.NextView(&rec);
        if (status == StreamReadStatus::kEnd) {
            break;
        }
        if (status == StreamReadStatus::kInvalid) {
            // 崩溃可能只写入半条尾日志；最后一条完整日志之后的字节不可参与 redo/undo。
            std::cerr << "Ignoring invalid or incomplete WAL at LSN " << stream.next_lsn() << "\n";
            break;
        }
        scan_end_lsn_ = stream.next_lsn();
        txn_id_t tid = rec.tid;
        if (tid != INVALID_TXN_ID && tid >= next_txn_id_) {
            next_txn_id_ = tid + 1;
        }
        switch (rec.log_type) {
            case LogType::kBegin: {
                txn_table_.try_emplace(tid, RecoveryTxnEntry{RecoveryTxnStatus::kRunning, rec.lsn});
                break;
            }
            case LogType::kCommit: {
                // A durable COMMIT is a winner and never participates in UNDO.
                // REDO is driven by the DPT rather than the ATT, so retaining
                // every winner only grows the hash table and makes recovery
                // append a redundant END record for every committed runtime
                // transaction.
                txn_table_.erase(tid);
                break;
            }
            case LogType::kAbort: {
                auto &entry = txn_table_[tid];
                entry.status = RecoveryTxnStatus::kAborting;
                entry.last_lsn = std::max(entry.last_lsn, rec.lsn);
                break;
            }
            case LogType::kEnd: {
                auto entry = txn_table_.find(tid);
                if (entry != txn_table_.end() && rec.lsn >= entry->second.last_lsn) {
                    txn_table_.erase(entry);
                }
                break;
            }
            case LogType::kInsert:
            case LogType::kDelete:
            case LogType::kUpdate:
            case LogType::kClr:
                if (tid != INVALID_TXN_ID) {
                    auto &entry = txn_table_[tid];
                    entry.last_lsn = std::max(entry.last_lsn, rec.lsn);
                }
                break;
            default:
                break;
        }

        RecoveryDmlIdentity identity;
        if (ExtractRecoveryDmlIdentity(rec, &identity)) {
            RecoveryPageKey page_key{identity.table_hash, identity.rid.page_no};
            auto dpt = dirty_page_table_.find(page_key);
            const bool before_checkpoint =
                checkpoint_begin_lsn != INVALID_LSN && rec.lsn < checkpoint_begin_lsn;
            if (before_checkpoint) {
                if (dpt == dirty_page_table_.end()) {
                    // DML belonging to a transaction captured in the ATT may be
                    // in flight between WAL append and page finalization. Treat
                    // its first retained page record as a conservative recLSN.
                    if (checkpoint_txns.find(tid) == checkpoint_txns.end()) {
                        continue;
                    }
                    dpt = dirty_page_table_.emplace(page_key, rec.lsn).first;
                } else if (rec.lsn < dpt->second) {
                    continue;
                }
            } else if (dpt == dirty_page_table_.end()) {
                dpt = dirty_page_table_.emplace(page_key, rec.lsn).first;
            }
            if (rec.log_type == LogType::kDelete) {
                auto [iter, inserted] = logical_delete_redo_table_.emplace(page_key, rec.lsn);
                if (!inserted && rec.lsn < iter->second) {
                    iter->second = rec.lsn;
                }
            }
        }
    }
}

/**
 * @description: ARIES repeat-history REDO. Winners, losers and CLRs are all replayed;
 * the DPT recLSN and heap pageLSN decide whether an individual record is needed.
 */
void RecoveryManager::redo() {
    if (scan_end_lsn_ <= scan_start_lsn_ || dirty_page_table_.empty()) {
        return;
    }

    struct RedoTableTarget {
        RmFileHandle *fh{nullptr};
        const TabMeta *tab{nullptr};
    };
    std::unordered_map<rmdb::u64, RedoTableTarget> targets;
    targets.reserve(sm_manager_->fhs_.size());
    for (const auto &entry : sm_manager_->fhs_) {
        const auto &tab = sm_manager_->db_.get_table(entry.first);
        targets.emplace(HashTableName(entry.first),
                        RedoTableTarget{entry.second.get(), &tab});
    }
    std::unordered_set<rmdb::u64> warned_missing_tables;

    LogRecordStream stream(disk_manager_, scan_start_lsn_, scan_end_lsn_);
    while (true) {
        std::unique_ptr<LogRecord> rec;
        const StreamReadStatus status = stream.Next(&rec);
        if (status == StreamReadStatus::kEnd) {
            break;
        }
        if (status == StreamReadStatus::kInvalid) {
            throw InternalError("WAL changed after successful recovery analysis at LSN " +
                                std::to_string(stream.next_lsn()));
        }

        RecoveryDmlIdentity identity;
        if (!ExtractRecoveryDmlIdentity(rec.get(), &identity)) {
            continue;
        }
        const RecoveryPageKey page_key{identity.table_hash, identity.rid.page_no};
        auto dpt = dirty_page_table_.find(page_key);
        if (dpt == dirty_page_table_.end() || rec->lsn_ < dpt->second) {
            continue;
        }
        auto target = targets.find(identity.table_hash);
        if (target == targets.end()) {
            if (warned_missing_tables.insert(identity.table_hash).second) {
                std::cerr << "Recovery redo: skipping missing table hash "
                          << identity.table_hash << "\n";
            }
            continue;
        }
        auto logical_redo = logical_delete_redo_table_.find(page_key);
        const bool repeat_logical_delete_history =
            logical_redo != logical_delete_redo_table_.end() && rec->lsn_ >= logical_redo->second;
        if (!repeat_logical_delete_history &&
            !PageNeedsRedo(target->second.fh, buffer_pool_manager_, identity.rid, rec->lsn_)) {
            continue;
        }
        if (rec->log_type_ == LogType::kClr) {
            auto compensated = ParseCompensatedLog(dynamic_cast<ClrLogRecord *>(rec.get()));
            if (compensated == nullptr) {
                throw InternalError("invalid compensated record in CLR");
            }
            ApplyRecoveryDml(compensated.get(), true, rec->lsn_, target->second.fh,
                             *target->second.tab, buffer_pool_manager_);
        } else {
            ApplyRecoveryDml(rec.get(), false, rec->lsn_, target->second.fh,
                             *target->second.tab, buffer_pool_manager_);
        }
    }
}

void RecoveryManager::undo() {
    const lsn_t file_size = disk_manager_->get_log_end_lsn();
    RandomLogReader random_reader(disk_manager_, disk_manager_->get_first_log_lsn(), file_size);
    struct UndoTableCache {
        std::string tab_name;
        RmFileHandle *fh = nullptr;
        const TabMeta *tab = nullptr;
    };
    std::unordered_map<rmdb::u64, UndoTableCache> undo_table_cache;
    auto resolve_table = [&](rmdb::u64 table_hash) -> UndoTableCache * {
        auto it = undo_table_cache.find(table_hash);
        if (it != undo_table_cache.end()) {
            return &it->second;
        }
        std::string tab_name = FindTableNameByHash(sm_manager_, table_hash);
        RmFileHandle *fh = FindOpenTable(sm_manager_, tab_name);
        if (fh == nullptr || tab_name.empty()) {
            return nullptr;
        }
        UndoTableCache cache;
        cache.tab_name = std::move(tab_name);
        cache.fh = fh;
        cache.tab = &sm_manager_->db_.get_table(cache.tab_name);
        auto emplaced = undo_table_cache.emplace(table_hash, std::move(cache));
        return &emplaced.first->second;
    };
    using UndoWork = std::pair<lsn_t, txn_id_t>;
    std::priority_queue<UndoWork> work;
    std::vector<txn_id_t> committed_without_end;
    for (auto &[tid, entry] : txn_table_) {
        if (entry.status == RecoveryTxnStatus::kCommitting) {
            committed_without_end.push_back(tid);
            continue;
        }
        if (entry.status == RecoveryTxnStatus::kRunning) {
            const lsn_t undo_start = entry.last_lsn;
            AbortLogRecord abort_record(tid);
            abort_record.prev_lsn_ = undo_start;
            const lsn_t abort_lsn = log_manager_->add_log_to_buffer(&abort_record);
            entry.status = RecoveryTxnStatus::kAborting;
            entry.last_lsn = abort_lsn;
            if (undo_start != INVALID_LSN) {
                work.emplace(undo_start, tid);
            }
            continue;
        }
        if (entry.status == RecoveryTxnStatus::kAborting && entry.last_lsn != INVALID_LSN) {
            work.emplace(entry.last_lsn, tid);
        }
    }

    for (txn_id_t tid : committed_without_end) {
        auto iter = txn_table_.find(tid);
        if (iter == txn_table_.end()) continue;
        EndLogRecord end_record(tid);
        end_record.prev_lsn_ = iter->second.last_lsn;
        log_manager_->add_log_to_buffer(&end_record);
        txn_table_.erase(iter);
    }

    while (!work.empty()) {
        auto [current_lsn, tid] = work.top();
        work.pop();
        auto txn_iter = txn_table_.find(tid);
        if (txn_iter == txn_table_.end()) {
            continue;
        }
        auto rec = random_reader.Read(current_lsn);
        if (rec == nullptr || rec->log_tid_ != tid) {
            throw InternalError("recovery undo chain is invalid at LSN " +
                                std::to_string(current_lsn) + " for transaction " +
                                std::to_string(tid));
        }

        lsn_t next_lsn = rec->prev_lsn_;
        if (rec->log_type_ == LogType::kClr) {
            next_lsn = dynamic_cast<ClrLogRecord *>(rec.get())->undo_next_lsn_;
        } else if (rec->log_type_ == LogType::kInsert || rec->log_type_ == LogType::kDelete ||
                   rec->log_type_ == LogType::kUpdate) {
            RecoveryDmlIdentity identity;
            if (!ExtractRecoveryDmlIdentity(rec.get(), &identity)) {
                throw InternalError("undo record has no page identity");
            }
            UndoTableCache *table = resolve_table(identity.table_hash);
            if (table == nullptr) {
                throw InternalError("undo record references a missing table");
            }
            ClrLogRecord clr(tid, rec->prev_lsn_, *rec);
            clr.prev_lsn_ = txn_iter->second.last_lsn;
            const lsn_t clr_lsn = log_manager_->add_log_to_buffer(&clr);
            txn_iter->second.last_lsn = clr_lsn;
            ApplyRecoveryDml(rec.get(), true, clr_lsn, table->fh, *table->tab,
                             buffer_pool_manager_);
            MaybeCrashAfterRecoveryClr(log_manager_);
        }

        if (rec->log_type_ == LogType::kBegin || next_lsn == INVALID_LSN) {
            EndLogRecord end_record(tid);
            end_record.prev_lsn_ = txn_iter->second.last_lsn;
            log_manager_->add_log_to_buffer(&end_record);
            txn_table_.erase(txn_iter);
        } else {
            work.emplace(next_lsn, tid);
        }
    }

    txn_table_.clear();
    dirty_page_table_.clear();
    logical_delete_redo_table_.clear();
    scan_start_lsn_ = 0;
    scan_end_lsn_ = 0;
}
