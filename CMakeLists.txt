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
  elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
    find_program(LIQUID_CLANGCL "clang-cl")
    if(LIQUID_CLANGCL)
      set(CMAKE_C_COMPILER "${LIQUID_CLANGCL}" CACHE FILEPATH "")
      set(CMAKE_CXX_COMPILER "${LIQUID_CLANGCL}" CACHE FILEPATH "")
    endif()
  endif()

  if(NOT DEFINED CMAKE_CXX_COMPILER)
    message(FATAL_ERROR
      "liquid requires a modern Clang compiler. Install Homebrew LLVM on macOS, "
      "clang++-17 or newer on Linux, or clang-cl on Windows; alternatively set "
      "CMAKE_CXX_COMPILER explicitly or enable LIQUID_USE_SYSTEM_COMPILER.")
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
option(LIQUID_BUILD_FRONTENDS "Build bundled frontend modules" ON)
option(LIQUID_SELF_HOST   "Configure a network-free liquid wrap build step" OFF)
option(BUILD_SHARED_LIBS  "Build libliquid as a shared library" OFF)
if(LIQUID_SELF_HOST)
  set(CMAKE_SUPPRESS_REGENERATION ON)
endif()
if(LIQUID_BUILD_BENCH AND NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# -----------------------------------------------------------------------------
# libliquid — everything under include/liquid/ is the public API.
# -----------------------------------------------------------------------------
include(FetchContent)
find_package(ZLIB REQUIRED)

if(NOT TARGET Boost::capy)
  set(BOOST_CAPY_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
  set(BOOST_CAPY_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(BOOST_CAPY_BUILD_BENCH    OFF CACHE BOOL "" FORCE)
  add_subdirectory(modules/capy EXCLUDE_FROM_ALL)
endif()

if(APPLE)
  list(APPEND CMAKE_PREFIX_PATH /opt/homebrew/opt/openssl@3)
endif()
find_package(OpenSSL REQUIRED)
if(NOT TARGET Boost::corosio)
  set(BOOST_COROSIO_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
  set(BOOST_COROSIO_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(BOOST_COROSIO_BUILD_PERF     OFF CACHE BOOL "" FORCE)
  add_subdirectory(modules/corosio EXCLUDE_FROM_ALL)
endif()

# Boost.HTTP expects its Boost component targets to exist before it is added.
# Match lake's data-plane dependency set and pin so both sides compile the same
# protocol implementation.
set(BOOST_INCLUDE_LIBRARIES
  json url assert config core mp11 system throw_exception type_traits winapi)
set(BOOST_ENABLE_CMAKE ON)
FetchContent_Declare(boost_core_libs
  URL https://github.com/boostorg/boost/releases/download/boost-1.91.0-1/boost-1.91.0-1-cmake.tar.xz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(boost_core_libs)
set(BOOST_HTTP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BOOST_HTTP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(modules/http EXCLUDE_FROM_ALL)
set_target_properties(boost_http PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/modules/http/include>;$<INSTALL_INTERFACE:include>")

set(LIQUID_FUSE_TARGET "")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIQUID_FUSE3 REQUIRED IMPORTED_TARGET fuse3)
  set(LIQUID_FUSE_TARGET PkgConfig::LIQUID_FUSE3)
  set(LIQUID_FUSE_DEFINITION LIQUID_HAS_FUSE3=1)
elseif(APPLE)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(LIQUID_FUSE3 QUIET IMPORTED_TARGET fuse3)
    pkg_check_modules(LIQUID_MACFUSE QUIET IMPORTED_TARGET fuse)
  endif()
  if(LIQUID_FUSE3_FOUND)
    set(LIQUID_FUSE_TARGET PkgConfig::LIQUID_FUSE3)
    set(LIQUID_FUSE_DEFINITION LIQUID_HAS_FUSE3=1)
  elseif(LIQUID_MACFUSE_FOUND)
    set(LIQUID_FUSE_TARGET PkgConfig::LIQUID_MACFUSE)
    set(LIQUID_FUSE_DEFINITION LIQUID_HAS_FUSE2=1 _FILE_OFFSET_BITS=64)
  else()
    find_path(LIQUID_MACFUSE_INCLUDE_DIR fuse.h
      PATHS /usr/local/include/fuse /opt/homebrew/include/fuse)
    find_library(LIQUID_MACFUSE_LIBRARY fuse
      PATHS /usr/local/lib /opt/homebrew/lib)
    if(LIQUID_MACFUSE_INCLUDE_DIR AND LIQUID_MACFUSE_LIBRARY)
      add_library(liquid_macfuse UNKNOWN IMPORTED)
      set_target_properties(liquid_macfuse PROPERTIES
        IMPORTED_LOCATION "${LIQUID_MACFUSE_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${LIQUID_MACFUSE_INCLUDE_DIR}")
      set(LIQUID_FUSE_TARGET liquid_macfuse)
      set(LIQUID_FUSE_DEFINITION LIQUID_HAS_FUSE2=1 _FILE_OFFSET_BITS=64)
    endif()
  endif()
endif()

FetchContent_Declare(blake3_source
  GIT_REPOSITORY https://github.com/BLAKE3-team/BLAKE3.git
  GIT_TAG        1.8.7
  SOURCE_SUBDIR  c/liquid-no-upstream-cmake)
FetchContent_MakeAvailable(blake3_source)

# Monocypher is a private, pinned single-file C crypto dependency. It supplies
# XChaCha20-Poly1305 and Argon2i; BLAKE3 remains the identity implementation.
FetchContent_Declare(monocypher_source
  URL      https://monocypher.org/download/monocypher-4.0.3.tar.gz
  URL_HASH SHA256=8cc9bc341a66249016db9bd70e9142d8d0aef9945973744b1ac05dbc55d8ee66)
FetchContent_MakeAvailable(monocypher_source)

# Compression is a private codec dependency of the pack format.  Do not expose
# its headers or targets through libliquid's public interface.
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zstd_source
  GIT_REPOSITORY https://github.com/facebook/zstd.git
  GIT_TAG        v1.5.7
  GIT_SHALLOW    TRUE
  SOURCE_SUBDIR  build/cmake)
FetchContent_MakeAvailable(zstd_source)

if(LIQUID_BUILD_FRONTENDS)
  FetchContent_Declare(tree_sitter_source
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter.git
    GIT_TAG        v0.25.9
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  liquid-no-upstream-cmake)
  FetchContent_Declare(tree_sitter_c_source
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-c.git
    GIT_TAG        v0.24.1
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  liquid-no-upstream-cmake)
  FetchContent_Declare(tree_sitter_cpp_source
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-cpp.git
    GIT_TAG        v0.23.4
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  liquid-no-upstream-cmake)
  FetchContent_MakeAvailable(tree_sitter_source tree_sitter_c_source tree_sitter_cpp_source)

  add_library(liquid_frontend_syntax STATIC
    modules/frontends/common/cbor.cpp
    modules/frontends/common/syntax.cpp
    ${tree_sitter_source_SOURCE_DIR}/lib/src/lib.c
    ${tree_sitter_c_source_SOURCE_DIR}/src/parser.c
    ${tree_sitter_cpp_source_SOURCE_DIR}/src/parser.c
    ${tree_sitter_cpp_source_SOURCE_DIR}/src/scanner.c)
  target_include_directories(liquid_frontend_syntax PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/frontends
    ${tree_sitter_source_SOURCE_DIR}/lib/include
    ${tree_sitter_source_SOURCE_DIR}/lib/src
    ${tree_sitter_c_source_SOURCE_DIR}/src
    ${tree_sitter_cpp_source_SOURCE_DIR}/src)
  target_compile_features(liquid_frontend_syntax PRIVATE c_std_11 cxx_std_26)
  target_compile_definitions(liquid_frontend_syntax PRIVATE TREE_SITTER_HIDE_SYMBOLS)
  set_target_properties(liquid_frontend_syntax PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

  add_library(liquid-frontend-tree-sitter MODULE modules/frontends/tree_sitter/module.cpp)
  target_include_directories(liquid-frontend-tree-sitter PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/frontends)
  target_link_libraries(liquid-frontend-tree-sitter PRIVATE liquid_frontend_syntax)
  set_target_properties(liquid-frontend-tree-sitter PROPERTIES
    PREFIX ""
    OUTPUT_NAME "liquid-frontend-tree-sitter"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/modules/frontends/tree-sitter"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/modules/frontends/tree-sitter"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

  add_library(liquid-frontend-clang MODULE modules/frontends/clang/module.cpp)
  target_include_directories(liquid-frontend-clang PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/frontends)
  target_link_libraries(liquid-frontend-clang PRIVATE liquid_frontend_syntax)
  set_target_properties(liquid-frontend-clang PROPERTIES
    PREFIX ""
    OUTPUT_NAME "liquid-frontend-clang"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/modules/frontends/clang"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/modules/frontends/clang"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

  add_library(liquid-frontend-clang-static STATIC modules/frontends/clang/module.cpp)
  target_include_directories(liquid-frontend-clang-static PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/frontends)
  target_link_libraries(liquid-frontend-clang-static PRIVATE liquid_frontend_syntax)
  set_target_properties(liquid-frontend-clang-static PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
  if(APPLE)
    target_link_options(liquid-frontend-tree-sitter PRIVATE
      "LINKER:-exported_symbol,_liquid_frontend_get_module_v1")
    target_link_options(liquid-frontend-clang PRIVATE
      "LINKER:-exported_symbol,_liquid_frontend_get_module_v1")
  elseif(UNIX)
    target_link_options(liquid-frontend-tree-sitter PRIVATE
      "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/modules/frontends/exports.map")
    target_link_options(liquid-frontend-clang PRIVATE
      "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/modules/frontends/exports.map")
  endif()
endif()

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

add_library(liquid_monocypher_objects OBJECT
  ${monocypher_source_SOURCE_DIR}/src/monocypher.c
  ${monocypher_source_SOURCE_DIR}/src/optional/monocypher-ed25519.c)
target_include_directories(liquid_monocypher_objects SYSTEM PRIVATE
  ${monocypher_source_SOURCE_DIR}/src
  ${monocypher_source_SOURCE_DIR}/src/optional)
set_target_properties(liquid_monocypher_objects PROPERTIES
  C_EXTENSIONS OFF
  C_VISIBILITY_PRESET hidden
  POSITION_INDEPENDENT_CODE ON)
target_compile_features(liquid_monocypher_objects PRIVATE c_std_99)
add_library(liquid_monocypher STATIC $<TARGET_OBJECTS:liquid_monocypher_objects>)
set_target_properties(liquid_monocypher PROPERTIES
  C_VISIBILITY_PRESET hidden
  POSITION_INDEPENDENT_CODE ON)

add_library(liquid-wrap-hook SHARED src/sandbox/wrap_hook.c)
set_target_properties(liquid-wrap-hook PROPERTIES
  OUTPUT_NAME liquid-wrap-hook
  C_VISIBILITY_PRESET hidden
  POSITION_INDEPENDENT_CODE ON)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(liquid-read-guard src/sandbox/read_guard.c)
  target_link_options(liquid-read-guard PRIVATE -static)
  target_compile_features(liquid-read-guard PRIVATE c_std_99)
endif()

# automerge-cpp is the private CRDT engine. Keep it static even when libliquid is shared;
# no dependency header or target is part of liquid's installed interface.
set(LIQUID_SAVED_BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF)
set(AUTOMERGE_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(AUTOMERGE_CPP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(AUTOMERGE_CPP_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(AUTOMERGE_CPP_BUILD_FUZZ OFF CACHE BOOL "" FORCE)
block()
  # automerge-cpp always configures its bundled JSON checkout. Keep that header-only target private
  # when the consumer already owns the canonical target that automerge-cpp links.
  if(TARGET nlohmann_json::nlohmann_json)
    set(NLOHMANN_JSON_TARGET_NAME liquid_vendored_nlohmann_json)
  endif()
  add_subdirectory(modules/automerge-cpp EXCLUDE_FROM_ALL)
endblock()
set(BUILD_SHARED_LIBS ${LIQUID_SAVED_BUILD_SHARED_LIBS})
set_target_properties(automerge-cpp PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  POSITION_INDEPENDENT_CODE ON
  EXCLUDE_FROM_ALL ON)

add_library(liquid_metadata_objects OBJECT
  src/crdt/document.cpp
  src/crdt/journal.cpp
  src/crdt/schema.cpp
  src/encoding/cbor.cpp)
target_include_directories(liquid_metadata_objects PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_BINARY_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(liquid_metadata_objects PRIVATE cxx_std_26)
target_link_libraries(liquid_metadata_objects PRIVATE automerge-cpp::automerge-cpp)
set_target_properties(liquid_metadata_objects PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  POSITION_INDEPENDENT_CODE ON)

add_library(libliquid
  src/attestation/registry.cpp
  src/build/cache.cpp
  src/build/executor.cpp
  src/build/distribution.cpp
  src/build/enumeration.cpp
  src/build/facts.cpp
  src/build/verify.cpp
  src/build/materialise.cpp
  src/build/provenance.cpp
  src/build/trace.cpp
  src/build/why.cpp
  src/build/policy.cpp
  src/build/sbom.cpp
  src/build/wrap.cpp
  src/capture/coalesce.cpp
  src/capture/cursor.cpp
  src/capture/reconcile.cpp
  src/daemon.cpp
  src/daemon/fanout.cpp
  src/drip.cpp
  src/delta.cpp
  src/encryption/leaf.cpp
  src/error.cpp
  src/formats/dotenv.cpp
  src/formats/json.cpp
  src/frontend/graph.cpp
  src/frontend/module.cpp
  src/graph.cpp
  src/hash.cpp
  src/hash/sha256.c
  src/http2/frame.cpp
  src/http2/grpc.cpp
  src/http2/hpack.cpp
  src/http2/connection.cpp
  src/git/export.cpp
  src/git/import.cpp
  src/git/object_store.cpp
  src/git/sha1.cpp
  src/identity.cpp
  src/identity/taap.cpp
  src/crypto/crypto.cpp
  src/object.cpp
  src/objects/records.cpp
  src/protobuf/wire.cpp
  src/reapi/conversion.cpp
  src/reapi/client.cpp
  src/reapi/messages.cpp
  src/reapi/remote_client.cpp
  src/reapi/server.cpp
  src/repository/diff.cpp
  src/repository/merge.cpp
  src/remote.cpp
  src/repository/repository_impl.cpp
  src/runtime/runtime.cpp
  src/sandbox/environment.cpp
  src/scan/detectors.cpp
  src/store.cpp
  src/store/chunker.cpp
  src/store/collector.cpp
  src/store/compactor.cpp
  src/store/faults.cpp
  src/store/index.cpp
  src/store/leases.cpp
  src/store/mapped_file.cpp
  src/store/pack.cpp
  src/store/references.cpp
  src/store/store_impl.cpp
  src/store/subject_index.cpp
  src/sync/codec.cpp
  src/sync/endpoint.cpp
  src/sync/server.cpp
  src/sync/session.cpp
  src/version.cpp
  src/vfs.cpp
  src/vfs/fuse_fs.cpp
  src/vfs/hydration.cpp
  src/vfs/materialise.cpp
  src/vfs/observation.cpp
  src/vfs/request_queue.cpp
)
if(APPLE)
  target_sources(libliquid PRIVATE src/capture/macos_watcher.cpp src/sandbox/macos.cpp)
  target_link_libraries(libliquid PRIVATE "-framework CoreServices")
elseif(UNIX)
  target_sources(libliquid PRIVATE src/capture/linux_watcher.cpp src/sandbox/linux.cpp)
elseif(WIN32)
  target_sources(libliquid PRIVATE src/capture/windows_watcher.cpp src/sandbox/windows.cpp)
endif()
target_sources(libliquid PRIVATE $<TARGET_OBJECTS:liquid_metadata_objects>)
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
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
          ${blake3_source_SOURCE_DIR}/c
          ${monocypher_source_SOURCE_DIR}/src
          ${monocypher_source_SOURCE_DIR}/src/optional)
target_compile_features(libliquid PUBLIC cxx_std_26)
target_compile_definitions(libliquid PRIVATE
  LIQUID_WRAP_HOOK_PATH="$<TARGET_FILE:liquid-wrap-hook>"
  LIQUID_WRAP_HOOK_FILENAME="$<TARGET_FILE_NAME:liquid-wrap-hook>")
add_dependencies(libliquid liquid-wrap-hook)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_compile_definitions(libliquid PRIVATE
    LIQUID_READ_GUARD_PATH="$<TARGET_FILE:liquid-read-guard>"
    LIQUID_READ_GUARD_FILENAME="$<TARGET_FILE_NAME:liquid-read-guard>")
  add_dependencies(libliquid liquid-read-guard)
endif()
target_link_libraries(libliquid
  PUBLIC Boost::capy
  PRIVATE Boost::corosio Boost::corosio_openssl Boost::http libzstd_static ZLIB::ZLIB
          ${CMAKE_DL_LIBS} $<BUILD_INTERFACE:automerge-cpp::automerge-cpp>)
if(LIQUID_FUSE_TARGET)
  target_compile_definitions(libliquid PRIVATE ${LIQUID_FUSE_DEFINITION})
  target_link_libraries(libliquid PRIVATE ${LIQUID_FUSE_TARGET})
endif()
if(BUILD_SHARED_LIBS)
  target_link_libraries(libliquid PRIVATE liquid_blake3 liquid_monocypher)
else()
  target_sources(libliquid PRIVATE $<TARGET_OBJECTS:liquid_blake3_objects>
                            $<TARGET_OBJECTS:liquid_monocypher_objects>)
endif()
if(WIN32)
  target_link_libraries(libliquid PRIVATE bcrypt)
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
  add_executable(liquid-cli
    src/main.cpp
    src/cli/anchor.cpp
    src/cli/attestation.cpp
    src/cli/capture.cpp
    src/cli/daemon.cpp
    src/cli/compose.cpp
    src/cli/delta.cpp
    src/cli/gc.cpp
    src/cli/facts.cpp
    src/cli/git.cpp
    src/cli/history.cpp
    src/cli/identity.cpp
    src/cli/locks.cpp
    src/cli/policy.cpp
    src/cli/provenance.cpp
    src/cli/sbom.cpp
    src/cli/serve.cpp
    src/cli/sync.cpp
    src/cli/serve.cpp
    src/cli/repo.cpp
    src/cli/trace.cpp
    src/cli/retain.cpp
    src/cli/tree.cpp
    src/cli/verify.cpp
    src/cli/vfs.cpp
    src/cli/why.cpp
    src/cli/wrap.cpp)
  set_target_properties(liquid-cli PROPERTIES OUTPUT_NAME liquid)
  target_link_libraries(liquid-cli PRIVATE
    liquid::liquid Boost::corosio Boost::http)
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
install(TARGETS liquid-wrap-hook
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/liquid
  RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/liquid)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  install(TARGETS liquid-read-guard
    RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/liquid)
endif()
if(LIQUID_BUILD_FRONTENDS)
  install(TARGETS liquid-frontend-tree-sitter liquid-frontend-clang
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/liquid/frontends
    RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/liquid/frontends)
endif()
install(TARGETS boost_capy EXPORT liquidTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS boost_corosio EXPORT liquidTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS boost_corosio_openssl boost_http EXPORT liquidTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS
  boost_align boost_assert boost_compat boost_config boost_container
  boost_container_hash boost_core boost_describe boost_endian boost_json
  boost_intrusive boost_move boost_mp11 boost_optional boost_predef boost_system
  boost_throw_exception boost_type_traits boost_url boost_variant2 boost_winapi
  EXPORT liquidTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ ${CMAKE_CURRENT_BINARY_DIR}/include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY modules/capy/include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY modules/http/include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
foreach(LIQUID_BOOST_LIBRARY
    align assert compat config container container_hash core describe endian json
    intrusive move mp11 optional predef static_assert system throw_exception type_traits url
    variant2 winapi)
  install(DIRECTORY
    ${boost_core_libs_SOURCE_DIR}/libs/${LIQUID_BOOST_LIBRARY}/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endforeach()
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
