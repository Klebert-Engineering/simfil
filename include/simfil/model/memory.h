// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#include <cstddef>

namespace simfil
{

/**
 * Capacity-oriented memory measurement for one owned storage component.
 *
 * `logicalBytes` describes live payload, while `allocatedBytes` describes the
 * retained backing capacity. Container objects and allocator bookkeeping are
 * intentionally excluded, so allocated bytes form a stable lower bound rather
 * than pretending to equal process-resident memory.
 */
struct MemoryUsage
{
    std::size_t logicalBytes = 0;
    std::size_t allocatedBytes = 0;

    /** Add another independently owned storage component. */
    MemoryUsage& operator+=(MemoryUsage const& other)
    {
        logicalBytes += other.logicalBytes;
        allocatedBytes += other.allocatedBytes;
        return *this;
    }
};

/** Combine two independently owned storage components. */
inline MemoryUsage operator+(MemoryUsage left, MemoryUsage const& right)
{
    left += right;
    return left;
}

} // namespace simfil
