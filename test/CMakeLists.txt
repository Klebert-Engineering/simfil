project(test.simfil)

set(CMAKE_CXX_STANDARD 17) # Required for catch2
find_package(Catch2 REQUIRED)

add_executable(test.simfil
  token.cpp
  simfil.cpp
  complex.cpp
  ext-geo.cpp
  performance.cpp
  arena.cpp)

target_link_libraries(test.simfil
  PUBLIC
    simfil
    Catch2::Catch2WithMain)

include(Catch)
include(CTest)
catch_discover_tests(test.simfil)
