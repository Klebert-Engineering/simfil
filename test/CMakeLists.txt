project(test.simfil)

set(CMAKE_CXX_STANDARD 17) # Required for catch2
if (NOT TARGET Catch2)
  FetchContent_Declare(Catch2
    GIT_REPOSITORY "https://github.com/catchorg/Catch2.git"
    GIT_TAG        "v3.1.0"
    GIT_SHALLOW    ON)
  FetchContent_MakeAvailable(Catch2)
endif()

add_executable(test.simfil
  simfil.cpp
  token.cpp
  ext-geo.cpp)

target_link_libraries(test.simfil
  PUBLIC
    simfil
    Catch2::Catch2WithMain)

add_test(NAME test.simfil COMMAND test.simfil)
