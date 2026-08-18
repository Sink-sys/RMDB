#pragma once

#include <algorithm>
#include <memory>

#include "common/plan_template_materialization.h"

namespace rmdb {

inline bool index_unique_point_lookup(const ScanPlan &scan, const IndexMeta &index) {
    if (!index.unique) {
        return false;
    }
    size_t equality_prefix = 0;
    for (const auto &index_col : index.cols) {
        auto cond_it = std::find_if(scan.conds_.begin(), scan.conds_.end(), [&](const Condition &cond) {
            return cond.is_rhs_val && cond.op == OP_EQ && cond.lhs_col.tab_name == scan.visible_name_ &&
                   cond.lhs_col.col_name == index_col.name;
        });
        if (cond_it == scan.conds_.end()) {
            break;
        }
        equality_prefix++;
    }
    return equality_prefix == static_cast<size_t>(index.col_num);
}

// A mutable plan instance is deliberately owned by one client session.  The
// global plan-template cache remains immutable and may be shared by all
// sessions; executors are still constructed per statement and never escape it.
class SessionPointProgram {
   public:
    static std::unique_ptr<SessionPointProgram> Create(
        std::shared_ptr<const plan_template_detail::PlanTemplate> templ,
        const SqlTemplateCandidate &candidate, Context *context) {
        if (templ == nullptr || templ->skeleton == nullptr) {
            return nullptr;
        }

        auto program = std::unique_ptr<SessionPointProgram>(new SessionPointProgram(std::move(templ)));
        plan_template_detail::PlanCloneBuilder clone_builder;
        program->plan_ = plan_template_detail::clone_plan_skeleton(
            program->templ_->skeleton, &clone_builder, &program->materializer_);
        if (program->plan_ == nullptr || !program->is_exact_unique_point_dml()) {
            return nullptr;
        }
        program->materializer_.set_root(program->plan_);
        if (!program->Rebind(candidate, context)) {
            return nullptr;
        }
        return program;
    }

    bool Rebind(const SqlTemplateCandidate &candidate, Context *context) {
        if (candidate.literals.size() != templ_->literal_count ||
            templ_->slots.size() < candidate.literals.size()) {
            return false;
        }
        for (const auto &slot : templ_->slots) {
            if (slot.source_index >= candidate.literals.size() ||
                !plan_template_detail::apply_literal_slot(slot, candidate.literals[slot.source_index], materializer_,
                                                          context)) {
                return false;
            }
        }
        return true;
    }

    bool ConfigurePreparedBindings(size_t parameter_count,
                                   const std::vector<rmdb::u16> &literal_indexes,
                                   const std::vector<rmdb::u16> &parameter_indexes) {
        if (literal_indexes.size() != parameter_indexes.size()) {
            return false;
        }
        std::vector<int> parameter_by_literal(templ_->literal_count, -1);
        for (size_t i = 0; i < literal_indexes.size(); ++i) {
            size_t literal_index = literal_indexes[i];
            size_t parameter_index = parameter_indexes[i];
            if (literal_index >= parameter_by_literal.size() || parameter_index >= parameter_count ||
                parameter_by_literal[literal_index] != -1) {
                return false;
            }
            parameter_by_literal[literal_index] = static_cast<int>(parameter_index);
        }

        std::vector<bool> bound_literals(templ_->literal_count, false);
        prepared_bindings_.clear();
        prepared_bindings_.reserve(templ_->slots.size());
        for (size_t slot_index = 0; slot_index < templ_->slots.size(); ++slot_index) {
            const auto &slot = templ_->slots[slot_index];
            if (slot.source_index >= parameter_by_literal.size()) {
                prepared_bindings_.clear();
                return false;
            }
            int parameter_index = parameter_by_literal[slot.source_index];
            if (parameter_index >= 0) {
                prepared_bindings_.push_back(
                    PreparedBinding{slot_index, static_cast<size_t>(parameter_index)});
                bound_literals[slot.source_index] = true;
            }
        }
        for (size_t literal_index = 0; literal_index < parameter_by_literal.size(); ++literal_index) {
            if (parameter_by_literal[literal_index] >= 0 && !bound_literals[literal_index]) {
                prepared_bindings_.clear();
                return false;
            }
        }
        prepared_parameter_count_ = parameter_count;
        return !prepared_bindings_.empty();
    }

    template <typename LiteralProvider>
    bool RebindPrepared(size_t parameter_count, LiteralProvider &&literal_provider, Context *context) {
        if (parameter_count != prepared_parameter_count_ || prepared_bindings_.empty()) {
            return false;
        }
        for (const auto &binding : prepared_bindings_) {
            SqlTemplateLiteral literal;
            if (!literal_provider(binding.parameter_index, &literal) ||
                !plan_template_detail::apply_literal_slot(templ_->slots[binding.slot_index], literal,
                                                          materializer_, context)) {
                return false;
            }
        }
        return true;
    }

    std::shared_ptr<Plan> plan() const { return plan_; }

   private:
    struct PreparedBinding {
        size_t slot_index;
        size_t parameter_index;
    };

    explicit SessionPointProgram(std::shared_ptr<const plan_template_detail::PlanTemplate> templ)
        : templ_(std::move(templ)), materializer_(templ_->feedback == nullptr ? 0 : templ_->feedback->nodes.size()) {}

    bool is_exact_unique_point_dml() const {
        auto dml = std::dynamic_pointer_cast<DMLPlan>(plan_);
        if (dml == nullptr || (dml->tag != T_select && dml->tag != T_Update && dml->tag != T_Delete)) {
            return false;
        }

        std::shared_ptr<Plan> access = dml->subplan_;
        if (dml->tag == T_select) {
            auto projection = std::dynamic_pointer_cast<ProjectionPlan>(access);
            if (projection == nullptr) {
                return false;
            }
            access = projection->subplan_;
        }
        auto scan = std::dynamic_pointer_cast<ScanPlan>(access);
        return scan != nullptr && scan->tag == T_IndexScan && scan->runtime_cache_ != nullptr &&
               scan->runtime_cache_->has_index &&
               index_unique_point_lookup(*scan, scan->runtime_cache_->index_meta);
    }

    std::shared_ptr<const plan_template_detail::PlanTemplate> templ_;
    plan_template_detail::PlanMaterializer materializer_;
    std::shared_ptr<Plan> plan_;
    std::vector<PreparedBinding> prepared_bindings_;
    size_t prepared_parameter_count_{0};
};

}  // namespace rmdb
