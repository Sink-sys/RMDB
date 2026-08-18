#pragma once

#include <atomic>

#include "common/types.h"

namespace rmdb {

struct WorkloadFeedbackEpochSnapshot {
    rmdb::u64 data_epoch{0};
    rmdb::u64 feedback_generation{0};
};

class WorkloadFeedbackLifecycle {
   public:
    static WorkloadFeedbackLifecycle &Instance() {
        static WorkloadFeedbackLifecycle lifecycle;
        return lifecycle;
    }

    WorkloadFeedbackEpochSnapshot Snapshot() const {
        return WorkloadFeedbackEpochSnapshot{
            data_epoch_.load(std::memory_order_acquire),
            feedback_generation_.load(std::memory_order_acquire),
        };
    }

    void AdvanceDataGeneration() {
        data_epoch_.fetch_add(1, std::memory_order_acq_rel);
        feedback_generation_.fetch_add(1, std::memory_order_acq_rel);
    }

   private:
    WorkloadFeedbackLifecycle() = default;

    std::atomic<rmdb::u64> data_epoch_{1};
    std::atomic<rmdb::u64> feedback_generation_{1};
};

inline rmdb::u64 workload_data_epoch() {
    return WorkloadFeedbackLifecycle::Instance().Snapshot().data_epoch;
}

inline rmdb::u64 workload_feedback_generation() {
    return WorkloadFeedbackLifecycle::Instance().Snapshot().feedback_generation;
}

inline void advance_workload_data_generation() {
    WorkloadFeedbackLifecycle::Instance().AdvanceDataGeneration();
}

}  // namespace rmdb
