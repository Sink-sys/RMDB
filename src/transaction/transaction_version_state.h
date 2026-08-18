#pragma once

#include "transaction_version_storage.h"
#include "common/types.h"

#include <atomic>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>

struct TransactionVersionState {
    using TableMap = std::unordered_map<std::string, std::shared_ptr<TransactionTableVersionInfo>>;
    using TableIdMap = std::unordered_map<rmdb::u32, std::shared_ptr<TransactionTableVersionInfo>>;

    mutable std::shared_mutex mutex;
    TableMap tables;
    // 稳定表标识 → 版本信息:退役元组(仅存 u32)的查找路径,随 tables 同步维护。
    TableIdMap tables_by_id;
    std::atomic<rmdb::u32> next_table_id{1};
    std::atomic<rmdb::u64> epoch{1};
    inline static std::atomic<rmdb::u64> next_cache_id{1};
    const rmdb::u64 cache_id{next_cache_id.fetch_add(1, std::memory_order_relaxed)};
};
