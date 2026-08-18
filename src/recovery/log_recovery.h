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

#include <cstddef>
#include <map>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include "log_manager.h"
#include "storage/disk_manager.h"
#include "system/sm_manager.h"

std::unique_ptr<LogRecord> ParseLogRecordForRecovery(const char *data, size_t size,
                                                     lsn_t expected_lsn);

// Fuzzy checkpoint snapshots can observe a transaction after its terminal WAL
// record is appended but before its in-memory state becomes terminal. Reconcile
// the snapshot with the durable record named by lastLSN before seeding the ATT.
bool ReconcileCheckpointTxnTail(const LogRecord &last_record,
                                RecoveryTxnStatus *status);

class RedoLogsInPage {
public:
    RedoLogsInPage() { table_file_ = nullptr; }
    RmFileHandle* table_file_;
    std::vector<lsn_t> redo_logs_;   // 在该page上需要redo的操作的lsn
};

struct RecoveryPageKey {
    rmdb::u64 table_hash{0};
    page_id_t page_no{INVALID_PAGE_ID};

    friend bool operator==(const RecoveryPageKey &lhs, const RecoveryPageKey &rhs) {
        return lhs.table_hash == rhs.table_hash && lhs.page_no == rhs.page_no;
    }
};

struct RecoveryPageKeyHash {
    size_t operator()(const RecoveryPageKey &key) const noexcept {
        size_t seed = std::hash<rmdb::u64>{}(key.table_hash);
        return seed ^ (std::hash<page_id_t>{}(key.page_no) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U));
    }
};

struct RecoveryTxnEntry {
    RecoveryTxnStatus status{RecoveryTxnStatus::kRunning};
    lsn_t last_lsn{INVALID_LSN};
};

class RecoveryManager {
public:
    RecoveryManager(DiskManager* disk_manager, BufferPoolManager* buffer_pool_manager, SmManager* sm_manager,
                    LogManager *log_manager) {
        disk_manager_ = disk_manager;
        buffer_pool_manager_ = buffer_pool_manager;
        sm_manager_ = sm_manager;
        log_manager_ = log_manager;
    }

    void analyze();
    void redo();
    void undo();
    [[nodiscard]] txn_id_t next_txn_id() const { return next_txn_id_; }
    [[nodiscard]] lsn_t scan_end_lsn() const { return scan_end_lsn_; }
private:
    DiskManager* disk_manager_;                                     // 用来读写文件
    BufferPoolManager* buffer_pool_manager_;                        // 对页面进行读写
    SmManager* sm_manager_;                                         // 访问数据库元数据
    LogManager *log_manager_;                                       // 恢复期间写 CLR/END
    std::unordered_map<txn_id_t, RecoveryTxnEntry> txn_table_;      // ARIES ATT
    std::unordered_map<RecoveryPageKey, lsn_t, RecoveryPageKeyHash> dirty_page_table_;  // ARIES DPT
    // Logical DELETE does not touch the heap until MVCC GC. From the earliest such
    // record onward, repeat the page's history without trusting its physical pageLSN.
    std::unordered_map<RecoveryPageKey, lsn_t, RecoveryPageKeyHash> logical_delete_redo_table_;
    lsn_t scan_start_lsn_{0};                                       // REDO 顺序扫描起点
    lsn_t scan_end_lsn_{0};                                         // 最后一条完整有效日志之后的位置
    txn_id_t next_txn_id_{0};                                       // WAL 中最大事务号的后继
};
