#pragma once

#include <memory>
#include <utility>

#include "common/lru_cache_store.h"
#include "common/plan_template_materialization.h"
#include "common/types.h"
#include "common/workload_feedback.h"

namespace rmdb {
namespace plan_template_detail {

struct PlanTemplateKey {
    SqlTemplateKey sql;
    IsolationLevel isolation_level = IsolationLevel::SNAPSHOT_ISOLATION;

    bool operator==(const PlanTemplateKey &other) const {
        return sql == other.sql && isolation_level == other.isolation_level;
    }
};

struct PlanTemplateKeyHash {
    size_t operator()(const PlanTemplateKey &key) const {
        size_t hash = SqlTemplateKeyHash{}(key.sql);
        hash ^= static_cast<size_t>(key.isolation_level) + 0x9e3779b9U + (hash << 6) + (hash >> 2);
        return hash;
    }
};

struct CacheEntry {
    rmdb::u64 schema_epoch = 0;
    std::shared_ptr<const PlanTemplate> templ;
};

class PlanTemplateCache {
   public:
    explicit PlanTemplateCache(size_t capacity = 256) : store_(capacity) {}

    std::shared_ptr<const PlanTemplate> lookup_template(const SqlTemplateCandidate &candidate,
                                                        IsolationLevel isolation_level,
                                                        rmdb::u64 schema_epoch,
                                                        rmdb::u64 feedback_generation) {
        PlanTemplateKey key{candidate.key, isolation_level};
        auto cached = store_.Lookup(key, [&](const CacheEntry &entry) {
            return entry.schema_epoch == schema_epoch && entry.templ != nullptr &&
                   entry.templ->feedback != nullptr && entry.templ->feedback->generation == feedback_generation;
        });
        return cached.has_value() ? cached->templ : nullptr;
    }

    std::shared_ptr<Plan> lookup(const SqlTemplateCandidate &candidate, IsolationLevel isolation_level,
                                 rmdb::u64 schema_epoch, rmdb::u64 feedback_generation,
                                 Context *context = nullptr) {
        auto templ = lookup_template(candidate, isolation_level, schema_epoch, feedback_generation);
        if (templ == nullptr) {
            return nullptr;
        }
        auto plan = materialize_template(*templ, candidate, context);
        if (sql_template_schema_epoch() != schema_epoch ||
            workload_feedback_generation() != feedback_generation) {
            return nullptr;
        }
        return plan;
    }

    void store(const SqlTemplateCandidate &candidate, const std::shared_ptr<Query> &query,
               const std::shared_ptr<Plan> &plan, IsolationLevel isolation_level, rmdb::u64 schema_epoch,
               rmdb::u64 feedback_generation) {
        auto templ = make_template(candidate, query, plan, feedback_generation);
        if (templ == nullptr) {
            return;
        }
        if (sql_template_schema_epoch() != schema_epoch ||
            workload_feedback_generation() != feedback_generation) {
            return;
        }
        PlanTemplateKey key{candidate.key, isolation_level};
        store_.Store(std::move(key), CacheEntry{schema_epoch, std::move(templ)});
    }

    void clear() { store_.Clear(); }

   private:
    LruCacheStore<PlanTemplateKey, CacheEntry, PlanTemplateKeyHash> store_;
};

inline PlanTemplateCache &cache_instance() {
    static PlanTemplateCache cache;
    return cache;
}

}  // namespace plan_template_detail

inline std::shared_ptr<Plan> lookup_plan_template(const SqlTemplateCandidate &candidate,
                                                  IsolationLevel isolation_level, Context *context = nullptr) {
    rmdb::u64 schema_epoch = sql_template_schema_epoch();
    rmdb::u64 feedback_generation = workload_feedback_generation();
    return plan_template_detail::cache_instance().lookup(candidate, isolation_level, schema_epoch,
                                                         feedback_generation, context);
}

inline std::shared_ptr<const plan_template_detail::PlanTemplate> lookup_plan_template_definition(
    const SqlTemplateCandidate &candidate, IsolationLevel isolation_level) {
    return plan_template_detail::cache_instance().lookup_template(
        candidate, isolation_level, sql_template_schema_epoch(), workload_feedback_generation());
}

inline void store_plan_template(const SqlTemplateCandidate &candidate, const std::shared_ptr<Query> &query,
                                const std::shared_ptr<Plan> &plan, IsolationLevel isolation_level) {
    rmdb::u64 schema_epoch = sql_template_schema_epoch();
    rmdb::u64 feedback_generation = workload_feedback_generation();
    plan_template_detail::cache_instance().store(candidate, query, plan, isolation_level, schema_epoch,
                                                 feedback_generation);
}

inline void clear_plan_template_cache() {
    plan_template_detail::cache_instance().clear();
}

inline bool plan_template_cacheable_session(IsolationLevel isolation_level) {
    return isolation_level == IsolationLevel::SNAPSHOT_ISOLATION;
}

}  // namespace rmdb
