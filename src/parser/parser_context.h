#pragma once

#include <memory>
#include <string>

namespace ast {
struct TreeNode;
}

namespace rmdb {

struct ParserContext {
    std::shared_ptr<ast::TreeNode> result;
    std::string error;
};

struct ParseResult {
    int status{1};
    std::shared_ptr<ast::TreeNode> tree;
    std::string error;

    explicit operator bool() const { return status == 0; }
};

}  // namespace rmdb
