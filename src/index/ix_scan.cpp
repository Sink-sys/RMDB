/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "ix_scan.h"

#include <thread>

#include "common/index_test_barrier.h"

/**
 * @brief 推进到下一个合法位置;若当前槽位已越出逻辑上界或索引耗尽则置 exhausted_。
 * 每个叶页的边界在叶页 shared latch 保护下按 upper key 重新计算,不长期保存
 * 物理 Iid,叶页插入/删除/分裂造成的槽位移动不会使扫描越界或漏行。
 */
void IxScan::normalize() {
    if (!current_guard_) {
        exhausted_ = true;
        return;
    }
    assert(current_guard_.latch_mode() == LatchMode::Shared);
    assert(current_guard_->is_leaf_page());
    while (iid_.slot_no >= current_guard_->get_size()) {
        page_id_t next_leaf = ih_->safe_next_leaf(current_guard_.get());
        if (next_leaf == IX_LEAF_HEADER_PAGE || next_leaf == IX_NO_PAGE) {
            exhausted_ = true;
            return;
        }
        current_guard_.reset();
        // 测试 barrier:上一叶页已释放、下一叶页尚未 fetch。此时无 latch 持有,
        // 写者可修改目标叶页;恢复后重新 fetch 并按逻辑上界重算边界。
        if (rmdb::index_test::pause_before_next_leaf.load(std::memory_order_acquire)) {
            rmdb::index_test::pause_before_next_leaf_hits.fetch_add(1, std::memory_order_release);
            while (rmdb::index_test::pause_before_next_leaf.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        }
        current_guard_ = ih_->fetch_node_guard(next_leaf, LatchMode::Shared);
        iid_.slot_no = 0;
        iid_.page_no = next_leaf;
        while (current_guard_->is_tombstone() || current_guard_->get_size() == 0) {
            next_leaf = ih_->safe_next_leaf(current_guard_.get());
            if (next_leaf == IX_LEAF_HEADER_PAGE || next_leaf == IX_NO_PAGE) {
                exhausted_ = true;
                return;
            }
            current_guard_.reset();
            current_guard_ = ih_->fetch_node_guard(next_leaf, LatchMode::Shared);
            iid_.page_no = next_leaf;
        }
    }
    if (iid_.slot_no >= current_leaf_scan_end_slot()) {
        exhausted_ = true;
    }
}

/**
 * @brief 移动到下一个键值对。
 */
void IxScan::next() {
    assert(!is_end());
    assert(current_guard_);
    assert(current_guard_.latch_mode() == LatchMode::Shared);
    assert(current_guard_->is_leaf_page());
    assert(iid_.slot_no < current_guard_->get_size());
    iid_.slot_no++;
    normalize();
}

int IxScan::current_leaf_scan_end_slot() const {
    assert(!is_end());
    assert(current_guard_);
    assert(current_guard_.latch_mode() == LatchMode::Shared);
    assert(current_guard_->is_leaf_page());
    assert(iid_.slot_no <= current_guard_->get_size());
    if (!has_upper_bound_) {
        return current_guard_->get_size();
    }
    // 在当前叶页 shared latch 下按逻辑上界重新计算边界槽位(不含该槽位):
    //   LowerBound → 第一个 key >= upper_key(严格上界,key < upper_key)
    //   UpperBound → 第一个 key > upper_key(包含上界,key <= upper_key)
    return upper_mode_ == IxBoundMode::LowerBound ? current_guard_->lower_bound(upper_key_.data())
                                                  : current_guard_->upper_bound(upper_key_.data());
}

Rid IxScan::current_leaf_rid_at(int slot_no) const {
    assert(!is_end());
    assert(current_guard_);
    assert(current_guard_.latch_mode() == LatchMode::Shared);
    assert(current_guard_->is_leaf_page());
    assert(slot_no >= iid_.slot_no);
    assert(slot_no < current_leaf_scan_end_slot());
    return current_guard_->get_rid(slot_no);
}

const char *IxScan::current_leaf_key_at(int slot_no) const {
    assert(!is_end());
    assert(current_guard_);
    assert(current_guard_.latch_mode() == LatchMode::Shared);
    assert(current_guard_->is_leaf_page());
    assert(slot_no >= iid_.slot_no);
    assert(slot_no < current_leaf_scan_end_slot());
    return current_guard_->get_key(slot_no);
}

void IxScan::advance_current_leaf(int slots) {
    assert(slots >= 0);
    if (slots == 0) {
        return;
    }
    assert(!is_end());
    assert(current_guard_);
    assert(current_guard_.latch_mode() == LatchMode::Shared);
    assert(current_guard_->is_leaf_page());
    const int scan_end_slot = current_leaf_scan_end_slot();
    assert(iid_.slot_no + slots <= scan_end_slot);
    iid_.slot_no += slots;
    if (iid_.slot_no < scan_end_slot) {
        return;
    }
    if (iid_.slot_no < current_guard_->get_size()) {
        // 达到逻辑上界(位于叶页中部):后续 key 必然越界,结束扫描。
        exhausted_ = true;
        return;
    }
    // 扫完当前叶页全部槽位,继续下一个叶页;normalize 会重算下页的逻辑边界。
    normalize();
}

Rid IxScan::rid() const {
    assert(!is_end());
    assert(current_guard_);
    assert(current_guard_.latch_mode() == LatchMode::Shared);
    assert(iid_.slot_no < current_guard_->get_size());
    return current_guard_->get_rid(iid_.slot_no);
}

const char *IxScan::key() const {
    assert(!is_end());
    assert(current_guard_);
    assert(current_guard_.latch_mode() == LatchMode::Shared);
    assert(iid_.slot_no < current_guard_->get_size());
    return current_guard_->get_key(iid_.slot_no);
}
