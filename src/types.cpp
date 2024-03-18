#include "simfil/types.h"

#include "simfil/operator.h"
#include "fmt/core.h"

namespace simfil
{

IRangeType IRangeType::Type;
IRangeType::IRangeType()
    : TypedMetaType("irange")
{}

auto IRangeType::make(int64_t a, int64_t b) -> Value
{
    auto obj = TransientObject(&IRangeType::Type);
    auto range = get(obj);
    range->begin = a;
    range->end = b;

    return Value(ValueType::TransientObject, std::move(obj));
}

auto IRangeType::unaryOp(std::string_view op, const IRange& self) const -> Value
{
    if (op == OperatorTypeof::name())
        return Value::make(ident);

    if (op == OperatorBool::name())
        return Value::t();

    if (op == OperatorAsString::name())
        return Value::make(fmt::format("{}..{}", self.begin, self.end));

    if (op == OperatorLen::name())
        return Value::make((int64_t)self.high() - self.low());

    raise<InvalidOperandsError>(op);
}

auto IRangeType::binaryOp(std::string_view op, const IRange& l, const Value& r) const -> Value
{
    /* Range ==/!= operator checks if the other operand is _in_ the range (for int/float) */
    if (op == OperatorNeq::name()) {
        auto res = binaryOp(OperatorEq::name(), l, r);
        if (res.isa(ValueType::Bool))
            return Value::make(!res.as<ValueType::Bool>());
        assert(0);
    }

    if (op == OperatorEq::name()) {
        auto inRange = [&](auto v) {
            return Value::make(l.low() <= v && v <= l.high());
        };
        if (r.isa(ValueType::Null))
            return Value::f();
        if (r.isa(ValueType::Int))
            return inRange(r.as<ValueType::Int>());
        if (r.isa(ValueType::Float))
            return inRange(r.as<ValueType::Float>());
        if (auto o = getObject<IRange>(r, &IRangeType::Type))
            return Value::make(l.begin == o->begin && l.end == o->end);
    }

    raise<InvalidOperandsError>(op);
}

auto IRangeType::binaryOp(std::string_view op, const Value& l, const IRange& r) const -> Value
{
    if (op == OperatorEq::name() || op == OperatorNeq::name())
        return binaryOp(op, r, l);

    raise<InvalidOperandsError>(op);
}

auto IRangeType::unpack(const IRange& self, std::function<bool(Value)> res) const -> void
{
    auto begin = self.begin;
    auto end = self.end;

    auto step = 1;
    if (begin > end)
        step = -1;
    auto i = begin - step;
    do {
        i += step;
        if (!res(Value(ValueType::Int, static_cast<int64_t>(i))))
            return;
    } while (i != end);
}

}
