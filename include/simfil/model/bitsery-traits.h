#pragma once

#include <cstdint>
#include <iostream>
#include <bitsery/traits/core/std_defaults.h>
#include <bitsery/bitsery.h>
#include <bitsery/ext/std_map.h>
#include <sfl/segmented_vector.hpp>

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

namespace ext
{

struct ArrayArenaExt
{
    template <typename S, typename ElementType, size_t PageSize, size_t ChunkPageSize, typename Fnc>
    void serialize(S& s, simfil::ArrayArena<ElementType, PageSize, ChunkPageSize> const& arena, Fnc&& fnc) const
    {
        auto numArrays = static_cast<simfil::ArrayIndex>(arena.heads_.size());
        s.value4b(numArrays);
        for (simfil::ArrayIndex i = 0; i < numArrays; ++i) {
            auto size = arena.size(i);
            s.value4b(size);
            for (size_t j = 0; j < size; ++j)
                fnc(s, const_cast<ElementType&>(arena.at(i, j)));
        }
    }

    template <typename S, typename ElementType, size_t PageSize, size_t ChunkPageSize, typename Fnc>
    void deserialize(S& s, simfil::ArrayArena<ElementType, PageSize, ChunkPageSize>& arena, Fnc&& fnc) const
    {
        simfil::ArrayIndex numArrays;
        s.value4b(numArrays);
        for (simfil::ArrayIndex i = 0; i < numArrays; ++i) {
            typename std::decay_t<decltype(arena)>::SizeType size;
            s.value4b(size);
            auto arrayIndex = arena.new_array(size);
            for (size_t j = 0; j < size; ++j)
                fnc(s, arena.emplace_back(arrayIndex));
        }
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
