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

#include <deque>
#include <memory>
#include <unordered_map>
#include <vector>

#include "transaction/transaction.h"
#include "transaction/concurrency/lock_manager.h"
#include "recovery/log_manager.h"
#include "common/result_writer.h"

class TransactionManager;
class IxIndexHandle;

// used for data_send
static int const_offset = -1;

class StatementScratch {
public:
    void reset() {
        record_cursor_ = 0;
        string_cursor_ = 0;
        rid_cursor_ = 0;
    }

    std::shared_ptr<RmRecord> acquire_record(int len) {
        if (record_cursor_ == records_.size()) {
            records_.push_back(std::make_shared<RmRecord>(len));
        }
        auto record = records_[record_cursor_++];
        record->Resize(len);
        return record;
    }

    std::string *acquire_string(size_t len) {
        if (string_cursor_ == strings_.size()) {
            strings_.emplace_back();
        }
        auto *value = &strings_[string_cursor_++];
        value->resize(len);
        return value;
    }

    std::shared_ptr<std::vector<Rid>> acquire_rids() {
        if (rid_cursor_ == rid_vectors_.size()) {
            rid_vectors_.push_back(std::make_shared<std::vector<Rid>>());
        }
        auto value = rid_vectors_[rid_cursor_++];
        value->clear();
        return value;
    }

private:
    std::vector<std::shared_ptr<RmRecord>> records_;
    size_t record_cursor_ = 0;
    std::deque<std::string> strings_;
    size_t string_cursor_ = 0;
    std::vector<std::shared_ptr<std::vector<Rid>>> rid_vectors_;
    size_t rid_cursor_ = 0;
};

class Context {
public:
    Context (LockManager *lock_mgr, LogManager *log_mgr,
            TransactionManager *txn_mgr, Transaction *txn, char *data_send = nullptr, int *offset = &const_offset,
            IsolationLevel *session_isolation = nullptr, StatementScratch *scratch = nullptr,
            QueryResultWriter *result_writer = nullptr,
            std::unordered_map<const IxIndexHandle *, int> *index_hints = nullptr,
            std::vector<std::shared_ptr<TransactionResponseGate>> *response_gates = nullptr)
        : lock_mgr_(lock_mgr), log_mgr_(log_mgr), txn_mgr_(txn_mgr), txn_(txn),
          data_send_(data_send), offset_(offset), session_isolation_(session_isolation), scratch_(scratch),
          result_writer_(result_writer), index_hints_(index_hints), response_gates_(response_gates) {
            ellipsis_ = false;
            // 语句入口时事务已持有的锁数:用于"持锁行例外"的写冲突放行
            // (仅放行本语句之前已获取的锁,语句内新锁如 CAS 保持原语义)。
            locks_at_entry_ = txn_ == nullptr ? 0 : txn_->get_lock_set().size();
          }

    // A transaction admitted on a logical write key must not let a later
    // same-key response overtake its own response.  Network request handlers
    // defer gate completion until after SendWireFrame; non-wire callers leave
    // response_gates_ null and the transaction manager completes it itself.
    void DeferResponseGate(Transaction *txn) {
        if (txn == nullptr || response_gates_ == nullptr || txn->response_gate_deferred()) {
            return;
        }
        auto gate = txn->get_response_gate();
        if (gate == nullptr) {
            return;
        }
        txn->set_response_gate_deferred(true);
        response_gates_->push_back(std::move(gate));
    }

    size_t locks_at_entry() const { return locks_at_entry_; }

    std::shared_ptr<RmRecord> acquire_template_raw_record(int len) {
        if (scratch_ == nullptr) {
            return std::make_shared<RmRecord>(len);
        }
        return scratch_->acquire_record(len);
    }

    std::string *acquire_statement_string(size_t len) {
        if (scratch_ == nullptr) {
            return nullptr;
        }
        return scratch_->acquire_string(len);
    }

    std::shared_ptr<std::vector<Rid>> acquire_statement_rids() {
        return scratch_ == nullptr ? std::make_shared<std::vector<Rid>>() : scratch_->acquire_rids();
    }

    LockManager *lock_mgr_;
    LogManager *log_mgr_;
    TransactionManager *txn_mgr_;
    Transaction *txn_;
    char *data_send_;
    int *offset_;
    IsolationLevel *session_isolation_;
    StatementScratch *scratch_;
    QueryResultWriter *result_writer_;
    // Batch-level index leaf hint: index handle -> leaf page used by the last
    // point lookup. Hints are validated on every use and discarded when stale.
    std::unordered_map<const IxIndexHandle *, int> *index_hints_;
    std::vector<std::shared_ptr<TransactionResponseGate>> *response_gates_;
    bool ellipsis_;
    size_t locks_at_entry_{0};
    // Number of rows written by this statement. Delete/update executors increment
    // it after each successful row change. Callers can use it to verify statement
    // atomicity before committing.
    uint64_t affected_rows = 0;
    // 批处理在 BEGIN 之前根据通用 SQL 结构提取的唯一点写键。事务管理器
    // 必须先取得这些准入令牌、再固定 SI read_ts；令牌本身不替代正常的
    // 记录锁与写写冲突校验。
    std::vector<lock_data_key_t> pre_snapshot_write_keys_;
};
