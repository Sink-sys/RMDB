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
#include <map>
#include <string>
#include <vector>

#include "parser/parser.h"
#include "system/sm.h"
#include "common/common.h"

enum class StmtKind {
    Unknown,
    Select,
    Insert,
    Update,
    Delete,
    Load,
    Union,
    Explain,
    Help,
    ShowTables,
    ShowIndex,
    DescTable,
    TxnBegin,
    TxnCommit,
    TxnAbort,
    TxnRollback,
    SetTransactionIsolation,
    SetKnob,
    CreateCheckpoint,
    CreateTable,
    DropTable,
    CreateIndex,
    DropIndex,
};

class Query{
    public:
    StmtKind kind = StmtKind::Unknown;
    std::shared_ptr<ast::TreeNode> parse;
    std::string target_table;
    // TODO jointree
    // where条件
    std::vector<Condition> conds;
    // 投影列
    std::vector<TabCol> cols;
    // 表名
    std::vector<std::string> tables;
    // update 的set 值
    std::vector<SetClause> set_clauses;
    //insert 的values值
    std::vector<Value> values;
    // load 的文件名
    std::string load_file;
    std::vector<std::shared_ptr<ast::SelectItem>> select_items;
    std::vector<TabCol> group_cols;
    std::vector<std::shared_ptr<ast::HavingExpr>> having_conds;
    std::vector<std::pair<TabCol, bool>> order_cols;
    bool has_aggregate = false;
    bool has_group = false;
    bool has_limit = false;
    int limit = -1;
    bool is_semi_join = false;
    std::vector<Condition> semi_conds;
    std::vector<std::shared_ptr<ast::TableRef>> table_refs;
    std::map<std::string, std::string> alias_to_table;
    std::vector<std::shared_ptr<Query>> union_queries;
    std::vector<ColMeta> union_cols;

    Query(){}

};

class Analyze
{
private:
    SmManager *sm_manager_;
public:
    Analyze(SmManager *sm_manager) : sm_manager_(sm_manager){}
    ~Analyze(){}

    std::shared_ptr<Query> do_analyze(std::shared_ptr<ast::TreeNode> root);

private:
    TabCol check_column(const std::vector<ColMeta> &all_cols, TabCol target);
    void get_all_cols(const std::vector<std::string> &tab_names, std::vector<ColMeta> &all_cols);
    void get_clause(const std::vector<std::shared_ptr<ast::BinaryExpr>> &sv_conds, std::vector<Condition> &conds);
    void check_clause(const std::vector<std::string> &tab_names, std::vector<Condition> &conds,
                      const std::map<std::string, std::string> *alias_to_table = nullptr);
    Value convert_sv_value(const std::shared_ptr<ast::Value> &sv_val);
    CompOp convert_sv_comp_op(ast::SvCompOp op);
};
