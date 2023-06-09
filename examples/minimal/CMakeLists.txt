project(simfil-minimal-example)

add_executable(simfil-minimal-example
  main.cpp)

target_link_libraries(simfil-minimal-example
  PUBLIC
    simfil)
