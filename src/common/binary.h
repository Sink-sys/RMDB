#pragma once

#include <cstring>
#include <type_traits>

namespace rmdb {

template <typename T>
inline T load_unaligned(const void *data) noexcept {
    static_assert(std::is_trivially_copyable_v<T>);
    T value;
    std::memcpy(&value, data, sizeof(T));
    return value;
}

template <typename T>
inline void store_unaligned(void *data, T value) noexcept {
    static_assert(std::is_trivially_copyable_v<T>);
    std::memcpy(data, &value, sizeof(T));
}

}  // namespace rmdb
