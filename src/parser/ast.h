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

#include <vector>
#include <string>
#include <memory>

#include "common/types.h"

enum JoinType {
    INNER_JOIN, LEFT_JOIN, RIGHT_JOIN, FULL_JOIN, SEMI_JOIN
};
namespace ast {

enum SvType {
    SV_TYPE_INT, SV_TYPE_FLOAT, SV_TYPE_STRING, SV_TYPE_BOOL
};

enum SvCompOp {
    SV_OP_EQ, SV_OP_NE, SV_OP_LT, SV_OP_GT, SV_OP_LE, SV_OP_GE
};

enum OrderByDir {
    OrderBy_DEFAULT,
    OrderBy_ASC,
    OrderBy_DESC
};

enum AggType {
    AGG_NONE,
    AGG_COUNT,
    AGG_MAX,
    AGG_MIN,
    AGG_SUM,
    AGG_AVG
};

enum SetKnobType {
    EnableNestLoop, EnableSortMerge
};

enum SetClauseOp {
    SET_OP_ASSIGN,
    SET_OP_ADD,
    SET_OP_SUB,
    SET_OP_MUL,
    SET_OP_DIV
};

// Base class for tree nodes
struct TreeNode {
    virtual ~TreeNode() = default;  // enable polymorphism
};

struct Help : public TreeNode {
};

struct ShowTables : public TreeNode {
};

struct ShowIndex : public TreeNode {
    std::string tab_name;

    ShowIndex(std::string tab_name_) : tab_name(std::move(tab_name_)) {}
};

struct TxnBegin : public TreeNode {
};

struct TxnCommit : public TreeNode {
};

struct TxnAbort : public TreeNode {
};

struct TxnRollback : public TreeNode {
};

struct SetTransactionIsolation : public TreeNode {
    bool serializable;

    SetTransactionIsolation(bool serializable_) : serializable(serializable_) {}
};

struct TypeLen : public TreeNode {
    SvType type;
    int len;

    TypeLen(SvType type_, int len_) : type(type_), len(len_) {}
};

struct Field : public TreeNode {
};

struct ColDef : public Field {
    std::string col_name;
    std::shared_ptr<TypeLen> type_len;

    ColDef(std::string col_name_, std::shared_ptr<TypeLen> type_len_) :
            col_name(std::move(col_name_)), type_len(std::move(type_len_)) {}
};

struct CreateTable : public TreeNode {
    std::string tab_name;
    std::vector<std::shared_ptr<Field>> fields;

    CreateTable(std::string tab_name_, std::vector<std::shared_ptr<Field>> fields_) :
            tab_name(std::move(tab_name_)), fields(std::move(fields_)) {}
};

struct DropTable : public TreeNode {
    std::string tab_name;

    DropTable(std::string tab_name_) : tab_name(std::move(tab_name_)) {}
};

struct DescTable : public TreeNode {
    std::string tab_name;

    DescTable(std::string tab_name_) : tab_name(std::move(tab_name_)) {}
};

struct CreateIndex : public TreeNode {
    std::string tab_name;
    std::vector<std::string> col_names;

    CreateIndex(std::string tab_name_, std::vector<std::string> col_names_) :
            tab_name(std::move(tab_name_)), col_names(std::move(col_names_)) {}
};

struct DropIndex : public TreeNode {
    std::string tab_name;
    std::vector<std::string> col_names;

    DropIndex(std::string tab_name_, std::vector<std::string> col_names_) :
            tab_name(std::move(tab_name_)), col_names(std::move(col_names_)) {}
};

struct LoadStmt : public TreeNode {
    std::string file_name;
    std::string tab_name;

    LoadStmt(std::string file_name_, std::string tab_name_)
        : file_name(std::move(file_name_)), tab_name(std::move(tab_name_)) {}
};

struct Expr : public TreeNode {
};

struct Value : public Expr {
};

struct IntLit : public Value {
    int val;

    IntLit(int val_) : val(val_) {}
};

struct FloatLit : public Value {
    float val;

    FloatLit(float val_) : val(val_) {}
};

struct StringLit : public Value {
    std::string val;

    StringLit(std::string val_) : val(std::move(val_)) {}
};

struct BoolLit : public Value {
    bool val;

    BoolLit(bool val_) : val(val_) {}
};

struct ParameterRef : public Value {
    rmdb::u32 ordinal;

    explicit ParameterRef(rmdb::u32 ordinal_) : ordinal(ordinal_) {}
};

struct Col : public Expr {
    std::string tab_name;
    std::string col_name;

    Col(std::string tab_name_, std::string col_name_) :
            tab_name(std::move(tab_name_)), col_name(std::move(col_name_)) {}
};

struct SelectItem : public TreeNode {
    bool is_agg;
    AggType agg_type;
    std::shared_ptr<Col> col;
    bool count_star;
    bool distinct;
    std::string alias;

    SelectItem(std::shared_ptr<Col> col_, std::string alias_ = "") :
            is_agg(false), agg_type(AGG_NONE), col(std::move(col_)), count_star(false), distinct(false),
            alias(std::move(alias_)) {}

    SelectItem(AggType agg_type_, std::shared_ptr<Col> col_, bool count_star_, std::string alias_ = "",
               bool distinct_ = false) :
            is_agg(true), agg_type(agg_type_), col(std::move(col_)), count_star(count_star_), distinct(distinct_),
            alias(std::move(alias_)) {}
};

struct SetClause : public TreeNode {
    struct ArithmeticStep {
        SetClauseOp op;
        std::shared_ptr<Value> val;

        ArithmeticStep(SetClauseOp op_, std::shared_ptr<Value> val_) : op(op_), val(std::move(val_)) {}
    };

    std::string col_name;
    std::string rhs_col_name;
    std::shared_ptr<Value> val;
    bool rhs_is_col;
    SetClauseOp op;
    bool reversed_operands{false};  // true for e.g. SET col = 10 - col (literal on left)
    std::vector<ArithmeticStep> extra_steps;

    SetClause(std::string col_name_, std::shared_ptr<Value> val_) :
            col_name(std::move(col_name_)), rhs_col_name(), val(std::move(val_)), rhs_is_col(false),
            op(SET_OP_ASSIGN) {}

    SetClause(std::string col_name_, std::string rhs_col_name_) :
            col_name(std::move(col_name_)), rhs_col_name(std::move(rhs_col_name_)), val(nullptr), rhs_is_col(true),
            op(SET_OP_ASSIGN) {}

    SetClause(std::string col_name_, std::string rhs_col_name_, std::shared_ptr<Value> val_, SetClauseOp op_,
              bool reversed_ = false) :
            col_name(std::move(col_name_)), rhs_col_name(std::move(rhs_col_name_)), val(std::move(val_)),
            rhs_is_col(true), op(op_), reversed_operands(reversed_) {}

    void append_step(SetClauseOp op_, std::shared_ptr<Value> val_) {
        extra_steps.emplace_back(op_, std::move(val_));
    }
};

struct BinaryExpr : public TreeNode {
    std::shared_ptr<Col> lhs;
    SvCompOp op;
    std::shared_ptr<Expr> rhs;

    BinaryExpr(std::shared_ptr<Col> lhs_, SvCompOp op_, std::shared_ptr<Expr> rhs_) :
            lhs(std::move(lhs_)), op(op_), rhs(std::move(rhs_)) {}
};

struct HavingExpr : public TreeNode {
    std::shared_ptr<SelectItem> lhs;
    SvCompOp op;
    std::shared_ptr<Value> rhs;

    HavingExpr(std::shared_ptr<SelectItem> lhs_, SvCompOp op_, std::shared_ptr<Value> rhs_) :
            lhs(std::move(lhs_)), op(op_), rhs(std::move(rhs_)) {}
};

struct OrderByItem : public TreeNode {
    std::shared_ptr<Col> col;
    OrderByDir orderby_dir;

    OrderByItem(std::shared_ptr<Col> col_, OrderByDir orderby_dir_) :
            col(std::move(col_)), orderby_dir(orderby_dir_) {}
};

struct OrderBy : public TreeNode
{
    std::vector<std::shared_ptr<OrderByItem>> items;
    OrderBy(std::vector<std::shared_ptr<OrderByItem>> items_) : items(std::move(items_)) {}
};

struct SelectStmt;

struct TableRef : public TreeNode {
    std::string tab_name;
    std::string alias;
    std::vector<std::shared_ptr<SelectStmt>> union_selects;

    TableRef(std::string tab_name_, std::string alias_) :
            tab_name(std::move(tab_name_)), alias(std::move(alias_)) {}

    TableRef(std::vector<std::shared_ptr<SelectStmt>> union_selects_, std::string alias_) :
            tab_name(std::move(alias_)), alias(tab_name), union_selects(std::move(union_selects_)) {}

    std::string visible_name() const { return alias.empty() ? tab_name : alias; }
};

struct FromClause : public TreeNode {
    std::vector<std::shared_ptr<TableRef>> table_refs;
    std::vector<std::shared_ptr<BinaryExpr>> join_conds;
    bool is_semi_join;
    std::vector<std::shared_ptr<BinaryExpr>> semi_conds;

    FromClause() : is_semi_join(false) {}
};

struct InsertStmt : public TreeNode {
    std::string tab_name;
    std::vector<std::shared_ptr<Value>> vals;

    InsertStmt(std::string tab_name_, std::vector<std::shared_ptr<Value>> vals_) :
            tab_name(std::move(tab_name_)), vals(std::move(vals_)) {}
};

struct DeleteStmt : public TreeNode {
    std::string tab_name;
    std::vector<std::shared_ptr<BinaryExpr>> conds;

    DeleteStmt(std::string tab_name_, std::vector<std::shared_ptr<BinaryExpr>> conds_) :
            tab_name(std::move(tab_name_)), conds(std::move(conds_)) {}
};

struct UpdateStmt : public TreeNode {
    std::string tab_name;
    std::vector<std::shared_ptr<SetClause>> set_clauses;
    std::vector<std::shared_ptr<BinaryExpr>> conds;

    UpdateStmt(std::string tab_name_,
               std::vector<std::shared_ptr<SetClause>> set_clauses_,
               std::vector<std::shared_ptr<BinaryExpr>> conds_) :
            tab_name(std::move(tab_name_)), set_clauses(std::move(set_clauses_)), conds(std::move(conds_)) {}
};

struct JoinExpr : public TreeNode {
    std::string left;
    std::string right;
    std::vector<std::shared_ptr<BinaryExpr>> conds;
    JoinType type;

    JoinExpr(std::string left_, std::string right_,
               std::vector<std::shared_ptr<BinaryExpr>> conds_, JoinType type_) :
            left(std::move(left_)), right(std::move(right_)), conds(std::move(conds_)), type(type_) {}
};

struct SelectStmt : public TreeNode {
    std::vector<std::shared_ptr<Col>> cols;
    std::vector<std::string> tabs;
    std::vector<std::shared_ptr<SelectItem>> select_items;
    std::vector<std::shared_ptr<TableRef>> table_refs;
    std::vector<std::shared_ptr<BinaryExpr>> conds;
    std::vector<std::shared_ptr<Col>> group_cols;
    std::vector<std::shared_ptr<HavingExpr>> having_conds;
    std::vector<std::shared_ptr<JoinExpr>> jointree;

    
    bool has_sort;
    std::shared_ptr<OrderBy> order;
    bool has_limit;
    int limit;
    bool is_semi_join;
    std::vector<std::shared_ptr<BinaryExpr>> semi_conds;


    SelectStmt(std::vector<std::shared_ptr<Col>> cols_,
               std::vector<std::string> tabs_,
               std::vector<std::shared_ptr<BinaryExpr>> conds_,
               std::shared_ptr<OrderBy> order_) :
            cols(std::move(cols_)), tabs(std::move(tabs_)), conds(std::move(conds_)), 
            order(std::move(order_)) {
                has_sort = (bool)order;
                has_limit = false;
                limit = -1;
                is_semi_join = false;
            }

    SelectStmt(std::vector<std::shared_ptr<SelectItem>> select_items_,
               std::vector<std::shared_ptr<TableRef>> table_refs_,
               std::vector<std::shared_ptr<BinaryExpr>> conds_,
               std::vector<std::shared_ptr<Col>> group_cols_,
               std::vector<std::shared_ptr<HavingExpr>> having_conds_,
               std::shared_ptr<OrderBy> order_,
               int limit_,
               bool is_semi_join_,
               std::vector<std::shared_ptr<BinaryExpr>> semi_conds_) :
            select_items(std::move(select_items_)), table_refs(std::move(table_refs_)), conds(std::move(conds_)),
            group_cols(std::move(group_cols_)), having_conds(std::move(having_conds_)), order(std::move(order_)),
            has_limit(limit_ >= 0), limit(limit_), is_semi_join(is_semi_join_),
            semi_conds(std::move(semi_conds_)) {
                has_sort = (bool)order;
                for (auto &ref : table_refs) {
                    tabs.push_back(ref->tab_name);
                }
                if (select_items.empty()) {
                    cols = {};
                } else {
                    for (auto &item : select_items) {
                        if (!item->is_agg && item->col) {
                            cols.push_back(item->col);
                        }
                    }
                }
            }
};

struct CreateCheckpoint : public TreeNode {
};

struct ExplainStmt : public TreeNode {
    std::shared_ptr<SelectStmt> select;

    ExplainStmt(std::shared_ptr<SelectStmt> select_) : select(std::move(select_)) {}
};

struct UnionStmt : public TreeNode {
    std::vector<std::shared_ptr<SelectStmt>> selects;
    std::string alias;
    bool has_sort;
    std::shared_ptr<OrderBy> order;

    UnionStmt(std::vector<std::shared_ptr<SelectStmt>> selects_, std::string alias_,
              std::shared_ptr<OrderBy> order_) :
            selects(std::move(selects_)), alias(std::move(alias_)), has_sort((bool)order_),
            order(std::move(order_)) {}
};

// set enable_nestloop
struct SetStmt : public TreeNode {
    SetKnobType set_knob_type_;
    bool bool_val_;

    SetStmt(SetKnobType &type, bool bool_value) : 
        set_knob_type_(type), bool_val_(bool_value) { }
};

// Semantic value
struct SemValue {
    int sv_int;
    float sv_float;
    std::string sv_str;
    bool sv_bool;
    OrderByDir sv_orderby_dir;
    std::vector<std::string> sv_strs;

    std::shared_ptr<TreeNode> sv_node;

    SvCompOp sv_comp_op;

    std::shared_ptr<TypeLen> sv_type_len;

    std::shared_ptr<Field> sv_field;
    std::vector<std::shared_ptr<Field>> sv_fields;

    std::shared_ptr<Expr> sv_expr;

    std::shared_ptr<Value> sv_val;
    std::vector<std::shared_ptr<Value>> sv_vals;

    std::shared_ptr<Col> sv_col;
    std::vector<std::shared_ptr<Col>> sv_cols;

    std::shared_ptr<SelectItem> sv_select_item;
    std::vector<std::shared_ptr<SelectItem>> sv_select_items;
    std::shared_ptr<SelectStmt> sv_select_stmt;
    std::vector<std::shared_ptr<SelectStmt>> sv_select_stmts;
    std::shared_ptr<TableRef> sv_table_ref;
    std::vector<std::shared_ptr<TableRef>> sv_table_refs;
    std::shared_ptr<FromClause> sv_from;
    std::shared_ptr<HavingExpr> sv_having;
    std::vector<std::shared_ptr<HavingExpr>> sv_havings;
    std::vector<std::shared_ptr<BinaryExpr>> sv_join_conds;
    std::shared_ptr<OrderByItem> sv_orderby_item;
    std::vector<std::shared_ptr<OrderByItem>> sv_orderby_items;
    AggType sv_agg_type;

    std::shared_ptr<SetClause> sv_set_clause;
    std::vector<std::shared_ptr<SetClause>> sv_set_clauses;

    std::shared_ptr<BinaryExpr> sv_cond;
    std::vector<std::shared_ptr<BinaryExpr>> sv_conds;

    std::shared_ptr<OrderBy> sv_orderby;

    SetKnobType sv_setKnobType;
};

}

#define YYSTYPE ast::SemValue
