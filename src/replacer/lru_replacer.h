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

#include <atomic>
#include <memory>
#include <thread>

#include "common/config.h"
#include "common/types.h"
#include "replacer/replacer.h"

/*
LRUReplacer implements the buffer replacement policy.  The current
implementation uses a sharded CLOCK policy while preserving the original
interface name used by the rest of the codebase.
*/
class LRUReplacer : public Replacer {
   public:
    /**
     * @description: 创建一个新的LRUReplacer
     * @param {size_t} num_pages LRUReplacer最多需要存储的page数量
     */
    explicit LRUReplacer(size_t num_pages);

    ~LRUReplacer() override;

    bool victim(frame_id_t *frame_id) override;

    void pin(frame_id_t frame_id) override;

    void unpin(frame_id_t frame_id) override;

    void unpin_cold(frame_id_t frame_id) override;

    size_t Size() override;

   private:
    size_t max_size_;   // 最大容量（与缓冲池的容量相同）
    size_t shard_count_{1};
    std::atomic<size_t> size_{0};
    std::atomic<size_t> victim_hand_{0};
    struct alignas(64) Shard {
        std::atomic<size_t> hand{0};
        size_t frame_count{0};
    };
    std::unique_ptr<Shard[]> shards_;
    // 0: pinned/not present, 1: cold evictable, 2: referenced evictable.
    std::unique_ptr<std::atomic<rmdb::u8>[]> states_;

    frame_id_t frame_for_shard_pos(size_t shard_idx, size_t pos) const;

    void unpin_internal(frame_id_t frame_id, bool cold);
};
