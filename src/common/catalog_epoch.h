#pragma once

#include "common/types.h"
#include "common/workload_feedback.h"

namespace rmdb {

inline rmdb::u64 catalog_data_epoch() {
    return workload_data_epoch();
}

inline rmdb::u64 catalog_feedback_generation() {
    return workload_feedback_generation();
}

inline void advance_catalog_data_epoch() {
    advance_workload_data_generation();
}

}  // namespace rmdb
