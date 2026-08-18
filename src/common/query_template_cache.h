#pragma once

#include <memory>

#include "common/lru_cache_store.h"
#include "common/query_template.h"
#include "common/types.h"

namespace rmdb::query_template_detail {

struct CacheEntry {
    rmdb::u64 schema_epoch = 0;
    std::shared_ptr<const QueryTemplate> templ;
};

class QueryTemplateCache {
   public:
    explicit QueryTemplateCache(size_t capacity = 256) : store_(capacity) {}

    std::shared_ptr<Query> lookup(const SqlTemplateCandidate &candidate, rmdb::u64 epoch,
                                  Context *context = nullptr) {
        auto cached = store_.Lookup(candidate.key, [&](const CacheEntry &entry) {
            return entry.schema_epoch == epoch && entry.templ != nullptr &&
                   entry.templ->slots.size() == candidate.literals.size();
        });
        if (!cached.has_value()) {
            return nullptr;
        }
        return materialize_template(*cached->templ, candidate, context);
    }

    void store(const SqlTemplateCandidate &candidate, const std::shared_ptr<ast::TreeNode> &root,
               const std::shared_ptr<Query> &query, rmdb::u64 epoch) {
        auto templ = make_template(candidate, root, query);
        if (templ != nullptr) {
            store_.Store(candidate.key, CacheEntry{epoch, std::move(templ)});
        }
    }

    void clear() { store_.Clear(); }

   private:
    LruCacheStore<SqlTemplateKey, CacheEntry, SqlTemplateKeyHash> store_;
};

inline QueryTemplateCache &cache_instance() {
    static QueryTemplateCache cache;
    return cache;
}

}  // namespace rmdb::query_template_detail

namespace rmdb {

inline std::shared_ptr<Query> lookup_query_template(const SqlTemplateCandidate &candidate,
                                                    Context *context = nullptr) {
    return query_template_detail::cache_instance().lookup(candidate, sql_template_schema_epoch(), context);
}

inline void store_query_template(const SqlTemplateCandidate &candidate,
                                 const std::shared_ptr<ast::TreeNode> &root,
                                 const std::shared_ptr<Query> &query) {
    query_template_detail::cache_instance().store(candidate, root, query, sql_template_schema_epoch());
}

inline void clear_query_template_cache() {
    query_template_detail::cache_instance().clear();
}

}  // namespace rmdb
