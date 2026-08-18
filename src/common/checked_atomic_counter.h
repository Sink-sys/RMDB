#pragma once

#include <atomic>
#include <string>

#include "common/exception.h"

namespace rmdb::atomic_counter {

template <typename T>
inline void Increment(std::atomic<T> &counter, T amount = 1,
                      std::memory_order order = std::memory_order_relaxed) {
    counter.fetch_add(amount, order);
}

template <typename T>
inline void DecrementPositive(std::atomic<T> &counter, const char *name, T amount = 1,
                              std::memory_order success_order = std::memory_order_relaxed,
                              std::memory_order failure_order = std::memory_order_relaxed) {
    if (amount == 0) {
        return;
    }
    T current = counter.load(std::memory_order_relaxed);
    while (true) {
        if (current < amount) {
            throw InternalError(std::string(name) + " underflow");
        }
        if (counter.compare_exchange_weak(current, current - amount, success_order, failure_order)) {
            return;
        }
    }
}

}  // namespace rmdb::atomic_counter
