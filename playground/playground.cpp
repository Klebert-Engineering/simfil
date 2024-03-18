#include "playground.h"

#include "simfil/simfil.h"
#include "simfil/expression.h"
#include "simfil/value.h"
#include "simfil/model/model.h"
#include "simfil/model/json.h"

#include <cstring>
#include <sstream>

const char* EMSCRIPTEN_KEEPALIVE simfil_simple_json_text(const char* query,
                                                         const char* json)
{
    static auto buffer = std::string();
    try {
        auto env = simfil::Environment{simfil::Environment::WithNewStringCache};
        auto model = std::make_shared<simfil::ModelPool>(env.fieldNames());

        auto f = std::stringstream(json);
        simfil::json::parse(f, model);

        auto expr = simfil::compile(env, std::string(query), false);
        auto res = simfil::eval(env, *expr, *model);

        std::stringstream ss;
        for (const auto& v : res) {
            ss << v.toString() << "\n";
        }
        
        buffer = ss.str();
        return buffer.c_str();
    } catch (const std::exception& e) {
        buffer = e.what();
        return buffer.c_str();
    } catch (...) {
        return "BUG";
    }
}

