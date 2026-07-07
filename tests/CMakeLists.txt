add_executable(lsm_tests
  db_test.cpp
  skiplist_test.cpp
  memtable_test.cpp
  crc32_test.cpp
  wal_test.cpp
  recovery_test.cpp
)

# tests reach into internal headers (skiplist, memtable, wal, ...) that live in src/.
target_include_directories(lsm_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(lsm_tests PRIVATE lsm lsm_warnings GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(lsm_tests)
