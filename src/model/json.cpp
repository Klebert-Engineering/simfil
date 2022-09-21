// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#include "simfil/model/json.h"
#include "simfil/model.h"

#include <nlohmann/json.hpp>

namespace simfil::json
{
using json = nlohmann::json;

template <class Type>
static ModelNode* make(Type&& v, Model& model)
{
    return &model.scalars.emplace_back(Value::make(std::forward<Type>(v)));
}

static ModelNode* make(Value&& v, Model& model)
{
    return &model.scalars.emplace_back(std::move(v));
}

static ModelNode* build(const json& j, Model& model)
{
    switch (j.type()) {
    case json::value_t::null:
        return make(Value::null(), model);
    case json::value_t::boolean:
        return make(j.get<bool>(), model);
    case json::value_t::number_float:
        return make(j.get<double>(), model);
    case json::value_t::number_integer:
        return make(j.get<int64_t>(), model);
    case json::value_t::number_unsigned:
        return make((int64_t)j.get<uint64_t>(), model);
    case json::value_t::string:
        return make(j.get<std::string>(), model);
    default:
        break;
    }

    if (j.is_object()) {
        auto& r = model.objects.emplace_back();
        for (auto&& [key, value] : j.items()) {
            r.nodes_[model.strings->getOrInsert(key)] = build(value, model);
        }
        return &r;
    }

    if (j.is_array()) {
        auto& r = model.arrays.emplace_back();
        for (const auto& value : j) {
            r.nodes_.push_back(build(value, model));
        }
        return &r;
    }

    return make(Value::null(), model);
}

std::unique_ptr<Model> parse(std::istream& input)
{
    auto model = std::make_unique<Model>();
    build(json::parse(input), *model);
    return model;
}

std::unique_ptr<Model> parse(const std::string& input)
{
    auto model = std::make_unique<Model>();
    build(json::parse(input), *model);
    return model;
}

}
