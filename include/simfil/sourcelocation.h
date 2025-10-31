#pragma once

#include <cstddef>

namespace simfil
{

struct SourceLocation
{
    size_t offset = 0;
    size_t size = 0;

    SourceLocation()
        : offset(0), size(0)
    {}

    SourceLocation(size_t offset, size_t size)
        : offset(offset), size(size)
    {}

    SourceLocation(const SourceLocation&) = default;

    auto operator==(const SourceLocation& o) const -> bool = default;
};

}
