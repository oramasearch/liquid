cmake_minimum_required(VERSION 3.28)

# liquid is C++26. Capy/Corosio (when they arrive) need Clang 17+, so prefer a
# modern Clang before the project() call — the same auto-selection Zetta uses.
if(NOT DEFINED CMAKE_CXX_COMPILER AND NOT LIQUID_USE_SYSTEM_COMPILER)
  if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin"
     AND EXISTS "/opt/homebrew/opt/llvm/bin/clang++")
    set(CMAKE_C_COMPILER   "/opt/homebrew/opt/llvm/bin/clang"   CACHE FILEPATH "")
    set(CMAKE_CXX_COMPILER "/opt/homebrew/opt/llvm/bin/clang++" CACHE FILEPATH "")
    if(NOT CMAKE_OSX_SYSROOT)
      execute_process(COMMAND xcrun --show-sdk-path
                      OUTPUT_VARIABLE LIQUID_SDKROOT OUTPUT_STRIP_TRAILING_WHITESPACE)
      set(CMAKE_OSX_SYSROOT "${LIQUID_SDKROOT}" CACHE PATH "")
    endif()
  elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    foreach(LIQUID_VER 22 21 20 19 18 17)
      find_program(LIQUID_CLANGXX_${LIQUID_VER} "clang++-${LIQUID_VER}")
      if(LIQUID_CLANGXX_${LIQUID_VER})
        set(CMAKE_CXX_COMPILER "${LIQUID_CLANGXX_${LIQUID_VER}}" CACHE FILEPATH "")
        find_program(LIQUID_CLANGC_${LIQUID_VER} "clang-${LIQUID_VER}")
        if(LIQUID_CLANGC_${LIQUID_VER})
          set(CMAKE_C_COMPILER "${LIQUID_CLANGC_${LIQUID_VER}}" CACHE FILEPATH "")
        endif()
        break()
      endif()
    endforeach()
  endif()
endif()

project(liquid VERSION 0.0.1 LANGUAGES C CXX
        DESCRIPTION "liquid — version control, build, packages, artifacts and runtime in one program")

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# libliquid is the product; the `liquid` binary is a thin client of it, and so are liquify
# and lake. Static by default (one binary to ship); -DBUILD_SHARED_LIBS=ON builds libliquid.so.
option(LIQUID_BUILD_CLI   "Build the liquid command-line binary" ON)
option(LIQUID_BUILD_TESTS "Build the test suite" ${PROJECT_IS_TOP_LEVEL})
option(LIQUID_BUILD_BENCH "Build optional benchmarks" OFF)
option(BUILD_SHARED_LIBS  "Build libliquid as a shared library" OFF)
if(LIQUID_BUILD_BENCH AND NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# -----------------------------------------------------------------------------
# libliquid — everything under include/liquid/ is the public API.
# -----------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(blake3_source
  GIT_REPOSITORY https://github.com/BLAKE3-team/BLAKE3.git
  GIT_TAG        1.8.7
  SOURCE_SUBDIR  c/liquid-no-upstream-cmake)
FetchContent_MakeAvailable(blake3_source)

add_library(liquid_blake3_objects OBJECT
  ${blake3_source_SOURCE_DIR}/c/blake3.c
  ${blake3_source_SOURCE_DIR}/c/blake3_dispatch.c
  ${blake3_source_SOURCE_DIR}/c/blake3_portable.c)
target_include_directories(liquid_blake3_objects SYSTEM PRIVATE
  ${blake3_source_SOURCE_DIR}/c)
target_compile_definitions(liquid_blake3_objects PRIVATE
  BLAKE3_API=
  BLAKE3_PRIVATE=
  BLAKE3_USE_NEON=0
  BLAKE3_NO_SSE2
  BLAKE3_NO_SSE41
  BLAKE3_NO_AVX2
  BLAKE3_NO_AVX512)
set_target_properties(liquid_blake3_objects PROPERTIES
  C_EXTENSIONS OFF
  C_VISIBILITY_PRESET hidden
  POSITION_INDEPENDENT_CODE ON)
target_compile_features(liquid_blake3_objects PRIVATE c_std_99)

# Keep the upstream C implementation as a private static target. Its object files are also
# folded into libliquid so static-package consumers have no transitive dependency.
add_library(liquid_blake3 STATIC $<TARGET_OBJECTS:liquid_blake3_objects>)
set_target_properties(liquid_blake3 PROPERTIES
  C_VISIBILITY_PRESET hidden
  POSITION_INDEPENDENT_CODE ON)

add_library(libliquid
  src/drip.cpp
  src/error.cpp
  src/hash.cpp
  src/hash/sha256.c
  src/identity.cpp
  src/object.cpp
  src/remote.cpp
  src/repository.cpp
  src/store.cpp
  src/version.cpp
  src/vfs.cpp
)
add_library(liquid::liquid ALIAS libliquid)
set_target_properties(libliquid PROPERTIES
  OUTPUT_NAME liquid            # libliquid.a / libliquid.dylib / libliquid.so
  EXPORT_NAME liquid            # find_package(liquid) → liquid::liquid
  VERSION     ${PROJECT_VERSION}
  SOVERSION   0
  C_VISIBILITY_PRESET hidden)
target_include_directories(libliquid PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
  $<INSTALL_INTERFACE:include>
  PRIVATE ${blake3_source_SOURCE_DIR}/c)
target_compile_features(libliquid PUBLIC cxx_std_26)
if(BUILD_SHARED_LIBS)
  target_link_libraries(libliquid PRIVATE liquid_blake3)
else()
  target_sources(libliquid PRIVATE $<TARGET_OBJECTS:liquid_blake3_objects>)
endif()
include(GenerateExportHeader)
generate_export_header(libliquid
  BASE_NAME liquid
  EXPORT_FILE_NAME ${CMAKE_CURRENT_BINARY_DIR}/include/liquid/export.hpp)
if(NOT BUILD_SHARED_LIBS)
  target_compile_definitions(libliquid PUBLIC LIQUID_STATIC_DEFINE)
endif()

# -----------------------------------------------------------------------------
# The CLI — a thin client of libliquid.
# -----------------------------------------------------------------------------
if(LIQUID_BUILD_CLI)
  add_executable(liquid-cli src/main.cpp)
  set_target_properties(liquid-cli PROPERTIES OUTPUT_NAME liquid)
  target_link_libraries(liquid-cli PRIVATE liquid::liquid)
endif()

# -----------------------------------------------------------------------------
# Install + package config: find_package(liquid CONFIG) → liquid::liquid.
# -----------------------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
install(TARGETS libliquid EXPORT liquidTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ ${CMAKE_CURRENT_BINARY_DIR}/include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(LIQUID_BUILD_CLI)
  install(TARGETS liquid-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(EXPORT liquidTargets NAMESPACE liquid::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/liquid)
configure_package_config_file(cmake/liquidConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/liquidConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/liquid)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/liquidConfigVersion.cmake
  COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/liquidConfig.cmake
              ${CMAKE_CURRENT_BINARY_DIR}/liquidConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/liquid)

# -----------------------------------------------------------------------------
# Tests: GoogleTest via FetchContent + CTest, matching Zetta.
# -----------------------------------------------------------------------------
if(LIQUID_BUILD_TESTS)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.17.0)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  enable_testing()
  add_subdirectory(tests)
endif()

# Optional benchmarks have their own dependencies and never enter the default build.
if(LIQUID_BUILD_BENCH)
  add_subdirectory(bench/chunk-index)
endif()
