project(test.simfil)

set(CMAKE_CXX_STANDARD 17) # Required for catch2

add_executable(test.simfil
  token.cpp
  simfil.cpp
  complex.cpp
  performance.cpp
  arena.cpp)

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

include(Catch)
include(CTest)
catch_discover_tests(test.simfil)
