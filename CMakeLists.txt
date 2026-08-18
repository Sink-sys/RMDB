cmake_minimum_required(VERSION 3.16)
project(RMDB)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "RMDB competition builds support Linux only")
endif()

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type")
endif()
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall")


enable_testing()

# Use glibc malloc by default. jemalloc remains available for local profiling,
# but allocator choice changes benchmark results.
option(RMDB_USE_JEMALLOC "Link jemalloc for local profiling" OFF)
if(RMDB_USE_JEMALLOC)
    find_library(RMDB_JEMALLOC_LIB NAMES libjemalloc.a jemalloc libjemalloc.so.2)
    if(RMDB_JEMALLOC_LIB)
        message(STATUS "RMDB: linking jemalloc (${RMDB_JEMALLOC_LIB})")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${RMDB_JEMALLOC_LIB}")
    else()
        message(STATUS "RMDB: jemalloc not found; using glibc malloc")
    endif()
else()
    message(STATUS "RMDB: using glibc malloc (RMDB_USE_JEMALLOC=ON to enable jemalloc)")
endif()

add_subdirectory(src)
add_subdirectory(deps)
