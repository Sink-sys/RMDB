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

#include <cstring>
#include <optional>
#include <vector>

#include "transaction/transaction.h"
#include "common/common.h"

struct ExecutionJoinKeyPart {
    size_t left_idx = 0;
    ColMeta left_col;
    size_t right_idx = 0;
    ColMeta right_col;
};

inline const ColMeta *FindExecutorColumn(const std::vector<ColMeta> &cols, const TabCol &target, size_t *index) {
    for (size_t i = 0; i < cols.size(); ++i) {
        if (cols[i].tab_name == target.tab_name && cols[i].name == target.col_name) {
            *index = i;
            return &cols[i];
        }
    }
    return nullptr;
}

inline std::vector<ExecutionJoinKeyPart> BuildEquiJoinKeyParts(const std::vector<ColMeta> &left_cols,
                                                               const std::vector<ColMeta> &right_cols,
                                                               const std::vector<Condition> &conditions) {
    std::vector<ExecutionJoinKeyPart> parts;
    parts.reserve(conditions.size());
    for (const auto &cond : conditions) {
        if (cond.is_rhs_val || cond.op != OP_EQ) {
            continue;
        }
        size_t left_idx = 0;
        size_t right_idx = 0;
        const ColMeta *left_col = FindExecutorColumn(left_cols, cond.lhs_col, &left_idx);
        const ColMeta *right_col = FindExecutorColumn(right_cols, cond.rhs_col, &right_idx);
        if (left_col == nullptr || right_col == nullptr) {
            left_col = FindExecutorColumn(left_cols, cond.rhs_col, &left_idx);
            right_col = FindExecutorColumn(right_cols, cond.lhs_col, &right_idx);
            if (left_col == nullptr || right_col == nullptr) {
                continue;
            }
        }
        if (left_col->type != right_col->type || left_col->len != right_col->len) {
            continue;
        }
        parts.push_back({left_idx, *left_col, right_idx, *right_col});
    }
    return parts;
}

inline auto IsWriteWriteConflict(timestamp_t tuple_ts, Transaction *txn) -> bool {
    if (txn == nullptr || tuple_ts == INVALID_TS) {
        return false;
    }
    if (tuple_ts >= TXN_START_ID) {
        txn_id_t owner = tuple_ts - TXN_START_ID;
        return owner != txn->get_transaction_id();
    }
    return tuple_ts > txn->get_read_ts();
}

inline bool UseMvccReadVisibility(Transaction *txn) {
    if (txn == nullptr) {
        return false;
    }
    return txn->get_isolation_level() == IsolationLevel::SNAPSHOT_ISOLATION ||
           txn->get_isolation_level() == IsolationLevel::SERIALIZABLE;
}
