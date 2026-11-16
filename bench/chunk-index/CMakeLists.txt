# SPDX-License-Identifier: MIT

include(FetchContent)
enable_language(C)

set(WITH_GFLAGS OFF CACHE BOOL "" FORCE)
set(WITH_TESTS OFF CACHE BOOL "" FORCE)
set(WITH_TOOLS OFF CACHE BOOL "" FORCE)
set(WITH_BENCHMARK_TOOLS OFF CACHE BOOL "" FORCE)
set(WITH_CORE_TOOLS OFF CACHE BOOL "" FORCE)
set(WITH_TRACE_TOOLS OFF CACHE BOOL "" FORCE)
set(ROCKSDB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(FAIL_ON_WARNINGS OFF CACHE BOOL "" FORCE)
set(WITH_SNAPPY OFF CACHE BOOL "" FORCE)
set(WITH_LZ4 OFF CACHE BOOL "" FORCE)
set(WITH_ZLIB OFF CACHE BOOL "" FORCE)
set(WITH_ZSTD OFF CACHE BOOL "" FORCE)
set(WITH_BZ2 OFF CACHE BOOL "" FORCE)

FetchContent_Declare(rocksdb
  GIT_REPOSITORY https://github.com/facebook/rocksdb.git
  GIT_TAG        v10.10.1
  GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(rocksdb)

FetchContent_Declare(lmdb
  GIT_REPOSITORY https://github.com/LMDB/lmdb.git
  GIT_TAG        LMDB_0.9.36
  GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(lmdb)

add_library(chunk-index-lmdb STATIC
  ${lmdb_SOURCE_DIR}/libraries/liblmdb/mdb.c
  ${lmdb_SOURCE_DIR}/libraries/liblmdb/midl.c)
target_include_directories(chunk-index-lmdb PUBLIC
  ${lmdb_SOURCE_DIR}/libraries/liblmdb)
set_target_properties(chunk-index-lmdb PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS ON)

add_executable(chunk-index-bench chunk_index_bench.cpp)
target_compile_features(chunk-index-bench PRIVATE cxx_std_23)
target_include_directories(chunk-index-bench PRIVATE ${PROJECT_SOURCE_DIR}/src/store)
target_link_libraries(chunk-index-bench PRIVATE liquid::liquid rocksdb chunk-index-lmdb Boost::corosio)
