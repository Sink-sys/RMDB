#pragma once

#include <array>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/types.h"

namespace rmdb {

enum class SqlTemplateLiteralType { kInt, kFloat, kString };

struct SqlTemplateLiteral {
    SqlTemplateLiteralType type = SqlTemplateLiteralType::kInt;
    int int_val = 0;
    float float_val = 0.0f;
    // Borrowed from the SQL statement buffer. SqlTemplateCandidate is statement-local
    // and must be consumed before that buffer is reused.
    std::string_view str_val;
};

struct SqlTemplateKey {
    rmdb::u64 h1 = 0;
    rmdb::u64 h2 = 0;
    rmdb::u32 normalized_len = 0;
    rmdb::u16 literal_count = 0;

    bool operator==(const SqlTemplateKey &other) const {
        return h1 == other.h1 && h2 == other.h2 && normalized_len == other.normalized_len &&
               literal_count == other.literal_count;
    }
};

struct SqlTemplateKeyHash {
    size_t operator()(const SqlTemplateKey &key) const {
        rmdb::u64 mixed = key.h1 ^ (key.h2 + 0x9e3779b97f4a7c15ULL + (key.h1 << 6) + (key.h1 >> 2));
        mixed ^= static_cast<rmdb::u64>(key.normalized_len) << 32;
        mixed ^= key.literal_count;
        return static_cast<size_t>(mixed);
    }
};

class SqlTemplateLiteralList {
   public:
    static constexpr size_t kInlineCapacity = 16;

    bool push_back(SqlTemplateLiteral literal) {
        if (size_ == std::numeric_limits<rmdb::u16>::max()) {
            return false;
        }
        size_t index = size_++;
        if (index < kInlineCapacity) {
            inline_literals_[index] = std::move(literal);
        } else {
            overflow_literals_.push_back(std::move(literal));
        }
        return true;
    }

    size_t size() const { return size_; }

    const SqlTemplateLiteral &operator[](size_t index) const {
        return index < kInlineCapacity ? inline_literals_[index] : overflow_literals_[index - kInlineCapacity];
    }

   private:
    rmdb::u16 size_ = 0;
    std::array<SqlTemplateLiteral, kInlineCapacity> inline_literals_{};
    std::vector<SqlTemplateLiteral> overflow_literals_;
};

struct SqlTemplateCandidate {
    SqlTemplateKey key;
    SqlTemplateLiteralList literals;
};

namespace sql_template_detail {

inline std::atomic<rmdb::u64> &schema_epoch_storage() {
    static std::atomic<rmdb::u64> epoch{1};
    return epoch;
}

enum class TemplateKeywordClass {
    kNone,
    kSelect,
    kInsert,
    kUpdate,
    kDelete,
    kSet,
    kTransaction,
    kUnsupported,
    kOther,
};

constexpr unsigned char ascii_lower(unsigned char ch) {
    return ch >= 'A' && ch <= 'Z' ? static_cast<unsigned char>(ch + ('a' - 'A')) : ch;
}

constexpr bool ascii_space(unsigned char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
}

constexpr bool ascii_alpha(unsigned char ch) {
    ch = ascii_lower(ch);
    return ch >= 'a' && ch <= 'z';
}

constexpr bool ascii_digit(unsigned char ch) { return ch >= '0' && ch <= '9'; }

constexpr bool ascii_alnum(unsigned char ch) { return ascii_alpha(ch) || ascii_digit(ch); }

template <size_t N>
inline bool ascii_iequals(const char *text, size_t len, const char (&word)[N]) {
    if (len != N - 1) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        if (ascii_lower(static_cast<unsigned char>(text[i])) != static_cast<unsigned char>(word[i])) {
            return false;
        }
    }
    return true;
}

// This classifier is on every template-cache lookup. Keep it allocation-free,
// locale-independent, and bounded by token length instead of vocabulary size.
inline TemplateKeywordClass keyword_class(const char *text, size_t len) {
    switch (len) {
        case 2:
            if (ascii_iequals(text, len, "on") || ascii_iequals(text, len, "as") ||
                ascii_iequals(text, len, "by")) return TemplateKeywordClass::kOther;
            break;
        case 3:
            if (ascii_iequals(text, len, "set")) return TemplateKeywordClass::kSet;
            if (ascii_iequals(text, len, "and") || ascii_iequals(text, len, "asc") ||
                ascii_iequals(text, len, "max") || ascii_iequals(text, len, "min") ||
                ascii_iequals(text, len, "sum") || ascii_iequals(text, len, "avg") ||
                ascii_iequals(text, len, "int")) return TemplateKeywordClass::kOther;
            break;
        case 4:
            if (ascii_iequals(text, len, "drop") || ascii_iequals(text, len, "load") ||
                ascii_iequals(text, len, "show") ||
                ascii_iequals(text, len, "help") || ascii_iequals(text, len, "exit") ||
                ascii_iequals(text, len, "true")) return TemplateKeywordClass::kUnsupported;
            if (ascii_iequals(text, len, "into") || ascii_iequals(text, len, "from") ||
                ascii_iequals(text, len, "char") || ascii_iequals(text, len, "join") ||
                ascii_iequals(text, len, "semi") || ascii_iequals(text, len, "desc"))
                return TemplateKeywordClass::kOther;
            break;
        case 5:
            if (ascii_iequals(text, len, "begin") || ascii_iequals(text, len, "abort") ||
                ascii_iequals(text, len, "union") || ascii_iequals(text, len, "false")) return TemplateKeywordClass::kUnsupported;
            if (ascii_iequals(text, len, "table") || ascii_iequals(text, len, "where") ||
                ascii_iequals(text, len, "level") || ascii_iequals(text, len, "float") ||
                ascii_iequals(text, len, "index") || ascii_iequals(text, len, "group") ||
                ascii_iequals(text, len, "limit") || ascii_iequals(text, len, "order") ||
                ascii_iequals(text, len, "count")) return TemplateKeywordClass::kOther;
            break;
        case 6:
            if (ascii_iequals(text, len, "select")) return TemplateKeywordClass::kSelect;
            if (ascii_iequals(text, len, "insert")) return TemplateKeywordClass::kInsert;
            if (ascii_iequals(text, len, "update")) return TemplateKeywordClass::kUpdate;
            if (ascii_iequals(text, len, "delete")) return TemplateKeywordClass::kDelete;
            if (ascii_iequals(text, len, "create") || ascii_iequals(text, len, "commit")) return TemplateKeywordClass::kUnsupported;
            if (ascii_iequals(text, len, "tables") || ascii_iequals(text, len, "values") ||
                ascii_iequals(text, len, "having")) return TemplateKeywordClass::kOther;
            break;
        case 7:
            if (ascii_iequals(text, len, "explain") || ascii_iequals(text, len, "analyze")) return TemplateKeywordClass::kUnsupported;
            break;
        case 8:
            if (ascii_iequals(text, len, "rollback")) return TemplateKeywordClass::kUnsupported;
            if (ascii_iequals(text, len, "snapshot") || ascii_iequals(text, len, "datetime")) return TemplateKeywordClass::kOther;
            break;
        case 9:
            if (ascii_iequals(text, len, "isolation")) return TemplateKeywordClass::kOther;
            break;
        case 11:
            if (ascii_iequals(text, len, "transaction")) return TemplateKeywordClass::kTransaction;
            break;
        case 12:
            if (ascii_iequals(text, len, "serializable")) return TemplateKeywordClass::kOther;
            break;
        case 15:
            if (ascii_iequals(text, len, "enable_nestloop")) return TemplateKeywordClass::kOther;
            break;
        case 16:
            if (ascii_iequals(text, len, "enable_sortmerge")) return TemplateKeywordClass::kOther;
            break;
        case 17:
            if (ascii_iequals(text, len, "static_checkpoint")) return TemplateKeywordClass::kUnsupported;
            break;
        default: break;
    }
    return TemplateKeywordClass::kNone;
}

class TemplateKeyBuilder {
   public:
    void begin_token() {
        if (has_token_) {
            append_byte(0xff);
        }
        has_token_ = true;
    }

    void append_byte(unsigned char byte) {
        h1_ ^= byte;
        h1_ *= 1099511628211ULL;
        h2_ ^= static_cast<rmdb::u64>(byte) + 0x9e3779b97f4a7c15ULL + (h2_ << 6) + (h2_ >> 2);
        ++normalized_len_;
    }

    void append_token(const char *text, size_t len, bool lowercase) {
        begin_token();
        for (size_t i = 0; i < len; ++i) {
            unsigned char ch = static_cast<unsigned char>(text[i]);
            append_byte(lowercase ? ascii_lower(ch) : ch);
        }
    }

    void append_token(const char *text) {
        append_token(text, std::strlen(text), false);
    }

    SqlTemplateKey finish(size_t literal_count) const {
        SqlTemplateKey key;
        key.h1 = h1_;
        key.h2 = h2_;
        key.normalized_len = normalized_len_;
        key.literal_count = static_cast<rmdb::u16>(literal_count);
        return key;
    }

   private:
    rmdb::u64 h1_ = 1469598103934665603ULL;
    rmdb::u64 h2_ = 0x9e3779b97f4a7c15ULL;
    rmdb::u32 normalized_len_ = 0;
    bool has_token_ = false;
};

inline bool read_string_literal_raw(const char *sql, size_t *pos, std::string_view *value) {
    size_t i = *pos + 1;
    while (sql[i] != '\0' && sql[i] != '\'') {
        ++i;
    }
    if (sql[i] == '\0') {
        return false;
    }
    *value = std::string_view(sql + *pos + 1, i - *pos - 1);
    *pos = i + 1;
    return true;
}

inline bool read_numeric_literal_raw(const char *sql, size_t *pos, SqlTemplateLiteral *literal,
                                     const char **placeholder) {
    size_t start = *pos;
    size_t i = start;
    int int_value = 0;
    size_t integral_digits = 0;
    while (ascii_digit(static_cast<unsigned char>(sql[i]))) {
        int_value = int_value * 10 + (sql[i] - '0');
        ++i;
        ++integral_digits;
    }
    bool is_float = false;
    if (sql[i] == '.') {
        is_float = true;
        ++i;
        while (ascii_digit(static_cast<unsigned char>(sql[i]))) {
            ++i;
        }
    }
    if (integral_digits == 0 && i == start + 1) {
        return false;
    }
    if (sql[i] == 'e' || sql[i] == 'E') {
        is_float = true;
        ++i;
        if (sql[i] == '+' || sql[i] == '-') {
            ++i;
        }
        size_t exponent_start = i;
        while (ascii_digit(static_cast<unsigned char>(sql[i]))) {
            ++i;
        }
        if (i == exponent_start) {
            return false;
        }
    }
    if (is_float) {
        literal->type = SqlTemplateLiteralType::kFloat;
        char *end = nullptr;
        literal->float_val = std::strtof(sql + start, &end);
        if (end != sql + i) {
            return false;
        }
        *placeholder = "?f";
    } else {
        literal->type = SqlTemplateLiteralType::kInt;
        literal->int_val = int_value;
        *placeholder = "?i";
    }
    *pos = i;
    return true;
}

}  // namespace sql_template_detail

inline rmdb::u64 sql_template_schema_epoch() {
    return sql_template_detail::schema_epoch_storage().load(std::memory_order_acquire);
}

inline void bump_sql_template_schema_epoch() {
    sql_template_detail::schema_epoch_storage().fetch_add(1, std::memory_order_acq_rel);
}

inline std::optional<SqlTemplateCandidate> make_sql_template_candidate(const char *raw_sql) {
    if (raw_sql == nullptr) {
        return std::nullopt;
    }
    sql_template_detail::TemplateKeyBuilder key_builder;
    SqlTemplateCandidate candidate;
    bool saw_token = false;
    bool first_token_cacheable = false;
    bool unsupported = false;
    bool previous_set = false;
    size_t i = 0;
    while (raw_sql[i] != '\0') {
        unsigned char ch = static_cast<unsigned char>(raw_sql[i]);
        if (sql_template_detail::ascii_space(ch)) {
            ++i;
            continue;
        }
        if ((raw_sql[i] == '-' && raw_sql[i + 1] == '-') ||
            (raw_sql[i] == '/' && raw_sql[i + 1] == '*')) {
            return std::nullopt;
        }
        if (sql_template_detail::ascii_alpha(ch)) {
            size_t start = i++;
            while (raw_sql[i] != '\0') {
                unsigned char next = static_cast<unsigned char>(raw_sql[i]);
                if (!sql_template_detail::ascii_alnum(next) && raw_sql[i] != '_') {
                    break;
                }
                ++i;
            }
            size_t len = i - start;
            auto keyword = sql_template_detail::keyword_class(raw_sql + start, len);
            bool is_keyword = keyword != sql_template_detail::TemplateKeywordClass::kNone;
            if (!saw_token) {
                first_token_cacheable = keyword == sql_template_detail::TemplateKeywordClass::kSelect ||
                                        keyword == sql_template_detail::TemplateKeywordClass::kInsert ||
                                        keyword == sql_template_detail::TemplateKeywordClass::kUpdate ||
                                        keyword == sql_template_detail::TemplateKeywordClass::kDelete;
            }
            if (keyword == sql_template_detail::TemplateKeywordClass::kUnsupported ||
                (previous_set && keyword == sql_template_detail::TemplateKeywordClass::kTransaction)) {
                unsupported = true;
            }
            previous_set = keyword == sql_template_detail::TemplateKeywordClass::kSet;
            key_builder.append_token(raw_sql + start, len, is_keyword);
            saw_token = true;
            continue;
        }
        if (sql_template_detail::ascii_digit(ch) ||
            (raw_sql[i] == '.' && sql_template_detail::ascii_digit(static_cast<unsigned char>(raw_sql[i + 1])))) {
            SqlTemplateLiteral literal;
            const char *placeholder = nullptr;
            if (!sql_template_detail::read_numeric_literal_raw(raw_sql, &i, &literal, &placeholder) ||
                !candidate.literals.push_back(std::move(literal))) {
                return std::nullopt;
            }
            key_builder.append_token(placeholder);
            previous_set = false;
            saw_token = true;
            continue;
        }
        if (raw_sql[i] == '\'') {
            SqlTemplateLiteral literal;
            literal.type = SqlTemplateLiteralType::kString;
            if (!sql_template_detail::read_string_literal_raw(raw_sql, &i, &literal.str_val) ||
                !candidate.literals.push_back(std::move(literal))) {
                return std::nullopt;
            }
            key_builder.append_token("?s");
            previous_set = false;
            saw_token = true;
            continue;
        }
        if (raw_sql[i + 1] != '\0' &&
            ((raw_sql[i] == '>' && raw_sql[i + 1] == '=') ||
             (raw_sql[i] == '<' && (raw_sql[i + 1] == '=' || raw_sql[i + 1] == '>')))) {
            key_builder.append_token(raw_sql + i, 2, false);
            i += 2;
            previous_set = false;
            saw_token = true;
            continue;
        }
        if (raw_sql[i] == '!' && raw_sql[i + 1] == '=') {
            key_builder.append_token(raw_sql + i, 2, false);
            i += 2;
            previous_set = false;
            saw_token = true;
            continue;
        }
        switch (raw_sql[i]) {
            case ';': case '(': case ')': case ',': case '*': case '/': case '=': case '<':
            case '>': case '.': case '+': case '-':
            key_builder.append_token(raw_sql + i, 1, false);
            ++i;
            previous_set = false;
            saw_token = true;
            continue;
            default: break;
        }
        return std::nullopt;
    }
    if (!saw_token || !first_token_cacheable || unsupported ||
        candidate.literals.size() > std::numeric_limits<rmdb::u16>::max()) {
        return std::nullopt;
    }
    candidate.key = key_builder.finish(candidate.literals.size());
    return candidate;
}

}  // namespace rmdb
