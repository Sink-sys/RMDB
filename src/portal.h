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

#include <cerrno>
#include <cstring>
#include <string>
#include <utility>
#include "optimizer/plan.h"
#include "execution/executor_abstract.h"
#include "execution/executor_nestedloop_join.h"
#include "execution/executor_projection.h"
#include "execution/executor_seq_scan.h"
#include "execution/executor_index_scan.h"
#include "execution/executor_index_nestedloop_join.h"
#include "execution/executor_index_join_distinct_count.h"
#include "execution/executor_hash_join.h"
#include "execution/executor_sort_merge_join.h"
#include "execution/executor_update.h"
#include "execution/executor_aggregate.h"
#include "execution/executor_minmax_index.h"
#include "execution/executor_count_index.h"
#include "execution/executor_insert.h"
#include "execution/executor_limit.h"
#include "execution/executor_delete.h"
#include "execution/executor_semi_join.h"
#include "execution/executor_union.h"
#include "execution/execution_sort.h"
#include "common/common.h"

typedef enum portalTag{
    PORTAL_Invalid_Query = 0,
    PORTAL_ONE_SELECT,
    PORTAL_DML_WITHOUT_SELECT,
    PORTAL_MULTI_QUERY,
    PORTAL_CMD_UTILITY
} portalTag;


struct PortalStmt {
    portalTag tag;
    
    std::vector<TabCol> sel_cols;
    std::unique_ptr<AbstractExecutor> root;
    std::shared_ptr<Plan> plan;
    
    PortalStmt(portalTag tag_, std::vector<TabCol> sel_cols_, std::unique_ptr<AbstractExecutor> root_, std::shared_ptr<Plan> plan_) :
            tag(tag_), sel_cols(std::move(sel_cols_)), root(std::move(root_)), plan(std::move(plan_)) {}
};

class Portal
{
   private:
    SmManager *sm_manager_;
    

   public:
    Portal(SmManager *sm_manager) : sm_manager_(sm_manager){}
    ~Portal(){}

    // 将查询执行计划转换成对应的算子树
    std::shared_ptr<PortalStmt> start(std::shared_ptr<Plan> plan, Context *context)
    {
        // 这里可以将select进行拆分，例如：一个select，带有return的select等
        if (auto x = std::dynamic_pointer_cast<OtherPlan>(plan)) {
            return std::make_shared<PortalStmt>(PORTAL_CMD_UTILITY, std::vector<TabCol>(), std::unique_ptr<AbstractExecutor>(),plan);
        } else if (auto x = std::dynamic_pointer_cast<SetTransactionIsolationPlan>(plan)) {
            return std::make_shared<PortalStmt>(PORTAL_CMD_UTILITY, std::vector<TabCol>(), std::unique_ptr<AbstractExecutor>(), plan);
        } else if(auto x = std::dynamic_pointer_cast<SetKnobPlan>(plan)) {
            return std::make_shared<PortalStmt>(PORTAL_CMD_UTILITY, std::vector<TabCol>(), std::unique_ptr<AbstractExecutor>(), plan); 
        } else if(auto x = std::dynamic_pointer_cast<ExplainPlan>(plan)) {
            return std::make_shared<PortalStmt>(PORTAL_CMD_UTILITY, std::vector<TabCol>(), std::unique_ptr<AbstractExecutor>(), plan); 
        } else if (auto x = std::dynamic_pointer_cast<DDLPlan>(plan)) {
            return std::make_shared<PortalStmt>(PORTAL_MULTI_QUERY, std::vector<TabCol>(), std::unique_ptr<AbstractExecutor>(),plan);
        } else if (auto x = std::dynamic_pointer_cast<DMLPlan>(plan)) {
            switch(x->tag) {
                case T_select:
                {
                    std::vector<TabCol> sel_cols = x->output_cols_.empty() ? output_cols(x->subplan_) : x->output_cols_;
                    std::unique_ptr<AbstractExecutor> root= convert_plan_executor(x->subplan_, context);
                    return std::make_shared<PortalStmt>(PORTAL_ONE_SELECT, std::move(sel_cols), std::move(root), plan);
                }
                    
                case T_Update:
                {
                    std::unique_ptr<AbstractExecutor> scan= convert_plan_executor(x->subplan_, context);
                    auto rids = context->acquire_statement_rids();
                    for (scan->beginTuple(); !scan->is_end(); scan->nextTuple()) {
                        rids->push_back(scan->rid());
                    }
                    scan.reset();
                    std::unique_ptr<AbstractExecutor> root =std::make_unique<UpdateExecutor>(sm_manager_, 
                                                            x->tab_name_, x->set_clauses_, x->conds_, rids, context,
                                                            x->runtime_cache_);
                    return std::make_shared<PortalStmt>(PORTAL_DML_WITHOUT_SELECT, std::vector<TabCol>(), std::move(root), plan);
                }
                case T_Delete:
                {
                    std::unique_ptr<AbstractExecutor> scan= convert_plan_executor(x->subplan_, context);
                    auto rids = context->acquire_statement_rids();
                    for (scan->beginTuple(); !scan->is_end(); scan->nextTuple()) {
                        rids->push_back(scan->rid());
                    }
                    scan.reset();

                    std::unique_ptr<AbstractExecutor> root =
                        std::make_unique<DeleteExecutor>(sm_manager_, x->tab_name_, x->conds_, rids, context,
                                                         x->runtime_cache_);

                    return std::make_shared<PortalStmt>(PORTAL_DML_WITHOUT_SELECT, std::vector<TabCol>(), std::move(root), plan);
                }

                case T_Insert:
                {
                    std::unique_ptr<AbstractExecutor> root =
                            std::make_unique<InsertExecutor>(sm_manager_, x->tab_name_, x->values_, context,
                                                             x->runtime_cache_);
            
                    return std::make_shared<PortalStmt>(PORTAL_DML_WITHOUT_SELECT, std::vector<TabCol>(), std::move(root), plan);
                }
                case T_Load:
                {
                    return std::make_shared<PortalStmt>(PORTAL_MULTI_QUERY, std::vector<TabCol>(),
                                                        std::unique_ptr<AbstractExecutor>(), plan);
                }


                default:
                    throw InternalError("Unexpected field type");
                    break;
            }
        } else {
            throw InternalError("Unexpected field type");
        }
        return nullptr;
    }

    // 遍历算子树并执行算子生成执行结果
    void run(std::shared_ptr<PortalStmt> portal, QlManager* ql, txn_id_t *txn_id, Context *context){
        switch(portal->tag) {
            case PORTAL_ONE_SELECT:
            {
                ql->select_from(std::move(portal->root), std::move(portal->sel_cols), context);
                break;
            }

            case PORTAL_DML_WITHOUT_SELECT:
            {
                ql->run_dml(std::move(portal->root));
                break;
            }
            case PORTAL_MULTI_QUERY:
            {
                ql->run_mutli_query(portal->plan, context);
                break;
            }
            case PORTAL_CMD_UTILITY:
            {
                ql->run_cmd_utility(portal->plan, txn_id, context);
                break;
            }
            default:
            {
                throw InternalError("Unexpected field type");
            }
        }
    }

    // 清空资源
    void drop(){}


    std::unique_ptr<AbstractExecutor> convert_plan_executor(const std::shared_ptr<Plan> &plan, Context *context)
    {
        if(auto x = std::dynamic_pointer_cast<ProjectionPlan>(plan)){
            return std::make_unique<ProjectionExecutor>(convert_plan_executor(x->subplan_, context), 
                                                        x->sel_cols_);
        } else if(auto x = std::dynamic_pointer_cast<ScanPlan>(plan)) {
            if(x->tag == T_SeqScan) {
                return std::make_unique<SeqScanExecutor>(sm_manager_, x->tab_name_, x->conds_, context,
                                                         x->visible_name_, x->required_cols_, x->runtime_cache_,
                                                         x->runtime_feedback_);
            }
            else {
                return std::make_unique<IndexScanExecutor>(sm_manager_, x->tab_name_, x->conds_,
                                                           x->index_col_names_, context, x->visible_name_,
                                                           x->required_cols_, x->runtime_cache_,
                                                           x->runtime_feedback_);
            } 
        } else if(auto x = std::dynamic_pointer_cast<JoinPlan>(plan)) {
            std::unique_ptr<AbstractExecutor> left = convert_plan_executor(x->left_, context);
            if (x->tag == T_IndexNestLoop) {
                auto right_scan = std::dynamic_pointer_cast<ScanPlan>(x->right_);
                return std::make_unique<IndexNestedLoopJoinExecutor>(
                    std::move(left), sm_manager_, right_scan->tab_name_, x->conds_, right_scan->conds_,
                    x->index_col_names_, context,
                    right_scan->visible_name_, right_scan->required_cols_, x->runtime_cache_, x->runtime_feedback_);
            }
            std::unique_ptr<AbstractExecutor> right = convert_plan_executor(x->right_, context);
            if (x->tag == T_HashJoin) {
                return std::make_unique<HashJoinExecutor>(std::move(left), std::move(right), x->conds_,
                                                          x->runtime_feedback_);
            }
            if (x->tag == T_SortMerge) {
                return std::make_unique<SortMergeJoinExecutor>(std::move(left), std::move(right),
                                                               x->conds_, x->runtime_feedback_);
            }
            std::unique_ptr<AbstractExecutor> join = std::make_unique<NestedLoopJoinExecutor>(
                                std::move(left), 
                                std::move(right), x->conds_, x->runtime_feedback_);
            return join;
        } else if(auto x = std::dynamic_pointer_cast<SortPlan>(plan)) {
            auto executor = std::make_unique<SortExecutor>(
                convert_plan_executor(x->subplan_, context),
                x->order_cols_.empty() ? std::vector<std::pair<TabCol, bool>>{{x->sel_col_, x->is_desc_}}
                                       : x->order_cols_);
            executor->set_limit(x->limit_);
            return executor;
        } else if(auto x = std::dynamic_pointer_cast<AggregatePlan>(plan)) {
            if (x->select_items_.size() == 1 && x->group_cols_.empty() && x->having_conds_.empty() &&
                x->output_cols_.size() == 1) {
                const auto &item = x->select_items_[0];
                auto join_plan = std::dynamic_pointer_cast<JoinPlan>(x->subplan_);
                if (item->is_agg && item->agg_type == ast::AGG_COUNT && item->distinct &&
                    !item->count_star && item->col != nullptr && join_plan != nullptr &&
                    join_plan->tag == T_IndexNestLoop) {
                    auto right_scan = std::dynamic_pointer_cast<ScanPlan>(join_plan->right_);
                    if (right_scan != nullptr) {
                        const TabCol target{item->col->tab_name, item->col->col_name};
                        auto target_meta = std::find_if(right_scan->cols_.begin(), right_scan->cols_.end(),
                            [&](const ColMeta &col) {
                                return col.tab_name == target.tab_name && col.name == target.col_name;
                            });
                        if (target_meta != right_scan->cols_.end() && target_meta->type == TYPE_INT &&
                            target_meta->len == static_cast<int>(sizeof(int))) {
                            auto left = convert_plan_executor(join_plan->left_, context);
                            auto join = std::make_unique<IndexNestedLoopJoinExecutor>(
                                std::move(left), sm_manager_, right_scan->tab_name_, join_plan->conds_,
                                right_scan->conds_, join_plan->index_col_names_, context,
                                right_scan->visible_name_, right_scan->required_cols_, join_plan->runtime_cache_,
                                join_plan->runtime_feedback_);
                            return std::make_unique<IndexJoinDistinctCountExecutor>(
                                std::move(join), target, x->output_cols_[0]);
                        }
                    }
                }
            }
            return std::make_unique<AggregateExecutor>(convert_plan_executor(x->subplan_, context),
                                                       x->select_items_, x->group_cols_, x->having_conds_,
                                                       x->output_cols_);
        } else if(auto x = std::dynamic_pointer_cast<MinMaxIndexAggregatePlan>(plan)) {
            return std::make_unique<MinMaxIndexAggregateExecutor>(
                sm_manager_, x->tab_name_, x->visible_name_, x->conds_, x->index_col_names_, x->agg_col_,
                x->agg_type_, x->output_cols_, context, x->runtime_cache_, x->runtime_feedback_);
        } else if(auto x = std::dynamic_pointer_cast<CountIndexAggregatePlan>(plan)) {
            return std::make_unique<CountIndexAggregateExecutor>(
                sm_manager_, x->tab_name_, x->visible_name_, x->conds_, x->index_col_names_,
                x->output_cols_, context, x->runtime_cache_, x->runtime_feedback_);
        } else if(auto x = std::dynamic_pointer_cast<LimitPlan>(plan)) {
            return std::make_unique<LimitExecutor>(convert_plan_executor(x->subplan_, context), x->limit_);
        } else if(auto x = std::dynamic_pointer_cast<SemiJoinPlan>(plan)) {
            return std::make_unique<SemiJoinExecutor>(convert_plan_executor(x->left_, context),
                                                      convert_plan_executor(x->right_, context),
                                                      x->conds_, x->runtime_feedback_);
        } else if(auto x = std::dynamic_pointer_cast<UnionPlan>(plan)) {
            std::vector<std::unique_ptr<AbstractExecutor>> children;
            for (auto &subplan : x->subplans_) {
                children.push_back(convert_plan_executor(subplan, context));
            }
            return std::make_unique<UnionExecutor>(std::move(children), x->cols_);
        }
        return nullptr;
    }

    std::vector<TabCol> output_cols(std::shared_ptr<Plan> plan)
    {
        if(auto x = std::dynamic_pointer_cast<DMLPlan>(plan)) {
            if (x->tag == T_select && !x->output_cols_.empty()) {
                return x->output_cols_;
            }
            if (x->tag == T_select) {
                return output_cols(x->subplan_);
            }
        }
        if(auto x = std::dynamic_pointer_cast<ProjectionPlan>(plan)) {
            return x->sel_cols_;
        } else if(auto x = std::dynamic_pointer_cast<AggregatePlan>(plan)) {
            return x->output_cols_;
        } else if(auto x = std::dynamic_pointer_cast<MinMaxIndexAggregatePlan>(plan)) {
            return x->output_cols_;
        } else if(auto x = std::dynamic_pointer_cast<CountIndexAggregatePlan>(plan)) {
            return x->output_cols_;
        } else if(auto x = std::dynamic_pointer_cast<LimitPlan>(plan)) {
            return output_cols(x->subplan_);
        } else if(auto x = std::dynamic_pointer_cast<SortPlan>(plan)) {
            return output_cols(x->subplan_);
        } else if(auto x = std::dynamic_pointer_cast<UnionPlan>(plan)) {
            std::vector<TabCol> cols;
            for (auto &col : x->cols_) {
                cols.push_back({.tab_name = "", .col_name = col.name});
            }
            return cols;
        }
        return {};
    }

};
