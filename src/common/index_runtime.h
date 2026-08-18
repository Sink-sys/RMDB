#pragma once

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "common/binary.h"
#include "index/ix.h"
#include "system/sm.h"

namespace rmdb {

struct IndexBinding {
    const IndexMeta *meta;
    IxIndexHandle *ih;
};

inline IxIndexHandle *resolve_index_handle(SmManager *sm_manager, const std::string &tab_name,
                                           const IndexMeta &index) {
    return sm_manager->ihs_.at(sm_manager->get_ix_manager()->get_index_name(tab_name, index)).get();
}

inline std::vector<IndexBinding> bind_table_indexes(SmManager *sm_manager, const std::string &tab_name,
                                                    const TabMeta &tab) {
    std::vector<IndexBinding> bindings;
    bindings.reserve(tab.indexes.size());
    for (const auto &index : tab.indexes) {
        bindings.push_back(IndexBinding{&index, resolve_index_handle(sm_manager, tab_name, index)});
    }
    return bindings;
}

inline void build_index_logical_key_into(const IndexMeta &index, const char *record_data, char *key) {
    int offset = 0;
    for (int i = 0; i < index.col_num; ++i) {
        memcpy(key + offset, record_data + index.cols[i].offset, index.cols[i].len);
        offset += index.cols[i].len;
    }
}

inline void append_index_rid_suffix(const IndexMeta &index, const Rid &rid, char *key) {
    if (index.unique) {
        return;
    }
    int offset = index.logical_col_tot_len();
    memcpy(key + offset, &rid.page_no, sizeof(int));
    offset += sizeof(int);
    memcpy(key + offset, &rid.slot_no, sizeof(int));
}

inline void build_index_key_into(const IndexMeta &index, const char *record_data, char *key) {
    build_index_logical_key_into(index, record_data, key);
    if (!index.unique) {
        memset(key + index.logical_col_tot_len(), 0, sizeof(Rid));
    }
}

inline void build_index_key_into(const IndexMeta &index, const char *record_data, const Rid &rid, char *key) {
    build_index_logical_key_into(index, record_data, key);
    append_index_rid_suffix(index, rid, key);
}

inline char *build_index_key_into(const IndexMeta &index, const char *record_data, std::string *key) {
    if (key->size() != static_cast<size_t>(index.col_tot_len)) {
        key->resize(index.col_tot_len);
    }
    build_index_key_into(index, record_data, key->data());
    return key->data();
}

inline char *build_index_key_into(const IndexMeta &index, const char *record_data, const Rid &rid, std::string *key) {
    if (key->size() != static_cast<size_t>(index.col_tot_len)) {
        key->resize(index.col_tot_len);
    }
    build_index_key_into(index, record_data, rid, key->data());
    return key->data();
}

inline std::unique_ptr<char[]> build_index_key(const IndexMeta &index, const char *record_data) {
    auto key = std::make_unique<char[]>(index.col_tot_len);
    build_index_key_into(index, record_data, key.get());
    return key;
}

inline std::unique_ptr<char[]> build_index_key(const IndexMeta &index, const char *record_data, const Rid &rid) {
    auto key = std::make_unique<char[]>(index.col_tot_len);
    build_index_key_into(index, record_data, rid, key.get());
    return key;
}

struct IndexKeyColRef {
    const ColMeta *col = nullptr;
    int offset = 0;

    explicit operator bool() const { return col != nullptr; }
};

inline IndexKeyColRef find_index_logical_col(const IndexMeta &index, const std::string &col_name) {
    int offset = 0;
    for (const auto &col : index.cols) {
        if (col.name == col_name) {
            return {&col, offset};
        }
        offset += col.len;
    }
    return {};
}

inline bool index_covers_col(const IndexMeta &index, const std::string &col_name) {
    return static_cast<bool>(find_index_logical_col(index, col_name));
}

inline bool index_covers_cols(const IndexMeta &index, const std::vector<TabCol> &cols,
                              const std::string &visible_name) {
    for (const auto &col : cols) {
        if (col.col_name.empty() || (!col.tab_name.empty() && col.tab_name != visible_name)) {
            continue;
        }
        if (!index_covers_col(index, col.col_name)) {
            return false;
        }
    }
    return true;
}

inline bool index_covers_conditions(const IndexMeta &index, const std::vector<Condition> &conds,
                                    const std::string &visible_name) {
    for (const auto &cond : conds) {
        if (cond.lhs_col.tab_name == visible_name && !index_covers_col(index, cond.lhs_col.col_name)) {
            return false;
        }
        if (!cond.is_rhs_val && cond.rhs_col.tab_name == visible_name &&
            !index_covers_col(index, cond.rhs_col.col_name)) {
            return false;
        }
    }
    return true;
}

inline bool index_covers_required_and_conditions(const IndexMeta &index, const std::vector<TabCol> &required_cols,
                                                 const std::vector<Condition> &conds,
                                                 const std::string &visible_name) {
    return index_covers_cols(index, required_cols, visible_name) &&
           index_covers_conditions(index, conds, visible_name);
}

inline int compare_index_raw_value(const char *lhs, const char *rhs, ColType type, int len) {
    if (type == TYPE_INT) {
        int l = load_unaligned<int>(lhs);
        int r = load_unaligned<int>(rhs);
        return (l > r) - (l < r);
    }
    if (type == TYPE_FLOAT) {
        float l = load_unaligned<float>(lhs);
        float r = load_unaligned<float>(rhs);
        return (l > r) - (l < r);
    }
    return std::memcmp(lhs, rhs, len);
}

inline void fill_index_col_min(char *dst, ColType type, int len) {
    if (type == TYPE_INT) {
        int value = std::numeric_limits<int>::min();
        std::memcpy(dst, &value, sizeof(int));
    } else if (type == TYPE_FLOAT) {
        float value = -std::numeric_limits<float>::max();
        std::memcpy(dst, &value, sizeof(float));
    } else {
        std::memset(dst, 0, len);
    }
}

inline void fill_index_col_max(char *dst, ColType type, int len) {
    if (type == TYPE_INT) {
        int value = std::numeric_limits<int>::max();
        std::memcpy(dst, &value, sizeof(int));
    } else if (type == TYPE_FLOAT) {
        float value = std::numeric_limits<float>::max();
        std::memcpy(dst, &value, sizeof(float));
    } else {
        std::memset(dst, 0xff, len);
    }
}

inline void fill_index_key_min_suffix(const IndexMeta &index, int start_offset, char *key) {
    int offset = 0;
    for (const auto &col : index.cols) {
        if (offset + col.len > start_offset) {
            int fill_offset = std::max(offset, start_offset);
            if (fill_offset == offset) {
                fill_index_col_min(key + offset, col.type, col.len);
            } else {
                std::memset(key + fill_offset, 0, offset + col.len - fill_offset);
            }
        }
        offset += col.len;
    }
    if (!index.unique && offset < index.col_tot_len) {
        int min_value = std::numeric_limits<int>::min();
        std::memcpy(key + offset, &min_value, sizeof(int));
        offset += sizeof(int);
        std::memcpy(key + offset, &min_value, sizeof(int));
    }
}

inline void fill_index_key_max_suffix(const IndexMeta &index, int start_offset, char *key) {
    int offset = 0;
    for (const auto &col : index.cols) {
        if (offset + col.len > start_offset) {
            int fill_offset = std::max(offset, start_offset);
            if (fill_offset == offset) {
                fill_index_col_max(key + offset, col.type, col.len);
            } else {
                std::memset(key + fill_offset, 0xff, offset + col.len - fill_offset);
            }
        }
        offset += col.len;
    }
    if (!index.unique && offset < index.col_tot_len) {
        int max_value = std::numeric_limits<int>::max();
        std::memcpy(key + offset, &max_value, sizeof(int));
        offset += sizeof(int);
        std::memcpy(key + offset, &max_value, sizeof(int));
    }
}

enum class IndexBoundLookup {
    LeafBegin,
    LowerBound,
    UpperBound,
    LeafEnd,
};

struct IndexRangeSpec {
    std::string lower_key;
    std::string upper_key;
    IndexBoundLookup lower_lookup = IndexBoundLookup::LeafBegin;
    IndexBoundLookup upper_lookup = IndexBoundLookup::LeafEnd;
    int equality_prefix_cols = 0;
    int equality_prefix_len = 0;
    int scan_prefix_len = 0;
    bool exact_unique_key = false;
    bool all_conditions_consumed = false;
};

inline IndexRangeSpec build_index_range_spec(const IndexMeta &index, const std::vector<Condition> &conds,
                                             const std::string &visible_name) {
    IndexRangeSpec spec;
    spec.lower_key.assign(static_cast<size_t>(index.col_tot_len), '\0');
    spec.upper_key.assign(static_cast<size_t>(index.col_tot_len), '\0');
    std::vector<bool> consumed_conds(conds.size(), false);

    int offset = 0;
    for (const auto &index_col : index.cols) {
        auto cond_it = std::find_if(conds.begin(), conds.end(), [&](const Condition &cond) {
            return cond.is_rhs_val && cond.op == OP_EQ && cond.rhs_val.raw != nullptr &&
                   cond.lhs_col.tab_name == visible_name && cond.lhs_col.col_name == index_col.name;
        });
        if (cond_it == conds.end()) {
            break;
        }
        consumed_conds[static_cast<size_t>(std::distance(conds.begin(), cond_it))] = true;
        std::memcpy(&spec.lower_key[static_cast<size_t>(offset)], cond_it->rhs_val.raw->data, index_col.len);
        std::memcpy(&spec.upper_key[static_cast<size_t>(offset)], cond_it->rhs_val.raw->data, index_col.len);
        offset += index_col.len;
        spec.equality_prefix_cols++;
        spec.equality_prefix_len = offset;
    }

    struct RangeBound {
        const char *value = nullptr;
        bool strict = false;
        bool set = false;
        size_t cond_idx = 0;
    };
    RangeBound lower;
    RangeBound upper;
    int range_col_offset = spec.equality_prefix_len;
    const ColMeta *range_col = nullptr;
    if (spec.equality_prefix_cols < index.col_num) {
        range_col = &index.cols[static_cast<size_t>(spec.equality_prefix_cols)];
        for (size_t cond_idx = 0; cond_idx < conds.size(); ++cond_idx) {
            const auto &cond = conds[cond_idx];
            if (!cond.is_rhs_val || cond.rhs_val.raw == nullptr || cond.lhs_col.tab_name != visible_name ||
                cond.lhs_col.col_name != range_col->name) {
                continue;
            }
            if (cond.op == OP_GT || cond.op == OP_GE) {
                if (!lower.set ||
                    compare_index_raw_value(cond.rhs_val.raw->data, lower.value, range_col->type, range_col->len) > 0 ||
                    (compare_index_raw_value(cond.rhs_val.raw->data, lower.value, range_col->type, range_col->len) == 0 &&
                     cond.op == OP_GT && !lower.strict)) {
                    lower = {cond.rhs_val.raw->data, cond.op == OP_GT, true, cond_idx};
                }
            } else if (cond.op == OP_LT || cond.op == OP_LE) {
                if (!upper.set ||
                    compare_index_raw_value(cond.rhs_val.raw->data, upper.value, range_col->type, range_col->len) < 0 ||
                    (compare_index_raw_value(cond.rhs_val.raw->data, upper.value, range_col->type, range_col->len) == 0 &&
                     cond.op == OP_LT && !upper.strict)) {
                    upper = {cond.rhs_val.raw->data, cond.op == OP_LT, true, cond_idx};
                }
            }
        }
    }

    if (lower.set && range_col != nullptr) {
        if (!lower.strict) {
            consumed_conds[lower.cond_idx] = true;
        }
        std::memcpy(&spec.lower_key[static_cast<size_t>(range_col_offset)], lower.value, range_col->len);
        int suffix_offset = range_col_offset + range_col->len;
        fill_index_key_min_suffix(index, suffix_offset, spec.lower_key.data());
        spec.lower_lookup = IndexBoundLookup::LowerBound;
    } else if (spec.equality_prefix_len > 0) {
        fill_index_key_min_suffix(index, spec.equality_prefix_len, spec.lower_key.data());
        spec.lower_lookup = IndexBoundLookup::LowerBound;
    }

    if (upper.set && range_col != nullptr) {
        consumed_conds[upper.cond_idx] = true;
        std::memcpy(&spec.upper_key[static_cast<size_t>(range_col_offset)], upper.value, range_col->len);
        int suffix_offset = range_col_offset + range_col->len;
        if (upper.strict) {
            fill_index_key_min_suffix(index, suffix_offset, spec.upper_key.data());
            spec.upper_lookup = IndexBoundLookup::LowerBound;
        } else {
            fill_index_key_max_suffix(index, suffix_offset, spec.upper_key.data());
            spec.upper_lookup = IndexBoundLookup::UpperBound;
        }
    } else if (spec.equality_prefix_len > 0) {
        // 仅有等值前缀、无显式范围上界:显式构造 prefix + max suffix 上界,
        // 而不是依赖 matches_prefix() 提前停止。物理 Iid 边界在叶页槽位移动
        // 后会失效,导致等值前缀范围扫描漏行(IndexScan / COUNT / MIN)。
        fill_index_key_max_suffix(index, spec.equality_prefix_len, spec.upper_key.data());
        spec.upper_lookup = IndexBoundLookup::UpperBound;
    }

    spec.scan_prefix_len = spec.equality_prefix_len;
    spec.exact_unique_key = index.unique && spec.equality_prefix_cols == index.col_num;
    spec.all_conditions_consumed = std::all_of(consumed_conds.begin(), consumed_conds.end(),
                                               [](bool consumed) { return consumed; });
    return spec;
}

}  // namespace rmdb
