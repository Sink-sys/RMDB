#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>

namespace rmdb {

enum class TcpFrameStatus {
    kCommand,
    kNeedMore,
    kTooLarge,
};

class TcpCommandFramer {
   public:
    explicit TcpCommandFramer(size_t max_command_bytes) : max_command_bytes_(max_command_bytes) {}

    void Append(const char *data, size_t size) { pending_.append(data, size); }

    TcpFrameStatus Next(std::string *command) {
        if (command == nullptr) {
            return TcpFrameStatus::kTooLarge;
        }
        command->clear();

        while (true) {
            while (!pending_.empty() && pending_.front() == '\0') {
                pending_.erase(0, 1);
            }
            if (pending_.empty()) {
                return TcpFrameStatus::kNeedMore;
            }

            size_t nul_end = pending_.find('\0');
            size_t semicolon_end = FindStatementSemicolon(pending_, nul_end);
            if (semicolon_end != std::string::npos &&
                (nul_end == std::string::npos || semicolon_end < nul_end)) {
                size_t command_size = semicolon_end + 1;
                if (command_size > max_command_bytes_) {
                    return TcpFrameStatus::kTooLarge;
                }
                command->assign(pending_.data(), command_size);
                pending_.erase(0, command_size);
            } else if (nul_end != std::string::npos) {
                if (nul_end > max_command_bytes_) {
                    return TcpFrameStatus::kTooLarge;
                }
                command->assign(pending_.data(), nul_end);
                pending_.erase(0, nul_end + 1);
            } else {
                return pending_.size() > max_command_bytes_ ? TcpFrameStatus::kTooLarge
                                                            : TcpFrameStatus::kNeedMore;
            }

            if (!OnlyAsciiWhitespace(*command)) {
                return TcpFrameStatus::kCommand;
            }
            command->clear();
        }
    }

    size_t pending_size() const { return pending_.size(); }

   private:
    static size_t FindStatementSemicolon(const std::string &input, size_t scan_end) {
        bool in_string = false;
        size_t end = std::min(scan_end, input.size());
        for (size_t i = 0; i < end; ++i) {
            if (input[i] == '\'') {
                if (in_string && i + 1 < end && input[i + 1] == '\'') {
                    ++i;
                    continue;
                }
                in_string = !in_string;
            } else if (input[i] == ';' && !in_string) {
                return i;
            }
        }
        return std::string::npos;
    }

    static bool OnlyAsciiWhitespace(std::string_view input) {
        return std::all_of(input.begin(), input.end(), [](unsigned char ch) {
            return std::isspace(ch) != 0;
        });
    }

    size_t max_command_bytes_;
    std::string pending_;
};

}  // namespace rmdb
