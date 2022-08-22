// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#include "simfil/model/json.h"
#include "simfil/model.h"

#include <nlohmann/json.hpp>

namespace simfil::json
{
using json = nlohmann::json;

template <class Type>
static std::unique_ptr<ModelNode> make(Type&& v)
{
    return std::make_unique<ScalarNode>(Value::make(std::forward<Type>(v)));
}

static std::unique_ptr<ModelNode> make(Value&& v)
{
    return std::make_unique<ScalarNode>(std::move(v));
}

static std::unique_ptr<ModelNode> build(const json& j)
{
    switch (j.type()) {
    case json::value_t::null:
        return make(Value::null());
    case json::value_t::boolean:
        return make(j.get<bool>());
    case json::value_t::number_float:
        return make(j.get<double>());
    case json::value_t::number_integer:
        return make(j.get<int64_t>());
    case json::value_t::number_unsigned:
        return make((int64_t)j.get<uint64_t>());
    case json::value_t::string:
        return make(j.get<std::string>());
    default:
        break;
    }

    if (j.is_object()) {
        auto r = std::make_unique<ObjectNode>();
        for (auto&& [key, value] : j.items()) {
            r->nodes_[std::move(key)] = build(value);
        }

        return r;
    }

    if (j.is_array()) {
        auto r = std::make_unique<ArrayNode>();
        for (const auto& value : j) {
            r->nodes_.push_back(build(value));
        }

        return r;
    }

    return make(Value::null());
}

std::unique_ptr<ModelNode> parse(std::istream& input)
{
    return build(json::parse(input));
}

std::unique_ptr<ModelNode> parse(const std::string& input)
{
    return build(json::parse(input));
}

}
