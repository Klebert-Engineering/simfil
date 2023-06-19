#include <emscripten.h>
#include <string>

extern "C" {
    EMSCRIPTEN_KEEPALIVE 
    const char* simfil_simple_json_text(
        const char* query,
        const char* json);
}
