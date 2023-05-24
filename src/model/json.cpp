// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#include "simfil/model/json.h"
#include "simfil/model/model.h"

#include <nlohmann/json.hpp>

namespace simfil::json
{
using json = nlohmann::json;

static ModelNode::Ptr build(const json& j, ModelPool & model)
{
    switch (j.type()) {
    case json::value_t::null:
        return {};
    case json::value_t::boolean:
        return model.newSmallValue(j.get<bool>());
    case json::value_t::number_float:
        return model.newValue(j.get<double>());
    case json::value_t::number_unsigned:
        return model.newValue((int64_t)j.get<uint64_t>());
    case json::value_t::number_integer:
        return model.newValue(j.get<int64_t>());
    case json::value_t::string:
        return model.newValue(j.get<std::string>());
    default:
        break;
    }

    if (j.is_object()) {
        auto object = model.newObject(j.size());
        for (auto&& [key, value] : j.items())
            object->addField(key, build(value, model));
        return object;
    }

    if (j.is_array()) {
        auto array = model.newArray(j.size());
        for (const auto& value : j)
            array->append(build(value, model));
        return array;
    }

    return {};
}

void parse(std::istream& input, ModelPoolPtr const& model)
{
    model->addRoot(build(json::parse(input), *model));
    model->validate();
}

void parse(const std::string& input, ModelPoolPtr const& model)
{
    model->addRoot(build(json::parse(input), *model));
    model->validate();
}

ModelPoolPtr parse(const std::string& input)
{
    auto model = std::make_shared<simfil::ModelPool>();
    model->addRoot(build(json::parse(input), *model));
    model->validate();
    return model;
}

}
