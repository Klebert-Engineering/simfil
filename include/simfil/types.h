// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#include "value.h"
#include "typed-meta-type.h"

#include <string_view>
#include <regex>

namespace simfil
{

/** Inclusive endpoints with direction retained for unpacking; low()/high() support membership tests. */
struct IRange
{
    int64_t begin = 0;
    int64_t end = 0;

    int64_t low() const
    {
        return begin < end ? begin : end;
    }

    int64_t high() const
    {
        return begin >= end ? begin : end;
    }
};

/** Range operators: numeric equality tests membership; unpack emits both endpoints in input order. */
class IRangeType : public TypedMetaType<IRange>
{
public:
    static IRangeType Type;

    IRangeType();

    auto make(int64_t a, int64_t b) -> Value;

    auto unaryOp(std::string_view op, const IRange& self) const -> tl::expected<Value, Error> override;
    auto binaryOp(std::string_view op, const IRange& l, const Value& r) const -> tl::expected<Value, Error> override;
    auto binaryOp(std::string_view op, const Value& l, const IRange& r) const -> tl::expected<Value, Error> override;

    auto unpack(const IRange& , std::function<bool(Value)> res) const -> tl::expected<void, Error> override;
};

/** Retain the original pattern for display alongside its compiled std::regex. */
struct Re
{
    std::string str;
    std::regex re;
};

/**
 * Full-string regex matching. Equality returns the matched input string or false;
 * inequality returns the input string only when it does not match.
 */
class ReType : public TypedMetaType<Re>
{
public:
    static ReType Type;

    ReType();

    auto make(std::string_view expr) -> Value;

    auto unaryOp(std::string_view op, const Re&) const -> tl::expected<Value, Error> override;
    auto binaryOp(std::string_view op, const Re&, const Value&) const -> tl::expected<Value, Error> override;
    auto binaryOp(std::string_view op, const Value&, const Re&) const -> tl::expected<Value, Error> override;
};

}
