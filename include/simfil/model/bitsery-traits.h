#pragma once

#include <csignal>
#include <cstdint>
#include <type_traits>
#include <bitsery/traits/core/std_defaults.h>
#include <bitsery/bitsery.h>
#include <bitsery/ext/std_map.h>
#include <sfl/segmented_vector.hpp>

#include "bitsery/details/adapter_common.h"
#include "nodes.h"
#include "arena.h"

namespace bitsery
{

namespace traits
{
template <typename T, std::size_t N, typename Allocator>
struct ContainerTraits<sfl::segmented_vector<T, N, Allocator>>
    : public StdContainer<sfl::segmented_vector<T, N, Allocator>, true, true>
{
};
}

template <typename S>
void serialize(S& s, simfil::ArrayIndex& v)
{
    s.value4b(v);
}

template <typename S>
void serialize(S& s, int64_t& v)
{
    s.value8b(v);
}

template <typename S>
void serialize(S& s, double& v)
{
    s.value8b(v);
}

template <typename S>
void serialize(S& s, simfil::StringId& v)
{
    s.value2b(v);
}

namespace ext
{

struct ArrayArenaExt
{
    template <typename S, typename ElementType, size_t PageSize, size_t ChunkPageSize, typename Fnc>
    void serialize(S& s, simfil::ArrayArena<ElementType, PageSize, ChunkPageSize> const& arena, Fnc&& fnc) const
    {
        (void)fnc;

        // If the arena is already compact, we can simply dump out heads and data
        if (arena.isCompact()) {
            s.object(arena.heads_);
            s.object(arena.data_);
            return;
        }

        // Otherwise: Build compact temporary heads/data, then serialize those buffers.
        using HeadsStorage = typename std::remove_cv_t<std::remove_reference_t<decltype(arena.heads_)>>;
        using DataStorage = typename std::remove_cv_t<std::remove_reference_t<decltype(arena.data_)>>;
        using Chunk = typename simfil::ArrayArena<ElementType, PageSize, ChunkPageSize>::Chunk;
        using SizeType = typename simfil::ArrayArena<ElementType, PageSize, ChunkPageSize>::SizeType;

        HeadsStorage compactHeads;
        DataStorage compactData;
        compactHeads.reserve(arena.heads_.size());

        size_t totalElements = 0;
        for (auto const& head : arena.heads_) {
            totalElements += static_cast<size_t>(head.size);
        }
        compactData.resize(totalElements);

        size_t writeIndex = 0;
        size_t packedOffset = 0;
        for (auto const& head : arena.heads_) {
            compactHeads.push_back(Chunk{
                static_cast<SizeType>(packedOffset),
                head.size,
                head.size,
                simfil::InvalidArrayIndex,
                simfil::InvalidArrayIndex
            });

            auto const* current = &head;
            auto remaining = static_cast<size_t>(head.size);
            while (current != nullptr && remaining > 0) {
                size_t chunkUsed = 0;
                if (current == &head) {
                    chunkUsed = std::min(static_cast<size_t>(head.capacity), remaining);
                } else {
                    chunkUsed = std::min(static_cast<size_t>(current->size), remaining);
                }

                for (size_t i = 0; i < chunkUsed; ++i) {
                    compactData[writeIndex++] = arena.data_[current->offset + i];
                }
                remaining -= chunkUsed;
                current = (current->next != simfil::InvalidArrayIndex) ? &arena.continuations_[current->next] : nullptr;
            }
            packedOffset += static_cast<size_t>(head.size);
        }

        s.object(compactHeads);
        s.object(compactData);
    }

    template <typename S, typename ElementType, size_t PageSize, size_t ChunkPageSize, typename Fnc>
    void deserialize(S& s, simfil::ArrayArena<ElementType, PageSize, ChunkPageSize>& arena, Fnc&& fnc) const
    {
        s.object(arena.heads_);
        s.object(arena.data_);
        arena.continuations_.clear();
        arena.isCompact_ = true;
    }
};

}

namespace traits {
template<typename T>
struct ExtensionTraits<ext::ArrayArenaExt, T>
{
    using TValue = typename T::ElementType;
    static constexpr bool SupportValueOverload = true;
    static constexpr bool SupportObjectOverload = true;
    static constexpr bool SupportLambdaOverload = true;
};
}

template <typename S, typename ElementType, size_t PageSize, size_t ChunkPageSize>
void serialize(S& s, simfil::ArrayArena<ElementType, PageSize, ChunkPageSize>& arena)
{
    s.ext(arena, ext::ArrayArenaExt{});
}

}
