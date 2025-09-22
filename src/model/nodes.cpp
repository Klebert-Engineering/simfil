#include "simfil/model/model.h"
#include "simfil/model/string-pool.h"
#include "simfil/value.h"
#include "simfil/model/nodes.h"

#include "../expected.h"
#include "tl/expected.hpp"

#include <fmt/format.h>

namespace simfil
{

/// Create a ModelNode from a model pool which serves as its
/// VFT, and a TreeNodeAddress.
ModelNode::ModelNode(ModelConstPtr pool, ModelNodeAddress addr, ScalarValueType data)
    : model_(std::move(pool)), addr_(addr), data_(std::move(data))
{}

/// Get the node's scalar value if it has one
ScalarValueType ModelNode::value() const {
    ScalarValueType result;
    if (model_)
        model_->resolve(*this, Model::Lambda([&](auto&& resolved) { result = resolved.value(); }));
    return result;
}

/// Get the node's abstract model type
ValueType ModelNode::type() const {
    ValueType result = ValueType::Null;
    if (model_)
        model_->resolve(*this, Model::Lambda([&](auto&& resolved) { result = resolved.type(); }));
    return result;
}

/// Get a child by name
ModelNode::Ptr ModelNode::get(const StringId& field) const {
    ModelNode::Ptr result;
    if (model_)
        model_
            ->resolve(*this, Model::Lambda([&](auto&& resolved) { result = resolved.get(field); }));
    return result;
}

/// Get a child by index
ModelNode::Ptr ModelNode::at(int64_t index) const {
    ModelNode::Ptr result;
    if (model_)
        model_
            ->resolve(*this, Model::Lambda([&](auto&& resolved) { result = resolved.at(index); }));
    return result;
}

/// Get an Object model's field names
StringId ModelNode::keyAt(int64_t i) const {
    StringId result = 0;
    if (model_)
        model_->resolve(*this, Model::Lambda([&](auto&& resolved) { result = resolved.keyAt(i); }));
    return result;
}

/// Get the number of children
uint32_t ModelNode::size() const {
    uint32_t result = 0;
    if (model_)
        model_->resolve(*this, Model::Lambda([&](auto&& resolved) { result = resolved.size(); }));
    return result;
}

/// Fast iteration
bool ModelNode::iterate(const IterCallback& cb) const {
    bool result = true;
    if (model_)
        model_->resolve(*this, Model::Lambda([&](auto&& resolved) { result = resolved.iterate(cb); }));
    return result;
}

#if defined(SIMFIL_WITH_MODEL_JSON)
nlohmann::json ModelNode::toJson() const
{
    if (type() == ValueType::Object) {
        auto j = nlohmann::json::object();
        auto isMultiMap = false;
        for (const auto& [fieldId, childNode] : fields()) {
            if (auto resolvedField = model_->lookupStringId(fieldId)) {
                // As soon as we find the first duplicate key,
                // change all existing values to arrays.
                if (!isMultiMap && j.contains(*resolvedField)) {
                    isMultiMap = true;
                    for (auto&& [key, value] : j.items()) {
                        j[std::move(key)] = nlohmann::json::array({std::move(value)});
                    }
                }

                if (isMultiMap) {
                    if (!j.contains(*resolvedField))
                        j[*resolvedField] = nlohmann::json::array({childNode->toJson()});
                    else
                        j[*resolvedField].push_back(childNode->toJson());
                } else {
                    j[*resolvedField] = childNode->toJson();
                }
            }
        }

        if (isMultiMap)
            j["_multimap"] = true;
        return j;
    }
    else if (type() == ValueType::Array) {
        auto j = nlohmann::json::array();
        for (const auto& i : *this) {
            j.push_back(i->toJson());
        }
        return j;
    }
    else {
        auto j = nlohmann::json{};
        std::visit(
            [&j](auto&& v)
            {
                using T = decltype(v);
                if constexpr (!std::is_same_v<std::decay_t<T>, std::monostate>) {
                    j = std::forward<T>(v);
                } else {
                    j = nullptr;
                }
            }, value());
        return j;
    }
}
#endif

/** Model Node Base Impl. */

ModelNodeBase::ModelNodeBase(ModelConstPtr pool, ModelNodeAddress addr, ScalarValueType data)
    : ModelNode(std::move(pool), addr, std::move(data))
{
}

ModelNodeBase::ModelNodeBase(const ModelNode& n)
    : ModelNode(n)
{
}

ScalarValueType ModelNodeBase::value() const
{
    return data_;
}

ValueType ModelNodeBase::type() const
{
    return ValueType::Null;
}

ModelNode::Ptr ModelNodeBase::get(const StringId&) const
{
    return nullptr;
}

ModelNode::Ptr ModelNodeBase::at(int64_t) const
{
    return nullptr;
}

StringId ModelNodeBase::keyAt(int64_t) const
{
    return 0;
}

uint32_t ModelNodeBase::size() const
{
    return 0;
}

/** Model Node impls. for arbitrary self-contained value storage. */

ValueNode::ValueNode(ScalarValueType const& value)
    : ModelNodeBase(std::make_shared<Model>(), Model::Scalar, value)
{}

ValueNode::ValueNode(const ScalarValueType& value, const ModelConstPtr& p)
    : ModelNodeBase(p, Model::Scalar, value)
{}

ValueNode::ValueNode(ModelNode const& n) : ModelNodeBase(n) {}

ValueType ValueNode::type() const {
    ValueType result;
    std::visit([&result](auto&& v){
        result = ValueType4CType<std::decay_t<decltype(v)>>::Type;
    }, data_);
    return result;
}

/** Model Node impls. for SmallValueNode */

template<> ScalarValueType SmallValueNode<int16_t>::value() const {
    return static_cast<int64_t>(addr_.int16());
}

template<> ValueType SmallValueNode<int16_t>::type() const {
    return ValueType::Int;
}

template<>
SmallValueNode<int16_t>::SmallValueNode(ModelConstPtr p, ModelNodeAddress a)
    : ModelNodeBase(std::move(p), a)
{}

template<> ScalarValueType SmallValueNode<uint16_t>::value() const {
    return static_cast<int64_t>(addr_.uint16());
}

template<> ValueType SmallValueNode<uint16_t>::type() const {
    return ValueType::Int;
}

template<>
SmallValueNode<uint16_t>::SmallValueNode(ModelConstPtr p, ModelNodeAddress a)
    : ModelNodeBase(std::move(p), a)
{}

template<> ScalarValueType SmallValueNode<bool>::value() const {
    return addr_.uint16() != 0;
}

template<> ValueType SmallValueNode<bool>::type() const {
    return ValueType::Bool;
}

template<>
SmallValueNode<bool>::SmallValueNode(ModelConstPtr p, ModelNodeAddress a)
    : ModelNodeBase(std::move(p), a)
{}

/** Model Node impls for an array. */

Array& Array::append(bool value) {storage_->push_back(members_, model().newSmallValue(value)->addr()); return *this;}
Array& Array::append(uint16_t value) {storage_->push_back(members_, model().newSmallValue(value)->addr()); return *this;}
Array& Array::append(int16_t value) {storage_->push_back(members_, model().newSmallValue(value)->addr()); return *this;}
Array& Array::append(int64_t const& value) {storage_->push_back(members_, model().newValue(value)->addr()); return *this;}
Array& Array::append(double const& value) {storage_->push_back(members_, model().newValue(value)->addr()); return *this;}
Array& Array::append(std::string_view const& value) {storage_->push_back(members_, model().newValue(value)->addr()); return *this;}

tl::expected<void, Error> Array::extend(model_ptr<Array> const& other) {
    auto otherSize = other->size();
    for (auto i = 0u; i < otherSize; ++i) {
        if (auto value = storage_->at(other->members_, i))
            storage_->push_back(members_, *value);
        else
            return tl::unexpected<Error>(std::move(value.error()));
    }
    return {};
}

/** Model Node impls for an object. */

tl::expected<ModelNode::Ptr, Error> Object::get(std::string_view const& fieldName) const {
    auto fieldId = model().strings()->emplace(fieldName);
    TRY_EXPECTED(fieldId);
    auto field = get(*fieldId);
    if (!field)
        return tl::unexpected<Error>(Error::FieldNotFound, fmt::format("No such field {}", fieldName));
    return field;
}

tl::expected<void, Error> Object::addBool(std::string_view const& name, bool value) {
    auto fieldId = model().strings()->emplace(name);
    TRY_EXPECTED(fieldId);
    storage_->emplace_back(members_, *fieldId, model().newSmallValue(value)->addr());
    return {};
}

tl::expected<void, Error> Object::addField(std::string_view const& name, uint16_t value) {
    auto fieldId = model().strings()->emplace(name);
    TRY_EXPECTED(fieldId);
    storage_->emplace_back(members_, *fieldId, model().newSmallValue(value)->addr());
    return {};
}

tl::expected<void, Error> Object::addField(std::string_view const& name, int16_t value) {
    auto fieldId = model().strings()->emplace(name);
    TRY_EXPECTED(fieldId);
    storage_->emplace_back(members_, *fieldId, model().newSmallValue(value)->addr());
    return {};
}

tl::expected<void, Error> Object::addField(std::string_view const& name, int64_t const& value) {
    auto fieldId = model().strings()->emplace(name);
    TRY_EXPECTED(fieldId);
    storage_->emplace_back(members_, *fieldId, model().newValue(value)->addr());
    return {};
}

tl::expected<void, Error> Object::addField(std::string_view const& name, double const& value) {
    auto fieldId = model().strings()->emplace(name);
    TRY_EXPECTED(fieldId);
    storage_->emplace_back(members_, *fieldId, model().newValue(value)->addr());
    return {};
}

tl::expected<void, Error> Object::addField(std::string_view const& name, std::string_view const& value) {
    auto fieldId = model().strings()->emplace(name);
    TRY_EXPECTED(fieldId);
    storage_->emplace_back(members_, *fieldId, model().newValue(value)->addr());
    return {};
}

tl::expected<void, Error> Object::extend(model_ptr<Object> const& other)
{
    auto otherSize = other->size();
    for (auto i = 0u; i < otherSize; ++i) {
        if (auto value = storage_->at(other->members_, i))
            storage_->push_back(members_, *value);
        else
            return tl::unexpected<Error>(std::move(value.error()));
    }
    return {};
}

}
