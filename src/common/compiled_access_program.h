#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "common/common.h"
#include "common/index_runtime.h"
#include "common/types.h"

namespace rmdb {

// Immutable, catalog-bound part of an index access path. Literal addresses are
// deliberately not cached: cloned template plans own different Value objects,
// so execution binds them through condition_index on every invocation.
class CompiledAccessProgram {
   public:
    struct Predicate {
        size_t condition_index{0};
        int lhs_offset{0};
        int rhs_offset{0};
        int len{0};
        ColType type{TYPE_INT};
        CompOp op{OP_EQ};
        bool rhs_is_literal{false};
    };

    struct RangeColumn {
        int key_offset{0};
        int len{0};
        ColType type{TYPE_INT};
        std::vector<size_t> lower_conditions;
        std::vector<size_t> upper_conditions;
    };

    static std::shared_ptr<const CompiledAccessProgram> BuildIndex(
        const IndexMeta &index, const std::vector<Condition> &conditions,
        const std::string &visible_name, rmdb::u64 schema_epoch) {
        auto program = std::shared_ptr<CompiledAccessProgram>(new CompiledAccessProgram());
        program->schema_epoch_ = schema_epoch;
        program->index_id_ = index.index_id;
        program->logical_key_len_ = index.logical_col_tot_len();
        program->predicates_.reserve(conditions.size());
        for (size_t i = 0; i < conditions.size(); ++i) {
            const auto &condition = conditions[i];
            if (condition.lhs_col.tab_name != visible_name) {
                return nullptr;
            }
            auto lhs = find_index_logical_col(index, condition.lhs_col.col_name);
            if (!lhs) {
                return nullptr;
            }
            Predicate predicate;
            predicate.condition_index = i;
            predicate.lhs_offset = lhs.offset;
            predicate.len = lhs.col->len;
            predicate.type = lhs.col->type;
            predicate.op = condition.op;
            predicate.rhs_is_literal = condition.is_rhs_val;
            if (!condition.is_rhs_val) {
                if (condition.rhs_col.tab_name != visible_name) {
                    return nullptr;
                }
                auto rhs = find_index_logical_col(index, condition.rhs_col.col_name);
                if (!rhs || rhs.col->type != lhs.col->type || rhs.col->len != lhs.col->len) {
                    return nullptr;
                }
                predicate.rhs_offset = rhs.offset;
            }
            program->predicates_.push_back(predicate);
        }
        int key_offset = 0;
        for (const auto &index_col : index.cols) {
            auto it = std::find_if(conditions.begin(), conditions.end(), [&](const Condition &condition) {
                return condition.is_rhs_val && condition.op == OP_EQ &&
                       condition.rhs_val.raw != nullptr &&
                       condition.lhs_col.tab_name == visible_name &&
                       condition.lhs_col.col_name == index_col.name;
            });
            if (it == conditions.end()) {
                break;
            }
            program->equality_conditions_.push_back(
                static_cast<size_t>(std::distance(conditions.begin(), it)));
            ++program->equality_prefix_cols_;
            key_offset += index_col.len;
        }
        program->exact_unique_key_ = index.unique && program->equality_prefix_cols_ == index.col_num;
        if (program->equality_prefix_cols_ < index.col_num) {
            const auto &range_col = index.cols[static_cast<size_t>(program->equality_prefix_cols_)];
            RangeColumn range;
            range.key_offset = key_offset;
            range.len = range_col.len;
            range.type = range_col.type;
            for (size_t i = 0; i < conditions.size(); ++i) {
                const auto &condition = conditions[i];
                if (!condition.is_rhs_val || condition.rhs_val.raw == nullptr ||
                    condition.lhs_col.tab_name != visible_name ||
                    condition.lhs_col.col_name != range_col.name) {
                    continue;
                }
                if (condition.op == OP_GT || condition.op == OP_GE) {
                    range.lower_conditions.push_back(i);
                } else if (condition.op == OP_LT || condition.op == OP_LE) {
                    range.upper_conditions.push_back(i);
                }
            }
            program->range_column_ = std::move(range);
        }
        return program;
    }

    bool exact_unique_key() const { return exact_unique_key_; }
    int equality_prefix_cols() const { return equality_prefix_cols_; }

    bool BindCoveringOutput(const IndexMeta &index, const std::vector<ColMeta> &columns,
                            std::vector<int> *offsets) const {
        if (offsets == nullptr || index.index_id != index_id_ ||
            index.logical_col_tot_len() != logical_key_len_) {
            return false;
        }
        offsets->clear();
        offsets->reserve(columns.size());
        for (const auto &column : columns) {
            auto key_column = find_index_logical_col(index, column.name);
            if (!key_column) {
                offsets->clear();
                return false;
            }
            offsets->push_back(key_column.offset);
        }
        return true;
    }

    bool MaterializeRange(const IndexMeta &index, const std::vector<Condition> &conditions,
                          IndexRangeSpec *spec) const {
        if (spec == nullptr || conditions.size() != predicates_.size() ||
            index.index_id != index_id_ || index.logical_col_tot_len() != logical_key_len_) {
            return false;
        }
        *spec = {};
        spec->lower_key.assign(static_cast<size_t>(index.col_tot_len), '\0');
        spec->upper_key.assign(static_cast<size_t>(index.col_tot_len), '\0');
        std::vector<bool> consumed(conditions.size(), false);
        int offset = 0;
        for (size_t condition_index : equality_conditions_) {
            if (condition_index >= conditions.size() || conditions[condition_index].rhs_val.raw == nullptr) {
                return false;
            }
            const auto &index_col = index.cols[static_cast<size_t>(spec->equality_prefix_cols)];
            std::memcpy(spec->lower_key.data() + offset, conditions[condition_index].rhs_val.raw->data,
                        index_col.len);
            std::memcpy(spec->upper_key.data() + offset, conditions[condition_index].rhs_val.raw->data,
                        index_col.len);
            consumed[condition_index] = true;
            offset += index_col.len;
            ++spec->equality_prefix_cols;
            spec->equality_prefix_len = offset;
        }

        auto choose_bound = [&](const std::vector<size_t> &candidates, bool lower) -> std::optional<size_t> {
            std::optional<size_t> best;
            for (size_t candidate : candidates) {
                if (candidate >= conditions.size() || conditions[candidate].rhs_val.raw == nullptr) return {};
                if (!best.has_value()) {
                    best = candidate;
                    continue;
                }
                int cmp = compare_index_raw_value(conditions[candidate].rhs_val.raw->data,
                                                  conditions[*best].rhs_val.raw->data,
                                                  range_column_->type, range_column_->len);
                bool candidate_strict = conditions[candidate].op == (lower ? OP_GT : OP_LT);
                bool best_strict = conditions[*best].op == (lower ? OP_GT : OP_LT);
                if ((lower && cmp > 0) || (!lower && cmp < 0) ||
                    (cmp == 0 && candidate_strict && !best_strict)) {
                    best = candidate;
                }
            }
            return best;
        };

        std::optional<size_t> lower;
        std::optional<size_t> upper;
        if (range_column_.has_value()) {
            lower = choose_bound(range_column_->lower_conditions, true);
            upper = choose_bound(range_column_->upper_conditions, false);
        }
        if (lower.has_value()) {
            const auto &condition = conditions[*lower];
            bool strict = condition.op == OP_GT;
            if (!strict) consumed[*lower] = true;
            std::memcpy(spec->lower_key.data() + range_column_->key_offset,
                        condition.rhs_val.raw->data, range_column_->len);
            fill_index_key_min_suffix(index, range_column_->key_offset + range_column_->len,
                                      spec->lower_key.data());
            spec->lower_lookup = IndexBoundLookup::LowerBound;
        } else if (spec->equality_prefix_len > 0) {
            fill_index_key_min_suffix(index, spec->equality_prefix_len, spec->lower_key.data());
            spec->lower_lookup = IndexBoundLookup::LowerBound;
        }
        if (upper.has_value()) {
            const auto &condition = conditions[*upper];
            bool strict = condition.op == OP_LT;
            consumed[*upper] = true;
            std::memcpy(spec->upper_key.data() + range_column_->key_offset,
                        condition.rhs_val.raw->data, range_column_->len);
            int suffix = range_column_->key_offset + range_column_->len;
            if (strict) {
                fill_index_key_min_suffix(index, suffix, spec->upper_key.data());
                spec->upper_lookup = IndexBoundLookup::LowerBound;
            } else {
                fill_index_key_max_suffix(index, suffix, spec->upper_key.data());
                spec->upper_lookup = IndexBoundLookup::UpperBound;
            }
        } else if (spec->equality_prefix_len > 0) {
            // 与 build_index_range_spec 一致:仅有等值前缀时显式构造
            // prefix + max suffix 上界,不依赖 matches_prefix() 提前停止。
            fill_index_key_max_suffix(index, spec->equality_prefix_len, spec->upper_key.data());
            spec->upper_lookup = IndexBoundLookup::UpperBound;
        }
        spec->scan_prefix_len = spec->equality_prefix_len;
        spec->exact_unique_key = exact_unique_key_;
        spec->all_conditions_consumed = std::all_of(consumed.begin(), consumed.end(), [](bool value) {
            return value;
        });
        return true;
    }

    bool Matches(const IndexMeta &index, size_t condition_count, rmdb::u64 schema_epoch) const {
        return schema_epoch_ == schema_epoch && index_id_ == index.index_id &&
               logical_key_len_ == index.logical_col_tot_len() && predicates_.size() == condition_count;
    }

    bool EvaluateIndexKey(const char *key, const std::vector<Condition> &conditions) const {
        if (key == nullptr || conditions.size() != predicates_.size()) {
            return false;
        }
        for (const auto &predicate : predicates_) {
            const auto &condition = conditions[predicate.condition_index];
            const char *rhs = nullptr;
            if (predicate.rhs_is_literal) {
                if (!condition.is_rhs_val || condition.rhs_val.raw == nullptr) {
                    return false;
                }
                rhs = condition.rhs_val.raw->data;
            } else {
                rhs = key + predicate.rhs_offset;
            }
            int cmp = compare_index_raw_value(key + predicate.lhs_offset, rhs,
                                              predicate.type, predicate.len);
            if (!Compare(cmp, predicate.op)) {
                return false;
            }
        }
        return true;
    }

   private:
    static bool Compare(int cmp, CompOp op) {
        switch (op) {
            case OP_EQ: return cmp == 0;
            case OP_NE: return cmp != 0;
            case OP_LT: return cmp < 0;
            case OP_GT: return cmp > 0;
            case OP_LE: return cmp <= 0;
            case OP_GE: return cmp >= 0;
        }
        return false;
    }

    rmdb::u64 schema_epoch_{0};
    rmdb::u64 index_id_{0};
    int logical_key_len_{0};
    int equality_prefix_cols_{0};
    bool exact_unique_key_{false};
    std::vector<size_t> equality_conditions_;
    std::optional<RangeColumn> range_column_;
    std::vector<Predicate> predicates_;
};

class CompiledMutationProgram {
   public:
    struct SetSlot {
        int lhs_offset{0};
        int len{0};
        ColType lhs_type{TYPE_INT};
        SetOp op{SetOp::ASSIGN};
        bool reversed{false};
        bool rhs_is_col{false};
        int rhs_offset{0};
        ColType rhs_type{TYPE_INT};
    };

    static std::shared_ptr<const CompiledMutationProgram> BuildUpdate(
        const TabMeta &table, const std::vector<SetClause> &clauses, rmdb::u64 schema_epoch) {
        auto program = std::shared_ptr<CompiledMutationProgram>(new CompiledMutationProgram());
        program->schema_epoch_ = schema_epoch;
        program->slots_.reserve(clauses.size());
        program->index_touched_.assign(table.indexes.size(), false);
        for (const auto &clause : clauses) {
            auto lhs = table.get_col(clause.lhs.col_name);
            SetSlot slot;
            slot.lhs_offset = lhs->offset;
            slot.len = lhs->len;
            slot.lhs_type = lhs->type;
            slot.op = clause.op;
            slot.reversed = clause.reversed_operands;
            slot.rhs_is_col = clause.rhs_is_col;
            if (clause.rhs_is_col) {
                auto rhs = table.get_col(clause.rhs_col.col_name);
                slot.rhs_offset = rhs->offset;
                slot.rhs_type = rhs->type;
            }
            program->slots_.push_back(slot);
            for (size_t i = 0; i < table.indexes.size(); ++i) {
                if (index_covers_col(table.indexes[i], clause.lhs.col_name)) {
                    program->index_touched_[i] = true;
                    program->touches_index_ = true;
                    program->key_conflict_required_ =
                        program->key_conflict_required_ || table.indexes[i].unique;
                }
            }
            program->key_conflict_required_ = program->key_conflict_required_ || clause.lhs.col_name == "id";
        }
        return program;
    }

    bool Matches(size_t clause_count, size_t index_count, rmdb::u64 schema_epoch) const {
        return schema_epoch_ == schema_epoch && slots_.size() == clause_count &&
               index_touched_.size() == index_count;
    }
    const std::vector<SetSlot> &slots() const { return slots_; }
    const std::vector<bool> &index_touched() const { return index_touched_; }
    bool touches_index() const { return touches_index_; }
    bool key_conflict_required() const { return key_conflict_required_; }

   private:
    rmdb::u64 schema_epoch_{0};
    std::vector<SetSlot> slots_;
    std::vector<bool> index_touched_;
    bool touches_index_{false};
    bool key_conflict_required_{false};
};

}  // namespace rmdb
