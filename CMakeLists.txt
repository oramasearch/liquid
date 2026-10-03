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

file(READ
  "${CMAKE_CURRENT_SOURCE_DIR}/src/projection/fixtures/debian-bookworm-path-packages.tsv"
  LIQUID_DEBIAN_PATH_PACKAGES)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/projection/debian_path_packages.hpp.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/debian_path_packages.hpp"
  @ONLY)

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
include(cmake/Grammars.cmake)
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

# AppImage type 2 runtimes are private, vendored, byte-pinned projection inputs. Keep both
# supported architectures available so cross-projection does not consult the network.
set(LIQUID_APPIMAGE_RUNTIME_SOURCE "")
if(NOT WIN32)
  enable_language(ASM)
  set(LIQUID_APPIMAGE_RUNTIME_VENDOR_DIRECTORY
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/appimage-runtimes")
  set(LIQUID_APPIMAGE_RUNTIME_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/appimage-runtimes")
  file(MAKE_DIRECTORY "${LIQUID_APPIMAGE_RUNTIME_DIRECTORY}")
  set(LIQUID_APPIMAGE_RUNTIME_X86_64
    "${LIQUID_APPIMAGE_RUNTIME_DIRECTORY}/runtime-x86_64")
  set(LIQUID_APPIMAGE_RUNTIME_AARCH64
    "${LIQUID_APPIMAGE_RUNTIME_DIRECTORY}/runtime-aarch64")
  file(DOWNLOAD
    "file://${LIQUID_APPIMAGE_RUNTIME_VENDOR_DIRECTORY}/runtime-x86_64"
    "${LIQUID_APPIMAGE_RUNTIME_X86_64}"
    EXPECTED_HASH SHA256=1cc49bcf1e2ccd593c379adb17c9f85a36d619088296504de95b1d06215aebbf
    TLS_VERIFY ON
    STATUS LIQUID_APPIMAGE_RUNTIME_X86_64_STATUS)
  list(GET LIQUID_APPIMAGE_RUNTIME_X86_64_STATUS 0 LIQUID_APPIMAGE_RUNTIME_X86_64_CODE)
  if(NOT LIQUID_APPIMAGE_RUNTIME_X86_64_CODE EQUAL 0)
    list(GET LIQUID_APPIMAGE_RUNTIME_X86_64_STATUS 1 LIQUID_APPIMAGE_RUNTIME_X86_64_MESSAGE)
    message(FATAL_ERROR "Cannot load the vendored x86-64 AppImage runtime: ${LIQUID_APPIMAGE_RUNTIME_X86_64_MESSAGE}")
  endif()
  file(DOWNLOAD
    "file://${LIQUID_APPIMAGE_RUNTIME_VENDOR_DIRECTORY}/runtime-aarch64"
    "${LIQUID_APPIMAGE_RUNTIME_AARCH64}"
    EXPECTED_HASH SHA256=7d5d772b7c32f0c84caf0a452a3072a5709027d7eac5856feb89a7a7a8881372
    TLS_VERIFY ON
    STATUS LIQUID_APPIMAGE_RUNTIME_AARCH64_STATUS)
  list(GET LIQUID_APPIMAGE_RUNTIME_AARCH64_STATUS 0 LIQUID_APPIMAGE_RUNTIME_AARCH64_CODE)
  if(NOT LIQUID_APPIMAGE_RUNTIME_AARCH64_CODE EQUAL 0)
    list(GET LIQUID_APPIMAGE_RUNTIME_AARCH64_STATUS 1 LIQUID_APPIMAGE_RUNTIME_AARCH64_MESSAGE)
    message(FATAL_ERROR "Cannot load the vendored AArch64 AppImage runtime: ${LIQUID_APPIMAGE_RUNTIME_AARCH64_MESSAGE}")
  endif()
  file(TO_CMAKE_PATH "${LIQUID_APPIMAGE_RUNTIME_X86_64}" LIQUID_APPIMAGE_RUNTIME_X86_64_ASM)
  file(TO_CMAKE_PATH "${LIQUID_APPIMAGE_RUNTIME_AARCH64}" LIQUID_APPIMAGE_RUNTIME_AARCH64_ASM)
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
  configure_file(cmake/appimage_runtimes.S.in
    "${CMAKE_CURRENT_BINARY_DIR}/generated/appimage_runtimes.S" @ONLY)
  set(LIQUID_APPIMAGE_RUNTIME_SOURCE
    "${CMAKE_CURRENT_BINARY_DIR}/generated/appimage_runtimes.S")
endif()

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
  liquid_configure_grammars(
    OUT_RUNTIME_SOURCE LIQUID_TREE_SITTER_RUNTIME_SOURCE
    OUT_RUNTIME_INCLUDE LIQUID_TREE_SITTER_RUNTIME_INCLUDE
    OUT_SOURCES LIQUID_GRAMMAR_SOURCES
    OUT_INCLUDES LIQUID_GRAMMAR_INCLUDES
    OUT_GENERATED LIQUID_GRAMMAR_GENERATED_SOURCE
    OUT_SOURCE_MAP LIQUID_GRAMMAR_SOURCE_MAP)

  add_library(liquid_frontend_syntax STATIC
    modules/frontends/common/cbor.cpp
    modules/frontends/common/syntax.cpp
    ${LIQUID_GRAMMAR_GENERATED_SOURCE}
    ${LIQUID_TREE_SITTER_RUNTIME_SOURCE}
    ${LIQUID_GRAMMAR_SOURCES})
  target_include_directories(liquid_frontend_syntax PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/frontends
    ${LIQUID_TREE_SITTER_RUNTIME_INCLUDE}
    ${LIQUID_GRAMMAR_INCLUDES})
  target_compile_features(liquid_frontend_syntax PRIVATE c_std_11 cxx_std_26)
  target_compile_definitions(liquid_frontend_syntax PRIVATE TREE_SITTER_HIDE_SYMBOLS)
  set_target_properties(liquid_frontend_syntax PROPERTIES
    EXPORT_NAME detail_frontend_syntax
    POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

  add_library(liquid-frontend-tree-sitter MODULE modules/frontends/tree_sitter/module.cpp)
  target_include_directories(liquid-frontend-tree-sitter PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/frontends
    ${LIQUID_TREE_SITTER_RUNTIME_INCLUDE})
  target_link_libraries(liquid-frontend-tree-sitter PRIVATE liquid_frontend_syntax)
  set_target_properties(liquid-frontend-tree-sitter PROPERTIES
    PREFIX ""
    OUTPUT_NAME "liquid-frontend-tree-sitter"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/modules/frontends/tree-sitter"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/modules/frontends/tree-sitter"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

  add_library(liquid-frontend-tree-sitter-static OBJECT
    modules/frontends/tree_sitter/module.cpp)
  target_include_directories(liquid-frontend-tree-sitter-static PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/frontends
    ${LIQUID_TREE_SITTER_RUNTIME_INCLUDE})
  target_compile_definitions(liquid-frontend-tree-sitter-static PRIVATE
    LIQUID_FRONTEND_STATIC=1)
  set_target_properties(liquid-frontend-tree-sitter-static PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    POSITION_INDEPENDENT_CODE ON)

  add_library(liquid-frontend-clang MODULE modules/frontends/clang/module.cpp)
  target_include_directories(liquid-frontend-clang PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/frontends
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/automerge-cpp/upstream/json/include)
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
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/frontends
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/automerge-cpp/upstream/json/include)
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

add_library(liquid-hot src/hot_update/runtime.c)
add_library(liquid::hot ALIAS liquid-hot)
target_include_directories(liquid-hot PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_features(liquid-hot PUBLIC c_std_11)
target_compile_definitions(liquid-hot PRIVATE LIQUID_HOT_BUILDING)
set_target_properties(liquid-hot PROPERTIES
  OUTPUT_NAME liquid-hot
  EXPORT_NAME hot
  VERSION 1
  SOVERSION 1
  C_VISIBILITY_PRESET hidden
  POSITION_INDEPENDENT_CODE ON)
if(UNIX)
  target_link_libraries(liquid-hot PRIVATE ${CMAKE_DL_LIBS})
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(liquid-read-guard src/sandbox/read_guard.c)
  target_link_options(liquid-read-guard PRIVATE -static)
  target_compile_features(liquid-read-guard PRIVATE c_std_99)
endif()

# automerge-cpp is the private CRDT engine. Keep it static even when libliquid is shared;
# its archive is an installed link dependency, but its headers remain private.
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
  EXPORT_NAME detail_automerge
  INTERFACE_INCLUDE_DIRECTORIES
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/modules/automerge-cpp/include>"
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  POSITION_INDEPENDENT_CODE ON
  EXCLUDE_FROM_ALL ON)
# JSON is header-only and needed only when compiling automerge. Preserve its runtime
# link requirements (Threads, zlib, and bcrypt on Windows) in the installed export.
get_target_property(LIQUID_AUTOMERGE_LINK_LIBRARIES automerge-cpp INTERFACE_LINK_LIBRARIES)
list(TRANSFORM LIQUID_AUTOMERGE_LINK_LIBRARIES REPLACE
  "^nlohmann_json::nlohmann_json$" "$<BUILD_INTERFACE:nlohmann_json::nlohmann_json>")
set_property(TARGET automerge-cpp PROPERTY INTERFACE_LINK_LIBRARIES
  "${LIQUID_AUTOMERGE_LINK_LIBRARIES}")

# oramacore-cpp supplies the private BM25F engine. Keep its own test tree out of
# liquid's build and fold its static target into shared libliquid with hidden symbols.
block()
  set(BUILD_TESTING OFF)
  add_subdirectory(modules/oramacore-cpp EXCLUDE_FROM_ALL)
endblock()
set_target_properties(oramacore-cpp PROPERTIES
  EXPORT_NAME detail_oramacore
  INTERFACE_INCLUDE_DIRECTORIES
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/modules/oramacore-cpp/include>"
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
  src/asn1.cpp
  src/abi.cpp
  src/admission/decision.cpp
  src/admission/policy.cpp
  src/admission/review.cpp
  src/admission/server.cpp
  src/admission/source.cpp
  src/attestation/image.cpp
  src/attestation/registry.cpp
  src/build/abi.cpp
  src/build/cache.cpp
  src/build/content.cpp
  src/build/events.cpp
  src/build/executor.cpp
  src/build/distribution.cpp
  src/build/enumeration.cpp
  src/build/frontend.cpp
  src/build/facts.cpp
  src/build/verify.cpp
  src/build/materialise.cpp
  src/build/normalise.cpp
  src/build/plan.cpp
  src/build/provenance.cpp
  src/build/relocation.cpp
  src/build/trace.cpp
  src/build/why.cpp
  src/build/policy.cpp
  src/build/sbom.cpp
  src/build/tests.cpp
  src/build/wrap.cpp
  src/capture/coalesce.cpp
  src/capture/cursor.cpp
  src/capture/reconcile.cpp
  src/cms.cpp
  src/configuration/language.cpp
  src/configuration/generator.cpp
  src/configuration/parser.cpp
  src/configuration/records.cpp
  src/daemon.cpp
  src/daemon/fanout.cpp
  src/data.cpp
  src/drip.cpp
  src/drive/client.cpp
  src/drive/settings.cpp
  src/delta.cpp
  src/deploy.cpp
  src/edit.cpp
  src/edit_codec.cpp
  src/encryption/leaf.cpp
  src/error.cpp
  src/formats/dotenv.cpp
  src/formats/apk/apk.cpp
  src/formats/ar/ar.cpp
  src/formats/binary/codec.cpp
  src/formats/deb/deb.cpp
  src/formats/deflate/deflate.cpp
  src/formats/hdf5/hdf5.cpp
  src/formats/binary/elf.cpp
  src/formats/binary/macho.cpp
  src/formats/binary/pe.cpp
  src/formats/xml/xml.cpp
  src/formats/json.cpp
  src/formats/ostree/ostree.cpp
  src/formats/parquet/parquet.cpp
  src/formats/pdf/pdf.cpp
  src/formats/model/model.cpp
  src/formats/document/document.cpp
  src/formats/package/common.cpp
  src/formats/package/codecs.cpp
  src/formats/registry.cpp
  src/formats/rpm/rpm.cpp
  src/formats/squashfs/squashfs.cpp
  src/formats/sqlite/sqlite.cpp
  src/formats/tar/tar.cpp
  src/formats/tar/writer.cpp
  src/formats/zip/zip.cpp
  src/frontend/graph.cpp
  src/frontend/module.cpp
  src/frontend/symbols.cpp
  src/forge.cpp
  src/frontend/syntax.cpp
  src/flow_control.cpp
  src/graph.cpp
  src/hash.cpp
  src/hash/sha256.c
  src/hot_update/hot_update.cpp
  src/http1/client.cpp
  src/http1/server.cpp
  src/http2/frame.cpp
  src/http2/grpc.cpp
  src/http2/hpack.cpp
  src/http2/connection.cpp
  src/git/export.cpp
  src/git/import.cpp
  src/git/object_store.cpp
  src/git/sha1.cpp
  src/identity.cpp
  src/identity/custody/age.cpp
  src/identity/custody/aws_kms.cpp
  src/identity/custody/azure_kv.cpp
  src/identity/custody/common.cpp
  src/identity/custody/gcp_kms.cpp
  src/identity/custody/http.cpp
  src/identity/custody/lake.cpp
  src/identity/custody/shamir.cpp
  src/identity/custody/tpm.cpp
  src/identity/keys/keys.cpp
  src/identity/keys/hid.cpp
  src/identity/taap.cpp
  src/install/faults.cpp
  src/install/install.cpp
  src/install/records.cpp
  src/identity/threshold.cpp
  src/import/spack.cpp
  src/node_store.cpp
  src/node_store/service.cpp
  src/crypto/crypto.cpp
  src/net/http_client.cpp
  src/crypto/signature.cpp
  src/object.cpp
  src/objects/records.cpp
  src/protobuf/wire.cpp
  src/projection/helm.cpp
  src/projection/conflicts.cpp
  src/projection/appimage_runtime.cpp
  src/projection/attestations.cpp
  src/projection/homebrew.cpp
  src/projection/pkgbuild.cpp
  src/projection/bundled.cpp
  ${LIQUID_APPIMAGE_RUNTIME_SOURCE}
  src/projection/bootc.cpp
  src/projection/debian_source.cpp
  src/projection/oci.cpp
  src/projection/ostree.cpp
  src/projection/package.cpp
  src/projection/platform.cpp
  src/projection/platform_tools.cpp
  src/projection/projection.cpp
  src/projection/signing.cpp
  src/projection/rules.cpp
  src/projection/scripts.cpp
  src/reapi/conversion.cpp
  src/reapi/client.cpp
  src/reapi/messages.cpp
  src/reapi/remote_client.cpp
  src/reapi/server.cpp
  src/serve/http.cpp
  src/workers/protocol.cpp
  src/workers/control.cpp
  src/workers/scheduler.cpp
  src/workers/worker.cpp
  src/x509.cpp
  src/repository/diff.cpp
  src/repository/structural_diff.cpp
  src/repository/merge.cpp
  src/resolve/records.cpp
  src/resolve/concretizer.cpp
  src/resolve/resolve.cpp
  src/resolve/spec.cpp
  src/remote.cpp
  src/repository/repository_impl.cpp
  src/runtime/runtime.cpp
  src/sandbox/environment.cpp
  src/scan/detectors.cpp
  src/search/index.cpp
  src/search/repository.cpp
  src/secret/sealed_secret.cpp
  src/store.cpp
  src/store/backend.cpp
  src/store/chunker.cpp
  src/store/collector.cpp
  src/store/compactor.cpp
  src/store/faults.cpp
  src/store/index.cpp
  src/store/leases.cpp
  src/store/owner_lock.cpp
  src/store/mapped_file.cpp
  src/store/pack.cpp
  src/store/references.cpp
  src/store/store_impl.cpp
  src/store/subject_index.cpp
  src/sync/codec.cpp
  src/sync/endpoint.cpp
  src/sync/server.cpp
  src/sync/session.cpp
  src/lake_transparency.cpp
  src/transparency.cpp
  src/tiers.cpp
  src/tuf.cpp
  src/version.cpp
  src/vfs.cpp
  src/vfs/fuse_fs.cpp
  src/vfs/hydration.cpp
  src/vfs/materialise.cpp
  src/vfs/observation.cpp
  src/vfs/request_queue.cpp
  src/vfs/xattrs.cpp
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
          ${CMAKE_CURRENT_BINARY_DIR}/generated
          ${blake3_source_SOURCE_DIR}/c
          ${monocypher_source_SOURCE_DIR}/src
          ${monocypher_source_SOURCE_DIR}/src/optional
          ${boost_core_libs_SOURCE_DIR}/libs/multiprecision/include
          ${CMAKE_CURRENT_SOURCE_DIR}/modules/automerge-cpp/upstream/json/include)
target_compile_features(libliquid PUBLIC cxx_std_26)
target_compile_definitions(libliquid PRIVATE
  LIQUID_GRAMMAR_LOCK_DIGEST="sha256:${LIQUID_GRAMMAR_LOCK_SHA256}"
  LIQUID_WRAP_HOOK_PATH="$<TARGET_FILE:liquid-wrap-hook>"
  LIQUID_WRAP_HOOK_FILENAME="$<TARGET_FILE_NAME:liquid-wrap-hook>")
if(LIQUID_BUILD_FRONTENDS)
  target_compile_definitions(libliquid PRIVATE LIQUID_HAS_BUILTIN_FRONTEND=1)
  target_sources(libliquid PRIVATE $<TARGET_OBJECTS:liquid-frontend-tree-sitter-static>)
  target_link_libraries(libliquid PRIVATE liquid_frontend_syntax)
endif()
if(NOT WIN32)
  target_compile_definitions(libliquid PRIVATE LIQUID_HAS_APPIMAGE_RUNTIME=1)
endif()
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
          ${CMAKE_DL_LIBS} automerge-cpp::automerge-cpp orama::oramacore-cpp)
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
  target_compile_definitions(libliquid PRIVATE LIQUID_HAVE_PCSC=1)
  target_link_libraries(libliquid PRIVATE bcrypt hid setupapi tbs winscard)
elseif(APPLE)
  target_compile_definitions(libliquid PRIVATE LIQUID_HAVE_PCSC=1)
  target_link_libraries(libliquid PRIVATE "-framework CoreFoundation" "-framework IOKit" "-framework PCSC")
elseif(UNIX)
  find_library(LIQUID_PCSC_LIBRARY pcsclite)
  if(LIQUID_PCSC_LIBRARY)
    target_compile_definitions(libliquid PRIVATE LIQUID_HAVE_PCSC=1)
    target_link_libraries(libliquid PRIVATE ${LIQUID_PCSC_LIBRARY})
  endif()
endif()
include(GenerateExportHeader)
generate_export_header(libliquid
  BASE_NAME liquid
  EXPORT_FILE_NAME ${CMAKE_CURRENT_BINARY_DIR}/include/liquid/export.hpp)
if(NOT BUILD_SHARED_LIBS)
  target_compile_definitions(libliquid PUBLIC LIQUID_STATIC_DEFINE)
endif()

add_executable(liquid-hot-demo tools/hot-update-demo/main.cpp)
target_link_libraries(liquid-hot-demo PRIVATE liquid::liquid liquid::hot)
add_library(liquid-hot-demo-patch MODULE tools/hot-update-demo/patch.c)
set_target_properties(liquid-hot-demo-patch PROPERTIES
  PREFIX ""
  OUTPUT_NAME "liquid-hot-demo-patch")
add_executable(liquid-hot-demo-restart-hook tools/hot-update-demo/restart_hook.c)

# -----------------------------------------------------------------------------
# The CLI — a thin client of libliquid.
# -----------------------------------------------------------------------------
if(LIQUID_BUILD_CLI)
  add_executable(liquid-cli
    src/main.cpp
    src/cli/abi.cpp
    src/cli/arguments.cpp
    src/cli/anchor.cpp
    src/cli/attestation.cpp
    src/cli/blame.cpp
    src/cli/capture.cpp
    src/cli/clone.cpp
    src/cli/daemon.cpp
    src/cli/compose.cpp
    src/cli/configuration.cpp
    src/cli/data.cpp
    src/cli/delta.cpp
    src/cli/deploy.cpp
    src/cli/diff.cpp
    src/cli/docs.cpp
    src/cli/edit.cpp
    src/cli/drive.cpp
    src/cli/gc.cpp
    src/cli/facts.cpp
    src/cli/git.cpp
    src/cli/history.cpp
    src/cli/hot_update.cpp
    src/cli/identity.cpp
    src/cli/install.cpp
    src/cli/inspect.cpp
    src/cli/keys.cpp
    src/cli/locks.cpp
    src/cli/node_store.cpp
    src/cli/policy.cpp
    src/cli/provenance.cpp
    src/cli/project.cpp
    src/cli/sbom.cpp
    src/cli/symbols.cpp
    src/cli/serve.cpp
    src/cli/sync.cpp
    src/cli/log.cpp
    src/cli/transparency.cpp
    src/cli/test.cpp
    src/cli/tier.cpp
    src/cli/tiers.cpp
    src/cli/repo.cpp
    src/cli/conflict.cpp
    src/cli/registry.cpp
    src/cli/review.cpp
    src/cli/release.cpp
    src/cli/trace.cpp
    src/cli/retain.cpp
    src/cli/resolve.cpp
    src/cli/search.cpp
    src/cli/secret.cpp
    src/cli/session.cpp
    src/cli/sign.cpp
    src/cli/tree.cpp
    src/cli/verify.cpp
    src/cli/vfs.cpp
    src/cli/why.cpp
    src/cli/workers.cpp
    src/cli/wrap.cpp)
  set_target_properties(liquid-cli PROPERTIES OUTPUT_NAME liquid)
  # libliquid is a static archive, so CMake already carries its private
  # dependencies to consumers for the final link. Linking them again put the
  # same archive on the command line twice; COMPILE_ONLY keeps the headers
  # and usage requirements without the duplicate.
  target_link_libraries(liquid-cli PRIVATE
    liquid::liquid $<COMPILE_ONLY:Boost::corosio> $<COMPILE_ONLY:Boost::corosio_openssl>
    $<COMPILE_ONLY:Boost::http> $<COMPILE_ONLY:Boost::json>)
  target_include_directories(liquid-cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

  # Keep a build-tree reference beside the binary.  The CTest below compares this
  # binary-produced tree with the committed documentation.
  set(LIQUID_REFERENCE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/reference/cli)
  set(LIQUID_REFERENCE_STAMP ${CMAKE_CURRENT_BINARY_DIR}/reference/generated.stamp)
  add_custom_command(
    OUTPUT ${LIQUID_REFERENCE_STAMP}
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${LIQUID_REFERENCE_DIRECTORY}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${LIQUID_REFERENCE_DIRECTORY}
    COMMAND $<TARGET_FILE:liquid-cli> docs --format markdown --out ${LIQUID_REFERENCE_DIRECTORY}
    COMMAND $<TARGET_FILE:liquid-cli> docs --format json --out ${LIQUID_REFERENCE_DIRECTORY}
    COMMAND ${CMAKE_COMMAND} -E touch ${LIQUID_REFERENCE_STAMP}
    DEPENDS liquid-cli
    VERBATIM)
  add_custom_target(liquid-reference-docs ALL
    DEPENDS ${LIQUID_REFERENCE_STAMP})

  set(LIQUID_MAN_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/man)
  add_custom_command(
    OUTPUT ${LIQUID_MAN_DIRECTORY}/generated.stamp
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${LIQUID_MAN_DIRECTORY}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${LIQUID_MAN_DIRECTORY}
    COMMAND $<TARGET_FILE:liquid-cli> docs --format man --out ${LIQUID_MAN_DIRECTORY}
    COMMAND ${CMAKE_COMMAND} -E touch ${LIQUID_MAN_DIRECTORY}/generated.stamp
    DEPENDS liquid-cli
    VERBATIM)
  add_custom_target(liquid-man-pages ALL
    DEPENDS ${LIQUID_MAN_DIRECTORY}/generated.stamp)
endif()

# The forge double is shipped for downstream review-client integration tests.
include(GNUInstallDirs)
add_library(liquid_forge_double STATIC tests/support/forge_double.cpp)
target_include_directories(liquid_forge_double
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/tests/support>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/automerge-cpp/upstream/json/include)
target_link_libraries(liquid_forge_double
  PUBLIC liquid::liquid
  PRIVATE $<COMPILE_ONLY:Boost::corosio> $<COMPILE_ONLY:Boost::corosio_openssl>
          $<COMPILE_ONLY:Boost::http>)
set_target_properties(liquid_forge_double PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_executable(liquid-forge-double tests/support/forge_double_main.cpp)
target_link_libraries(liquid-forge-double PRIVATE liquid_forge_double)

# -----------------------------------------------------------------------------
# Install + package config: find_package(liquid CONFIG) → liquid::liquid.
# -----------------------------------------------------------------------------
include(CMakePackageConfigHelpers)
install(TARGETS libliquid EXPORT liquidTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
# Static consumers need the private implementation archives. Export them as link-only
# dependencies of liquid::liquid; shared libliquid already contains these implementations.
if(NOT BUILD_SHARED_LIBS)
  install(TARGETS automerge-cpp oramacore-cpp EXPORT liquidTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
  if(LIQUID_BUILD_FRONTENDS)
    install(TARGETS liquid_frontend_syntax EXPORT liquidTargets
      ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
  endif()
endif()
install(TARGETS liquid_forge_double EXPORT liquidTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(TARGETS liquid-forge-double RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES tests/support/forge_double.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/liquid/test)
install(TARGETS liquid-wrap-hook
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/liquid
  RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/liquid)
install(TARGETS liquid-hot EXPORT liquidTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
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
  install(DIRECTORY ${LIQUID_MAN_DIRECTORY}/
    DESTINATION ${CMAKE_INSTALL_MANDIR}/man1
    FILES_MATCHING PATTERN "*.1")
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
  add_executable(liquid-platform-installer-fixture tests/platform/installer_fixture.cpp)
  target_link_libraries(liquid-platform-installer-fixture PRIVATE liquid::liquid)
  add_executable(liquid-platform-installer-program tests/platform/installer_program.cpp)
  # SYSTEM marks its headers as system headers, so googletest's own warnings
  # do not appear in liquid's build output where they read as ours.
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.17.0
    SYSTEM)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  # FetchContent's SYSTEM does not reach targets that publish their own include
  # directories, so mark them explicitly; otherwise googletest's warnings appear
  # in liquid's output where they read as ours.
  foreach(gtest_target gtest gtest_main gmock gmock_main)
    if(TARGET ${gtest_target})
      get_target_property(gtest_includes ${gtest_target} INTERFACE_INCLUDE_DIRECTORIES)
      if(gtest_includes)
        set_target_properties(${gtest_target} PROPERTIES
          INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${gtest_includes}")
      endif()
      # gtest-printers.h converts char8_t to char32_t, which recent Clang warns
      # about while googletest compiles its own sources. System include marking
      # cannot reach that, and the code is not ours to fix.
      if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${gtest_target} PRIVATE -Wno-character-conversion)
      endif()
    endif()
  endforeach()
  enable_testing()
  add_subdirectory(tests)
endif()

# Optional benchmarks have their own dependencies and never enter the default build.
if(LIQUID_BUILD_BENCH)
  add_subdirectory(bench/chunk-index)
endif()
