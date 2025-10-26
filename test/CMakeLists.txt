project(test.simfil)

set(CMAKE_CXX_STANDARD 20) # Required for catch2
target_compile_features(Catch2 PRIVATE cxx_std_20)
target_compile_features(Catch2WithMain PRIVATE cxx_std_20)

add_executable(test.simfil
  common.hpp
  common.cpp
  token.cpp
  simfil.cpp
  diagnostics.cpp
  completion.cpp
  complex.cpp
  performance.cpp
  arena.cpp
  error.cpp
  value.cpp
  operator.cpp)

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
catch_discover_tests(test.simfil)
