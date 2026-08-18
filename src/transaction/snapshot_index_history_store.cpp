#include "snapshot_index_history_store.h"

#include <algorithm>
#include <functional>

namespace rmdb {

size_t SnapshotIndexHistoryStore::KeyHash::operator()(const Key &key) const {
    size_t seed = std::hash<std::string>{}(key.table_name);
    size_t value = std::hash<rmdb::u64>{}(key.index_id);
    return seed ^ (value + 0x9e3779b9U + (seed << 6U) + (seed >> 2U));
}

size_t SnapshotIndexHistoryStore::shard_index(const Key &key) const {
    return KeyHash{}(key) % kShardCount;
}

void SnapshotIndexHistoryStore::Record(const std::string &table_name, rmdb::u64 index_id,
                                       const Rid &rid, timestamp_t retire_ts) {
    if (retire_ts == INVALID_TS) {
        return;
    }
    Key key{table_name, index_id};
    Shard &shard = shards_[shard_index(key)];
    std::lock_guard<std::mutex> guard(shard.mutex);
    auto &entries = shard.entries[key];
    auto insert_at = std::upper_bound(entries.begin(), entries.end(), retire_ts,
                                      [](timestamp_t ts, const Entry &entry) { return ts < entry.retire_ts; });
    entries.insert(insert_at, Entry{retire_ts, rid});
}

std::vector<Rid> SnapshotIndexHistoryStore::Lookup(const std::string &table_name,
                                                   rmdb::u64 index_id,
                                                   timestamp_t read_ts) {
    Key key{table_name, index_id};
    Shard &shard = shards_[shard_index(key)];
    std::lock_guard<std::mutex> guard(shard.mutex);
    auto iter = shard.entries.find(key);
    if (iter == shard.entries.end()) {
        return {};
    }
    const auto &entries = iter->second;
    auto first = std::upper_bound(entries.begin(), entries.end(), read_ts,
                                  [](timestamp_t ts, const Entry &entry) { return ts < entry.retire_ts; });
    std::vector<Rid> rids;
    rids.reserve(static_cast<size_t>(entries.end() - first));
    for (; first != entries.end(); ++first) {
        rids.push_back(first->rid);
    }
    return rids;
}

void SnapshotIndexHistoryStore::Purge(timestamp_t watermark) {
    for (auto &shard : shards_) {
        std::lock_guard<std::mutex> guard(shard.mutex);
        for (auto iter = shard.entries.begin(); iter != shard.entries.end();) {
            auto &entries = iter->second;
            auto first_live = std::upper_bound(entries.begin(), entries.end(), watermark,
                                               [](timestamp_t ts, const Entry &entry) {
                                                   return ts < entry.retire_ts;
                                               });
            entries.erase(entries.begin(), first_live);
            if (entries.empty()) {
                iter = shard.entries.erase(iter);
            } else {
                ++iter;
            }
        }
    }
}

void SnapshotIndexHistoryStore::EraseIndex(const std::string &table_name,
                                           rmdb::u64 index_id) {
    Key key{table_name, index_id};
    Shard &shard = shards_[shard_index(key)];
    std::lock_guard<std::mutex> guard(shard.mutex);
    shard.entries.erase(key);
}

void SnapshotIndexHistoryStore::EraseTable(const std::string &table_name) {
    for (auto &shard : shards_) {
        std::lock_guard<std::mutex> guard(shard.mutex);
        for (auto iter = shard.entries.begin(); iter != shard.entries.end();) {
            if (iter->first.table_name == table_name) {
                iter = shard.entries.erase(iter);
            } else {
                ++iter;
            }
        }
    }
}

void SnapshotIndexHistoryStore::Clear() {
    for (auto &shard : shards_) {
        std::lock_guard<std::mutex> guard(shard.mutex);
        shard.entries.clear();
    }
}

size_t SnapshotIndexHistoryStore::EntryCountForTest() {
    size_t count = 0;
    for (auto &shard : shards_) {
        std::lock_guard<std::mutex> guard(shard.mutex);
        for (const auto &entry : shard.entries) {
            count += entry.second.size();
        }
    }
    return count;
}

}  // namespace rmdb
