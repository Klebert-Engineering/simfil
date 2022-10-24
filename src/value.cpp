#include "simfil/value.h"

namespace simfil
{

namespace {

/** Model Node Impl. to wrap a scalar value during execution. */

struct ValueModelNode : public ModelNodeBase {
    Value value_;

    Value value() const override {
        return value_;
    }

    explicit ValueModelNode(Value v) : value_(std::move(v)) {}
};

}

/** Value Impl. */

ModelNodePtr Value::toModelNode() {
    if (node)
        return node;
    return std::make_shared<ValueModelNode>(*this);
}

/** Model Node Base Impl. */

Value ModelNodeBase::value() const
{
    return Value::null();
}

ModelNodeBase::Type ModelNodeBase::type() const
{
    return ModelNode::Scalar;
}

ModelNodePtr ModelNodeBase::get(const StringId &) const
{
    return nullptr;
}

ModelNodePtr ModelNodeBase::at(int64_t) const
{
    return nullptr;
}

std::vector<ModelNodePtr> ModelNodeBase::children() const
{
    return {};
}

std::vector<std::string> ModelNodeBase::keys() const
{
    return {};
}

uint32_t ModelNodeBase::size() const
{
    return 0;
}

}
