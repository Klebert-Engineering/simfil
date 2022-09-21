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
        std::vector<ObjectNode::Member> members;
        members.reserve(j.size());
        for (auto&& [key, value] : j.items()) {
            members.emplace_back(key, build(value, model));
        }
        if (!members.empty()) {
            model.objectMembers.insert(model.objectMembers.end(), members.begin(), members.end());
            r.size_ = members.size();
            r.storage_ = &(model.objectMembers);
            r.firstMemberIndex_ = model.objectMembers.size() - r.size_;
        }
        return &r;
    }

    if (j.is_array()) {
        auto& r = model.arrays.emplace_back();
        std::vector<ModelNode*> members;
        for (const auto& value : j) {
            members.emplace_back(build(value, model));
        }
        if (!members.empty()) {
            model.arrayMembers.insert(model.arrayMembers.end(), members.begin(), members.end());
            r.size_ = members.size();
            r.storage_ = &(model.arrayMembers);
            r.firstMemberIndex_ = model.arrayMembers.size() - r.size_;
        }
        return &r;
    }

    return make(Value::null(), model);
}

void parse(std::istream& input, ModelPtr const& model)
{
    model->roots.push_back(build(json::parse(input), *model));
}

void parse(const std::string& input, ModelPtr const& model)
{
    model->roots.push_back(build(json::parse(input), *model));
}

}
