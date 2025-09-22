#include "simfil/overlay.h"

namespace simfil
{

tl::expected<void, Error> OverlayNodeStorage::resolve(ModelNode const& n, ResolveFn const& cb) const
{
    cb(OverlayNode(n));
    return {};
}

OverlayNode::OverlayNode(Value const& val)
    : MandatoryDerivedModelNodeBase<OverlayNodeStorage>(
          std::make_shared<OverlayNodeStorage>(val),
          {ModelPool::Objects, 0})
{}

OverlayNode::OverlayNode(ModelNode const& n)
    : MandatoryDerivedModelNodeBase<OverlayNodeStorage>(n)
{}

auto OverlayNode::set(StringId const& key, Value const& child) -> void
{
    model().overlayChildren_.insert({key, child});
}

[[nodiscard]] ScalarValueType OverlayNode::value() const
{
    return model().value_.getScalar();
}

[[nodiscard]] ValueType OverlayNode::type() const
{
    return ValueType::Object;
}

[[nodiscard]] ModelNode::Ptr OverlayNode::get(const StringId& key) const
{
    auto iter = model().overlayChildren_.find(key);
    if (iter != model().overlayChildren_.end()) {
        if (iter->second.node)
            return iter->second.node;
        return ValueNode(iter->second.getScalar());
    }
    return model().value_.node->get(key);
}

[[nodiscard]] ModelNode::Ptr OverlayNode::at(int64_t i) const
{
    return model().value_.node->at(i);
}

[[nodiscard]] StringId OverlayNode::keyAt(int64_t i) const
{
    return model().value_.node->keyAt(i);
}

[[nodiscard]] uint32_t OverlayNode::size() const
{
    return model().value_.node->size();
}

[[nodiscard]] bool OverlayNode::iterate(IterCallback const& cb) const {
    return model().value_.node->iterate(cb);
}

}
