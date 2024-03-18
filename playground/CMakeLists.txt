add_library(simfil-wasm STATIC EXCLUDE_FROM_ALL
  playground.h
  playground.cpp)

target_link_libraries(simfil-wasm PUBLIC
  simfil::simfil)

add_custom_command(
  OUTPUT simfil-wasm.wasm simfil-wasm.js
  COMMAND emcc -o simfil-wasm.js $<TARGET_FILE:simfil-wasm> $<TARGET_FILE:simfil>
               -s EXPORTED_FUNCTIONS=_simfil_simple_json_text
               -s EXPORTED_RUNTIME_METHODS=cwrap
               -s NO_DISABLE_EXCEPTION_CATCHING
               -s ALLOW_MEMORY_GROWTH
  DEPENDS simfil-wasm)

add_custom_command(
  OUTPUT playground.html
  COMMAND cp ${CMAKE_SOURCE_DIR}/playground/index.html playground.html
  DEPENDS simfil-wasm.js)

add_custom_target(playground DEPENDS
  playground.html)
