#pragma once

#include <istream>
#include <mutex>
#include <ostream>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace rmdb {

// Immutable SQL identifier stored once per process. Query, plan and executor
// objects copy only a pointer, so cloning a TabCol never allocates or destroys
// three independent std::strings. The pool intentionally has process lifetime:
// prepared-plan objects may outlive a catalog epoch, but their diagnostic names
// must remain valid until the last execution releases them.
class InternedIdentifier {
   public:
    InternedIdentifier() noexcept : value_(&empty_value()) {}
    InternedIdentifier(const char *value) : value_(intern(value == nullptr ? std::string_view{} : value)) {}
    InternedIdentifier(const std::string &value) : value_(intern(value)) {}
    InternedIdentifier(std::string_view value) : value_(intern(value)) {}

    InternedIdentifier(const InternedIdentifier &) noexcept = default;
    InternedIdentifier(InternedIdentifier &&) noexcept = default;
    InternedIdentifier &operator=(const InternedIdentifier &) noexcept = default;
    InternedIdentifier &operator=(InternedIdentifier &&) noexcept = default;

    InternedIdentifier &operator=(const char *value) {
        value_ = intern(value == nullptr ? std::string_view{} : value);
        return *this;
    }
    InternedIdentifier &operator=(const std::string &value) {
        value_ = intern(value);
        return *this;
    }
    InternedIdentifier &operator=(std::string_view value) {
        value_ = intern(value);
        return *this;
    }

    bool empty() const noexcept { return value_->empty(); }
    void clear() noexcept { value_ = &empty_value(); }
    size_t size() const noexcept { return value_->size(); }
    const char *data() const noexcept { return value_->data(); }
    const char *c_str() const noexcept { return value_->c_str(); }
    const std::string &str() const noexcept { return *value_; }
    operator const std::string &() const noexcept { return *value_; }

    int compare(const InternedIdentifier &other) const noexcept {
        return value_ == other.value_ ? 0 : value_->compare(*other.value_);
    }
    int compare(const std::string &other) const noexcept { return value_->compare(other); }
    int compare(std::string_view other) const noexcept { return std::string_view(*value_).compare(other); }

    friend bool operator==(const InternedIdentifier &lhs, const InternedIdentifier &rhs) noexcept {
        return lhs.value_ == rhs.value_;
    }
    friend bool operator!=(const InternedIdentifier &lhs, const InternedIdentifier &rhs) noexcept {
        return !(lhs == rhs);
    }
    friend bool operator<(const InternedIdentifier &lhs, const InternedIdentifier &rhs) noexcept {
        return lhs.value_ == rhs.value_ ? false : *lhs.value_ < *rhs.value_;
    }

    friend bool operator==(const InternedIdentifier &lhs, const std::string &rhs) noexcept {
        return *lhs.value_ == rhs;
    }
    friend bool operator==(const std::string &lhs, const InternedIdentifier &rhs) noexcept { return rhs == lhs; }
    friend bool operator!=(const InternedIdentifier &lhs, const std::string &rhs) noexcept { return !(lhs == rhs); }
    friend bool operator!=(const std::string &lhs, const InternedIdentifier &rhs) noexcept { return !(lhs == rhs); }
    friend bool operator==(const InternedIdentifier &lhs, const char *rhs) noexcept {
        return *lhs.value_ == (rhs == nullptr ? "" : rhs);
    }
    friend bool operator==(const char *lhs, const InternedIdentifier &rhs) noexcept { return rhs == lhs; }
    friend bool operator!=(const InternedIdentifier &lhs, const char *rhs) noexcept { return !(lhs == rhs); }
    friend bool operator!=(const char *lhs, const InternedIdentifier &rhs) noexcept { return !(lhs == rhs); }

    friend std::string operator+(const InternedIdentifier &lhs, const InternedIdentifier &rhs) {
        return lhs.str() + rhs.str();
    }
    friend std::string operator+(const InternedIdentifier &lhs, const std::string &rhs) {
        return lhs.str() + rhs;
    }
    friend std::string operator+(const std::string &lhs, const InternedIdentifier &rhs) {
        return lhs + rhs.str();
    }
    friend std::string operator+(const InternedIdentifier &lhs, const char *rhs) {
        return lhs.str() + (rhs == nullptr ? "" : rhs);
    }
    friend std::string operator+(const char *lhs, const InternedIdentifier &rhs) {
        return (lhs == nullptr ? std::string{} : std::string(lhs)) + rhs.str();
    }
    friend std::string operator+(const InternedIdentifier &lhs, char rhs) { return lhs.str() + rhs; }
    friend std::string operator+(char lhs, const InternedIdentifier &rhs) { return lhs + rhs.str(); }
    friend std::ostream &operator<<(std::ostream &out, const InternedIdentifier &value) {
        return out << value.str();
    }
    friend std::istream &operator>>(std::istream &in, InternedIdentifier &value) {
        std::string decoded;
        if (in >> decoded) {
            value = decoded;
        }
        return in;
    }

   private:
    struct TransparentHash {
        using is_transparent = void;
        size_t operator()(std::string_view value) const noexcept {
            return std::hash<std::string_view>{}(value);
        }
        size_t operator()(const std::string &value) const noexcept {
            return std::hash<std::string_view>{}(value);
        }
    };

    struct Pool {
        std::shared_mutex latch;
        std::unordered_set<std::string, TransparentHash, std::equal_to<>> values;
    };

    static const std::string &empty_value() {
        static const std::string empty;
        return empty;
    }

    static Pool &pool() {
        static Pool *identifier_pool = new Pool();
        return *identifier_pool;
    }

    static const std::string *intern(std::string_view value) {
        if (value.empty()) {
            return &empty_value();
        }
        auto &identifier_pool = pool();
        {
            std::shared_lock<std::shared_mutex> guard(identifier_pool.latch);
            auto found = identifier_pool.values.find(value);
            if (found != identifier_pool.values.end()) {
                return &*found;
            }
        }
        std::unique_lock<std::shared_mutex> guard(identifier_pool.latch);
        auto [entry, inserted] = identifier_pool.values.emplace(value);
        (void)inserted;
        return &*entry;
    }

    const std::string *value_;
};

static_assert(sizeof(InternedIdentifier) == sizeof(void *));

}  // namespace rmdb
