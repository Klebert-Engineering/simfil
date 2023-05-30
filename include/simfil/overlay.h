#pragma once

#include "simfil.h"
#include "value.h"
#include "model/model.h"

namespace simfil
{

struct OverlayNodeStorage final : public Model
{
    Value value_;
    std::map<FieldId, Value> overlayChildren_;

    explicit OverlayNodeStorage(Value const& val) : value_(val) {} // NOLINT

    void resolve(ModelNode const& n, ResolveFn const& cb) const override;
};

/** Node for injecting member fields */

class OverlayNode final : public MandatoryDerivedModelNodeBase<OverlayNodeStorage>
{
public:
    explicit OverlayNode(Value const& val);
    explicit OverlayNode(ModelNode const& n);
    auto set(FieldId const& key, Value const& child) -> void;
    [[nodiscard]] ScalarValueType value() const override;
    [[nodiscard]] ValueType type() const override;
    [[nodiscard]] ModelNode::Ptr get(const FieldId& key) const override;
    [[nodiscard]] ModelNode::Ptr at(int64_t i) const override;
    [[nodiscard]] FieldId keyAt(int64_t i) const override;
    [[nodiscard]] uint32_t size() const override;
    [[nodiscard]] bool iterate(IterCallback const& cb) const override;
};

}
