/* Copyright (c) 2026 RMDB contributors.
 * 确定性索引范围扫描并发测试。
 *
 * 测试在两个扫描边界暂停，分别覆盖起始位置建立期间和叶页切换期间的并发修改。
 * 扫描使用逻辑 key 作为边界，因此叶页插入、删除或分裂不应导致漏行或越界。
 *
 * 运行：Linux 上 `build/bin/index_concurrency_test`。
 */

#include <gtest/gtest.h>

#include <cstring>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

#include "common/index_test_barrier.h"
#include "errors.h"
#include "index/ix.h"
#include "index/ix_index_handle.h"
#include "storage/buffer_pool_manager.h"
#include "storage/disk_manager.h"

namespace {

const std::string kTestDbName = "index_concurrency_test_db";

std::string int_key(int value) {
    std::string key(sizeof(int), '\0');
    std::memcpy(key.data(), &value, sizeof(int));
    return key;
}

std::vector<int> scan_keys(IxIndexHandle *ih, const std::string &lower, const std::string &upper) {
    IxScanBounds bounds;
    bounds.lower_key = lower;
    bounds.lower_mode = IxBoundMode::LowerBound;
    bounds.upper_key = upper;
    bounds.upper_mode = IxBoundMode::UpperBound;
    auto scan = ih->create_scan(bounds);
    std::vector<int> out;
    while (scan != nullptr && !scan->is_end()) {
        int value;
        std::memcpy(&value, scan->key(), sizeof(int));
        out.push_back(value);
        scan->next();
    }
    return out;
}

void insert_range(IxIndexHandle *ih, int first, int last, int rid_base) {
    for (int value = first; value <= last; ++value) {
        Rid rid{rid_base + value, value};
        auto outcome = ih->insert_entry(int_key(value).data(), rid, nullptr);
        ASSERT_EQ(outcome.result, IxInsertResult::kInserted) << "insert failed for key " << value;
    }
}

}  // namespace

class IndexConcurrencyTest : public ::testing::Test {
   public:
    static constexpr int kMaxFiles = 16;
    static constexpr int kMaxPages = 512;
    static constexpr int kTestIdxRoot = 0;

    std::unique_ptr<DiskManager> disk_manager_;
    std::unique_ptr<BufferPoolManager> buffer_pool_manager_;
    std::unique_ptr<IxManager> ix_manager_;
    IxIndexHandle *ih_ = nullptr;

    void SetUp() override {
        ::testing::Test::SetUp();
        disk_manager_ = std::make_unique<DiskManager>();
        if (!disk_manager_->is_dir(kTestDbName)) {
            disk_manager_->create_dir(kTestDbName);
        }
        if (chdir(kTestDbName.c_str()) < 0) {
            throw UnixError();
        }
        buffer_pool_manager_ =
            std::make_unique<BufferPoolManager>(kMaxFiles * kMaxPages, disk_manager_.get());
        ix_manager_ = std::make_unique<IxManager>(disk_manager_.get(), buffer_pool_manager_.get());
        const std::string table = "t";
        const std::string idx = table + "_id.idx";
        if (disk_manager_->is_file(idx)) {
            disk_manager_->destroy_file(idx);
        }
        ColMeta col;
        col.tab_name = table;
        col.name = "id";
        col.type = TYPE_INT;
        col.len = sizeof(int);
        col.offset = 0;
        ix_manager_->create_index(table, {col});
        ih_ = ix_manager_->open_index(table, {col}).release();
        rmdb::index_test::reset_barriers();
    }

    void TearDown() override {
        delete ih_;
        ih_ = nullptr;
        ix_manager_.reset();
        buffer_pool_manager_.reset();
        if (chdir("..") < 0) {
            throw UnixError();
        }
        rmdb::index_test::reset_barriers();
    }
};

// 完成 lower seek 后暂停，随后删除起始位置之前的 key。
// 扫描建立前仍持有叶页锁，删除不能使起始槽位失效。
TEST_F(IndexConcurrencyTest, LowerSlotShiftPausedAfterSeek) {
    insert_range(ih_, 90, 102, 0);
    rmdb::index_test::pause_after_seek.store(true, std::memory_order_release);

    std::vector<int> scanned;
    std::thread scanner([&] {
        scanned = scan_keys(ih_, int_key(100), int_key(std::numeric_limits<int>::max()));
    });
    // 等 seek 命中 barrier。
    while (rmdb::index_test::pause_after_seek_hits.load(std::memory_order_acquire) == 0) {
        std::this_thread::yield();
    }

    // 并发写者:删除 seek 位置之前的 key 90。修复后此删除会阻塞在叶页 latch 上,
    // 直到 T1 完成扫描建立;旧实现下删除抢先完成,槽位左移导致漏掉 100。
    std::thread deleter([&] { ih_->delete_entry(int_key(90).data(), nullptr); });

    // 短暂等待写者尝试进入(不要求其完成),再放行扫描。
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    rmdb::index_test::pause_after_seek.store(false, std::memory_order_release);
    scanner.join();
    deleter.join();

    ASSERT_EQ(scanned, (std::vector<int>{100, 101, 102}));
}

// 扫描在叶页之间暂停，随后插入超过上界的 key 并触发尾部叶页分裂。
// 恢复后仍须返回逻辑范围内的全部 key，不能因分裂漏行或越界。
TEST_F(IndexConcurrencyTest, LeafSplitDuringBetweenLeafPause) {
    insert_range(ih_, 1, 800, 0);
    rmdb::index_test::pause_before_next_leaf.store(true, std::memory_order_release);

    std::vector<int> scanned;
    std::thread scanner([&] {
        scanned = scan_keys(ih_, int_key(1), int_key(800));
    });
    while (rmdb::index_test::pause_before_next_leaf_hits.load(std::memory_order_acquire) == 0) {
        std::this_thread::yield();
    }

    // 并发插入 1000..1400(全部 > 上界 800),落在尾部叶页并迫使分裂。
    std::thread inserter([&] { insert_range(ih_, 1000, 1400, 1000); });
    inserter.join();

    rmdb::index_test::pause_before_next_leaf.store(false, std::memory_order_release);
    scanner.join();

    // 扫描必须恰好返回 [1, 800];1000..1400 虽已插入且可见,但超出逻辑上界,
    // 不得返回(修复前物理上界失效可能越界)。
    ASSERT_EQ(scanned.size(), static_cast<size_t>(800));
    for (size_t i = 0; i < scanned.size(); ++i) {
        ASSERT_EQ(scanned[i], static_cast<int>(i) + 1);
    }
}

// 扫描尚未读取上界 key 时删除它。恢复后结果应为 [1, 799]。
TEST_F(IndexConcurrencyTest, UpperSlotShiftDeletedBeforeRead) {
    insert_range(ih_, 1, 810, 0);
    rmdb::index_test::pause_before_next_leaf.store(true, std::memory_order_release);

    std::vector<int> scanned;
    std::thread scanner([&] {
        scanned = scan_keys(ih_, int_key(1), int_key(800));
    });
    while (rmdb::index_test::pause_before_next_leaf_hits.load(std::memory_order_acquire) == 0) {
        std::this_thread::yield();
    }

    // 删除上界 key 800；它位于扫描尚未读取的尾部叶页。
    ASSERT_TRUE(ih_->delete_entry(int_key(800).data(), nullptr));

    rmdb::index_test::pause_before_next_leaf.store(false, std::memory_order_release);
    scanner.join();

    ASSERT_EQ(scanned.size(), static_cast<size_t>(799));
    for (size_t i = 0; i < scanned.size(); ++i) {
        ASSERT_EQ(scanned[i], static_cast<int>(i) + 1);
    }
}
