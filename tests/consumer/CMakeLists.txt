cmake_minimum_required(VERSION 3.28)

project(liquid-consumer LANGUAGES C CXX)

if(NOT DEFINED LIQUID_SOURCE_DIR)
  message(FATAL_ERROR "LIQUID_SOURCE_DIR is required")
endif()

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(BOOST_CAPY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BOOST_CAPY_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BOOST_CAPY_BUILD_BENCH OFF CACHE BOOL "" FORCE)
add_subdirectory("${LIQUID_SOURCE_DIR}/modules/capy" capy EXCLUDE_FROM_ALL)

set(BOOST_COROSIO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BOOST_COROSIO_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BOOST_COROSIO_BUILD_PERF OFF CACHE BOOL "" FORCE)
add_subdirectory("${LIQUID_SOURCE_DIR}/modules/corosio" corosio EXCLUDE_FROM_ALL)

include(FetchContent)
if(NOT TARGET nlohmann_json::nlohmann_json)
  FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  set(JSON_BuildTests OFF CACHE INTERNAL "")
  FetchContent_MakeAvailable(nlohmann_json)
endif()

set(LIQUID_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LIQUID_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIQUID_BUILD_BENCH OFF CACHE BOOL "" FORCE)
add_subdirectory("${LIQUID_SOURCE_DIR}" liquid EXCLUDE_FROM_ALL)

add_executable(liquid-consumer main.cpp)
target_link_libraries(liquid-consumer PRIVATE liquid::liquid nlohmann_json::nlohmann_json)
