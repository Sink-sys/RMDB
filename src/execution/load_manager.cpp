#include "load_manager.h"

#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

#include "errors.h"
#include "common/index_runtime.h"
#include "common/types.h"
#include "record/rm_defs.h"
#include "record/rm_scan.h"
#include "system/sm_manager.h"

namespace rmdb {
namespace {

class CsvDecodeError : public std::exception {
   public:
    CsvDecodeError(size_t column, std::string reason) : column_(column), reason_(std::move(reason)) {}

    const char *what() const noexcept override { return reason_.c_str(); }
    size_t column() const { return column_; }

   private:
    size_t column_;
    std::string reason_;
};

std::string normalize_load_file_name(std::string file_name) {
    if (file_name.size() >= 2) {
        char first = file_name.front();
        char last = file_name.back();
        if ((first == '\'' && last == '\'') || (first == '"' && last == '"')) {
            file_name = file_name.substr(1, file_name.size() - 2);
        }
    }
    return file_name;
}

std::string error_detail(const RMDBError &error) {
    std::string detail = error.what();
    constexpr std::string_view prefix = "Error: ";
    if (detail.compare(0, prefix.size(), prefix.data(), prefix.size()) == 0) {
        detail.erase(0, prefix.size());
    }
    return detail;
}

[[noreturn]] void throw_load_error(const std::string &tab_name, const std::string &file_name,
                                   size_t line, size_t column, const std::string &column_name,
                                   const std::string &reason) {
    std::string message = "load failed table=" + tab_name + " file=" + file_name +
                          " line=" + std::to_string(line);
    if (column > 0) {
        message += " column=" + std::to_string(column);
        if (!column_name.empty()) {
            message += "(" + column_name + ")";
        }
    }
    message += ": " + reason;
    throw RMDBError(message);
}

void split_csv_line_views(std::string *line, std::vector<std::string_view> *fields) {
    if (!line->empty() && line->back() == '\r') {
        line->pop_back();
    }
    fields->clear();
    size_t read_pos = 0;
    size_t write_pos = 0;

    while (true) {
        size_t column = fields->size() + 1;
        size_t field_start = write_pos;
        if (read_pos < line->size() && (*line)[read_pos] == '"') {
            ++read_pos;
            bool closed = false;
            while (read_pos < line->size()) {
                char ch = (*line)[read_pos++];
                if (ch != '"') {
                    (*line)[write_pos++] = ch;
                    continue;
                }
                if (read_pos < line->size() && (*line)[read_pos] == '"') {
                    (*line)[write_pos++] = '"';
                    ++read_pos;
                    continue;
                }
                closed = true;
                break;
            }
            if (!closed) {
                throw CsvDecodeError(column, "unterminated quoted field");
            }
            if (read_pos < line->size() && (*line)[read_pos] != ',') {
                throw CsvDecodeError(column, "unexpected character after quoted field");
            }
        } else {
            while (read_pos < line->size() && (*line)[read_pos] != ',') {
                char ch = (*line)[read_pos++];
                if (ch == '"') {
                    throw CsvDecodeError(column, "quote in unquoted field");
                }
                (*line)[write_pos++] = ch;
            }
        }

        fields->emplace_back(line->data() + field_start, write_pos - field_start);
        if (read_pos == line->size()) {
            return;
        }
        ++read_pos;
        if (read_pos == line->size()) {
            fields->emplace_back(line->data() + write_pos, 0);
            return;
        }
    }
}

std::string_view trim_numeric_field(std::string_view field) {
    while (!field.empty() && std::isspace(static_cast<unsigned char>(field.front()))) {
        field.remove_prefix(1);
    }
    while (!field.empty() && std::isspace(static_cast<unsigned char>(field.back()))) {
        field.remove_suffix(1);
    }
    return field;
}

int parse_csv_int(std::string_view field) {
    field = trim_numeric_field(field);
    if (field.empty()) {
        throw RMDBError("invalid int value");
    }
    bool negative = false;
    usize pos = 0;
    if (field[pos] == '+' || field[pos] == '-') {
        negative = field[pos] == '-';
        ++pos;
    }
    if (pos == field.size()) {
        throw RMDBError("invalid int value");
    }
    i64 value = 0;
    for (; pos < field.size(); ++pos) {
        unsigned char ch = static_cast<unsigned char>(field[pos]);
        if (!std::isdigit(ch)) {
            throw RMDBError("invalid int value");
        }
        value = value * 10 + static_cast<int>(ch - '0');
        i64 limit = negative ? -(static_cast<i64>(std::numeric_limits<int>::min()))
                             : std::numeric_limits<int>::max();
        if (value > limit) {
            throw RMDBError("int value out of range");
        }
    }
    return static_cast<int>(negative ? -value : value);
}

void write_csv_field_to_record(std::string_view field, const ColMeta &col, char *record,
                               std::string *parse_scratch) {
    char *dst = record + col.offset;
    if (col.type == TYPE_INT) {
        int value = parse_csv_int(field);
        std::memcpy(dst, &value, sizeof(value));
    } else if (col.type == TYPE_FLOAT) {
        field = trim_numeric_field(field);
        parse_scratch->assign(field.data(), field.size());
        char *end = nullptr;
        errno = 0;
        float value = std::strtof(parse_scratch->c_str(), &end);
        if (end == parse_scratch->c_str() || *end != '\0' || errno == ERANGE || !std::isfinite(value)) {
            throw RMDBError("invalid float value");
        }
        std::memcpy(dst, &value, sizeof(value));
    } else if (col.type == TYPE_STRING) {
        if (field.size() > static_cast<size_t>(col.len)) {
            throw RMDBError("string length " + std::to_string(field.size()) +
                            " exceeds limit " + std::to_string(col.len));
        }
        std::memset(dst, 0, static_cast<size_t>(col.len));
        if (!field.empty()) {
            std::memcpy(dst, field.data(), field.size());
        }
    } else {
        throw RMDBError("unsupported column type");
    }
}

void validate_header(std::string *line, const TabMeta &tab, const std::string &file_name) {
    std::vector<std::string_view> fields;
    fields.reserve(tab.cols.size());
    try {
        split_csv_line_views(line, &fields);
    } catch (const CsvDecodeError &error) {
        throw_load_error(tab.name, file_name, 1, error.column(), "", error.what());
    }
    if (fields.size() != tab.cols.size()) {
        throw_load_error(tab.name, file_name, 1, 0, "",
                         "header column count " + std::to_string(fields.size()) +
                             " does not match table column count " + std::to_string(tab.cols.size()));
    }
    for (size_t i = 0; i < fields.size(); ++i) {
        if (fields[i] != tab.cols[i].name) {
            throw_load_error(tab.name, file_name, 1, i + 1, tab.cols[i].name,
                             "header name mismatch");
        }
    }
}

size_t load_rows(RmFileHandle *fh, const TabMeta &tab, const std::string &file_name, std::ifstream *input) {
    std::string line;
    std::vector<std::string_view> fields;
    fields.reserve(tab.cols.size());
    std::string parse_scratch;
    parse_scratch.reserve(64);
    size_t physical_line = 1;
    BufferAccessStrategy cold_write_strategy(BufferAccessClass::ColdWrite);

    return fh->bulk_insert_records([&](char *record) -> bool {
        while (std::getline(*input, line)) {
            ++physical_line;
            if (line.empty() || line == "\r") {
                continue;
            }
            try {
                split_csv_line_views(&line, &fields);
            } catch (const CsvDecodeError &error) {
                std::string column_name = error.column() <= tab.cols.size() ? tab.cols[error.column() - 1].name : "";
                throw_load_error(tab.name, file_name, physical_line, error.column(), column_name, error.what());
            }
            if (fields.size() != tab.cols.size()) {
                throw_load_error(tab.name, file_name, physical_line, 0, "",
                                 "column count " + std::to_string(fields.size()) +
                                     " does not match table column count " + std::to_string(tab.cols.size()));
            }
            for (size_t i = 0; i < fields.size(); ++i) {
                try {
                    write_csv_field_to_record(fields[i], tab.cols[i], record, &parse_scratch);
                } catch (const RMDBError &error) {
                    throw_load_error(tab.name, file_name, physical_line, i + 1, tab.cols[i].name,
                                     error_detail(error));
                }
            }
            return true;
        }
        if (input->bad()) {
            throw_load_error(tab.name, file_name, physical_line + 1, 0, "", "I/O error while reading csv");
        }
        return false;
    }, &cold_write_strategy);
}

void collect_index_entries(RmFileHandle *fh, const IndexMeta &index, IxBulkLoadEntries *entries) {
    const auto &file_hdr = fh->get_file_hdr();
    const size_t page_count = static_cast<size_t>(
        std::max(0, file_hdr.num_pages - RM_FIRST_RECORD_PAGE));
    const size_t records_per_page = static_cast<size_t>(file_hdr.num_records_per_page);
    if (records_per_page != 0 &&
        page_count > std::numeric_limits<rmdb::u32>::max() / records_per_page) {
        throw InternalError("Bulk-load entry estimate overflow");
    }
    entries->reserve(page_count * records_per_page);
    std::string key(index.col_tot_len, '\0');
    for (RmScan scan(fh, BufferAccessClass::IndexBuild); !scan.is_end(); scan.next()) {
        scan.with_current_slot([&](const char *slot) {
            rmdb::build_index_key_into(index, slot, scan.rid(), &key);
            entries->append(key.data(), scan.rid());
            return true;
        });
    }
}

void reset_indexes_to_empty(SmManager *sm_manager, const std::string &tab_name,
                            const std::vector<IndexMeta> &indexes) {
    IxManager *ix_manager = sm_manager->get_ix_manager();
    for (const auto &index : indexes) {
        std::string index_name = ix_manager->get_index_name(tab_name, index);
        auto handle = sm_manager->ihs_.find(index_name);
        if (handle != sm_manager->ihs_.end()) {
            ix_manager->close_index(handle->second.get());
            sm_manager->ihs_.erase(handle);
        }
    }
    for (const auto &index : indexes) {
        if (ix_manager->exists(tab_name, index)) {
            ix_manager->destroy_index(tab_name, index);
        }
        ix_manager->create_index(tab_name, index);
        auto handle = ix_manager->open_index(tab_name, index);
        sm_manager->ihs_.emplace(ix_manager->get_index_name(tab_name, index), std::move(handle));
    }
}

void build_existing_indexes(SmManager *sm_manager, RmFileHandle *fh, const TabMeta &tab,
                            bool *indexes_mutated) {
    if (tab.indexes.empty()) {
        return;
    }
    for (const auto &index : tab.indexes) {
        IxBulkLoadEntries entries(static_cast<size_t>(index.col_tot_len));
        collect_index_entries(fh, index, &entries);
        std::string index_name = sm_manager->get_ix_manager()->get_index_name(tab.name, index);
        auto *handle = sm_manager->ihs_.at(index_name).get();
        handle->prepare_bulk_load(entries, index.unique);
        *indexes_mutated = true;
        handle->bulk_load_prepared(entries);
        handle->flush();
        // Drop the key arena, RID array, and sort ordinals before collecting
        // the next index. The allocator trim at the LOAD boundary can then
        // return their pages instead of retaining the peak capacity.
        entries.release();
    }
}

void trim_allocator_after_load() {
#if defined(__GLIBC__)
    const char *disabled = std::getenv("RMDB_DISABLE_LOAD_TRIM");
    if (disabled == nullptr || std::strcmp(disabled, "1") != 0) {
        malloc_trim(0);
    }
#endif
}

}  // namespace

size_t load_csv_into_table(SmManager *sm_manager, const std::string &tab_name, const std::string &file_name) {
    if (!sm_manager->db_.is_table(tab_name)) {
        throw TableNotFoundError(tab_name);
    }
    TabMeta &tab = sm_manager->db_.get_table(tab_name);
    RmFileHandle *fh = sm_manager->fhs_.at(tab_name).get();
    RmFileHdr file_hdr = fh->get_file_hdr();
    if (file_hdr.num_pages != RM_FIRST_RECORD_PAGE || file_hdr.first_free_page_no != RM_NO_PAGE) {
        throw RMDBError("load requires an empty table: " + tab_name);
    }

    std::string normalized_file_name = normalize_load_file_name(file_name);
    std::ifstream input(normalized_file_name);
    if (!input.is_open()) {
        throw RMDBError("failed to open csv file: " + normalized_file_name);
    }

    std::string header;
    if (!std::getline(input, header)) {
        if (input.bad()) {
            throw_load_error(tab_name, normalized_file_name, 1, 0, "", "I/O error while reading csv header");
        }
        throw_load_error(tab_name, normalized_file_name, 1, 0, "", "missing csv header");
    }
    validate_header(&header, tab, normalized_file_name);
    size_t inserted = load_rows(fh, tab, normalized_file_name, &input);
    bool indexes_mutated = false;
    try {
        fh->flush();
        build_existing_indexes(sm_manager, fh, tab, &indexes_mutated);
    } catch (...) {
        std::exception_ptr load_error = std::current_exception();
        try {
            if (indexes_mutated) {
                reset_indexes_to_empty(sm_manager, tab_name, tab.indexes);
            }
            fh->reset_empty_bulk_load();
        } catch (...) {
            throw InternalError("failed to rollback indexed bulk load");
        }
        std::rethrow_exception(load_error);
    }
    trim_allocator_after_load();
    return inserted;
}

}  // namespace rmdb
