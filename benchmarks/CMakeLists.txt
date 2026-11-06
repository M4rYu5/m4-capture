# Benchmarks are explicit commands, never correctness checks in CTest.
add_executable(m4capture-benchmarks capture-benchmark.cpp)
target_link_libraries(m4capture-benchmarks PRIVATE obs-test-support)
set_target_properties(m4capture-benchmarks PROPERTIES
  AUTOMOC ON RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
