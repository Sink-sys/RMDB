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

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "common/config.h"
#include "common/types.h"
#include "errors.h"

/**
 * @description: DiskManager的作用主要是根据上层的需要对磁盘文件进行操作
 */
class DiskManager {
   public:
    explicit DiskManager();

    ~DiskManager();

    void write_page(int fd, page_id_t page_no, const char *offset, int num_bytes);

    // 批量连续页写回:从 first_page_no 起写 num_pages 个连续 8KB 页,
    // 一次 pwrite 完成。用于脏页写回合并(顺序写替代随机小写,提升磁盘带宽)。
    void write_pages_batch(int fd, page_id_t first_page_no, const char *data, size_t num_pages);

    void read_page(int fd, page_id_t page_no, char *offset, int num_bytes);

    size_t read_file_range(int fd, rmdb::i64 offset, char *data, size_t num_bytes);

    void write_file_range(int fd, rmdb::i64 offset, const char *data, size_t num_bytes);

    page_id_t allocate_page(int fd);

    void deallocate_page(page_id_t page_id);

    /*目录操作*/
    bool is_dir(const std::string &path);

    void create_dir(const std::string &path);

    void destroy_dir(const std::string &path);

    /*文件操作*/
    bool is_file(const std::string &path);

    void create_file(const std::string &path);

    void destroy_file(const std::string &path);

    int open_file(const std::string &path);

    void close_file(int fd);

    rmdb::i64 get_file_size(const std::string &file_name);

    std::string get_file_name(int fd);

    int get_file_fd(const std::string &file_name);

    /*日志操作*/
    size_t read_log(char *log_data, size_t size, lsn_t offset);

    void write_log(const char *log_data, size_t size, lsn_t offset);

    // Logical end of the segmented WAL namespace. LSNs remain absolute byte
    // positions even though bytes are stored in fixed-size files.
    lsn_t get_log_end_lsn();

    // First LSN of the contiguous retained suffix. Segment zero is kept as a
    // legacy anchor and may be followed by a reclaimed gap.
    lsn_t get_first_log_lsn();

    void sync_file(int fd);

    void sync_log();

    /* Sync the current WAL tail even when no new data was appended. After a
       group commit flush, callers use this as the per-ack durability barrier. */
    void sync_log_file();

    void sync_all_data_files();

    void truncate_file(int fd, rmdb::i64 size);

    void truncate_log();

    // Drop an incomplete crash tail without reusing any earlier absolute LSN.
    void truncate_log_tail(lsn_t end_lsn);

    // Remove complete historical segments below an already-durable checkpoint.
    void reclaim_log_segments_before(lsn_t first_retained_lsn);

    void remove_file_if_exists(const std::string &path);

    int GetLogFd() {
        std::lock_guard<std::mutex> wal_lock(wal_mutex_);
        return log_fd_;
    }

    /**
     * @description: 设置文件已经分配的页面个数
     * @param {int} fd 文件对应的文件句柄
     * @param {int} start_page_no 已经分配的页面个数，即文件接下来从start_page_no开始分配页面编号
     */
    void set_fd2pageno(int fd, int start_page_no) { fd2pageno_[fd] = start_page_no; }

    /**
     * @description: 获得文件目前已分配的页面个数，即如果文件要分配一个新页面，需要从fd2pagenp_[fd]开始分配
     * @return {page_id_t} 已分配的页面个数
     * @param {int} fd 文件对应的句柄
     */
    page_id_t get_fd2pageno(int fd) { return fd2pageno_[fd]; }

    static constexpr int MAX_FD = 8192;
    static constexpr lsn_t WAL_SEGMENT_SIZE = 16LL * 1024 * 1024;

   private:
    int ensure_log_segment_fd(rmdb::u64 segment_id, bool create);
    void discover_log_segments_locked();
    std::string log_segment_path(rmdb::u64 segment_id) const;
    void sync_log_directory_locked();

    // 保护 path2fd_ / fd2path_:崩溃恢复的并行索引重建会并发打开文件。
    mutable std::mutex file_map_mutex_;

    // 文件打开列表，用于记录文件是否被打开
    std::unordered_map<std::string, int> path2fd_;  //<Page文件磁盘路径,Page fd>哈希表
    std::unordered_map<int, std::string> fd2path_;  //<Page fd,Page文件磁盘路径>哈希表

    int log_fd_ = -1;                             // WAL日志文件的文件句柄，默认为-1，代表未打开日志文件
    bool first_sync_pending_{true};               // The first sync also persists the WAL directory entry.
    std::mutex wal_mutex_;
    std::unordered_map<rmdb::u64, int> wal_segment_fds_;  // segment 0 uses log_fd_
    std::unordered_set<rmdb::u64> dirty_wal_segments_;
    bool wal_segments_discovered_{false};
    bool wal_directory_sync_pending_{false};
    lsn_t wal_first_lsn_{0};
    lsn_t wal_end_lsn_{0};
    std::atomic<page_id_t> fd2pageno_[MAX_FD]{};  // 文件中已经分配的页面个数，初始值为0
};
