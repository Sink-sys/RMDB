/* Copyright (c) 2026 RMDB contributors.
 * 确定性索引范围扫描并发测试的 barrier 钩子，默认全部关闭。
 *
 * 测试代码使用这些钩子暂停扫描，再执行并发写入，最后比较扫描结果：
 *   - pause_after_seek：定位起始叶页后暂停，验证定位和扫描建立的原子性。
 *   - pause_before_next_leaf：释放上一叶页后、获取下一叶页前暂停，验证叶页
 *     插入、删除或分裂时仍按逻辑上界扫描。
 *
 * 测试线程将对应 flag 设为 true，等待 *_hits 增加后启动并发写者；清除 flag
 * 让扫描继续，最后核对扫描结果与预期顺序一致。运行时不开启这些开关时，
 * 不会增加额外同步。
 */
#pragma once

#include <atomic>

namespace rmdb {
namespace index_test {

inline std::atomic<bool> pause_after_seek{false};
inline std::atomic<int> pause_after_seek_hits{0};
inline std::atomic<bool> pause_before_next_leaf{false};
inline std::atomic<int> pause_before_next_leaf_hits{0};

inline void reset_barriers() {
    pause_after_seek.store(false, std::memory_order_release);
    pause_after_seek_hits.store(0, std::memory_order_release);
    pause_before_next_leaf.store(false, std::memory_order_release);
    pause_before_next_leaf_hits.store(0, std::memory_order_release);
}

}  // namespace index_test
}  // namespace rmdb
