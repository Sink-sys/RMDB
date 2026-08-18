/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "analyze.h"

#include <cmath>

namespace {
std::string agg_default_name(ast::AggType type) {
    switch (type) {
        case ast::AGG_COUNT: return "count";
        case ast::AGG_MAX: return "max";
        case ast::AGG_MIN: return "min";
        case ast::AGG_SUM: return "sum";
        case ast::AGG_AVG: return "avg";
        default: return "";
    }
}

std::string select_item_name(const std::shared_ptr<ast::SelectItem> &item) {
    if (!item->alias.empty()) {
        return item->alias;
    }
    if (!item->is_agg && item->col) {
        return item->col->col_name;
    }
    if (item->count_star) {
        return "count";
    }
    return agg_default_name(item->agg_type);
}

bool same_col(const TabCol &lhs, const TabCol &rhs) {
    return lhs.tab_name == rhs.tab_name && lhs.col_name == rhs.col_name;
}

StmtKind infer_stmt_kind(const std::shared_ptr<ast::TreeNode> &parse) {
    if (std::dynamic_pointer_cast<ast::SelectStmt>(parse)) return StmtKind::Select;
    if (std::dynamic_pointer_cast<ast::InsertStmt>(parse)) return StmtKind::Insert;
    if (std::dynamic_pointer_cast<ast::UpdateStmt>(parse)) return StmtKind::Update;
    if (std::dynamic_pointer_cast<ast::DeleteStmt>(parse)) return StmtKind::Delete;
    if (std::dynamic_pointer_cast<ast::LoadStmt>(parse)) return StmtKind::Load;
    if (std::dynamic_pointer_cast<ast::UnionStmt>(parse)) return StmtKind::Union;
    if (std::dynamic_pointer_cast<ast::ExplainStmt>(parse)) return StmtKind::Explain;
    if (std::dynamic_pointer_cast<ast::Help>(parse)) return StmtKind::Help;
    if (std::dynamic_pointer_cast<ast::ShowTables>(parse)) return StmtKind::ShowTables;
    if (std::dynamic_pointer_cast<ast::ShowIndex>(parse)) return StmtKind::ShowIndex;
    if (std::dynamic_pointer_cast<ast::DescTable>(parse)) return StmtKind::DescTable;
    if (std::dynamic_pointer_cast<ast::TxnBegin>(parse)) return StmtKind::TxnBegin;
    if (std::dynamic_pointer_cast<ast::TxnCommit>(parse)) return StmtKind::TxnCommit;
    if (std::dynamic_pointer_cast<ast::TxnAbort>(parse)) return StmtKind::TxnAbort;
    if (std::dynamic_pointer_cast<ast::TxnRollback>(parse)) return StmtKind::TxnRollback;
    if (std::dynamic_pointer_cast<ast::SetTransactionIsolation>(parse)) return StmtKind::SetTransactionIsolation;
    if (std::dynamic_pointer_cast<ast::SetStmt>(parse)) return StmtKind::SetKnob;
    if (std::dynamic_pointer_cast<ast::CreateCheckpoint>(parse)) return StmtKind::CreateCheckpoint;
    if (std::dynamic_pointer_cast<ast::CreateTable>(parse)) return StmtKind::CreateTable;
    if (std::dynamic_pointer_cast<ast::DropTable>(parse)) return StmtKind::DropTable;
    if (std::dynamic_pointer_cast<ast::CreateIndex>(parse)) return StmtKind::CreateIndex;
    if (std::dynamic_pointer_cast<ast::DropIndex>(parse)) return StmtKind::DropIndex;
    return StmtKind::Unknown;
}

void fill_stmt_metadata(Query *query, const std::shared_ptr<ast::TreeNode> &parse) {
    query->kind = infer_stmt_kind(parse);
    if (auto x = std::dynamic_pointer_cast<ast::InsertStmt>(parse)) {
        query->target_table = x->tab_name;
    } else if (auto x = std::dynamic_pointer_cast<ast::UpdateStmt>(parse)) {
        query->target_table = x->tab_name;
    } else if (auto x = std::dynamic_pointer_cast<ast::DeleteStmt>(parse)) {
        query->target_table = x->tab_name;
    } else if (auto x = std::dynamic_pointer_cast<ast::LoadStmt>(parse)) {
        query->target_table = x->tab_name;
    } else if (auto x = std::dynamic_pointer_cast<ast::ShowIndex>(parse)) {
        query->target_table = x->tab_name;
    } else if (auto x = std::dynamic_pointer_cast<ast::DescTable>(parse)) {
        query->target_table = x->tab_name;
    } else if (auto x = std::dynamic_pointer_cast<ast::CreateTable>(parse)) {
        query->target_table = x->tab_name;
    } else if (auto x = std::dynamic_pointer_cast<ast::DropTable>(parse)) {
        query->target_table = x->tab_name;
    } else if (auto x = std::dynamic_pointer_cast<ast::CreateIndex>(parse)) {
        query->target_table = x->tab_name;
    } else if (auto x = std::dynamic_pointer_cast<ast::DropIndex>(parse)) {
        query->target_table = x->tab_name;
    }
}
}

/**
 * @description: 分析器，进行语义分析和查询重写，需要检查不符合语义规定的部分
 * @param {shared_ptr<ast::TreeNode>} parse parser生成的结果集
 * @return {shared_ptr<Query>} Query 
 */
std::shared_ptr<Query> Analyze::do_analyze(std::shared_ptr<ast::TreeNode> parse)
{
    std::shared_ptr<Query> query = std::make_shared<Query>();
    fill_stmt_metadata(query.get(), parse);
    if (auto x = std::dynamic_pointer_cast<ast::SelectStmt>(parse))
    {
        if (x->table_refs.size() == 1 && !x->table_refs[0]->union_selects.empty()) {
            auto output_cols = [&](const std::shared_ptr<Query> &subquery) {
                std::vector<ColMeta> metas;
                int offset = 0;
                for (auto &col : subquery->cols) {
                    auto &tab = sm_manager_->db_.get_table(col.tab_name);
                    ColMeta meta = *tab.get_col(col.col_name);
                    meta.tab_name = "";
                    meta.name = col.col_name;
                    meta.offset = offset;
                    offset += meta.len;
                    metas.push_back(meta);
                }
                return metas;
            };

            for (auto &select : x->table_refs[0]->union_selects) {
                auto subquery = do_analyze(select);
                query->union_queries.push_back(subquery);
                auto sub_cols = output_cols(subquery);
                if (query->union_cols.empty()) {
                    query->union_cols = sub_cols;
                    for (auto &col : query->union_cols) {
                        query->cols.push_back({.tab_name = "", .col_name = col.name});
                    }
                    continue;
                }
                if (query->union_cols.size() != sub_cols.size()) {
                    throw RMDBError("union column count mismatch");
                }
                int offset = 0;
                for (size_t i = 0; i < query->union_cols.size(); ++i) {
                    auto &dst = query->union_cols[i];
                    auto &src = sub_cols[i];
                    if (dst.type == src.type) {
                        if (dst.type == TYPE_STRING) {
                            dst.len = std::max(dst.len, src.len);
                        }
                    } else if ((dst.type == TYPE_INT && src.type == TYPE_FLOAT) ||
                               (dst.type == TYPE_FLOAT && src.type == TYPE_INT)) {
                        dst.type = TYPE_FLOAT;
                        dst.len = sizeof(float);
                    } else {
                        throw RMDBError("union column type mismatch");
                    }
                    dst.offset = offset;
                    offset += dst.len;
                }
            }
            if (x->has_sort) {
                for (auto &order_item : x->order->items) {
                    bool found = false;
                    for (auto &col : query->union_cols) {
                        if (col.name == order_item->col->col_name) {
                            query->order_cols.push_back({{.tab_name = "", .col_name = col.name},
                                                         order_item->orderby_dir == ast::OrderBy_DESC});
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        throw ColumnNotFoundError(order_item->col->col_name);
                    }
                }
            }
            query->parse = std::move(parse);
            return query;
        }
        // 处理表名。查询计划内部使用可见名（别名或表名）区分 self join 的两侧。
        query->table_refs = x->table_refs;
        query->has_limit = x->has_limit;
        query->limit = x->limit;
        query->is_semi_join = x->is_semi_join;
        for (auto &ref : x->table_refs) {
            if (!sm_manager_->db_.is_table(ref->tab_name)) {
                throw TableNotFoundError(ref->tab_name);
            }
            query->tables.push_back(ref->visible_name());
        }

        // 处理target list，再target list中添加上表名，例如 a.id
        for (auto &ref : x->table_refs) {
            query->alias_to_table[ref->visible_name()] = ref->tab_name;
        }
        auto resolve_visible = [&](TabCol &col) {
            if (col.tab_name.empty()) {
                return;
            }
            if (query->alias_to_table.count(col.tab_name)) {
                return;
            }
            for (auto &ref : x->table_refs) {
                if (ref->tab_name == col.tab_name && ref->alias.empty()) {
                    col.tab_name = ref->visible_name();
                    return;
                }
            }
        };
        
        std::vector<ColMeta> all_cols;
        for (auto &ref : x->table_refs) {
            auto cols = sm_manager_->db_.get_table(ref->tab_name).cols;
            for (auto &col : cols) {
                col.tab_name = ref->visible_name();
            }
            all_cols.insert(all_cols.end(), cols.begin(), cols.end());
        }

        query->select_items = x->select_items;
        const std::vector<ColMeta> *select_check_cols = &all_cols;
        std::vector<ColMeta> semi_join_select_cols;
        if (query->is_semi_join && !query->tables.empty()) {
            auto left_ref = x->table_refs.front();
            semi_join_select_cols = sm_manager_->db_.get_table(left_ref->tab_name).cols;
            for (auto &col : semi_join_select_cols) {
                col.tab_name = left_ref->visible_name();
            }
            select_check_cols = &semi_join_select_cols;
        }

        for (auto &item : x->select_items) {
            if (item->is_agg) {
                    query->has_aggregate = true;
                    if (!item->count_star && item->col) {
                        TabCol agg_col = {.tab_name = item->col->tab_name, .col_name = item->col->col_name};
                    resolve_visible(agg_col);
                    agg_col = check_column(all_cols, agg_col);
                    item->col->tab_name = agg_col.tab_name;
                }
            } else if (item->col) {
                TabCol sel_col = {.tab_name = item->col->tab_name, .col_name = item->col->col_name};
                resolve_visible(sel_col);
                sel_col = check_column(*select_check_cols, sel_col);
                item->col->tab_name = sel_col.tab_name;
                query->cols.push_back(sel_col);
            }
        }

        query->has_group = !x->group_cols.empty();
        for (auto &group_col : x->group_cols) {
            TabCol tab_col = {.tab_name = group_col->tab_name, .col_name = group_col->col_name};
            resolve_visible(tab_col);
            tab_col = check_column(all_cols, tab_col);
            query->group_cols.push_back(tab_col);
            group_col->tab_name = tab_col.tab_name;
        }
        query->having_conds = x->having_conds;
        for (auto &having : query->having_conds) {
            if (!having->lhs->count_star && having->lhs->col) {
                TabCol having_col = {.tab_name = having->lhs->col->tab_name, .col_name = having->lhs->col->col_name};
                resolve_visible(having_col);
                having_col = check_column(all_cols, having_col);
                having->lhs->col->tab_name = having_col.tab_name;
            }
        }
        if (!query->having_conds.empty()) {
            query->has_aggregate = true;
        }

        if (x->has_sort) {
            for (auto &order_item : x->order->items) {
                TabCol order_col = {.tab_name = order_item->col->tab_name, .col_name = order_item->col->col_name};
                resolve_visible(order_col);
                order_col = check_column(all_cols, order_col);
                query->order_cols.push_back({order_col, order_item->orderby_dir == ast::OrderBy_DESC});
            }
        }

        if (query->has_aggregate || query->has_group) {
            query->cols.clear();
            for (auto &item : x->select_items) {
                query->cols.push_back({.tab_name = "", .col_name = select_item_name(item)});
                if (!item->is_agg && item->col) {
                    TabCol sel_col = {.tab_name = item->col->tab_name, .col_name = item->col->col_name};
                    resolve_visible(sel_col);
                    bool found = false;
                    for (auto &group_col : query->group_cols) {
                        if (same_col(sel_col, group_col)) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        throw RMDBError("selected column must appear in group by");
                    }
                }
            }
        } else if (query->cols.empty()) {
            // select all columns
            for (auto &col : all_cols) {
                TabCol sel_col = {.tab_name = col.tab_name, .col_name = col.name};
                query->cols.push_back(sel_col);
            }
        }
        //处理where条件
        get_clause(x->conds, query->conds);
        for (auto &cond : query->conds) {
            resolve_visible(cond.lhs_col);
            if (!cond.is_rhs_val) {
                resolve_visible(cond.rhs_col);
            }
        }
        check_clause(query->tables, query->conds, &query->alias_to_table);
        get_clause(x->semi_conds, query->semi_conds);
        if (query->is_semi_join) {
            for (auto &cond : query->semi_conds) {
                resolve_visible(cond.lhs_col);
                if (!cond.is_rhs_val) {
                    resolve_visible(cond.rhs_col);
                }
            }
            check_clause(query->tables, query->semi_conds, &query->alias_to_table);
            std::string left_table = query->tables.front();
            for (auto &sel_col : query->cols) {
                if (!sel_col.tab_name.empty() && sel_col.tab_name != left_table) {
                    throw RMDBError("semi join can only project left table columns");
                }
            }
        }
    } else if (auto x = std::dynamic_pointer_cast<ast::UpdateStmt>(parse)) {
        if (!sm_manager_->db_.is_table(x->tab_name)) {
            throw TableNotFoundError(x->tab_name);
        }
        auto &tab = sm_manager_->db_.get_table(x->tab_name);
        auto normalize_value_to_type = [](Value rhs, ColType target_type, int target_len) {
            if (target_type == TYPE_FLOAT && rhs.type == TYPE_INT) {
                rhs.set_float(static_cast<float>(rhs.int_val));
                rhs.init_raw(target_len);
                return rhs;
            }
            if (target_type != rhs.type) {
                throw IncompatibleTypeError(coltype2str(target_type), coltype2str(rhs.type));
            }
            rhs.init_raw(target_len);
            return rhs;
        };
        for (auto &sv_set_clause : x->set_clauses) {
            auto lhs_col = check_column(tab.cols, {.tab_name = x->tab_name, .col_name = sv_set_clause->col_name});
            auto col_meta = tab.get_col(lhs_col.col_name);
            SetClause clause;
            clause.lhs = lhs_col;

            if (!sv_set_clause->rhs_is_col) {
                clause.op = SetOp::ASSIGN;
                clause.rhs = normalize_value_to_type(convert_sv_value(sv_set_clause->val), col_meta->type, col_meta->len);
                clause.rhs_has_val = true;
                query->set_clauses.push_back(std::move(clause));
                continue;
            }

            auto rhs_col = check_column(tab.cols, {.tab_name = x->tab_name, .col_name = sv_set_clause->rhs_col_name});
            auto rhs_meta = tab.get_col(rhs_col.col_name);
            clause.rhs_is_col = true;
            clause.rhs_col = rhs_col;

            if (sv_set_clause->op == ast::SET_OP_ASSIGN) {
                clause.op = SetOp::ASSIGN;
                if (!(col_meta->type == TYPE_FLOAT && rhs_meta->type == TYPE_INT) &&
                    col_meta->type != rhs_meta->type) {
                    throw IncompatibleTypeError(coltype2str(col_meta->type), coltype2str(rhs_meta->type));
                }
                query->set_clauses.push_back(std::move(clause));
                continue;
            }

            if (sv_set_clause->op != ast::SET_OP_ADD &&
                sv_set_clause->op != ast::SET_OP_SUB &&
                sv_set_clause->op != ast::SET_OP_MUL &&
                sv_set_clause->op != ast::SET_OP_DIV) {
                throw InternalError("Unsupported update set operation");
            }
            if (col_meta->type != TYPE_INT && col_meta->type != TYPE_FLOAT) {
                throw IncompatibleTypeError("numeric column", coltype2str(col_meta->type));
            }
            if (rhs_meta->type != TYPE_INT && rhs_meta->type != TYPE_FLOAT) {
                throw IncompatibleTypeError("numeric column", coltype2str(rhs_meta->type));
            }
            if (col_meta->type == TYPE_INT && rhs_meta->type != TYPE_INT) {
                throw IncompatibleTypeError(coltype2str(col_meta->type), coltype2str(rhs_meta->type));
            }

            if (sv_set_clause->op == ast::SET_OP_ADD) {
                clause.op = SetOp::ADD;
            } else if (sv_set_clause->op == ast::SET_OP_SUB) {
                clause.op = SetOp::SUB;
            } else if (sv_set_clause->op == ast::SET_OP_MUL) {
                clause.op = SetOp::MUL;
            } else {
                clause.op = SetOp::DIV;
            }
            clause.reversed_operands = sv_set_clause->reversed_operands;
            clause.rhs = normalize_value_to_type(convert_sv_value(sv_set_clause->val), col_meta->type, col_meta->len);
            clause.rhs_has_val = true;
            clause.extra_steps.reserve(sv_set_clause->extra_steps.size());
            for (const auto &sv_step : sv_set_clause->extra_steps) {
                SetArithmeticStep step;
                if (sv_step.op == ast::SET_OP_ADD) {
                    step.op = SetOp::ADD;
                } else if (sv_step.op == ast::SET_OP_SUB) {
                    step.op = SetOp::SUB;
                } else if (sv_step.op == ast::SET_OP_MUL) {
                    step.op = SetOp::MUL;
                } else if (sv_step.op == ast::SET_OP_DIV) {
                    step.op = SetOp::DIV;
                } else {
                    throw InternalError("Unsupported update arithmetic operation");
                }
                step.rhs = normalize_value_to_type(convert_sv_value(sv_step.val), col_meta->type, col_meta->len);
                clause.extra_steps.push_back(std::move(step));
            }
            query->set_clauses.push_back(std::move(clause));
        }
        get_clause(x->conds, query->conds);
        check_clause({x->tab_name}, query->conds);

    } else if (auto x = std::dynamic_pointer_cast<ast::DeleteStmt>(parse)) {
        if (!sm_manager_->db_.is_table(x->tab_name)) {
            throw TableNotFoundError(x->tab_name);
        }
        //处理where条件
        get_clause(x->conds, query->conds);
        check_clause({x->tab_name}, query->conds);        
    } else if (auto x = std::dynamic_pointer_cast<ast::LoadStmt>(parse)) {
        if (!sm_manager_->db_.is_table(x->tab_name)) {
            throw TableNotFoundError(x->tab_name);
        }
        query->load_file = x->file_name;
    } else if (auto x = std::dynamic_pointer_cast<ast::InsertStmt>(parse)) {
        if (!sm_manager_->db_.is_table(x->tab_name)) {
            throw TableNotFoundError(x->tab_name);
        }
        // 处理insert 的values值
        for (auto &sv_val : x->vals) {
            query->values.push_back(convert_sv_value(sv_val));
        }
    } else if (auto x = std::dynamic_pointer_cast<ast::UnionStmt>(parse)) {
        if (x->selects.empty()) {
            throw RMDBError("empty union");
        }
        auto output_cols = [&](const std::shared_ptr<Query> &subquery) {
            std::vector<ColMeta> metas;
            int offset = 0;
            for (auto &col : subquery->cols) {
                ColMeta meta;
                if (col.tab_name.empty()) {
                    meta.tab_name = "";
                    meta.name = col.col_name;
                    meta.type = TYPE_INT;
                    meta.len = sizeof(int);
                } else {
                    auto &tab = sm_manager_->db_.get_table(col.tab_name);
                    meta = *tab.get_col(col.col_name);
                    meta.tab_name = "";
                    meta.name = col.col_name;
                }
                meta.offset = offset;
                offset += meta.len;
                metas.push_back(meta);
            }
            return metas;
        };

        for (auto &select : x->selects) {
            auto subquery = do_analyze(select);
            query->union_queries.push_back(subquery);
            auto sub_cols = output_cols(subquery);
            if (query->union_cols.empty()) {
                query->union_cols = sub_cols;
                query->cols.clear();
                for (auto &col : query->union_cols) {
                    query->cols.push_back({.tab_name = "", .col_name = col.name});
                }
                continue;
            }
            if (query->union_cols.size() != sub_cols.size()) {
                throw RMDBError("union column count mismatch");
            }
            int offset = 0;
            for (size_t i = 0; i < query->union_cols.size(); ++i) {
                auto &dst = query->union_cols[i];
                auto &src = sub_cols[i];
                if (dst.type == src.type) {
                    if (dst.type == TYPE_STRING) {
                        dst.len = std::max(dst.len, src.len);
                    }
                } else if ((dst.type == TYPE_INT && src.type == TYPE_FLOAT) ||
                           (dst.type == TYPE_FLOAT && src.type == TYPE_INT)) {
                    dst.type = TYPE_FLOAT;
                    dst.len = sizeof(float);
                } else {
                    throw RMDBError("union column type mismatch");
                }
                dst.offset = offset;
                offset += dst.len;
            }
        }
        if (x->has_sort) {
            for (auto &order_item : x->order->items) {
                bool found = false;
                for (auto &col : query->union_cols) {
                    if (col.name == order_item->col->col_name) {
                        query->order_cols.push_back({{.tab_name = "", .col_name = col.name},
                                                     order_item->orderby_dir == ast::OrderBy_DESC});
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    throw ColumnNotFoundError(order_item->col->col_name);
                }
            }
        }
    } else {
        // do nothing
    }
    query->parse = std::move(parse);
    return query;
}


TabCol Analyze::check_column(const std::vector<ColMeta> &all_cols, TabCol target) {
    if (target.tab_name.empty()) {
        // Table name not specified, infer table name from column name
        std::string tab_name;
        for (auto &col : all_cols) {
            if (col.name == target.col_name) {
                if (!tab_name.empty()) {
                    throw AmbiguousColumnError(target.col_name);
                }
                tab_name = col.tab_name;
            }
        }
        if (tab_name.empty()) {
            throw ColumnNotFoundError(target.col_name);
        }
        target.tab_name = tab_name;
    } else {
        bool table_found = false;
        bool column_found = false;
        for (auto &col : all_cols) {
            if (col.tab_name == target.tab_name) {
                table_found = true;
                if (col.name == target.col_name) {
                    column_found = true;
                    break;
                }
            }
        }
        if (!table_found) {
            throw TableNotFoundError(target.tab_name);
        }
        if (!column_found) {
            throw ColumnNotFoundError(target.col_name);
        }
    }
    return target;
}

void Analyze::get_all_cols(const std::vector<std::string> &tab_names, std::vector<ColMeta> &all_cols) {
    for (auto &sel_tab_name : tab_names) {
        // 这里db_不能写成get_db(), 注意要传指针
        const auto &sel_tab_cols = sm_manager_->db_.get_table(sel_tab_name).cols;
        all_cols.insert(all_cols.end(), sel_tab_cols.begin(), sel_tab_cols.end());
    }
}

void Analyze::get_clause(const std::vector<std::shared_ptr<ast::BinaryExpr>> &sv_conds, std::vector<Condition> &conds) {
    conds.clear();
    conds.reserve(sv_conds.size());
    for (auto &expr : sv_conds) {
        Condition cond;
        cond.lhs_col = {.tab_name = expr->lhs->tab_name, .col_name = expr->lhs->col_name};
        cond.op = convert_sv_comp_op(expr->op);
        if (auto rhs_val = std::dynamic_pointer_cast<ast::Value>(expr->rhs)) {
            cond.is_rhs_val = true;
            cond.rhs_val = convert_sv_value(rhs_val);
        } else if (auto rhs_col = std::dynamic_pointer_cast<ast::Col>(expr->rhs)) {
            cond.is_rhs_val = false;
            cond.rhs_col = {.tab_name = rhs_col->tab_name, .col_name = rhs_col->col_name};
        }
        conds.push_back(std::move(cond));
    }
}

void Analyze::check_clause(const std::vector<std::string> &tab_names, std::vector<Condition> &conds,
                           const std::map<std::string, std::string> *alias_to_table) {
    // auto all_cols = get_all_cols(tab_names);
    std::vector<ColMeta> all_cols;
    if (alias_to_table == nullptr) {
        get_all_cols(tab_names, all_cols);
    } else {
        for (const auto &tab_name : tab_names) {
            auto base_it = alias_to_table->find(tab_name);
            const std::string &base_name = base_it == alias_to_table->end() ? tab_name : base_it->second;
            auto cols = sm_manager_->db_.get_table(base_name).cols;
            for (auto &col : cols) {
                col.tab_name = tab_name;
            }
            all_cols.insert(all_cols.end(), cols.begin(), cols.end());
        }
    }
    // Get raw values in where clause
    for (auto &cond : conds) {
        // Infer table name from column name
        cond.lhs_col = check_column(all_cols, cond.lhs_col);
        if (!cond.is_rhs_val) {
            cond.rhs_col = check_column(all_cols, cond.rhs_col);
        }
        auto base_table = [&](const std::string &tab_name) -> std::string {
            if (alias_to_table != nullptr) {
                auto it = alias_to_table->find(tab_name);
                if (it != alias_to_table->end()) {
                    return it->second;
                }
            }
            return tab_name;
        };
        TabMeta &lhs_tab = sm_manager_->db_.get_table(base_table(cond.lhs_col.tab_name));
        auto lhs_col = lhs_tab.get_col(cond.lhs_col.col_name);
        ColType lhs_type = lhs_col->type;
        ColType rhs_type;
        if (cond.is_rhs_val) {
            cond.rhs_val.init_raw(lhs_col->len);
            rhs_type = cond.rhs_val.type;
        } else {
            TabMeta &rhs_tab = sm_manager_->db_.get_table(base_table(cond.rhs_col.tab_name));
            auto rhs_col = rhs_tab.get_col(cond.rhs_col.col_name);
            rhs_type = rhs_col->type;
        }
        if (cond.is_rhs_val && lhs_type == TYPE_FLOAT && rhs_type == TYPE_INT) {
            cond.rhs_val.set_float(static_cast<float>(cond.rhs_val.int_val));
            cond.rhs_val.raw = nullptr;
            cond.rhs_val.init_raw(lhs_col->len);
            rhs_type = TYPE_FLOAT;
        }
        if (lhs_type != rhs_type) {
            throw IncompatibleTypeError(coltype2str(lhs_type), coltype2str(rhs_type));
        }
    }
}


Value Analyze::convert_sv_value(const std::shared_ptr<ast::Value> &sv_val) {
    Value val;
    if (auto int_lit = std::dynamic_pointer_cast<ast::IntLit>(sv_val)) {
        val.set_int(int_lit->val);
    } else if (auto float_lit = std::dynamic_pointer_cast<ast::FloatLit>(sv_val)) {
        if (!std::isfinite(float_lit->val)) {
            throw RMDBError("FLOAT literal must be finite");
        }
        val.set_float(float_lit->val);
    } else if (auto str_lit = std::dynamic_pointer_cast<ast::StringLit>(sv_val)) {
        val.set_str(str_lit->val);
    } else {
        throw InternalError("Unexpected sv value type");
    }
    return val;
}

CompOp Analyze::convert_sv_comp_op(ast::SvCompOp op) {
    switch (op) {
        case ast::SV_OP_EQ: return OP_EQ;
        case ast::SV_OP_NE: return OP_NE;
        case ast::SV_OP_LT: return OP_LT;
        case ast::SV_OP_GT: return OP_GT;
        case ast::SV_OP_LE: return OP_LE;
        case ast::SV_OP_GE: return OP_GE;
    }
    throw InternalError("Unexpected comparison operator");
}
