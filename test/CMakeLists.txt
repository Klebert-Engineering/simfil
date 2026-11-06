project(test.simfil)

set(CMAKE_CXX_STANDARD 20) # Required for catch2
target_compile_features(Catch2 PRIVATE cxx_std_20)
target_compile_features(Catch2WithMain PRIVATE cxx_std_20)

add_executable(test.simfil
  token.cpp
  schema-type.cpp
  schema-runtime-type.cpp
  schema-node.cpp
  schema-append-checkpoint.cpp
  string-pool.cpp
  arena.cpp
  error.cpp
  value.cpp
  operator.cpp)

# Query/model fixtures use JSON ingestion to construct their data. Keep them
# all in the default build; JSON-free builds still exercise independent native
# storage, schemas, scalar values, tokenization and error behavior.
if (SIMFIL_WITH_MODEL_JSON)
  target_sources(test.simfil PRIVATE
    common.hpp
    common.cpp
    schema.cpp
    schema-domain.cpp
    schema-lookup.cpp
    schema-value.cpp
    evaluation.cpp
    simfil.cpp
    diagnostics.cpp
    completion.cpp
    complex.cpp
    model-copy.cpp
    performance.cpp)
endif()

target_link_libraries(test.simfil
  PUBLIC
    simfil
    Catch2::Catch2WithMain)

if (MSVC)
  # Ensure that tests work when compiled with Ninja on Windows.
  # The test.simfil.exe must be deployed side-by-side with simfil.dll.
  set_target_properties(test.simfil PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:simfil>)
endif()

# Make Catch2 CMake module available, brought in via CPM
list(APPEND CMAKE_MODULE_PATH "${Catch2_SOURCE_DIR}/extras")
include(Catch)
include(CTest)
if (SIMFIL_WITH_COVERAGE)
  # Exercise every benchmark body and assertion without statistical sampling:
  # instrumented timings are not meaningful, especially with shared gcov counters.
  catch_discover_tests(test.simfil EXTRA_ARGS --benchmark-samples 1 --benchmark-no-analysis)
else()
  catch_discover_tests(test.simfil)
endif()
