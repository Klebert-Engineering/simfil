// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#include "simfil/model/json.h"
#include "simfil/model.h"

#include <nlohmann/json.hpp>

namespace simfil::json
{
using json = nlohmann::json;

static ModelPool::ModelNodeIndex build(const json& j, ModelPool & model)
{
    switch (j.type()) {
    case json::value_t::null:
        return model.addNull();
    case json::value_t::boolean:
        return model.addValue(j.get<bool>());
    case json::value_t::number_float:
        return model.addValue(j.get<double>());
    case json::value_t::number_unsigned:
        return model.addValue((int64_t)j.get<uint64_t>());
    case json::value_t::number_integer:
        return model.addValue(j.get<int64_t>());
    case json::value_t::string:
        return model.addValue(j.get<std::string>());
    default:
        break;
    }

    if (j.is_object()) {
        std::vector<ModelPool::Member> members;
        members.reserve(j.size());
        for (auto&& [key, value] : j.items()) {
            members.emplace_back(model.strings->emplace(key), build(value, model));
        }
        return model.addObject(members);
    }

    if (j.is_array()) {
        std::vector<ModelPool::Member> members;
        members.reserve(j.size());
        for (const auto& value : j) {
            members.emplace_back(Strings::Empty, build(value, model));
        }
        return model.addArray(members);
    }

    return model.addNull();
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
