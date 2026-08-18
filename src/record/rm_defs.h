/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <utility>

#include "common/binary.h"
#include "common/exception.h"
#include "defs.h"
#include "storage/buffer_pool_manager.h"

constexpr int RM_NO_PAGE = -1;
constexpr int RM_FILE_HDR_PAGE = 0;
constexpr int RM_FIRST_RECORD_PAGE = 1;
constexpr int RM_MAX_RECORD_SIZE = 512;

struct TupleMeta {
    timestamp_t ts_;
    bool is_deleted_;

    friend auto operator==(const TupleMeta &a, const TupleMeta &b) {
        return a.ts_ == b.ts_ && a.is_deleted_ == b.is_deleted_;
    }

    friend auto operator!=(const TupleMeta &a, const TupleMeta &b) { return !(a == b); }
};

/* 文件头，记录表数据文件的元信息，写入磁盘中文件的第0号页面 */
struct RmFileHdr {
    int record_size;            // 表中每条记录的大小，由于不包含变长字段，因此当前字段初始化后保持不变
    int num_pages;              // 文件中分配的页面个数（初始化为1）
    int num_records_per_page;   // 每个页面最多能存储的元组个数
    int first_free_page_no;     // 文件中当前第一个包含空闲空间的页面号（初始化为-1）
    int bitmap_size;            // 每个页面bitmap大小
};

/* 表数据文件中每个页面的页头，记录每个页面的元信息 */
struct RmPageHdr {
    int next_free_page_no;  // 当前页面满了之后，下一个包含空闲空间的页面号（初始化为-1）
    int num_records;        // 当前页面中当前已经存储的记录个数（初始化为0）
};

/* 表中的记录 */
struct RmRecord {
    char* data = nullptr;  // 记录的数据
    int size = 0;          // 记录的大小

    RmRecord() = default;

    explicit RmRecord(int size_) { Resize(size_); }

    RmRecord(int size_, const char* data_) { ResizeAndCopy(data_, size_); }

    RmRecord(const RmRecord& other) { ResizeAndCopy(other.data, other.size); }

    RmRecord &operator=(const RmRecord& other) {
        if (this == &other) {
            return *this;
        }
        RmRecord copy(other);
        swap(copy);
        return *this;
    }

    RmRecord(RmRecord&& other) noexcept
        : data(other.data), size(other.size), capacity_(other.capacity_), storage_(std::move(other.storage_)) {
        if (storage_ != nullptr) {
            data = storage_.get();
        }
        other.reset_view();
    }

    RmRecord &operator=(RmRecord&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        storage_ = std::move(other.storage_);
        data = other.data;
        size = other.size;
        capacity_ = other.capacity_;
        if (storage_ != nullptr) {
            data = storage_.get();
        }
        other.reset_view();
        return *this;
    }

    // 仅借用外部缓冲区，不接管生命周期；调用者必须保证缓冲区在本对象最后一次读取前仍然有效。
    static RmRecord borrow(const char* borrowed_data, int borrowed_size) {
        validate_size(borrowed_size);
        if (borrowed_size > 0 && borrowed_data == nullptr) {
            throw InternalError("Cannot borrow a null record buffer");
        }
        RmRecord record;
        record.data = const_cast<char*>(borrowed_data);
        record.size = borrowed_size;
        return record;
    }

    ~RmRecord() = default;

    void SetData(const char* data_) {
        if (size > 0 && data_ == nullptr) {
            throw InternalError("Cannot copy from a null record buffer");
        }
        if (size > 0) {
            if (!owns_data()) {
                ensure_owned_capacity(size);
            }
            std::memmove(data, data_, static_cast<std::size_t>(size));
        }
    }

    void Resize(int size_) {
        ensure_owned_capacity(size_);
        size = size_;
    }

    void ResizeAndCopy(const char* data_, int size_) {
        validate_size(size_);
        if (size_ > 0 && data_ == nullptr) {
            throw InternalError("Cannot copy from a null record buffer");
        }
        if (size_ == 0) {
            storage_.reset();
            capacity_ = 0;
        } else if (storage_ != nullptr && capacity_ >= size_) {
            // data_ 可能来自当前 storage_（例如原地反序列化），因此必须支持区间重叠。
            std::memmove(storage_.get(), data_, static_cast<std::size_t>(size_));
        } else {
            auto new_storage = std::make_unique<char[]>(static_cast<std::size_t>(size_));
            std::memcpy(new_storage.get(), data_, static_cast<std::size_t>(size_));
            storage_ = std::move(new_storage);
            capacity_ = size_;
        }
        data = storage_.get();
        size = size_;
    }

    void Deserialize(std::span<const char> bytes) {
        if (bytes.size() < sizeof(int)) {
            throw InternalError("Serialized record is missing its size header");
        }
        const int serialized_size = rmdb::load_unaligned<int>(bytes.data());
        validate_size(serialized_size);
        if (static_cast<std::size_t>(serialized_size) > bytes.size() - sizeof(int)) {
            throw InternalError("Serialized record payload is truncated");
        }
        ResizeAndCopy(bytes.data() + sizeof(int), serialized_size);
    }

    [[nodiscard]] bool owns_data() const noexcept { return storage_ != nullptr; }

    [[nodiscard]] int capacity() const noexcept { return capacity_; }

    void swap(RmRecord& other) noexcept {
        using std::swap;
        swap(data, other.data);
        swap(size, other.size);
        swap(capacity_, other.capacity_);
        swap(storage_, other.storage_);
    }

   private:
    int capacity_ = 0;
    std::unique_ptr<char[]> storage_;

    static void validate_size(int record_size) {
        if (record_size < 0 || record_size > RM_MAX_RECORD_SIZE) {
            throw InternalError("Invalid record size: " + std::to_string(record_size));
        }
    }

    void ensure_owned_capacity(int required_capacity) {
        validate_size(required_capacity);
        if (storage_ != nullptr && capacity_ >= required_capacity) {
            data = storage_.get();
            return;
        }
        if (required_capacity == 0) {
            storage_.reset();
            data = nullptr;
            capacity_ = 0;
            return;
        }
        auto new_storage = std::make_unique<char[]>(static_cast<std::size_t>(required_capacity));
        storage_ = std::move(new_storage);
        data = storage_.get();
        capacity_ = required_capacity;
    }

    void reset_view() noexcept {
        data = nullptr;
        size = 0;
        capacity_ = 0;
    }
};
