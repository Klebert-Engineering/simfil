#pragma once

#include <algorithm>
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

        using CompactHeadsStorage = typename simfil::ArrayArena<ElementType, PageSize, ChunkPageSize>::CompactHeadStorage;
        using DataStorage = typename std::remove_cv_t<std::remove_reference_t<decltype(arena.data_)>>;
        using CompactChunk = typename simfil::ArrayArena<ElementType, PageSize, ChunkPageSize>::CompactArrayChunk;

        // Truncating a compact array preserves the other arrays' offsets. Only
        // densely packed heads may reuse the stored payload without compaction.
        if (arena.is_compact()) {
            size_t end = 0;
            bool dense = true;
            for (auto const& head : *arena.compactHeads_) {
                dense = dense && head.offset == end;
                end += head.size;
            }
            dense = dense && end == arena.data_.size();
            if (dense) {
                s.object(*arena.compactHeads_);
                s.object(arena.data_);
            } else {
                CompactHeadsStorage heads;
                DataStorage data;
                heads.reserve(arena.compactHeads_->size());
                data.resize(end);
                size_t position = 0;
                for (auto const& head : *arena.compactHeads_) {
                    heads.push_back(CompactChunk{static_cast<uint32_t>(position), head.size});
                    for (size_t i = 0; i < head.size; ++i)
                        data[position++] = arena.data_[head.offset + i];
                }
                s.object(heads);
                s.object(data);
            }
            s.object(arena.singletonValues_);
            s.object(arena.singletonOccupied_);
            return;
        }

        // Otherwise: Build compact temporary heads/data, then serialize those buffers.
        CompactHeadsStorage compactHeads;
        DataStorage compactData;
        compactHeads.reserve(arena.heads_.size());

        size_t totalElements = 0;
        for (auto const& head : arena.heads_) {
            totalElements += head.size;
        }
        compactData.resize(totalElements);

        size_t writeIndex = 0;
        size_t packedOffset = 0;
        for (auto const& head : arena.heads_) {
            compactHeads.push_back(CompactChunk{
                static_cast<std::uint32_t>(packedOffset),
                static_cast<std::uint32_t>(head.size)
            });

            auto const* current = &head;
            size_t remaining = head.size;
            while (current != nullptr && remaining > 0) {
                size_t chunkUsed = 0;
                if (current == &head) {
                    chunkUsed = std::min<size_t>(head.capacity, remaining);
                } else {
                    chunkUsed = std::min<size_t>(current->size, remaining);
                }

                for (size_t i = 0; i < chunkUsed; ++i) {
                    compactData[writeIndex++] = arena.data_[current->offset + i];
                }
                remaining -= chunkUsed;
                current = (current->next != simfil::InvalidArrayIndex)
                    ? &arena.continuations_[static_cast<size_t>(current->next)]
                    : nullptr;
            }
            packedOffset += head.size;
        }

        s.object(compactHeads);
        s.object(compactData);
        s.object(arena.singletonValues_);
        s.object(arena.singletonOccupied_);
    }

    template <typename S, typename ElementType, size_t PageSize, size_t ChunkPageSize, typename Fnc>
    void deserialize(S& s, simfil::ArrayArena<ElementType, PageSize, ChunkPageSize>& arena, Fnc&& fnc) const
    {
        (void)fnc;
        using CompactHeadsStorage = typename simfil::ArrayArena<ElementType, PageSize, ChunkPageSize>::CompactHeadStorage;

        CompactHeadsStorage compactHeads;
        s.object(compactHeads);
        s.object(arena.data_);
        s.object(arena.singletonValues_);
        s.object(arena.singletonOccupied_);

        // A compact stream must describe disjoint consecutive slices. Validate
        // before publishing its heads: otherwise malformed offsets reach unchecked
        // column access during ordinary arena traversal.
        size_t end = 0;
        bool valid = compactHeads.size() < simfil::SingletonArrayHandleMask &&
            arena.singletonValues_.size() <= simfil::SingletonArrayHandlePayloadMask &&
            arena.singletonOccupied_.size() == arena.singletonValues_.size();
        for (auto const& head : compactHeads) {
            if (head.offset != end || end > arena.data_.size() ||
                head.size > arena.data_.size() - end) {
                valid = false;
                break;
            }
            end += head.size;
        }
        valid = valid && end == arena.data_.size();
        for (auto occupied : arena.singletonOccupied_)
            valid = valid && occupied <= 1;
        if (!valid) {
            s.adapter().error(ReaderError::InvalidData);
            return;
        }

        arena.heads_.clear();
        arena.continuations_.clear();
        arena.compactHeads_ = std::move(compactHeads);
    }
};

}

namespace traits {
template<typename T>
struct ExtensionTraits<ext::ArrayArenaExt, T>
{
    using TValue = void;
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
