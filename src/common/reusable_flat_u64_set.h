#pragma once

#include <algorithm>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <vector>

#include "common/types.h"

namespace rmdb {

// A cursor-local set for packed RID keys. It uses open addressing instead of
// one allocation per key, and advances a generation on clear() so pooled
// cursors can retain and reuse their storage in O(1).
class ReusableFlatU64Set {
   public:
    bool insert(u64 key) {
        ensure_capacity_for_insert();
        return insert_without_growth(key);
    }

    bool contains(u64 key) const {
        if (keys_.empty()) {
            return false;
        }
        const size_t mask = keys_.size() - 1;
        size_t slot = hash(key) & mask;
        while (generations_[slot] == generation_) {
            if (keys_[slot] == key) {
                return true;
            }
            slot = (slot + 1) & mask;
        }
        return false;
    }

    void clear() {
        size_ = 0;
        ++generation_;
        if (generation_ == 0) {
            std::fill(generations_.begin(), generations_.end(), 0);
            generation_ = 1;
        }
    }

    void reserve(size_t expected_size) {
        if (expected_size == 0 && keys_.empty()) {
            return;
        }
        size_t capacity = keys_.empty() ? kInitialCapacity : keys_.size();
        while (expected_size > max_size_for_capacity(capacity)) {
            if (capacity > std::numeric_limits<size_t>::max() / 2) {
                throw std::length_error("ReusableFlatU64Set capacity overflow");
            }
            capacity *= 2;
        }
        if (capacity > keys_.size()) {
            rehash(capacity);
        }
    }

    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }

   private:
    static constexpr size_t kInitialCapacity = 8;

    static size_t max_size_for_capacity(size_t capacity) {
        return capacity - capacity / 4;
    }

    static size_t hash(u64 key) {
        key ^= key >> 30;
        key *= 0xbf58476d1ce4e5b9ULL;
        key ^= key >> 27;
        key *= 0x94d049bb133111ebULL;
        key ^= key >> 31;
        return static_cast<size_t>(key);
    }

    void ensure_capacity_for_insert() {
        if (keys_.empty()) {
            rehash(kInitialCapacity);
            return;
        }
        if (size_ + 1 > max_size_for_capacity(keys_.size())) {
            if (keys_.size() > std::numeric_limits<size_t>::max() / 2) {
                throw std::length_error("ReusableFlatU64Set capacity overflow");
            }
            rehash(keys_.size() * 2);
        }
    }

    bool insert_without_growth(u64 key) {
        const size_t mask = keys_.size() - 1;
        size_t slot = hash(key) & mask;
        while (generations_[slot] == generation_) {
            if (keys_[slot] == key) {
                return false;
            }
            slot = (slot + 1) & mask;
        }
        keys_[slot] = key;
        generations_[slot] = generation_;
        ++size_;
        return true;
    }

    void rehash(size_t capacity) {
        std::vector<u64> old_keys = std::move(keys_);
        std::vector<u32> old_generations = std::move(generations_);
        const u32 old_generation = generation_;

        keys_.resize(capacity);
        generations_.assign(capacity, 0);
        generation_ = 1;
        size_ = 0;
        for (size_t slot = 0; slot < old_keys.size(); ++slot) {
            if (old_generations[slot] == old_generation) {
                insert_without_growth(old_keys[slot]);
            }
        }
    }

    std::vector<u64> keys_;
    std::vector<u32> generations_;
    u32 generation_{1};
    size_t size_{0};
};

}  // namespace rmdb
