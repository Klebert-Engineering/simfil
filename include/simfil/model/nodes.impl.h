#pragma once
#include "nodes.h"

namespace simfil
{

template <class ModelType, class ModelNodeType>
BaseArray<ModelType, ModelNodeType>::BaseArray(ModelConstPtr pool_, ModelNodeAddress a)
    : MandatoryDerivedModelNodeBase<ModelType>(std::move(pool_), a), storage_(nullptr), members_((ArrayIndex)a.index())
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
    auto resolveAndCb = Model::Lambda([&cb, &cont](auto && node){
                                          cont = cb(node);
                                      });
    storage_->iterate(members_, [&, this](auto&& member){
                          model_->resolve(*ModelNode::Ptr::make(model_, member), resolveAndCb);
                          return cont;
                      });
    return cont;
}

template <class ModelType, class ModelNodeType>
BaseArray<ModelType, ModelNodeType>& BaseArray<ModelType, ModelNodeType>::append(ModelNode::Ptr const& value)
{
    storage_->push_back(members_, value->addr());
    return *this;
}

}