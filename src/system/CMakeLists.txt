set(SOURCES sm_manager.cpp maintenance_engine.cpp)
add_library(system STATIC ${SOURCES})
target_link_libraries(system index record)
