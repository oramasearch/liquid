add_executable(liquid-tests
  test_drip.cpp
  test_error.cpp
  test_hash.cpp
  test_identity.cpp
  test_object.cpp
  test_remote.cpp
  test_repository.cpp
  test_runtime.cpp
  test_store.cpp
  scan/test_capture.cpp
  scan/test_detectors.cpp
  scan/test_redaction.cpp
  ${PROJECT_SOURCE_DIR}/src/scan/detectors.cpp
  store/test_gc.cpp
  ${PROJECT_SOURCE_DIR}/src/store/chunker.cpp
  ${PROJECT_SOURCE_DIR}/src/objects/records.cpp
  ${PROJECT_SOURCE_DIR}/src/encoding/cbor.cpp
  test_version.cpp
  test_vfs.cpp
  vfs/test_materialise.cpp
  vfs/test_mount.cpp
)
target_link_libraries(liquid-tests PRIVATE liquid::liquid liquid_monocypher GTest::gtest_main)
target_include_directories(liquid-tests PRIVATE
  ${PROJECT_SOURCE_DIR}/src
  ${monocypher_source_SOURCE_DIR}/src
  ${monocypher_source_SOURCE_DIR}/src/optional)
include(GoogleTest)
gtest_discover_tests(liquid-tests)

if(LIQUID_BUILD_CLI)
  add_executable(liquid-gc-cli-fixture store/gc_cli_fixture.cpp)
  target_link_libraries(liquid-gc-cli-fixture PRIVATE liquid::liquid)
  set(LIQUID_GC_CLI_STORE ${CMAKE_CURRENT_BINARY_DIR}/gc-cli-store)
  add_test(NAME gc-cli-setup
    COMMAND liquid-gc-cli-fixture ${LIQUID_GC_CLI_STORE})
  set_tests_properties(gc-cli-setup PROPERTIES FIXTURES_SETUP gc_cli)
  add_test(NAME gc-cli-explain
    COMMAND $<TARGET_FILE:liquid-cli> gc --explain ${LIQUID_GC_CLI_STORE})
  set_tests_properties(gc-cli-explain PROPERTIES
    FIXTURES_REQUIRED gc_cli
    PASS_REGULAR_EXPRESSION "retained: lease")
  add_test(NAME gc-cli-compact
    COMMAND $<TARGET_FILE:liquid-cli> gc --compact --compact-below 0.5 ${LIQUID_GC_CLI_STORE})
  set_tests_properties(gc-cli-compact PROPERTIES
    FIXTURES_REQUIRED gc_cli
    PASS_REGULAR_EXPRESSION "packs_compacted=0")
  add_test(NAME gc-cli-clean
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${LIQUID_GC_CLI_STORE})
  set_tests_properties(gc-cli-clean PROPERTIES FIXTURES_CLEANUP gc_cli)
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
  headers/drip.cpp
  headers/error.cpp
  headers/export.cpp
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

add_executable(liquid-record-tests
  objects/test_records.cpp
  ${PROJECT_SOURCE_DIR}/src/objects/records.cpp
  ${PROJECT_SOURCE_DIR}/src/encoding/cbor.cpp)
target_include_directories(liquid-record-tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(liquid-record-tests PRIVATE
  LIQUID_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_link_libraries(liquid-record-tests PRIVATE liquid::liquid GTest::gtest_main)
gtest_discover_tests(liquid-record-tests)

add_executable(liquid-repository-scenario repository/scenario.cpp)
target_include_directories(liquid-repository-scenario PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(liquid-repository-scenario PRIVATE liquid::liquid)
