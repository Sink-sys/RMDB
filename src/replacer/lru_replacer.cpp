/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "lru_replacer.h"

#include <algorithm>

namespace {
size_t compute_shard_count(size_t num_pages) {
    if (num_pages < 64) {
        return 1;
    }
    size_t desired = std::thread::hardware_concurrency();
    if (desired == 0) {
        desired = 8;
    }
    desired *= 2;
    return std::max<size_t>(1, std::min<size_t>(64, std::min<size_t>(desired, num_pages)));
}
}

LRUReplacer::LRUReplacer(size_t num_pages) {
    max_size_ = num_pages;
    shard_count_ = compute_shard_count(num_pages);
    shards_ = std::make_unique<Shard[]>(shard_count_);
    states_ = std::make_unique<std::atomic<rmdb::u8>[]>(num_pages);
    for (size_t frame_idx = 0; frame_idx < num_pages; ++frame_idx) {
        states_[frame_idx].store(0, std::memory_order_relaxed);
    }
    for (size_t shard_idx = 0; shard_idx < shard_count_; ++shard_idx) {
        shards_[shard_idx].frame_count =
            shard_idx < max_size_ ? ((max_size_ - 1 - shard_idx) / shard_count_) + 1 : 0;
    }
}

LRUReplacer::~LRUReplacer() = default;  

frame_id_t LRUReplacer::frame_for_shard_pos(size_t shard_idx, size_t pos) const {
    return static_cast<frame_id_t>(shard_idx + pos * shard_count_);
}

/**
 * @description: 使用CLOCK策略删除一个victim frame，并返回该frame的id
 * @param {frame_id_t*} frame_id 被移除的frame的id，如果没有frame被移除返回nullptr
 * @return {bool} 如果成功淘汰了一个页面则返回true，否则返回false
 */
bool LRUReplacer::victim(frame_id_t* frame_id) {
    if (frame_id == nullptr) {
        return false;
    }
    size_t start = victim_hand_.fetch_add(1, std::memory_order_relaxed) % shard_count_;
    for (size_t i = 0; i < shard_count_; ++i) {
        size_t shard_idx = (start + i) % shard_count_;
        Shard &shard = shards_[shard_idx];
        if (shard.frame_count == 0) {
            continue;
        }

        const size_t scan_limit = shard.frame_count * 2;
        for (size_t scanned = 0; scanned < scan_limit; ++scanned) {
            size_t pos = shard.hand.fetch_add(1, std::memory_order_relaxed) % shard.frame_count;
            frame_id_t candidate = frame_for_shard_pos(shard_idx, pos);
            rmdb::u8 state = states_[candidate].load(std::memory_order_acquire);
            if (state == 0) {
                continue;
            }
            if (state == 2) {
                states_[candidate].compare_exchange_strong(state, 1, std::memory_order_acq_rel,
                                                           std::memory_order_acquire);
                continue;
            }
            if (!states_[candidate].compare_exchange_strong(state, 0, std::memory_order_acq_rel,
                                                            std::memory_order_acquire)) {
                continue;
            }
            size_.fetch_sub(1, std::memory_order_relaxed);
            *frame_id = candidate;
            return true;
        }
    }
    return false;
}

/**
 * @description: 固定指定的frame，即该页面无法被淘汰
 * @param {frame_id_t} 需要固定的frame的id
 */
void LRUReplacer::pin(frame_id_t frame_id) {
    if (frame_id < 0 || static_cast<size_t>(frame_id) >= max_size_) {
        return;
    }
    rmdb::u8 previous = states_[frame_id].exchange(0, std::memory_order_acq_rel);
    if (previous == 0) {
        return;
    }
    size_.fetch_sub(1, std::memory_order_relaxed);
}

/**
 * @description: 取消固定一个frame，代表该页面可以被淘汰
 * @param {frame_id_t} frame_id 取消固定的frame的id
 */
void LRUReplacer::unpin_internal(frame_id_t frame_id, bool cold) {
    if (frame_id < 0 || static_cast<size_t>(frame_id) >= max_size_) {
        return;
    }
    rmdb::u8 expected = 0;
    rmdb::u8 desired = cold ? 1 : 2;
    if (!states_[frame_id].compare_exchange_strong(expected, desired, std::memory_order_acq_rel,
                                                   std::memory_order_acquire)) {
        return;
    }
    size_.fetch_add(1, std::memory_order_relaxed);
}

void LRUReplacer::unpin(frame_id_t frame_id) {
    unpin_internal(frame_id, false);
}

void LRUReplacer::unpin_cold(frame_id_t frame_id) {
    unpin_internal(frame_id, true);
}

/**
 * @description: 获取当前replacer中可以被淘汰的页面数量
 */
size_t LRUReplacer::Size() {
    return size_.load(std::memory_order_relaxed);
}
