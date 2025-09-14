#include "simfil/types.h"

#include "simfil/model/nodes.h"
#include "simfil/operator.h"
#include "fmt/format.h"

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

    return {ValueType::TransientObject, std::move(obj)};
}

auto IRangeType::unaryOp(std::string_view op, const IRange& self) const -> tl::expected<Value, Error>
{
    if (op == OperatorTypeof::name())
        return Value::make(ident);

    if (op == OperatorBool::name())
        return Value::t();

    if (op == OperatorAsString::name())
        return Value::make(fmt::format("{}..{}", self.begin, self.end));

    if (op == OperatorLen::name())
        return Value::make(static_cast<int64_t>(self.high() - self.low()));

    return tl::unexpected<Error>(Error::InvalidOperands, fmt::format("Invalid operands for operator '{}'", op));
}

auto IRangeType::binaryOp(std::string_view op, const IRange& l, const Value& r) const -> tl::expected<Value, Error>
{
    /* Range ==/!= operator checks if the other operand is _in_ the range (for int/float) */
    if (op == OperatorNeq::name()) {
        auto res = binaryOp(OperatorEq::name(), l, r);
        if (res->isa(ValueType::Bool))
            return Value::make(!res->template as<ValueType::Bool>());
        assert(0);
    }

    if (op == OperatorEq::name()) {
        auto inRange = [&](auto v) {
            return Value::make(l.low() <= v && v <= l.high());
        };
        if (r.isa(ValueType::Int))
            return inRange(r.as<ValueType::Int>());
        if (r.isa(ValueType::Float))
            return inRange(r.as<ValueType::Float>());
        if (auto o = getObject<IRange>(r, &IRangeType::Type))
            return Value::make(l.begin == o->begin && l.end == o->end);
        return Value::f();
    }

    return tl::unexpected<Error>(Error::InvalidOperands, fmt::format("Invalid operands for operator '{}'", op));
}

auto IRangeType::binaryOp(std::string_view op, const Value& l, const IRange& r) const -> tl::expected<Value, Error>
{
    if (op == OperatorEq::name() || op == OperatorNeq::name())
        return binaryOp(op, r, l);

    return tl::unexpected<Error>(Error::InvalidOperands, fmt::format("Invalid operands for operator '{}'", op));
}

auto IRangeType::unpack(const IRange& self, std::function<bool(Value)> res) const -> tl::expected<void, Error>
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
            return {};
    } while (i != end);
    return {};
}

ReType ReType::Type;
ReType::ReType()
    : TypedMetaType("re")
{}

auto ReType::make(std::string_view expr) -> Value
{
    auto obj = TransientObject(&ReType::Type);
    auto re = get(obj);
    re->str = expr;
    re->re = std::regex(std::string(expr));

    return Value(ValueType::TransientObject, std::move(obj));
}

auto ReType::unaryOp(std::string_view op, const Re& self) const -> tl::expected<Value, Error>
{
    if (op == OperatorTypeof::name())
        return Value::make(ident);

    if (op == OperatorBool::name())
        return Value::t();

    if (op == OperatorAsString::name())
        return Value::make(self.str);

    return tl::unexpected<Error>(Error::InvalidOperands, fmt::format("Invalid operands for operator '{}'", op));
}

auto ReType::binaryOp(std::string_view op, const Re& l, const Value& r) const -> tl::expected<Value, Error>
{
    return binaryOp(op, r, l);
}

auto ReType::binaryOp(std::string_view op, const Value& l, const Re& r) const -> tl::expected<Value, Error>
{
    if (l.isa(ValueType::Null))
        return Value::null();

    if (l.isa(ValueType::String)) {
        const auto& str = l.as<ValueType::String>();

        if (op == OperatorEq::name())
            return std::regex_match(str, r.re) ? Value::make(str) : Value::f();

        if (op == OperatorNeq::name())
            return std::regex_match(str, r.re) ? Value::f() : Value::make(str);
    }

    // Just ignore incompatible types
    if (op == OperatorEq::name())
        return Value::f();
    if (op == OperatorNeq::name())
        return Value::t();

    return tl::unexpected<Error>(Error::InvalidOperands, fmt::format("Invalid operands for operator '{}'", op));
}

}
