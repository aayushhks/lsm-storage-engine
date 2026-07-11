add_library(lsm
  status.cpp
  crc32.cpp
  coding.cpp
  file_util.cpp
  bloom.cpp
  skiplist.cpp
  memtable.cpp
  wal.cpp
  sstable.cpp
  manifest.cpp
  compaction.cpp
  db.cpp
  db_impl.cpp
)

find_package(Threads REQUIRED)

target_include_directories(lsm PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(lsm PRIVATE lsm_warnings PUBLIC Threads::Threads)
