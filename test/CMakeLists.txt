project(test.simfil)

set(CMAKE_CXX_STANDARD 17) # Required for catch2
if (NOT TARGET Catch2)
  FetchContent_Declare(Catch2
    GIT_REPOSITORY "https://github.com/catchorg/Catch2.git"
    GIT_TAG        "v3.1.0"
    GIT_SHALLOW    ON)
  FetchContent_MakeAvailable(Catch2)
  list(APPEND CMAKE_MODULE_PATH "${CATCH2_SOURCE_DIR}/contrib")
endif()

add_executable(test.simfil
  simfil.cpp
  token.cpp
  ext-geo.cpp)

target_link_libraries(test.simfil
  PUBLIC
    simfil
    Catch2::Catch2WithMain)

include(Catch)
include(CTest)
catch_discover_tests(test.simfil)
