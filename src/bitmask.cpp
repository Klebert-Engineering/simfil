#include "simfil/operator.h"
#include "simfil/model/schema.h"

#include <limits>
#include <charconv>

namespace simfil
{

auto Bitmask::parseBits(std::string_view text) -> tl::expected<std::uint64_t, Error>
{
    auto base = 10;
    if (text.starts_with("0x") || text.starts_with("0X")) {
        text.remove_prefix(2);
        base = 16;
    }
    if (text.empty())
        return tl::unexpected(Error{Error::InvalidArguments, "Expected unsigned 64-bit decimal or hexadecimal bits"});
    std::uint64_t bits = 0;
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), bits, base);
    if (error != std::errc{} || end != text.data() + text.size())
        return tl::unexpected(Error{Error::InvalidArguments, "Expected unsigned 64-bit decimal or hexadecimal bits"});
    return bits;
}

auto Bitmask::hasFlag(std::string_view name) const -> tl::expected<bool, Error>
{
    auto schema = domain.definition();
    auto flag = schema ? schema->namedValue(name) : std::nullopt;
    if (!flag || flag->role != Schema::NamedValue::Role::Flag)
        return tl::unexpected(Error{Error::InvalidArguments, "Unknown flag in bitmask domain: " + std::string(name)});
    std::optional<std::uint64_t> mask;
    if (auto value = std::get_if<std::uint64_t>(&flag->value))
        mask = *value;
    else if (auto value = std::get_if<std::int64_t>(&flag->value); value && *value >= 0)
        mask = static_cast<std::uint64_t>(*value);
    if (!mask || !schema->isBitmask() || !schema->bitWidth() || schema->bitWidth() > 64 ||
        (schema->bitWidth() < 64 && ((*mask | bits) >> schema->bitWidth())))
        return tl::unexpected(Error{Error::InvalidArguments, "Flag or value exceeds its bitmask domain encoding"});
    return *mask ? (bits & *mask) == *mask : bits == 0;
}

}

namespace simfil::impl
{

auto bitmaskUnary(std::string_view operation, Bitmask const& value) -> tl::expected<Value, Error>
{
    if (operation == "typeof")
        return Value::make("bitmask"sv);
    if (operation == "string")
        return Value::make(ValueToString{}(value));
    // Match simfil's established truthiness: even numeric/bitmask zero is truthy.
    if (operation == "?")
        return Value::t();
    if (operation == "not")
        return Value::f();
    auto domain = value.domain.definition();
    if (!domain || !domain->isBitmask() || domain->bitWidth() == 0 || domain->bitWidth() > 64)
        return tl::unexpected(Error{Error::InvalidOperands, "Bitmask has no valid domain"});
    auto width = domain->bitWidth();
    auto mask = width == 64 ? std::numeric_limits<std::uint64_t>::max() : (std::uint64_t{1} << width) - 1;
    if (operation == "~")
        return Value::make(Bitmask{~value.bits & mask, value.domain});
    return tl::unexpected(Error{Error::InvalidOperands, "Unsupported bitmask unary operation"});
}

auto bitmaskBinary(std::string_view operation, Value const& left, Value const& right) -> tl::expected<Value, Error>
{
    if (left.type == ValueType::Undef || right.type == ValueType::Undef)
        return Value::undef();
    // Typed masks still participate in ordinary present/missing comparisons without integer coercion.
    if (left.type == ValueType::Null || right.type == ValueType::Null) {
        if (operation == "=" || operation == "==")
            return Value::f();
        if (operation == "!=")
            return Value::t();
    }
    auto leftMask = left.type == ValueType::Bitmask;
    auto rightMask = right.type == ValueType::Bitmask;
    auto const& typed = leftMask ? left.as<ValueType::Bitmask>() : right.as<ValueType::Bitmask>();
    auto definition = typed.domain.definition();
    if (!definition || !definition->isBitmask() || definition->bitWidth() == 0 || definition->bitWidth() > 64)
        return tl::unexpected(Error{Error::InvalidOperands, "Bitmask has no valid domain"});
    if (leftMask && rightMask && left.as<ValueType::Bitmask>().domain != right.as<ValueType::Bitmask>().domain)
        return tl::unexpected(Error{Error::InvalidOperands, "Incompatible bitmask domains"});

    auto raw = [](Value const& value) -> std::optional<std::uint64_t> {
        if (value.type == ValueType::Bitmask)
            return value.as<ValueType::Bitmask>().bits;
        if (value.type == ValueType::Int && value.as<ValueType::Int>() >= 0)
            return static_cast<std::uint64_t>(value.as<ValueType::Int>());
        return {};
    };
    auto a = raw(left), b = raw(right);
    if (!a || !b)
        return tl::unexpected(Error{Error::InvalidOperands, "Bitmask operands must be masks or nonnegative integers"});
    if (operation == "==" || operation == "=")
        return Value::make(*a == *b);
    if (operation == "!=")
        return Value::make(*a != *b);

    auto width = definition->bitWidth();
    auto widthMask = width == 64 ? std::numeric_limits<std::uint64_t>::max() : (std::uint64_t{1} << width) - 1;
    if ((leftMask && (*a & ~widthMask)) || (rightMask && (*b & ~widthMask)))
        return tl::unexpected(Error{Error::InvalidOperands, "Bitmask exceeds its encoding width"});
    if (operation == "<<" || operation == ">>") {
        if (!leftMask || rightMask || *b >= width)
            return tl::unexpected(Error{Error::InvalidOperands, "Bitmask shift count is outside its encoding width"});
        auto bits = operation == "<<" ? (*a << *b) & widthMask : *a >> *b;
        return Value::make(Bitmask{bits, typed.domain});
    }
    if ((*a & ~widthMask) || (*b & ~widthMask))
        return tl::unexpected(Error{Error::InvalidOperands, "Raw mask operand exceeds the encoding width"});
    if (operation == "&")
        return Value::make(Bitmask{*a & *b, typed.domain});
    if (operation == "|")
        return Value::make(Bitmask{*a | *b, typed.domain});
    if (operation == "^")
        return Value::make(Bitmask{*a ^ *b, typed.domain});
    return tl::unexpected(Error{Error::InvalidOperands, "Unsupported bitmask binary operation"});
}

}
