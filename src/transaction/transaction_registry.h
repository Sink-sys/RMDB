#pragma once

#include "common/exception.h"
#include "transaction.h"

#include <array>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <utility>

class TransactionRegistry {
   public:
    using Map = std::unordered_map<txn_id_t, Transaction *>;
    static constexpr size_t kShardCount = 32;

    class AllView {
       public:
        // 分片共享锁读取:与 WithTransactionShared/Insert(分片排他)互斥,
        // 使 GC 不再需要全局排他锁保护 Find。
        Transaction *Find(txn_id_t txn_id) const {
            auto &shard = registry_->shards_[registry_->ShardIndex(txn_id)];
            std::shared_lock<std::shared_mutex> lock(shard.mutex);
            auto iter = shard.transactions.find(txn_id);
            return iter == shard.transactions.end() ? nullptr : iter->second;
        }

        bool Erase(txn_id_t txn_id) {
            auto &shard = registry_->shards_[registry_->ShardIndex(txn_id)];
            // 取分片排他锁:WithTransactionShared 只持分片共享锁,分片锁是
            // map 变更是唯一屏障;全局锁只负责跨分片一致性与 Insert 串行。
            std::unique_lock<std::shared_mutex> lock(shard.mutex);
            return shard.transactions.erase(txn_id) != 0;
        }

        size_t Size() const {
            size_t size = 0;
            for (const auto &shard : registry_->shards_) {
                std::shared_lock<std::shared_mutex> lock(shard.mutex);
                size += shard.transactions.size();
            }
            return size;
        }

        template <typename Fn>
        void ForEach(Fn &&fn) const {
            // 逐分片共享锁遍历:与 Insert/Erase(分片排他)互斥,不同分片
            // 并行。回调内不得调用 Erase(同分片 shared->exclusive 升级死锁)。
            for (const auto &shard : registry_->shards_) {
                std::shared_lock<std::shared_mutex> lock(shard.mutex);
                for (const auto &entry : shard.transactions) {
                    fn(entry.first, entry.second);
                }
            }
        }

       private:
        friend class TransactionRegistry;
        explicit AllView(TransactionRegistry *registry) : registry_(registry) {}
        TransactionRegistry *registry_;
    };

    TransactionRegistry() = default;
    TransactionRegistry(const TransactionRegistry &) = delete;
    TransactionRegistry &operator=(const TransactionRegistry &) = delete;

    ~TransactionRegistry() {
        std::unique_lock<std::shared_mutex> all_lock(all_latch_);
        for (auto &shard : shards_) {
            for (auto &entry : shard.transactions) {
                delete entry.second;
            }
        }
    }

    void Insert(Transaction *txn) {
        if (txn == nullptr) {
            throw InternalError("Cannot register a null transaction");
        }
        // 分片排他锁已足够:所有读者(Find/ForEach/GetThreadOwned/
        // WithTransactionShared)均持分片共享锁。不再取全局 all_latch_
        // shared,使 GC 批处理(逐分片锁)期间 begin 不被全局锁排队。
        auto &shard = shards_[ShardIndex(txn->get_transaction_id())];
        std::unique_lock<std::shared_mutex> lock(shard.mutex);
        auto [iter, inserted] = shard.transactions.emplace(txn->get_transaction_id(), txn);
        if (!inserted && iter->second != txn) {
            throw InternalError("Transaction id is already registered");
        }
        iter->second = txn;
    }

    Transaction *GetThreadOwned(txn_id_t txn_id) const {
        if (txn_id == INVALID_TXN_ID) {
            return nullptr;
        }
        // 只取分片共享锁:map 读安全由分片锁保证(插入/删除均持分片排他锁),
        // 不再触碰全局 all_latch_ 缓存行。
        const auto &shard = shards_[ShardIndex(txn_id)];
        std::shared_lock<std::shared_mutex> lock(shard.mutex);
        auto iter = shard.transactions.find(txn_id);
        if (iter == shard.transactions.end() || iter->second == nullptr) {
            throw InternalError("Transaction is not registered");
        }
        Transaction *txn = iter->second;
        if (txn->get_thread_id() != std::this_thread::get_id()) {
            throw InternalError("Transaction belongs to a different thread");
        }
        return txn;
    }

    size_t Size() const {
        std::shared_lock<std::shared_mutex> all_lock(all_latch_);
        std::array<std::shared_lock<std::shared_mutex>, kShardCount> shard_locks;
        size_t size = 0;
        for (size_t i = 0; i < kShardCount; ++i) {
            shard_locks[i] = std::shared_lock<std::shared_mutex>(shards_[i].mutex);
            size += shards_[i].transactions.size();
        }
        return size;
    }

    template <typename Fn>
    decltype(auto) WithAllShared(Fn &&fn) const {
        std::shared_lock<std::shared_mutex> all_lock(all_latch_);
        std::array<std::shared_lock<std::shared_mutex>, kShardCount> shard_locks;
        for (size_t i = 0; i < kShardCount; ++i) {
            shard_locks[i] = std::shared_lock<std::shared_mutex>(shards_[i].mutex);
        }
        return std::forward<Fn>(fn)(AllView(const_cast<TransactionRegistry *>(this)));
    }

    template <typename Fn>
    decltype(auto) WithAllExclusive(Fn &&fn) {
        std::unique_lock<std::shared_mutex> all_lock(all_latch_);
        return std::forward<Fn>(fn)(AllView(this));
    }

    // 无全局锁视图:AllView 的 Find/ForEach/Erase 均由分片锁保护,不提供
    // 跨分片一致性,也不与 SSI 的 rw_dependencies_ 串行化。仅供 GC 在
    // 无活跃 SERIALIZABLE 事务时使用(SI 负载),避免全局排他锁阻塞 begin。
    AllView ShardsView() { return AllView(this); }

    template <typename Fn>
    decltype(auto) WithTransactionShared(txn_id_t txn_id, Fn &&fn) const {
        // 只取分片共享锁:对象生命周期由分片锁保证(GC 在分片排他锁下 erase,
        // 释放对象,读取方在共享锁作用域内使用指针),全局锁从热路径移除。
        const auto &shard = shards_[ShardIndex(txn_id)];
        std::shared_lock<std::shared_mutex> lock(shard.mutex);
        auto iter = shard.transactions.find(txn_id);
        return std::forward<Fn>(fn)(iter == shard.transactions.end() ? nullptr : iter->second);
    }

   private:
    struct alignas(64) Shard {
        mutable std::shared_mutex mutex;
        Map transactions;
    };

    size_t ShardIndex(txn_id_t txn_id) const {
        return std::hash<txn_id_t>{}(txn_id) % kShardCount;
    }

    Transaction *FindUnlocked(txn_id_t txn_id) const {
        const auto &map = shards_[ShardIndex(txn_id)].transactions;
        auto iter = map.find(txn_id);
        return iter == map.end() ? nullptr : iter->second;
    }

    mutable std::shared_mutex all_latch_;
    std::array<Shard, kShardCount> shards_;
};
