#include "simfil/value.h"
#include "simfil/model/model.h"

namespace simfil
{

ValueNode::ValueNode(Value v) :
    ModelNodeBase(
      std::make_shared<ModelPoolBase>(),
      {ModelPoolBase::VirtualValue, 0})
{
    data_ = std::move(v);
}

ValueNode::ValueNode(ModelNode const& n)
    : ModelNodeBase(n)
{
}

Value ValueNode::value() const {
    return *std::any_cast<Value>(&data_);
}

ValueType ValueNode::type() const {
    return std::any_cast<Value>(&data_)->type;
}

}
