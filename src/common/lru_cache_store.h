#pragma once

#include <cstddef>
#include <iterator>
#include <list>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

namespace rmdb {

template <typename Key, typename Value, typename Hash = std::hash<Key>>
class LruCacheStore {
   public:
    explicit LruCacheStore(size_t capacity) : capacity_(capacity) {}

    template <typename Validator>
    std::optional<Value> Lookup(const Key &key, Validator &&validator) {
        std::lock_guard<std::mutex> guard(latch_);
        auto iter = entries_.find(key);
        if (iter == entries_.end()) {
            return std::nullopt;
        }
        auto list_iter = iter->second;
        if (!validator(list_iter->value)) {
            lru_.erase(list_iter);
            entries_.erase(iter);
            return std::nullopt;
        }
        lru_.splice(lru_.begin(), lru_, list_iter);
        return lru_.front().value;
    }

    void Store(Key key, Value value) {
        std::lock_guard<std::mutex> guard(latch_);
        auto existing = entries_.find(key);
        if (existing != entries_.end()) {
            lru_.erase(existing->second);
            entries_.erase(existing);
        }
        lru_.push_front(Entry{std::move(key), std::move(value)});
        entries_[lru_.front().key] = lru_.begin();
        while (entries_.size() > capacity_) {
            auto last = std::prev(lru_.end());
            entries_.erase(last->key);
            lru_.pop_back();
        }
    }

    void Clear() {
        std::lock_guard<std::mutex> guard(latch_);
        entries_.clear();
        lru_.clear();
    }

    size_t SizeForTest() const {
        std::lock_guard<std::mutex> guard(latch_);
        return entries_.size();
    }

   private:
    struct Entry {
        Key key;
        Value value;
    };

    size_t capacity_;
    mutable std::mutex latch_;
    std::list<Entry> lru_;
    std::unordered_map<Key, typename std::list<Entry>::iterator, Hash> entries_;
};

}  // namespace rmdb
