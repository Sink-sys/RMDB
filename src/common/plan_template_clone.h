#pragma once

#include <memory>
#include <utility>
#include <vector>

#include "common/query_template.h"
#include "common/types.h"
#include "optimizer/plan.h"

namespace rmdb {
namespace plan_template_detail {

inline bool cacheable_dml_tag(PlanTag tag) {
    return tag == T_select || tag == T_Insert || tag == T_Update || tag == T_Delete;
}

class PlanCloneBuilder {
   public:
    rmdb::u32 next_node() { return next_node_++; }
    rmdb::u32 node_count() const { return next_node_; }

   private:
    rmdb::u32 next_node_ = 0;
};

class PlanMaterializer {
   public:
    PlanMaterializer() = default;
    explicit PlanMaterializer(size_t expected_nodes) { nodes_.reserve(expected_nodes); }

    void set_root(std::shared_ptr<Plan> root) { root_ = std::move(root); }

    void register_node(rmdb::u32 id, const std::shared_ptr<Plan> &plan) {
        if (nodes_.size() <= id) {
            nodes_.resize(static_cast<size_t>(id) + 1);
        }
        nodes_[id] = plan;
    }

    std::shared_ptr<Plan> root() const { return root_; }

    std::shared_ptr<Plan> get(rmdb::u32 id) const {
        return id < nodes_.size() ? nodes_[id] : nullptr;
    }

   private:
    std::shared_ptr<Plan> root_;
    std::vector<std::shared_ptr<Plan>> nodes_;
};

inline std::shared_ptr<Plan> clone_plan_skeleton(const std::shared_ptr<Plan> &src, PlanCloneBuilder *builder,
                                                 PlanMaterializer *materializer = nullptr);

inline void copy_runtime_cache(const std::shared_ptr<Plan> &src, const std::shared_ptr<Plan> &dst) {
    if (src != nullptr && dst != nullptr) {
        dst->runtime_cache_ = src->runtime_cache_;
        dst->runtime_feedback_ = src->runtime_feedback_;
    }
}

inline void assign_template_node(const std::shared_ptr<Plan> &plan, rmdb::u32 node_id,
                                 PlanMaterializer *materializer) {
    plan->template_node_id_ = node_id;
    if (materializer != nullptr) {
        materializer->register_node(node_id, plan);
    }
}

inline std::vector<Value> clone_values(const std::vector<Value> &src) {
    std::vector<Value> dst;
    dst.reserve(src.size());
    for (const auto &value : src) {
        dst.push_back(query_template_detail::clone_value(value));
    }
    return dst;
}

inline std::vector<Condition> clone_conditions(const std::vector<Condition> &src) {
    std::vector<Condition> dst;
    dst.reserve(src.size());
    for (const auto &cond : src) {
        dst.push_back(query_template_detail::clone_condition(cond));
    }
    return dst;
}

inline std::vector<SetClause> clone_set_clauses(const std::vector<SetClause> &src) {
    std::vector<SetClause> dst;
    dst.reserve(src.size());
    for (const auto &clause : src) {
        dst.push_back(query_template_detail::clone_set_clause(clause));
    }
    return dst;
}

inline std::vector<std::shared_ptr<ast::SelectItem>> clone_select_items(
    const std::vector<std::shared_ptr<ast::SelectItem>> &src) {
    std::vector<std::shared_ptr<ast::SelectItem>> dst;
    dst.reserve(src.size());
    for (const auto &item : src) {
        dst.push_back(sql_template_detail::clone_select_item(item));
    }
    return dst;
}

inline std::shared_ptr<Plan> clone_scan_plan(const std::shared_ptr<ScanPlan> &src, PlanCloneBuilder *builder,
                                             PlanMaterializer *materializer) {
    auto dst = std::make_shared<ScanPlan>(src->tag, src->tab_name_, src->visible_name_, src->cols_,
                                          clone_conditions(src->conds_), src->len_,
                                          clone_conditions(src->fed_conds_), src->index_col_names_,
                                          src->required_cols_);
    copy_runtime_cache(src, dst);
    rmdb::u32 id = builder->next_node();
    assign_template_node(dst, id, materializer);
    return dst;
}

inline std::shared_ptr<Plan> clone_plan_skeleton(const std::shared_ptr<Plan> &src, PlanCloneBuilder *builder,
                                                 PlanMaterializer *materializer) {
    if (src == nullptr) {
        return nullptr;
    }
    if (auto dml = std::dynamic_pointer_cast<DMLPlan>(src)) {
        if (!cacheable_dml_tag(dml->tag)) {
            return nullptr;
        }
        auto subplan = clone_plan_skeleton(dml->subplan_, builder, materializer);
        auto dst = std::make_shared<DMLPlan>(dml->tag, subplan, dml->tab_name_, clone_values(dml->values_),
                                             clone_conditions(dml->conds_), clone_set_clauses(dml->set_clauses_));
        copy_runtime_cache(src, dst);
        dst->file_name_ = dml->file_name_;
        dst->output_cols_ = dml->output_cols_;
        rmdb::u32 id = builder->next_node();
        assign_template_node(dst, id, materializer);
        return dst;
    }
    if (auto scan = std::dynamic_pointer_cast<ScanPlan>(src)) {
        if (scan->tag != T_SeqScan && scan->tag != T_IndexScan) {
            return nullptr;
        }
        return clone_scan_plan(scan, builder, materializer);
    }
    if (auto join = std::dynamic_pointer_cast<JoinPlan>(src)) {
        if (join->tag != T_NestLoop && join->tag != T_IndexNestLoop && join->tag != T_HashJoin &&
            join->tag != T_SortMerge) {
            return nullptr;
        }
        auto left = clone_plan_skeleton(join->left_, builder, materializer);
        auto right = clone_plan_skeleton(join->right_, builder, materializer);
        if (left == nullptr || right == nullptr) {
            return nullptr;
        }
        auto dst = std::make_shared<JoinPlan>(join->tag, left, right, clone_conditions(join->conds_),
                                              join->index_col_names_);
        copy_runtime_cache(src, dst);
        dst->type = join->type;
        rmdb::u32 id = builder->next_node();
        assign_template_node(dst, id, materializer);
        return dst;
    }
    if (auto projection = std::dynamic_pointer_cast<ProjectionPlan>(src)) {
        auto subplan = clone_plan_skeleton(projection->subplan_, builder, materializer);
        if (subplan == nullptr) {
            return nullptr;
        }
        auto dst = std::make_shared<ProjectionPlan>(projection->tag, subplan, projection->sel_cols_);
        copy_runtime_cache(src, dst);
        rmdb::u32 id = builder->next_node();
        assign_template_node(dst, id, materializer);
        return dst;
    }
    if (auto sort = std::dynamic_pointer_cast<SortPlan>(src)) {
        auto subplan = clone_plan_skeleton(sort->subplan_, builder, materializer);
        if (subplan == nullptr) {
            return nullptr;
        }
        auto dst = std::make_shared<SortPlan>(sort->tag, subplan, sort->order_cols_);
        copy_runtime_cache(src, dst);
        dst->sel_col_ = sort->sel_col_;
        dst->is_desc_ = sort->is_desc_;
        dst->limit_ = sort->limit_;
        rmdb::u32 id = builder->next_node();
        assign_template_node(dst, id, materializer);
        return dst;
    }
    if (auto aggregate = std::dynamic_pointer_cast<AggregatePlan>(src)) {
        auto subplan = clone_plan_skeleton(aggregate->subplan_, builder, materializer);
        if (subplan == nullptr) {
            return nullptr;
        }
        auto dst = std::make_shared<AggregatePlan>(
            subplan, clone_select_items(aggregate->select_items_), aggregate->group_cols_,
            sql_template_detail::clone_having_exprs(aggregate->having_conds_), aggregate->output_cols_);
        copy_runtime_cache(src, dst);
        rmdb::u32 id = builder->next_node();
        assign_template_node(dst, id, materializer);
        return dst;
    }
    if (auto count = std::dynamic_pointer_cast<CountIndexAggregatePlan>(src)) {
        auto dst = std::make_shared<CountIndexAggregatePlan>(
            count->tab_name_, count->visible_name_, clone_conditions(count->conds_), count->index_col_names_,
            count->count_star_, count->count_col_, count->output_cols_);
        copy_runtime_cache(src, dst);
        rmdb::u32 id = builder->next_node();
        assign_template_node(dst, id, materializer);
        return dst;
    }
    if (auto minmax = std::dynamic_pointer_cast<MinMaxIndexAggregatePlan>(src)) {
        auto dst = std::make_shared<MinMaxIndexAggregatePlan>(
            minmax->tab_name_, minmax->visible_name_, clone_conditions(minmax->conds_),
            minmax->index_col_names_, minmax->agg_col_, minmax->agg_type_, minmax->output_cols_);
        copy_runtime_cache(src, dst);
        rmdb::u32 id = builder->next_node();
        assign_template_node(dst, id, materializer);
        return dst;
    }
    if (auto limit = std::dynamic_pointer_cast<LimitPlan>(src)) {
        auto subplan = clone_plan_skeleton(limit->subplan_, builder, materializer);
        if (subplan == nullptr) {
            return nullptr;
        }
        auto dst = std::make_shared<LimitPlan>(subplan, limit->limit_);
        copy_runtime_cache(src, dst);
        rmdb::u32 id = builder->next_node();
        assign_template_node(dst, id, materializer);
        return dst;
    }
    if (auto semi = std::dynamic_pointer_cast<SemiJoinPlan>(src)) {
        auto left = clone_plan_skeleton(semi->left_, builder, materializer);
        auto right = clone_plan_skeleton(semi->right_, builder, materializer);
        if (left == nullptr || right == nullptr) {
            return nullptr;
        }
        auto dst = std::make_shared<SemiJoinPlan>(left, right, clone_conditions(semi->conds_));
        copy_runtime_cache(src, dst);
        rmdb::u32 id = builder->next_node();
        assign_template_node(dst, id, materializer);
        return dst;
    }
    return nullptr;
}

inline RuntimeNodeKind runtime_kind_for_plan(const std::shared_ptr<Plan> &plan) {
    if (plan == nullptr) {
        return RuntimeNodeKind::kOther;
    }
    switch (plan->tag) {
        case T_SeqScan: return RuntimeNodeKind::kSeqScan;
        case T_IndexScan: return RuntimeNodeKind::kIndexScan;
        case T_NestLoop:
        case T_IndexNestLoop:
        case T_HashJoin:
        case T_SortMerge:
        case T_SemiJoin:
            return RuntimeNodeKind::kJoin;
        case T_CountIndexAggregate: return RuntimeNodeKind::kCountIndex;
        case T_MinMaxIndexAggregate: return RuntimeNodeKind::kMinMaxIndex;
        case T_Aggregate: return RuntimeNodeKind::kAggregate;
        case T_Sort: return RuntimeNodeKind::kSort;
        case T_Limit: return RuntimeNodeKind::kLimit;
        case T_select:
        case T_Insert:
        case T_Update:
        case T_Delete:
        case T_Load:
            return RuntimeNodeKind::kDml;
        default: return RuntimeNodeKind::kOther;
    }
}

inline void attach_runtime_feedback(const std::shared_ptr<Plan> &plan,
                                    const std::shared_ptr<RuntimeFeedbackStore> &store) {
    if (plan == nullptr || store == nullptr) {
        return;
    }
    if (plan->template_node_id_ != kInvalidRuntimeNodeId) {
        plan->runtime_feedback_ =
            store->ensure(plan->template_node_id_, runtime_kind_for_plan(plan));
    }

    if (auto dml = std::dynamic_pointer_cast<DMLPlan>(plan)) {
        attach_runtime_feedback(dml->subplan_, store);
        return;
    }
    if (auto join = std::dynamic_pointer_cast<JoinPlan>(plan)) {
        attach_runtime_feedback(join->left_, store);
        attach_runtime_feedback(join->right_, store);
        return;
    }
    if (auto projection = std::dynamic_pointer_cast<ProjectionPlan>(plan)) {
        attach_runtime_feedback(projection->subplan_, store);
        return;
    }
    if (auto sort = std::dynamic_pointer_cast<SortPlan>(plan)) {
        attach_runtime_feedback(sort->subplan_, store);
        return;
    }
    if (auto aggregate = std::dynamic_pointer_cast<AggregatePlan>(plan)) {
        attach_runtime_feedback(aggregate->subplan_, store);
        return;
    }
    if (auto limit = std::dynamic_pointer_cast<LimitPlan>(plan)) {
        attach_runtime_feedback(limit->subplan_, store);
        return;
    }
    if (auto semi = std::dynamic_pointer_cast<SemiJoinPlan>(plan)) {
        attach_runtime_feedback(semi->left_, store);
        attach_runtime_feedback(semi->right_, store);
        return;
    }
    if (auto union_plan = std::dynamic_pointer_cast<UnionPlan>(plan)) {
        for (auto &subplan : union_plan->subplans_) {
            attach_runtime_feedback(subplan, store);
        }
    }
}

}  // namespace plan_template_detail
}  // namespace rmdb
