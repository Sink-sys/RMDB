#pragma once

#include "common/catalog_epoch.h"
#include "common/plan_template_cache.h"
#include "common/query_template_cache.h"

namespace rmdb {

class CatalogChangeCoordinator {
   public:
    static CatalogChangeCoordinator &Instance() {
        static CatalogChangeCoordinator coordinator;
        return coordinator;
    }

    void PublishSchemaChange() {
        bump_sql_template_schema_epoch();
        advance_catalog_data_epoch();
        clear_query_template_cache();
        clear_plan_template_cache();
    }

    void PublishBulkDataChange() {
        advance_catalog_data_epoch();
        clear_plan_template_cache();
    }

   private:
    CatalogChangeCoordinator() = default;
};

inline void publish_schema_change() {
    CatalogChangeCoordinator::Instance().PublishSchemaChange();
}

inline void publish_bulk_data_change() {
    CatalogChangeCoordinator::Instance().PublishBulkDataChange();
}

}  // namespace rmdb
