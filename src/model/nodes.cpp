#include "simfil/model/model.h"
#include "simfil/value.h"

namespace simfil
{

/// Create a ModelNode from a model pool which serves as its
/// VFT, and a TreeNodeAddress.
ModelNode::ModelNode(ModelPoolConstBasePtr pool, ModelNodeAddress addr)
    : pool_(std::move(pool)), addr_(addr)
{}

/// Get the node's scalar value if it has one
Value ModelNode::value() const {
    Value result{ValueType::Null};
    if (pool_)
        pool_->resolve(*this, [&](auto&& resolved) { result = resolved.value(); });
    return result;
}

/// Get the node's abstract model type
ValueType ModelNode::type() const {
    ValueType result = ValueType::Null;
    if (pool_)
        pool_->resolve(*this, [&](auto&& resolved) { result = resolved.type(); });
    return result;
}

/// Get a child by name
ModelNode::Ptr ModelNode::get(const FieldId& field) const {
    ModelNode::Ptr result;
    if (pool_)
        pool_->resolve(*this, [&](auto&& resolved) { result = resolved.get(field); });
    return result;
}

/// Get a child by index
ModelNode::Ptr ModelNode::at(int64_t index) const {
    ModelNode::Ptr result;
    if (pool_)
        pool_->resolve(*this, [&](auto&& resolved) { result = resolved.at(index); });
    return result;
}

/// Get an Object model's field names
FieldId ModelNode::keyAt(int64_t i) const {
    FieldId result = 0;
    if (pool_)
        pool_->resolve(*this, [&](auto&& resolved) { result = resolved.keyAt(i); });
    return result;
}

/// Get the number of children
uint32_t ModelNode::size() const {
    uint32_t result = 0;
    if (pool_)
        pool_->resolve(*this, [&](auto&& resolved) { result = resolved.size(); });
    return result;
}

/** Model Node Base Impl. */

ModelNodeBase::ModelNodeBase(ModelPoolConstBasePtr pool, ModelNodeAddress addr)
    : ModelNode(std::move(pool), addr)
{
}

ModelNodeBase::ModelNodeBase(const ModelNode& n)
    : ModelNode(n)
{
}

Value ModelNodeBase::value() const
{
    return Value::null();
}

ValueType ModelNodeBase::type() const
{
    return ValueType::Null;
}

ModelNode::Ptr ModelNodeBase::get(const FieldId&) const
{
    return nullptr;
}

ModelNode::Ptr ModelNodeBase::at(int64_t) const
{
    return nullptr;
}

FieldId ModelNodeBase::keyAt(int64_t) const
{
    return 0;
}

uint32_t ModelNodeBase::size() const
{
    return 0;
}

/** Model Node impls. for SmallScalarNode */

template<> Value SmallScalarNode<int16_t>::value() const {
    return Value::make((int64_t)addr_.int16());
}

template<> ValueType SmallScalarNode<int16_t>::type() const {
    return ValueType::Int;
}

template<> SmallScalarNode<int16_t>::SmallScalarNode(ModelPoolConstBasePtr p, ModelNodeAddress a)
    : ModelNodeBase(std::move(p), a)
{}

template<> Value SmallScalarNode<uint16_t>::value() const {
    return Value::make((int64_t)addr_.uint16());
}

template<> ValueType SmallScalarNode<uint16_t>::type() const {
    return ValueType::Int;
}

template<> SmallScalarNode<uint16_t>::SmallScalarNode(ModelPoolConstBasePtr p, ModelNodeAddress a)
    : ModelNodeBase(std::move(p), a)
{}

template<> Value SmallScalarNode<bool>::value() const {
    return Value::make((bool)addr_.uint16());
}

template<> ValueType SmallScalarNode<bool>::type() const {
    return ValueType::Bool;
}

template<> SmallScalarNode<bool>::SmallScalarNode(ModelPoolConstBasePtr p, ModelNodeAddress a)
    : ModelNodeBase(std::move(p), a)
{}

/** ModelNode impls for StringNode */

Value StringNode::value() const {
    return Value::strref(str_);
}

ValueType StringNode::type() const {
    return ValueType::String;
}

StringNode::StringNode(std::string_view s, ModelPoolConstBasePtr storage, ModelNodeAddress a)
    : MandatoryModelPoolNodeBase(std::move(storage), a), str_(s) {}


/** Model Node impls for a scalar value. */

template<> Value ScalarNode<int64_t>::value() const
{
    return Value::make(value_);
}

template<> ValueType ScalarNode<int64_t>::type() const {
    return ValueType::Int;
}

template<> ScalarNode<int64_t>::ScalarNode(int64_t const& value, ModelPoolConstBasePtr storage, ModelNodeAddress a)
    : MandatoryModelPoolNodeBase(std::move(storage), a), value_(value) {}

template<> Value ScalarNode<double>::value() const
{
    return Value::make(value_);
}

template<> ValueType ScalarNode<double>::type() const {
    return ValueType::Float;
}

template<> ScalarNode<double>::ScalarNode(const double& value, ModelPoolConstBasePtr storage, ModelNodeAddress a)
    : MandatoryModelPoolNodeBase(std::move(storage), a), value_(value) {}

/** Model Node impls for an array. */

Array::Array(ArrayIndex i, ModelPoolConstBasePtr pool_, ModelNodeAddress a)
    : MandatoryModelPoolNodeBase(std::move(pool_), a), storage_(nullptr), members_(i)
{
    storage_ = &pool().arrayMemberStorage();
}

ValueType Array::type() const
{
    return ValueType::Array;
}

ModelNode::Ptr Array::at(int64_t i) const
{
    if (i < 0 || i >= storage_->size(members_))
        return {};
    return ModelNode::Ptr::make(pool_, storage_->at(members_, i));
}

uint32_t Array::size() const
{
    return storage_->size(members_);
}

Array& Array::append(bool value) {storage_->push_back(members_, pool().newSmallValue(value)->addr()); return *this;}
Array& Array::append(uint16_t value) {storage_->push_back(members_, pool().newSmallValue(value)->addr()); return *this;}
Array& Array::append(int16_t value) {storage_->push_back(members_, pool().newSmallValue(value)->addr()); return *this;}
Array& Array::append(int64_t const& value) {storage_->push_back(members_, pool().newValue(value)->addr()); return *this;}
Array& Array::append(double const& value) {storage_->push_back(members_, pool().newValue(value)->addr()); return *this;}
Array& Array::append(std::string_view const& value) {storage_->push_back(members_, pool().newValue(value)->addr()); return *this;}
Array& Array::append(ModelNode::Ptr const& value) {storage_->push_back(members_, value->addr()); return *this;}

/** Model Node impls for an object. */

Object::Object(ArrayIndex i, ModelPoolConstBasePtr pool_, ModelNodeAddress a)
    : MandatoryModelPoolNodeBase(std::move(pool_), a), storage_(nullptr), members_(i)
{
    storage_ = &pool().objectMemberStorage();
}

ValueType Object::type() const
{
    return ValueType::Object;
}

ModelNode::Ptr Object::at(int64_t i) const
{
    if (i < 0 || i >= storage_->size(members_))
        return {};
    return ModelNode::Ptr::make(pool_, storage_->at(members_, i).node_);
}

FieldId Object::keyAt(int64_t i) const {
    if (i < 0 || i >= storage_->size(members_))
        return {};
    return storage_->at(members_, i).name_;
}

uint32_t Object::size() const
{
    return storage_->size(members_);
}

ModelNode::Ptr Object::get(const FieldId & field) const
{
    for (auto const& member : storage_->range(members_)) {
        if (member.name_ == field) {
            return ModelNode::Ptr::make(pool_, member.node_);
        }
    }
    return {};
}

ModelNode::Ptr Object::get(std::string_view const& fieldName) const {
    auto fieldId = pool().fieldNames()->emplace(fieldName);
    for (auto&& [field, value] : fields())
        if (field == fieldId)
            return value;
    return {};
}

Object& Object::addField(std::string_view const& name, bool value) {
    auto fieldId = pool().fieldNames()->emplace(name);
    storage_->emplace_back(members_, fieldId, pool().newSmallValue(value)->addr());
    return *this;
}

Object& Object::addField(std::string_view const& name, uint16_t value) {
    auto fieldId = pool().fieldNames()->emplace(name);
    storage_->emplace_back(members_, fieldId, pool().newSmallValue(value)->addr());
    return *this;
}

Object& Object::addField(std::string_view const& name, int16_t value) {
    auto fieldId = pool().fieldNames()->emplace(name);
    storage_->emplace_back(members_, fieldId, pool().newSmallValue(value)->addr());
    return *this;
}

Object& Object::addField(std::string_view const& name, int64_t const& value) {
    auto fieldId = pool().fieldNames()->emplace(name);
    storage_->emplace_back(members_, fieldId, pool().newValue(value)->addr());
    return *this;
}

Object& Object::addField(std::string_view const& name, double const& value) {
    auto fieldId = pool().fieldNames()->emplace(name);
    storage_->emplace_back(members_, fieldId, pool().newValue(value)->addr());
    return *this;
}

Object& Object::addField(std::string_view const& name, std::string_view const& value) {
    auto fieldId = pool().fieldNames()->emplace(name);
    storage_->emplace_back(members_, fieldId, pool().newValue(value)->addr());
    return *this;
}

Object& Object::addField(std::string_view const& name, ModelNode::Ptr const& value) {
    auto fieldId = pool().fieldNames()->emplace(name);
    storage_->emplace_back(members_, fieldId, value->addr());
    return *this;
}

}
