/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "storage/disk_manager.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <limits>
#include <set>
#include <sstream>
#include <iomanip>
#include <string.h>    // for memset
#include <sys/stat.h>  // for stat
#include <unistd.h>

#include "defs.h"

namespace {
void write_all_at(int fd, const char *data, size_t num_bytes, off_t offset, const char *context) {
    size_t bytes_written = 0;
    while (bytes_written < num_bytes) {
        ssize_t result = pwrite(fd, data + bytes_written, num_bytes - bytes_written,
                                offset + static_cast<off_t>(bytes_written));
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw UnixError();
        }
        if (result == 0) {
            throw InternalError(context);
        }
        bytes_written += static_cast<size_t>(result);
    }
}

size_t read_at_most(int fd, char *data, size_t num_bytes, off_t offset) {
    size_t bytes_read = 0;
    while (bytes_read < num_bytes) {
        ssize_t result = pread(fd, data + bytes_read, num_bytes - bytes_read,
                               offset + static_cast<off_t>(bytes_read));
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw UnixError();
        }
        if (result == 0) {
            break;
        }
        bytes_read += static_cast<size_t>(result);
    }
    return bytes_read;
}

void sync_fd(int fd) {
    while (fdatasync(fd) < 0) {
        if (errno == EINTR) {
            continue;
        }
        throw UnixError();
    }
}

void truncate_fd(int fd, off_t size) {
    while (ftruncate(fd, size) < 0) {
        if (errno == EINTR) {
            continue;
        }
        throw UnixError();
    }
}
}  // namespace

DiskManager::DiskManager() {
    for (auto &next_page_no : fd2pageno_) {
        next_page_no.store(0, std::memory_order_relaxed);
    }
}

DiskManager::~DiskManager() {
    std::lock_guard<std::mutex> wal_lock(wal_mutex_);
    for (const auto &[segment_id, fd] : wal_segment_fds_) {
        (void)segment_id;
        close(fd);
    }
    wal_segment_fds_.clear();
    if (log_fd_ >= 0) {
        close(log_fd_);
        log_fd_ = -1;
    }
}

/**
 * @description: 将数据写入文件的指定磁盘页面中
 * @param {int} fd 磁盘文件的文件句柄
 * @param {page_id_t} page_no 写入目标页面的page_id
 * @param {char} *offset 要写入磁盘的数据
 * @param {int} num_bytes 要写入磁盘的数据大小
 */
void DiskManager::write_page(int fd, page_id_t page_no, const char *offset, int num_bytes) {
    off_t file_offset = static_cast<off_t>(page_no) * PAGE_SIZE;
    write_all_at(fd, offset, num_bytes, file_offset, "DiskManager::write_page Error");
}

void DiskManager::write_pages_batch(int fd, page_id_t first_page_no, const char *data, size_t num_pages) {
    if (num_pages == 0) {
        return;
    }
    const size_t num_bytes = num_pages * static_cast<size_t>(PAGE_SIZE);
    off_t file_offset = static_cast<off_t>(first_page_no) * PAGE_SIZE;
    write_all_at(fd, data, num_bytes, file_offset, "DiskManager::write_pages_batch Error");
}

/**
 * @description: 读取文件中指定编号的页面中的部分数据到内存中
 * @param {int} fd 磁盘文件的文件句柄
 * @param {page_id_t} page_no 指定的页面编号
 * @param {char} *offset 读取的内容写入到offset中
 * @param {int} num_bytes 读取的数据量大小
 */
void DiskManager::read_page(int fd, page_id_t page_no, char *offset, int num_bytes) {
    off_t file_offset = static_cast<off_t>(page_no) * PAGE_SIZE;
    size_t bytes_read = read_at_most(fd, offset, static_cast<size_t>(num_bytes), file_offset);
    if (bytes_read < static_cast<size_t>(num_bytes)) {
        memset(offset + bytes_read, 0, static_cast<size_t>(num_bytes) - bytes_read);
    }
}

size_t DiskManager::read_file_range(int fd, rmdb::i64 offset, char *data, size_t num_bytes) {
    if (offset < 0) {
        throw InternalError("DiskManager::read_file_range negative offset");
    }
    size_t bytes_read = read_at_most(fd, data, num_bytes, static_cast<off_t>(offset));
    return bytes_read;
}

void DiskManager::write_file_range(int fd, rmdb::i64 offset, const char *data, size_t num_bytes) {
    if (offset < 0) {
        throw InternalError("DiskManager::write_file_range negative offset");
    }
    write_all_at(fd, data, num_bytes, static_cast<off_t>(offset),
                 "DiskManager::write_file_range Error");
}

/**
 * @description: 分配一个新的页号
 * @return {page_id_t} 分配的新页号
 * @param {int} fd 指定文件的文件句柄
 */
page_id_t DiskManager::allocate_page(int fd) {
    // 简单的自增分配策略，指定文件的页面编号加1
    if (fd < 0 || fd >= MAX_FD) {
        throw FileNotOpenError(fd);
    }
    return fd2pageno_[fd]++;
}

void DiskManager::deallocate_page(__attribute__((unused)) page_id_t page_id) {}

bool DiskManager::is_dir(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

void DiskManager::create_dir(const std::string &path) {
    // Create a subdirectory
    std::string cmd = "mkdir " + path;
    if (system(cmd.c_str()) < 0) {  // 创建一个名为path的目录
        throw UnixError();
    }
}

void DiskManager::destroy_dir(const std::string &path) {
    std::string cmd = "rm -r " + path;
    if (system(cmd.c_str()) < 0) {
        throw UnixError();
    }
}

/**
 * @description: 判断指定路径文件是否存在
 * @return {bool} 若指定路径文件存在则返回true
 * @param {string} &path 指定路径文件
 */
bool DiskManager::is_file(const std::string &path) {
    // 用struct stat获取文件信息
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

/**
 * @description: 用于创建指定路径文件
 * @return {*}
 * @param {string} &path
 */
void DiskManager::create_file(const std::string &path) {
    if (is_file(path)) {
        throw FileExistsError(path);
    }
    int fd = open(path.c_str(), O_CREAT | O_EXCL | O_RDWR, 0644);
    if (fd == -1) {
        throw UnixError();
    }
    if (close(fd) == -1) {
        throw UnixError();
    }
}

/**
 * @description: 删除指定路径的文件
 * @param {string} &path 文件所在路径
 */
void DiskManager::destroy_file(const std::string &path) {
    if (!is_file(path)) {
        throw FileNotFoundError(path);
    }
    if (path2fd_.count(path)) {
        throw FileNotClosedError(path);
    }
    if (unlink(path.c_str()) == -1) {
        throw UnixError();
    }
}


/**
 * @description: 打开指定路径文件
 * @return {int} 返回打开的文件的文件句柄
 * @param {string} &path 文件所在路径
 */
int DiskManager::open_file(const std::string &path) {
    if (!is_file(path)) {
        throw FileNotFoundError(path);
    }
    int fd = open(path.c_str(), O_RDWR);
    if (fd == -1) {
        throw UnixError();
    }
    if (fd >= MAX_FD) {
        close(fd);
        throw InternalError("DiskManager file descriptor exceeds page allocator capacity");
    }
    {
        std::lock_guard<std::mutex> lock(file_map_mutex_);
        if (path2fd_.count(path)) {
            close(fd);
            throw FileNotClosedError(path);
        }
        path2fd_[path] = fd;
        fd2path_[fd] = path;
        fd2pageno_[fd] = static_cast<page_id_t>((get_file_size(path) + PAGE_SIZE - 1) / PAGE_SIZE);
    }
    return fd;
}

/**
 * @description:用于关闭指定路径文件
 * @param {int} fd 打开的文件的文件句柄
 */
void DiskManager::close_file(int fd) {
    std::string path;
    {
        std::lock_guard<std::mutex> wal_lock(wal_mutex_);
        std::lock_guard<std::mutex> lock(file_map_mutex_);
        auto it = fd2path_.find(fd);
        if (it == fd2path_.end()) {
            throw FileNotOpenError(fd);
        }
        path = it->second;
        fd2path_.erase(it);
        path2fd_.erase(path);
        if (log_fd_ == fd) {
            log_fd_ = -1;
            for (const auto &[segment_id, segment_fd] : wal_segment_fds_) {
                (void)segment_id;
                close(segment_fd);
            }
            wal_segment_fds_.clear();
            wal_segments_discovered_ = false;
            wal_first_lsn_ = 0;
            wal_end_lsn_ = 0;
            dirty_wal_segments_.clear();
        }
    }
    if (close(fd) == -1) {
        throw UnixError();
    }
}


/**
 * @description: 获得文件的大小
 * @return {int} 文件的大小
 * @param {string} &file_name 文件名
 */
rmdb::i64 DiskManager::get_file_size(const std::string &file_name) {
    struct stat stat_buf;
    int rc = stat(file_name.c_str(), &stat_buf);
    return rc == 0 ? stat_buf.st_size : -1;
}

/**
 * @description: 根据文件句柄获得文件名
 * @return {string} 文件句柄对应文件的文件名
 * @param {int} fd 文件句柄
 */
std::string DiskManager::get_file_name(int fd) {
    std::lock_guard<std::mutex> lock(file_map_mutex_);
    auto it = fd2path_.find(fd);
    if (it == fd2path_.end()) {
        throw FileNotOpenError(fd);
    }
    return it->second;
}

/**
 * @description:  获得文件名对应的文件句柄
 * @return {int} 文件句柄
 * @param {string} &file_name 文件名
 */
int DiskManager::get_file_fd(const std::string &file_name) {
    {
        std::lock_guard<std::mutex> lock(file_map_mutex_);
        auto it = path2fd_.find(file_name);
        if (it != path2fd_.end()) {
            return it->second;
        }
    }
    return open_file(file_name);
}


/**
 * @description:  读取日志文件内容
 * @return {int} 返回读取的数据量，若为-1说明读取数据的起始位置超过了文件大小
 * @param {char} *log_data 读取内容到log_data中
 * @param {int} size 读取的数据量大小
 * @param {int} offset 读取的内容在文件中的位置
 */
size_t DiskManager::read_log(char *log_data, size_t size, lsn_t offset) {
    if (log_data == nullptr || offset < 0 || size == 0) {
        return 0;
    }
    std::lock_guard<std::mutex> wal_lock(wal_mutex_);
    discover_log_segments_locked();
    if (offset >= wal_end_lsn_) {
        return 0;
    }
    const size_t requested = std::min<size_t>(
        size, static_cast<size_t>(wal_end_lsn_ - offset));
    size_t bytes_read = 0;
    while (bytes_read < requested) {
        const lsn_t absolute = offset + static_cast<lsn_t>(bytes_read);
        const rmdb::u64 segment_id = static_cast<rmdb::u64>(absolute / WAL_SEGMENT_SIZE);
        const lsn_t segment_offset = absolute % WAL_SEGMENT_SIZE;
        const size_t chunk = std::min<size_t>(
            requested - bytes_read,
            static_cast<size_t>(WAL_SEGMENT_SIZE - segment_offset));
        const int fd = ensure_log_segment_fd(segment_id, false);
        const size_t got = read_at_most(fd, log_data + bytes_read, chunk,
                                        static_cast<off_t>(segment_offset));
        if (got != chunk) {
            throw InternalError("DiskManager::read_log incomplete segment read");
        }
        bytes_read += got;
    }
    return bytes_read;
}


/**
 * @description: 写日志内容
 * @param {char} *log_data 要写入的日志内容
 * @param {int} size 要写入的内容大小
 */
void DiskManager::write_log(const char *log_data, size_t size, lsn_t offset) {
    if (log_data == nullptr || offset < 0) {
        throw InternalError("DiskManager::write_log negative offset");
    }
    if (size == 0) {
        return;
    }
    std::lock_guard<std::mutex> wal_lock(wal_mutex_);
    discover_log_segments_locked();
    if (offset > wal_end_lsn_) {
        throw InternalError("DiskManager::write_log would create a WAL gap");
    }
    size_t bytes_written = 0;
    while (bytes_written < size) {
        const lsn_t absolute = offset + static_cast<lsn_t>(bytes_written);
        const rmdb::u64 segment_id = static_cast<rmdb::u64>(absolute / WAL_SEGMENT_SIZE);
        const lsn_t segment_offset = absolute % WAL_SEGMENT_SIZE;
        const size_t chunk = std::min<size_t>(
            size - bytes_written,
            static_cast<size_t>(WAL_SEGMENT_SIZE - segment_offset));
        const int fd = ensure_log_segment_fd(segment_id, true);
        write_all_at(fd, log_data + bytes_written, chunk, static_cast<off_t>(segment_offset),
                     "DiskManager::write_log Error");
        dirty_wal_segments_.insert(segment_id);
        bytes_written += chunk;
    }
    wal_end_lsn_ = std::max(wal_end_lsn_, offset + static_cast<lsn_t>(size));
}

std::string DiskManager::log_segment_path(rmdb::u64 segment_id) const {
    if (segment_id == 0) {
        return LOG_FILE_NAME;
    }
    constexpr auto kMaxLsn = static_cast<rmdb::u64>(std::numeric_limits<lsn_t>::max());
    if (segment_id > kMaxLsn / static_cast<rmdb::u64>(WAL_SEGMENT_SIZE)) {
        throw InternalError("WAL segment id exceeds the absolute LSN range");
    }
    std::ostringstream name;
    name << LOG_FILE_NAME << '.' << std::hex << std::setw(16) << std::setfill('0')
         << segment_id * static_cast<rmdb::u64>(WAL_SEGMENT_SIZE);
    return name.str();
}

int DiskManager::ensure_log_segment_fd(rmdb::u64 segment_id, bool create) {
    if (segment_id == 0) {
        if (log_fd_ == -1) {
            if (!is_file(LOG_FILE_NAME)) {
                if (!create) {
                    throw FileNotFoundError(LOG_FILE_NAME);
                }
                create_file(LOG_FILE_NAME);
                wal_directory_sync_pending_ = true;
            }
            log_fd_ = get_file_fd(LOG_FILE_NAME);
        }
        return log_fd_;
    }
    auto cached = wal_segment_fds_.find(segment_id);
    if (cached != wal_segment_fds_.end()) {
        return cached->second;
    }
    const std::string path = log_segment_path(segment_id);
    int fd = open(path.c_str(), O_RDWR);
    if (fd < 0 && errno == ENOENT && create) {
        fd = open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0644);
        if (fd >= 0) {
            wal_directory_sync_pending_ = true;
        } else if (errno == EEXIST) {
            fd = open(path.c_str(), O_RDWR);
        }
    }
    if (fd < 0) {
        if (errno == ENOENT) {
            throw FileNotFoundError(path);
        }
        throw UnixError();
    }
    wal_segment_fds_.emplace(segment_id, fd);
    return fd;
}

void DiskManager::discover_log_segments_locked() {
    if (wal_segments_discovered_) {
        return;
    }
    std::set<rmdb::u64> segment_ids;
    if (is_file(LOG_FILE_NAME)) {
        segment_ids.insert(0);
    }
    DIR *directory = opendir(".");
    if (directory == nullptr) {
        throw UnixError();
    }
    const std::string prefix = LOG_FILE_NAME + ".";
    errno = 0;
    while (dirent *entry = readdir(directory)) {
        const std::string name = entry->d_name;
        if (name.rfind(prefix, 0) != 0 || name.size() != prefix.size() + 16) {
            continue;
        }
        const char *suffix = name.c_str() + prefix.size();
        char *end = nullptr;
        errno = 0;
        const unsigned long long start = std::strtoull(suffix, &end, 16);
        if (errno != 0 || end == suffix || *end != '\0' ||
            start > static_cast<unsigned long long>(std::numeric_limits<lsn_t>::max()) ||
            start % static_cast<unsigned long long>(WAL_SEGMENT_SIZE) != 0) {
            closedir(directory);
            throw InternalError("invalid WAL segment name: " + name);
        }
        segment_ids.insert(static_cast<rmdb::u64>(
            start / static_cast<unsigned long long>(WAL_SEGMENT_SIZE)));
    }
    if (errno != 0) {
        const int saved_errno = errno;
        closedir(directory);
        errno = saved_errno;
        throw UnixError();
    }
    closedir(directory);

    if (segment_ids.empty()) {
        wal_first_lsn_ = 0;
        wal_end_lsn_ = 0;
        wal_segments_discovered_ = true;
        return;
    }
    if (*segment_ids.begin() != 0) {
        throw InternalError("WAL segment zero is missing");
    }
    const bool has_reclaimed_gap = segment_ids.size() > 1 && *std::next(segment_ids.begin()) != 1;
    rmdb::u64 expected = 0;
    size_t position = 0;
    wal_first_lsn_ = has_reclaimed_gap
                         ? static_cast<lsn_t>(*std::next(segment_ids.begin())) * WAL_SEGMENT_SIZE
                         : 0;
    for (rmdb::u64 segment_id : segment_ids) {
        if (has_reclaimed_gap && position == 1) {
            expected = segment_id;
        }
        if (segment_id != expected) {
            throw InternalError("WAL segment sequence has a gap");
        }
        const rmdb::i64 size = get_file_size(log_segment_path(segment_id));
        if (size < 0 || size > WAL_SEGMENT_SIZE) {
            throw InternalError("invalid WAL segment size");
        }
        const bool is_last = segment_id == *segment_ids.rbegin();
        if (!is_last && size != WAL_SEGMENT_SIZE) {
            throw InternalError("non-final WAL segment is not full");
        }
        wal_end_lsn_ = static_cast<lsn_t>(segment_id) * WAL_SEGMENT_SIZE + size;
        ++expected;
        ++position;
    }
    wal_segments_discovered_ = true;
}

lsn_t DiskManager::get_log_end_lsn() {
    std::lock_guard<std::mutex> wal_lock(wal_mutex_);
    discover_log_segments_locked();
    return wal_end_lsn_;
}

lsn_t DiskManager::get_first_log_lsn() {
    std::lock_guard<std::mutex> wal_lock(wal_mutex_);
    discover_log_segments_locked();
    return wal_first_lsn_;
}

void DiskManager::sync_log_directory_locked() {
    int dir_fd = open(".", O_RDONLY | O_DIRECTORY);
    if (dir_fd < 0) {
        throw UnixError();
    }
    while (fsync(dir_fd) < 0) {
        if (errno == EINTR) {
            continue;
        }
        close(dir_fd);
        throw UnixError();
    }
    if (close(dir_fd) < 0) {
        throw UnixError();
    }
}

void DiskManager::sync_file(int fd) {
    if (fd < 0) {
        throw InternalError("DiskManager::sync_file invalid file descriptor");
    }
    sync_fd(fd);
}

void DiskManager::sync_log_file() {
    std::lock_guard<std::mutex> wal_lock(wal_mutex_);
    discover_log_segments_locked();
    const rmdb::u64 tail_segment = wal_end_lsn_ == 0
                                       ? 0
                                       : static_cast<rmdb::u64>((wal_end_lsn_ - 1) / WAL_SEGMENT_SIZE);
    sync_fd(ensure_log_segment_fd(tail_segment, true));
}

void DiskManager::sync_log() {
    std::lock_guard<std::mutex> wal_lock(wal_mutex_);
    discover_log_segments_locked();
    if (dirty_wal_segments_.empty()) {
        const rmdb::u64 tail_segment = wal_end_lsn_ == 0
                                           ? 0
                                           : static_cast<rmdb::u64>((wal_end_lsn_ - 1) / WAL_SEGMENT_SIZE);
        sync_fd(ensure_log_segment_fd(tail_segment, true));
    } else {
        for (rmdb::u64 segment_id : dirty_wal_segments_) {
            sync_fd(ensure_log_segment_fd(segment_id, false));
        }
    }
    if (first_sync_pending_ || wal_directory_sync_pending_) {
        // A newly created WAL segment needs its directory entry persisted before
        // the first acknowledgement. Use fsync for the directory, not fdatasync.
        sync_log_directory_locked();
        first_sync_pending_ = false;
        wal_directory_sync_pending_ = false;
    }
    dirty_wal_segments_.clear();
}

void DiskManager::sync_all_data_files() {
    // 拷贝 fd 列表后在锁外逐个 fsync,避免遍历期间阻塞并发 open_file。
    std::vector<int> fds;
    {
        std::lock_guard<std::mutex> lock(file_map_mutex_);
        fds.reserve(fd2path_.size());
        for (const auto &entry : fd2path_) {
            if (entry.first == log_fd_ || entry.second == LOG_FILE_NAME) {
                continue;
            }
            fds.push_back(entry.first);
        }
    }
    for (int fd : fds) {
        sync_fd(fd);
    }
}

void DiskManager::truncate_file(int fd, rmdb::i64 size) {
    if (!fd2path_.count(fd)) {
        throw FileNotOpenError(fd);
    }
    if (size < 0) {
        throw InternalError("negative file truncate size");
    }
    truncate_fd(fd, static_cast<off_t>(size));
    fd2pageno_[fd] = static_cast<page_id_t>((size + PAGE_SIZE - 1) / PAGE_SIZE);
}

void DiskManager::truncate_log() {
    truncate_log_tail(0);
}

void DiskManager::truncate_log_tail(lsn_t end_lsn) {
    std::lock_guard<std::mutex> wal_lock(wal_mutex_);
    discover_log_segments_locked();
    if (end_lsn < 0 || end_lsn > wal_end_lsn_ ||
        (end_lsn != 0 && end_lsn < wal_first_lsn_)) {
        throw InternalError("invalid WAL tail truncate position");
    }
    const rmdb::u64 last_segment = end_lsn == 0
                                       ? 0
                                       : static_cast<rmdb::u64>((end_lsn - 1) / WAL_SEGMENT_SIZE);
    const lsn_t last_size = end_lsn == 0
                                ? 0
                                : end_lsn - static_cast<lsn_t>(last_segment) * WAL_SEGMENT_SIZE;
    const rmdb::u64 old_last_segment = wal_end_lsn_ == 0
                                           ? 0
                                           : static_cast<rmdb::u64>((wal_end_lsn_ - 1) / WAL_SEGMENT_SIZE);
    truncate_fd(ensure_log_segment_fd(last_segment, true), static_cast<off_t>(last_size));
    dirty_wal_segments_.insert(last_segment);
    for (rmdb::u64 segment_id = last_segment + 1; segment_id <= old_last_segment; ++segment_id) {
        auto cached = wal_segment_fds_.find(segment_id);
        if (cached != wal_segment_fds_.end()) {
            close(cached->second);
            wal_segment_fds_.erase(cached);
        }
        const std::string path = log_segment_path(segment_id);
        while (unlink(path.c_str()) < 0) {
            if (errno == EINTR) continue;
            if (errno == ENOENT) break;
            throw UnixError();
        }
        wal_directory_sync_pending_ = true;
        dirty_wal_segments_.erase(segment_id);
        if (segment_id == old_last_segment) {
            break;
        }
    }
    wal_end_lsn_ = end_lsn;
    if (end_lsn == 0) {
        // A full reset leaves only the empty legacy anchor.  Do not retain the
        // previous reclaimed-suffix boundary in the in-memory namespace.
        wal_first_lsn_ = 0;
    }
}

void DiskManager::reclaim_log_segments_before(lsn_t first_retained_lsn) {
    std::lock_guard<std::mutex> wal_lock(wal_mutex_);
    discover_log_segments_locked();
    if (first_retained_lsn < 0 || first_retained_lsn > wal_end_lsn_ ||
        first_retained_lsn % WAL_SEGMENT_SIZE != 0) {
        throw InternalError("invalid WAL reclaim boundary");
    }
    if (first_retained_lsn <= wal_first_lsn_) {
        return;
    }
    const rmdb::u64 first_retained_segment =
        static_cast<rmdb::u64>(first_retained_lsn / WAL_SEGMENT_SIZE);
    if (first_retained_segment <= 1) {
        return;
    }
    bool removed_any = false;
    try {
        for (rmdb::u64 segment_id = 1; segment_id < first_retained_segment; ++segment_id) {
            auto cached = wal_segment_fds_.find(segment_id);
            if (cached != wal_segment_fds_.end()) {
                close(cached->second);
                wal_segment_fds_.erase(cached);
            }
            const std::string path = log_segment_path(segment_id);
            while (unlink(path.c_str()) < 0) {
                if (errno == EINTR) continue;
                if (errno == ENOENT) break;
                throw UnixError();
            }
            dirty_wal_segments_.erase(segment_id);
            wal_first_lsn_ = static_cast<lsn_t>(segment_id + 1) * WAL_SEGMENT_SIZE;
            removed_any = true;
        }
    } catch (...) {
        if (removed_any) {
            sync_log_directory_locked();
        }
        throw;
    }
    if (removed_any) {
        sync_log_directory_locked();
        wal_directory_sync_pending_ = false;
    }
}

void DiskManager::remove_file_if_exists(const std::string &path) {
    while (unlink(path.c_str()) < 0) {
        if (errno == EINTR) {
            continue;
        }
        if (errno == ENOENT) {
            return;
        }
        throw UnixError();
    }
}
