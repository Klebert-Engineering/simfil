#include "simfil/model/model.h"
#include "simfil/value.h"
#include "simfil/model/nodes.h"

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
ModelNode::Ptr ModelNode::get(const FieldId& field) const {
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
FieldId ModelNode::keyAt(int64_t i) const {
    FieldId result = 0;
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
        for (const auto& [fieldId, childNode] : fields()) {
            if (auto resolvedField = model_->lookupFieldId(fieldId)) {
                j[*resolvedField] = childNode->toJson();
            }
        }
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
    return (int64_t)addr_.int16();
}

template<> ValueType SmallValueNode<int16_t>::type() const {
    return ValueType::Int;
}

template<>
SmallValueNode<int16_t>::SmallValueNode(ModelConstPtr p, ModelNodeAddress a)
    : ModelNodeBase(std::move(p), a)
{}

template<> ScalarValueType SmallValueNode<uint16_t>::value() const {
    return (int64_t)addr_.uint16();
}

template<> ValueType SmallValueNode<uint16_t>::type() const {
    return ValueType::Int;
}

template<>
SmallValueNode<uint16_t>::SmallValueNode(ModelConstPtr p, ModelNodeAddress a)
    : ModelNodeBase(std::move(p), a)
{}

template<> ScalarValueType SmallValueNode<bool>::value() const {
    return (bool)addr_.uint16();
}

template<> ValueType SmallValueNode<bool>::type() const {
    return ValueType::Bool;
}

template<>
SmallValueNode<bool>::SmallValueNode(ModelConstPtr p, ModelNodeAddress a)
    : ModelNodeBase(std::move(p), a)
{}

/** Model Node impls for an array. */

Array::Array(ModelConstPtr pool_, ModelNodeAddress a)
    : MandatoryModelPoolNodeBase(std::move(pool_), a), storage_(nullptr), members_((ArrayIndex)a.index())
{
    storage_ = &model().arrayMemberStorage();
}

ValueType Array::type() const
{
    return ValueType::Array;
}

ModelNode::Ptr Array::at(int64_t i) const
{
    if (i < 0 || i >= (int64_t)storage_->size(members_))
        return {};
    return ModelNode::Ptr::make(model_, storage_->at(members_, i));
}

uint32_t Array::size() const
{
    return (uint32_t)storage_->size(members_);
}

Array& Array::append(bool value) {storage_->push_back(members_, model().newSmallValue(value)->addr()); return *this;}
Array& Array::append(uint16_t value) {storage_->push_back(members_, model().newSmallValue(value)->addr()); return *this;}
Array& Array::append(int16_t value) {storage_->push_back(members_, model().newSmallValue(value)->addr()); return *this;}
Array& Array::append(int64_t const& value) {storage_->push_back(members_, model().newValue(value)->addr()); return *this;}
Array& Array::append(double const& value) {storage_->push_back(members_, model().newValue(value)->addr()); return *this;}
Array& Array::append(std::string_view const& value) {storage_->push_back(members_, model().newValue(value)->addr()); return *this;}
Array& Array::append(ModelNode::Ptr const& value) {storage_->push_back(members_, value->addr()); return *this;}

bool Array::iterate(const ModelNode::IterCallback& cb) const
{
    auto cont = true;
    auto resolveAndCb = Model::Lambda([&cb, &cont](auto && node){
        cont = cb(node);
    });
    storage_->iterate(members_, [&, this](auto&& member){
            model_->resolve(*ModelNode::Ptr::make(model_, member), resolveAndCb);
        return cont;
    });
    return cont;
}

Array& Array::extend(shared_model_ptr<Array> const& other) {
    auto otherSize = other->size();
    for (auto i = 0u; i < otherSize; ++i) {
        storage_->push_back(members_, storage_->at(other->members_, i));
    }
    return *this;
}

/** Model Node impls for an object. */

Object::Object(ModelConstPtr pool_, ModelNodeAddress a)
    : MandatoryModelPoolNodeBase(std::move(pool_), a), storage_(nullptr), members_((ArrayIndex)a.index())
{
    storage_ = &model().objectMemberStorage();
}

Object::Object(ArrayIndex members, ModelConstPtr pool_, ModelNodeAddress a)
    : MandatoryModelPoolNodeBase(std::move(pool_), a), storage_(nullptr), members_(members)
{
    storage_ = &model().objectMemberStorage();
}

ValueType Object::type() const
{
    return ValueType::Object;
}

ModelNode::Ptr Object::at(int64_t i) const
{
    if (i < 0 || i >= (int64_t)storage_->size(members_))
        return {};
    return ModelNode::Ptr::make(model_, storage_->at(members_, i).node_);
}

FieldId Object::keyAt(int64_t i) const {
    if (i < 0 || i >= (int64_t)storage_->size(members_))
        return {};
    return storage_->at(members_, i).name_;
}

uint32_t Object::size() const
{
    return (uint32_t)storage_->size(members_);
}

ModelNode::Ptr Object::get(const FieldId & field) const
{
    ModelNode::Ptr result;
    storage_->iterate(members_, [&field, &result, this](auto&& member){
        if (member.name_ == field) {
            result = ModelNode::Ptr::make(model_, member.node_);
            return false;
        }
        return true;
    });
    return result;
}

ModelNode::Ptr Object::get(std::string_view const& fieldName) const {
    auto fieldId = model().fieldNames()->emplace(fieldName);
    return get(fieldId);
}

Object& Object::addBool(std::string_view const& name, bool value) {
    auto fieldId = model().fieldNames()->emplace(name);
    storage_->emplace_back(members_, fieldId, model().newSmallValue(value)->addr());
    return *this;
}

Object& Object::addField(std::string_view const& name, uint16_t value) {
    auto fieldId = model().fieldNames()->emplace(name);
    storage_->emplace_back(members_, fieldId, model().newSmallValue(value)->addr());
    return *this;
}

Object& Object::addField(std::string_view const& name, int16_t value) {
    auto fieldId = model().fieldNames()->emplace(name);
    storage_->emplace_back(members_, fieldId, model().newSmallValue(value)->addr());
    return *this;
}

Object& Object::addField(std::string_view const& name, int64_t const& value) {
    auto fieldId = model().fieldNames()->emplace(name);
    storage_->emplace_back(members_, fieldId, model().newValue(value)->addr());
    return *this;
}

Object& Object::addField(std::string_view const& name, double const& value) {
    auto fieldId = model().fieldNames()->emplace(name);
    storage_->emplace_back(members_, fieldId, model().newValue(value)->addr());
    return *this;
}

Object& Object::addField(std::string_view const& name, std::string_view const& value) {
    auto fieldId = model().fieldNames()->emplace(name);
    storage_->emplace_back(members_, fieldId, model().newValue(value)->addr());
    return *this;
}

Object& Object::addField(std::string_view const& name, ModelNode::Ptr const& value) {
    auto fieldId = model().fieldNames()->emplace(name);
    storage_->emplace_back(members_, fieldId, value->addr());
    return *this;
}

bool Object::iterate(const ModelNode::IterCallback& cb) const
{
    auto cont = true;
    auto resolveAndCb = Model::Lambda([&cb, &cont](auto && node){
        cont = cb(node);
    });
    storage_->iterate(members_, [&, this](auto&& member) {
        model_->resolve(*ModelNode::Ptr::make(model_, member.node_), resolveAndCb);
        return cont;
    });
    return cont;
}

Object& Object::extend(shared_model_ptr<Object> const& other)
{
    auto otherSize = other->size();
    for (auto i = 0u; i < otherSize; ++i) {
        storage_->push_back(members_, storage_->at(other->members_, i));
    }
    return *this;
}

}
