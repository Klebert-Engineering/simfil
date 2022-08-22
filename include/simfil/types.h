// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#include "value.h"
#include "typed-meta-type.h"

namespace simfil
{

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

class IRangeType : public TypedMetaType<IRange>
{
public:
    static IRangeType Type;

    IRangeType();

    auto make(int64_t a, int64_t b) -> Value;

    auto unaryOp(std::string_view op, const IRange& self) const -> Value override;
    auto binaryOp(std::string_view op, const IRange& l, const Value& r) const -> Value override;
    auto binaryOp(std::string_view op, const Value& l, const IRange& r) const -> Value override;

    auto unpack(const IRange& , std::function<bool(Value)> res) const -> void override;
};

}
