# RMDB 本地回归测试

`tests/` 是项目自带的本地回归工具。本地通过表示当前环境下的回归用例通过，不能替代其他环境的验证。

全部服务端测试共享 TCP 端口 `8765` 和本地数据库目录，必须串行执行。

## 测试入口

先构建服务端：

```bash
cmake --build build --target rmdb
```

列出或运行主回归套件：

```bash
python3 tests/run_doc_tests.py --list
python3 tests/run_doc_tests.py
python3 tests/run_doc_tests.py 02_01
python3 tests/run_doc_tests.py 09_si_ --show-server-log
python3 tests/run_doc_tests.py 10_ --recovery-row-count 1000 --keep-db --show-server-log
```

主运行器通过 RMDB Wire Protocol v3 执行 SQL，并直接比较 META、类型化 ROW、终结状态和规范化结果，不读取 `output.txt`。

协议专项测试：

```bash
RMDB_TEST_BUILD=build python3 tests/test_wire_protocol.py
RMDB_TEST_BUILD=build python3 tests/test_prepared_wire_protocol.py
```

## 目录结构

- `cases/`：静态 SQL 输入。
- `expected/`：静态输入的期望结果。
- `run_doc_tests.py`：静态、事务、隔离和恢复测试的统一串行入口。
- `test_final_isolation.py`：隔离级别和多会话回归用例。
- `test_wire_protocol.py`：EXEC_STREAM、类型化结果、错误状态和传输边界。
- `test_prepared_wire_protocol.py`：PREPARE_SET、typed EXEC_BATCH 和 AUTO_ABORT。

## 隔离级别测试

`test_final_isolation.py` 覆盖快照隔离和可串行化隔离下的常见事务语义。

可单独运行：

```bash
python3 tests/test_final_isolation.py --list
python3 tests/test_final_isolation.py 09_cfg_
python3 tests/test_final_isolation.py 09_si_
python3 tests/test_final_isolation.py 09_ser_
```

### 恢复测试

本地恢复测试通过外部 `SIGKILL` 终止服务端，验证服务重启后的数据和索引状态。

## 结果判定

查询必须返回一组完整的 `META → ROW* → RESULT_END`；命令成功返回 `COMMAND_OK`；可重试事务冲突在完整回滚后返回 `TRANSACTION_ABORT`；其他错误返回 `ERROR`。

静态用例默认忽略无显式排序保证的行顺序。需要验证 `ORDER BY`、事务输出或依赖顺序的用例保留结果顺序。FLOAT 静态输出格式化为六位小数；协议和金额精度专项测试直接检查 FLOAT32 位模式。

失败时可使用：

```bash
python3 tests/run_doc_tests.py <case> --keep-db --show-server-log
```

`--keep-db` 会保留测试数据库，`--show-server-log` 会把日志尾部加入失败信息。运行结束后不要提交数据库目录、日志、缓存或生成二进制。

## 测试纪律

- 不并行运行任何需要服务端的 Python 测试。
- 不根据用例名称或 SQL 文本硬编码数据库行为。
- 不修改期望结果掩盖实现错误。
- 新功能先增加最小回归，再运行相邻分组和完整主套件。
- 本地测试只验证当前用例覆盖的语义。
