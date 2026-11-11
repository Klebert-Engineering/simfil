#pragma once

#include "value.h"
#include "model/model.h"
#include <map>

namespace simfil
{

/** Own a wrapped value and lookup overrides, retaining each value's model/schema owners. */
struct OverlayNodeStorage final : public Model
{
    Value value_;
    std::map<StringId, Value> overlayChildren_;

    explicit OverlayNodeStorage(Value const& val) : value_(val) {} // NOLINT

    tl::expected<void, Error> resolve(ModelNode const& n, ResolveFn const& cb) const override;

    /** Overlay keys share the wrapped model's namespace, including native subtree copies. */
    std::optional<std::string_view> lookupStringId(StringId id) const override;

    /** Return the exact Value's provenance for root or overlay occurrences, without a side table. */
    auto resolveSchemaType(ModelNode const& node) const -> SchemaType override;
};

/**
 * Add lookup-only members to a value without changing its enumerable children.
 * get() prefers injected values, while at(), keyAt(), size() and iterate() still
 * describe the wrapped node. For example, injecting a context field makes it
 * available by name without introducing another recursive-wildcard result.
 */
class OverlayNode final : public MandatoryDerivedModelNodeBase<OverlayNodeStorage>
{
public:
    explicit OverlayNode(Value const& val, detail::mp_key key);
    explicit OverlayNode(ModelNode const& n, detail::mp_key key);

    /** Copy/replace one override using the wrapped model's StringId namespace. */
    auto set(StringId const& key, Value const& child) -> void;

    [[nodiscard]] ScalarValueType value() const override;
    [[nodiscard]] ValueType type() const override;
    [[nodiscard]] ModelNode::Ptr get(const StringId& key) const override;
    [[nodiscard]] ModelNode::Ptr at(int64_t i) const override;
    [[nodiscard]] StringId keyAt(int64_t i) const override;
    [[nodiscard]] uint32_t size() const override;
    [[nodiscard]] bool iterate(IterCallback const& cb) const override;
};

}
