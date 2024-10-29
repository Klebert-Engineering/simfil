#pragma once
#include "nodes.h"

namespace simfil
{

template <class ModelType, class ModelNodeType>
BaseArray<ModelType, ModelNodeType>::BaseArray(ModelConstPtr pool_, ModelNodeAddress a)
    : MandatoryDerivedModelNodeBase<ModelType>(std::move(pool_), a),
      storage_(nullptr),
      members_((ArrayIndex)a.index())
{
    storage_ = &model().arrayMemberStorage();
}

template <class ModelType, class ModelNodeType>
ValueType BaseArray<ModelType, ModelNodeType>::type() const
{
    return ValueType::Array;
}

template <class ModelType, class ModelNodeType>
ModelNode::Ptr BaseArray<ModelType, ModelNodeType>::at(int64_t i) const
{
    if (i < 0 || i >= (int64_t)storage_->size(members_))
        return {};
    return ModelNode::Ptr::make(model_, storage_->at(members_, i));
}

template <class ModelType, class ModelNodeType>
uint32_t BaseArray<ModelType, ModelNodeType>::size() const
{
    return (uint32_t)storage_->size(members_);
}

template <class ModelType, class ModelNodeType>
bool BaseArray<ModelType, ModelNodeType>::iterate(const ModelNode::IterCallback& cb) const
{
    auto cont = true;
    auto resolveAndCb = Model::Lambda([&cb, &cont](auto&& node) { cont = cb(node); });
    storage_->iterate(
        members_,
        [&, this](auto&& member)
        {
            model_->resolve(*ModelNode::Ptr::make(model_, member), resolveAndCb);
            return cont;
        });
    return cont;
}

template <class ModelType, class ModelNodeType>
bool BaseArray<ModelType, ModelNodeType>::forEach(
    std::function<bool(ModelNodeType const&)> const& callback) const
{
    return iterate(ModelNode::IterLambda(
        [&callback](auto&& node) { return callback(static_cast<ModelNodeType&>(node)); }));
}

template <class ModelType, class ModelNodeType>
BaseArray<ModelType, ModelNodeType>&
BaseArray<ModelType, ModelNodeType>::append(ModelNode::Ptr const& value)
{
    storage_->push_back(members_, value->addr());
    return *this;
}

template <class ModelType, class ModelNodeType>
BaseObject<ModelType, ModelNodeType>::BaseObject(ModelConstPtr pool_, ModelNodeAddress a)
    : MandatoryDerivedModelNodeBase<ModelType>(std::move(pool_), a),
      storage_(nullptr),
      members_((ArrayIndex)a.index())
{
    storage_ = &model().objectMemberStorage();
}

template <class ModelType, class ModelNodeType>
BaseObject<ModelType, ModelNodeType>::BaseObject(
    ArrayIndex members,
    ModelConstPtr pool_,
    ModelNodeAddress a)
    : MandatoryDerivedModelNodeBase<ModelType>(std::move(pool_), a),
      storage_(nullptr),
      members_(members)
{
    storage_ = &model().objectMemberStorage();
}

template <class ModelType, class ModelNodeType>
ValueType BaseObject<ModelType, ModelNodeType>::type() const
{
    return ValueType::Object;
}

template <class ModelType, class ModelNodeType>
ModelNode::Ptr BaseObject<ModelType, ModelNodeType>::at(int64_t i) const
{
    if (i < 0 || i >= (int64_t)storage_->size(members_))
        return {};
    return ModelNode::Ptr::make(model_, storage_->at(members_, i).node_);
}

template <class ModelType, class ModelNodeType>
StringId BaseObject<ModelType, ModelNodeType>::keyAt(int64_t i) const
{
    if (i < 0 || i >= (int64_t)storage_->size(members_))
        return {};
    return storage_->at(members_, i).name_;
}

template <class ModelType, class ModelNodeType>
uint32_t BaseObject<ModelType, ModelNodeType>::size() const
{
    return (uint32_t)storage_->size(members_);
}

template <class ModelType, class ModelNodeType>
ModelNode::Ptr BaseObject<ModelType, ModelNodeType>::get(const StringId& field) const
{
    ModelNode::Ptr result;
    storage_->iterate(
        members_,
        [&field, &result, this](auto&& member)
        {
            if (member.name_ == field) {
                result = ModelNode::Ptr::make(model_, member.node_);
                return false;
            }
            return true;
        });
    return result;
}

template <class ModelType, class ModelNodeType>
bool BaseObject<ModelType, ModelNodeType>::iterate(const ModelNode::IterCallback& cb) const
{
    auto cont = true;
    auto resolveAndCb = Model::Lambda([&cb, &cont](auto&& node) { cont = cb(node); });
    storage_->iterate(
        members_,
        [&, this](auto&& member)
        {
            model_->resolve(*ModelNode::Ptr::make(model_, member.node_), resolveAndCb);
            return cont;
        });
    return cont;
}

template <class ModelType, class ModelNodeType>
BaseObject<ModelType, ModelNodeType>& BaseObject<ModelType, ModelNodeType>::addField(
    std::string_view const& name,
    ModelNode::Ptr const& value)
{
    auto fieldId = model().strings()->emplace(name);
    storage_->emplace_back(members_, fieldId, value->addr());
    return *this;
}

}  // namespace simfil