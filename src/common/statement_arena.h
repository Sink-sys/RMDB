#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rmdb {

/**
 * 线程本地 bump arena:面向"线程私有且生命周期 <= 当前语句"的临时对象,
 * 语句结束统一回收,减少高频小对象分配对 malloc 全局锁的竞争。
 *
 * 约束(必须严格遵守,否则 UAF):
 *   1. 只分配"调用线程拥有且语句结束后不再访问"的对象;
 *   2. 跨语句/跨线程对象(undo、版本链、写集、plan/executor 缓存、runtime
 *      cache)必须保持系统分配;
 *   3. 语句边界由 StatementArena::Get().Reset() 标记(rmdb.cpp 语句循环)。
 */
class StatementArena {
   public:
    static StatementArena &Get() {
        thread_local StatementArena arena;
        return arena;
    }

    /** bump 分配:块内对齐分配;块满开新块(低频,走系统分配)。 */
    void *Alloc(size_t size, size_t alignment) {
        if (size == 0) {
            size = 1;
        }
        if (blocks_.empty() || !AllocFromBlock(size, alignment)) {
            GrowBlock();
            if (!AllocFromBlock(size, alignment)) {
                // 单对象超过块容量:直接系统分配(罕见),由调用方正常释放。
                // 标记大对象需要单独释放——ArenaAllocator 的 deallocate 为
                // no-op,因此大对象路径必须避免;块容量取 1 MiB 足够覆盖
                // 语句级临时对象,此处直接返回 nullptr 由调用方回退。
                return nullptr;
            }
        }
        return blocks_[current_].data.data() + blocks_[current_].used - size;
    }

    /** 语句结束:回收全部块(块内存保留复用,避免系统往返),RSS 稳定在峰值。 */
    void Reset() {
        for (auto &block : blocks_) {
            block.used = 0;
        }
        current_ = 0;
    }

   private:
    static constexpr size_t kBlockCapacity = 1 << 20;  // 1 MiB/块

    struct Block {
        std::vector<char> data;
        size_t used = 0;
    };

    bool AllocFromBlock(size_t size, size_t alignment) {
        Block &block = blocks_[current_];
        size_t aligned = (block.used + alignment - 1) & ~(alignment - 1);
        if (aligned + size > block.data.size()) {
            return false;
        }
        block.used = aligned + size;
        return true;
    }

    void GrowBlock() {
        Block block;
        block.data.resize(kBlockCapacity);
        block.used = 0;
        blocks_.push_back(std::move(block));
        current_ = blocks_.size() - 1;
    }

    std::vector<Block> blocks_;
    size_t current_ = 0;
};

/** Arena 分配器(allocator-aware 容器用):deallocate 为 no-op,统一由语句
 *  Reset 回收。只用于语句级私有容器。 */
template <typename T>
class ArenaAllocator {
   public:
    using value_type = T;

    ArenaAllocator() = default;
    template <typename U>
    ArenaAllocator(const ArenaAllocator<U> &) noexcept {}

    T *allocate(size_t n) {
        void *ptr = StatementArena::Get().Alloc(n * sizeof(T), alignof(T));
        if (ptr == nullptr) {
            throw std::bad_alloc();
        }
        return static_cast<T *>(ptr);
    }

    void deallocate(T *, size_t) noexcept {}

    template <typename U>
    bool operator==(const ArenaAllocator<U> &) const noexcept {
        return true;
    }
    template <typename U>
    bool operator!=(const ArenaAllocator<U> &) const noexcept {
        return false;
    }
};

template <typename T>
using ArenaVector = std::vector<T, ArenaAllocator<T>>;

}  // namespace rmdb
