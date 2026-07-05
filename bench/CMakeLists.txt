add_executable(lsm_bench
  main.cpp
)

target_link_libraries(lsm_bench PRIVATE lsm lsm_warnings)
