/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sm_manager.h"

#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <chrono>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

#include "index/ix.h"
#include "record/rm.h"
#include "record/rm_scan.h"
#include "record_printer.h"
#include "common/index_runtime.h"
#include "common/catalog_change.h"
#include "transaction/transaction_manager.h"

namespace {

void collect_table_index_entries(
    RmFileHandle *fh, const std::vector<IndexMeta> &indexes,
    std::vector<IxBulkLoadEntries> *all_entries) {
    std::vector<std::string> keys;
    keys.reserve(indexes.size());
    all_entries->clear();
    all_entries->reserve(indexes.size());
    const auto &file_hdr = fh->get_file_hdr();
    const size_t page_count = static_cast<size_t>(
        std::max(0, file_hdr.num_pages - RM_FIRST_RECORD_PAGE));
    const size_t records_per_page = static_cast<size_t>(file_hdr.num_records_per_page);
    if (records_per_page != 0 &&
        page_count > std::numeric_limits<rmdb::u32>::max() / records_per_page) {
        throw InternalError("Bulk-load entry estimate overflow");
    }
    const size_t entry_capacity = page_count * records_per_page;
    for (const auto &index : indexes) {
        keys.emplace_back(index.col_tot_len, '\0');
        all_entries->emplace_back(static_cast<size_t>(index.col_tot_len));
        all_entries->back().reserve(entry_capacity);
    }
    for (RmScan scan(fh, BufferAccessClass::IndexBuild); !scan.is_end(); scan.next()) {
        scan.with_current_slot([&](const char *slot) {
            for (size_t i = 0; i < indexes.size(); ++i) {
                rmdb::build_index_key_into(indexes[i], slot, scan.rid(), &keys[i]);
                (*all_entries)[i].append(keys[i].data(), scan.rid());
            }
            return true;
        });
    }
}

void collect_index_entries(RmFileHandle *fh, const IndexMeta &index,
                           IxBulkLoadEntries *entries) {
    std::vector<IndexMeta> indexes{index};
    std::vector<IxBulkLoadEntries> all_entries;
    collect_table_index_entries(fh, indexes, &all_entries);
    *entries = std::move(all_entries.front());
}

}  // namespace

/**
 * @description: 判断是否为一个文件夹
 * @return {bool} 返回是否为一个文件夹
 * @param {string&} db_name 数据库文件名称，与文件夹同名
 */
bool SmManager::is_dir(const std::string& db_name) {
    struct stat st;
    return stat(db_name.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

/**
 * @description: 创建数据库，所有的数据库相关文件都放在数据库同名文件夹下
 * @param {string&} db_name 数据库名称
 */
void SmManager::create_db(const std::string& db_name) {
    if (is_dir(db_name)) {
        throw DatabaseExistsError(db_name);
    }
    //为数据库创建一个子目录
    std::string cmd = "mkdir " + db_name;
    if (system(cmd.c_str()) < 0) {  // 创建一个名为db_name的目录
        throw UnixError();
    }
    if (chdir(db_name.c_str()) < 0) {  // 进入名为db_name的目录
        throw UnixError();
    }
    //创建系统目录
    DbMeta new_db;
    new_db.name_ = db_name;

    // 注意，此处ofstream会在当前目录创建(如果没有此文件先创建)和打开一个名为DB_META_NAME的文件
    std::ofstream ofs(DB_META_NAME);

    // 将new_db中的信息，按照定义好的operator<<操作符，写入到ofs打开的DB_META_NAME文件中
    ofs << new_db;  // 注意：此处重载了操作符<<

    // 创建日志文件
    disk_manager_->create_file(LOG_FILE_NAME);
    std::ofstream clean_marker(DB_CLEAN_SHUTDOWN_MARKER, std::ios::trunc);
    clean_marker.close();

    // 回到根目录
    if (chdir("..") < 0) {
        throw UnixError();
    }
}

/**
 * @description: 删除数据库，同时需要清空相关文件以及数据库同名文件夹
 * @param {string&} db_name 数据库名称，与文件夹同名
 */
void SmManager::drop_db(const std::string& db_name) {
    if (!is_dir(db_name)) {
        throw DatabaseNotFoundError(db_name);
    }
    std::string cmd = "rm -r " + db_name;
    if (system(cmd.c_str()) < 0) {
        throw UnixError();
    }
}

/**
 * @description: 打开数据库，找到数据库对应的文件夹，并加载数据库元数据和相关文件
 * @param {string&} db_name 数据库名称，与文件夹同名
 */
void SmManager::open_db(const std::string& db_name) {
    if (!is_dir(db_name)) {
        throw DatabaseNotFoundError(db_name);
    }
    if (chdir(db_name.c_str()) < 0) {
        throw UnixError();
    }
    bool clean_shutdown = disk_manager_->is_file(DB_CLEAN_SHUTDOWN_MARKER);
    recovery_required_ = !clean_shutdown;
    if (clean_shutdown) {
        if (unlink(DB_CLEAN_SHUTDOWN_MARKER.c_str()) < 0) {
            throw UnixError();
        }
    }
    std::ifstream ifs(DB_META_NAME);
    ifs >> db_;
    next_index_id_ = 1;
    for (const auto &table_entry : db_.tabs_) {
        for (const auto &index : table_entry.second.indexes) {
            next_index_id_ = std::max(next_index_id_, index.index_id + 1);
        }
    }
    bool metadata_upgraded = false;
    for (auto &table_entry : db_.tabs_) {
        for (auto &index : table_entry.second.indexes) {
            if (index.index_id == 0) {
                index.index_id = allocate_index_id();
                metadata_upgraded = true;
            }
        }
    }
    for (auto &entry : db_.tabs_) {
        for (auto &index : entry.second.indexes) {
            if (index.index_name.empty()) {
                index.index_name = ix_manager_->get_index_name(entry.first, index.cols);
            }
        }
    }
    // 非正常启动时索引是派生结构：这里只恢复 heap 文件头，WAL 完成后再重建。
    // 正常启动直接打开已经持久化的索引。
    for (auto &entry : db_.tabs_) {
        fhs_.emplace(entry.first, rm_manager_->open_file(entry.first));
    }
    std::vector<std::thread> rebuild_threads;
    std::mutex ihs_mutex;
    std::mutex error_mutex;
    std::vector<std::exception_ptr> rebuild_errors;
    rebuild_threads.reserve(db_.tabs_.size());
    for (auto &entry : db_.tabs_) {
        const std::string table_name = entry.first;
        rebuild_threads.emplace_back([&, table_name] {
            try {
                auto &table = db_.tabs_.at(table_name);
                auto &fh = fhs_.at(table_name);
                if (!clean_shutdown) {
                    fh->rebuild_file_hdr_from_disk();
                    return;
                }
                for (auto &index : table.indexes) {
                    auto ih = ix_manager_->open_index(table_name, index);
                    std::lock_guard<std::mutex> ihs_guard(ihs_mutex);
                    ihs_.emplace(index.index_name, std::move(ih));
                }
            } catch (...) {
                std::lock_guard<std::mutex> error_guard(error_mutex);
                rebuild_errors.push_back(std::current_exception());
            }
        });
    }
    for (auto &thread : rebuild_threads) {
        thread.join();
    }
    if (!rebuild_errors.empty()) {
        std::rethrow_exception(rebuild_errors.front());
    }
    if (metadata_upgraded) {
        flush_meta();
    }
}

void SmManager::rebuild_indexes_after_recovery() {
    if (!recovery_required_) {
        return;
    }
    if (!ihs_.empty()) {
        throw InternalError("recovery index rebuild requires an empty index handle map");
    }

    std::vector<std::thread> rebuild_threads;
    std::mutex ihs_mutex;
    std::mutex log_mutex;
    std::mutex error_mutex;
    std::vector<std::exception_ptr> rebuild_errors;
    rebuild_threads.reserve(db_.tabs_.size());
    for (const auto &entry : db_.tabs_) {
        const std::string table_name = entry.first;
        rebuild_threads.emplace_back([&, table_name] {
            try {
                auto &table = db_.tabs_.at(table_name);
                auto &fh = fhs_.at(table_name);
                if (table.indexes.empty()) {
                    return;
                }
                std::vector<std::unique_ptr<IxIndexHandle>> handles;
                handles.reserve(table.indexes.size());
                for (const auto &index : table.indexes) {
                    const std::string index_file = ix_manager_->get_index_name(table_name, index);
                    if (disk_manager_->is_file(index_file)) {
                        ix_manager_->destroy_index(table_name, index);
                    }
                    ix_manager_->create_index(table_name, index);
                    handles.push_back(ix_manager_->open_index(table_name, index));
                }
                auto build_started = std::chrono::steady_clock::now();
                std::vector<IxBulkLoadEntries> all_entries;
                collect_table_index_entries(fh.get(), table.indexes, &all_entries);
                auto collected = std::chrono::steady_clock::now();
                for (size_t i = 0; i < table.indexes.size(); ++i) {
                    auto &index = table.indexes[i];
                    handles[i]->bulk_load(all_entries[i], index.unique);
                    auto built = std::chrono::steady_clock::now();
                    auto elapsed_ms = [](auto begin, auto end) {
                        return std::chrono::duration<double, std::milli>(end - begin).count();
                    };
                    {
                        std::lock_guard<std::mutex> log_guard(log_mutex);
                        std::cerr << "Recovery index rebuild: " << table_name << "."
                                  << index.index_name << " entries=" << all_entries[i].size()
                                  << " collect_ms=" << elapsed_ms(build_started, collected)
                                  << " bulk_ms=" << elapsed_ms(collected, built) << "\n";
                    }
                    std::lock_guard<std::mutex> ihs_guard(ihs_mutex);
                    ihs_.emplace(index.index_name, std::move(handles[i]));
                }
            } catch (...) {
                std::lock_guard<std::mutex> error_guard(error_mutex);
                rebuild_errors.push_back(std::current_exception());
            }
        });
    }
    for (auto &thread : rebuild_threads) {
        thread.join();
    }
    if (!rebuild_errors.empty()) {
        std::rethrow_exception(rebuild_errors.front());
    }
}

/**
 * @description: 把数据库相关的元数据刷入磁盘中
 */
void SmManager::flush_meta() {
    // 默认清空文件
    std::ofstream ofs(DB_META_NAME);
    ofs << db_;
}

/**
 * @description: 关闭数据库并把数据落盘
 */
void SmManager::close_db() {
    for (auto &entry : ihs_) {
        ix_manager_->close_index(entry.second.get());
    }
    ihs_.clear();
    for (auto &entry : fhs_) {
        rm_manager_->close_file(entry.second.get());
    }
    fhs_.clear();
    flush_meta();
    int meta_fd = disk_manager_->get_file_fd(DB_META_NAME);
    disk_manager_->sync_file(meta_fd);
    disk_manager_->close_file(meta_fd);
    // Keep absolute WAL offsets across clean restarts. Resetting db.log to offset
    // zero while heap pages retain pageLSN would break the ARIES redo comparison.
    // Safe online reclamation is performed at whole-segment boundaries only.
    std::ofstream clean_marker(DB_CLEAN_SHUTDOWN_MARKER, std::ios::trunc);
    clean_marker.close();
    int marker_fd = disk_manager_->get_file_fd(DB_CLEAN_SHUTDOWN_MARKER);
    disk_manager_->sync_file(marker_fd);
    disk_manager_->close_file(marker_fd);
    if (chdir("..") < 0) {
        throw UnixError();
    }
}

/**
 * @description: 显示所有的表
 * @param {Context*} context 
 */
void SmManager::show_tables(Context* context) {
    RecordPrinter printer(1);
    printer.print_separator(context);
    printer.print_record({"Tables"}, context);
    printer.print_separator(context);
    for (auto &entry : db_.tabs_) {
        auto &tab = entry.second;
        printer.print_record({tab.name}, context);
    }
    printer.print_separator(context);
}

/**
 * @description: 显示表的元数据
 * @param {string&} tab_name 表名称
 * @param {Context*} context 
 */
void SmManager::desc_table(const std::string& tab_name, Context* context) {
    TabMeta &tab = db_.get_table(tab_name);

    std::vector<std::string> captions = {"Field", "Type", "Index"};
    RecordPrinter printer(captions.size());
    // Print header
    printer.print_separator(context);
    printer.print_record(captions, context);
    printer.print_separator(context);
    // Print fields
    for (auto &col : tab.cols) {
        std::vector<std::string> field_info = {col.name, coltype2str(col.type), col.index ? "YES" : "NO"};
        printer.print_record(field_info, context);
    }
    // Print footer
    printer.print_separator(context);
}

void SmManager::show_index(const std::string& tab_name, Context* context) {
    TabMeta &tab = db_.get_table(tab_name);
    for (auto &index : tab.indexes) {
        if (index.hidden) {
            continue;
        }
        std::string output = "| " + tab_name + " | " + (index.unique ? "unique" : "non_unique") + " | (";
        for (int i = 0; i < index.col_num; ++i) {
            if (i > 0) {
                output += ",";
            }
            output += index.cols[i].name;
        }
        output += ") |\n";
        if (context != nullptr && context->data_send_ != nullptr && context->offset_ != nullptr &&
            *context->offset_ >= 0 && *context->offset_ < BUFFER_LENGTH &&
            output.size() < static_cast<size_t>(BUFFER_LENGTH - *context->offset_)) {
            std::memcpy(context->data_send_ + *context->offset_, output.data(), output.size());
            *context->offset_ += static_cast<int>(output.size());
        }
    }
}

/**
 * @description: 创建表
 * @param {string&} tab_name 表的名称
 * @param {vector<ColDef>&} col_defs 表的字段
 * @param {Context*} context 
 */
void SmManager::create_table(const std::string& tab_name, const std::vector<ColDef>& col_defs, Context* context) {
    if (db_.is_table(tab_name)) {
        throw TableExistsError(tab_name);
    }
    if (context != nullptr && context->txn_mgr_ != nullptr) {
        context->txn_mgr_->EraseTableState(tab_name);
    }
    // Create table meta
    int curr_offset = 0;
    TabMeta tab;
    tab.name = tab_name;
    for (auto &col_def : col_defs) {
        ColMeta col = {.tab_name = tab_name,
                       .name = col_def.name,
                       .type = col_def.type,
                       .len = col_def.len,
                       .offset = curr_offset,
                       .index = false};
        curr_offset += col_def.len;
        tab.cols.push_back(col);
    }
    // Create & open record file
    int record_size = curr_offset;  // record_size就是col meta所占的大小（表的元数据也是以记录的形式进行存储的）
    rm_manager_->create_file(tab_name, record_size);
    db_.tabs_[tab_name] = tab;
    // fhs_[tab_name] = rm_manager_->open_file(tab_name);
    fhs_.emplace(tab_name, rm_manager_->open_file(tab_name));

    flush_meta();
}

/**
 * @description: 删除表
 * @param {string&} tab_name 表的名称
 * @param {Context*} context
 */
void SmManager::drop_table(const std::string& tab_name, Context* context) {
    if (!db_.is_table(tab_name)) {
        throw TableNotFoundError(tab_name);
    }
    auto &tab = db_.get_table(tab_name);
    for (auto &index : tab.indexes) {
        std::string ix_name = ix_manager_->get_index_name(tab_name, index);
        if (ihs_.count(ix_name)) {
            ix_manager_->close_index(ihs_.at(ix_name).get());
            ihs_.erase(ix_name);
        }
        ix_manager_->destroy_index(tab_name, index);
    }
    if (fhs_.count(tab_name)) {
        rm_manager_->close_file(fhs_.at(tab_name).get());
        fhs_.erase(tab_name);
    }
    rm_manager_->destroy_file(tab_name);
    db_.tabs_.erase(tab_name);
    flush_meta();
    if (context != nullptr && context->txn_mgr_ != nullptr) {
        context->txn_mgr_->EraseTableState(tab_name);
    }
}

/**
 * @description: 创建索引
 * @param {string&} tab_name 表的名称
 * @param {vector<string>&} col_names 索引包含的字段名称
 * @param {Context*} context
 */
void SmManager::create_index(const std::string& tab_name, const std::vector<std::string>& col_names, Context* context) {
    if (!db_.is_table(tab_name)) {
        throw TableNotFoundError(tab_name);
    }
    TabMeta &tab = db_.get_table(tab_name);
    if (tab.is_index(col_names)) {
        throw IndexExistsError(tab_name, col_names);
    }
    std::vector<ColMeta> index_cols;
    int col_tot_len = 0;
    for (auto &col_name : col_names) {
        auto col = tab.get_col(col_name);
        index_cols.push_back(*col);
        col_tot_len += col->len;
        col->index = true;
    }
    std::unique_ptr<IxIndexHandle> ih;
    bool index_file_created = false;
    IndexMeta index_meta;
    index_meta.tab_name = tab_name;
    index_meta.index_name = ix_manager_->get_index_name(tab_name, index_cols);
    index_meta.index_id = allocate_index_id();
    index_meta.col_tot_len = col_tot_len;
    index_meta.col_num = static_cast<int>(index_cols.size());
    index_meta.cols = index_cols;
    index_meta.unique = true;
    index_meta.hidden = false;
    try {
        ix_manager_->create_index(tab_name, index_meta);
        index_file_created = true;
        ih = ix_manager_->open_index(tab_name, index_meta);

        // Bulk-load: collect, sort, build tree bottom-up
        IxBulkLoadEntries entries(static_cast<size_t>(index_meta.col_tot_len));
        collect_index_entries(fhs_.at(tab_name).get(), index_meta, &entries);
        ih->bulk_load(entries, index_meta.unique);

        tab.indexes.push_back(index_meta);
        ihs_.emplace(ix_manager_->get_index_name(tab_name, index_meta), std::move(ih));
        flush_meta();
    } catch (...) {
        if (ih != nullptr) {
            ix_manager_->close_index(ih.get());
            ih.reset();
        }
        for (auto &col_name : col_names) {
            tab.get_col(col_name)->index = false;
        }
        if (index_file_created && ix_manager_->exists(tab_name, index_meta)) {
            ix_manager_->destroy_index(tab_name, index_meta);
        }
        throw;
    }
}

/**
 * @description: 删除索引
 * @param {string&} tab_name 表名称
 * @param {vector<string>&} col_names 索引包含的字段名称
 * @param {Context*} context
 */
void SmManager::drop_index(const std::string& tab_name, const std::vector<std::string>& col_names, Context* context) {
    if (!db_.is_table(tab_name)) {
        throw TableNotFoundError(tab_name);
    }
    TabMeta &tab = db_.get_table(tab_name);
    auto index_it = tab.get_index_meta(col_names);
    for (auto &col_meta : index_it->cols) {
        tab.get_col(col_meta.name)->index = false;
    }
    std::string ix_name = ix_manager_->get_index_name(tab_name, *index_it);
    IndexMeta dropped_index = *index_it;
    if (ihs_.count(ix_name)) {
        ix_manager_->close_index(ihs_.at(ix_name).get());
        ihs_.erase(ix_name);
    }
    ix_manager_->destroy_index(tab_name, *index_it);
    tab.indexes.erase(index_it);
    flush_meta();
    if (context != nullptr && context->txn_mgr_ != nullptr) {
        context->txn_mgr_->EraseSnapshotIndexHistory(tab_name, dropped_index);
    }
}
