cmake_minimum_required(VERSION 3.23)
project(simfil-schema-runtime-benchmark LANGUAGES C CXX)

set(SIMFIL_SOURCE_DIR "" CACHE PATH "Simfil checkout to measure")
set(SIMFIL_CPM_SOURCE "" CACHE PATH "Optional initialized CPM checkout")
option(SIMFIL_PERF_REFERENCE "Build the original callback-only reference" OFF)
if (NOT EXISTS "${SIMFIL_SOURCE_DIR}/include/simfil/simfil.h")
    message(FATAL_ERROR "Set SIMFIL_SOURCE_DIR to the checkout being measured")
endif ()
if (NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    message(FATAL_ERROR "Use a Release build for comparable measurements")
endif ()

# A detached reference worktree need not populate another CPM submodule.
if (SIMFIL_CPM_SOURCE)
    add_subdirectory("${SIMFIL_CPM_SOURCE}" cpm)
    set(CPM_INITIALIZED ON)
endif ()
set(SIMFIL_SHARED OFF CACHE BOOL "" FORCE)
set(SIMFIL_WITH_COVERAGE OFF CACHE BOOL "" FORCE)
set(SIMFIL_WITH_TESTS OFF CACHE BOOL "" FORCE)
set(SIMFIL_WITH_REPL OFF CACHE BOOL "" FORCE)
set(SIMFIL_WITH_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SIMFIL_WITH_MODEL_JSON ON CACHE BOOL "" FORCE)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_subdirectory("${SIMFIL_SOURCE_DIR}" simfil)

find_package(Threads REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_executable(schema-runtime-benchmark schema-runtime-benchmark.cpp)
target_link_libraries(schema-runtime-benchmark PRIVATE simfil Threads::Threads)
if (SIMFIL_PERF_REFERENCE)
    target_compile_definitions(schema-runtime-benchmark PRIVATE SIMFIL_PERF_REFERENCE)
endif ()

# Capture source and binary identities at link time, not retrospectively after edits.
add_custom_command(TARGET schema-runtime-benchmark POST_BUILD
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/schema-runtime-benchmark.py"
        manifest --binary "$<TARGET_FILE:schema-runtime-benchmark>"
        --source "${SIMFIL_SOURCE_DIR}" --build "${CMAKE_BINARY_DIR}"
    VERBATIM)
