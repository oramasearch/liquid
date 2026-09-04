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
target_link_libraries(liquid-tests PRIVATE liquid::liquid GTest::gtest_main)
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
  ${PROJECT_SOURCE_DIR}/src/store/pack.cpp)
target_include_directories(liquid-index-tests PRIVATE ${PROJECT_SOURCE_DIR}/src/store)
target_link_libraries(liquid-index-tests PRIVATE liquid::liquid GTest::gtest_main Boost::corosio libzstd_static)
gtest_discover_tests(liquid-index-tests)

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
