<div align="center">
<img src="assets/RMDB-logo.png" width="25%" alt="RMDB logo" />
</div>



全国大学生计算机系统能力大赛数据库管理系统赛道，以培养学生“数据库管理系统内核实现”能力为目标。本次比赛为参赛队伍提供数据库管理系统代码框架RMDB，参赛队伍在RMDB的基础上，设计和实现一个完整的关系型数据库管理系统，该系统要求具备运行TPC-C基准测试（TPC-C是一个面向联机事务处理的测试基准）常用负载的能力。

RMDB由中国人民大学数据库教学团队开发，同时得到教育部-华为”智能基座”项目的支持，平台、赛题和测试用例等得到了全国大学生计算机系统能力大赛数据库管理系统赛道技术委员会的支持和审核。系统能力大赛专家组和[101计划数据库系统课程](http://101.pku.edu.cn/courseDetails?id=DC767C683D697417E0555943CA7634DE)工作组给予了指导。

## 实验环境：
- 操作系统：Ubuntu 18.04 及以上(64位)
- 编译器：GCC
- 编程语言：C++20
- 管理工具：cmake
- 推荐编辑器：VScode

### 依赖环境库配置：
- gcc 10及以上版本（要求完全支持C++20）
- cmake 3.16及以上版本
- flex
- bison
- readline

## 项目结构

- `src/rmdb.cpp`：服务端入口，负责数据库打开/创建、恢复、TCP 连接处理和 SQL 分发。
- `rmdb_client/main.cpp`：通过 TCP/Unix socket 与服务端交互的客户端。
- `src/parser/`：SQL 词法、语法解析和 AST 定义。
- `src/analyze/`：表、列、类型、谓词、分组、连接和聚合的语义检查。
- `src/optimizer/`：逻辑计划和优化，包括索引选择、谓词/投影下推、连接顺序、排序和 `explain`。
- `src/execution/`：插入、删除、更新、顺序扫描、索引扫描、连接、投影和排序等执行器。
- `src/system/`：元数据和 DDL 管理，包括表/索引创建删除、`show tables`、`desc` 和元数据持久化。
- `src/storage/`：磁盘管理器、页抽象和缓冲池。
- `src/replacer/`：缓冲池替换策略，主要为 LRU。
- `src/record/`：记录文件、记录页和记录扫描。
- `src/index/`：B+ 树索引和索引扫描。
- `src/transaction/`：事务、锁、时间戳、MVCC、写集合和回滚。
- `src/recovery/`：日志、redo/undo、崩溃恢复和检查点。
- `src/unit_test.cpp`：低层模块的 GoogleTest 单元测试。
- `tests/`：本地回归测试，包括 SQL 用例、期望输出和 Python 运行器。
- `deps/`：第三方依赖，包括 vendored 的 googletest。

本地回归测试及运行说明位于 `tests/`。

## 推荐参考资料

- [**Database System Concepts** (***Seventh Edition***)](https://db-book.com/)
- [PostgreSQL 数据库内核分析](https://book.douban.com/subject/6971366//)
- [数据库系统实现](https://book.douban.com/subject/4838430/)
- [数据库系统概论(第5版)](http://chinadb.ruc.edu.cn/second/url/2)

## License
RMDB采用[木兰宽松许可证，第2版](https://license.coscl.org.cn/MulanPSL2)，可以自由拷贝和使用源码, 当做修改或分发时, 请遵守[木兰宽松许可证，第2版](https://license.coscl.org.cn/MulanPSL2).
