#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "analyze/analyze.h"
#include "common/sql_template_ast.h"
#include "common/types.h"

namespace rmdb {

namespace query_template_detail {

enum class QueryLiteralTarget {
    kQueryValue,
    kUpdateSetValue,
    kWhereCondValue,
    kSemiCondValue,
    kHavingValue,
    kLimitValue,
};

struct QueryLiteralSlot {
    QueryLiteralTarget target = QueryLiteralTarget::kQueryValue;
    rmdb::u32 index = 0;
    sql_template_detail::LiteralSlot parsed_slot;
    ColType materialized_type = TYPE_INT;
    int raw_len = 0;
    bool init_raw = false;
};

struct QueryTemplate {
    std::shared_ptr<Query> skeleton;
    std::vector<QueryLiteralSlot> slots;
};

inline Value clone_value(const Value &src) {
    Value dst;
    if (src.type == TYPE_INT) {
        dst.set_int(src.int_val);
    } else if (src.type == TYPE_FLOAT) {
        dst.set_float(src.float_val);
    } else {
        dst.set_str(src.str_val);
    }
    if (src.raw != nullptr) {
        dst.raw = std::make_shared<RmRecord>(*src.raw);
    }
    return dst;
}

inline Condition clone_condition(const Condition &src) {
    Condition dst = src;
    if (src.is_rhs_val) {
        dst.rhs_val = clone_value(src.rhs_val);
    }
    return dst;
}

inline SetClause clone_set_clause(const SetClause &src) {
    SetClause dst = src;
    if (src.rhs_has_val) {
        dst.rhs = clone_value(src.rhs);
    }
    return dst;
}

inline std::shared_ptr<Query> clone_query_skeleton(const std::shared_ptr<Query> &src) {
    auto dst = std::make_shared<Query>();
    dst->kind = src->kind;
    dst->parse = nullptr;
    dst->target_table = src->target_table;
    dst->cols = src->cols;
    dst->tables = src->tables;
    dst->load_file = src->load_file;
    dst->group_cols = src->group_cols;
    dst->order_cols = src->order_cols;
    dst->has_aggregate = src->has_aggregate;
    dst->has_group = src->has_group;
    dst->has_limit = src->has_limit;
    dst->limit = src->limit;
    dst->is_semi_join = src->is_semi_join;
    dst->alias_to_table = src->alias_to_table;
    dst->union_cols = src->union_cols;

    dst->conds.reserve(src->conds.size());
    for (const auto &cond : src->conds) {
        dst->conds.push_back(clone_condition(cond));
    }
    dst->semi_conds.reserve(src->semi_conds.size());
    for (const auto &cond : src->semi_conds) {
        dst->semi_conds.push_back(clone_condition(cond));
    }
    dst->set_clauses.reserve(src->set_clauses.size());
    for (const auto &set_clause : src->set_clauses) {
        dst->set_clauses.push_back(clone_set_clause(set_clause));
    }
    dst->values.reserve(src->values.size());
    for (const auto &value : src->values) {
        dst->values.push_back(clone_value(value));
    }
    dst->select_items.reserve(src->select_items.size());
    for (const auto &item : src->select_items) {
        dst->select_items.push_back(sql_template_detail::clone_select_item(item));
    }
    dst->having_conds = sql_template_detail::clone_having_exprs(src->having_conds);
    dst->table_refs = sql_template_detail::clone_table_refs(src->table_refs);
    return dst;
}

inline bool fill_value_from_literal(Value *dst, const QueryLiteralSlot &slot, const SqlTemplateLiteral &literal,
                                    Context *context = nullptr) {
    if (slot.parsed_slot.type != literal.type) {
        return false;
    }
    Value value;
    if (literal.type == SqlTemplateLiteralType::kInt) {
        int int_value = literal.int_val * slot.parsed_slot.numeric_sign;
        if (slot.materialized_type == TYPE_FLOAT) {
            value.set_float(static_cast<float>(int_value));
        } else {
            value.set_int(int_value);
        }
    } else if (literal.type == SqlTemplateLiteralType::kFloat) {
        float float_value = literal.float_val * static_cast<float>(slot.parsed_slot.numeric_sign);
        value.set_float(float_value);
    } else {
        value.set_str(std::string(literal.str_val));
    }
    if (slot.init_raw) {
        if (context == nullptr) {
            value.init_raw(slot.raw_len);
        } else {
            value.init_raw(slot.raw_len, context->acquire_template_raw_record(slot.raw_len));
        }
    }
    *dst = std::move(value);
    return true;
}

inline std::shared_ptr<ast::Value> ast_value_from_literal(const QueryLiteralSlot &slot,
                                                          const SqlTemplateLiteral &literal) {
    if (slot.parsed_slot.type != literal.type) {
        return nullptr;
    }
    if (literal.type == SqlTemplateLiteralType::kInt) {
        return std::make_shared<ast::IntLit>(literal.int_val * slot.parsed_slot.numeric_sign);
    }
    if (literal.type == SqlTemplateLiteralType::kFloat) {
        return std::make_shared<ast::FloatLit>(literal.float_val * static_cast<float>(slot.parsed_slot.numeric_sign));
    }
    return std::make_shared<ast::StringLit>(std::string(literal.str_val));
}

class SlotBuilder {
   public:
    explicit SlotBuilder(const std::vector<sql_template_detail::LiteralSlot> &parsed_slots)
        : parsed_slots_(parsed_slots) {}

    bool add_value(QueryLiteralTarget target, size_t index, const Value &value) {
        if (next_ >= parsed_slots_.size()) {
            return false;
        }
        QueryLiteralSlot slot;
        slot.target = target;
        slot.index = static_cast<rmdb::u32>(index);
        slot.parsed_slot = parsed_slots_[next_++];
        slot.materialized_type = value.type;
        slot.init_raw = value.raw != nullptr;
        slot.raw_len = value.raw == nullptr ? 0 : value.raw->size;
        slots_.push_back(slot);
        return true;
    }

    bool add_having(size_t index) {
        if (next_ >= parsed_slots_.size()) {
            return false;
        }
        QueryLiteralSlot slot;
        slot.target = QueryLiteralTarget::kHavingValue;
        slot.index = static_cast<rmdb::u32>(index);
        slot.parsed_slot = parsed_slots_[next_++];
        slots_.push_back(slot);
        return true;
    }

    bool add_limit() {
        if (next_ >= parsed_slots_.size()) {
            return false;
        }
        QueryLiteralSlot slot;
        slot.target = QueryLiteralTarget::kLimitValue;
        slot.parsed_slot = parsed_slots_[next_++];
        slots_.push_back(slot);
        return true;
    }

    bool done() const { return next_ == parsed_slots_.size(); }
    std::vector<QueryLiteralSlot> take_slots() { return std::move(slots_); }

   private:
    const std::vector<sql_template_detail::LiteralSlot> &parsed_slots_;
    size_t next_ = 0;
    std::vector<QueryLiteralSlot> slots_;
};

inline bool add_condition_slots(SlotBuilder *builder, QueryLiteralTarget target,
                                const std::vector<Condition> &conds) {
    for (size_t i = 0; i < conds.size(); ++i) {
        if (!conds[i].is_rhs_val) {
            continue;
        }
        if (!builder->add_value(target, i, conds[i].rhs_val)) {
            return false;
        }
    }
    return true;
}

inline std::shared_ptr<QueryTemplate> make_template(const SqlTemplateCandidate &candidate,
                                                    const std::shared_ptr<ast::TreeNode> &root,
                                                    const std::shared_ptr<Query> &query) {
    if (!sql_template_detail::root_supported(root)) {
        return nullptr;
    }
    std::vector<sql_template_detail::LiteralSlot> parsed_slots;
    sql_template_detail::collect_root_slots(root, &parsed_slots);
    if (!sql_template_detail::validate_slots(parsed_slots, candidate.literals)) {
        return nullptr;
    }

    SlotBuilder builder(parsed_slots);
    if (query->kind == StmtKind::Insert) {
        for (size_t i = 0; i < query->values.size(); ++i) {
            if (!builder.add_value(QueryLiteralTarget::kQueryValue, i, query->values[i])) {
                return nullptr;
            }
        }
    } else if (query->kind == StmtKind::Update) {
        for (size_t i = 0; i < query->set_clauses.size(); ++i) {
            if (query->set_clauses[i].rhs_has_val &&
                !builder.add_value(QueryLiteralTarget::kUpdateSetValue, i, query->set_clauses[i].rhs)) {
                return nullptr;
            }
        }
        if (!add_condition_slots(&builder, QueryLiteralTarget::kWhereCondValue, query->conds)) {
            return nullptr;
        }
    } else if (query->kind == StmtKind::Delete) {
        if (!add_condition_slots(&builder, QueryLiteralTarget::kWhereCondValue, query->conds)) {
            return nullptr;
        }
    } else if (query->kind == StmtKind::Select) {
        if (!add_condition_slots(&builder, QueryLiteralTarget::kWhereCondValue, query->conds)) {
            return nullptr;
        }
        for (size_t i = 0; i < query->having_conds.size(); ++i) {
            if (!builder.add_having(i)) {
                return nullptr;
            }
        }
        if (!add_condition_slots(&builder, QueryLiteralTarget::kSemiCondValue, query->semi_conds)) {
            return nullptr;
        }
        if (query->has_limit && !builder.add_limit()) {
            return nullptr;
        }
    } else {
        return nullptr;
    }
    if (!builder.done()) {
        return nullptr;
    }

    auto templ = std::make_shared<QueryTemplate>();
    templ->skeleton = clone_query_skeleton(query);
    templ->slots = builder.take_slots();
    return templ;
}

inline bool apply_literal_slot(const QueryLiteralSlot &slot, const SqlTemplateLiteral &literal, Query *query,
                               Context *context = nullptr) {
    switch (slot.target) {
        case QueryLiteralTarget::kQueryValue:
            if (slot.index >= query->values.size()) return false;
            return fill_value_from_literal(&query->values[slot.index], slot, literal, context);
        case QueryLiteralTarget::kUpdateSetValue:
            if (slot.index >= query->set_clauses.size()) return false;
            return fill_value_from_literal(&query->set_clauses[slot.index].rhs, slot, literal, context);
        case QueryLiteralTarget::kWhereCondValue:
            if (slot.index >= query->conds.size()) return false;
            return fill_value_from_literal(&query->conds[slot.index].rhs_val, slot, literal, context);
        case QueryLiteralTarget::kSemiCondValue:
            if (slot.index >= query->semi_conds.size()) return false;
            return fill_value_from_literal(&query->semi_conds[slot.index].rhs_val, slot, literal, context);
        case QueryLiteralTarget::kHavingValue: {
            if (slot.index >= query->having_conds.size()) return false;
            auto value = ast_value_from_literal(slot, literal);
            if (value == nullptr) return false;
            query->having_conds[slot.index]->rhs = std::move(value);
            return true;
        }
        case QueryLiteralTarget::kLimitValue:
            if (literal.type != SqlTemplateLiteralType::kInt) return false;
            query->limit = literal.int_val * slot.parsed_slot.numeric_sign;
            query->has_limit = true;
            return true;
    }
    return false;
}

inline std::shared_ptr<Query> materialize_template(const QueryTemplate &templ,
                                                   const SqlTemplateCandidate &candidate,
                                                   Context *context = nullptr) {
    if (templ.slots.size() != candidate.literals.size()) {
        return nullptr;
    }
    auto query = clone_query_skeleton(templ.skeleton);
    for (size_t i = 0; i < templ.slots.size(); ++i) {
        if (!apply_literal_slot(templ.slots[i], candidate.literals[i], query.get(), context)) {
            return nullptr;
        }
    }
    return query;
}

}  // namespace query_template_detail
}  // namespace rmdb
