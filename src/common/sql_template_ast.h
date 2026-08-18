#pragma once

#include <algorithm>
#include <memory>
#include <vector>

#include "common/sql_template_cache.h"
#include "parser/ast.h"

namespace rmdb::sql_template_detail {

enum class SlotKind { kValue, kLimitInt };

struct LiteralSlot {
    SlotKind kind = SlotKind::kValue;
    SqlTemplateLiteralType type = SqlTemplateLiteralType::kInt;
    int numeric_sign = 1;
};

inline void collect_value_slot(const std::shared_ptr<ast::Value> &value, std::vector<LiteralSlot> *slots) {
    if (auto int_lit = std::dynamic_pointer_cast<ast::IntLit>(value)) {
        LiteralSlot slot;
        slot.type = SqlTemplateLiteralType::kInt;
        slot.numeric_sign = int_lit->val < 0 ? -1 : 1;
        slots->push_back(slot);
    } else if (auto float_lit = std::dynamic_pointer_cast<ast::FloatLit>(value)) {
        LiteralSlot slot;
        slot.type = SqlTemplateLiteralType::kFloat;
        slot.numeric_sign = float_lit->val < 0.0f ? -1 : 1;
        slots->push_back(slot);
    } else if (std::dynamic_pointer_cast<ast::StringLit>(value)) {
        LiteralSlot slot;
        slot.type = SqlTemplateLiteralType::kString;
        slots->push_back(slot);
    }
}

inline bool has_bool_value(const std::shared_ptr<ast::Value> &value) {
    return std::dynamic_pointer_cast<ast::BoolLit>(value) != nullptr;
}

inline void collect_expr_slots(const std::shared_ptr<ast::Expr> &expr, std::vector<LiteralSlot> *slots) {
    if (auto value = std::dynamic_pointer_cast<ast::Value>(expr)) {
        collect_value_slot(value, slots);
    }
}

inline void collect_binary_slots(const std::vector<std::shared_ptr<ast::BinaryExpr>> &conds,
                                 std::vector<LiteralSlot> *slots) {
    for (const auto &cond : conds) {
        collect_expr_slots(cond->rhs, slots);
    }
}

inline void collect_having_slots(const std::vector<std::shared_ptr<ast::HavingExpr>> &conds,
                                 std::vector<LiteralSlot> *slots) {
    for (const auto &cond : conds) {
        collect_value_slot(cond->rhs, slots);
    }
}

inline void collect_select_slots(const std::shared_ptr<ast::SelectStmt> &select,
                                 std::vector<LiteralSlot> *slots) {
    collect_binary_slots(select->conds, slots);
    collect_having_slots(select->having_conds, slots);
    collect_binary_slots(select->semi_conds, slots);
    if (select->has_limit) {
        LiteralSlot slot;
        slot.kind = SlotKind::kLimitInt;
        slot.type = SqlTemplateLiteralType::kInt;
        slot.numeric_sign = select->limit < 0 ? -1 : 1;
        slots->push_back(slot);
    }
}

inline bool value_supported(const std::shared_ptr<ast::Value> &value) {
    return value != nullptr && !has_bool_value(value);
}

inline bool expr_supported(const std::shared_ptr<ast::Expr> &expr) {
    if (auto value = std::dynamic_pointer_cast<ast::Value>(expr)) {
        return value_supported(value);
    }
    return std::dynamic_pointer_cast<ast::Col>(expr) != nullptr;
}

inline bool binary_supported(const std::vector<std::shared_ptr<ast::BinaryExpr>> &conds) {
    for (const auto &cond : conds) {
        if (cond == nullptr || cond->lhs == nullptr || !expr_supported(cond->rhs)) {
            return false;
        }
    }
    return true;
}

inline bool having_supported(const std::vector<std::shared_ptr<ast::HavingExpr>> &conds) {
    for (const auto &cond : conds) {
        if (cond == nullptr || cond->lhs == nullptr || !value_supported(cond->rhs)) {
            return false;
        }
    }
    return true;
}

inline bool select_supported(const std::shared_ptr<ast::SelectStmt> &select) {
    if (select == nullptr || !binary_supported(select->conds) || !binary_supported(select->semi_conds) ||
        !having_supported(select->having_conds)) {
        return false;
    }
    for (const auto &ref : select->table_refs) {
        if (ref == nullptr || !ref->union_selects.empty()) {
            return false;
        }
    }
    return true;
}

inline bool root_supported(const std::shared_ptr<ast::TreeNode> &root) {
    if (auto insert = std::dynamic_pointer_cast<ast::InsertStmt>(root)) {
        return std::all_of(insert->vals.begin(), insert->vals.end(), value_supported);
    }
    if (auto update = std::dynamic_pointer_cast<ast::UpdateStmt>(root)) {
        if (!binary_supported(update->conds)) {
            return false;
        }
        for (const auto &set_clause : update->set_clauses) {
            if (set_clause == nullptr || (!set_clause->rhs_is_col && !value_supported(set_clause->val)) ||
                (set_clause->rhs_is_col && set_clause->op != ast::SET_OP_ASSIGN && !value_supported(set_clause->val))) {
                return false;
            }
        }
        return true;
    }
    if (auto del = std::dynamic_pointer_cast<ast::DeleteStmt>(root)) {
        return binary_supported(del->conds);
    }
    if (auto select = std::dynamic_pointer_cast<ast::SelectStmt>(root)) {
        return select_supported(select);
    }
    return false;
}

inline void collect_root_slots(const std::shared_ptr<ast::TreeNode> &root, std::vector<LiteralSlot> *slots) {
    if (auto insert = std::dynamic_pointer_cast<ast::InsertStmt>(root)) {
        for (const auto &value : insert->vals) {
            collect_value_slot(value, slots);
        }
    } else if (auto update = std::dynamic_pointer_cast<ast::UpdateStmt>(root)) {
        for (const auto &set_clause : update->set_clauses) {
            if (!set_clause->rhs_is_col || set_clause->op != ast::SET_OP_ASSIGN) {
                collect_value_slot(set_clause->val, slots);
            }
        }
        collect_binary_slots(update->conds, slots);
    } else if (auto del = std::dynamic_pointer_cast<ast::DeleteStmt>(root)) {
        collect_binary_slots(del->conds, slots);
    } else if (auto select = std::dynamic_pointer_cast<ast::SelectStmt>(root)) {
        collect_select_slots(select, slots);
    }
}

inline bool validate_slots(const std::vector<LiteralSlot> &slots, const SqlTemplateLiteralList &literals) {
    if (slots.size() != literals.size()) {
        return false;
    }
    for (size_t i = 0; i < slots.size(); ++i) {
        if (slots[i].type != literals[i].type) {
            return false;
        }
    }
    return true;
}

inline std::shared_ptr<ast::Col> clone_col(const std::shared_ptr<ast::Col> &col) {
    if (col == nullptr) {
        return nullptr;
    }
    return std::make_shared<ast::Col>(col->tab_name, col->col_name);
}

inline std::shared_ptr<ast::Value> clone_value(const std::shared_ptr<ast::Value> &value) {
    if (value == nullptr) {
        return nullptr;
    }
    if (auto int_lit = std::dynamic_pointer_cast<ast::IntLit>(value)) {
        return std::make_shared<ast::IntLit>(int_lit->val);
    }
    if (auto float_lit = std::dynamic_pointer_cast<ast::FloatLit>(value)) {
        return std::make_shared<ast::FloatLit>(float_lit->val);
    }
    if (auto str_lit = std::dynamic_pointer_cast<ast::StringLit>(value)) {
        return std::make_shared<ast::StringLit>(str_lit->val);
    }
    if (auto bool_lit = std::dynamic_pointer_cast<ast::BoolLit>(value)) {
        return std::make_shared<ast::BoolLit>(bool_lit->val);
    }
    return nullptr;
}

inline std::shared_ptr<ast::SelectItem> clone_select_item(const std::shared_ptr<ast::SelectItem> &item) {
    if (item == nullptr) {
        return nullptr;
    }
    if (item->is_agg) {
        return std::make_shared<ast::SelectItem>(item->agg_type, clone_col(item->col), item->count_star, item->alias,
                                                 item->distinct);
    }
    return std::make_shared<ast::SelectItem>(clone_col(item->col), item->alias);
}

inline std::shared_ptr<ast::HavingExpr> clone_having_expr(const std::shared_ptr<ast::HavingExpr> &expr) {
    return std::make_shared<ast::HavingExpr>(clone_select_item(expr->lhs), expr->op, clone_value(expr->rhs));
}

inline std::vector<std::shared_ptr<ast::HavingExpr>> clone_having_exprs(
    const std::vector<std::shared_ptr<ast::HavingExpr>> &exprs) {
    std::vector<std::shared_ptr<ast::HavingExpr>> result;
    result.reserve(exprs.size());
    for (const auto &expr : exprs) {
        result.push_back(clone_having_expr(expr));
    }
    return result;
}

inline std::vector<std::shared_ptr<ast::TableRef>> clone_table_refs(
    const std::vector<std::shared_ptr<ast::TableRef>> &refs) {
    std::vector<std::shared_ptr<ast::TableRef>> result;
    result.reserve(refs.size());
    for (const auto &ref : refs) {
        result.push_back(std::make_shared<ast::TableRef>(ref->tab_name, ref->alias));
    }
    return result;
}

}  // namespace rmdb::sql_template_detail
