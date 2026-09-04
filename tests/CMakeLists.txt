add_executable(liquid-tests
  test_drip.cpp
  test_error.cpp
  test_hash.cpp
  test_identity.cpp
  test_object.cpp
  test_remote.cpp
  test_repository.cpp
  store/test_store.cpp
  ${PROJECT_SOURCE_DIR}/src/store/chunker.cpp
  test_version.cpp
  test_vfs.cpp
)
target_link_libraries(liquid-tests PRIVATE liquid::liquid GTest::gtest_main)
target_include_directories(liquid-tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
include(GoogleTest)
gtest_discover_tests(liquid-tests)

add_executable(liquid-store-large-tests store/test_store_large.cpp)
target_link_libraries(liquid-store-large-tests PRIVATE liquid::liquid GTest::gtest_main)
add_test(NAME store-256-mib-round-trip COMMAND liquid-store-large-tests)
set_tests_properties(store-256-mib-round-trip PROPERTIES LABELS large)

add_executable(liquid-store-http-fixture store/http_range_fixture.cpp)
target_link_libraries(liquid-store-http-fixture PRIVATE liquid::liquid)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(
  NAME store-pack-http-range
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/store/http_range_test.py
          $<TARGET_FILE:liquid-store-http-fixture>)

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
  headers/store.cpp
  headers/version.cpp
  headers/vfs.cpp)
target_link_libraries(liquid-header-tests PRIVATE liquid::liquid)
add_test(NAME public-headers-isolated COMMAND liquid-header-tests)
