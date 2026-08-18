/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#undef NDEBUG

// GCC 的 <sstream> 在 private 区域声明内部辅助类型。测试需要临时暴露 RMDB
// 内部成员，因此必须先完成标准库头文件解析，避免宏改写 libstdc++ 的访问控制。
#include <sstream>

#define private public

#include "record/rm.h"
#include "storage/buffer_pool_manager.h"

#undef private

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <future>
#include <limits>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <thread>  // NOLINT
#include <unordered_map>
#include <vector>

#include "gtest/gtest.h"
#include "common/catalog_epoch.h"
#include "common/checked_atomic_counter.h"
#include "common/lru_cache_store.h"
#include "common/interned_identifier.h"
#include "common/plan_template_materialization.h"
#include "common/reusable_flat_u64_set.h"
#include "common/runtime_feedback.h"
#include "common/scope_exit.h"
#include "common/sql_template_cache.h"
#include "common/tcp_command_framer.h"
#include "common/workload_feedback.h"
#include "execution/executor_abstract.h"
#include "common/compiled_access_program.h"
#include "execution/visible_tuple_ref.h"
#include "recovery/checkpoint_master.h"
#include "recovery/log_manager.h"
#include "recovery/log_recovery.h"
#include "replacer/lru_replacer.h"
#include "storage/disk_manager.h"
#include "transaction/statement_checkpoint_gate.h"
#include "transaction/snapshot_index_history_store.h"
#include "transaction/transaction_manager.h"
#include "transaction/transaction_registry.h"
#include "transaction/watermark.h"

const std::string TEST_DB_NAME = "BufferPoolManagerTest_db";  // 以数据库名作为根目录
const std::string TEST_FILE_NAME = "basic";                   // 测试文件的名字
const std::string TEST_FILE_NAME_CCUR = "concurrency";        // 测试文件的名字
const std::string TEST_FILE_NAME_BIG = "bigdata";             // 测试文件的名字
constexpr int MAX_FILES = 32;
constexpr int MAX_PAGES = 128;
constexpr size_t TEST_BUFFER_POOL_SIZE = MAX_FILES * MAX_PAGES;

TEST(ScopeExitTest, RunsCleanupUnlessReleased) {
    int cleanup_count = 0;
    {
        auto guard = rmdb::make_scope_exit([&] { ++cleanup_count; });
    }
    EXPECT_EQ(cleanup_count, 1);

    {
        auto guard = rmdb::make_scope_exit([&] { ++cleanup_count; });
        guard.release();
    }
    EXPECT_EQ(cleanup_count, 1);
}

TEST(ScopeExitTest, TransfersCleanupOnMove) {
    int cleanup_count = 0;
    {
        auto first = rmdb::make_scope_exit([&] { ++cleanup_count; });
        auto second = std::move(first);
        (void)second;
    }
    EXPECT_EQ(cleanup_count, 1);
}

// 创建BufferPoolManager
auto disk_manager = std::make_unique<DiskManager>();
auto buffer_pool_manager = std::make_unique<BufferPoolManager>(TEST_BUFFER_POOL_SIZE, disk_manager.get());

std::unordered_map<int, char *> mock;  // fd -> buffer

char *mock_get_page(int fd, int page_no) { return &mock[fd][page_no * PAGE_SIZE]; }

void check_disk(int fd, int page_no) {
    char buf[PAGE_SIZE];
    disk_manager->read_page(fd, page_no, buf, PAGE_SIZE);
    char *mock_buf = mock_get_page(fd, page_no);
    assert(memcmp(buf, mock_buf, PAGE_SIZE) == 0);
}

void check_disk_all() {
    for (auto &file : mock) {
        int fd = file.first;
        for (int page_no = 0; page_no < MAX_PAGES; page_no++) {
            check_disk(fd, page_no);
        }
    }
}

void check_cache(int fd, int page_no) {
    Page *page = buffer_pool_manager->fetch_page(PageId{fd, page_no});
    char *mock_buf = mock_get_page(fd, page_no);  // &mock[fd][page_no * PAGE_SIZE];
    assert(memcmp(page->get_data(), mock_buf, PAGE_SIZE) == 0);
    buffer_pool_manager->unpin_page(PageId{fd, page_no}, false);
}

void check_cache_all() {
    for (auto &file : mock) {
        int fd = file.first;
        for (int page_no = 0; page_no < MAX_PAGES; page_no++) {
            check_cache(fd, page_no);
        }
    }
}

void rand_buf(int size, char *buf) {
    for (int i = 0; i < size; i++) {
        int rand_ch = rand() & 0xff;
        buf[i] = rand_ch;
    }
}

int rand_fd() {
    assert(mock.size() == MAX_FILES);
    int fd_idx = rand() % MAX_FILES;
    auto it = mock.begin();
    for (int i = 0; i < fd_idx; i++) {
        it++;
    }
    return it->first;
}

struct rid_hash_t {
    size_t operator()(const Rid &rid) const { return (rid.page_no << 16) | rid.slot_no; }
};

struct rid_equal_t {
    bool operator()(const Rid &x, const Rid &y) const { return x.page_no == y.page_no && x.slot_no == y.slot_no; }
};

void check_equal(const RmFileHandle *file_handle,
                 const std::unordered_map<Rid, std::string, rid_hash_t, rid_equal_t> &mock) {
    // Test all records
    for (auto &entry : mock) {
        Rid rid = entry.first;
        auto mock_buf = (char *)entry.second.c_str();
        auto rec = file_handle->get_record(rid, nullptr);
        assert(memcmp(mock_buf, rec->data, file_handle->file_hdr_.record_size) == 0);
    }
    // Randomly get record
    for (int i = 0; i < 10; i++) {
        Rid rid = {.page_no = 1 + rand() % (file_handle->file_hdr_.num_pages - 1),
                   .slot_no = rand() % file_handle->file_hdr_.num_records_per_page};
        bool mock_exist = mock.count(rid) > 0;
        bool rm_exist = file_handle->is_record(rid);
        assert(rm_exist == mock_exist);
    }
    // Test RM scan
    size_t num_records = 0;
    for (RmScan scan(file_handle); !scan.is_end(); scan.next()) {
        assert(mock.count(scan.rid()) > 0);
        auto rec = file_handle->get_record(scan.rid(), nullptr);
        assert(memcmp(rec->data, mock.at(scan.rid()).c_str(), file_handle->file_hdr_.record_size) == 0);
        num_records++;
    }
    assert(num_records == mock.size());
}

// std::cout can call this, for example: std::cout << rid
std::ostream &operator<<(std::ostream &os, const Rid &rid) {
    return os << '(' << rid.page_no << ", " << rid.slot_no << ')';
}

/** 注意：每个测试点只测试了单个文件！
 * 对于每个测试点，先创建和进入目录TEST_DB_NAME
 * 然后在此目录下创建和打开文件TEST_FILE_NAME_BIG，记录其文件描述符fd */

class BigStorageTest : public ::testing::Test {
   public:
    std::unique_ptr<DiskManager> disk_manager_;
    int fd_ = -1;  // 此文件描述符为disk_manager_->open_file的返回值

   public:
    // This function is called before every test.
    void SetUp() override {
        ::testing::Test::SetUp();
        // For each test, we create a new DiskManager
        disk_manager_ = std::make_unique<DiskManager>();
        // 如果测试目录不存在，则先创建测试目录
        if (!disk_manager_->is_dir(TEST_DB_NAME)) {
            disk_manager_->create_dir(TEST_DB_NAME);
        }
        assert(disk_manager_->is_dir(TEST_DB_NAME));
        // 进入测试目录
        if (chdir(TEST_DB_NAME.c_str()) < 0) {
            throw UnixError();
        }
        // 如果测试文件存在，则先删除原文件（最后留下来的文件存的是最后一个测试点的数据）
        if (disk_manager_->is_file(TEST_FILE_NAME_BIG)) {
            disk_manager_->destroy_file(TEST_FILE_NAME_BIG);
        }
        // 创建测试文件
        disk_manager_->create_file(TEST_FILE_NAME_BIG);
        assert(disk_manager_->is_file(TEST_FILE_NAME_BIG));
        // 打开测试文件
        fd_ = disk_manager_->open_file(TEST_FILE_NAME_BIG);
        assert(fd_ != -1);
    }

    // This function is called after every test.
    void TearDown() override {
        disk_manager_->close_file(fd_);
        // disk_manager_->destroy_file(TEST_FILE_NAME_BIG);  // you can choose to delete the file

        // 返回上一层目录
        if (chdir("..") < 0) {
            throw UnixError();
        }
        assert(disk_manager_->is_dir(TEST_DB_NAME));
    };
};

TEST(LRUReplacerTest, SampleTest) {
    LRUReplacer lru_replacer(7);

    // Scenario: unpin six elements, i.e. add them to the replacer.
    lru_replacer.unpin(1);
    lru_replacer.unpin(2);
    lru_replacer.unpin(3);
    lru_replacer.unpin(4);
    lru_replacer.unpin(5);
    lru_replacer.unpin(6);
    lru_replacer.unpin(1);
    EXPECT_EQ(6, lru_replacer.Size());

    // Scenario: get three victims from the lru.
    int value;
    lru_replacer.victim(&value);
    EXPECT_EQ(1, value);
    lru_replacer.victim(&value);
    EXPECT_EQ(2, value);
    lru_replacer.victim(&value);
    EXPECT_EQ(3, value);

    // Scenario: pin elements in the replacer.
    // Note that 3 has already been victimized, so pinning 3 should have no effect.
    lru_replacer.pin(3);
    lru_replacer.pin(4);
    EXPECT_EQ(2, lru_replacer.Size());

    // Scenario: unpin 4. We expect that the reference bit of 4 will be set to 1.
    lru_replacer.unpin(4);

    // Scenario: continue looking for victims. We expect these victims.
    lru_replacer.victim(&value);
    EXPECT_EQ(5, value);
    lru_replacer.victim(&value);
    EXPECT_EQ(6, value);
    lru_replacer.victim(&value);
    EXPECT_EQ(4, value);
}

TEST(LRUReplacerTest, ConcurrentVictimsClaimEachFrameOnce) {
    constexpr int kFrameCount = 4096;
    constexpr int kThreads = 8;
    LRUReplacer replacer(kFrameCount);
    for (int frame_id = 0; frame_id < kFrameCount; ++frame_id) {
        replacer.unpin(frame_id);
    }

    auto seen = std::make_unique<std::atomic<rmdb::u8>[]>(kFrameCount);
    for (int frame_id = 0; frame_id < kFrameCount; ++frame_id) {
        seen[frame_id].store(0, std::memory_order_relaxed);
    }
    std::atomic<int> claimed{0};
    std::atomic<int> duplicates{0};
    std::vector<std::thread> workers;
    for (int thread_id = 0; thread_id < kThreads; ++thread_id) {
        workers.emplace_back([&] {
            frame_id_t frame_id;
            while (replacer.victim(&frame_id)) {
                if (seen[frame_id].fetch_add(1, std::memory_order_relaxed) != 0) {
                    duplicates.fetch_add(1, std::memory_order_relaxed);
                }
                claimed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto &worker : workers) {
        worker.join();
    }

    EXPECT_EQ(duplicates.load(), 0);
    EXPECT_EQ(claimed.load(), kFrameCount);
    EXPECT_EQ(replacer.Size(), 0U);
}

TEST(ReleaseRuntimeInvariantTest, RejectsCounterUnderflowAndInvalidFileDescriptor) {
    Transaction txn(1);
    EXPECT_THROW(txn.ReleaseUndoReference(), InternalError);
    EXPECT_EQ(txn.GetUndoReferenceCount(), 0);

    txn.RetainUndoReference();
    EXPECT_TRUE(txn.ReleaseUndoReference());
    EXPECT_EQ(txn.GetUndoReferenceCount(), 0);

    txn.RetainUndoReference();
    txn.RetainUndoReference();
    EXPECT_FALSE(txn.ReleaseUndoReference());
    EXPECT_TRUE(txn.ReleaseUndoReference());
    EXPECT_TRUE(txn.TryMarkUndoReadyEnqueued());
    EXPECT_FALSE(txn.TryMarkUndoReadyEnqueued());
    txn.ClearUndoReadyEnqueued();
    EXPECT_TRUE(txn.TryMarkUndoReadyEnqueued());

    std::atomic<rmdb::u32> page_counter{0};
    EXPECT_THROW(rmdb::atomic_counter::DecrementPositive(page_counter, "Page counter", rmdb::u32{1}),
                 InternalError);
    rmdb::atomic_counter::Increment(page_counter, rmdb::u32{1});
    EXPECT_NO_THROW(rmdb::atomic_counter::DecrementPositive(page_counter, "Page counter", rmdb::u32{1}));
    EXPECT_EQ(page_counter.load(), 0U);

    Transaction chain_owner(2);
    UndoLog older;
    older.ts_ = 40;
    older.prev_version_ = UndoLink{1, 7};
    const UndoLink older_link = chain_owner.AppendUndoLog(std::move(older));
    UndoLink next;
    UndoLink released;
    EXPECT_EQ(chain_owner.InspectOrCutUndoPredecessor(
                  static_cast<size_t>(older_link.prev_log_idx_), 39, &next, &released),
              Transaction::UndoGcLinkResult::CONTINUE);
    EXPECT_EQ(next, (UndoLink{1, 7}));
    EXPECT_FALSE(released.IsValid());
    EXPECT_EQ(chain_owner.InspectOrCutUndoPredecessor(
                  static_cast<size_t>(older_link.prev_log_idx_), 40, &next, &released),
              Transaction::UndoGcLinkResult::BOUNDARY);
    EXPECT_EQ(released, (UndoLink{1, 7}));
    EXPECT_FALSE(chain_owner.GetUndoLog(older_link.prev_log_idx_).prev_version_.IsValid());

    DiskManager local_disk_manager;
    EXPECT_THROW(local_disk_manager.allocate_page(-1), FileNotOpenError);
    EXPECT_THROW(local_disk_manager.allocate_page(DiskManager::MAX_FD), FileNotOpenError);
}

TEST(CompiledComparatorTest, EvaluatesTypedPredicatesWithoutFunctionPointers) {
    int int_lhs = 7;
    int int_rhs = 9;
    auto int_lt = AbstractExecutor::compile_comparator(TYPE_INT, OP_LT);
    EXPECT_TRUE(AbstractExecutor::eval_compiled_comparator(
        int_lt, reinterpret_cast<const char *>(&int_lhs), reinterpret_cast<const char *>(&int_rhs), sizeof(int)));

    float float_lhs = 3.5f;
    float float_rhs = 3.5f;
    auto float_eq = AbstractExecutor::compile_comparator(TYPE_FLOAT, OP_EQ);
    EXPECT_TRUE(AbstractExecutor::eval_compiled_comparator(
        float_eq, reinterpret_cast<const char *>(&float_lhs), reinterpret_cast<const char *>(&float_rhs),
        sizeof(float)));

    const char bytes_lhs[] = "abc";
    const char bytes_rhs[] = "abd";
    auto bytes_lt = AbstractExecutor::compile_comparator(TYPE_STRING, OP_LT);
    EXPECT_TRUE(AbstractExecutor::eval_compiled_comparator(bytes_lt, bytes_lhs, bytes_rhs, 3));
}

TEST(CompiledAccessProgramTest, BindsLiteralsWithoutCachingTheirAddress) {
    ColMeta key_col{"t", "id", TYPE_INT, static_cast<int>(sizeof(int)), 0, true};
    IndexMeta index;
    index.index_id = 42;
    index.col_num = 1;
    index.col_tot_len = sizeof(int);
    index.cols.push_back(key_col);

    Condition condition;
    condition.lhs_col = {"t", "id"};
    condition.op = OP_GE;
    condition.is_rhs_val = true;
    condition.rhs_val.set_int(7);
    condition.rhs_val.init_raw(sizeof(int));
    std::vector<Condition> conditions{condition};

    auto program = rmdb::CompiledAccessProgram::BuildIndex(index, conditions, "t", 9);
    ASSERT_NE(program, nullptr);
    EXPECT_TRUE(program->Matches(index, conditions.size(), 9));
    EXPECT_FALSE(program->exact_unique_key());
    EXPECT_EQ(program->equality_prefix_cols(), 0);
    EXPECT_FALSE(program->Matches(index, conditions.size(), 10));

    int key = 8;
    EXPECT_TRUE(program->EvaluateIndexKey(reinterpret_cast<const char *>(&key), conditions));
    conditions[0].rhs_val = Value();
    conditions[0].rhs_val.set_int(10);
    conditions[0].rhs_val.init_raw(sizeof(int));
    EXPECT_FALSE(program->EvaluateIndexKey(reinterpret_cast<const char *>(&key), conditions));

    conditions[0].op = OP_EQ;
    auto point_program = rmdb::CompiledAccessProgram::BuildIndex(index, conditions, "t", 9);
    ASSERT_NE(point_program, nullptr);
    EXPECT_TRUE(point_program->exact_unique_key());
    EXPECT_EQ(point_program->equality_prefix_cols(), 1);
}

TEST(CompiledAccessProgramTest, MaterializesSameFixedPrefixRangeAsGenericBuilder) {
    IndexMeta index;
    index.index_id = 77;
    index.col_num = 2;
    index.col_tot_len = 2 * static_cast<int>(sizeof(int));
    index.cols.push_back(ColMeta{"t", "a", TYPE_INT, static_cast<int>(sizeof(int)), 0, true});
    index.cols.push_back(ColMeta{"t", "b", TYPE_INT, static_cast<int>(sizeof(int)), 4, true});
    auto make_condition = [](const char *name, CompOp op, int value) {
        Condition condition;
        condition.lhs_col = {"t", name};
        condition.op = op;
        condition.is_rhs_val = true;
        condition.rhs_val.set_int(value);
        condition.rhs_val.init_raw(sizeof(int));
        return condition;
    };
    std::vector<Condition> conditions{
        make_condition("a", OP_EQ, 1), make_condition("b", OP_GE, 10), make_condition("b", OP_LT, 20)};
    auto program = rmdb::CompiledAccessProgram::BuildIndex(index, conditions, "t", 3);
    ASSERT_NE(program, nullptr);
    rmdb::IndexRangeSpec compiled;
    ASSERT_TRUE(program->MaterializeRange(index, conditions, &compiled));
    auto generic = rmdb::build_index_range_spec(index, conditions, "t");
    EXPECT_EQ(compiled.lower_key, generic.lower_key);
    EXPECT_EQ(compiled.upper_key, generic.upper_key);
    EXPECT_EQ(compiled.lower_lookup, generic.lower_lookup);
    EXPECT_EQ(compiled.upper_lookup, generic.upper_lookup);
    EXPECT_EQ(compiled.equality_prefix_cols, generic.equality_prefix_cols);
    EXPECT_EQ(compiled.scan_prefix_len, generic.scan_prefix_len);
    EXPECT_EQ(compiled.all_conditions_consumed, generic.all_conditions_consumed);
}

TEST(CompiledMutationProgramTest, CachesOffsetsAndIndexTouchShape) {
    TabMeta table;
    table.name = "t";
    table.cols.push_back(ColMeta{"t", "id", TYPE_INT, 4, 0, true});
    table.cols.push_back(ColMeta{"t", "balance", TYPE_FLOAT, 4, 4, false});
    IndexMeta index;
    index.index_id = 9;
    index.col_num = 1;
    index.col_tot_len = 4;
    index.cols.push_back(table.cols[0]);
    table.indexes.push_back(index);

    SetClause clause;
    clause.lhs = {"t", "balance"};
    clause.op = SetOp::ADD;
    clause.rhs_is_col = true;
    clause.rhs_col = {"t", "balance"};
    std::vector<SetClause> clauses{clause};
    auto program = rmdb::CompiledMutationProgram::BuildUpdate(table, clauses, 4);
    ASSERT_NE(program, nullptr);
    ASSERT_TRUE(program->Matches(1, 1, 4));
    ASSERT_EQ(program->slots().size(), 1U);
    EXPECT_EQ(program->slots()[0].lhs_offset, 4);
    EXPECT_EQ(program->slots()[0].rhs_offset, 4);
    EXPECT_FALSE(program->touches_index());
    EXPECT_FALSE(program->index_touched()[0]);

    clauses[0].lhs = {"t", "id"};
    auto key_program = rmdb::CompiledMutationProgram::BuildUpdate(table, clauses, 4);
    ASSERT_NE(key_program, nullptr);
    EXPECT_TRUE(key_program->touches_index());
    EXPECT_TRUE(key_program->key_conflict_required());
    EXPECT_TRUE(key_program->index_touched()[0]);
}

TEST(RuntimeFeedbackTest, CollectorFlushesExactlyOnce) {
    auto node = std::make_shared<rmdb::RuntimeNodeFeedback>(rmdb::RuntimeNodeKind::kIndexScan, 7);
    {
        rmdb::RuntimeScanFeedbackCollector collector(node, rmdb::RuntimeNodeKind::kIndexScan);
        collector.begin();
        collector.add_rows_scanned(3);
        collector.add_rows_visible(2);
        collector.add_rows_output(1);
        collector.add_index_entries(4);
        collector.add_heap_fetches(2);
        collector.add_index_only_rows(1);
        collector.flush();
        collector.flush();
    }

    auto snapshot = rmdb::load_scan_feedback_snapshot(node);
    EXPECT_EQ(snapshot.executions, 1);
    EXPECT_EQ(snapshot.rows_scanned, 3);
    EXPECT_EQ(snapshot.rows_visible, 2);
    EXPECT_EQ(snapshot.rows_output, 1);
    EXPECT_EQ(snapshot.index_entries, 4);
    EXPECT_EQ(snapshot.heap_fetches, 2);
    EXPECT_EQ(snapshot.index_only_rows, 1);
}

TEST(RuntimeFeedbackTest, RejectsMismatchedPlanShape) {
    auto node = std::make_shared<rmdb::RuntimeNodeFeedback>(rmdb::RuntimeNodeKind::kIndexScan, 7);
    {
        rmdb::RuntimeScanFeedbackCollector collector(node, rmdb::RuntimeNodeKind::kSeqScan);
        collector.begin();
        collector.add_rows_scanned(3);
        collector.add_rows_output(2);
    }

    auto snapshot = rmdb::load_scan_feedback_snapshot(node);
    EXPECT_EQ(snapshot.executions, 0);
    EXPECT_EQ(snapshot.rows_scanned, 0);
    EXPECT_EQ(snapshot.rows_output, 0);
}

TEST(RuntimeFeedbackTest, PropagatesCatalogGeneration) {
    rmdb::u64 generation = rmdb::catalog_feedback_generation();
    rmdb::RuntimeFeedbackStore store(generation);
    auto node = store.ensure(3, rmdb::RuntimeNodeKind::kIndexScan);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(store.generation, generation);
    EXPECT_EQ(node->generation, generation);

    rmdb::advance_catalog_data_epoch();
    EXPECT_GT(rmdb::catalog_feedback_generation(), generation);
}

TEST(RuntimeFeedbackTest, DefaultsToCurrentWorkloadGeneration) {
    rmdb::RuntimeFeedbackStore store;
    EXPECT_EQ(store.generation, rmdb::workload_feedback_generation());
}

TEST(WorkloadFeedbackTest, AdvancesDataAndFeedbackGenerationTogether) {
    auto before = rmdb::WorkloadFeedbackLifecycle::Instance().Snapshot();
    rmdb::advance_workload_data_generation();
    auto after = rmdb::WorkloadFeedbackLifecycle::Instance().Snapshot();

    EXPECT_GT(after.data_epoch, before.data_epoch);
    EXPECT_GT(after.feedback_generation, before.feedback_generation);
}

TEST(StatementScratchTest, ReusesConnectionLocalStringCapacity) {
    StatementScratch scratch;
    std::string *first = scratch.acquire_string(128);
    ASSERT_NE(first, nullptr);
    const size_t capacity = first->capacity();
    scratch.reset();
    std::string *second = scratch.acquire_string(32);
    EXPECT_EQ(first, second);
    EXPECT_GE(second->capacity(), capacity);

    auto first_rids = scratch.acquire_rids();
    first_rids->reserve(64);
    first_rids->push_back(Rid{1, 2});
    auto *rid_storage = first_rids.get();
    scratch.reset();
    auto second_rids = scratch.acquire_rids();
    EXPECT_EQ(second_rids.get(), rid_storage);
    EXPECT_TRUE(second_rids->empty());
    EXPECT_GE(second_rids->capacity(), 64U);
}

TEST(StatementScratchTest, KeepsAcquiredStringAddressesStable) {
    StatementScratch scratch;
    std::string *first = scratch.acquire_string(8);
    first->assign("join-key");

    for (size_t i = 0; i < 256; ++i) {
        scratch.acquire_string(i + 1);
    }

    EXPECT_EQ(*first, "join-key");
    first->resize(16, 'x');
    EXPECT_EQ(first->size(), 16);
}

TEST(TcpCommandFramerTest, HandlesFragmentedAndCoalescedCommands) {
    rmdb::TcpCommandFramer framer(128);
    std::string command;

    framer.Append("select 'a;", 10);
    EXPECT_EQ(framer.Next(&command), rmdb::TcpFrameStatus::kNeedMore);
    framer.Append("b';select 2;\0", 13);
    EXPECT_EQ(framer.Next(&command), rmdb::TcpFrameStatus::kCommand);
    EXPECT_EQ(command, "select 'a;b';");
    EXPECT_EQ(framer.Next(&command), rmdb::TcpFrameStatus::kCommand);
    EXPECT_EQ(command, "select 2;");
    EXPECT_EQ(framer.Next(&command), rmdb::TcpFrameStatus::kNeedMore);
}

TEST(TcpCommandFramerTest, RequiresDelimiterAndRejectsOversize) {
    rmdb::TcpCommandFramer framer(32);
    std::string command;

    framer.Append("select 1", sizeof("select 1") - 1);
    EXPECT_EQ(framer.Next(&command), rmdb::TcpFrameStatus::kNeedMore);
    framer.Append(";", 1);
    EXPECT_EQ(framer.Next(&command), rmdb::TcpFrameStatus::kCommand);
    EXPECT_EQ(command, "select 1;");

    framer.Append("123456789012345678901234567890123", 33);
    EXPECT_EQ(framer.Next(&command), rmdb::TcpFrameStatus::kTooLarge);
}

TEST(RuntimeFeedbackTest, JoinCollectorUsesTypedPayload) {
    auto feedback = std::make_shared<rmdb::RuntimeNodeFeedback>(rmdb::RuntimeNodeKind::kJoin, 3);
    {
        rmdb::RuntimeJoinFeedbackCollector collector(feedback);
        collector.begin();
        collector.add_left_rows(2);
        collector.add_right_rows(5);
        collector.add_candidate_pairs(4);
        collector.add_rows_output(1);
        collector.flush();
        collector.flush();
    }
    EXPECT_EQ(feedback->join.executions.load(), 1U);
    EXPECT_EQ(feedback->join.left_rows.load(), 2U);
    EXPECT_EQ(feedback->join.right_rows.load(), 5U);
    EXPECT_EQ(feedback->join.candidate_pairs.load(), 4U);
    EXPECT_EQ(feedback->join.rows_output.load(), 1U);
    EXPECT_EQ(feedback->scan.executions.load(), 0U);

    auto scan_feedback = std::make_shared<rmdb::RuntimeNodeFeedback>(rmdb::RuntimeNodeKind::kSeqScan, 4);
    rmdb::RuntimeJoinFeedbackCollector mismatched(scan_feedback);
    mismatched.begin();
    mismatched.add_rows_output();
    mismatched.flush();
    EXPECT_EQ(scan_feedback->join.executions.load(), 0U);
}

TEST(TupleViewLifetimeTest, MoveRebindsOwnedFallbackView) {
    auto record = std::make_unique<RmRecord>(sizeof(int));
    int expected = 42;
    std::memcpy(record->data, &expected, sizeof(expected));
    auto source = AbstractExecutor::TupleViewRef::Owned(std::move(record));

    AbstractExecutor::TupleViewRef moved = std::move(source);
    ASSERT_TRUE(moved);
    EXPECT_TRUE(moved.owns_record());
    EXPECT_EQ(moved.ownership(), AbstractExecutor::TupleViewRef::Ownership::Owned);
    EXPECT_EQ(*reinterpret_cast<const int *>(moved->record->data), expected);
    EXPECT_FALSE(source);
    EXPECT_EQ(source.ownership(), AbstractExecutor::TupleViewRef::Ownership::Empty);

    AbstractExecutor::TupleView borrowed_view;
    borrowed_view.record = moved->record;
    auto borrowed = AbstractExecutor::TupleViewRef::Borrowed(&borrowed_view);
    EXPECT_EQ(borrowed.ownership(), AbstractExecutor::TupleViewRef::Ownership::Borrowed);
    EXPECT_FALSE(borrowed.owns_record());
}

TEST(LruCacheStoreTest, EvictsLeastRecentlyUsedAndDropsInvalidEntry) {
    rmdb::LruCacheStore<int, int> store(2);
    store.Store(1, 10);
    store.Store(2, 20);

    auto first = store.Lookup(1, [](int) { return true; });
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(*first, 10);

    store.Store(3, 30);
    EXPECT_FALSE(store.Lookup(2, [](int) { return true; }).has_value());
    EXPECT_EQ(store.SizeForTest(), 2);

    EXPECT_FALSE(store.Lookup(1, [](int) { return false; }).has_value());
    EXPECT_EQ(store.SizeForTest(), 1);
}

TEST(SqlTemplateCacheTest, ClassifiesAsciiKeywordsWithoutChangingTemplateKeys) {
    auto lower = rmdb::make_sql_template_candidate("select c_id from customer where c_id = 17;");
    auto upper = rmdb::make_sql_template_candidate(" SELECT c_id FROM customer WHERE c_id = 29 ; ");
    ASSERT_TRUE(lower.has_value());
    ASSERT_TRUE(upper.has_value());
    EXPECT_EQ(lower->key.h1, upper->key.h1);
    EXPECT_EQ(lower->key.h2, upper->key.h2);
    EXPECT_EQ(lower->key.normalized_len, upper->key.normalized_len);
    ASSERT_EQ(lower->literals.size(), 1U);
    ASSERT_EQ(upper->literals.size(), 1U);
    EXPECT_EQ(lower->literals[0].int_val, 17);
    EXPECT_EQ(upper->literals[0].int_val, 29);

    EXPECT_FALSE(rmdb::make_sql_template_candidate("BEGIN;").has_value());
    EXPECT_FALSE(rmdb::make_sql_template_candidate("static_checkpoint;").has_value());
    EXPECT_TRUE(rmdb::make_sql_template_candidate("select selectivity from t where id=1;").has_value());
}

TEST(SqlTemplateCacheTest, BorrowsStringLiteralsFromStatementBuffer) {
    std::string sql = "insert into t values ('a string literal longer than sso');";
    auto candidate = rmdb::make_sql_template_candidate(sql.c_str());
    ASSERT_TRUE(candidate.has_value());
    ASSERT_EQ(candidate->literals.size(), 1U);
    const auto literal = candidate->literals[0].str_val;
    EXPECT_EQ(literal, "a string literal longer than sso");
    EXPECT_GE(literal.data(), sql.data());
    EXPECT_LT(literal.data(), sql.data() + sql.size());
}

/** 注意：每个测试点只测试了单个文件！
 * 对于每个测试点，先创建和进入目录TEST_DB_NAME
 * 然后在此目录下创建和打开文件TEST_FILE_NAME，记录其文件描述符fd */
class BufferPoolManagerTest : public ::testing::Test {
   public:
    std::unique_ptr<DiskManager> disk_manager_;
    int fd_ = -1;  // 此文件描述符为disk_manager_->open_file的返回值

   public:
    // This function is called before every test.
    void SetUp() override {
        ::testing::Test::SetUp();
        // For each test, we create a new DiskManager
        disk_manager_ = std::make_unique<DiskManager>();
        // 如果测试目录不存在，则先创建测试目录
        if (!disk_manager_->is_dir(TEST_DB_NAME)) {
            disk_manager_->create_dir(TEST_DB_NAME);
        }
        assert(disk_manager_->is_dir(TEST_DB_NAME));
        // 进入测试目录
        if (chdir(TEST_DB_NAME.c_str()) < 0) {
            throw UnixError();
        }
        // 如果测试文件存在，则先删除原文件（最后留下来的文件存的是最后一个测试点的数据）
        if (disk_manager_->is_file(TEST_FILE_NAME)) {
            disk_manager_->destroy_file(TEST_FILE_NAME);
        }
        // 创建测试文件
        disk_manager_->create_file(TEST_FILE_NAME);
        assert(disk_manager_->is_file(TEST_FILE_NAME));
        // 打开测试文件
        fd_ = disk_manager_->open_file(TEST_FILE_NAME);
        assert(fd_ != -1);
    }

    // This function is called after every test.
    void TearDown() override {
        disk_manager_->close_file(fd_);
        // disk_manager_->destroy_file(TEST_FILE_NAME);  // you can choose to delete the file

        // 返回上一层目录
        if (chdir("..") < 0) {
            throw UnixError();
        }
        assert(disk_manager_->is_dir(TEST_DB_NAME));
    };
};

// NOLINTNEXTLINE
TEST_F(BufferPoolManagerTest, SampleTest) {
    // create BufferPoolManager
    const size_t buffer_pool_size = 10;
    auto disk_manager = BufferPoolManagerTest::disk_manager_.get();
    auto bpm = std::make_unique<BufferPoolManager>(buffer_pool_size, disk_manager);
    // create tmp PageId
    int fd = BufferPoolManagerTest::fd_;
    PageId page_id_temp = {.fd = fd, .page_no = INVALID_PAGE_ID};
    auto *page0 = bpm->new_page(&page_id_temp);

    // Scenario: The buffer pool is empty. We should be able to create a new page.
    ASSERT_NE(nullptr, page0);
    EXPECT_EQ(0, page_id_temp.page_no);

    // Scenario: Once we have a page, we should be able to read and write content.
    snprintf(page0->get_data(), sizeof(page0->get_data()), "Hello");
    EXPECT_EQ(0, strcmp(page0->get_data(), "Hello"));

    // Scenario: We should be able to create new pages until we fill up the buffer pool.
    for (size_t i = 1; i < buffer_pool_size; ++i) {
        EXPECT_NE(nullptr, bpm->new_page(&page_id_temp));
    }

    // Scenario: Once the buffer pool is full, we should not be able to create any new pages.
    for (size_t i = buffer_pool_size; i < buffer_pool_size * 2; ++i) {
        EXPECT_EQ(nullptr, bpm->new_page(&page_id_temp));
    }

    // Scenario: After unpinning pages {0, 1, 2, 3, 4} and pinning another 4 new pages,
    // there would still be one cache frame left for reading page 0.
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(true, bpm->unpin_page(PageId{fd, i}, true));
    }
    for (int i = 0; i < 4; ++i) {
        EXPECT_NE(nullptr, bpm->new_page(&page_id_temp));
    }

    // Scenario: We should be able to fetch the data we wrote a while ago.
    page0 = bpm->fetch_page(PageId{fd, 0});
    EXPECT_EQ(0, strcmp(page0->get_data(), "Hello"));
    EXPECT_EQ(true, bpm->unpin_page(PageId{fd, 0}, true));
    // new_page again, and now all buffers are pinned. Page 0 would be failed to fetch.
    EXPECT_NE(nullptr, bpm->new_page(&page_id_temp));
    EXPECT_EQ(nullptr, bpm->fetch_page(PageId{fd, 0}));

    bpm->flush_all_pages(fd);
}

TEST_F(BufferPoolManagerTest, DedicatedWalWriterPublishesDurableLsnToConcurrentWaiters) {
    auto *disk = BufferPoolManagerTest::disk_manager_.get();
    if (disk->is_file(LOG_FILE_NAME)) {
        disk->destroy_file(LOG_FILE_NAME);
    }
    disk->create_file(LOG_FILE_NAME);

    lsn_t final_offset = 0;
    {
        auto manager = std::make_unique<LogManager>(disk);
        constexpr int thread_count = 8;
        constexpr int records_per_thread = 100;
        std::promise<void> start_promise;
        std::shared_future<void> start = start_promise.get_future().share();
        std::vector<std::future<void>> writers;
        for (int thread_id = 0; thread_id < thread_count; ++thread_id) {
            writers.push_back(std::async(std::launch::async, [&, thread_id] {
                start.wait();
                lsn_t target = INVALID_LSN;
                for (int sequence = 0; sequence < records_per_thread; ++sequence) {
                    BeginLogRecord record(thread_id);
                    target = manager->add_log_to_buffer(&record) + record.log_tot_len_ - 1;
                }
                manager->flush_log_to_disk_until_group(target);
                EXPECT_GE(manager->get_persist_lsn(), target);
            }));
        }
        start_promise.set_value();
        for (auto &writer : writers) {
            writer.get();
        }
        manager->flush_log_to_disk();
        final_offset = manager->get_log_file_offset();
        EXPECT_EQ(manager->get_persist_lsn(), final_offset - 1);
    }

    EXPECT_EQ(disk->get_file_size(LOG_FILE_NAME), final_offset);
    const int log_fd = disk->GetLogFd();
    ASSERT_GE(log_fd, 0);
    disk->close_file(log_fd);
    disk->destroy_file(LOG_FILE_NAME);
}

TEST_F(BufferPoolManagerTest, WalWriterIgnoresStaleGenerationLsnWithActiveData) {
    auto *disk = BufferPoolManagerTest::disk_manager_.get();
    if (disk->is_file(LOG_FILE_NAME)) {
        disk->destroy_file(LOG_FILE_NAME);
    }
    disk->create_file(LOG_FILE_NAME);

    {
        auto manager = std::make_unique<LogManager>(disk);
        BeginLogRecord record(1);
        const lsn_t current_end = manager->add_log_to_buffer(&record) + record.log_tot_len_;

        // Simulate a dirty buffer frame whose page header still contains an
        // LSN from the WAL generation before a durable reset. The current WAL
        // already has active data, which used to bypass the stale-LSN guard.
        manager->flush_log_to_disk_until(current_end + 1024 * 1024);
        manager->flush_log_to_disk();
        EXPECT_EQ(manager->get_persist_lsn(), current_end - 1);
    }

    const int log_fd = disk->GetLogFd();
    ASSERT_GE(log_fd, 0);
    disk->close_file(log_fd);
    disk->destroy_file(LOG_FILE_NAME);
}

TEST_F(BufferPoolManagerTest, DeferredExplicitBeginWritesWalOnlyAfterPromotion) {
    auto *disk = BufferPoolManagerTest::disk_manager_.get();
    if (disk->is_file(LOG_FILE_NAME)) {
        disk->destroy_file(LOG_FILE_NAME);
    }
    disk->create_file(LOG_FILE_NAME);

    {
        auto log = std::make_unique<LogManager>(disk);
        TransactionManager manager(nullptr, nullptr);
        Transaction *read_only =
            manager.begin_read_only(IsolationLevel::SNAPSHOT_ISOLATION, true);
        ASSERT_NE(read_only, nullptr);
        EXPECT_EQ(log->get_log_file_offset(), 0);
        manager.commit(read_only, log.get());
        EXPECT_EQ(log->get_log_file_offset(), 0);

        Transaction *writer =
            manager.begin_read_only(IsolationLevel::SNAPSHOT_ISOLATION, true);
        const txn_id_t reserved_id = writer->get_transaction_id();
        manager.PromoteToWrite(writer, log.get());
        EXPECT_EQ(writer->get_transaction_id(), reserved_id);
        EXPECT_EQ(log->get_log_file_offset(), LOG_HEADER_SIZE);
        manager.abort(writer, log.get());
        log->flush_log_to_disk();
        manager.GarbageCollection();
    }

    const int log_fd = disk->GetLogFd();
    ASSERT_GE(log_fd, 0);
    disk->close_file(log_fd);
    disk->destroy_file(LOG_FILE_NAME);
}

TEST_F(BufferPoolManagerTest, ReadOnlyCommitWaitsForSnapshotWalDependency) {
    auto *disk = BufferPoolManagerTest::disk_manager_.get();
    if (disk->is_file(LOG_FILE_NAME)) {
        disk->destroy_file(LOG_FILE_NAME);
    }
    disk->create_file(LOG_FILE_NAME);

    {
        auto log = std::make_unique<LogManager>(disk);
        BeginLogRecord predecessor(17);
        const lsn_t dependency =
            log->add_log_to_buffer(&predecessor) + predecessor.log_tot_len_ - 1;
        ASSERT_LT(log->get_persist_lsn(), dependency);

        TransactionManager manager(nullptr, nullptr);
        Transaction *read_only =
            manager.begin_read_only(IsolationLevel::SNAPSHOT_ISOLATION);
        read_only->set_snapshot_durability_lsn(dependency);
        manager.commit(read_only, log.get());

        EXPECT_GE(log->get_persist_lsn(), dependency);
        EXPECT_EQ(manager.ActiveTransactionCount(), 0U);
    }

    const int log_fd = disk->GetLogFd();
    ASSERT_GE(log_fd, 0);
    disk->close_file(log_fd);
    disk->destroy_file(LOG_FILE_NAME);
}

TEST_F(BufferPoolManagerTest, SegmentedWalPreservesAbsoluteLsnAcrossBoundary) {
    auto *disk = BufferPoolManagerTest::disk_manager_.get();
    if (disk->is_file(LOG_FILE_NAME)) {
        disk->destroy_file(LOG_FILE_NAME);
    }
    disk->create_file(LOG_FILE_NAME);

    const size_t bytes = static_cast<size_t>(DiskManager::WAL_SEGMENT_SIZE) + 257;
    std::vector<char> expected(bytes);
    for (size_t i = 0; i < expected.size(); ++i) {
        expected[i] = static_cast<char>((i * 131U + 17U) & 0xffU);
    }
    disk->write_log(expected.data(), expected.size(), 0);
    disk->sync_log();
    EXPECT_EQ(disk->get_log_end_lsn(), static_cast<lsn_t>(expected.size()));

    const lsn_t read_start = DiskManager::WAL_SEGMENT_SIZE - 97;
    std::vector<char> actual(320);
    ASSERT_EQ(disk->read_log(actual.data(), actual.size(), read_start), actual.size());
    EXPECT_EQ(std::memcmp(actual.data(), expected.data() + read_start, actual.size()), 0);

    const lsn_t repaired_end = DiskManager::WAL_SEGMENT_SIZE + 31;
    disk->truncate_log_tail(repaired_end);
    disk->sync_log();
    EXPECT_EQ(disk->get_log_end_lsn(), repaired_end);
    EXPECT_EQ(disk->read_log(actual.data(), actual.size(), repaired_end), 0U);

    disk->truncate_log();
    disk->sync_log();
    const int log_fd = disk->GetLogFd();
    ASSERT_GE(log_fd, 0);
    disk->close_file(log_fd);
    disk->destroy_file(LOG_FILE_NAME);
}

TEST_F(BufferPoolManagerTest, SegmentedWalReclaimsOnlyCompleteHistoricalSegments) {
    auto *disk = BufferPoolManagerTest::disk_manager_.get();
    if (disk->is_file(LOG_FILE_NAME)) {
        disk->destroy_file(LOG_FILE_NAME);
    }
    disk->create_file(LOG_FILE_NAME);

    const size_t bytes = static_cast<size_t>(2 * DiskManager::WAL_SEGMENT_SIZE) + 257;
    std::vector<char> expected(bytes, 'w');
    disk->write_log(expected.data(), expected.size(), 0);
    disk->sync_log();
    disk->reclaim_log_segments_before(2 * DiskManager::WAL_SEGMENT_SIZE);
    disk->reclaim_log_segments_before(2 * DiskManager::WAL_SEGMENT_SIZE);
    disk->reclaim_log_segments_before(DiskManager::WAL_SEGMENT_SIZE);
    EXPECT_EQ(disk->get_first_log_lsn(), 2 * DiskManager::WAL_SEGMENT_SIZE);
    EXPECT_EQ(disk->get_log_end_lsn(), static_cast<lsn_t>(expected.size()));

    std::array<char, 32> tail{};
    ASSERT_EQ(disk->read_log(tail.data(), tail.size(), 2 * DiskManager::WAL_SEGMENT_SIZE),
              tail.size());
    EXPECT_TRUE(std::all_of(tail.begin(), tail.end(), [](char value) { return value == 'w'; }));
    EXPECT_THROW(disk->read_log(tail.data(), tail.size(), DiskManager::WAL_SEGMENT_SIZE),
                 FileNotFoundError);

    int log_fd = disk->GetLogFd();
    ASSERT_GE(log_fd, 0);
    disk->close_file(log_fd);
    EXPECT_EQ(disk->get_first_log_lsn(), 2 * DiskManager::WAL_SEGMENT_SIZE);
    EXPECT_EQ(disk->get_log_end_lsn(), static_cast<lsn_t>(expected.size()));
    EXPECT_THROW(disk->truncate_log_tail(DiskManager::WAL_SEGMENT_SIZE), InternalError);

    disk->truncate_log();
    disk->sync_log();
    EXPECT_EQ(disk->get_first_log_lsn(), 0);
    EXPECT_EQ(disk->get_log_end_lsn(), 0);
    log_fd = disk->GetLogFd();
    ASSERT_GE(log_fd, 0);
    disk->close_file(log_fd);
    EXPECT_EQ(disk->get_first_log_lsn(), 0);
    EXPECT_EQ(disk->get_log_end_lsn(), 0);
    disk->destroy_file(LOG_FILE_NAME);
}

TEST_F(BufferPoolManagerTest, PageLsnIsMonotonicAndDirtyPageTableKeepsEarliestLsn) {
    BufferPoolManager bpm(2, BufferPoolManagerTest::disk_manager_.get());
    PageId page_id{BufferPoolManagerTest::fd_, INVALID_PAGE_ID};
    Page *page = bpm.new_page(&page_id);
    ASSERT_NE(page, nullptr);

    ASSERT_TRUE(bpm.finalize_page_write_fast(page, page_id, 100, true));
    auto dirty_pages = bpm.snapshot_dirty_page_table();
    ASSERT_EQ(dirty_pages.size(), 1U);
    EXPECT_EQ(dirty_pages.at(page_id), 100);

    page = bpm.fetch_page(page_id);
    ASSERT_NE(page, nullptr);
    ASSERT_TRUE(bpm.finalize_page_write_fast(page, page_id, 50, true));
    EXPECT_EQ(page->get_page_lsn(), 100);
    dirty_pages = bpm.snapshot_dirty_page_table();
    ASSERT_EQ(dirty_pages.size(), 1U);
    EXPECT_EQ(dirty_pages.at(page_id), 50);

    ASSERT_TRUE(bpm.flush_page(page_id));
    EXPECT_TRUE(bpm.snapshot_dirty_page_table().empty());

    // Ordinary dirty unpins (including derived index pages) are intentionally
    // outside the ARIES heap DPT because they do not carry a WAL record LSN.
    PageId derived_page_id{BufferPoolManagerTest::fd_, INVALID_PAGE_ID};
    Page *derived_page = bpm.new_page(&derived_page_id);
    ASSERT_NE(derived_page, nullptr);
    ASSERT_TRUE(bpm.unpin_page(derived_page_id, true));
    EXPECT_TRUE(bpm.snapshot_dirty_page_table().empty());
}

TEST_F(BufferPoolManagerTest, DerivedIndexPageDoesNotInterpretHeaderAsWalLsn) {
    auto *disk = BufferPoolManagerTest::disk_manager_.get();
    if (disk->is_file(LOG_FILE_NAME)) {
        disk->destroy_file(LOG_FILE_NAME);
    }
    disk->create_file(LOG_FILE_NAME);

    auto log_manager = std::make_unique<LogManager>(disk);
    BufferPoolManager bpm(2, disk);
    bpm.set_log_manager(log_manager.get());
    bpm.set_fd_page_wal_policy(BufferPoolManagerTest::fd_, PageWalPolicy::DerivedIndex);

    PageId page_id{BufferPoolManagerTest::fd_, INVALID_PAGE_ID};
    Page *page = bpm.new_page(&page_id);
    ASSERT_NE(page, nullptr);
    const page_id_t next_free_page = -1;
    const page_id_t parent_page = 7;
    std::memcpy(page->get_data(), &next_free_page, sizeof(next_free_page));
    std::memcpy(page->get_data() + sizeof(next_free_page), &parent_page, sizeof(parent_page));
    const lsn_t fake_lsn = page->get_page_lsn();
    ASSERT_GT(fake_lsn, log_manager->get_persist_lsn());
    ASSERT_TRUE(bpm.unpin_page(page_id, true));

    size_t next_frame = 0;
    bool pass_complete = false;
    EXPECT_EQ(bpm.flush_unpinned_pages_batch(2, 2, &next_frame, &pass_complete, false), 0U);
    EXPECT_TRUE(page->is_dirty());
    next_frame = 0;
    pass_complete = false;
    EXPECT_EQ(bpm.flush_unpinned_pages_batch(2, 2, &next_frame, &pass_complete), 1U);
    EXPECT_FALSE(page->is_dirty());

    std::array<char, PAGE_SIZE> persisted{};
    disk->read_page(BufferPoolManagerTest::fd_, page_id.page_no, persisted.data(), PAGE_SIZE);
    EXPECT_EQ(rmdb::load_unaligned<page_id_t>(persisted.data()), next_free_page);
    EXPECT_EQ(rmdb::load_unaligned<page_id_t>(persisted.data() + sizeof(next_free_page)),
              parent_page);
}

TEST_F(BufferPoolManagerTest, FlushWaitsForPinnedWriter) {
    BufferPoolManager bpm(2, BufferPoolManagerTest::disk_manager_.get());
    PageId page_id{BufferPoolManagerTest::fd_, INVALID_PAGE_ID};
    Page *page = bpm.new_page(&page_id);
    ASSERT_NE(page, nullptr);
    std::memset(page->get_data(), 'p', PAGE_SIZE);
    BufferPoolManager::mark_dirty(page);

    EXPECT_FALSE(bpm.flush_page(page_id));
    EXPECT_TRUE(page->is_dirty());
    ASSERT_TRUE(bpm.unpin_page(page_id, false));
    EXPECT_TRUE(bpm.flush_page(page_id));
    EXPECT_FALSE(page->is_dirty());
}

TEST_F(BufferPoolManagerTest, PageGuardsKeepPinAndLatchLifetimeTogether) {
    auto disk_manager = BufferPoolManagerTest::disk_manager_.get();
    BufferPoolManager bpm(4, disk_manager);
    const int fd = BufferPoolManagerTest::fd_;
    PageId page_id{fd, INVALID_PAGE_ID};
    Page *page = bpm.new_page(&page_id);
    ASSERT_NE(page, nullptr);
    std::strcpy(page->get_data(), "guarded");
    ASSERT_TRUE(bpm.unpin_page(page_id, true));

    {
        ReadPageGuard guard = bpm.fetch_page_read(page_id);
        ASSERT_TRUE(guard);
        EXPECT_STREQ(guard.get_data(), "guarded");
        EXPECT_EQ(page_id, guard.page_id());
    }

    {
        WritePageGuard guard = bpm.fetch_page_write(page_id);
        ASSERT_TRUE(guard);
        std::strcpy(guard.get_data(), "updated");
        guard.MarkDirty();
    }
    Page *wal_pinned_page = nullptr;
    {
        WritePageGuard guard = bpm.fetch_page_write(page_id);
        ASSERT_TRUE(guard);
        std::strcpy(guard.get_data(), "wal-pinned");
        guard.MarkDirty();
        wal_pinned_page = guard.ReleaseLatchKeepPinned();
    }
    ASSERT_NE(wal_pinned_page, nullptr);
    {
        ReadPageGuard concurrent_reader = bpm.fetch_page_read(page_id);
        ASSERT_TRUE(concurrent_reader);
        EXPECT_STREQ(concurrent_reader.get_data(), "wal-pinned");
    }
    ASSERT_TRUE(bpm.unpin_page_fast(wal_pinned_page, page_id, false));
    ReadPageGuard verify = bpm.fetch_page_read(page_id);
    ASSERT_TRUE(verify);
    EXPECT_STREQ(verify.get_data(), "wal-pinned");
}

TEST(VisibleTupleRefTest, OwnedViewCarriesRawTupleLifetime) {
    auto record = std::make_unique<RmRecord>(4);
    std::memcpy(record->data, "data", 4);
    VisibleTupleRef ref = VisibleTupleRef::Owned(std::move(record));
    ASSERT_TRUE(ref);
    EXPECT_TRUE(ref.owned());
    EXPECT_EQ(ref.size(), 4);
    auto tuple_ref = AbstractExecutor::TupleViewRef::Visible(std::move(ref));
    ASSERT_TRUE(tuple_ref);
    EXPECT_TRUE(tuple_ref.owns_visible_lease());
    EXPECT_EQ(tuple_ref->raw_data[0], 'd');
    EXPECT_EQ(tuple_ref->raw_size, 4);
}

TEST(VisibleTupleRefTest, IndexKeyBorrowedUsesCurrentScanLifetime) {
    const char key[] = "abc";
    auto view = VisibleTupleRef::IndexKeyBorrowed(key, 3);
    ASSERT_TRUE(view);
    EXPECT_TRUE(view.index_key());
    EXPECT_TRUE(view.borrowed());
    EXPECT_EQ(std::memcmp(view.data(), key, 3), 0);
}

TEST(LogRecordBorrowedImageTest, SerializesDmlImagesWithoutConstructorCopies) {
    RmRecord old_record(4);
    RmRecord new_record(4);
    std::memcpy(old_record.data, "old!", 4);
    std::memcpy(new_record.data, "new!", 4);
    Rid rid{7, 3};
    const std::string table_name = "borrowed_log";

    InsertLogRecord insert(11, new_record, rid, table_name);
    EXPECT_EQ(insert.insert_value_.data, nullptr);
    std::vector<char> insert_bytes(static_cast<size_t>(insert.log_tot_len_));
    insert.serialize(insert_bytes.data());
    InsertLogRecord decoded_insert;
    decoded_insert.deserialize(insert_bytes.data());
    EXPECT_EQ(std::memcmp(decoded_insert.insert_value_.data, new_record.data, 4), 0);

    DeleteLogRecord deletion(12, old_record, rid, table_name);
    EXPECT_EQ(deletion.old_record_.data, nullptr);
    std::vector<char> delete_bytes(static_cast<size_t>(deletion.log_tot_len_));
    deletion.serialize(delete_bytes.data());
    DeleteLogRecord decoded_delete;
    decoded_delete.deserialize(delete_bytes.data());
    EXPECT_EQ(std::memcmp(decoded_delete.old_record_.data, old_record.data, 4), 0);

    ColMeta col_meta;
    col_meta.name = "payload";
    col_meta.type = TYPE_INT;
    col_meta.len = 4;
    col_meta.offset = 0;
    std::vector<ColMeta> cols{col_meta};
    UpdateLogRecord update(13, old_record, new_record, rid, table_name, &cols);
    EXPECT_EQ(update.deltas_.size(), 1u);
    EXPECT_TRUE(update.deltas_[0].borrowed);
    EXPECT_TRUE(update.deltas_[0].old_value.empty());
    EXPECT_TRUE(update.deltas_[0].new_value.empty());
    EXPECT_EQ(update.deltas_[0].OldValue().data(), old_record.data);
    EXPECT_EQ(update.deltas_[0].NewValue().data(), new_record.data);
    std::vector<char> update_bytes(static_cast<size_t>(update.log_tot_len_));
    update.serialize(update_bytes.data());
    UpdateLogRecord decoded_update;
    decoded_update.deserialize(update_bytes.data());
    EXPECT_EQ(decoded_update.deltas_.size(), 1u);
    EXPECT_EQ(decoded_update.deltas_[0].col_index, 0u);
    EXPECT_FALSE(decoded_update.deltas_[0].borrowed);
    EXPECT_EQ(std::memcmp(decoded_update.deltas_[0].OldValue().data(), old_record.data, 4), 0);
    EXPECT_EQ(std::memcmp(decoded_update.deltas_[0].NewValue().data(), new_record.data, 4), 0);
    EXPECT_EQ(decoded_update.rid_, rid);
    EXPECT_EQ(decoded_update.table_hash_, update.table_hash_);

    UpdateLogRecord noop_update(14, old_record, old_record, rid, table_name, &cols);
    EXPECT_TRUE(noop_update.deltas_.empty());
    std::vector<char> noop_bytes(static_cast<size_t>(noop_update.log_tot_len_));
    noop_update.serialize(noop_bytes.data());
    UpdateLogRecord decoded_noop;
    decoded_noop.deserialize(noop_bytes.data());
    EXPECT_TRUE(decoded_noop.deltas_.empty());
}

TEST(AriesLogRecordTest, PreservesLegacyTypesAndRoundTripsClr) {
    EXPECT_EQ(static_cast<rmdb::i32>(LogType::kUpdate), 0);
    EXPECT_EQ(static_cast<rmdb::i32>(LogType::kInsert), 1);
    EXPECT_EQ(static_cast<rmdb::i32>(LogType::kDelete), 2);
    EXPECT_EQ(static_cast<rmdb::i32>(LogType::kBegin), 3);
    EXPECT_EQ(static_cast<rmdb::i32>(LogType::kCommit), 4);
    EXPECT_EQ(static_cast<rmdb::i32>(LogType::kAbort), 5);
    EXPECT_EQ(static_cast<rmdb::i32>(LogType::kCheckpoint), 6);

    RmRecord old_record(4);
    RmRecord new_record(4);
    std::memcpy(old_record.data, "old!", 4);
    std::memcpy(new_record.data, "new!", 4);
    ColMeta column;
    column.len = 4;
    column.offset = 0;
    std::vector<ColMeta> columns{column};
    UpdateLogRecord update(17, old_record, new_record, Rid{7, 3}, "aries_log", &columns);
    update.lsn_ = 64;
    update.prev_lsn_ = 12;

    ClrLogRecord clr(17, update.prev_lsn_, update);
    clr.lsn_ = 256;
    clr.prev_lsn_ = update.lsn_;
    std::vector<char> bytes(clr.log_tot_len_);
    clr.serialize(bytes.data());

    auto parsed = ParseLogRecordForRecovery(bytes.data(), bytes.size(), clr.lsn_);
    ASSERT_NE(parsed, nullptr);
    auto *decoded_clr = dynamic_cast<ClrLogRecord *>(parsed.get());
    ASSERT_NE(decoded_clr, nullptr);
    EXPECT_EQ(decoded_clr->undo_next_lsn_, 12);
    auto compensated = ParseLogRecordForRecovery(decoded_clr->compensated_log_.data(),
                                                  decoded_clr->compensated_log_.size(), update.lsn_);
    ASSERT_NE(compensated, nullptr);
    auto *decoded_update = dynamic_cast<UpdateLogRecord *>(compensated.get());
    ASSERT_NE(decoded_update, nullptr);
    EXPECT_EQ(decoded_update->rid_, update.rid_);
    EXPECT_EQ(decoded_update->deltas_.size(), 1U);

    lsn_t invalid_undo_next = clr.lsn_;
    std::memcpy(bytes.data() + OFFSET_LOG_DATA, &invalid_undo_next, sizeof(invalid_undo_next));
    EXPECT_EQ(ParseLogRecordForRecovery(bytes.data(), bytes.size(), clr.lsn_), nullptr);
}

TEST(AriesLogRecordTest, RoundTripsFuzzyCheckpointState) {
    std::vector<CheckpointTxnEntry> txn_table{
        CheckpointTxnEntry{11, RecoveryTxnStatus::kRunning, 48},
        CheckpointTxnEntry{12, RecoveryTxnStatus::kAborting, 72},
    };
    std::vector<CheckpointDirtyPageInfo> dirty_pages{
        CheckpointDirtyPageInfo{HashTableName("warehouse"), 3, 64},
        CheckpointDirtyPageInfo{HashTableName("orders"), 9, 80},
    };
    EndCheckpointLogRecord checkpoint(400, 42, std::move(txn_table), std::move(dirty_pages));
    checkpoint.lsn_ = 512;
    std::vector<char> bytes(checkpoint.log_tot_len_);
    checkpoint.serialize(bytes.data());

    auto parsed = ParseLogRecordForRecovery(bytes.data(), bytes.size(), checkpoint.lsn_);
    ASSERT_NE(parsed, nullptr);
    auto *decoded = dynamic_cast<EndCheckpointLogRecord *>(parsed.get());
    ASSERT_NE(decoded, nullptr);
    EXPECT_EQ(decoded->begin_lsn_, 400);
    EXPECT_EQ(decoded->next_txn_id_, 42);
    ASSERT_EQ(decoded->txn_table_.size(), 2U);
    EXPECT_EQ(decoded->txn_table_[1].status_, RecoveryTxnStatus::kAborting);
    ASSERT_EQ(decoded->dirty_pages_.size(), 2U);
    EXPECT_EQ(decoded->dirty_pages_[1].page_no_, 9);
    EXPECT_EQ(decoded->dirty_pages_[1].rec_lsn_, 80);

    bytes.pop_back();
    EXPECT_EQ(ParseLogRecordForRecovery(bytes.data(), bytes.size(), checkpoint.lsn_), nullptr);
}

TEST(AriesLogRecordTest, ReconcilesTerminalWalAheadOfTxnState) {
    RecoveryTxnStatus status = RecoveryTxnStatus::kRunning;
    CommitLogRecord commit(7);
    EXPECT_TRUE(ReconcileCheckpointTxnTail(commit, &status));
    EXPECT_EQ(status, RecoveryTxnStatus::kCommitting);

    status = RecoveryTxnStatus::kRunning;
    AbortLogRecord abort(8);
    EXPECT_TRUE(ReconcileCheckpointTxnTail(abort, &status));
    EXPECT_EQ(status, RecoveryTxnStatus::kAborting);

    status = RecoveryTxnStatus::kRunning;
    EndLogRecord end(9);
    EXPECT_FALSE(ReconcileCheckpointTxnTail(end, &status));
}

TEST(AriesLogRecordTest, ValidatesCheckpointMasterChecksum) {
    CheckpointMasterRecord expected{9, 4096, 2048};
    auto bytes = rmdb::checkpoint_master::Encode(expected);
    CheckpointMasterRecord decoded;
    ASSERT_TRUE(rmdb::checkpoint_master::Decode(bytes.data(), bytes.size(), &decoded));
    EXPECT_EQ(decoded.generation, expected.generation);
    EXPECT_EQ(decoded.end_checkpoint_lsn, expected.end_checkpoint_lsn);
    EXPECT_EQ(decoded.first_retained_lsn, expected.first_retained_lsn);

    bytes[24] ^= 0x1;
    EXPECT_FALSE(rmdb::checkpoint_master::Decode(bytes.data(), bytes.size(), &decoded));
    EXPECT_FALSE(rmdb::checkpoint_master::Decode(bytes.data(), bytes.size() - 1, &decoded));

    std::array<char, rmdb::checkpoint_master::kLegacyRecordSize> legacy{};
    const rmdb::u64 magic = rmdb::checkpoint_master::kMagic;
    const rmdb::u32 version = rmdb::checkpoint_master::kLegacyVersion;
    const rmdb::u32 size = rmdb::checkpoint_master::kLegacyRecordSize;
    const rmdb::u64 generation = 3;
    const lsn_t end_lsn = 1024;
    std::memcpy(legacy.data(), &magic, sizeof(magic));
    std::memcpy(legacy.data() + 8, &version, sizeof(version));
    std::memcpy(legacy.data() + 12, &size, sizeof(size));
    std::memcpy(legacy.data() + 16, &generation, sizeof(generation));
    std::memcpy(legacy.data() + 24, &end_lsn, sizeof(end_lsn));
    const rmdb::u64 legacy_checksum = rmdb::checkpoint_master::Checksum(
        legacy.data(), rmdb::checkpoint_master::kLegacyChecksumOffset);
    std::memcpy(legacy.data() + rmdb::checkpoint_master::kLegacyChecksumOffset,
                &legacy_checksum, sizeof(legacy_checksum));
    ASSERT_TRUE(rmdb::checkpoint_master::Decode(legacy.data(), legacy.size(), &decoded));
    EXPECT_EQ(decoded.first_retained_lsn, 0);
}

TEST_F(BufferPoolManagerTest, BorrowedTupleKeepsPageAndRecordReadLease) {
    BufferPoolManager bpm(4, BufferPoolManagerTest::disk_manager_.get());
    PageId page_id{BufferPoolManagerTest::fd_, INVALID_PAGE_ID};
    Page *page = bpm.new_page(&page_id);
    ASSERT_NE(page, nullptr);
    std::memcpy(page->get_data(), "view", 4);
    ASSERT_TRUE(bpm.unpin_page(page_id, true));

    std::shared_mutex record_mutex;
    auto record_guard = std::shared_lock<std::shared_mutex>(record_mutex);
    auto page_guard = bpm.fetch_page_read(page_id);
    const char *view = page_guard.get_data();
    VisibleTupleRef ref = VisibleTupleRef::Borrowed(std::move(page_guard), std::move(record_guard), view, 4);
    ASSERT_TRUE(ref);
    EXPECT_TRUE(ref.borrowed());
    EXPECT_FALSE(record_mutex.try_lock());
    ref = {};
    ASSERT_TRUE(record_mutex.try_lock());
    record_mutex.unlock();
}

TEST(RmRecordTest, OwnsCopiesMovesAndBorrowsRecordData) {
    char source[] = {'r', 'm', 'd', 'b'};

    RmRecord borrowed = RmRecord::borrow(source, sizeof(source));
    EXPECT_FALSE(borrowed.owns_data());
    EXPECT_EQ(borrowed.data, source);

    const char replacement[] = {'s', 'a', 'f', 'e'};
    borrowed.SetData(replacement);
    EXPECT_TRUE(borrowed.owns_data());
    EXPECT_NE(borrowed.data, source);
    EXPECT_EQ(0, std::memcmp(borrowed.data, replacement, sizeof(replacement)));
    EXPECT_EQ(0, std::memcmp(source, "rmdb", sizeof(source)));

    RmRecord copied = borrowed;
    EXPECT_TRUE(copied.owns_data());
    EXPECT_NE(copied.data, source);
    EXPECT_EQ(0, std::memcmp(copied.data, replacement, sizeof(replacement)));

    char* copied_data = copied.data;
    RmRecord moved = std::move(copied);
    EXPECT_EQ(moved.data, copied_data);
    EXPECT_EQ(copied.data, nullptr);
    EXPECT_EQ(copied.size, 0);

    borrowed.Resize(8);
    EXPECT_TRUE(borrowed.owns_data());
    EXPECT_NE(borrowed.data, source);
    EXPECT_EQ(borrowed.size, 8);

    RmRecord overlapping(6, "abcdef");
    overlapping.ResizeAndCopy(overlapping.data + 1, 5);
    EXPECT_EQ(overlapping.size, 5);
    EXPECT_EQ(0, std::memcmp(overlapping.data, "bcdef", 5));

    RmRecord serialized(8);
    const int payload_size = 4;
    std::memcpy(serialized.data, &payload_size, sizeof(payload_size));
    std::memcpy(serialized.data + sizeof(payload_size), "safe", payload_size);
    serialized.Deserialize(std::span<const char>(serialized.data, serialized.size));
    EXPECT_EQ(serialized.size, payload_size);
    EXPECT_EQ(0, std::memcmp(serialized.data, "safe", payload_size));
}

TEST(RmRecordTest, RejectsInvalidSerializedSizes) {
    RmRecord record;
    int invalid_size = RM_MAX_RECORD_SIZE + 1;
    char serialized_size[sizeof(invalid_size)];
    std::memcpy(serialized_size, &invalid_size, sizeof(invalid_size));

    EXPECT_THROW(record.Deserialize(serialized_size), InternalError);

    int truncated_size = 4;
    std::memcpy(serialized_size, &truncated_size, sizeof(truncated_size));
    EXPECT_THROW(record.Deserialize(serialized_size), InternalError);
    EXPECT_THROW((void)RmRecord(-1), InternalError);
}

/** 注意：每个测试点只测试了单个文件！
 * 对于每个测试点，先创建和进入目录TEST_DB_NAME
 * 然后在此目录下创建和打开文件TEST_FILE_NAME_CCUR，记录其文件描述符fd */

// Add by jiawen
class BufferPoolManagerConcurrencyTest : public ::testing::Test {
   public:
    std::unique_ptr<DiskManager> disk_manager_;
    int fd_ = -1;  // 此文件描述符为disk_manager_->open_file的返回值

   public:
    // This function is called before every test.
    void SetUp() override {
        ::testing::Test::SetUp();
        // For each test, we create a new DiskManager
        disk_manager_ = std::make_unique<DiskManager>();
        // 如果测试目录不存在，则先创建测试目录
        if (!disk_manager_->is_dir(TEST_DB_NAME)) {
            disk_manager_->create_dir(TEST_DB_NAME);
        }
        assert(disk_manager_->is_dir(TEST_DB_NAME));
        // 进入测试目录
        if (chdir(TEST_DB_NAME.c_str()) < 0) {
            throw UnixError();
        }
        // 如果测试文件存在，则先删除原文件（最后留下来的文件存的是最后一个测试点的数据）
        if (disk_manager_->is_file(TEST_FILE_NAME_CCUR)) {
            disk_manager_->destroy_file(TEST_FILE_NAME_CCUR);
        }
        // 创建测试文件
        disk_manager_->create_file(TEST_FILE_NAME_CCUR);
        assert(disk_manager_->is_file(TEST_FILE_NAME_CCUR));
        // 打开测试文件
        fd_ = disk_manager_->open_file(TEST_FILE_NAME_CCUR);
        assert(fd_ != -1);
    }

    // This function is called after every test.
    void TearDown() override {
        disk_manager_->close_file(fd_);
        // disk_manager_->destroy_file(TEST_FILE_NAME_CCUR);  // you can choose to delete the file

        // 返回上一层目录
        if (chdir("..") < 0) {
            throw UnixError();
        }
        assert(disk_manager_->is_dir(TEST_DB_NAME));
    };
};

TEST_F(BufferPoolManagerConcurrencyTest, ConcurrencyTest) {
    const int num_threads = 5;
    const int num_runs = 50;

    // get fd
    int fd = BufferPoolManagerConcurrencyTest::fd_;

    for (int run = 0; run < num_runs; run++) {
        // create BufferPoolManager
        auto disk_manager = BufferPoolManagerConcurrencyTest::disk_manager_.get();
        std::shared_ptr<BufferPoolManager> bpm{new BufferPoolManager(50, disk_manager)};

        std::vector<std::thread> threads;
        for (int tid = 0; tid < num_threads; tid++) {
            threads.push_back(std::thread([&bpm, fd]() {  // NOLINT
                PageId temp_page_id = {.fd = fd, .page_no = INVALID_PAGE_ID};
                std::vector<PageId> page_ids;
                for (int i = 0; i < 10; i++) {
                    auto new_page = bpm->new_page(&temp_page_id);
                    EXPECT_NE(nullptr, new_page);
                    ASSERT_NE(nullptr, new_page);
                    strcpy(new_page->get_data(), std::to_string(temp_page_id.page_no).c_str());  // NOLINT
                    page_ids.push_back(temp_page_id);
                }
                for (int i = 0; i < 10; i++) {
                    EXPECT_EQ(1, bpm->unpin_page(page_ids[i], true));
                }
                for (int j = 0; j < 10; j++) {
                    auto page = bpm->fetch_page(page_ids[j]);
                    EXPECT_NE(nullptr, page);
                    ASSERT_NE(nullptr, page);
                    EXPECT_EQ(0, std::strcmp(std::to_string(page_ids[j].page_no).c_str(), (page->get_data())));
                    EXPECT_EQ(1, bpm->unpin_page(page_ids[j], true));
                }
                for (int j = 0; j < 10; j++) {
                    EXPECT_EQ(1, bpm->delete_page(page_ids[j]));
                }
                bpm->flush_all_pages(fd);  // add this test by jiawen
            }));
        }  // end loop tid=[0,num_threads)

        for (int i = 0; i < num_threads; i++) {
            threads[i].join();
        }
    }  // end loop run=[0,num_runs)
}

// TODO: fix detected memory leaks found by Google Test
TEST(StorageTest, SimpleTest) {
    srand((unsigned)time(nullptr));

    /** Test disk_manager */
    std::vector<std::string> filenames(MAX_FILES);  // MAX_FILES=32
    std::unordered_map<int, std::string> fd2name;
    for (size_t i = 0; i < filenames.size(); i++) {
        auto &filename = filenames[i];
        filename = std::to_string(i) + ".txt";
        if (disk_manager->is_file(filename)) {
            disk_manager->destroy_file(filename);
        }
        // open without create
        try {
            disk_manager->open_file(filename);
            assert(false);
        } catch (const FileNotFoundError &e) {
        }

        disk_manager->create_file(filename);
        assert(disk_manager->is_file(filename));
        try {
            disk_manager->create_file(filename);
            assert(false);
        } catch (const FileExistsError &e) {
        }

        // open file
        int fd = disk_manager->open_file(filename);
        char *tmp = new char[PAGE_SIZE * MAX_PAGES];  // TODO: fix error in detected memory leaks

        mock[fd] = tmp;
        fd2name[fd] = filename;

        disk_manager->set_fd2pageno(fd, 0);  // diskmanager在fd对应的文件中从0开始分配page_no
    }

    /** Test buffer_pool_manager*/
    int num_pages = 0;
    char init_buf[PAGE_SIZE];
    for (auto &fh : mock) {
        int fd = fh.first;
        for (page_id_t i = 0; i < MAX_PAGES; i++) {
            rand_buf(PAGE_SIZE, init_buf);  // 将init_buf填充PAGE_SIZE个字节的随机数据

            PageId tmp_page_id = {.fd = fd, .page_no = INVALID_PAGE_ID};
            Page *page = buffer_pool_manager->new_page(&tmp_page_id);
            int page_no = tmp_page_id.page_no;
            assert(page_no != INVALID_PAGE_ID);
            assert(page_no == i);

            memcpy(page->get_data(), init_buf, PAGE_SIZE);
            buffer_pool_manager->unpin_page(PageId{fd, page_no}, true);

            char *mock_buf = mock_get_page(fd, page_no);  // &mock[fd][page_no * PAGE_SIZE]
            memcpy(mock_buf, init_buf, PAGE_SIZE);

            num_pages++;

            check_cache(fd, page_no);  // 调用了fetch_page, unpin_page
        }
    }
    check_cache_all();

    assert(num_pages == TEST_BUFFER_POOL_SIZE);

    /** Test flush_all_pages() */
    // Flush and test disk
    for (auto &entry : fd2name) {
        int fd = entry.first;
        buffer_pool_manager->flush_all_pages(fd);
        for (int page_no = 0; page_no < MAX_PAGES; page_no++) {
            check_disk(fd, page_no);
        }
    }
    check_disk_all();

    for (int r = 0; r < 10000; r++) {
        int fd = rand_fd();
        int page_no = rand() % MAX_PAGES;
        // fetch page
        Page *page = buffer_pool_manager->fetch_page(PageId{fd, page_no});
        char *mock_buf = mock_get_page(fd, page_no);
        assert(memcmp(page->get_data(), mock_buf, PAGE_SIZE) == 0);

        // modify
        rand_buf(PAGE_SIZE, init_buf);
        memcpy(page->get_data(), init_buf, PAGE_SIZE);
        memcpy(mock_buf, init_buf, PAGE_SIZE);

        buffer_pool_manager->unpin_page(page->get_page_id(), true);
        // BufferPool::mark_dirty(page);

        // flush
        if (rand() % 10 == 0) {
            buffer_pool_manager->flush_page(page->get_page_id());
            check_disk(fd, page_no);
        }
        // flush entire file
        if (rand() % 100 == 0) {
            buffer_pool_manager->flush_all_pages(fd);
        }
        // re-open file
        if (rand() % 100 == 0) {
            disk_manager->close_file(fd);
            auto filename = fd2name[fd];
            char *buf = mock[fd];
            fd2name.erase(fd);
            mock.erase(fd);
            int new_fd = disk_manager->open_file(filename);
            mock[new_fd] = buf;
            fd2name[new_fd] = filename;
        }
        // assert equal in cache
        check_cache(fd, page_no);
    }
    check_cache_all();

    for (auto &entry : fd2name) {
        int fd = entry.first;
        buffer_pool_manager->flush_all_pages(fd);
        for (int page_no = 0; page_no < MAX_PAGES; page_no++) {
            check_disk(fd, page_no);
        }
    }
    check_disk_all();

    // close and destroy files
    for (auto &entry : fd2name) {
        int fd = entry.first;
        auto &filename = entry.second;
        disk_manager->close_file(fd);
        disk_manager->destroy_file(filename);
        try {
            disk_manager->destroy_file(filename);
            assert(false);
        } catch (const FileNotFoundError &e) {
        }
    }
}

TEST(RecordManagerTest, SimpleTest) {
    srand((unsigned)time(nullptr));

    // 创建RmManager类的对象rm_manager
    auto disk_manager = std::make_unique<DiskManager>();
    auto buffer_pool_manager = std::make_unique<BufferPoolManager>(BUFFER_POOL_SIZE, disk_manager.get());
    auto rm_manager = std::make_unique<RmManager>(disk_manager.get(), buffer_pool_manager.get());

    std::unordered_map<Rid, std::string, rid_hash_t, rid_equal_t> mock;

    std::string filename = "abc.txt";

    int record_size = 4 + rand() % 256;  // 元组大小随便设置，只要不超过RM_MAX_RECORD_SIZE
    // test files
    {
        // 删除残留的同名文件
        if (disk_manager->is_file(filename)) {
            disk_manager->destroy_file(filename);
        }
        // 将file header写入到磁盘中的filename文件
        rm_manager->create_file(filename, record_size);
        // 将磁盘中的filename文件读出到内存中的file handle的file header
        std::unique_ptr<RmFileHandle> file_handle = rm_manager->open_file(filename);
        // 检查filename文件在内存中的file header的参数
        assert(file_handle->file_hdr_.record_size == record_size);
        assert(file_handle->file_hdr_.first_free_page_no == RM_NO_PAGE);
        assert(file_handle->file_hdr_.num_pages == 1);

        int max_bytes = file_handle->file_hdr_.record_size * file_handle->file_hdr_.num_records_per_page +
                        file_handle->file_hdr_.bitmap_size + (int)sizeof(RmPageHdr);
        assert(max_bytes <= PAGE_SIZE);
        int rand_val = rand();
        file_handle->file_hdr_.num_pages = rand_val;
        rm_manager->close_file(file_handle.get());

        // reopen file
        file_handle = rm_manager->open_file(filename);
        assert(file_handle->file_hdr_.num_pages == rand_val);
        rm_manager->close_file(file_handle.get());
        rm_manager->destroy_file(filename);
    }
    // test pages
    rm_manager->create_file(filename, record_size);
    auto file_handle = rm_manager->open_file(filename);

    char write_buf[PAGE_SIZE];
    size_t add_cnt = 0;
    size_t upd_cnt = 0;
    size_t del_cnt = 0;
    for (int round = 0; round < 1000; round++) {
        double insert_prob = 1. - mock.size() / 250.;
        double dice = rand() * 1. / RAND_MAX;
        if (mock.empty() || dice < insert_prob) {
            rand_buf(file_handle->file_hdr_.record_size, write_buf);
            Rid rid = file_handle->insert_record(write_buf, nullptr);
            mock[rid] = std::string((char *)write_buf, file_handle->file_hdr_.record_size);
            add_cnt++;
            //            std::cout << "insert " << rid << '\n'; // operator<<(cout,rid)
        } else {
            // update or erase random rid
            int rid_idx = rand() % mock.size();
            auto it = mock.begin();
            for (int i = 0; i < rid_idx; i++) {
                it++;
            }
            auto rid = it->first;
            if (rand() % 2 == 0) {
                // update
                rand_buf(file_handle->file_hdr_.record_size, write_buf);
                file_handle->update_record(rid, write_buf, nullptr);
                mock[rid] = std::string((char *)write_buf, file_handle->file_hdr_.record_size);
                upd_cnt++;
                //                std::cout << "update " << rid << '\n';
            } else {
                // erase
                file_handle->delete_record(rid, nullptr);
                mock.erase(rid);
                del_cnt++;
                //                std::cout << "delete " << rid << '\n';
            }
        }
        // Randomly re-open file
        if (round % 50 == 0) {
            rm_manager->close_file(file_handle.get());
            file_handle = rm_manager->open_file(filename);
        }
        check_equal(file_handle.get(), mock);
    }
    assert(mock.size() == add_cnt - del_cnt);
    std::cout << "insert " << add_cnt << '\n' << "delete " << del_cnt << '\n' << "update " << upd_cnt << '\n';
    // clean up
    rm_manager->close_file(file_handle.get());
    rm_manager->destroy_file(filename);
}

TEST(RecordManagerTest, RecoveryInsertPreservesFreeList) {
    auto local_disk_manager = std::make_unique<DiskManager>();
    auto local_buffer_pool_manager = std::make_unique<BufferPoolManager>(64, local_disk_manager.get());
    auto local_rm_manager =
        std::make_unique<RmManager>(local_disk_manager.get(), local_buffer_pool_manager.get());
    const std::string filename = "recovery_free_list.txt";
    if (local_disk_manager->is_file(filename)) {
        local_disk_manager->destroy_file(filename);
    }

    local_rm_manager->create_file(filename, RM_MAX_RECORD_SIZE);
    auto file_handle = local_rm_manager->open_file(filename);
    std::vector<char> record(RM_MAX_RECORD_SIZE, 'x');
    const int records_per_page = file_handle->file_hdr_.num_records_per_page;
    std::vector<Rid> records;
    for (int i = 0; i < records_per_page * 3; ++i) {
        records.push_back(file_handle->insert_record(record.data(), nullptr));
    }

    Rid page1_hole = records[0];
    Rid page2_hole = records[records_per_page];
    Rid page3_hole = records[records_per_page * 2];
    file_handle->delete_record(page1_hole, nullptr);
    file_handle->delete_record(page2_hole, nullptr);
    file_handle->delete_record(page3_hole, nullptr);

    // Recovery can fill a non-head free page by RID. Keep its successor linked
    // until the normal allocator reaches and removes the now-full stale node.
    file_handle->insert_record(page2_hole, record.data());
    EXPECT_EQ(file_handle->insert_record(record.data(), nullptr).page_no, page3_hole.page_no);
    EXPECT_EQ(file_handle->insert_record(record.data(), nullptr).page_no, page1_hole.page_no);

    local_rm_manager->close_file(file_handle.get());
    local_rm_manager->destroy_file(filename);
}

TEST(RecordManagerTest, ConditionalDeleteSerializesValidationAndRidReuse) {
    auto local_disk_manager = std::make_unique<DiskManager>();
    auto local_buffer_pool_manager = std::make_unique<BufferPoolManager>(64, local_disk_manager.get());
    auto local_rm_manager =
        std::make_unique<RmManager>(local_disk_manager.get(), local_buffer_pool_manager.get());
    const std::string filename = "conditional_delete.txt";
    if (local_disk_manager->is_file(filename)) {
        local_disk_manager->destroy_file(filename);
    }

    local_rm_manager->create_file(filename, RM_MAX_RECORD_SIZE);
    auto file_handle = local_rm_manager->open_file(filename);
    std::vector<char> record(RM_MAX_RECORD_SIZE, 'd');
    const Rid rid = file_handle->insert_record(record.data(), nullptr);

    EXPECT_EQ(file_handle->delete_record_if(rid, [] { return false; }),
              RmFileHandle::ConditionalDeleteResult::CONDITION_FAILED);
    EXPECT_TRUE(file_handle->is_record(rid));

    std::promise<void> entered_promise;
    auto entered = entered_promise.get_future();
    std::promise<void> release_promise;
    auto release = release_promise.get_future().share();
    auto deleter = std::async(std::launch::async, [&] {
        return file_handle->delete_record_if(rid, [&] {
            entered_promise.set_value();
            release.wait();
            return true;
        });
    });
    entered.wait();

    auto reader = std::async(std::launch::async, [&] {
        RmRecord out;
        return file_handle->read_record(rid, &out, nullptr);
    });
    EXPECT_EQ(reader.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout);

    release_promise.set_value();
    EXPECT_EQ(deleter.get(), RmFileHandle::ConditionalDeleteResult::DELETED);
    EXPECT_FALSE(reader.get());
    EXPECT_EQ(file_handle->delete_record_if(rid, [] { return true; }),
              RmFileHandle::ConditionalDeleteResult::RECORD_MISSING);

    const Rid reused = file_handle->insert_record(record.data(), nullptr);
    EXPECT_EQ(reused, rid);
    local_rm_manager->close_file(file_handle.get());
    local_rm_manager->destroy_file(filename);
}

TEST(RecordManagerTest, RecoveryRebuildsFreeListAcrossScanChunks) {
    constexpr int kRecordPages = 514;
    constexpr std::array<page_id_t, 6> kFreePages = {1, 2, 511, 512, 513, 514};
    const std::string filename = "recovery_chunked_free_list.txt";

    auto local_disk_manager = std::make_unique<DiskManager>();
    auto local_buffer_pool_manager = std::make_unique<BufferPoolManager>(8, local_disk_manager.get());
    auto local_rm_manager =
        std::make_unique<RmManager>(local_disk_manager.get(), local_buffer_pool_manager.get());
    if (local_disk_manager->is_file(filename)) {
        local_disk_manager->destroy_file(filename);
    }
    local_rm_manager->create_file(filename, RM_MAX_RECORD_SIZE);

    int fd = local_disk_manager->open_file(filename);
    RmFileHdr file_hdr{};
    local_disk_manager->read_page(fd, RM_FILE_HDR_PAGE,
                                  reinterpret_cast<char *>(&file_hdr), sizeof(file_hdr));
    std::vector<char> pages(static_cast<size_t>(kRecordPages) * PAGE_SIZE, 0);
    for (int page_index = 0; page_index < kRecordPages; ++page_index) {
        char *page_hdr_data = pages.data() + static_cast<size_t>(page_index) * PAGE_SIZE +
                              Page::OFFSET_PAGE_HDR;
        const page_id_t page_no = page_index + RM_FIRST_RECORD_PAGE;
        const bool is_free = std::find(kFreePages.begin(), kFreePages.end(), page_no) !=
                             kFreePages.end();
        rmdb::store_unaligned<page_id_t>(page_hdr_data, RM_NO_PAGE);
        rmdb::store_unaligned<int>(page_hdr_data + offsetof(RmPageHdr, num_records),
                                   file_hdr.num_records_per_page - (is_free ? 1 : 0));
    }
    local_disk_manager->write_pages_batch(fd, RM_FIRST_RECORD_PAGE, pages.data(), kRecordPages);
    local_disk_manager->close_file(fd);

    auto file_handle = local_rm_manager->open_file(filename);
    file_handle->rebuild_file_hdr_from_disk();
    const RmFileHdr rebuilt_hdr = file_handle->get_file_hdr();
    EXPECT_EQ(rebuilt_hdr.num_pages, kRecordPages + RM_FIRST_RECORD_PAGE);
    EXPECT_EQ(rebuilt_hdr.first_free_page_no, kFreePages.front());
    for (size_t index = 0; index < kFreePages.size(); ++index) {
        const page_id_t expected_next =
            index + 1 < kFreePages.size() ? kFreePages[index + 1] : RM_NO_PAGE;
        page_id_t persisted_next = RM_NO_PAGE;
        const rmdb::i64 link_offset = static_cast<rmdb::i64>(kFreePages[index]) * PAGE_SIZE +
                                      Page::OFFSET_PAGE_HDR +
                                      offsetof(RmPageHdr, next_free_page_no);
        ASSERT_EQ(local_disk_manager->read_file_range(
                      file_handle->GetFd(), link_offset,
                      reinterpret_cast<char *>(&persisted_next), sizeof(persisted_next)),
                  sizeof(persisted_next));
        EXPECT_EQ(persisted_next, expected_next);
    }

    local_rm_manager->close_file(file_handle.get());
    local_rm_manager->destroy_file(filename);
}

TEST(RecordManagerTest, PendingInsertIsHiddenUntilPublished) {
    auto local_disk_manager = std::make_unique<DiskManager>();
    auto local_buffer_pool_manager = std::make_unique<BufferPoolManager>(64, local_disk_manager.get());
    auto local_rm_manager =
        std::make_unique<RmManager>(local_disk_manager.get(), local_buffer_pool_manager.get());
    const std::string filename = "pending_insert_visibility.txt";
    if (local_disk_manager->is_file(filename)) {
        local_disk_manager->destroy_file(filename);
    }

    local_rm_manager->create_file(filename, RM_MAX_RECORD_SIZE);
    auto file_handle = local_rm_manager->open_file(filename);
    std::vector<char> record(RM_MAX_RECORD_SIZE, 'p');
    const int records_per_page = file_handle->file_hdr_.num_records_per_page;
    for (int i = 0; i < records_per_page - 1; ++i) {
        file_handle->insert_record(record.data(), nullptr);
    }

    Rid pending = file_handle->insert_record(record.data(), nullptr, nullptr, false, nullptr, nullptr, true);
    EXPECT_FALSE(file_handle->is_record(pending));
    RmRecord read_buffer;
    EXPECT_FALSE(file_handle->read_record(pending, &read_buffer, nullptr));
    size_t visible_before_publish = 0;
    for (RmScan scan(file_handle.get()); !scan.is_end(); scan.next()) {
        ++visible_before_publish;
    }
    EXPECT_EQ(visible_before_publish, static_cast<size_t>(records_per_page - 1));

    file_handle->rollback_pending_insert(pending);
    EXPECT_FALSE(file_handle->is_record(pending));
    Rid reused = file_handle->insert_record(record.data(), nullptr);
    EXPECT_EQ(reused, pending);
    file_handle->delete_record(reused, nullptr);

    Rid published = file_handle->insert_record(record.data(), nullptr, nullptr, false, nullptr, nullptr, true);
    EXPECT_EQ(published, pending);
    {
        // Publishing metadata must not upgrade the file latch. A borrowed
        // clean tuple may still own a shared file lease on this thread.
        auto borrowed_guard = file_handle->acquire_shared_latch();
        file_handle->publish_pending_insert(published);
    }
    EXPECT_TRUE(file_handle->is_record(published));
    EXPECT_TRUE(file_handle->read_record(published, &read_buffer, nullptr));
    size_t visible_after_publish = 0;
    for (RmScan scan(file_handle.get()); !scan.is_end(); scan.next()) {
        ++visible_after_publish;
    }
    EXPECT_EQ(visible_after_publish, static_cast<size_t>(records_per_page));

    {
        RmScan scan(file_handle.get());
        while (!scan.is_end() && scan.rid() != published) {
            scan.next();
        }
        ASSERT_FALSE(scan.is_end());
        file_handle->delete_record(published, nullptr);
        Rid repending =
            file_handle->insert_record(record.data(), nullptr, nullptr, false, nullptr, nullptr, true);
        ASSERT_EQ(repending, published);
        EXPECT_FALSE(scan.with_current_slot([](const char *) { return true; }));
        file_handle->rollback_pending_insert(repending);
    }

    local_rm_manager->close_file(file_handle.get());
    local_rm_manager->destroy_file(filename);
}

TEST(RecordManagerTest, UpdatesOnDifferentPagesDoNotShareATableWriteLatch) {
    auto local_disk_manager = std::make_unique<DiskManager>();
    auto local_buffer_pool_manager = std::make_unique<BufferPoolManager>(64, local_disk_manager.get());
    auto local_rm_manager =
        std::make_unique<RmManager>(local_disk_manager.get(), local_buffer_pool_manager.get());
    const std::string filename = "record_page_latch_concurrency.txt";
    if (local_disk_manager->is_file(filename)) {
        local_disk_manager->destroy_file(filename);
    }

    local_rm_manager->create_file(filename, RM_MAX_RECORD_SIZE);
    auto file_handle = local_rm_manager->open_file(filename);
    std::vector<char> initial(RM_MAX_RECORD_SIZE, 'i');
    const int records_per_page = file_handle->file_hdr_.num_records_per_page;
    std::vector<Rid> records;
    records.reserve(static_cast<size_t>(records_per_page + 1));
    for (int i = 0; i <= records_per_page; ++i) {
        records.push_back(file_handle->insert_record(initial.data(), nullptr));
    }
    const Rid first_page_rid = records.front();
    const Rid second_page_rid = records.back();
    ASSERT_NE(first_page_rid.page_no, second_page_rid.page_no);

    WritePageGuard blocked_page = local_buffer_pool_manager->fetch_page_write(
        PageId{file_handle->GetFd(), first_page_rid.page_no});
    ASSERT_TRUE(blocked_page);
    std::vector<char> first_update(RM_MAX_RECORD_SIZE, 'a');
    std::vector<char> second_update(RM_MAX_RECORD_SIZE, 'b');
    std::promise<void> first_started;
    auto first_started_future = first_started.get_future();
    auto first_writer = std::async(std::launch::async, [&]() {
        first_started.set_value();
        file_handle->update_record(first_page_rid, first_update.data(), nullptr);
    });
    first_started_future.wait();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    auto second_writer = std::async(std::launch::async, [&]() {
        file_handle->update_record(second_page_rid, second_update.data(), nullptr);
    });
    const auto second_status = second_writer.wait_for(std::chrono::seconds(1));
    blocked_page.Drop();
    second_writer.get();
    first_writer.get();
    EXPECT_EQ(second_status, std::future_status::ready);

    auto first_record = file_handle->get_record(first_page_rid, nullptr);
    auto second_record = file_handle->get_record(second_page_rid, nullptr);
    EXPECT_EQ(std::memcmp(first_record->data, first_update.data(), RM_MAX_RECORD_SIZE), 0);
    EXPECT_EQ(std::memcmp(second_record->data, second_update.data(), RM_MAX_RECORD_SIZE), 0);

    local_rm_manager->close_file(file_handle.get());
    local_rm_manager->destroy_file(filename);
}

TEST(RecordManagerTest, ConcurrentInsertLanesReserveUniquePersistentSlots) {
    auto local_disk_manager = std::make_unique<DiskManager>();
    auto local_buffer_pool_manager = std::make_unique<BufferPoolManager>(256, local_disk_manager.get());
    auto local_rm_manager =
        std::make_unique<RmManager>(local_disk_manager.get(), local_buffer_pool_manager.get());
    const std::string filename = "record_insert_lane_concurrency.txt";
    if (local_disk_manager->is_file(filename)) {
        local_disk_manager->destroy_file(filename);
    }

    local_rm_manager->create_file(filename, 64);
    auto file_handle = local_rm_manager->open_file(filename);
    constexpr int thread_count = 8;
    constexpr int inserts_per_thread = 200;
    std::mutex rid_latch;
    std::vector<Rid> inserted;
    inserted.reserve(thread_count * inserts_per_thread);
    std::vector<std::future<void>> writers;
    for (int thread_id = 0; thread_id < thread_count; ++thread_id) {
        writers.push_back(std::async(std::launch::async, [&, thread_id] {
            std::array<char, 64> record{};
            for (int sequence = 0; sequence < inserts_per_thread; ++sequence) {
                const int value = thread_id * inserts_per_thread + sequence;
                memcpy(record.data(), &value, sizeof(value));
                Rid rid = file_handle->insert_record(record.data(), nullptr);
                std::lock_guard<std::mutex> guard(rid_latch);
                inserted.push_back(rid);
            }
        }));
    }
    for (auto &writer : writers) {
        writer.get();
    }

    std::sort(inserted.begin(), inserted.end(), [](const Rid &lhs, const Rid &rhs) {
        return lhs.page_no == rhs.page_no ? lhs.slot_no < rhs.slot_no : lhs.page_no < rhs.page_no;
    });
    EXPECT_EQ(inserted.size(), static_cast<size_t>(thread_count * inserts_per_thread));
    EXPECT_EQ(std::unique(inserted.begin(), inserted.end()), inserted.end());

    local_rm_manager->close_file(file_handle.get());
    file_handle.reset();
    file_handle = local_rm_manager->open_file(filename);
    std::array<char, 64> final_record{};
    Rid final_rid = file_handle->insert_record(final_record.data(), nullptr);
    EXPECT_TRUE(file_handle->is_record(final_rid));

    size_t visible = 0;
    for (RmScan scan(file_handle.get()); !scan.is_end(); scan.next()) {
        ++visible;
    }
    EXPECT_EQ(visible, static_cast<size_t>(thread_count * inserts_per_thread + 1));

    local_rm_manager->close_file(file_handle.get());
    file_handle.reset();
    local_rm_manager->destroy_file(filename);
}

TEST(BufferPoolBackgroundFlushTest, FlushUnpinnedPagesInBatches) {
    auto local_disk_manager = std::make_unique<DiskManager>();
    const std::string filename = "background_flush.txt";
    if (local_disk_manager->is_file(filename)) {
        local_disk_manager->destroy_file(filename);
    }
    local_disk_manager->create_file(filename);
    int fd = local_disk_manager->open_file(filename);
    auto local_buffer_pool_manager = std::make_unique<BufferPoolManager>(8, local_disk_manager.get());

    std::vector<PageId> page_ids;
    for (int i = 0; i < 3; ++i) {
        PageId page_id{fd, INVALID_PAGE_ID};
        Page *page = local_buffer_pool_manager->new_page(&page_id);
        ASSERT_NE(page, nullptr);
        memset(page->get_data(), 'a' + i, PAGE_SIZE);
        ASSERT_TRUE(local_buffer_pool_manager->unpin_page(page_id, true));
        page_ids.push_back(page_id);
    }

    size_t cursor = 0;
    bool pass_complete = false;
    size_t flushed = 0;
    while (!pass_complete) {
        flushed += local_buffer_pool_manager->flush_unpinned_pages_batch(1, 2, &cursor, &pass_complete);
    }
    EXPECT_EQ(flushed, page_ids.size());
    for (size_t i = 0; i < page_ids.size(); ++i) {
        std::vector<char> data(PAGE_SIZE);
        local_disk_manager->read_page(fd, page_ids[i].page_no, data.data(), PAGE_SIZE);
        EXPECT_EQ(data[0], 'a' + static_cast<int>(i));
    }

    local_disk_manager->close_file(fd);
    local_disk_manager->destroy_file(filename);
}

TEST(BufferPoolBackgroundFlushTest, FailedBatchWriteKeepsPageDirty) {
    auto local_disk_manager = std::make_unique<DiskManager>();
    const std::string filename = "background_flush_failure.txt";
    if (local_disk_manager->is_file(filename)) {
        local_disk_manager->destroy_file(filename);
    }
    local_disk_manager->create_file(filename);
    int fd = local_disk_manager->open_file(filename);
    auto local_buffer_pool_manager = std::make_unique<BufferPoolManager>(2, local_disk_manager.get());
    PageId page_id{fd, INVALID_PAGE_ID};
    Page *page = local_buffer_pool_manager->new_page(&page_id);
    ASSERT_NE(page, nullptr);
    std::memset(page->get_data(), 'x', PAGE_SIZE);
    ASSERT_TRUE(local_buffer_pool_manager->unpin_page(page_id, true));

    local_disk_manager->close_file(fd);
    size_t cursor = 0;
    bool pass_complete = false;
    EXPECT_THROW(local_buffer_pool_manager->flush_unpinned_pages_batch(1, 2, &cursor,
                                                                       &pass_complete),
                 UnixError);
    EXPECT_TRUE(page->is_dirty());

    local_buffer_pool_manager.reset();
    local_disk_manager->destroy_file(filename);
}

TEST(BufferPoolBackgroundFlushTest, FlushesSelectedFilesInOnePass) {
    auto local_disk_manager = std::make_unique<DiskManager>();
    const std::array<std::string, 3> filenames = {
        "selected_flush_a.txt", "selected_flush_b.txt", "selected_flush_c.txt"};
    std::array<int, 3> fds{};
    for (size_t i = 0; i < filenames.size(); ++i) {
        if (local_disk_manager->is_file(filenames[i])) {
            local_disk_manager->destroy_file(filenames[i]);
        }
        local_disk_manager->create_file(filenames[i]);
        fds[i] = local_disk_manager->open_file(filenames[i]);
    }

    constexpr std::array<size_t, 3> page_counts = {3, 2, 3};
    auto local_buffer_pool_manager = std::make_unique<BufferPoolManager>(8, local_disk_manager.get());
    for (size_t file_idx = 0; file_idx < fds.size(); ++file_idx) {
        for (size_t page_idx = 0; page_idx < page_counts[file_idx]; ++page_idx) {
            PageId page_id{fds[file_idx], INVALID_PAGE_ID};
            Page *page = local_buffer_pool_manager->new_page(&page_id);
            ASSERT_NE(page, nullptr);
            memset(page->get_data(), 'a' + static_cast<int>(file_idx * 3 + page_idx), PAGE_SIZE);
            ASSERT_TRUE(local_buffer_pool_manager->unpin_page(page_id, true));
        }
    }

    EXPECT_EQ(local_buffer_pool_manager->flush_pages_for_fds({fds[0], fds[0], fds[2]}), 6U);
    EXPECT_THROW(local_buffer_pool_manager->flush_pages_for_fds({DiskManager::MAX_FD}),
                 FileNotOpenError);
    for (size_t file_idx = 0; file_idx < fds.size(); ++file_idx) {
        for (size_t page_idx = 0; page_idx < page_counts[file_idx]; ++page_idx) {
            std::array<char, PAGE_SIZE> data{};
            local_disk_manager->read_page(fds[file_idx], static_cast<page_id_t>(page_idx),
                                          data.data(), PAGE_SIZE);
            const char expected = file_idx == 1
                                      ? '\0'
                                      : static_cast<char>('a' + file_idx * 3 + page_idx);
            EXPECT_EQ(data[0], expected);
        }
    }

    EXPECT_EQ(local_buffer_pool_manager->flush_all_pages(fds[1]), 2U);
    for (size_t page_idx = 0; page_idx < page_counts[1]; ++page_idx) {
        std::array<char, PAGE_SIZE> data{};
        local_disk_manager->read_page(fds[1], static_cast<page_id_t>(page_idx), data.data(), PAGE_SIZE);
        EXPECT_EQ(data[0], static_cast<char>('a' + 3 + page_idx));
    }

    for (size_t i = 0; i < filenames.size(); ++i) {
        local_disk_manager->close_file(fds[i]);
        local_disk_manager->destroy_file(filenames[i]);
    }
}

TEST(StatementCheckpointGateTest, SerializesWritersAndMovesOwnership) {
    StatementCheckpointGate gate;
    auto reader = gate.ReadEnter();
    std::promise<void> start_promise;
    auto start = start_promise.get_future().share();
    std::promise<void> release_promise;
    auto release = release_promise.get_future().share();
    std::atomic<int> ready{0};
    std::atomic<int> attempting{0};
    std::atomic<int> active_writers{0};
    std::atomic<int> max_active_writers{0};

    auto writer = [&] {
        ready.fetch_add(1, std::memory_order_release);
        start.wait();
        attempting.fetch_add(1, std::memory_order_release);
        auto guard = gate.WriteEnter();
        StatementCheckpointGate::WriteGuard moved_guard(std::move(guard));
        int active = active_writers.fetch_add(1, std::memory_order_acq_rel) + 1;
        int observed = max_active_writers.load(std::memory_order_relaxed);
        while (active > observed &&
               !max_active_writers.compare_exchange_weak(observed, active,
                                                         std::memory_order_relaxed)) {
        }
        release.wait();
        active_writers.fetch_sub(1, std::memory_order_acq_rel);
    };

    auto first = std::async(std::launch::async, writer);
    auto second = std::async(std::launch::async, writer);
    while (ready.load(std::memory_order_acquire) != 2) {
        std::this_thread::yield();
    }
    start_promise.set_value();
    while (attempting.load(std::memory_order_acquire) != 2) {
        std::this_thread::yield();
    }
    EXPECT_EQ(active_writers.load(std::memory_order_acquire), 0);

    reader = StatementCheckpointGate::ReadGuard();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (active_writers.load(std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    ASSERT_EQ(active_writers.load(std::memory_order_acquire), 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(max_active_writers.load(std::memory_order_acquire), 1);

    release_promise.set_value();
    first.get();
    second.get();
    EXPECT_EQ(max_active_writers.load(std::memory_order_acquire), 1);
}

TEST(TransactionManagerTest, TableVersionCacheObservesCreation) {
    TransactionManager manager(nullptr, nullptr);
    EXPECT_EQ(manager.GetTableVersionInfo("cached_table"), nullptr);
    auto created = manager.GetOrCreateTableVersionInfo("cached_table");
    ASSERT_NE(created, nullptr);
    EXPECT_EQ(manager.GetTableVersionInfo("cached_table"), created);
}

TEST(TransactionManagerTest, LightweightReadOnlySkipsRegistryAndCommitPublication) {
    TransactionManager manager(nullptr, nullptr);
    Transaction *txn = manager.begin_read_only(IsolationLevel::SNAPSHOT_ISOLATION);
    ASSERT_NE(txn, nullptr);
    EXPECT_TRUE(txn->is_lightweight_read_only());
    EXPECT_EQ(manager.RegisteredTransactionCount(), 0U);
    EXPECT_EQ(manager.ActiveTransactionCount(), 1U);
    manager.commit(txn, nullptr);
    EXPECT_EQ(manager.RegisteredTransactionCount(), 0U);
    EXPECT_EQ(manager.ActiveTransactionCount(), 0U);
}

TEST(TransactionManagerTest, LightweightSnapshotPromotesWithoutChangingReadTimestamp) {
    TransactionManager manager(nullptr, nullptr);
    Transaction *txn = manager.begin_read_only(IsolationLevel::SNAPSHOT_ISOLATION);
    ASSERT_NE(txn, nullptr);
    timestamp_t read_ts = txn->get_read_ts();
    EXPECT_EQ(txn->get_transaction_id(), INVALID_TXN_ID);
    manager.PromoteToWrite(txn, nullptr);
    EXPECT_FALSE(txn->is_lightweight_read_only());
    EXPECT_NE(txn->get_transaction_id(), INVALID_TXN_ID);
    EXPECT_EQ(txn->get_read_ts(), read_ts);
    EXPECT_EQ(manager.RegisteredTransactionCount(), 1U);
    manager.commit(txn, nullptr);
}

TEST(TransactionManagerTest, DeferredExplicitBeginKeepsIdAndSkipsRegistryUntilWrite) {
    TransactionManager manager(nullptr, nullptr);
    Transaction *txn = manager.begin_read_only(IsolationLevel::SNAPSHOT_ISOLATION, true);
    ASSERT_NE(txn, nullptr);
    const txn_id_t reserved_id = txn->get_transaction_id();
    const timestamp_t read_ts = txn->get_read_ts();
    EXPECT_NE(reserved_id, INVALID_TXN_ID);
    EXPECT_EQ(txn->get_first_lsn(), INVALID_LSN);
    EXPECT_EQ(manager.RegisteredTransactionCount(), 0U);

    manager.PromoteToWrite(txn, nullptr);
    EXPECT_FALSE(txn->is_lightweight_read_only());
    EXPECT_EQ(txn->get_transaction_id(), reserved_id);
    EXPECT_EQ(txn->get_read_ts(), read_ts);
    EXPECT_EQ(manager.RegisteredTransactionCount(), 1U);
    manager.commit(txn, nullptr);
}

TEST(TransactionManagerTest, ReclaimsFinishedZeroReferenceTransaction) {
    TransactionManager manager(nullptr, nullptr);
    Transaction *txn = manager.begin(nullptr, nullptr, IsolationLevel::SNAPSHOT_ISOLATION);
    ASSERT_NE(txn, nullptr);

    manager.commit(txn, nullptr);
    EXPECT_EQ(manager.RegisteredTransactionCount(), 1U);

    manager.GarbageCollection();
    EXPECT_EQ(manager.RegisteredTransactionCount(), 0U);
}

TEST(TransactionManagerTest, ReclaimsEntireUndoChainInOneGcPass) {
    TransactionManager manager(nullptr, nullptr);
    constexpr size_t kChainLength = 128;
    const std::string table_name = "undo_chain";
    const Rid rid{1, 0};

    auto table_info = manager.GetOrCreateTableVersionInfo(table_name);
    auto page_info = std::make_shared<TransactionManager::PageVersionInfo>();
    page_info->InitDirtySlots(1);
    table_info->mark_page_version_info(rid.page_no);
    table_info->mark_page_dirty_exact(rid.page_no);
    {
        std::unique_lock<std::shared_mutex> table_lock(table_info->mutex_);
        table_info->pages_.emplace(rid.page_no, page_info);
        table_info->publish_page_info_dense(rid.page_no, page_info.get());
    }

    for (size_t i = 0; i < kChainLength; ++i) {
        Transaction *txn = manager.begin(nullptr, nullptr, IsolationLevel::SNAPSHOT_ISOLATION);
        ASSERT_NE(txn, nullptr);
        UndoLog log;
        log.ts_ = txn->get_read_ts();
        const UndoLink link = manager.AppendUndoLog(table_name, rid, txn, std::move(log));
        manager.InstallTupleVersion(table_name, rid, VersionUndoLink{link, true},
                                    TupleMeta{TXN_START_ID + txn->get_transaction_id(), false}, txn);
        manager.commit(txn, nullptr);
    }

    ASSERT_EQ(manager.RegisteredTransactionCount(), kChainLength);
    ASSERT_TRUE(manager.UpdateVersionLink(table_name, rid, std::nullopt));

    manager.GarbageCollection();
    EXPECT_EQ(manager.RegisteredTransactionCount(), 0U);
}

TEST(TransactionRegistryTest, OwnsAndLooksUpTransactions) {
    TransactionRegistry registry;
    auto *txn = new Transaction(42);
    registry.Insert(txn);
    EXPECT_EQ(registry.Size(), 1U);
    EXPECT_EQ(registry.GetThreadOwned(42), txn);
    auto duplicate = std::make_unique<Transaction>(42);
    EXPECT_THROW(registry.Insert(duplicate.get()), InternalError);
}

TEST(WatermarkTest, FinishesTransactionInOneCriticalSection) {
    Watermark watermark(5);
    watermark.AddTxn(5);
    watermark.AddTxn(7);
    EXPECT_EQ(watermark.GetWatermark(), 5);

    watermark.FinishTxn(7, 10);
    EXPECT_EQ(watermark.GetWatermark(), 5);
    watermark.FinishTxn(5, 11);
    EXPECT_EQ(watermark.GetWatermark(), 11);

    // A missing reader still advances the empty-set frontier, matching the
    // previous UpdateCommitTs() followed by RemoveTxn() behavior.
    watermark.FinishTxn(99, 12);
    EXPECT_EQ(watermark.GetWatermark(), 12);
}

TEST(IndexMetaTest, PreservesStableIdAndReadsV2Metadata) {
    std::istringstream legacy("@index_meta_v2 orders orders_id.idx 4 0 1 0");
    IndexMeta upgraded;
    legacy >> upgraded;
    EXPECT_EQ(upgraded.index_id, 0U);

    upgraded.index_id = 77;
    std::ostringstream encoded;
    encoded << upgraded;
    std::istringstream current(encoded.str());
    IndexMeta decoded;
    current >> decoded;
    EXPECT_EQ(decoded.index_id, 77U);
    EXPECT_EQ(decoded.index_name, "orders_id.idx");
}

TEST(SnapshotIndexHistoryStoreTest, SeparatesIndexesAndHonorsLifecycle) {
    rmdb::SnapshotIndexHistoryStore store;
    store.Record("orders", 1, Rid{1, 1}, 30);
    store.Record("orders", 1, Rid{1, 2}, 10);
    store.Record("orders", 2, Rid{2, 1}, 20);
    store.Record("customer", 3, Rid{3, 1}, 40);

    EXPECT_EQ(store.Lookup("orders", 1, 5), (std::vector<Rid>{{1, 2}, {1, 1}}));
    EXPECT_EQ(store.Lookup("orders", 1, 10), (std::vector<Rid>{{1, 1}}));
    EXPECT_EQ(store.Lookup("orders", 2, 10), (std::vector<Rid>{{2, 1}}));

    store.Purge(20);
    EXPECT_EQ(store.Lookup("orders", 1, 0), (std::vector<Rid>{{1, 1}}));
    EXPECT_TRUE(store.Lookup("orders", 2, 0).empty());

    store.EraseIndex("orders", 1);
    EXPECT_TRUE(store.Lookup("orders", 1, 0).empty());
    store.EraseTable("customer");
    EXPECT_EQ(store.EntryCountForTest(), 0U);
}

TEST(ReusableFlatU64SetTest, ReusesStorageAcrossClearAndRehash) {
    rmdb::ReusableFlatU64Set values;
    values.reserve(64);
    EXPECT_TRUE(values.insert(0));
    EXPECT_TRUE(values.insert(std::numeric_limits<rmdb::u64>::max()));
    EXPECT_FALSE(values.insert(0));
    for (rmdb::u64 i = 1; i <= 2000; ++i) {
        EXPECT_TRUE(values.insert(i << 32));
    }
    EXPECT_EQ(values.size(), 2002U);
    EXPECT_TRUE(values.contains(0));
    EXPECT_TRUE(values.contains(std::numeric_limits<rmdb::u64>::max()));
    EXPECT_TRUE(values.contains(2000ULL << 32));
    EXPECT_FALSE(values.contains(2001ULL << 32));

    for (int generation = 0; generation < 100; ++generation) {
        values.clear();
        EXPECT_TRUE(values.empty());
        const rmdb::u64 key = (static_cast<rmdb::u64>(generation) << 32) | 7;
        EXPECT_TRUE(values.insert(key));
        EXPECT_TRUE(values.contains(key));
        EXPECT_FALSE(values.contains(key + 1));
    }
}

TEST(InternedIdentifierTest, CopiesWithoutDuplicatingStringStorage) {
    EXPECT_EQ(sizeof(TabCol), sizeof(void *) * 3);
    std::string long_name = "identifier_longer_than_sso";
    rmdb::InternedIdentifier first(long_name);
    rmdb::InternedIdentifier second{std::string(long_name)};
    rmdb::InternedIdentifier copied = first;

    EXPECT_EQ(first, second);
    EXPECT_EQ(first, copied);
    EXPECT_EQ(first.data(), second.data());
    EXPECT_EQ(first.data(), copied.data());
    EXPECT_EQ(first, long_name);
    EXPECT_EQ(long_name, first);
    EXPECT_EQ(first + ".suffix", long_name + ".suffix");

    second = "another_identifier_longer_than_sso";
    EXPECT_NE(first, second);
    EXPECT_LT(second, first);
    second = "";
    EXPECT_TRUE(second.empty());
}

TEST(InternedIdentifierTest, PreservesCatalogTextSerialization) {
    ColMeta original;
    original.tab_name = "table_name_longer_than_sso";
    original.name = "column_name_longer_than_sso";
    original.type = TYPE_INT;
    original.len = static_cast<int>(sizeof(int));
    original.offset = 17;
    original.index = true;

    std::stringstream encoded;
    encoded << original;
    ColMeta decoded;
    encoded >> decoded;

    EXPECT_EQ(decoded.tab_name, original.tab_name);
    EXPECT_EQ(decoded.name, original.name);
    EXPECT_EQ(decoded.type, original.type);
    EXPECT_EQ(decoded.len, original.len);
    EXPECT_EQ(decoded.offset, original.offset);
    EXPECT_EQ(decoded.index, original.index);
    EXPECT_EQ(decoded.tab_name.data(), original.tab_name.data());
    EXPECT_EQ(decoded.name.data(), original.name.data());
}

TEST(PlanTemplateTest, BindsEqualInsertValuesByPosition) {
    auto query = std::make_shared<Query>();
    query->kind = StmtKind::Insert;
    query->target_table = "t";
    for (int i = 0; i < 3; ++i) {
        Value value;
        value.set_int(0);
        query->values.push_back(value);
    }

    auto query_template = std::make_shared<rmdb::query_template_detail::QueryTemplate>();
    query_template->skeleton = rmdb::query_template_detail::clone_query_skeleton(query);
    rmdb::SqlTemplateCandidate candidate;
    for (rmdb::u32 i = 0; i < 3; ++i) {
        rmdb::query_template_detail::QueryLiteralSlot slot;
        slot.target = rmdb::query_template_detail::QueryLiteralTarget::kQueryValue;
        slot.index = i;
        query_template->slots.push_back(slot);
        ASSERT_TRUE(candidate.literals.push_back(rmdb::SqlTemplateLiteral{}));
    }

    auto plan = std::make_shared<DMLPlan>(T_Insert, nullptr, "t", query->values,
                                          std::vector<Condition>{}, std::vector<SetClause>{});
    auto plan_template = rmdb::plan_template_detail::make_template(candidate, query_template, plan, 1);
    ASSERT_NE(plan_template, nullptr);
    ASSERT_EQ(plan_template->slots.size(), 3U);
    for (rmdb::u32 i = 0; i < 3; ++i) {
        EXPECT_EQ(plan_template->slots[i].index, i);
        EXPECT_EQ(plan_template->slots[i].source_index, i);
    }
}

TEST(TransactionManagerTest, FailedRegistrationRestoresAdmissionAndWatermarkState) {
    TransactionManager manager(nullptr, nullptr);
    Transaction *registered = manager.begin(nullptr, nullptr, IsolationLevel::SNAPSHOT_ISOLATION);
    ASSERT_NE(registered, nullptr);
    const timestamp_t initial_read_ts = registered->get_read_ts();

    Transaction duplicate(registered->get_transaction_id(), IsolationLevel::SNAPSHOT_ISOLATION);
    EXPECT_THROW(manager.begin(&duplicate, nullptr, IsolationLevel::SNAPSHOT_ISOLATION), InternalError);
    EXPECT_EQ(manager.RegisteredTransactionCount(), 1U);
    EXPECT_EQ(manager.ActiveTransactionCount(), 1U);
    EXPECT_FALSE(duplicate.watermark_registered());
    EXPECT_FALSE(duplicate.admission_active());

    manager.commit(registered, nullptr);
    EXPECT_EQ(manager.ActiveTransactionCount(), 0U);
    EXPECT_GT(manager.GetWatermark(), initial_read_ts);
}

TEST(TransactionManagerTest, ClearsVersionAndIndexHistoryOnTableLifecycle) {
    TransactionManager manager(nullptr, nullptr);
    IndexMeta index;
    index.tab_name = "orders";
    index.index_name = "orders_pk.idx";
    index.index_id = 1;

    manager.GetOrCreateTableVersionInfo("orders");
    manager.RecordSnapshotIndexRetirement("orders", index, Rid{4, 2}, 20);
    EXPECT_EQ(manager.LookupSnapshotIndexHistory("orders", index, 10), (std::vector<Rid>{{4, 2}}));

    manager.EraseSnapshotIndexHistory("orders", index);
    EXPECT_TRUE(manager.LookupSnapshotIndexHistory("orders", index, 10).empty());

    manager.RecordSnapshotIndexRetirement("orders", index, Rid{4, 3}, 30);
    manager.EraseTableState("orders");
    EXPECT_EQ(manager.GetTableVersionInfo("orders"), nullptr);
    EXPECT_TRUE(manager.LookupSnapshotIndexHistory("orders", index, 0).empty());
}

TEST(TransactionManagerTest, SerializableWriteSkewAbortsSecondWriter) {
    TransactionManager manager(nullptr, nullptr);
    Transaction *txn1 = manager.begin(nullptr, nullptr, IsolationLevel::SERIALIZABLE);
    Transaction *txn2 = manager.begin(nullptr, nullptr, IsolationLevel::SERIALIZABLE);
    Rid first{1, 1};
    Rid second{1, 2};

    manager.RecordSerializableRead(txn1, "duty", first);
    manager.RecordSerializableRead(txn2, "duty", second);
    manager.RecordSerializableWrite(txn1, "duty", second, nullptr, nullptr, nullptr);
    EXPECT_THROW(manager.RecordSerializableWrite(txn2, "duty", first, nullptr, nullptr, nullptr),
                 TransactionAbortException);

    manager.abort(txn2, nullptr);
    manager.abort(txn1, nullptr);
    // GC 触发有 2ms 节流（高频提交下避免逐事务全局扫描）；此处手动触发以验证
    // 事务最终从注册表移除（对外语义不变，仅注册表内部状态延迟清除）。
    manager.GarbageCollection();
    EXPECT_EQ(manager.RegisteredTransactionCount(), 0U);
}

TEST(LockManagerTest, SnapshotExclusiveRecordLockWaitsUnbounded) {
    LockManager manager;
    Transaction owner(100, IsolationLevel::SNAPSHOT_ISOLATION);
    Transaction waiter(101, IsolationLevel::SNAPSHOT_ISOLATION);
    Rid rid{7, 3};

    ASSERT_TRUE(manager.lock_exclusive_on_record(&owner, rid, 5));
    auto waiting = std::async(std::launch::async, [&] {
        return manager.lock_exclusive_on_record(&waiter, rid, 5);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    // PG-style unbounded wait: the waiter stays pending (no timeout abort).
    EXPECT_EQ(waiting.wait_for(std::chrono::milliseconds(1)), std::future_status::timeout);
    EXPECT_TRUE(manager.unlock_all(&owner));
    EXPECT_TRUE(waiting.get());
    EXPECT_TRUE(manager.unlock_all(&waiter));
}

TEST(LockManagerTest, SnapshotWaiterAcquiresAfterOwnerFinishes) {
    LockManager manager;
    Transaction owner(200, IsolationLevel::SNAPSHOT_ISOLATION);
    Transaction waiter(201, IsolationLevel::SNAPSHOT_ISOLATION);
    Rid rid{8, 4};

    ASSERT_TRUE(manager.lock_exclusive_on_record(&owner, rid, 6));
    auto waiting = std::async(std::launch::async, [&] {
        return manager.lock_exclusive_on_record(&waiter, rid, 6);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    EXPECT_TRUE(manager.unlock_all(&owner));
    EXPECT_TRUE(waiting.get());
    EXPECT_TRUE(manager.unlock_all(&waiter));
}

TEST(LockManagerTest, PreSnapshotWriteAdmissionHandsOffToOneSuccessor) {
    LockManager manager(std::chrono::seconds(1), 1);
    Transaction owner(210, IsolationLevel::SNAPSHOT_ISOLATION);
    Transaction first(211, IsolationLevel::SNAPSHOT_ISOLATION);
    Transaction second(212, IsolationLevel::SNAPSHOT_ISOLATION);
    constexpr lock_data_key_t key = 0x123456789ULL;

    ASSERT_TRUE(manager.acquire_pre_snapshot_writes(&owner, {key}));
    std::atomic<bool> first_started{false};
    auto first_wait = std::async(std::launch::async, [&] {
        first_started.store(true, std::memory_order_release);
        return manager.acquire_pre_snapshot_writes(&first, {key});
    });
    while (!first_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    std::atomic<bool> second_started{false};
    auto second_wait = std::async(std::launch::async, [&] {
        second_started.store(true, std::memory_order_release);
        return manager.acquire_pre_snapshot_writes(&second, {key});
    });
    while (!second_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    EXPECT_EQ(first_wait.wait_for(std::chrono::milliseconds(1)), std::future_status::timeout);
    EXPECT_EQ(second_wait.wait_for(std::chrono::milliseconds(20)), std::future_status::ready);
    EXPECT_FALSE(second_wait.get());
    EXPECT_TRUE(second.get_pre_snapshot_write_keys().empty());
    EXPECT_TRUE(manager.unlock_all(&owner));
    EXPECT_TRUE(first_wait.get());
    EXPECT_TRUE(manager.unlock_all(&first));
}

TEST(LockManagerTest, PreSnapshotWriteAdmissionTimeoutCleansWaiterState) {
    LockManager manager(std::chrono::milliseconds(2), 1);
    Transaction owner(220, IsolationLevel::SNAPSHOT_ISOLATION);
    Transaction waiter(221, IsolationLevel::SNAPSHOT_ISOLATION);
    constexpr lock_data_key_t key = 0x23456789aULL;

    ASSERT_TRUE(manager.acquire_pre_snapshot_writes(&owner, {key}));
    EXPECT_FALSE(manager.acquire_pre_snapshot_writes(&waiter, {key}));
    EXPECT_TRUE(waiter.get_pre_snapshot_write_keys().empty());
    EXPECT_TRUE(manager.unlock_all(&owner));
    EXPECT_TRUE(manager.acquire_pre_snapshot_writes(&waiter, {key}));
    EXPECT_TRUE(manager.unlock_all(&waiter));
}

TEST(LockManagerTest, PreSnapshotSuccessorWaitsForPredecessorResponseAfterLockRelease) {
    LockManager manager(std::chrono::seconds(1), 1);
    Transaction owner(230, IsolationLevel::SNAPSHOT_ISOLATION);
    Transaction successor(231, IsolationLevel::SNAPSHOT_ISOLATION);
    constexpr lock_data_key_t key = 0x3456789abULL;

    owner.set_response_order_keys({key});
    successor.set_response_order_keys({key});
    ASSERT_TRUE(manager.acquire_pre_snapshot_writes(&owner, {key}));
    auto acquire = std::async(std::launch::async, [&] {
        return manager.acquire_pre_snapshot_writes(&successor, {key});
    });
    EXPECT_EQ(acquire.wait_for(std::chrono::milliseconds(5)), std::future_status::timeout);

    owner.set_state(TransactionState::COMMITTED);
    manager.register_response_order(&owner);
    ASSERT_TRUE(manager.unlock_all(&owner));
    ASSERT_TRUE(acquire.get());
    manager.register_response_order(&successor);

    auto response_wait = std::async(std::launch::async, [&] {
        successor.wait_for_response_dependencies();
    });
    EXPECT_EQ(response_wait.wait_for(std::chrono::milliseconds(5)), std::future_status::timeout);
    owner.complete_response_gate();
    EXPECT_EQ(response_wait.wait_for(std::chrono::milliseconds(100)), std::future_status::ready);
    response_wait.get();

    successor.set_state(TransactionState::ABORTED);
    EXPECT_TRUE(manager.unlock_all(&successor));
    successor.complete_response_gate();
}

TEST(TransactionManagerTest, PreSnapshotAdmissionChoosesSnapshotAfterPredecessorCommit) {
    LockManager lock_manager(std::chrono::seconds(1), 1);
    TransactionManager manager(&lock_manager, nullptr);
    constexpr lock_data_key_t key = 0x3456789abULL;
    Transaction *owner = manager.begin(nullptr, nullptr, IsolationLevel::SNAPSHOT_ISOLATION, {key});
    ASSERT_NE(owner, nullptr);

    std::atomic<bool> waiter_started{false};
    auto waiting = std::async(std::launch::async, [&] {
        waiter_started.store(true, std::memory_order_release);
        return manager.begin(nullptr, nullptr, IsolationLevel::SNAPSHOT_ISOLATION, {key});
    });
    while (!waiter_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT_EQ(waiting.wait_for(std::chrono::milliseconds(1)), std::future_status::timeout);

    manager.commit(owner, nullptr);
    const timestamp_t predecessor_commit = owner->get_commit_ts();
    Transaction *waiter = waiting.get();
    ASSERT_NE(waiter, nullptr);
    EXPECT_GE(waiter->get_read_ts(), predecessor_commit);
    manager.commit(waiter, nullptr);
    manager.GarbageCollection();
}

TEST(TransactionManagerTest, WriteConflictRecheckReportsOwnerOutcome) {
    TransactionManager manager(nullptr, nullptr);
    Transaction waiter(301, IsolationLevel::SNAPSHOT_ISOLATION);
    waiter.set_read_ts(20);

    try {
        manager.EnsureWriteConflictFree(&waiter, TupleMeta{TXN_START_ID + 300, false});
        FAIL() << "active owner must conflict";
    } catch (TransactionAbortException &error) {
        EXPECT_EQ(error.GetAbortSubReason(), AbortSubReason::UNCOMMITTED_WRITE_CONFLICT);
    }

    try {
        manager.EnsureWriteConflictFree(&waiter, TupleMeta{21, false});
        FAIL() << "newer committed owner must conflict";
    } catch (TransactionAbortException &error) {
        EXPECT_EQ(error.GetAbortSubReason(), AbortSubReason::STALE_SNAPSHOT_WRITE_CONFLICT);
    }

    EXPECT_NO_THROW(manager.EnsureWriteConflictFree(&waiter, TupleMeta{20, false}));
}
