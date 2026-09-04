add_executable(liquid-tests
  test_drip.cpp
  test_delta.cpp
  test_error.cpp
  test_hash.cpp
  http2/test_codecs.cpp
  test_identity.cpp
  test_object.cpp
  test_remote.cpp
  reapi/test_wire.cpp
  reapi/test_messages.cpp
  reapi/test_conversion.cpp
  ${PROJECT_SOURCE_DIR}/src/protobuf/wire.cpp
  ${PROJECT_SOURCE_DIR}/src/reapi/messages.cpp
  ${PROJECT_SOURCE_DIR}/src/reapi/conversion.cpp
  test_repository.cpp
  test_runtime.cpp
  test_store.cpp
  attestation/test_registry.cpp
  scan/test_capture.cpp
  scan/test_detectors.cpp
  scan/test_redaction.cpp
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
  build/test_policy.cpp
  build/test_facts.cpp
  build/test_provenance.cpp
  build/test_why.cpp
  build/test_sbom.cpp
  build/test_wrap.cpp
)
target_link_libraries(liquid-tests PRIVATE liquid::liquid liquid_monocypher GTest::gtest_main Boost::corosio)
target_include_directories(liquid-tests PRIVATE
  ${PROJECT_SOURCE_DIR}/src
  ${PROJECT_SOURCE_DIR}/modules/corosio/include
  ${monocypher_source_SOURCE_DIR}/src
  ${monocypher_source_SOURCE_DIR}/src/optional)
include(GoogleTest)
gtest_discover_tests(liquid-tests)

add_executable(liquid-http2-test-server http2/test_server.cpp)
target_link_libraries(liquid-http2-test-server PRIVATE liquid::liquid Boost::corosio)
target_include_directories(liquid-http2-test-server PRIVATE ${PROJECT_SOURCE_DIR}/src)

add_executable(liquid-build-fixture build/fixture.c)
if(UNIX AND NOT APPLE)
  target_link_options(liquid-build-fixture PRIVATE -static)
endif()
add_dependencies(liquid-tests liquid-build-fixture)
target_compile_definitions(liquid-tests PRIVATE
  LIQUID_BUILD_FIXTURE="$<TARGET_FILE:liquid-build-fixture>"
  LIQUID_WRAP_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/build/wrap"
  LIQUID_BUILD_HAS_FUSE=$<BOOL:${LIQUID_FUSE_TARGET}>
  LIQUID_REAPI_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/reapi/fixtures")
if(UNIX)
  add_executable(liquid-wrap-sleep-tool build/sleep_copy.c)
  add_dependencies(liquid-tests liquid-wrap-sleep-tool)
  target_compile_definitions(liquid-tests PRIVATE
    LIQUID_WRAP_SLEEP_TOOL="$<TARGET_FILE:liquid-wrap-sleep-tool>")
endif()

if(LIQUID_BUILD_CLI)
  target_compile_definitions(liquid-tests PRIVATE LIQUID_CLI="$<TARGET_FILE:liquid-cli>")
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
      -DLIQUID_CLI_DOCS=${PROJECT_SOURCE_DIR}/docs/cli/README.md
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cli/help-docs.cmake)
  add_test(NAME cli-delta
    COMMAND ${CMAKE_COMMAND}
      -DLIQUID_CLI=$<TARGET_FILE:liquid-cli>
      -DLIQUID_DELTA_ROOT=${CMAKE_CURRENT_BINARY_DIR}/delta-cli
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cli/delta.cmake)
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
  add_test(NAME cli-sbom-spdx
    COMMAND $<TARGET_FILE:liquid-cli> sbom $<TARGET_FILE:liquid-build-fixture> --spdx)
  set_tests_properties(cli-sbom-spdx PROPERTIES
    PASS_REGULAR_EXPRESSION "\"spdxVersion\"[^\n]*\"SPDX-2.3\"")
  add_test(NAME cli-sbom-cyclonedx
    COMMAND $<TARGET_FILE:liquid-cli> sbom $<TARGET_FILE:liquid-build-fixture> --cyclonedx)
  set_tests_properties(cli-sbom-cyclonedx PROPERTIES
    PASS_REGULAR_EXPRESSION "\"bomFormat\"[^\n]*\"CycloneDX\"")
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
  headers/attestation.cpp
  headers/build.cpp
  headers/drip.cpp
  headers/delta.cpp
  headers/error.cpp
  headers/export.cpp
  headers/git.cpp
  headers/hash.cpp
  headers/identity.cpp
  headers/liquid.cpp
  headers/main.cpp
  headers/object.cpp
  headers/remote.cpp
  headers/repository.cpp
  headers/runtime.cpp
  headers/store.cpp
  headers/version.cpp
  headers/vfs.cpp)
target_link_libraries(liquid-header-tests PRIVATE liquid::liquid)
add_test(NAME public-headers-isolated COMMAND liquid-header-tests)

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
  ${PROJECT_SOURCE_DIR}/src/formats/dotenv.cpp
  ${PROJECT_SOURCE_DIR}/src/formats/json.cpp)
target_include_directories(liquid-format-tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(liquid-format-tests PRIVATE liquid::liquid GTest::gtest_main)
gtest_discover_tests(liquid-format-tests)

add_executable(liquid-encryption-tests
  encryption/test_leaf.cpp
  encryption/test_repository_leaf.cpp
  ${PROJECT_SOURCE_DIR}/src/crypto/crypto.cpp
  ${PROJECT_SOURCE_DIR}/src/encryption/leaf.cpp
  ${PROJECT_SOURCE_DIR}/src/formats/dotenv.cpp
  ${PROJECT_SOURCE_DIR}/src/formats/json.cpp)
target_include_directories(liquid-encryption-tests PRIVATE
  ${PROJECT_SOURCE_DIR}/src
  ${blake3_source_SOURCE_DIR}/c
  ${monocypher_source_SOURCE_DIR}/src)
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

add_executable(liquid-repository-scenario repository/scenario.cpp)
target_include_directories(liquid-repository-scenario PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(liquid-repository-scenario PRIVATE liquid::liquid)
