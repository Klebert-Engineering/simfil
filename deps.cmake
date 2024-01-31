include(FetchContent)

FetchContent_Declare(sfl
  GIT_REPOSITORY "https://github.com/slavenf/sfl-library.git"
  GIT_TAG        "master"
  GIT_SHALLOW    ON
  FIND_PACKAGE_ARGS)

FetchContent_Declare(fmt
  GIT_REPOSITORY "https://github.com/fmtlib/fmt.git"
  GIT_TAG        "10.2.1"
  GIT_SHALLOW    ON
  FIND_PACKAGE_ARGS)

FetchContent_Declare(Bitsery
  GIT_REPOSITORY "https://github.com/fraillt/bitsery.git"
  GIT_TAG        "v5.2.3"
  GIT_SHALLOW    ON
  FIND_PACKAGE_ARGS)

FetchContent_Declare(nlohmann_json
  GIT_REPOSITORY "https://github.com/nlohmann/json.git"
  GIT_TAG        "v3.11.2"
  GIT_SHALLOW    ON
  FIND_PACKAGE_ARGS)

FetchContent_Declare(Catch2
  GIT_REPOSITORY "https://github.com/catchorg/Catch2.git"
  GIT_TAG        "v3.5.2"
  GIT_SHALLOW    ON
  FIND_PACKAGE_ARGS)

FetchContent_MakeAvailable(sfl fmt Bitsery)
if (SIMFIL_WITH_MODEL_JSON)
  FetchContent_MakeAvailable(nlohmann_json)
endif()
if (SIMFIL_WITH_TESTS)
  FetchContent_MakeAvailable(Catch2)
endif()
