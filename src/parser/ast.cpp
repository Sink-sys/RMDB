/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */
#include <limits>
#include <utility>

#include "parser.h"
#include "parser_defs.h"

namespace rmdb {

ParseResult ParseSql(std::string_view sql) {
    ParseResult result;
    if (sql.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        result.error = "SQL input exceeds parser length limit";
        return result;
    }

    yyscan_t scanner = nullptr;
    if (yylex_init(&scanner) != 0) {
        result.error = "Failed to initialize SQL scanner";
        return result;
    }

    YY_BUFFER_STATE buffer = yy_scan_bytes(sql.data(), static_cast<int>(sql.size()), scanner);
    if (buffer == nullptr) {
        yylex_destroy(scanner);
        result.error = "Failed to allocate SQL scanner buffer";
        return result;
    }

    ParserContext context;
    try {
        result.status = yyparse(&context, scanner);
    } catch (...) {
        yy_delete_buffer(buffer, scanner);
        yylex_destroy(scanner);
        throw;
    }
    yy_delete_buffer(buffer, scanner);
    yylex_destroy(scanner);
    result.tree = std::move(context.result);
    result.error = std::move(context.error);
    return result;
}

}  // namespace rmdb
