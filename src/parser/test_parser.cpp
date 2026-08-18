/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */
#undef NDEBUG

#include <atomic>
#include <cassert>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "parser.h"

int main() {
    std::vector<std::string> sqls = {
        "show tables;",
        "desc tb;",
        "create table tb (a int, b float, c char(4));",
        "drop table tb;",
        "create index tb(a);",
        "create index tb(a, b, c);",
        "drop index tb(a, b, c);",
        "drop index tb(b);",
        "insert into tb values (1, 3.14, 'pi');",
        "delete from tb where a = 1;",
        "update tb set a = 1, b = 2.2, c = 'xyz' where x = 2 and y < 1.1 and z > 'abc';",
        "select * from tb;",
        "select * from tb where x <> 2 and y >= 3. and z <= '123' and b < tb.a;",
        "select x.a, y.b from x, y where x.a = y.b and c = d;",
        "select x.a, y.b from x join y where x.a = y.b and c = d;",
        "exit;",
        "help;",
        "",
    };
    for (auto &sql : sqls) {
        std::cout << sql << std::endl;
        auto result = rmdb::ParseSql(sql);
        assert(result.status == 0);
        if (result.tree != nullptr) {
            ast::TreePrinter::print(result.tree);
            std::cout << std::endl;
        } else {
            std::cout << "exit/EOF" << std::endl;
        }
    }

    auto prepared = rmdb::ParseSql(
        "select id from prepared_t where id >= $1 and id != $2 and id = $1;");
    assert(prepared.status == 0 && prepared.tree != nullptr);
    auto prepared_select = std::dynamic_pointer_cast<ast::SelectStmt>(prepared.tree);
    assert(prepared_select != nullptr && prepared_select->conds.size() == 3);
    auto first_parameter = std::dynamic_pointer_cast<ast::ParameterRef>(prepared_select->conds[0]->rhs);
    auto second_parameter = std::dynamic_pointer_cast<ast::ParameterRef>(prepared_select->conds[1]->rhs);
    auto repeated_parameter = std::dynamic_pointer_cast<ast::ParameterRef>(prepared_select->conds[2]->rhs);
    assert(first_parameter != nullptr && first_parameter->ordinal == 1);
    assert(second_parameter != nullptr && second_parameter->ordinal == 2);
    assert(repeated_parameter != nullptr && repeated_parameter->ordinal == 1);

    const std::vector<std::string> concurrent_sqls = {
        "select * from alpha where id = 1;",
        "insert into beta values (1, 'a;b');",
        "update gamma set value = 2 where id = 3;",
        "delete from delta where id = 4;",
        "select count(*) from epsilon where id >= 5;",
    };
    std::atomic<bool> concurrent_ok{true};
    std::vector<std::thread> workers;
    for (size_t worker = 0; worker < 16; ++worker) {
        workers.emplace_back([&, worker] {
            for (size_t iteration = 0; iteration < 500; ++iteration) {
                auto result = rmdb::ParseSql(concurrent_sqls[(worker + iteration) % concurrent_sqls.size()]);
                if (result.status != 0 || result.tree == nullptr) {
                    concurrent_ok.store(false, std::memory_order_relaxed);
                    return;
                }
                if (iteration % 25 == 0) {
                    auto invalid = rmdb::ParseSql("select from broken;");
                    if (invalid.status == 0 || invalid.error.empty()) {
                        concurrent_ok.store(false, std::memory_order_relaxed);
                        return;
                    }
                }
            }
        });
    }
    for (auto &worker : workers) {
        worker.join();
    }
    assert(concurrent_ok.load(std::memory_order_relaxed));
    return 0;
}
