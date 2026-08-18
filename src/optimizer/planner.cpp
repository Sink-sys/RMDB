/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "planner.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>

#include "common/index_runtime.h"
#include "common/sql_template_cache.h"
#include "execution/executor_delete.h"
#include "execution/executor_index_scan.h"
#include "execution/executor_insert.h"
#include "execution/executor_nestedloop_join.h"
#include "execution/executor_projection.h"
#include "execution/executor_seq_scan.h"
#include "execution/executor_update.h"
#include "index/ix.h"
#include "record/rm_scan.h"
#include "record_printer.h"

namespace {
std::string col_to_string(const std::shared_ptr<ast::Col> &col) {
    return col->tab_name.empty() ? col->col_name : col->tab_name + "." + col->col_name;
}

std::string value_to_string(const std::shared_ptr<ast::Value> &value) {
    if (auto int_lit = std::dynamic_pointer_cast<ast::IntLit>(value)) {
        return std::to_string(int_lit->val);
    }
    if (auto float_lit = std::dynamic_pointer_cast<ast::FloatLit>(value)) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.6f", float_lit->val);
        return buf;
    }
    if (auto str_lit = std::dynamic_pointer_cast<ast::StringLit>(value)) {
        return "'" + str_lit->val + "'";
    }
    return "";
}

std::string op_to_string(ast::SvCompOp op) {
    switch (op) {
        case ast::SV_OP_EQ: return "=";
        case ast::SV_OP_NE: return "<>";
        case ast::SV_OP_LT: return "<";
        case ast::SV_OP_GT: return ">";
        case ast::SV_OP_LE: return "<=";
        case ast::SV_OP_GE: return ">=";
    }
    return "";
}

std::string cond_to_string(const std::shared_ptr<ast::BinaryExpr> &cond) {
    std::string rhs;
    if (auto rhs_col = std::dynamic_pointer_cast<ast::Col>(cond->rhs)) {
        rhs = col_to_string(rhs_col);
    } else if (auto rhs_val = std::dynamic_pointer_cast<ast::Value>(cond->rhs)) {
        rhs = value_to_string(rhs_val);
    }
    return col_to_string(cond->lhs) + op_to_string(cond->op) + rhs;
}

std::string join_tables_string(const std::set<std::string> &tables) {
    std::string out;
    bool first = true;
    for (auto &table : tables) {
        if (!first) {
            out += ", ";
        }
        out += table;
        first = false;
    }
    return out;
}

std::string project_columns_string(std::vector<std::string> cols) {
    std::sort(cols.begin(), cols.end());
    std::string out;
    for (size_t i = 0; i < cols.size(); ++i) {
        if (i != 0) {
            out += ", ";
        }
        out += cols[i];
    }
    return out;
}

bool table_has_filter(const std::vector<std::shared_ptr<ast::BinaryExpr>> &conds, const std::string &visible) {
    for (auto &cond : conds) {
        auto rhs_col = std::dynamic_pointer_cast<ast::Col>(cond->rhs);
        if (cond->lhs->tab_name == visible &&
            (std::dynamic_pointer_cast<ast::Value>(cond->rhs) || (rhs_col && rhs_col->tab_name == visible))) {
            return true;
        }
    }
    return false;
}

std::string ast_col_key(const std::shared_ptr<ast::Col> &col) {
    return col->tab_name + "\x1f" + col->col_name;
}

std::string condition_col_key(const TabCol &col) {
    return col.tab_name + "\x1f" + col.col_name;
}

std::string condition_key(const Condition &cond) {
    std::string key = condition_col_key(cond.lhs_col) + "\x1e" + std::to_string(static_cast<int>(cond.op)) + "\x1e";
    if (!cond.is_rhs_val) {
        return key + condition_col_key(cond.rhs_col);
    }
    key += "v" + std::to_string(static_cast<int>(cond.rhs_val.type)) + "\x1e";
    if (cond.rhs_val.type == TYPE_INT) {
        key += std::to_string(cond.rhs_val.int_val);
    } else if (cond.rhs_val.type == TYPE_FLOAT) {
        key += std::to_string(cond.rhs_val.float_val);
    } else {
        key += cond.rhs_val.str_val;
    }
    return key;
}

void add_required_col(std::vector<TabCol> *cols, const TabCol &col) {
    if (col.col_name.empty()) {
        return;
    }
    auto it = std::find_if(cols->begin(), cols->end(), [&](const TabCol &existing) {
        return existing.tab_name == col.tab_name && existing.col_name == col.col_name;
    });
    if (it == cols->end()) {
        cols->push_back(col);
    }
}

void add_condition_required_cols(std::vector<TabCol> *cols, const std::vector<Condition> &conds) {
    for (const auto &cond : conds) {
        add_required_col(cols, cond.lhs_col);
        if (!cond.is_rhs_val) {
            add_required_col(cols, cond.rhs_col);
        }
    }
}

bool is_simple_count_query(const std::shared_ptr<Query> &query) {
    if (query == nullptr || !query->has_aggregate || query->has_group || !query->group_cols.empty() ||
        !query->having_conds.empty() || query->select_items.size() != 1 || query->cols.size() != 1 ||
        query->tables.size() != 1 || !query->order_cols.empty()) {
        return false;
    }
    const auto &item = query->select_items[0];
    return item != nullptr && item->is_agg && item->agg_type == ast::AGG_COUNT &&
           !item->distinct && (item->count_star || item->col != nullptr);
}

std::vector<TabCol> scan_required_cols_for_index_choice(const std::shared_ptr<Query> &query,
                                                        const std::string &visible,
                                                        const std::vector<Condition> &local_conds) {
    std::vector<TabCol> required;
    if (query == nullptr) {
        return required;
    }
    if (query->has_aggregate || query->has_group) {
        for (const auto &group_col : query->group_cols) {
            add_required_col(&required, group_col);
        }
        for (const auto &item : query->select_items) {
            if (item == nullptr) {
                continue;
            }
            if (item->is_agg && !item->count_star && item->col != nullptr) {
                add_required_col(&required, {item->col->tab_name, item->col->col_name});
            } else if (!item->is_agg && item->col != nullptr) {
                add_required_col(&required, {item->col->tab_name, item->col->col_name});
            }
        }
        for (const auto &having : query->having_conds) {
            if (having != nullptr && having->lhs != nullptr && !having->lhs->count_star &&
                having->lhs->col != nullptr) {
                add_required_col(&required, {having->lhs->col->tab_name, having->lhs->col->col_name});
            }
        }
    } else {
        for (const auto &col : query->cols) {
            add_required_col(&required, col);
        }
    }
    for (const auto &order_col : query->order_cols) {
        add_required_col(&required, order_col.first);
    }
    add_condition_required_cols(&required, local_conds);
    add_condition_required_cols(&required, query->conds);

    std::vector<TabCol> visible_required;
    for (const auto &col : required) {
        if (col.tab_name == visible) {
            add_required_col(&visible_required, col);
        }
    }
    return visible_required;
}

void propagate_equal_join_value_filters(std::vector<std::shared_ptr<ast::BinaryExpr>> &conds) {
    std::map<std::string, std::shared_ptr<ast::Col>> key_to_col;
    std::map<std::string, std::vector<std::string>> graph;
    for (const auto &cond : conds) {
        auto rhs_col = std::dynamic_pointer_cast<ast::Col>(cond->rhs);
        if (!rhs_col || cond->op != ast::SV_OP_EQ || cond->lhs->tab_name == rhs_col->tab_name) {
            continue;
        }
        std::string lhs_key = ast_col_key(cond->lhs);
        std::string rhs_key = ast_col_key(rhs_col);
        key_to_col[lhs_key] = cond->lhs;
        key_to_col[rhs_key] = rhs_col;
        graph[lhs_key].push_back(rhs_key);
        graph[rhs_key].push_back(lhs_key);
    }

    std::map<std::string, std::vector<std::shared_ptr<ast::Col>>> component_cols;
    std::set<std::string> visited;
    for (const auto &entry : key_to_col) {
        if (!visited.insert(entry.first).second) {
            continue;
        }
        std::vector<std::string> stack{entry.first};
        std::vector<std::shared_ptr<ast::Col>> cols;
        while (!stack.empty()) {
            std::string current = stack.back();
            stack.pop_back();
            cols.push_back(key_to_col.at(current));
            for (const auto &next : graph[current]) {
                if (visited.insert(next).second) {
                    stack.push_back(next);
                }
            }
        }
        for (const auto &col : cols) {
            component_cols[ast_col_key(col)] = cols;
        }
    }

    std::set<std::string> existing;
    for (const auto &cond : conds) {
        existing.insert(cond_to_string(cond));
    }
    std::vector<std::shared_ptr<ast::BinaryExpr>> additions;
    for (const auto &cond : conds) {
        if (!std::dynamic_pointer_cast<ast::Value>(cond->rhs)) {
            continue;
        }
        auto comp_it = component_cols.find(ast_col_key(cond->lhs));
        if (comp_it == component_cols.end()) {
            continue;
        }
        for (const auto &target_col : comp_it->second) {
            if (target_col->tab_name == cond->lhs->tab_name && target_col->col_name == cond->lhs->col_name) {
                continue;
            }
            auto derived_lhs = std::make_shared<ast::Col>(target_col->tab_name, target_col->col_name);
            auto derived = std::make_shared<ast::BinaryExpr>(derived_lhs, cond->op, cond->rhs);
            if (existing.insert(cond_to_string(derived)).second) {
                additions.push_back(std::move(derived));
            }
        }
    }
    conds.insert(conds.end(), additions.begin(), additions.end());
}

void propagate_equal_join_value_filters(std::vector<Condition> &conds) {
    std::map<std::string, TabCol> key_to_col;
    std::map<std::string, std::vector<std::string>> graph;
    for (const auto &cond : conds) {
        if (cond.is_rhs_val || cond.op != OP_EQ || cond.lhs_col.tab_name == cond.rhs_col.tab_name) {
            continue;
        }
        std::string lhs_key = condition_col_key(cond.lhs_col);
        std::string rhs_key = condition_col_key(cond.rhs_col);
        key_to_col[lhs_key] = cond.lhs_col;
        key_to_col[rhs_key] = cond.rhs_col;
        graph[lhs_key].push_back(rhs_key);
        graph[rhs_key].push_back(lhs_key);
    }

    std::map<std::string, std::vector<TabCol>> component_cols;
    std::set<std::string> visited;
    for (const auto &entry : key_to_col) {
        if (!visited.insert(entry.first).second) {
            continue;
        }
        std::vector<std::string> stack{entry.first};
        std::vector<TabCol> cols;
        while (!stack.empty()) {
            std::string current = stack.back();
            stack.pop_back();
            cols.push_back(key_to_col.at(current));
            for (const auto &next : graph[current]) {
                if (visited.insert(next).second) {
                    stack.push_back(next);
                }
            }
        }
        for (const auto &col : cols) {
            component_cols[condition_col_key(col)] = cols;
        }
    }

    std::set<std::string> existing;
    for (const auto &cond : conds) {
        existing.insert(condition_key(cond));
    }
    std::vector<Condition> additions;
    for (const auto &cond : conds) {
        if (!cond.is_rhs_val) {
            continue;
        }
        auto comp_it = component_cols.find(condition_col_key(cond.lhs_col));
        if (comp_it == component_cols.end()) {
            continue;
        }
        for (const auto &target_col : comp_it->second) {
            if (target_col.tab_name == cond.lhs_col.tab_name && target_col.col_name == cond.lhs_col.col_name) {
                continue;
            }
            Condition derived = cond;
            derived.lhs_col = target_col;
            if (existing.insert(condition_key(derived)).second) {
                additions.push_back(std::move(derived));
            }
        }
    }
    conds.insert(conds.end(), additions.begin(), additions.end());
}

std::string table_filter_string(const std::vector<std::shared_ptr<ast::BinaryExpr>> &conds, const std::string &visible) {
    std::vector<std::string> filters;
    for (auto &cond : conds) {
        auto rhs_col = std::dynamic_pointer_cast<ast::Col>(cond->rhs);
        if (cond->lhs->tab_name == visible &&
            (std::dynamic_pointer_cast<ast::Value>(cond->rhs) || (rhs_col && rhs_col->tab_name == visible))) {
            filters.push_back(cond_to_string(cond));
        }
    }
    std::sort(filters.begin(), filters.end());
    std::string out;
    for (size_t i = 0; i < filters.size(); ++i) {
        if (i != 0) {
            out += ", ";
        }
        out += filters[i];
    }
    return out;
}

std::vector<std::string> make_explain_lines(const std::shared_ptr<ast::SelectStmt> &select, SmManager *sm_manager) {
    std::map<std::string, std::string> alias_to_table;
    std::vector<std::string> input_visible;
    std::vector<std::string> input_tables;
    for (auto &ref : select->table_refs) {
        alias_to_table[ref->visible_name()] = ref->tab_name;
        input_visible.push_back(ref->visible_name());
        input_tables.push_back(ref->tab_name);
    }
    if (input_visible.size() == 1) {
        const std::string &default_alias = input_visible.front();
        for (auto &item : select->select_items) {
            if (item->col && item->col->tab_name.empty()) {
                item->col->tab_name = default_alias;
            }
        }
        for (auto &cond : select->conds) {
            if (cond->lhs->tab_name.empty()) {
                cond->lhs->tab_name = default_alias;
            }
            if (auto rhs_col = std::dynamic_pointer_cast<ast::Col>(cond->rhs)) {
                if (rhs_col->tab_name.empty()) {
                    rhs_col->tab_name = default_alias;
                }
            }
        }
    }
    auto explain_conds = select->conds;
    propagate_equal_join_value_filters(explain_conds);

    auto table_rows = [&](const std::string &alias) {
        int rows = 0;
        auto fh = sm_manager->fhs_.at(alias_to_table[alias]).get();
        for (RmScan scan(fh); !scan.is_end(); scan.next()) {
            rows++;
        }
        return rows;
    };

    auto table_has_join_index = [&](const std::string &alias, const std::shared_ptr<ast::BinaryExpr> &cond) {
        std::vector<std::string> cols;
        auto rhs_col = std::dynamic_pointer_cast<ast::Col>(cond->rhs);
        if (cond->lhs->tab_name == alias) {
            cols.push_back(cond->lhs->col_name);
        } else if (rhs_col && rhs_col->tab_name == alias) {
            cols.push_back(rhs_col->col_name);
        }
        if (cols.empty()) {
            return false;
        }
        auto &tab = sm_manager->db_.get_table(alias_to_table[alias]);
        return tab.is_index(cols, true);
    };

    auto read_col = [&](const std::string &alias, const Rid &rid, const std::string &col_name) {
        auto &tab = sm_manager->db_.get_table(alias_to_table[alias]);
        auto col = tab.get_col(col_name);
        auto rec = sm_manager->fhs_.at(alias_to_table[alias])->get_record(rid, nullptr);
        std::string raw(rec->data + col->offset, col->len);
        return std::pair<ColMeta, std::string>(*col, raw);
    };

    auto compare_raw = [&](const ColMeta &lhs_col, const std::string &lhs_raw,
                           const ColMeta &rhs_col, const std::string &rhs_raw,
                           ast::SvCompOp op) {
        int cmp = 0;
        if (lhs_col.type == TYPE_FLOAT || rhs_col.type == TYPE_FLOAT) {
            float lhs = lhs_col.type == TYPE_FLOAT ? rmdb::load_unaligned<float>(lhs_raw.data())
                                                   : static_cast<float>(rmdb::load_unaligned<int>(lhs_raw.data()));
            float rhs = rhs_col.type == TYPE_FLOAT ? rmdb::load_unaligned<float>(rhs_raw.data())
                                                   : static_cast<float>(rmdb::load_unaligned<int>(rhs_raw.data()));
            cmp = (lhs > rhs) - (lhs < rhs);
        } else if (lhs_col.type == TYPE_INT) {
            int lhs = rmdb::load_unaligned<int>(lhs_raw.data());
            int rhs = rmdb::load_unaligned<int>(rhs_raw.data());
            cmp = (lhs > rhs) - (lhs < rhs);
        } else {
            std::string lhs = lhs_raw;
            std::string rhs = rhs_raw;
            lhs.resize(strlen(lhs.c_str()));
            rhs.resize(strlen(rhs.c_str()));
            cmp = lhs.compare(rhs);
        }
        switch (op) {
            case ast::SV_OP_EQ: return cmp == 0;
            case ast::SV_OP_NE: return cmp != 0;
            case ast::SV_OP_LT: return cmp < 0;
            case ast::SV_OP_GT: return cmp > 0;
            case ast::SV_OP_LE: return cmp <= 0;
            case ast::SV_OP_GE: return cmp >= 0;
        }
        return false;
    };

    auto compare_value_cond = [&](const std::string &alias, const Rid &rid,
                                  const std::shared_ptr<ast::BinaryExpr> &cond) {
        auto [lhs_col, lhs_raw] = read_col(alias, rid, cond->lhs->col_name);
        ColMeta rhs_col = lhs_col;
        std::string rhs_raw(lhs_col.len, '\0');
        if (auto int_lit = std::dynamic_pointer_cast<ast::IntLit>(cond->rhs)) {
            int value = int_lit->val;
            if (lhs_col.type == TYPE_FLOAT) {
                float f = static_cast<float>(value);
                memcpy(rhs_raw.data(), &f, sizeof(float));
                rhs_col.type = TYPE_FLOAT;
                rhs_col.len = sizeof(float);
            } else {
                memcpy(rhs_raw.data(), &value, sizeof(int));
                rhs_col.type = TYPE_INT;
                rhs_col.len = sizeof(int);
            }
        } else if (auto float_lit = std::dynamic_pointer_cast<ast::FloatLit>(cond->rhs)) {
            float value = float_lit->val;
            memcpy(rhs_raw.data(), &value, sizeof(float));
            rhs_col.type = TYPE_FLOAT;
            rhs_col.len = sizeof(float);
        } else if (auto str_lit = std::dynamic_pointer_cast<ast::StringLit>(cond->rhs)) {
            rhs_raw.assign(lhs_col.len, '\0');
            memcpy(rhs_raw.data(), str_lit->val.c_str(), std::min<int>(lhs_col.len, str_lit->val.size()));
            rhs_col.type = TYPE_STRING;
        }
        return compare_raw(lhs_col, lhs_raw, rhs_col, rhs_raw, cond->op);
    };

    std::map<std::string, std::vector<std::shared_ptr<ast::BinaryExpr>>> local_conds;
    std::vector<std::shared_ptr<ast::BinaryExpr>> join_conds;
    for (auto &cond : explain_conds) {
        auto rhs_col = std::dynamic_pointer_cast<ast::Col>(cond->rhs);
        if (rhs_col && rhs_col->tab_name != cond->lhs->tab_name) {
            join_conds.push_back(cond);
        } else {
            local_conds[cond->lhs->tab_name].push_back(cond);
        }
    }
    auto compare_filter_cond = [&](const std::string &alias, const Rid &rid,
                                   const std::shared_ptr<ast::BinaryExpr> &cond) {
        if (std::dynamic_pointer_cast<ast::Value>(cond->rhs)) {
            return compare_value_cond(alias, rid, cond);
        }
        auto rhs_col = std::dynamic_pointer_cast<ast::Col>(cond->rhs);
        auto [lhs_col, lhs_raw] = read_col(alias, rid, cond->lhs->col_name);
        auto [rhs_meta, rhs_raw] = read_col(alias, rid, rhs_col->col_name);
        return compare_raw(lhs_col, lhs_raw, rhs_meta, rhs_raw, cond->op);
    };

    auto filtered_rows = [&](const std::string &alias) {
        int rows = 0;
        auto fh = sm_manager->fhs_.at(alias_to_table[alias]).get();
        for (RmScan scan(fh); !scan.is_end(); scan.next()) {
            bool pass = true;
            for (auto &cond : local_conds[alias]) {
                if (!compare_filter_cond(alias, scan.rid(), cond)) {
                    pass = false;
                    break;
                }
            }
            if (pass) {
                rows++;
            }
        }
        return rows;
    };

    auto enumerate_records = [&](const std::set<std::string> &aliases, auto &&emit) {
        std::vector<std::string> order;
        for (auto &alias : input_visible) {
            if (aliases.count(alias)) {
                order.push_back(alias);
            }
        }
        std::map<std::string, Rid> current;
        std::function<void(size_t)> dfs = [&](size_t idx) {
            if (idx == order.size()) {
                for (auto &cond : explain_conds) {
                    auto rhs_col = std::dynamic_pointer_cast<ast::Col>(cond->rhs);
                    if (rhs_col) {
                        if (!aliases.count(cond->lhs->tab_name) || !aliases.count(rhs_col->tab_name)) {
                            continue;
                        }
                        auto [lhs_col, lhs_raw] = read_col(cond->lhs->tab_name, current[cond->lhs->tab_name],
                                                           cond->lhs->col_name);
                        auto [rhs_meta, rhs_raw] = read_col(rhs_col->tab_name, current[rhs_col->tab_name],
                                                            rhs_col->col_name);
                        if (!compare_raw(lhs_col, lhs_raw, rhs_meta, rhs_raw, cond->op)) {
                            return;
                        }
                    } else if (aliases.count(cond->lhs->tab_name) &&
                               !compare_filter_cond(cond->lhs->tab_name, current[cond->lhs->tab_name], cond)) {
                        return;
                    }
                }
                emit(current);
                return;
            }
            auto &alias = order[idx];
            auto fh = sm_manager->fhs_.at(alias_to_table[alias]).get();
            for (RmScan scan(fh); !scan.is_end(); scan.next()) {
                current[alias] = scan.rid();
                dfs(idx + 1);
            }
        };
        dfs(0);
    };

    auto join_count = [&](const std::set<std::string> &aliases) {
        int rows = 0;
        enumerate_records(aliases, [&](const auto &) { ++rows; });
        return rows;
    };

    auto prefix_records = [&](const std::set<std::string> &aliases) {
        std::vector<std::map<std::string, Rid>> records;
        enumerate_records(aliases, [&](const auto &current) { records.push_back(current); });
        return records;
    };

    auto find_index_cond = [&](const std::string &right_alias,
                               const std::vector<std::shared_ptr<ast::BinaryExpr>> &conds)
        -> std::shared_ptr<ast::BinaryExpr> {
        for (auto &cond : conds) {
            if (cond->op == ast::SV_OP_EQ && table_has_join_index(right_alias, cond)) {
                return cond;
            }
        }
        return nullptr;
    };

    auto index_hit_count = [&](const std::set<std::string> &outer_aliases, const std::string &right_alias,
                               const std::shared_ptr<ast::BinaryExpr> &index_cond) {
        if (index_cond == nullptr) {
            return 0;
        }
        auto rhs_col = std::dynamic_pointer_cast<ast::Col>(index_cond->rhs);
        std::shared_ptr<ast::Col> right_col;
        std::shared_ptr<ast::Col> outer_col;
        if (index_cond->lhs->tab_name == right_alias && rhs_col && outer_aliases.count(rhs_col->tab_name)) {
            right_col = index_cond->lhs;
            outer_col = rhs_col;
        } else if (rhs_col && rhs_col->tab_name == right_alias && outer_aliases.count(index_cond->lhs->tab_name)) {
            right_col = rhs_col;
            outer_col = index_cond->lhs;
        } else {
            return 0;
        }

        int rows = 0;
        auto prefixes = prefix_records(outer_aliases);
        auto right_fh = sm_manager->fhs_.at(alias_to_table[right_alias]).get();
        for (auto &prefix : prefixes) {
            auto [outer_meta, outer_raw] = read_col(outer_col->tab_name, prefix[outer_col->tab_name], outer_col->col_name);
            for (RmScan scan(right_fh); !scan.is_end(); scan.next()) {
                auto [right_meta, right_raw] = read_col(right_alias, scan.rid(), right_col->col_name);
                if (!compare_raw(right_meta, right_raw, outer_meta, outer_raw, ast::SV_OP_EQ)) {
                    continue;
                }
                bool pass_local = true;
                for (auto &cond : local_conds[right_alias]) {
                    if (!compare_filter_cond(right_alias, scan.rid(), cond)) {
                        pass_local = false;
                        break;
                    }
                }
                if (pass_local) {
                    rows++;
                }
            }
        }
        return rows;
    };

    std::vector<std::string> lines;
    std::set<std::string> all_aliases(input_visible.begin(), input_visible.end());
    int final_rows = input_visible.empty() ? 0 : join_count(all_aliases);
    int project_rows = select->has_limit && select->limit >= 0 ? std::min(final_rows, select->limit) : final_rows;
    if (select->select_items.empty()) {
        lines.push_back("Project(columns=[*], rows=" + std::to_string(project_rows) + ")");
    } else {
        std::vector<std::string> cols;
        for (auto &item : select->select_items) {
            if (!item->is_agg && item->col) {
                cols.push_back(col_to_string(item->col));
            }
        }
        lines.push_back("Project(columns=[" + project_columns_string(cols) + "], rows=" + std::to_string(project_rows) + ")");
    }

    std::set<std::string> joined_aliases;
    std::map<std::string, int> leaf_multiplier;
    std::map<std::string, int> leaf_output_rows;
    std::map<std::string, std::shared_ptr<ast::BinaryExpr>> leaf_index_cond;
    std::map<std::string, bool> leaf_index;
    std::map<std::string, int> leaf_depth;
    std::vector<std::string> leaf_order;
    std::vector<std::string> join_lines;
    if (!join_conds.empty()) {
        struct JoinStep {
            std::set<std::string> aliases;
            std::vector<std::shared_ptr<ast::BinaryExpr>> conds;
        };
        std::vector<JoinStep> join_steps;

        int table_count = static_cast<int>(input_visible.size());
        if (!input_visible.empty()) {
            const std::string &first_alias = input_visible.front();
            leaf_multiplier[first_alias] = 1;
            leaf_output_rows[first_alias] = filtered_rows(first_alias);
            leaf_index[first_alias] = false;
            leaf_depth[first_alias] = table_count;
            leaf_order.push_back(first_alias);
            joined_aliases.insert(first_alias);
        }

        for (size_t i = 1; i < input_visible.size(); ++i) {
            const std::string &right_alias = input_visible[i];
            std::vector<std::shared_ptr<ast::BinaryExpr>> step_conds;
            for (auto &cond : join_conds) {
                auto rhs_col = std::dynamic_pointer_cast<ast::Col>(cond->rhs);
                if (!rhs_col) {
                    continue;
                }
                bool lhs_right = cond->lhs->tab_name == right_alias && joined_aliases.count(rhs_col->tab_name);
                bool rhs_right = rhs_col->tab_name == right_alias && joined_aliases.count(cond->lhs->tab_name);
                if (lhs_right || rhs_right) {
                    step_conds.push_back(cond);
                }
            }
            int prefix_rows = join_count(joined_aliases);
            leaf_multiplier[right_alias] = prefix_rows;
            leaf_index_cond[right_alias] = find_index_cond(right_alias, step_conds);
            leaf_index[right_alias] = leaf_index_cond[right_alias] != nullptr;
            leaf_depth[right_alias] = table_count - static_cast<int>(i) + 1;
            if (leaf_index[right_alias]) {
                leaf_output_rows[right_alias] = index_hit_count(joined_aliases, right_alias, leaf_index_cond[right_alias]);
            } else {
                leaf_output_rows[right_alias] = filtered_rows(right_alias) * prefix_rows;
            }
            leaf_order.push_back(right_alias);
            joined_aliases.insert(right_alias);
            if (!step_conds.empty()) {
                join_steps.push_back({joined_aliases, step_conds});
            }
        }
        for (auto &step : join_steps) {
            std::set<std::string> joined_tables;
            for (auto &alias : step.aliases) {
                joined_tables.insert(alias_to_table[alias]);
            }
            std::vector<std::string> conds;
            for (auto &cond : step.conds) {
                conds.push_back(cond_to_string(cond));
            }
            std::sort(conds.begin(), conds.end());
            int join_rows = join_count(step.aliases);
            join_lines.push_back("Join(tables=[" + join_tables_string(joined_tables) + "], condition=[" +
                                 project_columns_string(conds) + "], rows=" + std::to_string(join_rows) + ")");
        }
        for (int i = static_cast<int>(join_lines.size()) - 1; i >= 0; --i) {
            int depth = static_cast<int>(join_lines.size()) - i;
            lines.push_back(std::string(depth, '\t') + join_lines[i]);
        }
    }

    bool has_filters = false;
    for (auto &visible : input_visible) {
        has_filters = has_filters || table_has_filter(explain_conds, visible);
    }
    if (join_conds.empty() && has_filters) {
        std::sort(leaf_order.begin(), leaf_order.end(), [&](const std::string &lhs, const std::string &rhs) {
            return alias_to_table[lhs] < alias_to_table[rhs];
        });
    }
    for (auto &visible : input_visible) {
        if (std::find(leaf_order.begin(), leaf_order.end(), visible) == leaf_order.end()) {
            leaf_order.push_back(visible);
        }
    }

    for (auto &visible : leaf_order) {
        std::vector<std::string> projected;
        if (!select->select_items.empty()) {
            for (auto &item : select->select_items) {
                if (!item->is_agg && item->col && item->col->tab_name == visible) {
                    projected.push_back(col_to_string(item->col));
                }
            }
            for (auto &cond : join_conds) {
                auto rhs_col = std::dynamic_pointer_cast<ast::Col>(cond->rhs);
                if (cond->lhs->tab_name == visible) {
                    projected.push_back(col_to_string(cond->lhs));
                }
                if (rhs_col->tab_name == visible) {
                    projected.push_back(col_to_string(rhs_col));
                }
            }
            std::sort(projected.begin(), projected.end());
            projected.erase(std::unique(projected.begin(), projected.end()), projected.end());
            if (!projected.empty() && !join_conds.empty()) {
                int base_rows = leaf_output_rows.count(visible) ? leaf_output_rows[visible] : filtered_rows(visible);
                lines.push_back(std::string(leaf_depth[visible], '\t') + "Project(columns=[" +
                                project_columns_string(projected) + "], rows=" + std::to_string(base_rows) + ")");
            }
        }
        std::string filter = table_filter_string(explain_conds, visible);
        if (!filter.empty()) {
            int filter_rows = filtered_rows(visible) * (join_conds.empty() ? 1 : leaf_multiplier[visible]);
            int filter_depth = join_conds.empty() ? 1 : leaf_depth[visible] + (projected.empty() ? 0 : 1);
            lines.push_back(std::string(filter_depth, '\t') + "Filter(condition=[" + filter +
                            "], rows=" + std::to_string(filter_rows) + ")");
        }
        int scan_rows = leaf_index[visible] ? leaf_output_rows[visible]
                                            : table_rows(visible) * (join_conds.empty() ? 1 : leaf_multiplier[visible]);
        std::string scan = "Scan(table=" + alias_to_table[visible] + ", type=" +
                           (leaf_index[visible] ? "IndexScan" : "SeqScan");
        if (leaf_index[visible]) {
            auto index_cond = leaf_index_cond[visible];
            auto rhs_col = std::dynamic_pointer_cast<ast::Col>(index_cond->rhs);
            if (index_cond->lhs->tab_name == visible) {
                scan += ", using_index=(" + index_cond->lhs->col_name + ")";
            } else if (rhs_col && rhs_col->tab_name == visible) {
                scan += ", using_index=(" + rhs_col->col_name + ")";
            }
        }
        scan += ", rows=" + std::to_string(scan_rows) + ")";
        int scan_depth = 0;
        if (join_conds.empty()) {
            scan_depth = filter.empty() ? 1 : 2;
        } else {
            scan_depth = leaf_depth[visible] + (projected.empty() ? 0 : 1) + (filter.empty() ? 0 : 1);
        }
        lines.push_back(std::string(scan_depth, '\t') + scan);
    }
    return lines;
}
}

bool Planner::get_index_cols(std::string tab_name, std::string visible_name, std::vector<Condition> curr_conds,
                             std::vector<std::string>& index_col_names,
                             const std::vector<TabCol> &required_cols,
                             bool allow_covering_full_scan) {
    index_col_names.clear();
    TabMeta& tab = sm_manager_->db_.get_table(tab_name);
    size_t best_matched_prefix = 0;
    size_t best_equality_prefix = 0;
    int best_priority = -1;
    bool best_covering = false;
    bool best_unique_point = false;
    bool best_has_access_predicate = false;
    std::vector<std::string> best_index_cols;
    for (auto &index : tab.indexes) {
        size_t matched_prefix = 0;
        size_t equality_prefix = 0;
        for (auto &index_col : index.cols) {
            auto cond_it = std::find_if(curr_conds.begin(), curr_conds.end(), [&](const Condition &cond) {
                return cond.is_rhs_val && cond.lhs_col.tab_name == visible_name &&
                       cond.lhs_col.col_name == index_col.name && cond.op != OP_NE;
            });
            if (cond_it == curr_conds.end()) {
                break;
            }
            matched_prefix++;
            if (cond_it->op != OP_EQ) {
                break;
            }
            equality_prefix++;
        }
        bool covering = (!required_cols.empty() || allow_covering_full_scan) &&
                        rmdb::index_covers_required_and_conditions(index, required_cols, curr_conds, visible_name);
        if (matched_prefix == 0 && !(allow_covering_full_scan && covering)) {
            continue;
        }
        bool unique_point = index.unique && equality_prefix == static_cast<size_t>(index.col_num);
        bool has_access_predicate = matched_prefix > 0;
        int priority = (index.hidden ? 0 : 2) + (index.unique ? 1 : 0);
        bool better = best_index_cols.empty();
        if (!better && unique_point != best_unique_point) {
            better = unique_point;
        } else if (!better && unique_point && best_unique_point && equality_prefix != best_equality_prefix) {
            better = equality_prefix > best_equality_prefix;
        } else if (!better && !unique_point && !best_unique_point &&
                   has_access_predicate != best_has_access_predicate) {
            better = has_access_predicate;
        } else if (!better && covering != best_covering) {
            better = covering;
        } else if (!better && equality_prefix != best_equality_prefix) {
            better = equality_prefix > best_equality_prefix;
        } else if (!better && matched_prefix != best_matched_prefix) {
            better = matched_prefix > best_matched_prefix;
        } else if (!better) {
            better = priority > best_priority;
        }
        if (better) {
            best_matched_prefix = matched_prefix;
            best_equality_prefix = equality_prefix;
            best_priority = priority;
            best_covering = covering;
            best_unique_point = unique_point;
            best_has_access_predicate = has_access_predicate;
            best_index_cols.clear();
            for (auto &col : index.cols) {
                best_index_cols.push_back(col.name);
            }
        }
    }
    if (best_index_cols.empty()) {
        return false;
    }
    index_col_names = std::move(best_index_cols);
    return true;
}

/**
 * @brief 表算子条件谓词生成
 *
 * @param conds 条件
 * @param tab_names 表名
 * @return std::vector<Condition>
 */
std::vector<Condition> pop_conds(std::vector<Condition> &conds, std::string tab_names) {
    // auto has_tab = [&](const std::string &tab_name) {
    //     return std::find(tab_names.begin(), tab_names.end(), tab_name) != tab_names.end();
    // };
    std::vector<Condition> solved_conds;
    auto it = conds.begin();
    while (it != conds.end()) {
        if ((tab_names.compare(it->lhs_col.tab_name) == 0 && it->is_rhs_val) || (it->lhs_col.tab_name.compare(it->rhs_col.tab_name) == 0)) {
            solved_conds.emplace_back(std::move(*it));
            it = conds.erase(it);
        } else {
            it++;
        }
    }
    return solved_conds;
}

int push_conds(Condition *cond, std::shared_ptr<Plan> plan)
{
    if(auto x = std::dynamic_pointer_cast<ScanPlan>(plan))
    {
        if(x->visible_name_.compare(cond->lhs_col.tab_name) == 0) {
            return 1;
        } else if(x->visible_name_.compare(cond->rhs_col.tab_name) == 0){
            return 2;
        } else {
            return 0;
        }
    }
    else if(auto x = std::dynamic_pointer_cast<JoinPlan>(plan))
    {
        int left_res = push_conds(cond, x->left_);
        // 条件已经下推到左子节点
        if(left_res == 3){
            return 3;
        }
        int right_res = push_conds(cond, x->right_);
        // 条件已经下推到右子节点
        if(right_res == 3){
            return 3;
        }
        // 左子节点或右子节点有一个没有匹配到条件的列
        if(left_res == 0 || right_res == 0) {
            return left_res + right_res;
        }
        // 左子节点匹配到条件的右边
        if(left_res == 2) {
            // 需要将左右两边的条件变换位置
            std::map<CompOp, CompOp> swap_op = {
                {OP_EQ, OP_EQ}, {OP_NE, OP_NE}, {OP_LT, OP_GT}, {OP_GT, OP_LT}, {OP_LE, OP_GE}, {OP_GE, OP_LE},
            };
            std::swap(cond->lhs_col, cond->rhs_col);
            cond->op = swap_op.at(cond->op);
        }
        x->conds_.emplace_back(std::move(*cond));
        return 3;
    }
    return false;
}

std::vector<std::string> get_join_index_cols(SmManager *sm_manager, const std::string &right_table,
                                             const std::string &right_visible,
                                             const std::vector<Condition> &join_conds,
                                             const std::vector<Condition> &right_conds) {
    auto &tab = sm_manager->db_.get_table(right_table);
    size_t best_prefix = 0;
    int best_priority = -1;
    std::vector<std::string> best_cols;
    for (const auto &index : tab.indexes) {
        size_t prefix = 0;
        bool uses_join_key = false;
        for (const auto &index_col : index.cols) {
            auto join_it = std::find_if(join_conds.begin(), join_conds.end(), [&](const Condition &cond) {
                if (cond.is_rhs_val || cond.op != OP_EQ) {
                    return false;
                }
                return (cond.rhs_col.tab_name == right_visible && cond.rhs_col.col_name == index_col.name) ||
                       (cond.lhs_col.tab_name == right_visible && cond.lhs_col.col_name == index_col.name);
            });
            if (join_it != join_conds.end()) {
                uses_join_key = true;
                prefix++;
                continue;
            }
            auto value_it = std::find_if(right_conds.begin(), right_conds.end(), [&](const Condition &cond) {
                return cond.is_rhs_val && cond.op == OP_EQ && cond.rhs_val.raw != nullptr &&
                       cond.lhs_col.tab_name == right_visible && cond.lhs_col.col_name == index_col.name;
            });
            if (value_it == right_conds.end()) {
                break;
            }
            prefix++;
        }
        int priority = (index.hidden ? 0 : 2) + (index.unique ? 1 : 0);
        if (uses_join_key &&
            (prefix > best_prefix || (prefix == best_prefix && prefix > 0 && priority > best_priority))) {
            best_prefix = prefix;
            best_priority = priority;
            best_cols.clear();
            for (const auto &col : index.cols) {
                best_cols.push_back(col.name);
            }
        }
    }
    return best_prefix == 0 ? std::vector<std::string>{} : best_cols;
}

bool find_plan_col(const std::shared_ptr<Plan> &plan, const TabCol &target, ColMeta *out) {
    if (plan == nullptr) {
        return false;
    }
    if (auto scan = std::dynamic_pointer_cast<ScanPlan>(plan)) {
        for (const auto &col : scan->cols_) {
            if (col.tab_name == target.tab_name && col.name == target.col_name) {
                *out = col;
                return true;
            }
        }
        return false;
    }
    if (auto join = std::dynamic_pointer_cast<JoinPlan>(plan)) {
        return find_plan_col(join->left_, target, out) || find_plan_col(join->right_, target, out);
    }
    if (auto projection = std::dynamic_pointer_cast<ProjectionPlan>(plan)) {
        return find_plan_col(projection->subplan_, target, out);
    }
    return false;
}

bool has_hash_join_key(const std::shared_ptr<Plan> &left, const std::shared_ptr<Plan> &right,
                       const std::vector<Condition> &join_conds) {
    for (const auto &cond : join_conds) {
        if (cond.is_rhs_val || cond.op != OP_EQ) {
            continue;
        }
        ColMeta left_col;
        ColMeta right_col;
        bool lhs_left = find_plan_col(left, cond.lhs_col, &left_col);
        bool rhs_right = find_plan_col(right, cond.rhs_col, &right_col);
        if (!lhs_left || !rhs_right) {
            bool rhs_left = find_plan_col(left, cond.rhs_col, &left_col);
            bool lhs_right = find_plan_col(right, cond.lhs_col, &right_col);
            if (!rhs_left || !lhs_right) {
                continue;
            }
        }
        if (left_col.type == right_col.type && left_col.len == right_col.len) {
            return true;
        }
    }
    return false;
}

enum class JoinAlgoPreference {
    kAuto,
    kNested,
    kHash,
    kMerge,
};

JoinAlgoPreference join_algo_preference() {
    const char *value = std::getenv("RMDB_JOIN_ALGO");
    if (value == nullptr) {
        return JoinAlgoPreference::kAuto;
    }
    std::string mode(value);
    std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (mode == "nested" || mode == "nlj") {
        return JoinAlgoPreference::kNested;
    }
    if (mode == "hash") {
        return JoinAlgoPreference::kHash;
    }
    if (mode == "merge" || mode == "sortmerge" || mode == "sort-merge" || mode == "smj") {
        return JoinAlgoPreference::kMerge;
    }
    return JoinAlgoPreference::kAuto;
}


std::shared_ptr<Query> Planner::logical_optimization(std::shared_ptr<Query> query, Context *context)
{
    
    //TODO 实现逻辑优化规则

    return query;
}

std::shared_ptr<Plan> Planner::physical_optimization(std::shared_ptr<Query> query, Context *context)
{
    std::shared_ptr<Plan> plan = make_one_rel(query);
    
    // 其他物理优化

    // 处理orderby
    plan = generate_sort_plan(query, std::move(plan)); 

    return plan;
}



std::shared_ptr<Plan> Planner::make_one_rel(std::shared_ptr<Query> query)
{
    std::vector<std::string> tables = query->tables;
    auto base_table = [&](const std::string &visible) -> const std::string & {
        auto it = query->alias_to_table.find(visible);
        return it == query->alias_to_table.end() ? visible : it->second;
    };
    propagate_equal_join_value_filters(query->conds);
    // // Scan table , 生成表算子列表tab_nodes
    std::vector<std::shared_ptr<Plan>> table_scan_executors(tables.size());
    for (size_t i = 0; i < tables.size(); i++) {
        auto curr_conds = pop_conds(query->conds, tables[i]);
        // int index_no = get_indexNo(tables[i], curr_conds);
        std::vector<std::string> index_col_names;
        const std::string &base = base_table(tables[i]);
        auto required_cols = scan_required_cols_for_index_choice(query, tables[i], curr_conds);
        bool index_exist = get_index_cols(base, tables[i], curr_conds, index_col_names, required_cols,
                                          is_simple_count_query(query));
        if (index_exist == false) {  // 该表没有索引
            index_col_names.clear();
            table_scan_executors[i] = 
                std::make_shared<ScanPlan>(T_SeqScan, sm_manager_, base, curr_conds, index_col_names, tables[i]);
        } else {  // 存在索引
            table_scan_executors[i] =
                std::make_shared<ScanPlan>(T_IndexScan, sm_manager_, base, curr_conds, index_col_names, tables[i]);
        }
    }
    // 只有一个表，不需要join。
    if(tables.size() == 1) {
        return table_scan_executors[0];
    }
    auto conds = std::move(query->conds);
    std::map<CompOp, CompOp> swap_op = {
        {OP_EQ, OP_EQ}, {OP_NE, OP_NE}, {OP_LT, OP_GT}, {OP_GT, OP_LT}, {OP_LE, OP_GE}, {OP_GE, OP_LE},
    };
    std::shared_ptr<Plan> table_join_executors = table_scan_executors[0];
    std::set<std::string> joined_tables{tables[0]};

    for (size_t i = 1; i < tables.size(); ++i) {
        const std::string &right_table = tables[i];
        std::vector<Condition> join_conds;
        auto it = conds.begin();
        while (it != conds.end()) {
            if (!it->is_rhs_val &&
                ((joined_tables.count(it->lhs_col.tab_name) && it->rhs_col.tab_name == right_table) ||
                 (it->lhs_col.tab_name == right_table && joined_tables.count(it->rhs_col.tab_name)))) {
                Condition cond = *it;
                if (cond.lhs_col.tab_name == right_table) {
                    std::swap(cond.lhs_col, cond.rhs_col);
                    cond.op = swap_op.at(cond.op);
                }
                join_conds.push_back(cond);
                it = conds.erase(it);
            } else {
                ++it;
            }
        }
        auto right_scan = std::dynamic_pointer_cast<ScanPlan>(table_scan_executors[i]);
        auto index_col_names = get_join_index_cols(sm_manager_, base_table(right_table), right_table, join_conds,
                                                   right_scan->conds_);
        PlanTag join_tag = T_NestLoop;
        auto preference = join_algo_preference();
        bool has_hash_key = has_hash_join_key(table_join_executors, table_scan_executors[i], join_conds);
        bool multi_table_left = std::dynamic_pointer_cast<JoinPlan>(table_join_executors) != nullptr;
        if (!index_col_names.empty() && preference != JoinAlgoPreference::kHash &&
            preference != JoinAlgoPreference::kMerge) {
            join_tag = T_IndexNestLoop;
        } else if (has_hash_key && preference == JoinAlgoPreference::kMerge) {
            join_tag = T_SortMerge;
        } else if (has_hash_key &&
                   (preference == JoinAlgoPreference::kHash ||
                    (preference == JoinAlgoPreference::kAuto && multi_table_left))) {
            join_tag = T_HashJoin;
        }
        table_join_executors = std::make_shared<JoinPlan>(join_tag, std::move(table_join_executors),
                                                          table_scan_executors[i], join_conds, index_col_names);
        joined_tables.insert(right_table);
    }

    for (auto &cond : conds) {
        push_conds(&cond, table_join_executors);
    }

    return table_join_executors;

}


std::shared_ptr<Plan> Planner::generate_sort_plan(std::shared_ptr<Query> query, std::shared_ptr<Plan> plan)
{
    if(query->order_cols.empty()) {
        return plan;
    }
    return std::make_shared<SortPlan>(T_Sort, std::move(plan), query->order_cols);
}

namespace {
std::shared_ptr<Plan> try_minmax_index_aggregate(SmManager *sm_manager,
                                                 const std::shared_ptr<Query> &query,
                                                 const std::shared_ptr<Plan> &scan_plan,
                                                 Context *context) {
    if (query == nullptr || context == nullptr || context->txn_ == nullptr ||
        context->txn_->get_isolation_level() != IsolationLevel::SNAPSHOT_ISOLATION) {
        return nullptr;
    }
    if (!query->has_aggregate || query->has_group || !query->group_cols.empty() || !query->having_conds.empty() ||
        query->select_items.size() != 1 || query->cols.size() != 1 || query->tables.size() != 1 ||
        !query->order_cols.empty()) {
        return nullptr;
    }

    const auto &item = query->select_items[0];
    if (item == nullptr || !item->is_agg || item->count_star || item->col == nullptr ||
        (item->agg_type != ast::AGG_MIN && item->agg_type != ast::AGG_MAX)) {
        return nullptr;
    }

    auto scan = std::dynamic_pointer_cast<ScanPlan>(scan_plan);
    if (scan == nullptr || scan->tag != T_IndexScan || scan->index_col_names_.empty()) {
        return nullptr;
    }

    TabMeta &tab = sm_manager->db_.get_table(scan->tab_name_);
    IndexMeta index_meta;
    try {
        index_meta = *tab.get_index_meta(scan->index_col_names_, true);
    } catch (RMDBError &) {
        return nullptr;
    }
    if (index_meta.col_num <= 1) {
        return nullptr;
    }

    int prefix_cols = 0;
    for (const auto &index_col : index_meta.cols) {
        if (index_col.name == item->col->col_name) {
            break;
        }
        auto cond_it = std::find_if(scan->conds_.begin(), scan->conds_.end(), [&](const Condition &cond) {
            return cond.is_rhs_val && cond.op == OP_EQ && cond.rhs_val.raw != nullptr &&
                   cond.lhs_col.tab_name == scan->visible_name_ && cond.lhs_col.col_name == index_col.name;
        });
        if (cond_it == scan->conds_.end()) {
            return nullptr;
        }
        prefix_cols++;
    }
    if (prefix_cols <= 0 || prefix_cols >= index_meta.col_num ||
        index_meta.cols[prefix_cols].name != item->col->col_name) {
        return nullptr;
    }

    TabCol agg_col{scan->visible_name_, item->col->col_name};
    return std::make_shared<MinMaxIndexAggregatePlan>(scan->tab_name_, scan->visible_name_, scan->conds_,
                                                      scan->index_col_names_, agg_col, item->agg_type, query->cols);
}

std::shared_ptr<Plan> try_count_index_aggregate(SmManager *sm_manager,
                                                const std::shared_ptr<Query> &query,
                                                const std::shared_ptr<Plan> &scan_plan,
                                                Context *context) {
    if (query == nullptr || context == nullptr || context->txn_ == nullptr ||
        context->txn_->get_isolation_level() != IsolationLevel::SNAPSHOT_ISOLATION ||
        !is_simple_count_query(query)) {
        return nullptr;
    }

    auto scan = std::dynamic_pointer_cast<ScanPlan>(scan_plan);
    if (scan == nullptr || scan->tag != T_IndexScan || scan->index_col_names_.empty()) {
        return nullptr;
    }

    TabMeta &tab = sm_manager->db_.get_table(scan->tab_name_);
    IndexMeta index_meta;
    try {
        index_meta = *tab.get_index_meta(scan->index_col_names_, true);
    } catch (RMDBError &) {
        return nullptr;
    }

    const auto &item = query->select_items[0];
    std::vector<TabCol> required_cols;
    TabCol count_col;
    // RMDB has no NULL value semantics, and AggregateExecutor already treats
    // COUNT(col) as a per-row count.  The CountIndex fast path can therefore
    // plan COUNT(col) as COUNT(*) and avoid requiring the counted column to be
    // present in the chosen index.
    const bool count_all_rows = item->count_star || item->col != nullptr;
    if (!rmdb::index_covers_required_and_conditions(index_meta, required_cols, scan->conds_, scan->visible_name_)) {
        return nullptr;
    }

    return std::make_shared<CountIndexAggregatePlan>(scan->tab_name_, scan->visible_name_, scan->conds_,
                                                     scan->index_col_names_, count_all_rows, count_col,
                                                     query->cols);
}
}  // namespace

void Planner::apply_required_columns(const std::shared_ptr<Plan> &plan, const std::vector<TabCol> &required) {
    auto add_unique = [](std::vector<TabCol> *cols, const TabCol &col) {
        if (col.col_name.empty()) {
            return;
        }
        auto it = std::find_if(cols->begin(), cols->end(), [&](const TabCol &existing) {
            return existing.tab_name == col.tab_name && existing.col_name == col.col_name;
        });
        if (it == cols->end()) {
            cols->push_back(col);
        }
    };
    auto add_condition_cols = [&](std::vector<TabCol> *cols, const std::vector<Condition> &conds) {
        for (const auto &cond : conds) {
            add_unique(cols, cond.lhs_col);
            if (!cond.is_rhs_val) {
                add_unique(cols, cond.rhs_col);
            }
        }
    };
    auto add_agg_item_col = [&](std::vector<TabCol> *cols, const std::shared_ptr<ast::SelectItem> &item) {
        if (item != nullptr && item->is_agg && !item->count_star && item->col != nullptr) {
            add_unique(cols, {item->col->tab_name, item->col->col_name});
        }
    };

    if (plan == nullptr) {
        return;
    }
    if (auto scan = std::dynamic_pointer_cast<ScanPlan>(plan)) {
        std::vector<TabCol> scan_required = required;
        add_condition_cols(&scan_required, scan->conds_);
        scan->required_cols_.clear();
        for (const auto &col : scan_required) {
            if (col.tab_name == scan->visible_name_) {
                add_unique(&scan->required_cols_, col);
            }
        }
        return;
    }
    if (auto projection = std::dynamic_pointer_cast<ProjectionPlan>(plan)) {
        apply_required_columns(projection->subplan_, projection->sel_cols_);
        return;
    }
    if (auto sort = std::dynamic_pointer_cast<SortPlan>(plan)) {
        std::vector<TabCol> child_required = required;
        for (const auto &order_col : sort->order_cols_) {
            add_unique(&child_required, order_col.first);
        }
        if (sort->order_cols_.empty()) {
            add_unique(&child_required, sort->sel_col_);
        }
        apply_required_columns(sort->subplan_, child_required);
        return;
    }
    if (auto limit = std::dynamic_pointer_cast<LimitPlan>(plan)) {
        apply_required_columns(limit->subplan_, required);
        return;
    }
    if (auto aggregate = std::dynamic_pointer_cast<AggregatePlan>(plan)) {
        std::vector<TabCol> child_required;
        for (const auto &group_col : aggregate->group_cols_) {
            add_unique(&child_required, group_col);
        }
        for (const auto &item : aggregate->select_items_) {
            add_agg_item_col(&child_required, item);
        }
        for (const auto &having : aggregate->having_conds_) {
            add_agg_item_col(&child_required, having->lhs);
        }
        apply_required_columns(aggregate->subplan_, child_required);
        return;
    }
    if (auto join = std::dynamic_pointer_cast<JoinPlan>(plan)) {
        std::vector<TabCol> child_required = required;
        add_condition_cols(&child_required, join->conds_);
        apply_required_columns(join->left_, child_required);
        apply_required_columns(join->right_, child_required);
        return;
    }
    if (auto semi = std::dynamic_pointer_cast<SemiJoinPlan>(plan)) {
        std::vector<TabCol> child_required = required;
        add_condition_cols(&child_required, semi->conds_);
        apply_required_columns(semi->left_, child_required);
        apply_required_columns(semi->right_, child_required);
        return;
    }
    if (auto union_plan = std::dynamic_pointer_cast<UnionPlan>(plan)) {
        for (auto &subplan : union_plan->subplans_) {
            apply_required_columns(subplan, required);
        }
    }
}

void Planner::attach_runtime_cache(const std::shared_ptr<Plan> &plan) {
    if (plan == nullptr) {
        return;
    }

    auto build_cache = [&](const std::string &tab_name, const std::string &visible_name,
                           const std::vector<std::string> *index_cols) {
        auto cache = std::make_shared<PlanRuntimeCache>();
        TabMeta &tab = sm_manager_->db_.get_table(tab_name);
        cache->file_hdr = sm_manager_->fhs_.at(tab_name)->get_file_hdr();
        cache->full_cols = tab.cols;
        const std::string &effective_visible = visible_name.empty() ? tab_name : visible_name;
        for (auto &col : cache->full_cols) {
            col.tab_name = effective_visible;
        }
        cache->full_len = cache->full_cols.empty() ? 0 : cache->full_cols.back().offset + cache->full_cols.back().len;
        cache->has_table = true;
        if (index_cols != nullptr && !index_cols->empty()) {
            cache->index_meta = *tab.get_index_meta(*index_cols, true);
            cache->has_index = true;
        }
        return cache;
    };
    auto cache_projection = [](PlanRuntimeCache *cache, const std::vector<TabCol> &required_cols) {
        if (cache == nullptr || required_cols.empty()) {
            return;
        }
        cache->projected_cols.reserve(required_cols.size());
        cache->projected_source_cols.reserve(required_cols.size());
        for (const auto &required : required_cols) {
            const bool seen = std::any_of(cache->projected_source_cols.begin(), cache->projected_source_cols.end(),
                                          [&](const ColMeta &col) {
                                              return col.tab_name == required.tab_name && col.name == required.col_name;
                                          });
            if (seen) {
                continue;
            }
            auto source = std::find_if(cache->full_cols.begin(), cache->full_cols.end(), [&](const ColMeta &col) {
                return col.tab_name == required.tab_name && col.name == required.col_name;
            });
            if (source == cache->full_cols.end()) {
                throw ColumnNotFoundError(required.tab_name + '.' + required.col_name);
            }
            ColMeta output = *source;
            output.offset = static_cast<int>(cache->projected_len);
            cache->projected_len += static_cast<size_t>(output.len);
            cache->projected_cols.push_back(std::move(output));
            cache->projected_source_cols.push_back(*source);
        }
        cache->has_projection = !cache->projected_cols.empty();
    };

    if (auto dml = std::dynamic_pointer_cast<DMLPlan>(plan)) {
        attach_runtime_cache(dml->subplan_);
        if (!dml->tab_name_.empty() && (dml->tag == T_Insert || dml->tag == T_Update || dml->tag == T_Delete)) {
            auto cache = build_cache(dml->tab_name_, dml->tab_name_, nullptr);
            if (dml->tag == T_Update) {
                cache->mutation_program = rmdb::CompiledMutationProgram::BuildUpdate(
                    sm_manager_->db_.get_table(dml->tab_name_), dml->set_clauses_,
                    rmdb::sql_template_schema_epoch());
            }
            dml->runtime_cache_ = std::move(cache);
        }
        return;
    }
    if (auto scan = std::dynamic_pointer_cast<ScanPlan>(plan)) {
        auto cache = build_cache(scan->tab_name_, scan->visible_name_,
                                 scan->tag == T_IndexScan ? &scan->index_col_names_ : nullptr);
        if (scan->tag == T_IndexScan && cache->has_index) {
            cache->access_program = rmdb::CompiledAccessProgram::BuildIndex(
                cache->index_meta, scan->conds_, scan->visible_name_, rmdb::sql_template_schema_epoch());
            cache_projection(cache.get(), scan->required_cols_);
        }
        scan->runtime_cache_ = std::move(cache);
        return;
    }
    if (auto join = std::dynamic_pointer_cast<JoinPlan>(plan)) {
        attach_runtime_cache(join->left_);
        attach_runtime_cache(join->right_);
        if (join->tag == T_IndexNestLoop) {
            if (auto right_scan = std::dynamic_pointer_cast<ScanPlan>(join->right_)) {
                join->runtime_cache_ =
                    build_cache(right_scan->tab_name_, right_scan->visible_name_, &join->index_col_names_);
            }
        }
        return;
    }
    if (auto projection = std::dynamic_pointer_cast<ProjectionPlan>(plan)) {
        attach_runtime_cache(projection->subplan_);
        return;
    }
    if (auto sort = std::dynamic_pointer_cast<SortPlan>(plan)) {
        attach_runtime_cache(sort->subplan_);
        return;
    }
    if (auto aggregate = std::dynamic_pointer_cast<AggregatePlan>(plan)) {
        attach_runtime_cache(aggregate->subplan_);
        return;
    }
    if (auto limit = std::dynamic_pointer_cast<LimitPlan>(plan)) {
        attach_runtime_cache(limit->subplan_);
        return;
    }
    if (auto semi = std::dynamic_pointer_cast<SemiJoinPlan>(plan)) {
        attach_runtime_cache(semi->left_);
        attach_runtime_cache(semi->right_);
        return;
    }
    if (auto count = std::dynamic_pointer_cast<CountIndexAggregatePlan>(plan)) {
        auto cache = build_cache(count->tab_name_, count->visible_name_, &count->index_col_names_);
        cache->access_program = rmdb::CompiledAccessProgram::BuildIndex(
            cache->index_meta, count->conds_, count->visible_name_, rmdb::sql_template_schema_epoch());
        count->runtime_cache_ = std::move(cache);
        return;
    }
    if (auto minmax = std::dynamic_pointer_cast<MinMaxIndexAggregatePlan>(plan)) {
        auto cache = build_cache(minmax->tab_name_, minmax->visible_name_, &minmax->index_col_names_);
        cache->access_program = rmdb::CompiledAccessProgram::BuildIndex(
            cache->index_meta, minmax->conds_, minmax->visible_name_, rmdb::sql_template_schema_epoch());
        minmax->runtime_cache_ = std::move(cache);
        return;
    }
    if (auto union_plan = std::dynamic_pointer_cast<UnionPlan>(plan)) {
        for (auto &subplan : union_plan->subplans_) {
            attach_runtime_cache(subplan);
        }
    }
}


/**
 * @brief select plan 生成
 *
 * @param sel_cols select plan 选取的列
 * @param tab_names select plan 目标的表
 * @param conds select plan 选取条件
 */
std::shared_ptr<Plan> Planner::generate_select_plan(std::shared_ptr<Query> query, Context *context,
                                                    bool keep_root_projection) {
    //逻辑优化
    query = logical_optimization(std::move(query), context);

    //物理优化
    auto sel_cols = query->cols;
    std::shared_ptr<Plan> plannerRoot = physical_optimization(query, context);
    if (!query->has_aggregate && !query->has_group && query->has_limit && query->limit >= 0) {
        if (auto sort_plan = std::dynamic_pointer_cast<SortPlan>(plannerRoot)) {
            sort_plan->limit_ = query->limit;
        }
    }
    if (query->has_aggregate || query->has_group) {
        auto minmax_plan = try_minmax_index_aggregate(sm_manager_, query, plannerRoot, context);
        if (minmax_plan != nullptr) {
            plannerRoot = std::move(minmax_plan);
        } else {
            auto count_plan = try_count_index_aggregate(sm_manager_, query, plannerRoot, context);
            if (count_plan != nullptr) {
                plannerRoot = std::move(count_plan);
            } else {
                plannerRoot = std::make_shared<AggregatePlan>(std::move(plannerRoot), query->select_items,
                                                              query->group_cols, query->having_conds, sel_cols);
            }
        }
    } else if (keep_root_projection) {
        plannerRoot = std::make_shared<ProjectionPlan>(T_Projection, std::move(plannerRoot), std::move(sel_cols));
    }
    if (query->has_limit) {
        plannerRoot = std::make_shared<LimitPlan>(std::move(plannerRoot), query->limit);
    }
    apply_required_columns(plannerRoot, sel_cols);

    return plannerRoot;
}

// 生成DDL语句和DML语句的查询执行计划
std::shared_ptr<Plan> Planner::do_planner(std::shared_ptr<Query> query, Context *context)
{
    auto output_cols_with_aliases = [](const std::shared_ptr<Query> &select_query) {
        auto output_cols = select_query->cols;
        if (output_cols.size() == select_query->select_items.size()) {
            for (size_t i = 0; i < output_cols.size(); ++i) {
                if (!select_query->select_items[i]->alias.empty()) {
                    output_cols[i].output_name = select_query->select_items[i]->alias;
                }
            }
        }
        return output_cols;
    };
    std::shared_ptr<Plan> plannerRoot;
    auto make_dml_scan = [&](const std::string &table_name, const std::vector<Condition> &conditions) {
        std::vector<std::string> index_col_names;
        const bool has_index = get_index_cols(table_name, table_name, conditions, index_col_names);
        return std::make_shared<ScanPlan>(has_index ? T_IndexScan : T_SeqScan, sm_manager_, table_name,
                                          conditions, index_col_names);
    };
    auto make_union_select_plan = [&](const std::shared_ptr<Query> &select_query) {
        std::vector<std::shared_ptr<Plan>> subplans;
        subplans.reserve(select_query->union_queries.size());
        for (const auto &subquery : select_query->union_queries) {
            subplans.push_back(generate_select_plan(subquery, context, true));
        }
        std::shared_ptr<Plan> union_plan =
            std::make_shared<UnionPlan>(std::move(subplans), select_query->union_cols);
        if (!select_query->order_cols.empty()) {
            union_plan = std::make_shared<SortPlan>(T_Sort, std::move(union_plan), select_query->order_cols);
        }
        auto dml_plan = std::make_shared<DMLPlan>(T_select, std::move(union_plan), std::string(),
                                                  std::vector<Value>(), std::vector<Condition>(),
                                                  std::vector<SetClause>());
        dml_plan->output_cols_ = output_cols_with_aliases(select_query);
        return dml_plan;
    };
    if (query->parse == nullptr && query->kind == StmtKind::Insert) {
        plannerRoot = std::make_shared<DMLPlan>(T_Insert, std::shared_ptr<Plan>(), query->target_table,
                                                query->values, std::vector<Condition>(), std::vector<SetClause>());
    } else if (query->parse == nullptr && query->kind == StmtKind::Load) {
        plannerRoot = std::make_shared<DMLPlan>(T_Load, query->target_table, query->load_file);
    } else if (query->parse == nullptr && query->kind == StmtKind::Delete) {
        auto table_scan_executors = make_dml_scan(query->target_table, query->conds);
        plannerRoot = std::make_shared<DMLPlan>(T_Delete, table_scan_executors, query->target_table,
                                                std::vector<Value>(), query->conds, std::vector<SetClause>());
    } else if (query->parse == nullptr && query->kind == StmtKind::Update) {
        auto table_scan_executors = make_dml_scan(query->target_table, query->conds);
        plannerRoot = std::make_shared<DMLPlan>(T_Update, table_scan_executors, query->target_table,
                                                std::vector<Value>(), query->conds, query->set_clauses);
    } else if (query->parse == nullptr && query->kind == StmtKind::Select) {
        if (!query->union_queries.empty()) {
            plannerRoot = make_union_select_plan(query);
            attach_runtime_cache(plannerRoot);
            return plannerRoot;
        }

        std::shared_ptr<Plan> projection;
        auto final_output_cols = output_cols_with_aliases(query);
        if (query->is_semi_join) {
            std::vector<std::string> empty_index;
            auto base_table = [&](const std::string &visible) -> const std::string & {
                auto it = query->alias_to_table.find(visible);
                return it == query->alias_to_table.end() ? visible : it->second;
            };
            auto left_scan = std::make_shared<ScanPlan>(T_SeqScan, sm_manager_, base_table(query->tables[0]),
                                                        std::vector<Condition>(), empty_index, query->tables[0]);
            auto right_scan = std::make_shared<ScanPlan>(T_SeqScan, sm_manager_, base_table(query->tables[1]),
                                                         std::vector<Condition>(), empty_index, query->tables[1]);
            auto semi = std::make_shared<SemiJoinPlan>(left_scan, right_scan, query->semi_conds);
            projection = std::make_shared<ProjectionPlan>(T_Projection, semi, query->cols);
        } else {
            projection = generate_select_plan(std::move(query), context);
        }
        plannerRoot = std::make_shared<DMLPlan>(T_select, projection, std::string(), std::vector<Value>(),
                                                std::vector<Condition>(), std::vector<SetClause>());
        std::dynamic_pointer_cast<DMLPlan>(plannerRoot)->output_cols_ = std::move(final_output_cols);
    } else if (auto x = std::dynamic_pointer_cast<ast::CreateTable>(query->parse)) {
        // create table;
        std::vector<ColDef> col_defs;
        for (auto &field : x->fields) {
            if (auto sv_col_def = std::dynamic_pointer_cast<ast::ColDef>(field)) {
                ColDef col_def = {.name = sv_col_def->col_name,
                                  .type = interp_sv_type(sv_col_def->type_len->type),
                                  .len = sv_col_def->type_len->len};
                col_defs.push_back(col_def);
            } else {
                throw InternalError("Unexpected field type");
            }
        }
        plannerRoot = std::make_shared<DDLPlan>(T_CreateTable, x->tab_name, std::vector<std::string>(), col_defs);
    } else if (auto x = std::dynamic_pointer_cast<ast::DropTable>(query->parse)) {
        // drop table;
        plannerRoot = std::make_shared<DDLPlan>(T_DropTable, x->tab_name, std::vector<std::string>(), std::vector<ColDef>());
    } else if (auto x = std::dynamic_pointer_cast<ast::CreateIndex>(query->parse)) {
        // create index;
        plannerRoot = std::make_shared<DDLPlan>(T_CreateIndex, x->tab_name, x->col_names, std::vector<ColDef>());
    } else if (auto x = std::dynamic_pointer_cast<ast::DropIndex>(query->parse)) {
        // drop index
        plannerRoot = std::make_shared<DDLPlan>(T_DropIndex, x->tab_name, x->col_names, std::vector<ColDef>());
    } else if (auto x = std::dynamic_pointer_cast<ast::InsertStmt>(query->parse)) {
        // insert;
        plannerRoot = std::make_shared<DMLPlan>(T_Insert, std::shared_ptr<Plan>(),  x->tab_name,
                                                    query->values, std::vector<Condition>(), std::vector<SetClause>());
    } else if (auto x = std::dynamic_pointer_cast<ast::LoadStmt>(query->parse)) {
        plannerRoot = std::make_shared<DMLPlan>(T_Load, x->tab_name, query->load_file);
    } else if (auto x = std::dynamic_pointer_cast<ast::DeleteStmt>(query->parse)) {
        // delete;
        auto table_scan_executors = make_dml_scan(x->tab_name, query->conds);
        plannerRoot = std::make_shared<DMLPlan>(T_Delete, table_scan_executors, x->tab_name,
                                                std::vector<Value>(), query->conds, std::vector<SetClause>());
    } else if (auto x = std::dynamic_pointer_cast<ast::UpdateStmt>(query->parse)) {
        // update;
        auto table_scan_executors = make_dml_scan(x->tab_name, query->conds);
        plannerRoot = std::make_shared<DMLPlan>(T_Update, table_scan_executors, x->tab_name,
                                                     std::vector<Value>(), query->conds,
                                                     query->set_clauses);
    } else if (auto x = std::dynamic_pointer_cast<ast::ExplainStmt>(query->parse)) {
        plannerRoot = std::make_shared<ExplainPlan>(make_explain_lines(x->select, sm_manager_));
    } else if (std::dynamic_pointer_cast<ast::UnionStmt>(query->parse)) {
        plannerRoot = make_union_select_plan(query);
    } else if (std::dynamic_pointer_cast<ast::SelectStmt>(query->parse)) {
        if (!query->union_queries.empty()) {
            plannerRoot = make_union_select_plan(query);
            attach_runtime_cache(plannerRoot);
            return plannerRoot;
        }

        // 生成select语句的查询执行计划
        std::shared_ptr<Plan> projection;
        auto final_output_cols = output_cols_with_aliases(query);
        if (query->is_semi_join) {
            std::vector<std::string> empty_index;
            auto base_table = [&](const std::string &visible) -> const std::string & {
                auto it = query->alias_to_table.find(visible);
                return it == query->alias_to_table.end() ? visible : it->second;
            };
            auto left_scan = std::make_shared<ScanPlan>(T_SeqScan, sm_manager_, base_table(query->tables[0]),
                                                        std::vector<Condition>(), empty_index, query->tables[0]);
            auto right_scan = std::make_shared<ScanPlan>(T_SeqScan, sm_manager_, base_table(query->tables[1]),
                                                         std::vector<Condition>(), empty_index, query->tables[1]);
            auto semi = std::make_shared<SemiJoinPlan>(left_scan, right_scan, query->semi_conds);
            projection = std::make_shared<ProjectionPlan>(T_Projection, semi, query->cols);
        } else {
            projection = generate_select_plan(std::move(query), context);
        }
        plannerRoot = std::make_shared<DMLPlan>(T_select, projection, std::string(), std::vector<Value>(),
                                                    std::vector<Condition>(), std::vector<SetClause>());
        std::dynamic_pointer_cast<DMLPlan>(plannerRoot)->output_cols_ = std::move(final_output_cols);
    } else if (auto x = std::dynamic_pointer_cast<ast::ShowIndex>(query->parse)) {
        plannerRoot = std::make_shared<OtherPlan>(T_ShowIndex, x->tab_name);
    } else {
        throw InternalError("Unexpected AST root");
    }
    attach_runtime_cache(plannerRoot);
    return plannerRoot;
}
