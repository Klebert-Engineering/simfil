#include "simfil/overlay.h"

namespace simfil
{

void OverlayNodeStorage::resolve(ModelNode const& n, std::function<void(ModelNode&&)> const& cb) const
{
    cb(OverlayNode(n));
}

OverlayNode::OverlayNode(Value const& val)
    : MandatoryDerivedModelPoolNodeBase<OverlayNodeStorage>(
          std::make_shared<OverlayNodeStorage>(val),
          {ModelPool::Objects, 0})
{}

OverlayNode::OverlayNode(ModelNode const& n)
    : MandatoryDerivedModelPoolNodeBase<OverlayNodeStorage>(n)
{}

auto OverlayNode::set(FieldId const& key, Value const& child) -> void
{
    pool().overlayChildren_.insert({key, child});
}

[[nodiscard]] ScalarValueType OverlayNode::value() const
{
    return pool().value_.getScalar();
}

[[nodiscard]] ValueType OverlayNode::type() const
{
    return ValueType::Object;
}

[[nodiscard]] ModelNode::Ptr OverlayNode::get(const FieldId& key) const
{
    auto iter = pool().overlayChildren_.find(key);
    if (iter != pool().overlayChildren_.end()) {
        if (iter->second.node)
            return iter->second.node;
        return ValueNode(iter->second.getScalar());
    }
    return pool().value_.node->get(key);
}

[[nodiscard]] ModelNode::Ptr OverlayNode::at(int64_t i) const
{
    return pool().value_.node->at(i);
}

[[nodiscard]] FieldId OverlayNode::keyAt(int64_t i) const
{
    return pool().value_.node->keyAt(i);
}

[[nodiscard]] uint32_t OverlayNode::size() const
{
    return pool().value_.node->size();
}

}
