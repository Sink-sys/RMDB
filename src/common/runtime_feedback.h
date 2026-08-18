#pragma once

#include <atomic>
#include <chrono>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "common/workload_feedback.h"
#include "common/types.h"

namespace rmdb {

constexpr rmdb::u32 kInvalidRuntimeNodeId = std::numeric_limits<rmdb::u32>::max();

enum class RuntimeNodeKind : rmdb::u8 {
    kOther = 0,
    kSeqScan,
    kIndexScan,
    kJoin,
    kCountIndex,
    kMinMaxIndex,
    kAggregate,
    kSort,
    kLimit,
    kDml,
};

struct RuntimeScanFeedback {
    std::atomic<rmdb::u64> executions{0};
    std::atomic<rmdb::u64> rows_scanned{0};
    std::atomic<rmdb::u64> rows_visible{0};
    std::atomic<rmdb::u64> rows_output{0};
    std::atomic<rmdb::u64> index_entries{0};
    std::atomic<rmdb::u64> heap_fetches{0};
    std::atomic<rmdb::u64> index_only_rows{0};
    std::atomic<rmdb::u64> elapsed_ns{0};
};

struct RuntimeJoinFeedback {
    std::atomic<rmdb::u64> executions{0};
    std::atomic<rmdb::u64> left_rows{0};
    std::atomic<rmdb::u64> right_rows{0};
    std::atomic<rmdb::u64> candidate_pairs{0};
    std::atomic<rmdb::u64> rows_output{0};
    std::atomic<rmdb::u64> elapsed_ns{0};
};

struct RuntimeScanFeedbackSnapshot {
    rmdb::u64 executions = 0;
    rmdb::u64 rows_scanned = 0;
    rmdb::u64 rows_visible = 0;
    rmdb::u64 rows_output = 0;
    rmdb::u64 index_entries = 0;
    rmdb::u64 heap_fetches = 0;
    rmdb::u64 index_only_rows = 0;
    rmdb::u64 elapsed_ns = 0;
};

struct RuntimeNodeFeedback {
    RuntimeNodeKind kind = RuntimeNodeKind::kOther;
    rmdb::u32 node_id = kInvalidRuntimeNodeId;
    rmdb::u64 generation = 0;
    RuntimeScanFeedback scan;
    RuntimeJoinFeedback join;

    RuntimeNodeFeedback(RuntimeNodeKind kind_, rmdb::u32 node_id_, rmdb::u64 generation_ = 0)
        : kind(kind_), node_id(node_id_), generation(generation_) {}
};

struct RuntimeFeedbackStore {
    explicit RuntimeFeedbackStore(rmdb::u64 generation_ = workload_feedback_generation())
        : generation(generation_) {}

    rmdb::u64 generation = 0;
    std::vector<std::shared_ptr<RuntimeNodeFeedback>> nodes;

    std::shared_ptr<RuntimeNodeFeedback> get(rmdb::u32 node_id) const {
        if (node_id >= nodes.size()) {
            return nullptr;
        }
        return nodes[node_id];
    }

    std::shared_ptr<RuntimeNodeFeedback> ensure(rmdb::u32 node_id, RuntimeNodeKind kind) {
        if (nodes.size() <= node_id) {
            nodes.resize(static_cast<size_t>(node_id) + 1);
        }
        if (nodes[node_id] == nullptr) {
            nodes[node_id] = std::make_shared<RuntimeNodeFeedback>(kind, node_id, generation);
        }
        return nodes[node_id];
    }
};

inline void record_scan_feedback(const std::shared_ptr<RuntimeNodeFeedback> &feedback,
                                 rmdb::u64 rows_scanned,
                                 rmdb::u64 rows_visible,
                                 rmdb::u64 rows_output,
                                 rmdb::u64 index_entries,
                                 rmdb::u64 heap_fetches,
                                 rmdb::u64 index_only_rows,
                                 rmdb::u64 elapsed_ns) {
    if (feedback == nullptr) {
        return;
    }
    feedback->scan.executions.fetch_add(1, std::memory_order_relaxed);
    feedback->scan.rows_scanned.fetch_add(rows_scanned, std::memory_order_relaxed);
    feedback->scan.rows_visible.fetch_add(rows_visible, std::memory_order_relaxed);
    feedback->scan.rows_output.fetch_add(rows_output, std::memory_order_relaxed);
    feedback->scan.index_entries.fetch_add(index_entries, std::memory_order_relaxed);
    feedback->scan.heap_fetches.fetch_add(heap_fetches, std::memory_order_relaxed);
    feedback->scan.index_only_rows.fetch_add(index_only_rows, std::memory_order_relaxed);
    feedback->scan.elapsed_ns.fetch_add(elapsed_ns, std::memory_order_relaxed);
}

inline void record_join_feedback(const std::shared_ptr<RuntimeNodeFeedback> &feedback,
                                 rmdb::u64 left_rows,
                                 rmdb::u64 right_rows,
                                 rmdb::u64 candidate_pairs,
                                 rmdb::u64 rows_output,
                                 rmdb::u64 elapsed_ns) {
    if (feedback == nullptr || feedback->kind != RuntimeNodeKind::kJoin) {
        return;
    }
    feedback->join.executions.fetch_add(1, std::memory_order_relaxed);
    feedback->join.left_rows.fetch_add(left_rows, std::memory_order_relaxed);
    feedback->join.right_rows.fetch_add(right_rows, std::memory_order_relaxed);
    feedback->join.candidate_pairs.fetch_add(candidate_pairs, std::memory_order_relaxed);
    feedback->join.rows_output.fetch_add(rows_output, std::memory_order_relaxed);
    feedback->join.elapsed_ns.fetch_add(elapsed_ns, std::memory_order_relaxed);
}

class RuntimeJoinFeedbackCollector {
   public:
    RuntimeJoinFeedbackCollector() = default;
    explicit RuntimeJoinFeedbackCollector(std::shared_ptr<RuntimeNodeFeedback> feedback) {
        bind(std::move(feedback));
    }
    ~RuntimeJoinFeedbackCollector() { flush(); }

    RuntimeJoinFeedbackCollector(const RuntimeJoinFeedbackCollector &) = delete;
    RuntimeJoinFeedbackCollector &operator=(const RuntimeJoinFeedbackCollector &) = delete;

    void bind(std::shared_ptr<RuntimeNodeFeedback> feedback) {
        flush();
        feedback_ = std::move(feedback);
    }

    void begin() {
        flush();
        counters_ = {};
        active_ = feedback_ != nullptr && feedback_->kind == RuntimeNodeKind::kJoin;
        flushed_ = false;
        if (active_) {
            started_at_ = std::chrono::steady_clock::now();
        }
    }

    void add_left_rows(rmdb::u64 count = 1) {
        if (active_) counters_.left_rows += count;
    }
    void add_right_rows(rmdb::u64 count = 1) {
        if (active_) counters_.right_rows += count;
    }
    void add_candidate_pairs(rmdb::u64 count = 1) {
        if (active_) counters_.candidate_pairs += count;
    }
    void add_rows_output(rmdb::u64 count = 1) {
        if (active_) counters_.rows_output += count;
    }

    void flush() {
        if (!active_ || flushed_) {
            return;
        }
        flushed_ = true;
        auto elapsed_ns = static_cast<rmdb::u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - started_at_)
                .count());
        record_join_feedback(feedback_, counters_.left_rows, counters_.right_rows,
                             counters_.candidate_pairs, counters_.rows_output, elapsed_ns);
    }

   private:
    struct Counters {
        rmdb::u64 left_rows{0};
        rmdb::u64 right_rows{0};
        rmdb::u64 candidate_pairs{0};
        rmdb::u64 rows_output{0};
    };

    std::shared_ptr<RuntimeNodeFeedback> feedback_;
    Counters counters_;
    std::chrono::steady_clock::time_point started_at_;
    bool active_{false};
    bool flushed_{true};
};

class RuntimeScanFeedbackCollector {
   public:
    RuntimeScanFeedbackCollector() = default;

    RuntimeScanFeedbackCollector(std::shared_ptr<RuntimeNodeFeedback> feedback,
                                 RuntimeNodeKind expected_kind) {
        bind(std::move(feedback), expected_kind);
    }

    ~RuntimeScanFeedbackCollector() { flush(); }

    RuntimeScanFeedbackCollector(const RuntimeScanFeedbackCollector &) = delete;
    RuntimeScanFeedbackCollector &operator=(const RuntimeScanFeedbackCollector &) = delete;

    void bind(std::shared_ptr<RuntimeNodeFeedback> feedback, RuntimeNodeKind expected_kind) {
        flush();
        feedback_ = std::move(feedback);
        expected_kind_ = expected_kind;
    }

    void begin() {
        flush();
        counters_ = {};
        active_ = feedback_ != nullptr && feedback_->kind == expected_kind_;
        flushed_ = false;
        if (active_) {
            started_at_ = std::chrono::steady_clock::now();
        }
    }

    void add_rows_scanned(rmdb::u64 count = 1) {
        if (active_) counters_.rows_scanned += count;
    }
    void add_rows_visible(rmdb::u64 count = 1) {
        if (active_) counters_.rows_visible += count;
    }
    void add_rows_output(rmdb::u64 count = 1) {
        if (active_) counters_.rows_output += count;
    }
    void add_index_entries(rmdb::u64 count = 1) {
        if (active_) counters_.index_entries += count;
    }
    void add_heap_fetches(rmdb::u64 count = 1) {
        if (active_) counters_.heap_fetches += count;
    }
    void add_index_only_rows(rmdb::u64 count = 1) {
        if (active_) counters_.index_only_rows += count;
    }

    void flush() {
        if (!active_ || flushed_) {
            return;
        }
        flushed_ = true;
        auto elapsed_ns = static_cast<rmdb::u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - started_at_)
                .count());
        record_scan_feedback(feedback_, counters_.rows_scanned, counters_.rows_visible,
                             counters_.rows_output, counters_.index_entries, counters_.heap_fetches,
                             counters_.index_only_rows, elapsed_ns);
    }

   private:
    struct Counters {
        rmdb::u64 rows_scanned{0};
        rmdb::u64 rows_visible{0};
        rmdb::u64 rows_output{0};
        rmdb::u64 index_entries{0};
        rmdb::u64 heap_fetches{0};
        rmdb::u64 index_only_rows{0};
    };

    std::shared_ptr<RuntimeNodeFeedback> feedback_;
    RuntimeNodeKind expected_kind_{RuntimeNodeKind::kOther};
    Counters counters_;
    std::chrono::steady_clock::time_point started_at_;
    bool active_{false};
    bool flushed_{true};
};

inline RuntimeScanFeedbackSnapshot load_scan_feedback_snapshot(
    const std::shared_ptr<RuntimeNodeFeedback> &feedback) {
    RuntimeScanFeedbackSnapshot snapshot;
    if (feedback == nullptr) {
        return snapshot;
    }
    snapshot.executions = feedback->scan.executions.load(std::memory_order_relaxed);
    snapshot.rows_scanned = feedback->scan.rows_scanned.load(std::memory_order_relaxed);
    snapshot.rows_visible = feedback->scan.rows_visible.load(std::memory_order_relaxed);
    snapshot.rows_output = feedback->scan.rows_output.load(std::memory_order_relaxed);
    snapshot.index_entries = feedback->scan.index_entries.load(std::memory_order_relaxed);
    snapshot.heap_fetches = feedback->scan.heap_fetches.load(std::memory_order_relaxed);
    snapshot.index_only_rows = feedback->scan.index_only_rows.load(std::memory_order_relaxed);
    snapshot.elapsed_ns = feedback->scan.elapsed_ns.load(std::memory_order_relaxed);
    return snapshot;
}

}  // namespace rmdb
