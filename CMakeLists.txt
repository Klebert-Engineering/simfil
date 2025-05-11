cmake_minimum_required(VERSION 3.19)
enable_testing()

project(simfil
  HOMEPAGE_URL "https://github.com/Klebert-Engineering/simfil"
  LANGUAGES C CXX)

include(FetchContent)
include(GNUInstallDirs)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cmake-modules")

# Do not build additional content, if bundled
set(MAIN_PROJECT NO)
if (CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  set(MAIN_PROJECT YES)
endif()

if (CMAKE_COMPILER_IS_GNUCXX AND CMAKE_BUILD_TYPE STREQUAL "Debug")
  option(SIMFIL_WITH_COVERAGE "Enable gcovr coverage" ${MAIN_PROJECT})
else()
  set(SIMFIL_WITH_COVERAGE NO)
endif()

option(SIMFIL_SHARED          "Build a shared library" YES)
option(SIMFIL_FPIC            "Build with fPIC enabled" YES)
option(SIMFIL_WITH_REPL       "Build simfil repl" ${MAIN_PROJECT})
option(SIMFIL_WITH_EXAMPLES   "Build examples" ${MAIN_PROJECT})
option(SIMFIL_WITH_TESTS      "Build tests" ${MAIN_PROJECT})
option(SIMFIL_WITH_MODEL_JSON "Include JSON model support" YES)

find_program(GCOVR_BIN gcovr)
if (SIMFIL_WITH_COVERAGE AND NOT GCOVR_BIN)
  set(SIMFIL_WITH_COVERAGE NO)
  message(WARNING "Could not find gcovr binary. Disabling coverage report!")
endif()

if (SIMFIL_SHARED)
  set(LIBRARY_TYPE SHARED)
else()
  set(LIBRARY_TYPE STATIC)
endif()

include(deps.cmake)

add_library(simfil ${LIBRARY_TYPE}
  src/environment.cpp
  src/diagnostics.cpp
  src/function.cpp
  src/parser.cpp
  src/token.cpp
  src/types.cpp
  src/expressions.cpp
  src/simfil.cpp
  src/value.cpp
  src/overlay.cpp
  src/exception-handler.cpp
  src/model/model.cpp
  src/model/nodes.cpp
  src/model/string-pool.cpp)

target_sources(simfil PUBLIC
  FILE_SET public_headers
    TYPE HEADERS
    FILES
      include/simfil/environment.h
      include/simfil/diagnostics.h
      include/simfil/expression.h
      include/simfil/function.h
      include/simfil/operator.h
      include/simfil/parser.h
      include/simfil/token.h
      include/simfil/typed-meta-type.h
      include/simfil/types.h
      include/simfil/value.h
      include/simfil/transient.h
      include/simfil/simfil.h
      include/simfil/exception-handler.h

      include/simfil/model/arena.h
      include/simfil/model/string-pool.h
      include/simfil/model/model.h
      include/simfil/model/nodes.h
      include/simfil/model/bitsery-traits.h)

target_include_directories(simfil
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_compile_features(simfil
  PUBLIC
    cxx_std_20)

target_link_libraries(simfil
  PUBLIC
    sfl::sfl
    fmt::fmt
    Bitsery::bitsery)

if (SIMFIL_FPIC OR NOT SIMFIL_SHARED)
  set_property(TARGET simfil PROPERTY
    POSITION_INDEPENDENT_CODE ON)
endif()

if (MSVC)
  target_compile_definitions(simfil
    PRIVATE
      _CRT_SECURE_NO_WARNINGS)

  set_target_properties(simfil PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

if (SIMFIL_WITH_MODEL_JSON)
  target_compile_definitions(simfil
    PUBLIC
      SIMFIL_WITH_MODEL_JSON)
  target_sources(simfil PRIVATE
    src/model/json.cpp)
  target_sources(simfil PUBLIC
    FILE_SET public_headers
      TYPE HEADERS
      FILES
        include/simfil/model/json.h)

  target_link_libraries(simfil
    PUBLIC
      nlohmann_json::nlohmann_json)
endif()

if (SIMFIL_WITH_COVERAGE)
  include(CodeCoverage)
  append_coverage_compiler_flags()
endif()

add_library(simfil::simfil ALIAS simfil)

if (SIMFIL_WITH_TESTS)
  add_subdirectory(test)

  if (SIMFIL_WITH_COVERAGE)
    setup_target_for_coverage_gcovr_html(
      NAME coverage
      EXECUTABLE test.simfil
      BASE_DIRECTORY "${CMAKE_SOURCE_DIR}/src"
      EXCLUDE "*catch2*")
  endif()
endif()

if (SIMFIL_WITH_REPL)
  add_subdirectory(repl)
endif()

if (SIMFIL_WITH_EXAMPLES)
  add_subdirectory(examples/minimal)
endif()

install(TARGETS simfil)
install(DIRECTORY include/simfil
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
