cmake_minimum_required(VERSION 3.14.0)
enable_testing()

include(FetchContent)

project(simfil)

if (CMAKE_COMPILER_IS_GNUCXX AND CMAKE_BUILD_TYPE STREQUAL "Debug")
  option(WITH_COVERAGE "Enable gcovr coverage" YES)
else()
  set(WITH_COVERAGE NO)
endif()

find_program(GCOVR_BIN gcovr)
if (WITH_COVERAGE AND NOT GCOVR_BIN)
  set(WITH_COVERAGE NO)
  message(WARNING "Could not find gcovr binary. Disabling coverage report!")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/cmake-modules")

# Do not build tests, if bundled
set(MAIN_PROJECT OFF)
if (CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  set(MAIN_PROJECT ON)
endif()

if (NOT TARGET sfl)
  FetchContent_Declare(sfl
    GIT_REPOSITORY "https://github.com/slavenf/sfl-library.git"
    GIT_TAG        "master"
    GIT_SHALLOW    ON)
  FetchContent_MakeAvailable(sfl)
  add_library(sfl INTERFACE)
  FetchContent_GetProperties(sfl SOURCE_DIR sfl_source_dir)
  target_include_directories(sfl INTERFACE "${sfl_source_dir}/include")
  add_library(sfl::sfl ALIAS sfl)
endif()

if (NOT TARGET stx)
  FetchContent_Declare(stx
    GIT_REPOSITORY "https://github.com/Klebert-Engineering/stx.git"
    GIT_TAG        "main"
    GIT_SHALLOW    ON)
  FetchContent_MakeAvailable(stx)
endif()

if (NOT TARGET bitsery)
  FetchContent_Declare(bitsery
    GIT_REPOSITORY "https://github.com/fraillt/bitsery.git"
    GIT_TAG        "master"
    GIT_SHALLOW    ON)
  FetchContent_MakeAvailable(bitsery)
endif()

if (NOT DEFINED SIMFIL_WITH_MODEL_JSON)
  set(SIMFIL_WITH_MODEL_JSON YES)
endif()

if (WITH_COVERAGE)
  include(CodeCoverage)
endif()

add_library(simfil STATIC
  include/simfil/environment.h
  include/simfil/expression.h
  include/simfil/ext-geo.h
  include/simfil/function.h
  include/simfil/operator.h
  include/simfil/parser.h
  include/simfil/token.h
  include/simfil/typed-meta-type.h
  include/simfil/types.h
  include/simfil/value.h
  include/simfil/transient.h
  include/simfil/simfil.h

  include/simfil/model/arena.h
  include/simfil/model/fields.h
  include/simfil/model/point.h
  include/simfil/model/model.h
  include/simfil/model/nodes.h
  include/simfil/model/bitsery-traits.h

  src/environment.cpp
  src/function.cpp
  src/parser.cpp
  src/token.cpp
  src/types.cpp
  src/ext-geo.cpp
  src/simfil.cpp
  src/value.cpp
  src/overlay.cpp

  src/model/model.cpp
  src/model/nodes.cpp
  src/model/fields.cpp)

target_include_directories(simfil
  PUBLIC
    include)

target_compile_features(simfil
  PUBLIC
    cxx_std_17)

target_link_libraries(simfil
  PUBLIC
    sfl::sfl
    stx::stx
    Bitsery::bitsery)

if (MSVC)
  target_compile_definitions(simfil
    PRIVATE
      _CRT_SECURE_NO_WARNINGS)

  set_target_properties(simfil PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

if (SIMFIL_WITH_MODEL_JSON)
  if (NOT TARGET nlohmann_json)
    FetchContent_Declare(nlohmann_json
      GIT_REPOSITORY "https://github.com/nlohmann/json.git"
      GIT_TAG        "v3.11.2"
      GIT_SHALLOW    ON)
    FetchContent_MakeAvailable(nlohmann_json)
  endif()

  target_compile_definitions(simfil
    PUBLIC
      SIMFIL_WITH_MODEL_JSON)

  target_sources(simfil
    PRIVATE
      include/simfil/model/json.h
      src/model/json.cpp)

  target_link_libraries(simfil
    PRIVATE
      nlohmann_json::nlohmann_json)
endif()

if (WITH_COVERAGE)
  append_coverage_compiler_flags_to_target(simfil)
endif()

add_library(simfil::simfil ALIAS simfil)

add_subdirectory(repl)

if (MAIN_PROJECT)
  add_subdirectory(test)

  if (WITH_COVERAGE)
    setup_target_for_coverage_gcovr_html(
      NAME coverage
      EXECUTABLE test.simfil
      BASE_DIRECTORY "${CMAKE_SOURCE_DIR}/src"
      EXCLUDE "*catch2*")
  endif()
endif()
