#pragma once

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "common/config.h"
#include "common/types.h"

struct CheckpointMasterRecord {
    rmdb::u64 generation{0};
    lsn_t end_checkpoint_lsn{INVALID_LSN};
    lsn_t first_retained_lsn{0};
};

namespace rmdb::checkpoint_master {

inline constexpr rmdb::u64 kMagic = 0x524d4442434b5054ULL;  // "RMDBCKPT"
inline constexpr rmdb::u32 kVersion = 2;
inline constexpr rmdb::u32 kLegacyVersion = 1;
inline constexpr rmdb::u32 kLegacyRecordSize = 40;
inline constexpr rmdb::u32 kRecordSize = 48;
inline constexpr size_t kLegacyChecksumOffset = 32;
inline constexpr size_t kChecksumOffset = 40;

inline rmdb::u64 Checksum(const char *data, size_t size) {
    rmdb::u64 hash = 1469598103934665603ULL;
    for (size_t i = 0; i < size; ++i) {
        hash ^= static_cast<unsigned char>(data[i]);
        hash *= 1099511628211ULL;
    }
    return hash;
}

inline std::array<char, kRecordSize> Encode(const CheckpointMasterRecord &record) {
    std::array<char, kRecordSize> bytes{};
    memcpy(bytes.data(), &kMagic, sizeof(kMagic));
    memcpy(bytes.data() + 8, &kVersion, sizeof(kVersion));
    memcpy(bytes.data() + 12, &kRecordSize, sizeof(kRecordSize));
    memcpy(bytes.data() + 16, &record.generation, sizeof(record.generation));
    memcpy(bytes.data() + 24, &record.end_checkpoint_lsn, sizeof(record.end_checkpoint_lsn));
    memcpy(bytes.data() + 32, &record.first_retained_lsn, sizeof(record.first_retained_lsn));
    const rmdb::u64 checksum = Checksum(bytes.data(), kChecksumOffset);
    memcpy(bytes.data() + kChecksumOffset, &checksum, sizeof(checksum));
    return bytes;
}

inline bool Decode(const char *data, size_t size, CheckpointMasterRecord *record) {
    if (data == nullptr || record == nullptr ||
        (size != kLegacyRecordSize && size != kRecordSize)) {
        return false;
    }
    rmdb::u64 magic = 0;
    rmdb::u32 version = 0;
    rmdb::u32 record_size = 0;
    memcpy(&magic, data, sizeof(magic));
    memcpy(&version, data + 8, sizeof(version));
    memcpy(&record_size, data + 12, sizeof(record_size));
    memcpy(&record->generation, data + 16, sizeof(record->generation));
    memcpy(&record->end_checkpoint_lsn, data + 24, sizeof(record->end_checkpoint_lsn));
    if (magic != kMagic || record->end_checkpoint_lsn < 0) {
        return false;
    }
    rmdb::u64 checksum = 0;
    if (version == kLegacyVersion && record_size == kLegacyRecordSize &&
        size == kLegacyRecordSize) {
        memcpy(&checksum, data + kLegacyChecksumOffset, sizeof(checksum));
        record->first_retained_lsn = 0;
        return checksum == Checksum(data, kLegacyChecksumOffset);
    }
    if (version != kVersion || record_size != kRecordSize || size != kRecordSize) {
        return false;
    }
    memcpy(&record->first_retained_lsn, data + 32, sizeof(record->first_retained_lsn));
    memcpy(&checksum, data + kChecksumOffset, sizeof(checksum));
    return record->first_retained_lsn >= 0 &&
           checksum == Checksum(data, kChecksumOffset);
}

inline bool Read(CheckpointMasterRecord *record) {
    int fd = open(CHECKPOINT_FILE_NAME.c_str(), O_RDONLY);
    if (fd < 0) {
        return false;
    }
    struct stat stat_buf {};
    if (fstat(fd, &stat_buf) != 0 ||
        (stat_buf.st_size != kLegacyRecordSize && stat_buf.st_size != kRecordSize)) {
        close(fd);
        return false;
    }
    std::array<char, kRecordSize> bytes{};
    const size_t record_size = stat_buf.st_size == kLegacyRecordSize
                                   ? static_cast<size_t>(kLegacyRecordSize)
                                   : static_cast<size_t>(kRecordSize);
    size_t offset = 0;
    while (offset < bytes.size()) {
        ssize_t count = pread(fd, bytes.data() + offset, bytes.size() - offset,
                              static_cast<off_t>(offset));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            close(fd);
            return false;
        }
        if (count == 0) {
            break;
        }
        offset += static_cast<size_t>(count);
    }
    close(fd);
    return offset == record_size && Decode(bytes.data(), record_size, record);
}

inline bool Publish(const CheckpointMasterRecord &record) {
    const auto bytes = Encode(record);
    const std::string temp_path = std::string(CHECKPOINT_FILE_NAME) + ".tmp";
    int fd = open(temp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return false;
    }
    size_t offset = 0;
    while (offset < bytes.size()) {
        ssize_t count = write(fd, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            close(fd);
            unlink(temp_path.c_str());
            return false;
        }
        offset += static_cast<size_t>(count);
    }
    const bool file_synced = fdatasync(fd) == 0;
    const bool file_closed = close(fd) == 0;
    if (!file_synced || !file_closed) {
        unlink(temp_path.c_str());
        return false;
    }
    if (rename(temp_path.c_str(), CHECKPOINT_FILE_NAME.c_str()) != 0) {
        unlink(temp_path.c_str());
        return false;
    }
    int directory_fd = open(".", O_RDONLY | O_DIRECTORY);
    if (directory_fd < 0) {
        return false;
    }
    const bool synced = fsync(directory_fd) == 0;
    close(directory_fd);
    return synced;
}

}  // namespace rmdb::checkpoint_master
