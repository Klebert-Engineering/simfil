#pragma once

#include <cstdint>

namespace simfil
{

struct SourceLocation
{
    std::uint32_t offset = 0u;
    std::uint32_t size = 0u;

    SourceLocation() = default;

    SourceLocation(std::uint32_t offset, std::uint32_t size)
        : offset(offset), size(size)
    {}

    SourceLocation(const SourceLocation&) = default;

    auto operator==(const SourceLocation& o) const -> bool = default;
    auto operator!=(const SourceLocation& o) const -> bool = default;
};

}
