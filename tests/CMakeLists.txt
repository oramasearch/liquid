set(LIQUID_HTTP2_TEST_SOURCES
  ${PROJECT_SOURCE_DIR}/src/http2/connection.cpp
  ${PROJECT_SOURCE_DIR}/src/http2/frame.cpp
  ${PROJECT_SOURCE_DIR}/src/http2/grpc.cpp
  ${PROJECT_SOURCE_DIR}/src/http2/hpack.cpp)

find_program(LIQUID_PYTHON_EXECUTABLE NAMES python3 python REQUIRED)
add_test(NAME docs-links
  COMMAND ${LIQUID_PYTHON_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/docs/check-links.py
    ${PROJECT_SOURCE_DIR})

add_executable(liquid-tests
  ../src/daemon/fanout.cpp
  test_asn1.cpp
  test_drip.cpp
  test_delta.cpp
  test_deploy.cpp
  test_daemon.cpp
  test_data.cpp
  test_error.cpp
  test_forge.cpp
  test_hash.cpp
  test_hot_update.cpp
  http2/test_codecs.cpp
  test_identity.cpp
  test_node_store.cpp
  test_object.cpp
  test_remote.cpp
  reapi/test_wire.cpp
  reapi/test_messages.cpp
  reapi/test_conversion.cpp
  resolve/test_resolution.cpp
  workers/test_scheduler.cpp
  ${PROJECT_SOURCE_DIR}/src/protobuf/wire.cpp
  ${PROJECT_SOURCE_DIR}/src/reapi/messages.cpp
  ${PROJECT_SOURCE_DIR}/src/reapi/conversion.cpp
  ${PROJECT_SOURCE_DIR}/src/workers/protocol.cpp
  ${PROJECT_SOURCE_DIR}/src/workers/scheduler.cpp
  ${PROJECT_SOURCE_DIR}/src/identity/taap.cpp
  test_repository.cpp
  test_stale_reasoning.cpp
  test_runtime.cpp
  test_secret.cpp
  test_store.cpp
  test_temporary_directory.cpp
  test_transparency.cpp
  ${PROJECT_SOURCE_DIR}/src/crypto/signature.cpp
  attestation/test_registry.cpp
  admission/test_review.cpp
  scan/test_capture.cpp
  scan/test_detectors.cpp
  scan/test_redaction.cpp
  search/test_index.cpp
  ${PROJECT_SOURCE_DIR}/src/search/index.cpp
  ${PROJECT_SOURCE_DIR}/src/store/mapped_file.cpp
  ${PROJECT_SOURCE_DIR}/src/scan/detectors.cpp
  store/test_gc.cpp
  ${PROJECT_SOURCE_DIR}/src/store/chunker.cpp
  ${PROJECT_SOURCE_DIR}/src/objects/records.cpp
  ${PROJECT_SOURCE_DIR}/src/encoding/cbor.cpp
  ${PROJECT_SOURCE_DIR}/src/formats/json.cpp
  test_version.cpp
  test_vfs.cpp
  vfs/test_materialise.cpp
  vfs/test_mount.cpp
  build/test_executor.cpp
  build/test_remote_outputs.cpp
  build/test_distribution.cpp
  build/test_events.cpp
  build/test_policy.cpp
  build/test_facts.cpp
  build/test_graph.cpp
  build/test_content.cpp
  build/test_normalise.cpp
  build/test_provenance.cpp
  build/test_relocation.cpp
  build/test_why.cpp
  build/test_sbom.cpp
  build/test_abi.cpp
  build/test_symbols.cpp
  build/test_wrap.cpp
  ${LIQUID_HTTP2_TEST_SOURCES}
)
if(BUILD_SHARED_LIBS)
  target_sources(liquid-tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/build/content.cpp
    ${PROJECT_SOURCE_DIR}/src/build/normalise.cpp
    ${PROJECT_SOURCE_DIR}/src/drive/settings.cpp
    ${PROJECT_SOURCE_DIR}/src/sandbox/environment.cpp
    ${PROJECT_SOURCE_DIR}/src/store/references.cpp
    ${PROJECT_SOURCE_DIR}/src/vfs/xattrs.cpp)
  if(APPLE)
    target_sources(liquid-tests PRIVATE ${PROJECT_SOURCE_DIR}/src/sandbox/macos.cpp)
  endif()
endif()
if(UNIX)
  target_sources(liquid-tests PRIVATE test_sync.cpp admission/test_server.cpp)
endif()
target_link_libraries(liquid-tests PRIVATE
  liquid::liquid
  liquid_forge_double
  liquid_monocypher
  orama::oramacore-cpp
  GTest::gtest_main
  Boost::assert
  Boost::config
  Boost::throw_exception
  Boost::corosio
  Boost::corosio_openssl)
target_include_directories(liquid-tests PRIVATE
  ${PROJECT_SOURCE_DIR}/src
  ${PROJECT_SOURCE_DIR}/modules/corosio/include
  ${boost_core_libs_SOURCE_DIR}/libs/multiprecision/include
  ${monocypher_source_SOURCE_DIR}/src
  ${monocypher_source_SOURCE_DIR}/src/optional)
include(GoogleTest)
gtest_discover_tests(liquid-tests)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME desktop-nautilus-badge-mapping
  COMMAND ${Python3_EXECUTABLE}
    ${PROJECT_SOURCE_DIR}/tools/desktop/linux/nautilus/test_badges.py)
add_test(NAME desktop-dolphin-badge-mapping
  COMMAND ${Python3_EXECUTABLE}
    ${PROJECT_SOURCE_DIR}/tools/desktop/linux/dolphin/test_badges.py)

add_executable(liquid-http2-test-server
  http2/test_server.cpp
  ${LIQUID_HTTP2_TEST_SOURCES})
target_link_libraries(liquid-http2-test-server PRIVATE
  liquid::liquid
  Boost::corosio
  Boost::corosio_openssl)
target_include_directories(liquid-http2-test-server PRIVATE ${PROJECT_SOURCE_DIR}/src)

add_executable(liquid-build-fixture build/fixture.c)
if(UNIX AND NOT APPLE)
  target_link_options(liquid-build-fixture PRIVATE -static)
endif()
add_dependencies(liquid-tests liquid-build-fixture)
add_executable(liquid-secret-fixture-generator EXCLUDE_FROM_ALL secret_fixture.cpp)
target_include_directories(liquid-secret-fixture-generator PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(liquid-secret-fixture-generator PRIVATE liquid::liquid)

foreach(ABI_FIXTURE IN ITEMS v1 added break)
  add_library(liquid-abi-fixture-${ABI_FIXTURE} SHARED
    build/abi_fixture_${ABI_FIXTURE}.c)
  set_target_properties(liquid-abi-fixture-${ABI_FIXTURE} PROPERTIES
    C_VISIBILITY_PRESET hidden
    OUTPUT_NAME liquid-abi-fixture-${ABI_FIXTURE}
    SOVERSION 1)
  if(MSVC)
    target_compile_options(liquid-abi-fixture-${ABI_FIXTURE} PRIVATE /Zi)
  else()
    target_compile_options(liquid-abi-fixture-${ABI_FIXTURE} PRIVATE -g)
  endif()
  if(UNIX AND NOT APPLE)
    target_link_options(liquid-abi-fixture-${ABI_FIXTURE} PRIVATE
      "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/build/abi_fixture.map")
  endif()
  add_dependencies(liquid-tests liquid-abi-fixture-${ABI_FIXTURE})
endforeach()
if(UNIX)
  add_executable(liquid-graph-crash-fixture build/graph_crash_fixture.cpp)
  target_link_libraries(liquid-graph-crash-fixture PRIVATE liquid::liquid)
  add_dependencies(liquid-tests liquid-graph-crash-fixture)
endif()
target_compile_definitions(liquid-tests PRIVATE
  LIQUID_BUILD_FIXTURE="$<TARGET_FILE:liquid-build-fixture>"
  LIQUID_FORMAT_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/formats"
  LIQUID_ABI_FIXTURE_V1="$<TARGET_FILE:liquid-abi-fixture-v1>"
  LIQUID_ABI_FIXTURE_ADDED="$<TARGET_FILE:liquid-abi-fixture-added>"
  LIQUID_ABI_FIXTURE_BREAK="$<TARGET_FILE:liquid-abi-fixture-break>"
  LIQUID_ABI_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/build/fixtures/abi"
  LIQUID_SOURCE_ROOT="${PROJECT_SOURCE_DIR}"
  LIQUID_WRAP_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/build/wrap"
  LIQUID_BUILD_HAS_FUSE=$<BOOL:${LIQUID_FUSE_TARGET}>
  LIQUID_REAPI_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/reapi/fixtures")
if(UNIX)
  target_compile_definitions(liquid-tests PRIVATE
    LIQUID_ADMISSION_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/admission"
    LIQUID_ADMISSION_TLS_CERT="${PROJECT_SOURCE_DIR}/tools/reapi-peers/nativelink-tls-cert.pem"
    LIQUID_ADMISSION_TLS_KEY="${PROJECT_SOURCE_DIR}/tools/reapi-peers/nativelink-tls-key.pem")
endif()
target_compile_definitions(liquid-tests PRIVATE
  LIQUID_TLS_CERTIFICATE="${PROJECT_SOURCE_DIR}/tools/reapi-peers/nativelink-tls-cert.pem"
  LIQUID_TLS_PRIVATE_KEY="${PROJECT_SOURCE_DIR}/tools/reapi-peers/nativelink-tls-key.pem"
  LIQUID_SECRET_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/sealed-secret")
target_compile_definitions(liquid-tests PRIVATE
  LIQUID_TRANSPARENCY_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/transparency/fixtures")
if(LIQUID_BUILD_FRONTENDS)
  target_sources(liquid-tests PRIVATE build/test_frontend.cpp build/test_grammars.cpp test_edit.cpp)
  add_dependencies(liquid-tests liquid-frontend-tree-sitter liquid-frontend-clang)
  target_compile_definitions(liquid-tests PRIVATE
    LIQUID_TREE_SITTER_FRONTEND="$<TARGET_FILE:liquid-frontend-tree-sitter>"
    LIQUID_CLANG_FRONTEND="$<TARGET_FILE:liquid-frontend-clang>"
    LIQUID_FRONTEND_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/frontend-cmake"
    LIQUID_BINARY_ROOT="${PROJECT_BINARY_DIR}"
    LIQUID_GRAMMAR_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/grammars"
    LIQUID_CXX_COMPILER="${CMAKE_CXX_COMPILER}"
    LIQUID_NM="${CMAKE_NM}")
  target_include_directories(liquid-tests PRIVATE
    ${PROJECT_SOURCE_DIR}/modules/frontends
    ${LIQUID_TREE_SITTER_RUNTIME_INCLUDE})
  target_link_libraries(liquid-tests PRIVATE
    liquid-frontend-clang-static
    liquid_frontend_syntax)

  add_test(NAME grammar-lock-digests
    COMMAND ${CMAKE_COMMAND}
      -DLIQUID_GRAMMAR_LOCK=${LIQUID_GRAMMAR_LOCK}
      -DLIQUID_GRAMMAR_SOURCE_MAP=${LIQUID_GRAMMAR_SOURCE_MAP}
      -P ${PROJECT_SOURCE_DIR}/cmake/VerifyGrammars.cmake)

  add_executable(liquid-grammar-measure build/measure_grammars.cpp)
  target_include_directories(liquid-grammar-measure PRIVATE
    ${PROJECT_SOURCE_DIR}/modules/frontends
    ${LIQUID_TREE_SITTER_RUNTIME_INCLUDE})
  target_compile_definitions(liquid-grammar-measure PRIVATE
    LIQUID_GRAMMAR_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/grammars")
  target_link_libraries(liquid-grammar-measure PRIVATE liquid_frontend_syntax)
endif()
if(UNIX)
  target_compile_definitions(liquid-tests PRIVATE
    LIQUID_GRAPH_CRASH_FIXTURE="$<TARGET_FILE:liquid-graph-crash-fixture>")
  add_executable(liquid-wrap-sleep-tool build/sleep_copy.c)
  add_dependencies(liquid-tests liquid-wrap-sleep-tool)
  target_compile_definitions(liquid-tests PRIVATE
    LIQUID_WRAP_SLEEP_TOOL="$<TARGET_FILE:liquid-wrap-sleep-tool>")
endif()

if(UNIX)
  add_executable(liquid-hot-demo-tests
    hot_update/test_demo.cpp
    ${PROJECT_SOURCE_DIR}/src/encoding/cbor.cpp
    ${PROJECT_SOURCE_DIR}/src/objects/records.cpp)
  target_include_directories(liquid-hot-demo-tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
  target_link_libraries(liquid-hot-demo-tests PRIVATE
    liquid::liquid
    liquid::hot
    GTest::gtest_main)
  target_compile_definitions(liquid-hot-demo-tests PRIVATE
    LIQUID_HOT_DEMO="$<TARGET_FILE:liquid-hot-demo>"
    LIQUID_HOT_DEMO_PATCH="$<TARGET_FILE:liquid-hot-demo-patch>"
    LIQUID_HOT_DEMO_RESTART_HOOK="$<TARGET_FILE:liquid-hot-demo-restart-hook>"
    LIQUID_BUILD_HAS_FUSE=$<BOOL:${LIQUID_FUSE_TARGET}>)
  add_dependencies(liquid-hot-demo-tests
    liquid-hot-demo
    liquid-hot-demo-patch
    liquid-hot-demo-restart-hook)
  gtest_discover_tests(liquid-hot-demo-tests)
endif()

if(LIQUID_BUILD_CLI)
  target_compile_definitions(liquid-tests PRIVATE LIQUID_CLI="$<TARGET_FILE:liquid-cli>")
  add_dependencies(liquid-tests liquid-cli)
  if(NOT APPLE)
  set(LIQUID_SYMBOLS_CLI_REPOSITORY ${CMAKE_CURRENT_BINARY_DIR}/symbols-cli-repository)
  add_test(NAME symbols-cli-clean
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${LIQUID_SYMBOLS_CLI_REPOSITORY})
  set_tests_properties(symbols-cli-clean PROPERTIES FIXTURES_SETUP symbols_cli_clean)
  add_test(NAME symbols-cli-setup
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/frontend-cmake ${LIQUID_SYMBOLS_CLI_REPOSITORY})
  set_tests_properties(symbols-cli-setup PROPERTIES
    FIXTURES_REQUIRED symbols_cli_clean
    FIXTURES_SETUP symbols_cli)
  add_test(NAME symbols-cli-init
    COMMAND $<TARGET_FILE:liquid-cli> init .)
  set_tests_properties(symbols-cli-init PROPERTIES
    FIXTURES_REQUIRED symbols_cli
    FIXTURES_SETUP symbols_cli_initialized
    WORKING_DIRECTORY ${LIQUID_SYMBOLS_CLI_REPOSITORY})
  add_test(NAME symbols-cli-build
    COMMAND $<TARGET_FILE:liquid-cli> wrap ${CMAKE_CXX_COMPILER} -std=c++20 -c functions.cpp -o functions.o)
  set_tests_properties(symbols-cli-build PROPERTIES
    FIXTURES_REQUIRED symbols_cli_initialized
    FIXTURES_SETUP symbols_cli_build
    WORKING_DIRECTORY ${LIQUID_SYMBOLS_CLI_REPOSITORY})
  add_test(NAME symbols-cli
    COMMAND $<TARGET_FILE:liquid-cli> symbols value.hpp)
  set_tests_properties(symbols-cli PROPERTIES
    FIXTURES_REQUIRED symbols_cli_build
    WORKING_DIRECTORY ${LIQUID_SYMBOLS_CLI_REPOSITORY}
    PASS_REGULAR_EXPRESSION "origin=compiler")
  endif()
  if(LIQUID_BUILD_FRONTENDS)
    add_test(NAME symbols-cli-python
      COMMAND ${CMAKE_COMMAND}
        -DLIQUID_CLI=$<TARGET_FILE:liquid-cli>
        -DLIQUID_SYMBOLS_PYTHON_ROOT=${CMAKE_CURRENT_BINARY_DIR}/symbols-python-repository
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cli/symbols-python.cmake)
  endif()
  add_executable(liquid-gc-cli-fixture store/gc_cli_fixture.cpp)
  target_link_libraries(liquid-gc-cli-fixture PRIVATE liquid::liquid)
  set(LIQUID_GC_CLI_REPOSITORY ${CMAKE_CURRENT_BINARY_DIR}/gc-cli-repository)
  add_test(NAME gc-cli-setup
    COMMAND liquid-gc-cli-fixture ${LIQUID_GC_CLI_REPOSITORY})
  set_tests_properties(gc-cli-setup PROPERTIES FIXTURES_SETUP gc_cli)
  add_test(NAME gc-cli-explain
    COMMAND $<TARGET_FILE:liquid-cli> gc --explain ${LIQUID_GC_CLI_REPOSITORY})
  set_tests_properties(gc-cli-explain PROPERTIES
    FIXTURES_REQUIRED gc_cli
    PASS_REGULAR_EXPRESSION "retained: lease")
  add_test(NAME retain-cli-explain
    COMMAND ${CMAKE_COMMAND}
      -DLIQUID_CLI=$<TARGET_FILE:liquid-cli>
      -DLIQUID_RETAIN_REPOSITORY=${LIQUID_GC_CLI_REPOSITORY}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cli/retain-explain.cmake)
  set_tests_properties(retain-cli-explain PROPERTIES FIXTURES_REQUIRED gc_cli)
  add_test(NAME gc-cli-compact
    COMMAND $<TARGET_FILE:liquid-cli> gc --compact --compact-below 0.5 ${LIQUID_GC_CLI_REPOSITORY})
  set_tests_properties(gc-cli-compact PROPERTIES
    FIXTURES_REQUIRED gc_cli
    PASS_REGULAR_EXPRESSION "packs_compacted=0")
  add_test(NAME gc-cli-clean
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${LIQUID_GC_CLI_REPOSITORY})
  set_tests_properties(gc-cli-clean PROPERTIES FIXTURES_CLEANUP gc_cli)
  add_test(NAME cli-scenario
    COMMAND ${CMAKE_COMMAND}
      -DLIQUID_CLI=$<TARGET_FILE:liquid-cli>
      -DLIQUID_CLI_ROOT=${CMAKE_CURRENT_BINARY_DIR}/cli-scenario
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cli/scenario.cmake)
  add_test(NAME cli-help-docs
    COMMAND ${CMAKE_COMMAND}
      -DLIQUID_CLI=$<TARGET_FILE:liquid-cli>
      -DLIQUID_CLI_DOCS=${PROJECT_SOURCE_DIR}/docs/reference/cli/README.md
      -DLIQUID_CLI_REFERENCE=${PROJECT_SOURCE_DIR}/docs/reference/cli
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cli/help-docs.cmake)
  add_test(NAME cli-reference-docs
    COMMAND ${CMAKE_COMMAND}
      -DLIQUID_CLI=$<TARGET_FILE:liquid-cli>
      -DLIQUID_REFERENCE_SOURCE=${PROJECT_SOURCE_DIR}/docs/reference/cli
      -DLIQUID_REFERENCE_OUTPUT=${PROJECT_BINARY_DIR}/reference/cli
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cli/reference-docs.cmake)
  add_test(NAME cli-docs-json
    COMMAND $<TARGET_FILE:liquid-cli> docs --format json)
  set_tests_properties(cli-docs-json PROPERTIES
    PASS_REGULAR_EXPRESSION "^\\{\"schema_version\":1,\"commands\":\\[")
  add_test(NAME cli-man-pages
    COMMAND ${CMAKE_COMMAND}
      -DLIQUID_CLI=$<TARGET_FILE:liquid-cli>
      -DLIQUID_MAN_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/cli-man-pages
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cli/man-pages.cmake)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_test(NAME guides-run
      COMMAND ${LIQUID_PYTHON_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/guides/run.py
        --liquid $<TARGET_FILE:liquid-cli>
        --guides ${PROJECT_SOURCE_DIR}/docs/guides
        --fixtures ${CMAKE_CURRENT_SOURCE_DIR}/build/wrap
        --work ${CMAKE_CURRENT_BINARY_DIR}/guides-run)
    set_tests_properties(guides-run PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
  endif()
  add_test(NAME cli-transparency-configuration
    COMMAND ${CMAKE_COMMAND}
      -DLIQUID_CLI=$<TARGET_FILE:liquid-cli>
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cli/transparency.cmake)
  add_test(NAME cli-delta
    COMMAND ${CMAKE_COMMAND}
      -DLIQUID_CLI=$<TARGET_FILE:liquid-cli>
      -DLIQUID_DELTA_ROOT=${CMAKE_CURRENT_BINARY_DIR}/delta-cli
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cli/delta.cmake)
  add_test(NAME cli-diff
    COMMAND ${CMAKE_COMMAND}
      -DLIQUID_CLI=$<TARGET_FILE:liquid-cli>
      -DLIQUID_DIFF_ROOT=${CMAKE_CURRENT_BINARY_DIR}/diff-cli
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cli/diff.cmake)
  add_test(NAME cli-secret
    COMMAND ${CMAKE_COMMAND}
      -DLIQUID_CLI=$<TARGET_FILE:liquid-cli>
      -DLIQUID_SECRET_ROOT=${CMAKE_CURRENT_BINARY_DIR}/secret-cli
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cli/secret.cmake)
  add_executable(liquid-format-diff-fixture cli/format_diff_fixture.cpp)
  target_link_libraries(liquid-format-diff-fixture PRIVATE liquid::liquid)
  add_test(NAME cli-format-diff
    COMMAND ${CMAKE_COMMAND}
      -DLIQUID_CLI=$<TARGET_FILE:liquid-cli>
      -DLIQUID_FORMAT_DIFF_FIXTURE=$<TARGET_FILE:liquid-format-diff-fixture>
      -DLIQUID_FORMAT_DIFF_ROOT=${CMAKE_CURRENT_BINARY_DIR}/format-diff-cli
      -DLIQUID_FORMAT_FIXTURES=${PROJECT_SOURCE_DIR}/tests/fixtures/formats
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cli/format-diff.cmake)
  add_test(NAME cli-why-json
    COMMAND ${CMAKE_COMMAND} -E env
      LIQUID_WRAP_CACHE=${CMAKE_CURRENT_BINARY_DIR}/why-cli-cache
      $<TARGET_FILE:liquid-cli> why never-built.o --json)
  set_tests_properties(cli-why-json PROPERTIES
    PASS_REGULAR_EXPRESSION "\"reason\":\"never built\"")
  add_test(NAME cli-cache-stats-json
    COMMAND ${CMAKE_COMMAND} -E env
      LIQUID_WRAP_CACHE=${CMAKE_CURRENT_BINARY_DIR}/why-cli-cache
      $<TARGET_FILE:liquid-cli> cache stats --json)
  set_tests_properties(cli-cache-stats-json PROPERTIES
    PASS_REGULAR_EXPRESSION "\"disk\":\\{\"available\":true")
  add_test(NAME cli-public-includes
    COMMAND ${CMAKE_COMMAND}
      -DLIQUID_CLI_SOURCE=${PROJECT_SOURCE_DIR}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cli/public-includes.cmake)
  add_executable(liquid-identity-codec-fixture cli/identity_codec.cpp)
  target_link_libraries(liquid-identity-codec-fixture PRIVATE liquid::liquid)
  add_test(NAME cli-identity-codec
    COMMAND ${CMAKE_COMMAND}
      -DLIQUID_CLI=$<TARGET_FILE:liquid-cli>
      -DLIQUID_IDENTITY_CODEC=$<TARGET_FILE:liquid-identity-codec-fixture>
      -DLIQUID_IDENTITY_HOME=${CMAKE_CURRENT_BINARY_DIR}/identity-codec
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cli/identity-codec.cmake)
  add_executable(liquid-keys-cli-fixture cli/keys_fixture.cpp)
  target_link_libraries(liquid-keys-cli-fixture PRIVATE liquid::liquid)
  add_test(NAME cli-keys
    COMMAND ${CMAKE_COMMAND}
      -DLIQUID_CLI=$<TARGET_FILE:liquid-cli>
      -DLIQUID_KEYS_FIXTURE=$<TARGET_FILE:liquid-keys-cli-fixture>
      -DLIQUID_KEYS_ROOT=${CMAKE_CURRENT_BINARY_DIR}/keys-cli
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cli/keys.cmake)
  add_test(NAME cli-sbom-spdx
    COMMAND $<TARGET_FILE:liquid-cli> sbom $<TARGET_FILE:liquid-build-fixture> --spdx)
  set_tests_properties(cli-sbom-spdx PROPERTIES
    PASS_REGULAR_EXPRESSION "\"spdxVersion\"[^\n]*\"SPDX-2.3\"")
  add_test(NAME cli-sbom-cyclonedx
    COMMAND $<TARGET_FILE:liquid-cli> sbom $<TARGET_FILE:liquid-build-fixture> --cyclonedx)
  set_tests_properties(cli-sbom-cyclonedx PROPERTIES
    PASS_REGULAR_EXPRESSION "\"bomFormat\"[^\n]*\"CycloneDX\"")
  add_test(NAME cli-abi-show-json
    COMMAND $<TARGET_FILE:liquid-cli> abi show $<TARGET_FILE:liquid-abi-fixture-v1> --json)
  set_tests_properties(cli-abi-show-json PROPERTIES
    PASS_REGULAR_EXPRESSION "\"version\":1[^\n]*\"liquid_abi_retained\"")
  add_test(NAME cli-abi-diff-json
    COMMAND $<TARGET_FILE:liquid-cli> abi diff
      $<TARGET_FILE:liquid-abi-fixture-v1>
      $<TARGET_FILE:liquid-abi-fixture-added>
      --json)
  set_tests_properties(cli-abi-diff-json PROPERTIES
    PASS_REGULAR_EXPRESSION "\"verdict\":\"compatible_additions\"[^\n]*\"liquid_abi_added\"")
endif()

add_executable(liquid-vfs-queue-tests
  vfs/test_request_queue.cpp
  ${PROJECT_SOURCE_DIR}/src/vfs/request_queue.cpp)
target_include_directories(liquid-vfs-queue-tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(liquid-vfs-queue-tests PRIVATE liquid::liquid GTest::gtest_main)
gtest_discover_tests(liquid-vfs-queue-tests)

add_executable(liquid-capture-measure EXCLUDE_FROM_ALL capture_measure.cpp)
target_include_directories(liquid-capture-measure PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(liquid-capture-measure PRIVATE liquid::liquid)

add_executable(liquid-graph-measure EXCLUDE_FROM_ALL graph_measure.cpp)
target_link_libraries(liquid-graph-measure PRIVATE liquid::liquid)

if(LIQUID_BUILD_FRONTENDS)
  add_executable(liquid-edit-measure edit_measure.cpp)
  target_link_libraries(liquid-edit-measure PRIVATE liquid::liquid)
  target_compile_definitions(liquid-edit-measure PRIVATE
    LIQUID_TREE_SITTER_FRONTEND="$<TARGET_FILE:liquid-frontend-tree-sitter>")
  add_dependencies(liquid-edit-measure liquid-frontend-tree-sitter)

  add_executable(liquid-frontend-measure frontend_measure.cpp)
  target_sources(liquid-frontend-measure PRIVATE ${PROJECT_SOURCE_DIR}/src/encoding/cbor.cpp)
  if(NOT UNIX)
    set_target_properties(liquid-frontend-measure PROPERTIES EXCLUDE_FROM_ALL TRUE)
  endif()
  target_link_libraries(liquid-frontend-measure PRIVATE liquid::liquid)
  target_include_directories(liquid-frontend-measure PRIVATE ${PROJECT_SOURCE_DIR}/src)
  target_compile_definitions(liquid-frontend-measure PRIVATE
    LIQUID_CXX_COMPILER="${CMAKE_CXX_COMPILER}"
    LIQUID_BINARY_ROOT="${PROJECT_BINARY_DIR}"
    LIQUID_CLANG_FRONTEND="$<TARGET_FILE:liquid-frontend-clang>")
  add_dependencies(liquid-frontend-measure liquid-frontend-clang)
  if(UNIX)
    add_test(NAME frontend-one-function-measure
      COMMAND liquid-frontend-measure
        ${PROJECT_SOURCE_DIR}
        ${CMAKE_CURRENT_BINARY_DIR}/frontend-measure-state)
  endif()
endif()

add_executable(liquid-store-large-tests test_store_large.cpp)
target_include_directories(liquid-store-large-tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(liquid-store-large-tests PRIVATE liquid::liquid GTest::gtest_main)
gtest_discover_tests(liquid-store-large-tests PROPERTIES LABELS large)

# The 40 GiB run takes about ten minutes and 40 GiB of disk: it is registered only when
# LIQUID_LARGE_TESTS=ON (configure with it, then `ctest -L large-40g`).
option(LIQUID_LARGE_TESTS "Register the 40 GiB streaming test" OFF)
if(LIQUID_LARGE_TESTS)
  add_executable(liquid-store-40g-tests test_store_40g.cpp)
  target_include_directories(liquid-store-40g-tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
  target_link_libraries(liquid-store-40g-tests PRIVATE liquid::liquid GTest::gtest_main)
  gtest_discover_tests(liquid-store-40g-tests PROPERTIES LABELS large-40g)
endif()

# The index is private implementation code.  Compile its two private sources
# into this test target so the same assertions work for static and shared
# library configurations without widening the public ABI.
add_executable(liquid-index-tests
  test_index.cpp
  ${PROJECT_SOURCE_DIR}/src/store/index.cpp
  ${PROJECT_SOURCE_DIR}/src/store/mapped_file.cpp
  ${PROJECT_SOURCE_DIR}/src/store/pack.cpp
  ${PROJECT_SOURCE_DIR}/src/crypto/crypto.cpp)
target_include_directories(liquid-index-tests PRIVATE
  ${PROJECT_SOURCE_DIR}/src/store
  ${PROJECT_SOURCE_DIR}/src/crypto
  ${blake3_source_SOURCE_DIR}/c
  ${monocypher_source_SOURCE_DIR}/src)
target_link_libraries(liquid-index-tests PRIVATE liquid::liquid liquid_blake3 liquid_monocypher GTest::gtest_main Boost::corosio libzstd_static)
gtest_discover_tests(liquid-index-tests)

add_executable(liquid-crypto-tests
  crypto/test_aead.cpp
  ${PROJECT_SOURCE_DIR}/src/crypto/crypto.cpp)
target_include_directories(liquid-crypto-tests PRIVATE
  ${PROJECT_SOURCE_DIR}/include
  ${PROJECT_BINARY_DIR}/include
  ${PROJECT_SOURCE_DIR}/src/crypto
  ${blake3_source_SOURCE_DIR}/c
  ${monocypher_source_SOURCE_DIR}/src)
target_link_libraries(liquid-crypto-tests PRIVATE liquid_blake3 liquid_monocypher GTest::gtest_main)
gtest_discover_tests(liquid-crypto-tests)

add_executable(liquid-store-sim-tests
  store/test_gc_sim_regression.cpp
  sim/harness.cpp
  sim/test_store_sim.cpp)
target_include_directories(liquid-store-sim-tests PRIVATE
  ${PROJECT_SOURCE_DIR}/src/store
  ${CMAKE_CURRENT_SOURCE_DIR}/sim)
target_link_libraries(liquid-store-sim-tests PRIVATE liquid::liquid GTest::gtest_main)
gtest_discover_tests(liquid-store-sim-tests PROPERTIES LABELS sim-long)

add_executable(liquid-header-tests
  headers/abi.cpp
  headers/admission.cpp
  headers/attestation.cpp
  headers/build.cpp
  headers/daemon.cpp
  headers/drip.cpp
  headers/edit.cpp
  headers/delta.cpp
  headers/drive.cpp
  headers/error.cpp
  headers/export.cpp
  headers/format.cpp
  headers/flow.cpp
  headers/frontend.cpp
  headers/git.cpp
  headers/graph.cpp
  headers/hash.cpp
  headers/hot_update.cpp
  headers/identity.cpp
  headers/liquid.cpp
  headers/node_store.cpp
  headers/main.cpp
  headers/object.cpp
  headers/remote.cpp
  headers/repository.cpp
  headers/resolve.cpp
  headers/runtime.cpp
  headers/store.cpp
  headers/sync.cpp
  headers/version.cpp
  headers/vfs.cpp
  headers/workers.cpp)
target_link_libraries(liquid-header-tests PRIVATE liquid::liquid)
add_test(NAME public-headers-isolated COMMAND liquid-header-tests)

add_executable(liquid-frontend-c-header headers/frontend_abi.c)
target_include_directories(liquid-frontend-c-header PRIVATE ${PROJECT_SOURCE_DIR}/include)
add_test(NAME public-frontend-c-header COMMAND liquid-frontend-c-header)

add_executable(liquid-hot-c-header headers/hot_abi.c)
target_include_directories(liquid-hot-c-header PRIVATE ${PROJECT_SOURCE_DIR}/include)
add_test(NAME public-hot-c-header COMMAND liquid-hot-c-header)
add_executable(liquid-format-c-header headers/format_abi.c)
target_include_directories(liquid-format-c-header PRIVATE ${PROJECT_SOURCE_DIR}/include)
add_test(NAME public-format-c-header COMMAND liquid-format-c-header)

add_executable(liquid-cbor-tests encoding/test_cbor.cpp)
target_sources(liquid-cbor-tests PRIVATE $<TARGET_OBJECTS:liquid_metadata_objects>)
target_include_directories(liquid-cbor-tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(liquid-cbor-tests PRIVATE
  LIQUID_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_link_libraries(liquid-cbor-tests PRIVATE
  liquid::liquid automerge-cpp::automerge-cpp GTest::gtest_main)
gtest_discover_tests(liquid-cbor-tests)

add_executable(liquid-format-tests
  formats/test_dotenv.cpp
  formats/test_json.cpp
  formats/test_registry.cpp
  ${PROJECT_SOURCE_DIR}/src/formats/dotenv.cpp
  ${PROJECT_SOURCE_DIR}/src/formats/json.cpp)
target_include_directories(liquid-format-tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(liquid-format-tests PRIVATE
  LIQUID_FORMAT_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/formats"
  LIQUID_FORMAT_TEST_MODULE="$<TARGET_FILE:liquid-format-test-module>")
target_link_libraries(liquid-format-tests PRIVATE liquid::liquid GTest::gtest_main)
add_library(liquid-format-test-module MODULE formats/module.c)
target_include_directories(liquid-format-test-module PRIVATE ${PROJECT_SOURCE_DIR}/include)
add_dependencies(liquid-format-tests liquid-format-test-module)
gtest_discover_tests(liquid-format-tests)

add_executable(liquid-encryption-tests
  encryption/test_custody.cpp
  encryption/test_leaf.cpp
  encryption/test_repository_leaf.cpp
  ${PROJECT_SOURCE_DIR}/src/crypto/crypto.cpp
  ${PROJECT_SOURCE_DIR}/src/encryption/leaf.cpp
  ${PROJECT_SOURCE_DIR}/src/formats/dotenv.cpp
  ${PROJECT_SOURCE_DIR}/src/formats/json.cpp
  ${PROJECT_SOURCE_DIR}/src/hash/sha256.c
  ${PROJECT_SOURCE_DIR}/src/identity/custody/age.cpp
  ${PROJECT_SOURCE_DIR}/src/identity/custody/common.cpp)
if(UNIX)
  target_sources(liquid-encryption-tests PRIVATE encryption/test_remote_custody.cpp)
endif()
target_include_directories(liquid-encryption-tests PRIVATE
  ${PROJECT_SOURCE_DIR}/src
  ${blake3_source_SOURCE_DIR}/c
  ${monocypher_source_SOURCE_DIR}/src)
target_compile_definitions(liquid-encryption-tests PRIVATE
  LIQUID_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_link_libraries(liquid-encryption-tests PRIVATE
  liquid::liquid liquid_blake3 liquid_monocypher GTest::gtest_main)
gtest_discover_tests(liquid-encryption-tests)

add_executable(liquid-crdt-tests crdt/test_document.cpp crdt/test_journal.cpp)
target_sources(liquid-crdt-tests PRIVATE $<TARGET_OBJECTS:liquid_metadata_objects>)
target_include_directories(liquid-crdt-tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(liquid-crdt-tests PRIVATE
  liquid::liquid automerge-cpp::automerge-cpp GTest::gtest_main)
gtest_discover_tests(liquid-crdt-tests)

add_executable(liquid-crdt-measure EXCLUDE_FROM_ALL crdt/measure_document.cpp)
target_sources(liquid-crdt-measure PRIVATE $<TARGET_OBJECTS:liquid_metadata_objects>)
target_include_directories(liquid-crdt-measure PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(liquid-crdt-measure PRIVATE
  liquid::liquid automerge-cpp::automerge-cpp)

add_executable(liquid-repository-open-measure EXCLUDE_FROM_ALL repository/measure_open.cpp)
target_link_libraries(liquid-repository-open-measure PRIVATE liquid::liquid)

add_executable(
  liquid-search-measure EXCLUDE_FROM_ALL search/measure_index.cpp
                                         ${PROJECT_SOURCE_DIR}/src/encoding/cbor.cpp
                                         ${PROJECT_SOURCE_DIR}/src/search/index.cpp
                                         ${PROJECT_SOURCE_DIR}/src/store/mapped_file.cpp)
target_include_directories(liquid-search-measure PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(liquid-search-measure PRIVATE liquid::liquid orama::oramacore-cpp)

add_executable(liquid-record-tests
  objects/test_records.cpp
  ${PROJECT_SOURCE_DIR}/src/objects/records.cpp
  ${PROJECT_SOURCE_DIR}/src/encoding/cbor.cpp)
target_include_directories(liquid-record-tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(liquid-record-tests PRIVATE
  LIQUID_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_link_libraries(liquid-record-tests PRIVATE liquid::liquid GTest::gtest_main)
gtest_discover_tests(liquid-record-tests)

add_executable(liquid-git-tests
  git/test_object_store.cpp
  ${PROJECT_SOURCE_DIR}/src/git/object_store.cpp
  ${PROJECT_SOURCE_DIR}/src/git/sha1.cpp)
target_include_directories(liquid-git-tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(liquid-git-tests PRIVATE liquid::liquid GTest::gtest_main ZLIB::ZLIB)
gtest_discover_tests(liquid-git-tests)

option(LIQUID_REALWORLD_TESTS "Register the opt-in real-world Git interoperability check" OFF)
set(LIQUID_REALWORLD_GIT_DIRECTORY "" CACHE PATH "Checkout to import in the real-world Git check")
if(LIQUID_REALWORLD_TESTS)
  if(NOT LIQUID_REALWORLD_GIT_DIRECTORY)
    message(FATAL_ERROR "LIQUID_REALWORLD_GIT_DIRECTORY is required when LIQUID_REALWORLD_TESTS=ON")
  endif()
  add_test(NAME git-realworld
    COMMAND ${CMAKE_COMMAND}
      -DLIQUID_CLI=$<TARGET_FILE:liquid-cli>
      -DLIQUID_REALWORLD_GIT_DIRECTORY=${LIQUID_REALWORLD_GIT_DIRECTORY}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/git/realworld.cmake)
  set_tests_properties(git-realworld PROPERTIES LABELS realworld)
endif()

option(LIQUID_CONTENT_CHAIN_TESTS "Register the opt-in content-addressed build chain" OFF)
if(LIQUID_CONTENT_CHAIN_TESTS AND UNIX AND LIQUID_BUILD_CLI)
  add_test(NAME content-addressed-build-chain
    COMMAND ${CMAKE_COMMAND}
      -DLIQUID_CLI=$<TARGET_FILE:liquid-cli>
      -DLIQUID_CONTENT_CHAIN_ROOT=${CMAKE_CURRENT_BINARY_DIR}/content-addressed-build-chain
      -P ${CMAKE_CURRENT_SOURCE_DIR}/build/content-addressed-chain.cmake)
  set_tests_properties(content-addressed-build-chain PROPERTIES LABELS realworld TIMEOUT 600)
endif()

add_executable(liquid-repository-scenario repository/scenario.cpp)
target_include_directories(liquid-repository-scenario PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(liquid-repository-scenario PRIVATE liquid::liquid)

add_test(NAME consumer-project-build
  COMMAND ${CMAKE_COMMAND}
    -DLIQUID_BINARY_DIR=${PROJECT_BINARY_DIR}
    -DLIQUID_CONSUMER_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/consumer
    -DLIQUID_CONSUMER_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/consumer-project
    -DLIQUID_CONSUMER_INSTALL_DIR=${CMAKE_CURRENT_BINARY_DIR}/consumer-install
    -DLIQUID_CONSUMER_GENERATOR=${CMAKE_GENERATOR}
    -DLIQUID_CONSUMER_CXX_COMPILER=${CMAKE_CXX_COMPILER}
    -DLIQUID_CONSUMER_BUILD_TYPE=${CMAKE_BUILD_TYPE}
    -DLIQUID_CONSUMER_HAS_CLI=${LIQUID_BUILD_CLI}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/consumer/run.cmake)
