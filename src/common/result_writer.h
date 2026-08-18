#pragma once

#include <vector>

#include "common/types.h"
#include "system/sm_meta.h"

class QueryResultWriter {
   public:
    virtual ~QueryResultWriter() = default;

    virtual void BeginResult(const std::vector<ColMeta> &columns) = 0;
    virtual void WriteRow(const std::vector<ColMeta> &columns,
                          const std::vector<const char *> &cells) = 0;
    virtual void EndResult(rmdb::u64 row_count) = 0;
};
