// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#include "simfil/model/json.h"
#include "simfil/model/model.h"

#include <nlohmann/json.hpp>

#include "../expected.h"

namespace simfil::json
{
using json = nlohmann::json;

static auto build(const json& j, ModelPool & model) -> tl::expected<ModelNode::Ptr, Error>
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
        if (auto stringId = model.strings()->emplace(j.get<std::string>()); stringId)
            return model.newValue((StringId)*stringId);
        else
            return tl::unexpected<Error>(stringId.error());
    default:
        break;
    }

    if (j.is_object()) {
        auto object = model.newObject(j.size());
        for (auto&& [key, value] : j.items()) {
            auto child = build(value, model);
            TRY_EXPECTED(child);
            object->addField(key, *child);
        }
        return object;
    }

    if (j.is_array()) {
        auto array = model.newArray(j.size());
        for (const auto& value : j) {
            auto child = build(value, model);
            TRY_EXPECTED(child);
            array->append(*child);
        }
        return array;
    }

    return {};
}

auto parse(std::istream& input, ModelPoolPtr const& model) -> tl::expected<void, Error>
{
    auto root = build(json::parse(input), *model);
    if (!root)
        return tl::unexpected<Error>(root.error());
    model->addRoot(*root);
    return model->validate();
}

auto parse(const std::string& input, ModelPoolPtr const& model) -> tl::expected<void, Error>
{
    auto root = build(json::parse(input), *model);
    if (!root)
        return tl::unexpected<Error>(root.error());
    model->addRoot(*root);
    return model->validate();
}

auto parse(const std::string& input) -> tl::expected<ModelPoolPtr, Error>
{
    auto model = std::make_shared<simfil::ModelPool>();
    auto root = build(json::parse(input), *model);
    if (!root)
        return tl::unexpected<Error>(root.error());
    model->addRoot(*root);
    if (auto res = model->validate(); !res)
        return tl::unexpected<Error>(res.error());
    return model;
}

}
