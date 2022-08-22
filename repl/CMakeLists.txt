project(simfil-repl)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

if (NOT MSVC)
  find_package(PkgConfig)
  if (PkgConfig_FOUND)
    pkg_search_module(READLINE readline)
  endif()
endif()

add_executable(simfil-repl
  repl.cpp)

target_link_libraries(simfil-repl
  PUBLIC
    simfil
    Threads::Threads
    ${READLINE_LIBRARIES})

target_include_directories(simfil-repl
  PUBLIC
    ${READLINE_INCLUDE_DIRS})

if (READLINE_FOUND)
  target_compile_definitions(simfil-repl
    PRIVATE
      -DWITH_READLINE=1)
endif()
