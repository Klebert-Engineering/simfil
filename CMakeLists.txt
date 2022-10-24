cmake_minimum_required(VERSION 3.14.0)
enable_testing()

include(FetchContent)

project(simfil)

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

if (NOT DEFINED SIMFIL_WITH_MODEL_JSON)
  set(SIMFIL_WITH_MODEL_JSON YES)
endif()

add_library(simfil STATIC
  include/simfil/environment.h
  include/simfil/expression.h
  include/simfil/ext-geo.h
  include/simfil/function.h
  include/simfil/model.h
  include/simfil/operator.h
  include/simfil/parser.h
  include/simfil/token.h
  include/simfil/typed-meta-type.h
  include/simfil/types.h
  include/simfil/value.h
  include/simfil/object.h
  include/simfil/simfil.h

  src/environment.cpp
  src/function.cpp
  src/parser.cpp
  src/token.cpp
  src/types.cpp
  src/ext-geo.cpp
  src/simfil.cpp
  src/model.cpp
  src/value.cpp)

target_include_directories(simfil
  PUBLIC
    include)

target_compile_features(simfil
  PUBLIC
    cxx_std_17)

target_link_libraries(simfil
  PRIVATE
    sfl::sfl
    stx::stx)

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

add_library(simfil::simfil ALIAS simfil)

add_subdirectory(repl)

if (MAIN_PROJECT)
  add_subdirectory(test)
endif()
