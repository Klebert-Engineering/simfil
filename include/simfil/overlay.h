#pragma once

#include "simfil.h"
#include "value.h"
#include "model/model.h"

namespace simfil
{

/** Node for injecting member fields */
class OverlayNode : public ModelNodeBase
{
public:
    struct OverlayNodeData {
        Context ctx_;
        Value value_;
        ModelNode::Ptr base_;
        std::map<FieldId, ModelNode::Ptr> overlayChildren_;
    };

    // Held inside the ModelNode::data member.
    OverlayNodeData* dataPtr_;

    OverlayNode(Context ctx, Value const& val)
        : ModelNodeBase(std::make_shared<ModelPoolBase>(), {ModelPool::VirtualOverlay, 0})
    {
        data_ = OverlayNodeData{ctx, val, val.node};
        dataPtr_ = std::any_cast<OverlayNodeData>(&data_);
    }

    OverlayNode(ModelNode const& n) : ModelNodeBase(n) {
        dataPtr_ = std::any_cast<OverlayNodeData>(&data_);
    }

    auto add(FieldId const& key, ModelNode::Ptr const& child) -> void
    {
        dataPtr_->overlayChildren_[key] = child;
    }

    Value value() const override
    {
        return dataPtr_->base_->value();
    }

    ValueType type() const override
    {
        return dataPtr_->base_->type();
    }

    ModelNode::Ptr get(const FieldId& key) const override
    {
        auto iter = dataPtr_->overlayChildren_.find(key);
        if (iter != dataPtr_->overlayChildren_.end())
            return iter->second;
        return dataPtr_->base_->get(key);
    }

    ModelNode::Ptr at(int64_t i) const override
    {
        return dataPtr_->base_->at(i);
    }

    FieldId keyAt(int64_t i) const override
    {
        return dataPtr_->base_->keyAt(i);
    }

    uint32_t size() const override
    {
        return dataPtr_->base_->size();
    }
};

}
