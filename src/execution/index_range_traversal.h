#pragma once

#include <cstring>
#include <memory>

#include "common/index_runtime.h"
#include "index/ix.h"

class IndexRangeTraversal {
   public:
    IndexRangeTraversal() = default;
    IndexRangeTraversal(IxIndexHandle *index, const rmdb::IndexRangeSpec *range) {
        bind(index, range);
    }

    void bind(IxIndexHandle *index, const rmdb::IndexRangeSpec *range) {
        index_ = index;
        range_ = range;
    }

    // 直接以逻辑 key 边界创建游标:起始 seek 与起始叶页 pin 在 create_scan 内
    // 合并为一次 latch 保护的操作;结束边界在扫描中按 key 判定,不长期保存物理
    // Iid(叶页插入/删除/分裂导致的槽位移动不会使边界失效)。
    std::unique_ptr<IxScan> open_scan() const {
        if (index_ == nullptr || range_ == nullptr) {
            return nullptr;
        }
        IxScanBounds bounds;
        bounds.lower_key = range_->lower_key;
        bounds.upper_key = range_->upper_key;
        bounds.lower_mode = lower_bound_mode(range_->lower_lookup);
        bounds.upper_mode = upper_bound_mode(range_->upper_lookup);
        return index_->create_scan(bounds);
    }

    bool matches_prefix(const char *key) const {
        return range_ != nullptr &&
               (range_->scan_prefix_len <= 0 ||
                std::memcmp(key, range_->lower_key.data(), range_->scan_prefix_len) == 0);
    }

    // 逻辑范围复核:判定 key 是否仍在 [lower, upper] 逻辑区间内。
    // 执行层在扫描中逐条/逐批复核,即便 range_->all_conditions_consumed 为真
    // 也不完全信任扫描结束边界。
    bool logical_range_contains(const char *key) const {
        if (index_ == nullptr || range_ == nullptr || key == nullptr) {
            return true;
        }
        switch (range_->lower_lookup) {
            case rmdb::IndexBoundLookup::LowerBound:
                if (index_->compare_keys(key, range_->lower_key.data()) < 0) {
                    return false;
                }
                break;
            case rmdb::IndexBoundLookup::UpperBound:
                if (index_->compare_keys(key, range_->lower_key.data()) <= 0) {
                    return false;
                }
                break;
            default:
                break;
        }
        switch (range_->upper_lookup) {
            case rmdb::IndexBoundLookup::LowerBound:  // 严格上界:key < upper_key
                if (index_->compare_keys(key, range_->upper_key.data()) >= 0) {
                    return false;
                }
                break;
            case rmdb::IndexBoundLookup::UpperBound:  // 包含上界:key <= upper_key
                if (index_->compare_keys(key, range_->upper_key.data()) > 0) {
                    return false;
                }
                break;
            default:
                break;
        }
        return true;
    }

   private:
    static IxBoundMode lower_bound_mode(rmdb::IndexBoundLookup lookup) {
        switch (lookup) {
            case rmdb::IndexBoundLookup::LowerBound: return IxBoundMode::LowerBound;
            case rmdb::IndexBoundLookup::UpperBound: return IxBoundMode::UpperBound;
            default: return IxBoundMode::None;
        }
    }

    static IxBoundMode upper_bound_mode(rmdb::IndexBoundLookup lookup) {
        switch (lookup) {
            case rmdb::IndexBoundLookup::LowerBound: return IxBoundMode::LowerBound;
            case rmdb::IndexBoundLookup::UpperBound: return IxBoundMode::UpperBound;
            default: return IxBoundMode::None;
        }
    }

    IxIndexHandle *index_{nullptr};
    const rmdb::IndexRangeSpec *range_{nullptr};
};
