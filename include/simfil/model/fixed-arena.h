#pragma once

#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <sfl/segmented_vector.hpp>
#include <stdexcept>
#include <type_traits>

namespace simfil
{

/**
 * FixedArrayArena is an append-only container that stores
 * multiple sub-arrays in a memory efficient manner.
 *
 * Once created, sub-arrays are immutable in size and accessible
 * via their handle only. The arena itself does not store information
 * about any of the sub-arrays.
 */
template <
    class ElementType_,
    uint8_t IndexBits_ = 24u,
    uint8_t SizeBits_ = 8u,
    size_t PageSize_ = 1024
>
class FixedArrayArena
{
public:
    using ElementType = ElementType_;
    static constexpr uint8_t IndexBits = IndexBits_;
    static constexpr uint8_t SizeBits = SizeBits_;
    static constexpr size_t PageSize = PageSize_;
    static constexpr size_t MaxIndex = (~static_cast<size_t>(0u)) >> (sizeof(size_t) * 8u - IndexBits);
    static constexpr size_t MaxSize  = (~static_cast<size_t>(0u)) >> (sizeof(size_t) * 8u - SizeBits);

    static_assert((IndexBits + SizeBits) <= 64,
        "IndexBits + SizeBits must not exceed 64 bit");

    /* Find the smallest matching integer type to use as handle */
    using HandleValueType =
        std::conditional_t<IndexBits + SizeBits <=  8, uint8_t,
        std::conditional_t<IndexBits + SizeBits <= 16, uint16_t,
        std::conditional_t<IndexBits + SizeBits <= 32, uint32_t, uint64_t>>>;

    static constexpr HandleValueType HandleIndexMask = MaxIndex << SizeBits;
    static constexpr HandleValueType HandleSizeMask = MaxSize;

    /** Handle for a sub-array of the arena. */
    struct Handle
    {
        union {
            struct {
                const HandleValueType size : SizeBits;
                const HandleValueType index : IndexBits;
            };
            HandleValueType value;
        };

        Handle(HandleValueType index, HandleValueType size)
            : index(index), size(size)
        {}
    };

    static_assert(sizeof(Handle) == sizeof(HandleValueType),
        "Unexpected: The handle type is expected to be of the same size as its internal integer type");

    /**
     * Appends a new array of the specified length.
     *
     * @param size The fixed size of the array to create.
     * @param value Default value to initializa all array vaules to.
     * @return A handle to the new array.
     * @throws std::out_of_range if the index or the size overflows.
     */
    auto newArray(const size_t size, const ElementType& value = {}) -> Handle
    {
        const auto index = data.size();
        if (index > MaxIndex)
            throw std::out_of_range("Index out of range");
        if (size > MaxSize)
            throw std::out_of_range("Size out of range");

        data.insert(data.end(), size, value);
        return Handle(index, size);
    }

    /**
     * Appends a new array from a pair of iterators.
     *
     * @param begin Start iterator
     * @param begin End iterator
     * @return A handle to the new array.
     * @throws std::out_of_range if the index or the size overflows.
     */
    template <class Iter_>
    auto newArray(Iter_ begin, Iter_ end) -> std::enable_if_t<!std::is_same_v<typename std::iterator_traits<Iter_>::value_type, void>, Handle>
    {
        const auto index = data.size();
        if (index > MaxIndex)
            throw std::out_of_range("Index out of range");
        const auto size = std::distance(begin, end);
        if (size > MaxSize)
            throw std::out_of_range("Size out of range");

        data.insert(data.end(), begin, end);
        return Handle(index, size);
    }

    /**
     * Appends a new array from an initializer list.
     *
     * @param init Initializer list.
     * @return A handle to the new array.
     * @throws std::out_of_range if the index or the size overflows.
     */
    auto newArray(std::initializer_list<ElementType> init) -> Handle
    {
        return newArray(std::begin(init), std::end(init));
    }

    /**
     * Access sub-array element at index.
     *
     * @param h Handle of the sub-array.
     * @param index Index of the element.
     * @return A reference to the element at the specified index.
     * @throws std::out_of_range if the index is out of the sub-arrays bounds.
     */
    auto at(const Handle h, const size_t index) -> ElementType&
    {
        if (index >= h.size)
            throw std::out_of_range("Index out of range");
        return data.at(h.index + index);
    }
    auto at(const Handle h, const size_t index) const -> const ElementType&
    {
        return const_cast<FixedArrayArena&>(*this).at(h, index);
    }

    /**
     * C++ std compatible iterator for the items of a sub-array.
     *
     * NOTE: The iterate keeps a reference to the FixedArrayArena
     *   it refers to!
     */
    template <bool Const_>
    struct IteratorBase
    {
        using iterator_category = std::input_iterator_tag;
        using value_type = std::conditional_t<Const_, const ElementType, ElementType>;
        using difference_type = std::ptrdiff_t;
        using pointer = value_type*;
        using reference = value_type&;
        using ArenaType = std::conditional_t<Const_, const FixedArrayArena, FixedArrayArena>;

        ArenaType& arena;
        const FixedArrayArena::Handle handle;
        size_t index;

        IteratorBase(ArenaType& arena, const Handle h, const size_t i)
            : arena(arena), handle(h), index(i)
        {}

        auto operator*() -> reference
        {
            return arena.at(handle, index);
        }

        auto operator++() -> IteratorBase&
        {
            ++index;
            return *this;
        }

        auto operator==(const IteratorBase& other) const -> bool
        {
            return &arena == &other.arena && handle.index == other.handle.index && handle.size == other.handle.size;
        }

        auto operator!=(const IteratorBase& other) const -> bool
        {
            return !(*this == other);
        }
    };

    /**
     * Proxy type representing a single array of a FixedArrayArena.
     */
    template <bool Const_>
    struct ArrayRefBase
    {
        friend class FixedArrayArena;

        using iterator = IteratorBase<false>;
        using const_iterator = IteratorBase<true>;

        using IteratorType = std::conditional_t<Const_, const_iterator, iterator>;
        using ArenaType = std::conditional_t<Const_, const FixedArrayArena, FixedArrayArena>;

        ArenaType& arena;
        const Handle handle;

        ArrayRefBase(const ArrayRefBase&) = delete;
        ArrayRefBase(ArrayRefBase&&) = default;

        auto at(size_t index) const
        {
            return arena.at(index);
        }

        auto size() const
        {
            return handle.size;
        }

        auto begin() const -> IteratorType
        {
            return {arena, handle, 0u};
        }

        auto end() const -> IteratorType
        {
            return {arena, handle, handle.size};
        }

    private:
        ArrayRefBase(ArenaType& arena, const Handle h)
            : arena(arena), handle(h)
        {}
    };

    using ArrayRef = ArrayRefBase<false>;
    using ConstArrayRef = ArrayRefBase<true>;

    /**
     * Returns a reference to a single array useful for use in
     * C++ range based for-loops.
     *
     * NOTE: Make sure the FixedArrayArena the ArrayRef refers to
     *   is alive as long the ArrayRef is!
     *
     * @param h Handle of the array.
     * @return An object representing the sub-array, holding a reference
     *   to the FixedArrayArena.
     */
    auto array(Handle h) & -> ArrayRef
    {
        return {*this, h};
    }
    auto array(Handle h) const & -> ConstArrayRef
    {
        return {*this, h};
    }

    sfl::segmented_vector<ElementType, PageSize> data;
};

}
