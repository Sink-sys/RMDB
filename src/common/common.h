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

#include <cassert>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "binary.h"
#include "common/interned_identifier.h"
#include "defs.h"
#include "record/rm_defs.h"


struct TabCol {
    rmdb::InternedIdentifier tab_name;
    rmdb::InternedIdentifier col_name;
    rmdb::InternedIdentifier output_name;

    friend bool operator<(const TabCol &x, const TabCol &y) {
        return std::make_pair(x.tab_name, x.col_name) < std::make_pair(y.tab_name, y.col_name);
    }
};

struct Value {
    ColType type;  // type of value
    union {
        int int_val;      // int value
        float float_val;  // float value
    };
    std::string str_val;  // string value

    std::shared_ptr<RmRecord> raw;  // raw record buffer

    void set_int(int int_val_) {
        type = TYPE_INT;
        int_val = int_val_;
    }

    void set_float(float float_val_) {
        type = TYPE_FLOAT;
        float_val = float_val_;
    }

    void set_str(std::string str_val_) {
        type = TYPE_STRING;
        str_val = std::move(str_val_);
    }

    void init_raw(int len) {
        assert(raw == nullptr);
        raw = std::make_shared<RmRecord>(len);
        write_raw_data(len);
    }

    void init_raw(int len, std::shared_ptr<RmRecord> raw_buffer) {
        assert(raw == nullptr);
        raw = std::move(raw_buffer);
        raw->Resize(len);
        write_raw_data(len);
    }

    // 刷新 raw 字节为当前逻辑值(int_val/float_val/str_val)。持久化 plan
    // (SessionPointProgram)的常量槽 raw 可能来自 statement_scratch 池,
    // 池记录跨操作复用后数据被覆盖;消费方使用 raw->data 前必须先刷新。
    void refresh_raw(int len) {
        if (raw == nullptr) {
            init_raw(len);
            return;
        }
        raw->Resize(len);
        write_raw_data(len);
    }

private:
    void write_raw_data(int len) {
        if (type == TYPE_INT) {
            assert(len == sizeof(int));
            rmdb::store_unaligned<int>(raw->data, int_val);
        } else if (type == TYPE_FLOAT) {
            assert(len == sizeof(float));
            rmdb::store_unaligned<float>(raw->data, float_val);
        } else if (type == TYPE_STRING) {
            if (len < (int)str_val.size()) {
                throw StringOverflowError();
            }
            memset(raw->data, 0, len);
            memcpy(raw->data, str_val.c_str(), str_val.size());
        }
    }
};

enum CompOp { OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE };

struct Condition {
    TabCol lhs_col;   // left-hand side column
    CompOp op;        // comparison operator
    bool is_rhs_val;  // true if right-hand side is a value (not a column)
    TabCol rhs_col;   // right-hand side column
    Value rhs_val;    // right-hand side value
};

enum class SetOp { ASSIGN, ADD, SUB, MUL, DIV };

struct SetArithmeticStep {
    SetOp op{SetOp::ASSIGN};
    Value rhs;
};

struct SetClause {
    TabCol lhs;
    SetOp op{SetOp::ASSIGN};
    bool rhs_is_col{false};
    TabCol rhs_col;
    bool rhs_has_val{false};
    Value rhs;
    bool reversed_operands{false};  // true for e.g. SET col = 10 - col
    std::vector<SetArithmeticStep> extra_steps;
};
