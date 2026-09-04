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
  test_version.cpp
  test_vfs.cpp
)
target_link_libraries(liquid-tests PRIVATE liquid::liquid liquid_monocypher GTest::gtest_main)
target_include_directories(liquid-tests PRIVATE
  ${PROJECT_SOURCE_DIR}/src
  ${monocypher_source_SOURCE_DIR}/src
  ${monocypher_source_SOURCE_DIR}/src/optional)
include(GoogleTest)
gtest_discover_tests(liquid-tests)

add_executable(liquid-store-large-tests test_store_large.cpp)
target_link_libraries(liquid-store-large-tests PRIVATE liquid::liquid GTest::gtest_main)
gtest_discover_tests(liquid-store-large-tests PROPERTIES LABELS large)

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
