#pragma once

#include <array>
#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/config.h"
#include "common/types.h"
#include "defs.h"

namespace rmdb {

class SnapshotIndexHistoryStore {
   public:
    void Record(const std::string &table_name, rmdb::u64 index_id,
                const Rid &rid, timestamp_t retire_ts);
    std::vector<Rid> Lookup(const std::string &table_name, rmdb::u64 index_id,
                            timestamp_t read_ts);
    void Purge(timestamp_t watermark);
    void EraseIndex(const std::string &table_name, rmdb::u64 index_id);
    void EraseTable(const std::string &table_name);
    void Clear();
    size_t EntryCountForTest();

   private:
    struct Key {
        std::string table_name;
        rmdb::u64 index_id{0};

        bool operator==(const Key &other) const {
            return table_name == other.table_name && index_id == other.index_id;
        }
    };

    struct KeyHash {
        size_t operator()(const Key &key) const;
    };

    struct Entry {
        timestamp_t retire_ts{INVALID_TS};
        Rid rid{};
    };

    struct Shard {
        std::mutex mutex;
        std::unordered_map<Key, std::vector<Entry>, KeyHash> entries;
    };

    static constexpr size_t kShardCount = 32;

    size_t shard_index(const Key &key) const;
    std::array<Shard, kShardCount> shards_;
};

}  // namespace rmdb
