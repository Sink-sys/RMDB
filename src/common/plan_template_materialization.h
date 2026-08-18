#pragma once

#include <memory>
#include <utility>
#include <vector>

#include "common/plan_template_clone.h"
#include "common/query_template.h"
#include "common/types.h"
#include "optimizer/plan.h"

namespace rmdb {
namespace plan_template_detail {

enum class PlanLiteralTarget {
    kDmlValue,
    kDmlSetValue,
    kDmlCondValue,
    kScanCondValue,
    kJoinCondValue,
    kSemiCondValue,
    kHavingValue,
    kLimitValue,
    kSortLimitValue,
};

struct PlanLiteralSlot {
    PlanLiteralTarget target = PlanLiteralTarget::kDmlValue;
    rmdb::u32 plan_node = 0;
    rmdb::u32 index = 0;
    rmdb::u32 source_index = 0;
    query_template_detail::QueryLiteralSlot query_slot;
};

struct PlanTemplate {
    std::shared_ptr<Plan> skeleton;
    std::vector<PlanLiteralSlot> slots;
    std::shared_ptr<RuntimeFeedbackStore> feedback;
    size_t literal_count = 0;
};

inline query_template_detail::QueryLiteralTarget query_target_for_plan_target(PlanLiteralTarget target) {
    switch (target) {
        case PlanLiteralTarget::kDmlValue:
            return query_template_detail::QueryLiteralTarget::kQueryValue;
        case PlanLiteralTarget::kDmlSetValue:
            return query_template_detail::QueryLiteralTarget::kUpdateSetValue;
        case PlanLiteralTarget::kDmlCondValue:
        case PlanLiteralTarget::kScanCondValue:
        case PlanLiteralTarget::kJoinCondValue:
            return query_template_detail::QueryLiteralTarget::kWhereCondValue;
        case PlanLiteralTarget::kSemiCondValue:
            return query_template_detail::QueryLiteralTarget::kSemiCondValue;
        case PlanLiteralTarget::kHavingValue:
            return query_template_detail::QueryLiteralTarget::kHavingValue;
        case PlanLiteralTarget::kLimitValue:
        case PlanLiteralTarget::kSortLimitValue:
            return query_template_detail::QueryLiteralTarget::kLimitValue;
    }
    return query_template_detail::QueryLiteralTarget::kQueryValue;
}

inline bool same_tab_col(const TabCol &lhs, const TabCol &rhs) {
    return lhs.tab_name == rhs.tab_name && lhs.col_name == rhs.col_name;
}

inline bool same_literal_value(const Value &lhs, const Value &rhs) {
    if (lhs.type != rhs.type) {
        return false;
    }
    if (lhs.type == TYPE_INT) {
        return lhs.int_val == rhs.int_val;
    }
    if (lhs.type == TYPE_FLOAT) {
        return lhs.float_val == rhs.float_val;
    }
    return lhs.str_val == rhs.str_val;
}

inline bool condition_matches_literal(const Condition &plan_cond, const Condition &query_cond) {
    return plan_cond.is_rhs_val && query_cond.is_rhs_val && same_tab_col(plan_cond.lhs_col, query_cond.lhs_col) &&
           plan_cond.op == query_cond.op && same_literal_value(plan_cond.rhs_val, query_cond.rhs_val);
}

inline bool set_clause_matches_literal(const SetClause &plan_clause, const SetClause &query_clause) {
    return same_tab_col(plan_clause.lhs, query_clause.lhs) && plan_clause.op == query_clause.op &&
           plan_clause.rhs_is_col == query_clause.rhs_is_col && same_tab_col(plan_clause.rhs_col, query_clause.rhs_col) &&
           plan_clause.rhs_has_val && query_clause.rhs_has_val && same_literal_value(plan_clause.rhs, query_clause.rhs);
}

inline bool ast_value_matches_literal(const std::shared_ptr<ast::Value> &value,
                                      const query_template_detail::QueryLiteralSlot &slot,
                                      const SqlTemplateLiteral &literal) {
    if (value == nullptr || slot.parsed_slot.type != literal.type) {
        return false;
    }
    if (auto int_lit = std::dynamic_pointer_cast<ast::IntLit>(value)) {
        return literal.type == SqlTemplateLiteralType::kInt &&
               int_lit->val == literal.int_val * slot.parsed_slot.numeric_sign;
    }
    if (auto float_lit = std::dynamic_pointer_cast<ast::FloatLit>(value)) {
        return literal.type == SqlTemplateLiteralType::kFloat &&
               float_lit->val == literal.float_val * static_cast<float>(slot.parsed_slot.numeric_sign);
    }
    if (auto str_lit = std::dynamic_pointer_cast<ast::StringLit>(value)) {
        return literal.type == SqlTemplateLiteralType::kString &&
               std::string_view(str_lit->val) == literal.str_val;
    }
    return false;
}

inline bool fill_value_from_source(Value *value, const PlanLiteralSlot &slot, const SqlTemplateLiteral &literal,
                                   Context *context = nullptr) {
    return query_template_detail::fill_value_from_literal(value, slot.query_slot, literal, context);
}

class PlanSlotBuilder {
   public:
    PlanSlotBuilder(const std::shared_ptr<Query> &query, const std::vector<query_template_detail::QueryLiteralSlot> &slots,
                    const SqlTemplateCandidate &candidate)
        : query_(query), query_slots_(slots), candidate_(candidate) {
        used_.assign(query_slots_.size(), false);
    }

    bool add_value(PlanLiteralTarget target, rmdb::u32 node, size_t index, const Value &value) {
        return add_by_match(target, node, index, [&](const query_template_detail::QueryLiteralSlot &slot,
                                                     size_t source_index) {
            if (slot.target != query_target_for_plan_target(target)) {
                return false;
            }
            if (target == PlanLiteralTarget::kDmlValue && slot.index < query_->values.size()) {
                // INSERT values preserve their analyzed order all the way into
                // DMLPlan::values_. Matching equal values by content is
                // ambiguous when literals repeat, so use the stored slot index.
                return slot.index == index && same_literal_value(value, query_->values[index]);
            }
            if (target == PlanLiteralTarget::kDmlSetValue && slot.index < query_->set_clauses.size()) {
                return query_->set_clauses[slot.index].rhs_has_val &&
                       same_literal_value(value, query_->set_clauses[slot.index].rhs);
            }
            if ((target == PlanLiteralTarget::kDmlCondValue || target == PlanLiteralTarget::kScanCondValue ||
                 target == PlanLiteralTarget::kJoinCondValue) &&
                slot.index < query_->conds.size()) {
                return same_literal_value(value, query_->conds[slot.index].rhs_val);
            }
            if (target == PlanLiteralTarget::kSemiCondValue && slot.index < query_->semi_conds.size()) {
                return same_literal_value(value, query_->semi_conds[slot.index].rhs_val);
            }
            (void)source_index;
            return false;
        });
    }

    bool add_condition(PlanLiteralTarget target, rmdb::u32 node, size_t index, const Condition &cond) {
        if (!cond.is_rhs_val) {
            return true;
        }
        return add_by_match(target, node, index, [&](const query_template_detail::QueryLiteralSlot &slot,
                                                     size_t source_index) {
            if (slot.target != query_target_for_plan_target(target)) {
                return false;
            }
            if ((target == PlanLiteralTarget::kDmlCondValue || target == PlanLiteralTarget::kScanCondValue ||
                 target == PlanLiteralTarget::kJoinCondValue) &&
                slot.index < query_->conds.size()) {
                return condition_matches_literal(cond, query_->conds[slot.index]);
            }
            if (target == PlanLiteralTarget::kSemiCondValue && slot.index < query_->semi_conds.size()) {
                return condition_matches_literal(cond, query_->semi_conds[slot.index]);
            }
            (void)source_index;
            return false;
        });
    }

    bool add_set_clause(rmdb::u32 node, size_t index, const SetClause &clause) {
        if (!clause.rhs_has_val) {
            return true;
        }
        return add_by_match(PlanLiteralTarget::kDmlSetValue, node, index,
                            [&](const query_template_detail::QueryLiteralSlot &slot, size_t source_index) {
                                if (slot.target != query_template_detail::QueryLiteralTarget::kUpdateSetValue ||
                                    slot.index >= query_->set_clauses.size()) {
                                    return false;
                                }
                                (void)source_index;
                                return set_clause_matches_literal(clause, query_->set_clauses[slot.index]);
                            });
    }

    bool add_having(rmdb::u32 node, size_t index, const std::shared_ptr<ast::HavingExpr> &having) {
        return add_by_match(PlanLiteralTarget::kHavingValue, node, index,
                            [&](const query_template_detail::QueryLiteralSlot &slot, size_t source_index) {
                                if (slot.target != query_template_detail::QueryLiteralTarget::kHavingValue ||
                                    slot.index >= query_->having_conds.size()) {
                                    return false;
                                }
                                return ast_value_matches_literal(having->rhs, slot, candidate_.literals[source_index]);
                            });
    }

    bool add_limit(PlanLiteralTarget target, rmdb::u32 node, int limit) {
        if (limit < 0 || !query_->has_limit) {
            return true;
        }
        return add_by_match(target, node, 0, [&](const query_template_detail::QueryLiteralSlot &slot,
                                                 size_t source_index) {
            (void)source_index;
            return slot.target == query_template_detail::QueryLiteralTarget::kLimitValue &&
                   query_->limit == limit;
        });
    }

    bool done() const {
        for (size_t i = 0; i < query_slots_.size(); ++i) {
            if (!used_[i]) {
                return false;
            }
        }
        return true;
    }

    std::vector<PlanLiteralSlot> take_slots() { return std::move(slots_); }

   private:
    template <typename Predicate>
    bool add_by_match(PlanLiteralTarget target, rmdb::u32 node, size_t index, Predicate pred) {
        size_t match_count = 0;
        size_t unused_match_count = 0;
        size_t first_match = 0;
        size_t first_unused_match = 0;
        for (size_t i = 0; i < query_slots_.size(); ++i) {
            if (!pred(query_slots_[i], i)) {
                continue;
            }
            if (match_count++ == 0) {
                first_match = i;
            }
            if (!used_[i]) {
                if (unused_match_count++ == 0) {
                    first_unused_match = i;
                }
            }
        }
        if (match_count == 0) {
            return false;
        }
        if (unused_match_count > 1) {
            return false;
        }
        if (unused_match_count == 0 && match_count > 1) {
            return false;
        }
        const size_t source = unused_match_count == 0 ? first_match : first_unused_match;
        used_[source] = true;
        PlanLiteralSlot slot;
        slot.target = target;
        slot.plan_node = node;
        slot.index = static_cast<rmdb::u32>(index);
        slot.source_index = static_cast<rmdb::u32>(source);
        slot.query_slot = query_slots_[source];
        slots_.push_back(std::move(slot));
        return true;
    }

    const std::shared_ptr<Query> &query_;
    const std::vector<query_template_detail::QueryLiteralSlot> &query_slots_;
    const SqlTemplateCandidate &candidate_;
    std::vector<bool> used_;
    std::vector<PlanLiteralSlot> slots_;
};

inline bool collect_plan_slots(const std::shared_ptr<Plan> &plan, rmdb::u32 *next_node, PlanSlotBuilder *builder) {
    if (plan == nullptr) {
        return true;
    }
    if (auto dml = std::dynamic_pointer_cast<DMLPlan>(plan)) {
        if (!collect_plan_slots(dml->subplan_, next_node, builder)) {
            return false;
        }
        rmdb::u32 node = (*next_node)++;
        for (size_t i = 0; i < dml->values_.size(); ++i) {
            if (!builder->add_value(PlanLiteralTarget::kDmlValue, node, i, dml->values_[i])) {
                return false;
            }
        }
        for (size_t i = 0; i < dml->set_clauses_.size(); ++i) {
            if (!builder->add_set_clause(node, i, dml->set_clauses_[i])) {
                return false;
            }
        }
        for (size_t i = 0; i < dml->conds_.size(); ++i) {
            if (!builder->add_condition(PlanLiteralTarget::kDmlCondValue, node, i, dml->conds_[i])) {
                return false;
            }
        }
        return true;
    }
    if (auto scan = std::dynamic_pointer_cast<ScanPlan>(plan)) {
        rmdb::u32 node = (*next_node)++;
        for (size_t i = 0; i < scan->conds_.size(); ++i) {
            if (!builder->add_condition(PlanLiteralTarget::kScanCondValue, node, i, scan->conds_[i])) {
                return false;
            }
        }
        return true;
    }
    if (auto join = std::dynamic_pointer_cast<JoinPlan>(plan)) {
        if (!collect_plan_slots(join->left_, next_node, builder) ||
            !collect_plan_slots(join->right_, next_node, builder)) {
            return false;
        }
        rmdb::u32 node = (*next_node)++;
        for (size_t i = 0; i < join->conds_.size(); ++i) {
            if (!builder->add_condition(PlanLiteralTarget::kJoinCondValue, node, i, join->conds_[i])) {
                return false;
            }
        }
        return true;
    }
    if (auto projection = std::dynamic_pointer_cast<ProjectionPlan>(plan)) {
        if (!collect_plan_slots(projection->subplan_, next_node, builder)) {
            return false;
        }
        (*next_node)++;
        return true;
    }
    if (auto sort = std::dynamic_pointer_cast<SortPlan>(plan)) {
        if (!collect_plan_slots(sort->subplan_, next_node, builder)) {
            return false;
        }
        rmdb::u32 node = (*next_node)++;
        return builder->add_limit(PlanLiteralTarget::kSortLimitValue, node, sort->limit_);
    }
    if (auto aggregate = std::dynamic_pointer_cast<AggregatePlan>(plan)) {
        if (!collect_plan_slots(aggregate->subplan_, next_node, builder)) {
            return false;
        }
        rmdb::u32 node = (*next_node)++;
        for (size_t i = 0; i < aggregate->having_conds_.size(); ++i) {
            if (!builder->add_having(node, i, aggregate->having_conds_[i])) {
                return false;
            }
        }
        return true;
    }
    if (auto count = std::dynamic_pointer_cast<CountIndexAggregatePlan>(plan)) {
        rmdb::u32 node = (*next_node)++;
        for (size_t i = 0; i < count->conds_.size(); ++i) {
            if (!builder->add_condition(PlanLiteralTarget::kScanCondValue, node, i, count->conds_[i])) {
                return false;
            }
        }
        return true;
    }
    if (auto minmax = std::dynamic_pointer_cast<MinMaxIndexAggregatePlan>(plan)) {
        rmdb::u32 node = (*next_node)++;
        for (size_t i = 0; i < minmax->conds_.size(); ++i) {
            if (!builder->add_condition(PlanLiteralTarget::kScanCondValue, node, i, minmax->conds_[i])) {
                return false;
            }
        }
        return true;
    }
    if (auto limit = std::dynamic_pointer_cast<LimitPlan>(plan)) {
        if (!collect_plan_slots(limit->subplan_, next_node, builder)) {
            return false;
        }
        rmdb::u32 node = (*next_node)++;
        return builder->add_limit(PlanLiteralTarget::kLimitValue, node, limit->limit_);
    }
    if (auto semi = std::dynamic_pointer_cast<SemiJoinPlan>(plan)) {
        if (!collect_plan_slots(semi->left_, next_node, builder) ||
            !collect_plan_slots(semi->right_, next_node, builder)) {
            return false;
        }
        rmdb::u32 node = (*next_node)++;
        for (size_t i = 0; i < semi->conds_.size(); ++i) {
            if (!builder->add_condition(PlanLiteralTarget::kSemiCondValue, node, i, semi->conds_[i])) {
                return false;
            }
        }
        return true;
    }
    return false;
}

inline std::shared_ptr<PlanTemplate> make_template(const SqlTemplateCandidate &candidate,
                                                   const std::shared_ptr<Query> &query,
                                                   const std::shared_ptr<Plan> &plan,
                                                   rmdb::u64 feedback_generation) {
    if (query == nullptr || plan == nullptr || candidate.literals.size() == 0) {
        return nullptr;
    }
    auto dml = std::dynamic_pointer_cast<DMLPlan>(plan);
    if (dml == nullptr || !cacheable_dml_tag(dml->tag) || query->kind == StmtKind::Union ||
        !query->union_queries.empty()) {
        return nullptr;
    }

    auto query_template = query_template_detail::make_template(candidate, query->parse, query);
    if (query_template == nullptr || query_template->slots.size() != candidate.literals.size()) {
        return nullptr;
    }
    PlanSlotBuilder slot_builder(query, query_template->slots, candidate);
    rmdb::u32 node = 0;
    if (!collect_plan_slots(plan, &node, &slot_builder) || !slot_builder.done()) {
        return nullptr;
    }

    PlanCloneBuilder clone_builder;
    auto skeleton = clone_plan_skeleton(plan, &clone_builder);
    if (skeleton == nullptr) {
        return nullptr;
    }
    auto templ = std::make_shared<PlanTemplate>();
    templ->skeleton = std::move(skeleton);
    templ->slots = slot_builder.take_slots();
    templ->feedback = std::make_shared<RuntimeFeedbackStore>(feedback_generation);
    templ->feedback->nodes.resize(clone_builder.node_count());
    attach_runtime_feedback(templ->skeleton, templ->feedback);
    templ->literal_count = candidate.literals.size();
    return templ;
}

// PREPARE builds its immutable query template before optimization. Reuse that
// exact skeleton and slot map here: the optimizer may consume, reorder, or
// rewrite the analyzed Query while producing the plan, so rebuilding literal
// slots from the post-optimization Query can lose parameters and disable the
// plan template.
inline std::shared_ptr<PlanTemplate> make_template(
    const SqlTemplateCandidate &candidate,
    const std::shared_ptr<const query_template_detail::QueryTemplate> &query_template,
    const std::shared_ptr<Plan> &plan, rmdb::u64 feedback_generation) {
    if (query_template == nullptr || query_template->skeleton == nullptr || plan == nullptr ||
        candidate.literals.size() == 0 || query_template->slots.size() != candidate.literals.size()) {
        return nullptr;
    }
    const auto &query = query_template->skeleton;
    auto dml = std::dynamic_pointer_cast<DMLPlan>(plan);
    if (dml == nullptr || !cacheable_dml_tag(dml->tag) || query->kind == StmtKind::Union ||
        !query->union_queries.empty()) {
        return nullptr;
    }

    PlanSlotBuilder slot_builder(query, query_template->slots, candidate);
    rmdb::u32 node = 0;
    if (!collect_plan_slots(plan, &node, &slot_builder) || !slot_builder.done()) {
        return nullptr;
    }

    PlanCloneBuilder clone_builder;
    auto skeleton = clone_plan_skeleton(plan, &clone_builder);
    if (skeleton == nullptr) {
        return nullptr;
    }
    auto templ = std::make_shared<PlanTemplate>();
    templ->skeleton = std::move(skeleton);
    templ->slots = slot_builder.take_slots();
    templ->feedback = std::make_shared<RuntimeFeedbackStore>(feedback_generation);
    templ->feedback->nodes.resize(clone_builder.node_count());
    attach_runtime_feedback(templ->skeleton, templ->feedback);
    templ->literal_count = candidate.literals.size();
    return templ;
}

inline bool apply_literal_slot(const PlanLiteralSlot &slot, const SqlTemplateLiteral &literal,
                               const PlanMaterializer &materializer, Context *context = nullptr) {
    auto node = materializer.get(slot.plan_node);
    if (node == nullptr) {
        return false;
    }
    switch (slot.target) {
        case PlanLiteralTarget::kDmlValue: {
            auto dml = std::dynamic_pointer_cast<DMLPlan>(node);
            return dml != nullptr && slot.index < dml->values_.size() &&
                   fill_value_from_source(&dml->values_[slot.index], slot, literal, context);
        }
        case PlanLiteralTarget::kDmlSetValue: {
            auto dml = std::dynamic_pointer_cast<DMLPlan>(node);
            return dml != nullptr && slot.index < dml->set_clauses_.size() &&
                   fill_value_from_source(&dml->set_clauses_[slot.index].rhs, slot, literal, context);
        }
        case PlanLiteralTarget::kDmlCondValue: {
            auto dml = std::dynamic_pointer_cast<DMLPlan>(node);
            return dml != nullptr && slot.index < dml->conds_.size() &&
                   fill_value_from_source(&dml->conds_[slot.index].rhs_val, slot, literal, context);
        }
        case PlanLiteralTarget::kScanCondValue: {
            auto scan = std::dynamic_pointer_cast<ScanPlan>(node);
            if (scan != nullptr) {
                return slot.index < scan->conds_.size() &&
                       fill_value_from_source(&scan->conds_[slot.index].rhs_val, slot, literal, context);
            }
            auto count = std::dynamic_pointer_cast<CountIndexAggregatePlan>(node);
            if (count != nullptr) {
                return slot.index < count->conds_.size() &&
                       fill_value_from_source(&count->conds_[slot.index].rhs_val, slot, literal, context);
            }
            auto minmax = std::dynamic_pointer_cast<MinMaxIndexAggregatePlan>(node);
            return minmax != nullptr && slot.index < minmax->conds_.size() &&
                   fill_value_from_source(&minmax->conds_[slot.index].rhs_val, slot, literal, context);
        }
        case PlanLiteralTarget::kJoinCondValue: {
            auto join = std::dynamic_pointer_cast<JoinPlan>(node);
            return join != nullptr && slot.index < join->conds_.size() &&
                   fill_value_from_source(&join->conds_[slot.index].rhs_val, slot, literal, context);
        }
        case PlanLiteralTarget::kSemiCondValue: {
            auto semi = std::dynamic_pointer_cast<SemiJoinPlan>(node);
            return semi != nullptr && slot.index < semi->conds_.size() &&
                   fill_value_from_source(&semi->conds_[slot.index].rhs_val, slot, literal, context);
        }
        case PlanLiteralTarget::kHavingValue: {
            auto aggregate = std::dynamic_pointer_cast<AggregatePlan>(node);
            if (aggregate == nullptr || slot.index >= aggregate->having_conds_.size()) {
                return false;
            }
            auto value = query_template_detail::ast_value_from_literal(slot.query_slot, literal);
            if (value == nullptr) {
                return false;
            }
            aggregate->having_conds_[slot.index]->rhs = std::move(value);
            return true;
        }
        case PlanLiteralTarget::kLimitValue: {
            auto limit = std::dynamic_pointer_cast<LimitPlan>(node);
            if (limit == nullptr || literal.type != SqlTemplateLiteralType::kInt) {
                return false;
            }
            limit->limit_ = literal.int_val * slot.query_slot.parsed_slot.numeric_sign;
            return true;
        }
        case PlanLiteralTarget::kSortLimitValue: {
            auto sort = std::dynamic_pointer_cast<SortPlan>(node);
            if (sort == nullptr || literal.type != SqlTemplateLiteralType::kInt) {
                return false;
            }
            sort->limit_ = literal.int_val * slot.query_slot.parsed_slot.numeric_sign;
            return true;
        }
    }
    return false;
}

inline std::shared_ptr<Plan> materialize_template(const PlanTemplate &templ,
                                                  const SqlTemplateCandidate &candidate,
                                                  Context *context = nullptr) {
    PlanCloneBuilder clone_builder;
    PlanMaterializer materializer(templ.feedback == nullptr ? 0 : templ.feedback->nodes.size());
    auto root = clone_plan_skeleton(templ.skeleton, &clone_builder, &materializer);
    if (root == nullptr || templ.slots.empty()) {
        return nullptr;
    }
    materializer.set_root(root);
    if (templ.literal_count != candidate.literals.size() || templ.slots.size() < candidate.literals.size()) {
        return nullptr;
    }
    for (const auto &slot : templ.slots) {
        if (slot.source_index >= candidate.literals.size()) {
            return nullptr;
        }
        if (!apply_literal_slot(slot, candidate.literals[slot.source_index], materializer, context)) {
            return nullptr;
        }
    }
    return root;
}

}  // namespace plan_template_detail
}  // namespace rmdb
