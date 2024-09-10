include(FetchContent)

if (SIMFIL_CONAN)
  find_package(sfl REQUIRED)
  find_package(fmt REQUIRED)
  find_package(Bitsery REQUIRED)
  if (SIMFIL_WITH_MODEL_JSON)
    find_package(nlohmann_json REQUIRED)
  endif()
else()
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
      GIT_TAG        "10.0.0"
      GIT_SHALLOW    ON)
    FetchContent_MakeAvailable(fmt)
  endif()

  if (NOT TARGET bitsery::bitsery)
    FetchContent_Declare(bitsery
      GIT_REPOSITORY "https://github.com/fraillt/bitsery.git"
      GIT_TAG        "v5.2.3"
      GIT_SHALLOW    ON)
    FetchContent_MakeAvailable(bitsery)
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
