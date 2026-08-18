/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "transaction_manager.h"

#include <algorithm>
#include <cstring>
#include <memory>

namespace {

bool same_rid(const Rid &lhs, const Rid &rhs) {
    return lhs.page_no == rhs.page_no && lhs.slot_no == rhs.slot_no;
}

bool has_read_record(Transaction *txn, const std::string &tab_name, const Rid &rid) {
    const auto &read_records = txn->get_read_records();
    auto iter = read_records.find(tab_name);
    if (iter == read_records.end()) {
        return false;
    }
    for (const auto &read_rid : iter->second) {
        if (same_rid(read_rid, rid)) {
            return true;
        }
    }
    return false;
}

bool has_written_record(Transaction *txn, const std::string &tab_name, const Rid &rid) {
    if (txn == nullptr) {
        return false;
    }
    for (const auto &entry : txn->get_serializable_writes()) {
        const auto &write_info = entry.second;
        if (write_info.tab_name == tab_name && same_rid(write_info.rid, rid)) {
            return true;
        }
    }
    return false;
}

bool is_overlapping_serializable(Transaction *other, Transaction *txn) {
    if (other == nullptr || other == txn || other->get_isolation_level() != IsolationLevel::SERIALIZABLE) {
        return false;
    }
    if (other->get_state() == TransactionState::GROWING) {
        return true;
    }
    return other->get_state() == TransactionState::COMMITTED &&
           other->get_commit_ts() != INVALID_TS &&
           other->get_commit_ts() > txn->get_start_ts();
}

bool writer_invisible_to_reader(Transaction *reader, Transaction *writer) {
    if (reader == nullptr || writer == nullptr || reader == writer) {
        return false;
    }
    if (!is_overlapping_serializable(writer, reader)) {
        return false;
    }
    if (writer->get_state() == TransactionState::GROWING) {
        return true;
    }
    return writer->get_state() == TransactionState::COMMITTED &&
           writer->get_commit_ts() != INVALID_TS &&
           writer->get_commit_ts() > reader->get_read_ts();
}

bool record_matches(const std::vector<ColMeta> &cols, const RmRecord &record, const std::vector<Condition> &conds) {
    auto find_col = [&](const TabCol &target) {
        return std::find_if(cols.begin(), cols.end(), [&](const ColMeta &col) {
            return col.tab_name == target.tab_name && col.name == target.col_name;
        });
    };
    auto compare = [](const char *lhs, const char *rhs, ColType type, int len) {
        if (type == TYPE_INT) {
            int a = rmdb::load_unaligned<int>(lhs);
            int b = rmdb::load_unaligned<int>(rhs);
            return (a > b) - (a < b);
        }
        if (type == TYPE_FLOAT) {
            float a = rmdb::load_unaligned<float>(lhs);
            float b = rmdb::load_unaligned<float>(rhs);
            return (a > b) - (a < b);
        }
        return memcmp(lhs, rhs, len);
    };
    auto pass = [](int cmp, CompOp op) {
        switch (op) {
            case OP_EQ: return cmp == 0;
            case OP_NE: return cmp != 0;
            case OP_LT: return cmp < 0;
            case OP_GT: return cmp > 0;
            case OP_LE: return cmp <= 0;
            case OP_GE: return cmp >= 0;
        }
        return false;
    };
    for (const auto &cond : conds) {
        auto lhs_col = find_col(cond.lhs_col);
        if (lhs_col == cols.end()) {
            return false;
        }
        const char *rhs = cond.is_rhs_val ? cond.rhs_val.raw->data : nullptr;
        if (!cond.is_rhs_val) {
            auto rhs_col = find_col(cond.rhs_col);
            if (rhs_col == cols.end()) {
                return false;
            }
            rhs = record.data + rhs_col->offset;
        }
        if (!pass(compare(record.data + lhs_col->offset, rhs, lhs_col->type, lhs_col->len), cond.op)) {
            return false;
        }
    }
    return true;
}

bool commits_before(Transaction *tout, Transaction *tin) {
    return tout != nullptr && tout->get_state() == TransactionState::COMMITTED &&
           tout->get_commit_ts() != INVALID_TS &&
           (tin == nullptr || tin->get_state() != TransactionState::COMMITTED ||
            tout->get_commit_ts() < tin->get_commit_ts());
}

bool has_dangerous_structure_after_new_edge(const TransactionRegistry::AllView &transactions,
                                            Transaction *from, Transaction *to) {
    if (from == nullptr || to == nullptr) {
        return false;
    }

    for (auto next_id : to->get_rw_dependencies()) {
        auto *tout = transactions.Find(next_id);
        if (tout == nullptr || tout->get_state() == TransactionState::ABORTED) {
            continue;
        }
        if (tout == from || commits_before(tout, from)) {
            return true;
        }
    }

    bool dangerous = false;
    transactions.ForEach([&](txn_id_t, Transaction *tin) {
        if (tin == nullptr || tin == from || tin->get_state() == TransactionState::ABORTED) {
            return;
        }
        if (tin->get_rw_dependencies().count(from->get_transaction_id()) == 0) {
            return;
        }
        if (to == tin || commits_before(to, tin)) {
            dangerous = true;
        }
    });
    return dangerous;
}

void add_dependency(const TransactionRegistry::AllView &transactions, Transaction *from, Transaction *to,
                    Transaction *abort_candidate) {
    if (from == nullptr || to == nullptr || from == to) {
        return;
    }
    if (from->get_rw_dependencies().count(to->get_transaction_id()) != 0) {
        return;
    }
    from->add_rw_dependency(to->get_transaction_id());
    if (has_dangerous_structure_after_new_edge(transactions, from, to)) {
        throw TransactionAbortException(abort_candidate->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION,
                                        AbortSubReason::SSI_DANGEROUS_STRUCTURE);
    }
}

}  // namespace

void TransactionManager::RecordSerializableRead(Transaction *txn, const std::string &tab_name, const Rid &rid) {
    if (txn == nullptr || txn->get_isolation_level() != IsolationLevel::SERIALIZABLE) {
        return;
    }
    transaction_registry_.WithAllExclusive([&](TransactionRegistry::AllView transactions) {
        txn->add_read_record(tab_name, rid);
        transactions.ForEach([&](txn_id_t, Transaction *other) {
            if (!writer_invisible_to_reader(txn, other)) {
                return;
            }
            if (has_written_record(other, tab_name, rid)) {
                add_dependency(transactions, txn, other, txn);
            }
        });
    });
}

void TransactionManager::RecordSerializablePredicateRead(Transaction *txn, const std::string &tab_name,
                                                          const std::vector<ColMeta> &cols,
                                                          const std::vector<Condition> &conds) {
    if (txn == nullptr || txn->get_isolation_level() != IsolationLevel::SERIALIZABLE) {
        return;
    }
    transaction_registry_.WithAllExclusive([&](TransactionRegistry::AllView transactions) {
        txn->add_predicate_read(tab_name, cols, conds);
        transactions.ForEach([&](txn_id_t, Transaction *other) {
            if (!writer_invisible_to_reader(txn, other)) {
                return;
            }
            for (const auto &write_entry : other->get_serializable_writes()) {
                const auto &write_info = write_entry.second;
                if (write_info.tab_name != tab_name) {
                    continue;
                }
                bool matches_old = write_info.old_record != nullptr &&
                                   record_matches(cols, *write_info.old_record, conds);
                bool matches_new = write_info.new_record != nullptr &&
                                   record_matches(cols, *write_info.new_record, conds);
                if (matches_old || matches_new) {
                    add_dependency(transactions, txn, other, txn);
                    return;
                }
            }
        });
    });
}

void TransactionManager::RecordSerializableWrite(Transaction *txn, const std::string &tab_name, const Rid &rid,
                                                 const RmRecord *old_record, const RmRecord *new_record,
                                                 const std::vector<ColMeta> *cols) {
    if (txn == nullptr || txn->get_isolation_level() != IsolationLevel::SERIALIZABLE) {
        return;
    }
    transaction_registry_.WithAllExclusive([&](TransactionRegistry::AllView transactions) {
        std::shared_ptr<RmRecord> old_copy =
            old_record == nullptr ? nullptr : std::make_shared<RmRecord>(*old_record);
        std::shared_ptr<RmRecord> new_copy =
            new_record == nullptr ? nullptr : std::make_shared<RmRecord>(*new_record);
        txn->upsert_serializable_write(tab_name, rid, cols == nullptr ? std::vector<ColMeta>() : *cols,
                                       std::move(old_copy), std::move(new_copy));

        transactions.ForEach([&](txn_id_t, Transaction *other) {
            if (!is_overlapping_serializable(other, txn)) {
                return;
            }
            if (has_read_record(other, tab_name, rid)) {
                add_dependency(transactions, other, txn, txn);
            }
            if (cols == nullptr) {
                return;
            }
            for (const auto &predicate : other->get_predicate_reads()) {
                bool matches_old = old_record != nullptr && predicate.tab_name == tab_name &&
                                   record_matches(*cols, *old_record, predicate.conds);
                bool matches_new = new_record != nullptr && predicate.tab_name == tab_name &&
                                   record_matches(*cols, *new_record, predicate.conds);
                if (matches_old || matches_new) {
                    add_dependency(transactions, other, txn, txn);
                    return;
                }
            }
        });
    });
}
