include(FetchContent)

if (SIMFIL_SHARED)
  set (CMAKE_POSITION_INDEPENDENT_CODE ON)
endif ()

if (NOT TARGET sfl::sfl)
  FetchContent_Declare(sfl
    GIT_REPOSITORY "https://github.com/slavenf/sfl-library.git"
    GIT_TAG        "master"
    GIT_SHALLOW    ON)
  FetchContent_MakeAvailable(sfl)
endif()

if (NOT TARGET fmt::fmt)
  FetchContent_Declare(fmt
    GIT_REPOSITORY "https://github.com/fmtlib/fmt.git"
    GIT_TAG        "11.1.4"
    GIT_SHALLOW    ON)
  FetchContent_MakeAvailable(fmt)
endif()

if (NOT TARGET Bitsery::bitsery)
  FetchContent_Declare(bitsery
    GIT_REPOSITORY "https://github.com/fraillt/bitsery.git"
    GIT_TAG        "v5.2.4"
    GIT_SHALLOW    ON)
  FetchContent_MakeAvailable(bitsery)
endif()

if (NOT TARGET tl::expected)
  set(EXPECTED_BUILD_TESTS NO)
  set(EXPECTED_BUILD_PACKAGE_DEB NO)
  FetchContent_Declare(expected
    GIT_REPOSITORY "https://github.com/TartanLlama/expected.git"
    GIT_TAG        "v1.1.0"
    GIT_SHALLOW    ON)
  FetchContent_MakeAvailable(expected)
endif()

if (SIMFIL_WITH_MODEL_JSON)
  if (NOT TARGET nlohmann_json::nlohmann_json)
    FetchContent_Declare(nlohmann_json
      GIT_REPOSITORY "https://github.com/nlohmann/json.git"
      GIT_TAG        "v3.11.3"
      GIT_SHALLOW    ON)
    FetchContent_MakeAvailable(nlohmann_json)
  endif()
endif()

if (SIMFIL_WITH_TESTS)
  if (NOT TARGET Catch2::Catch2WithMain)
    FetchContent_Declare(catch2
      GIT_REPOSITORY "https://github.com/catchorg/Catch2.git"
      GIT_TAG        "v3.5.2"
      GIT_SHALLOW    ON
      FIND_PACKAGE_ARGS)
    FetchContent_MakeAvailable(catch2)
  endif()
endif()
