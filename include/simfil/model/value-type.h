#pragma once

#include "simfil/byte-array.h"
#include "simfil/error.h"
#include <tl/expected.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#if defined(SIMFIL_WITH_MODEL_JSON)
#include <nlohmann/json_fwd.hpp>
#endif

namespace simfil
{

class Schema;
class SchemaBinding;
using SchemaId = std::uint16_t;

/** An exact domain identity retaining its immutable graph independently of a source model. */
struct SchemaReference
{
    std::shared_ptr<const SchemaBinding> binding;
    SchemaId id = 0;

    /** Empty references carry no semantic interpretation. */
    explicit operator bool() const noexcept { return binding && id != 0; }
    /** Resolve the definition without transferring ownership to a borrowed view. */
    auto definition() const -> const Schema*;
    /** Equal content identities and local domain ids denote the same domain. */
    auto operator==(SchemaReference const& other) const noexcept -> bool;
};

/** Full-width flag bits with intrinsic domain identity; persistent rows store only bits and an id. */
struct Bitmask
{
    std::uint64_t bits = 0;
    SchemaReference domain;

    /** Exact bits and domain equality, never flag membership. */
    bool operator==(Bitmask const&) const = default;
    /** Parse unsigned decimal/hexadecimal bits with no lossy signed or floating conversion. */
    static auto parseBits(std::string_view text) -> tl::expected<std::uint64_t, Error>;
    /** Resolve a declared flag and test all its bits; a named zero means exactly zero. */
    auto hasFlag(std::string_view name) const -> tl::expected<bool, Error>;
#if defined(SIMFIL_WITH_MODEL_JSON)
    /** Emit the exact, lossless mask tag shared by model and bounded result serializers. */
    auto toJson() const -> nlohmann::json;
#endif
};

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

/** Scalar payload shared by model nodes and schema enum/constant metadata. */
using ScalarValueType = std::variant<
    std::monostate, bool, int64_t, double, std::string, std::string_view, ByteArray, Bitmask>;

}
