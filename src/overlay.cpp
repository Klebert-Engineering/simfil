#include "simfil/overlay.h"

namespace simfil
{

std::optional<std::string_view> OverlayNodeStorage::lookupStringId(StringId id) const
{
    auto node = value_.node();
    auto owner = node ? node->owningModel() : nullptr;
    return owner ? owner->lookupStringId(id) : std::nullopt;
}

tl::expected<void, Error> OverlayNodeStorage::resolve(ModelNode const& n, ResolveFn const& cb) const
{
    if (n.addr().column() < Model::FirstNontrivialColumnId)
        return Model::resolve(n, cb);
    auto node = model_ptr<OverlayNode>::make(n);
    cb(*node);
    return {};
}

auto OverlayNodeStorage::schemaReference(ModelNode const& node) const -> SchemaReference
{
    if (node.addr().column() != Model::Scalar)
        return value_.schemaReference();
    auto found = overlayChildren_.find(static_cast<StringId>(node.addr().index()));
    return found == overlayChildren_.end() ? SchemaReference{} : found->second.schemaReference();
}

OverlayNode::OverlayNode(Value const& val, detail::mp_key key)
    : MandatoryDerivedModelNodeBase<OverlayNodeStorage>(
          std::make_shared<OverlayNodeStorage>(val),
          {ModelPool::Objects, 0},
          key)
{}

OverlayNode::OverlayNode(ModelNode const& n, detail::mp_key key)
    : MandatoryDerivedModelNodeBase<OverlayNodeStorage>(n, key)
{}

auto OverlayNode::set(StringId const& key, Value const& child) -> void
{
    model().overlayChildren_.insert_or_assign(key, child);
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
        if (iter->second.nodePtr())
            return *iter->second.nodePtr();
        if (iter->second.type == ValueType::Undef)
            return ModelNode::Ptr::make(model_, ModelNodeAddress{Model::Undefined});
        return ModelNode::Ptr::make(model_, ModelNodeAddress{Model::Scalar, key}, iter->second.getScalar());
    }
    auto wrapped = model().value_.node();
    return wrapped ? wrapped->get(key) : ModelNode::Ptr{};
}

[[nodiscard]] ModelNode::Ptr OverlayNode::at(int64_t i) const
{
    auto wrapped = model().value_.node();
    return wrapped ? wrapped->at(i) : ModelNode::Ptr{};
}

[[nodiscard]] StringId OverlayNode::keyAt(int64_t i) const
{
    auto wrapped = model().value_.node();
    return wrapped ? wrapped->keyAt(i) : StringPool::Empty;
}

[[nodiscard]] uint32_t OverlayNode::size() const
{
    auto wrapped = model().value_.node();
    return wrapped ? wrapped->size() : 0;
}

[[nodiscard]] bool OverlayNode::iterate(IterCallback const& cb) const {
    auto wrapped = model().value_.node();
    return !wrapped || wrapped->iterate(cb);
}

}
