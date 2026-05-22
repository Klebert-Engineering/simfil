// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#include "simfil/diagnostics.h"

#include <bitsery/bitsery.h>
#include <bitsery/ext/std_bitset.h>
#include <bitsery/traits/string.h>
#include <bitsery/traits/vector.h>

#include <limits>

namespace simfil
{

/**
 * Bitsery adapter for the parsed SIMFIL diagnostics state.
 *
 * The implementation lives in a public header because bitsery instantiates
 * serialization templates at the call site. Keeping it separate from
 * diagnostics.h avoids forcing bitsery includes onto all diagnostics users.
 */
template<typename S>
void serialize(S& s, TypeFlags& flags)
{
    s.ext(flags.flags, bitsery::ext::StdBitset{});
}

/**
 * Serialize parsed diagnostics counters and expression-index mappings.
 *
 * The byte format matches Diagnostics::write/read and can also be embedded
 * directly in larger bitsery payloads such as mapget TileSearchResultLayer.
 */
template<typename S>
void serialize(S& s, Diagnostics& data)
{
    s.container(data.exprIndex_, std::numeric_limits<uint16_t>::max(), [](auto& s2, std::uint32_t& v) {
        s2.value4b(v);
    });
    s.container(data.fieldData_, std::numeric_limits<uint16_t>::max(), [](auto& s2, Diagnostics::FieldExprData& data) {
        s2.value4b(data.location.offset);
        s2.value4b(data.location.size);
        s2.value4b(data.hits);
        s2.value4b(data.evaluations);
        s2.text1b(data.name, 0xff);
    });
    s.container(data.comparisonData_, std::numeric_limits<uint16_t>::max(), [](auto& s2, Diagnostics::ComparisonExprData& data) {
        s2.value4b(data.location.offset);
        s2.value4b(data.location.size);
        s2.object(data.leftTypes);
        s2.object(data.rightTypes);
        s2.value4b(data.evaluations);
        s2.value4b(data.trueResults);
        s2.value4b(data.falseResults);
    });
}

}  // namespace simfil
