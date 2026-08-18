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
#include "common/runtime_feedback.h"
#include "common/types.h"
#include "common/compiled_access_program.h"
#include "parser/ast.h"
#include "system/sm.h"

#include "parser/parser.h"

typedef enum PlanTag{
    T_Invalid = 1,
    T_Help,
    T_ShowTable,
    T_ShowIndex,
    T_DescTable,
    T_CreateTable,
    T_DropTable,
    T_CreateIndex,
    T_DropIndex,
    T_SetKnob,
    T_Insert,
    T_Load,
    T_Update,
    T_Delete,
    T_select,
    T_Transaction_begin,
    T_Transaction_commit,
    T_Transaction_abort,
    T_Transaction_rollback,
    T_SetTransactionIsolation,
    T_SeqScan,
    T_IndexScan,
    T_NestLoop,
    T_IndexNestLoop,
    T_HashJoin,
    T_SortMerge,    // sort merge join
    T_Sort,
    T_Projection,
    T_Aggregate,
    T_MinMaxIndexAggregate,
    T_CountIndexAggregate,
    T_Limit,
    T_SemiJoin,
    T_Union,
    T_Explain,
    T_Checkpoint
} PlanTag;

struct PlanRuntimeCache {
    // Immutable catalog values may be shared by cached plans. Live file/index
    // handles are resolved from SmManager when an executor is constructed.
    RmFileHdr file_hdr;
    std::vector<ColMeta> full_cols;
    size_t full_len = 0;
    bool has_table = false;

    // Cached scan plans keep their projected layout immutable across
    // executions. Executors may borrow these vectors instead of rebuilding
    // and allocating the same ColMeta mappings for every statement.
    std::vector<ColMeta> projected_cols;
    std::vector<ColMeta> projected_source_cols;
    size_t projected_len = 0;
    bool has_projection = false;

    IndexMeta index_meta;
    bool has_index = false;
    std::shared_ptr<const rmdb::CompiledAccessProgram> access_program;
    std::shared_ptr<const rmdb::CompiledMutationProgram> mutation_program;
};

// 查询执行计划
class Plan
{
public:
    PlanTag tag;
    std::shared_ptr<const PlanRuntimeCache> runtime_cache_;
    rmdb::u32 template_node_id_ = rmdb::kInvalidRuntimeNodeId;
    std::shared_ptr<rmdb::RuntimeNodeFeedback> runtime_feedback_;
    virtual ~Plan() = default;
};

class ScanPlan : public Plan
{
    public:
        ScanPlan(PlanTag tag, SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds,
                 std::vector<std::string> index_col_names, std::string visible_name = "")
        {
            Plan::tag = tag;
            tab_name_ = std::move(tab_name);
            visible_name_ = visible_name.empty() ? tab_name_ : std::move(visible_name);
            conds_ = std::move(conds);
            TabMeta &tab = sm_manager->db_.get_table(tab_name_);
            cols_ = tab.cols;
            for (auto &col : cols_) {
                col.tab_name = visible_name_;
            }
            len_ = cols_.back().offset + cols_.back().len;
            fed_conds_ = conds_;
            index_col_names_ = index_col_names;
        
        }
        // Used only while materializing a cached plan template.  The caller
        // supplies independently-owned predicate vectors, so avoid first
        // copying a ScanPlan and immediately replacing those vectors.
        ScanPlan(PlanTag tag, std::string tab_name, std::string visible_name, std::vector<ColMeta> cols,
                 std::vector<Condition> conds, size_t len, std::vector<Condition> fed_conds,
                 std::vector<std::string> index_col_names, std::vector<TabCol> required_cols)
            : tab_name_(std::move(tab_name)), visible_name_(std::move(visible_name)), cols_(std::move(cols)),
              conds_(std::move(conds)), len_(len), fed_conds_(std::move(fed_conds)),
              index_col_names_(std::move(index_col_names)), required_cols_(std::move(required_cols)) {
            Plan::tag = tag;
        }
        ~ScanPlan(){}
        // 以下变量同ScanExecutor中的变量
        std::string tab_name_;                     
        std::string visible_name_;
        std::vector<ColMeta> cols_;                
        std::vector<Condition> conds_;             
        size_t len_;                               
        std::vector<Condition> fed_conds_;
        std::vector<std::string> index_col_names_;
        std::vector<TabCol> required_cols_;
    
};

class JoinPlan : public Plan
{
    public:
        JoinPlan(PlanTag tag, std::shared_ptr<Plan> left, std::shared_ptr<Plan> right, std::vector<Condition> conds,
                 std::vector<std::string> index_col_names = {})
        {
            Plan::tag = tag;
            left_ = std::move(left);
            right_ = std::move(right);
            conds_ = std::move(conds);
            index_col_names_ = std::move(index_col_names);
            type = INNER_JOIN;
        }
        ~JoinPlan(){}
        // 左节点
        std::shared_ptr<Plan> left_;
        // 右节点
        std::shared_ptr<Plan> right_;
        // 连接条件
        std::vector<Condition> conds_;
        std::vector<std::string> index_col_names_;
        // future TODO: 后续可以支持的连接类型
        JoinType type;
};

class SemiJoinPlan : public Plan
{
    public:
        SemiJoinPlan(std::shared_ptr<Plan> left, std::shared_ptr<Plan> right, std::vector<Condition> conds)
        {
            Plan::tag = T_SemiJoin;
            left_ = std::move(left);
            right_ = std::move(right);
            conds_ = std::move(conds);
        }
        ~SemiJoinPlan(){}
        std::shared_ptr<Plan> left_;
        std::shared_ptr<Plan> right_;
        std::vector<Condition> conds_;
};

class ProjectionPlan : public Plan
{
    public:
        ProjectionPlan(PlanTag tag, std::shared_ptr<Plan> subplan, std::vector<TabCol> sel_cols)
        {
            Plan::tag = tag;
            subplan_ = std::move(subplan);
            sel_cols_ = std::move(sel_cols);
        }
        ~ProjectionPlan(){}
        std::shared_ptr<Plan> subplan_;
        std::vector<TabCol> sel_cols_;
        
};

class SortPlan : public Plan
{
    public:
        SortPlan(PlanTag tag, std::shared_ptr<Plan> subplan, TabCol sel_col, bool is_desc)
        {
            Plan::tag = tag;
            subplan_ = std::move(subplan);
            sel_col_ = sel_col;
            is_desc_ = is_desc;
        }
        SortPlan(PlanTag tag, std::shared_ptr<Plan> subplan, std::vector<std::pair<TabCol, bool>> order_cols)
        {
            Plan::tag = tag;
            subplan_ = std::move(subplan);
            order_cols_ = std::move(order_cols);
            if (!order_cols_.empty()) {
                sel_col_ = order_cols_[0].first;
                is_desc_ = order_cols_[0].second;
            } else {
                is_desc_ = false;
            }
        }
        ~SortPlan(){}
        std::shared_ptr<Plan> subplan_;
        TabCol sel_col_;
        bool is_desc_;
        std::vector<std::pair<TabCol, bool>> order_cols_;
        int limit_ = -1;
        
};

class LimitPlan : public Plan
{
    public:
        LimitPlan(std::shared_ptr<Plan> subplan, int limit)
        {
            Plan::tag = T_Limit;
            subplan_ = std::move(subplan);
            limit_ = limit;
        }
        ~LimitPlan(){}
        std::shared_ptr<Plan> subplan_;
        int limit_;
};

class AggregatePlan : public Plan
{
    public:
        AggregatePlan(std::shared_ptr<Plan> subplan,
                      std::vector<std::shared_ptr<ast::SelectItem>> select_items,
                      std::vector<TabCol> group_cols,
                      std::vector<std::shared_ptr<ast::HavingExpr>> having_conds,
                      std::vector<TabCol> output_cols)
        {
            Plan::tag = T_Aggregate;
            subplan_ = std::move(subplan);
            select_items_ = std::move(select_items);
            group_cols_ = std::move(group_cols);
            having_conds_ = std::move(having_conds);
            output_cols_ = std::move(output_cols);
        }
        ~AggregatePlan(){}
        std::shared_ptr<Plan> subplan_;
        std::vector<std::shared_ptr<ast::SelectItem>> select_items_;
        std::vector<TabCol> group_cols_;
        std::vector<std::shared_ptr<ast::HavingExpr>> having_conds_;
        std::vector<TabCol> output_cols_;
};

class MinMaxIndexAggregatePlan : public Plan
{
    public:
        MinMaxIndexAggregatePlan(std::string tab_name,
                                 std::string visible_name,
                                 std::vector<Condition> conds,
                                 std::vector<std::string> index_col_names,
                                 TabCol agg_col,
                                 ast::AggType agg_type,
                                 std::vector<TabCol> output_cols)
        {
            Plan::tag = T_MinMaxIndexAggregate;
            tab_name_ = std::move(tab_name);
            visible_name_ = visible_name.empty() ? tab_name_ : std::move(visible_name);
            conds_ = std::move(conds);
            index_col_names_ = std::move(index_col_names);
            agg_col_ = std::move(agg_col);
            agg_type_ = agg_type;
            output_cols_ = std::move(output_cols);
        }
        ~MinMaxIndexAggregatePlan(){}
        std::string tab_name_;
        std::string visible_name_;
        std::vector<Condition> conds_;
        std::vector<std::string> index_col_names_;
        TabCol agg_col_;
        ast::AggType agg_type_;
        std::vector<TabCol> output_cols_;
};

class CountIndexAggregatePlan : public Plan
{
    public:
        CountIndexAggregatePlan(std::string tab_name,
                                std::string visible_name,
                                std::vector<Condition> conds,
                                std::vector<std::string> index_col_names,
                                bool count_star,
                                TabCol count_col,
                                std::vector<TabCol> output_cols)
        {
            Plan::tag = T_CountIndexAggregate;
            tab_name_ = std::move(tab_name);
            visible_name_ = visible_name.empty() ? tab_name_ : std::move(visible_name);
            conds_ = std::move(conds);
            index_col_names_ = std::move(index_col_names);
            count_star_ = count_star;
            count_col_ = std::move(count_col);
            output_cols_ = std::move(output_cols);
        }
        ~CountIndexAggregatePlan(){}
        std::string tab_name_;
        std::string visible_name_;
        std::vector<Condition> conds_;
        std::vector<std::string> index_col_names_;
        bool count_star_ = false;
        TabCol count_col_;
        std::vector<TabCol> output_cols_;
};

class ExplainPlan : public Plan
{
    public:
        ExplainPlan(std::vector<std::string> lines)
        {
            Plan::tag = T_Explain;
            lines_ = std::move(lines);
        }
        ~ExplainPlan(){}
        std::vector<std::string> lines_;
};

class UnionPlan : public Plan
{
    public:
        UnionPlan(std::vector<std::shared_ptr<Plan>> subplans, std::vector<ColMeta> cols)
        {
            Plan::tag = T_Union;
            subplans_ = std::move(subplans);
            cols_ = std::move(cols);
            len_ = cols_.empty() ? 0 : cols_.back().offset + cols_.back().len;
        }
        ~UnionPlan(){}
        std::vector<std::shared_ptr<Plan>> subplans_;
        std::vector<ColMeta> cols_;
        size_t len_;
};

// dml语句，包括insert; delete; update; select语句　
class DMLPlan : public Plan
{
    public:
        DMLPlan(PlanTag tag, std::shared_ptr<Plan> subplan,std::string tab_name,
                std::vector<Value> values, std::vector<Condition> conds,
                std::vector<SetClause> set_clauses)
        {
            Plan::tag = tag;
            subplan_ = std::move(subplan);
            tab_name_ = std::move(tab_name);
            values_ = std::move(values);
            conds_ = std::move(conds);
            set_clauses_ = std::move(set_clauses);
        }
        DMLPlan(PlanTag tag, std::string tab_name, std::string file_name)
        {
            Plan::tag = tag;
            tab_name_ = std::move(tab_name);
            file_name_ = std::move(file_name);
        }
        ~DMLPlan(){}
        std::shared_ptr<Plan> subplan_;
        std::string tab_name_;
        std::string file_name_;
        std::vector<Value> values_;
        std::vector<Condition> conds_;
        std::vector<SetClause> set_clauses_;
        std::vector<TabCol> output_cols_;
};

// ddl语句, 包括create/drop table; create/drop index;
class DDLPlan : public Plan
{
    public:
        DDLPlan(PlanTag tag, std::string tab_name, std::vector<std::string> col_names, std::vector<ColDef> cols)
        {
            Plan::tag = tag;
            tab_name_ = std::move(tab_name);
            cols_ = std::move(cols);
            tab_col_names_ = std::move(col_names);
        }
        ~DDLPlan(){}
        std::string tab_name_;
        std::vector<std::string> tab_col_names_;
        std::vector<ColDef> cols_;
};

// help; show tables; desc tables; begin; abort; commit; rollback语句对应的plan
class OtherPlan : public Plan
{
    public:
        OtherPlan(PlanTag tag, std::string tab_name)
        {
            Plan::tag = tag;
            tab_name_ = std::move(tab_name);            
        }
        ~OtherPlan(){}
        std::string tab_name_;
};

class SetTransactionIsolationPlan : public Plan
{
    public:
        explicit SetTransactionIsolationPlan(IsolationLevel isolation_level)
        {
            Plan::tag = T_SetTransactionIsolation;
            isolation_level_ = isolation_level;
        }
        ~SetTransactionIsolationPlan(){}
        IsolationLevel isolation_level_;
};

// Set Knob Plan
class SetKnobPlan : public Plan
{
    public:
        SetKnobPlan(ast::SetKnobType knob_type, bool bool_value) {
            Plan::tag = T_SetKnob;
            set_knob_type_ = knob_type;
            bool_value_ = bool_value;
        }
    ast::SetKnobType set_knob_type_;
    bool bool_value_;
};
