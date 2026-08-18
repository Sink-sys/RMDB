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

#include "ix_defs.h"
#include "ix_index_handle.h"

// 逻辑 key 边界的叶子遍历游标。
//
// 与传统 Iid 边界不同,本游标不长期保存 (page_no, slot_no) 形式的结束位置:
//   - 起始位置由 create_scan 在持有起始叶页 shared latch 时按 lower key seek 得到;
//   - 每个叶页的批量扫描终点在当前叶页 shared latch 保护下按 upper key 重新计算,
//     叶页插入/删除/分裂导致的槽位移动不会让结束边界失效;
//   - 达到上界时置 exhausted_,不再依赖"iid == end_"判断结束。
class IxScan : public RecScan {
    const IxIndexHandle *ih_;
    Iid iid_;  // 当前 (叶页, 槽位);exhausted_ 后无效
    std::string upper_key_;
    IxBoundMode upper_mode_{IxBoundMode::None};
    bool has_upper_bound_{false};
    bool exhausted_{false};
    IxReadGuard read_guard_;
    IxNodeHandleGuard current_guard_;

    // 推进到下一个合法位置;若已达逻辑末尾或索引耗尽则置 exhausted_。
    void normalize();

   public:
    IxScan(const IxIndexHandle *ih, const IxScanBounds &bounds, const Iid &start,
           IxNodeHandleGuard current_guard)
        : ih_(ih),
          iid_(start),
          upper_key_(bounds.upper_key),
          upper_mode_(bounds.upper_mode),
          has_upper_bound_(bounds.upper_mode != IxBoundMode::None),
          read_guard_(ih),
          current_guard_(std::move(current_guard)) {
        assert(!current_guard_ || current_guard_.latch_mode() == LatchMode::Shared);
        normalize();
    }

    void next() override;

    bool is_end() const override { return exhausted_; }

    Rid rid() const override;

    const Iid &iid() const { return iid_; }

    const char *key() const;

    int current_leaf_slot() const { return iid_.slot_no; }

    int current_leaf_scan_end_slot() const;

    int current_leaf_scan_remaining() const { return current_leaf_scan_end_slot() - iid_.slot_no; }

    Rid current_leaf_rid_at(int slot_no) const;

    const char *current_leaf_key_at(int slot_no) const;

    void advance_current_leaf(int slots);
};
