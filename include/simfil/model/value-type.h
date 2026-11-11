#pragma once

#include <cstdint>
#include <limits>

namespace simfil
{

/** Graph-local type identity; a nonzero ID is meaningful only with its schema pool. */
using TypeId = std::uint16_t;
inline constexpr TypeId NoTypeId = TypeId{0};
inline constexpr TypeId MaxTypeId = std::numeric_limits<TypeId>::max();

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
    Bitmask,
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

}
