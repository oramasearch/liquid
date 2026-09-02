add_executable(liquid-tests
  test_version.cpp
)
target_link_libraries(liquid-tests PRIVATE liquid-core GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(liquid-tests)
