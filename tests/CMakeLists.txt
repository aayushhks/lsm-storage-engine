add_executable(lsm_tests
  db_test.cpp
)

target_link_libraries(lsm_tests PRIVATE lsm lsm_warnings GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(lsm_tests)
