#pragma once

#include "simfil/byte-array.h"
#include "simfil/error.h"
#include "simfil/model/schema-type.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <tl/expected.hpp>
#if defined(SIMFIL_WITH_MODEL_JSON)
#include <nlohmann/json_fwd.hpp>
#endif

namespace simfil
{

/** Full-width flags retaining their exact schema pool; persistent rows store bits and a local ID. */
struct Bitmask
{
    std::uint64_t bits = 0;
    SchemaType type;

    Bitmask() = default;
    /** Retain borrowed type provenance at the independent scalar's construction boundary. */
    Bitmask(std::uint64_t bits, SchemaType type);
    /** Exact bits and pool/type equality, never flag membership or artifact-name equality. */
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

/** Runtime scalar payload, including independently owned typed masks. */
using ScalarValueType = std::variant<
    std::monostate, bool, int64_t, double, std::string, std::string_view, ByteArray, Bitmask>;

}
