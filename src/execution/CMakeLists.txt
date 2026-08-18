set(SOURCES execution_manager.cpp index_visibility_cursor.cpp load_manager.cpp visible_tuple_ref.cpp)
add_library(execution STATIC ${SOURCES})

target_link_libraries(execution system record transaction planner)
