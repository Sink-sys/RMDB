set(SOURCES concurrency/lock_manager.cpp serializable_metadata_retention.cpp snapshot_index_history_store.cpp
    transaction_gc.cpp
    transaction_lifecycle.cpp transaction_manager.cpp transaction_ssi.cpp transaction_visibility.cpp watermark.cpp)
add_library(transaction STATIC ${SOURCES})
target_link_libraries(transaction system recovery pthread)
