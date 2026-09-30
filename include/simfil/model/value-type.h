#pragma once

#include "simfil/byte-array.h"
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace simfil
{

/** Runtime value categories, also used for schema structural-affinity bits. */
enum class ValueType
{
    Undef,
    Null,
    Bool,
    Int,
    Float,
    String,
    Bytes,
    TransientObject,
    Object,
    Array,
    LAST_
};

/** One structural-affinity bit; ValueType itself is deliberately not a flags enum. */
constexpr auto valueTypeAffinity(ValueType type) -> std::uint16_t
{
    return type < ValueType::LAST_ ? std::uint16_t(1u << unsigned(type)) : 0;
}

static_assert(unsigned(ValueType::LAST_) <= 16);

/** Scalar payload shared by model nodes and schema enum/constant metadata. */
using ScalarValueType = std::variant<
    std::monostate, bool, int64_t, double, std::string, std::string_view, ByteArray>;

}
