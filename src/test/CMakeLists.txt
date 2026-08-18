# 确定性并发 barrier 测试:验证索引范围扫描在叶页插入/删除/分裂下的正确性。
# 需 Linux 环境构建运行(项目整体仅支持 Linux)。
add_executable(index_concurrency_test index_concurrency_test.cpp)
target_link_libraries(index_concurrency_test index storage transaction gtest_main pthread)
