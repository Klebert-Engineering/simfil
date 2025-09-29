// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#include <functional>
#include <vector>
#include <shared_mutex>
#include <mutex>
#include <cmath>
#include <tl/expected.hpp>
#include <sfl/segmented_vector.hpp>

#include "simfil/exception-handler.h"
#include "simfil/error.h"

// Define this to enable array arena read-write locking.
// #define ARRAY_ARENA_THREAD_SAFE

namespace bitsery::ext {
    // Pre-declare bitsery ArrayArena serialization extension
    struct ArrayArenaExt;
}

namespace simfil
{

/// Address of an array within an ArrayArena
using ArrayIndex = int32_t;

/// Array index which can be used to indicate a default/invalid value.
constexpr static ArrayIndex InvalidArrayIndex = -1;

/**
 * ArrayArena - An arena allocator for append-only vectors.
 *
 * The ArrayArena is a wrapper around a segmented_vector. It keeps track of
 * forward-linked array chunks. When an array grows beyond the current capacity c
 * of its current last chunk, a new chunk of size c*2 is allocated and becomes
 * the new last chunk. This is then set as linked to the previous last chunk.
 * Usually, appending will be lock-free, and only growth needs the lock.
 *
 * @tparam ElementType_ The type of elements stored in the arrays.
 * @tparam PageSize The number of elements that each segment in the
 *         segmented_vector can store.
 */
template <class ElementType_, size_t PageSize = 4096, size_t ChunkPageSize = 4096, typename SizeType_ =uint32_t>
class ArrayArena
{
    friend struct bitsery::ext::ArrayArenaExt;

public:
    using ElementType = ElementType_;
    using SizeType = SizeType_;

    /**
     * Creates a new array with the specified initial capacity.
     *
     * @param initialCapacity The initial capacity of the new array.
     * @return The index of the new array.
     */
    ArrayIndex new_array(size_t initialCapacity)
    {
        #ifdef ARRAY_ARENA_THREAD_SAFE
        std::unique_lock guard(lock_);
        #endif
        size_t offset = data_.size();
        data_.resize(offset + initialCapacity);
        auto index = static_cast<ArrayIndex>(heads_.size());
        heads_.push_back({(SizeType_)offset, (SizeType_)initialCapacity, 0,
             InvalidArrayIndex,
             InvalidArrayIndex});
        return index;
    }

    /**
     * Returns the number of arrays in the arena.
     * @return The number of arrays.
     */
    [[nodiscard]] size_t size() const {
    #ifdef ARRAY_ARENA_THREAD_SAFE
            std::shared_lock guard(lock_);
    #endif
            return heads_.size();
    }

    /**
     * Returns the size of the specified array.
     *
     * @param a The index of the array.
     * @return The size of the array.
     */
    [[nodiscard]] SizeType_ size(ArrayIndex const& a) const {
        #ifdef ARRAY_ARENA_THREAD_SAFE
        std::shared_lock guard(lock_);
        #endif
        return heads_[a].size;
    }

    /**
     * Returns a reference to the element at the specified index in the array.
     *
     * @param a The index of the array.
     * @param i The index of the element within the array.
     * @return A reference to the element at the specified index.
     * @throws std::out_of_range if the index is out of the array bounds.
     */
    tl::expected<std::reference_wrapper<ElementType_>, Error>
    at(ArrayIndex const& a, size_t const& i) {
        return at_impl<ElementType_>(*this, a, i);
    }
    tl::expected<std::reference_wrapper<const ElementType_>, Error>
    at(ArrayIndex const& a, size_t const& i) const {
        return at_impl<ElementType_ const>(*this, a, i);
    }

    /**
     * Appends an element to the specified array and returns a reference to it.
     *
     * @param a The index of the array.
     * @param data The element to be appended.
     * @return A reference to the appended element.
     */
    ElementType_& push_back(ArrayIndex const& a, ElementType_ const& data)
    {
        Chunk& updatedLast = ensure_capacity_and_get_last_chunk(a);
        #ifdef ARRAY_ARENA_THREAD_SAFE
        std::shared_lock guard(lock_);
        #endif
        auto& elem = data_[updatedLast.offset + updatedLast.size];
        elem = data;
        ++heads_[a].size;
        if (&heads_[a] != &updatedLast)
            ++updatedLast.size;
        return elem;
    }

    /**
     * Constructs and appends an element to the specified array and returns a reference to it.
     *
     * @tparam Args The types of the constructor arguments for ElementType_.
     * @param a The index of the array.
     * @param args The constructor arguments for ElementType_.
     * @return A reference to the appended element.
     */
    template <typename... Args>
    ElementType_& emplace_back(ArrayIndex const& a, Args&&... args)
    {
        Chunk& updatedLast = ensure_capacity_and_get_last_chunk(a);
        #ifdef ARRAY_ARENA_THREAD_SAFE
        std::shared_lock guard(lock_);
        #endif
        auto& elem = data_[updatedLast.offset + updatedLast.size];
        new (&elem) ElementType_(std::forward<Args>(args)...);
        ++heads_[a].size;
        if (&heads_[a] != &updatedLast)
            ++updatedLast.size;
        return elem;
    }

    /**
     * Clears the ArrayArena by removing all its arrays and releasing their memory.
     *
     * This operation is not thread-safe and should be used with caution.
     * Make sure no other threads are accessing the ArrayArena while calling this method.
     */
    void clear() {
        #ifdef ARRAY_ARENA_THREAD_SAFE
        std::unique_lock guard(lock_);
        #endif
        heads_.clear();
        continuations_.clear();
        data_.clear();
    }

    /**
     * Reduces the memory usage of the ArrayArena by minimizing the capacity of its internal
     * containers to fit their current size.
     *
     * This operation is not thread-safe and should be used with caution.
     * Make sure no other threads are accessing the ArrayArena while calling this method.
     */
    void shrink_to_fit() {
        #ifdef ARRAY_ARENA_THREAD_SAFE
        std::unique_lock guard(lock_);
        #endif
        heads_.shrink_to_fit();
        continuations_.shrink_to_fit();
        data_.shrink_to_fit();
    }

    // Iterator-related types and functions
    template<typename T, bool is_const>
    class ArrayIterator;
    class ArrayRange;
    using iterator = ArrayIterator<ElementType_, false>;
    using const_iterator = ArrayIterator<ElementType_, true>;

    template<typename T, bool is_const>
    class ArrayIterator {
        using ArrayArenaRef = std::conditional_t<is_const, const ArrayArena&, ArrayArena&>;
        using ElementRef = std::conditional_t<is_const, const T&, T&>;
        friend class ArrayRange;

    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = value_type*;
        using reference = ElementRef;

        ArrayIterator(ArrayArenaRef arena, ArrayIndex array_index, size_t elem_index)
            : arena_(arena), array_index_(array_index), elem_index_(elem_index) {}

        ElementRef operator*() noexcept {
            auto res = arena_.at(array_index_, elem_index_);
            assert(res);
            // Unchecked access!
            return *res;
        }

        ArrayIterator& operator++() {
            ++elem_index_;
            return *this;
        }

        bool operator==(const ArrayIterator& other) const {
            return &arena_ == &other.arena_ &&
                array_index_ == other.array_index_ &&
                elem_index_ == other.elem_index_;
        }

        bool operator!=(const ArrayIterator& other) const {
            return !(*this == other);  // NOLINT
        }

    private:
        ArrayArenaRef arena_;
        ArrayIndex array_index_;
        size_t elem_index_;
    };

    class ArrayRange
    {
    public:
        ArrayRange(iterator begin, iterator end) : begin_(begin), end_(end) {}

        iterator begin() const { return begin_; }
        iterator end() const { return end_; }
        [[nodiscard]] size_t size() const { return begin_.arena_.size(begin_.array_index_); }
        decltype(auto) operator[] (size_t const& i) const { return begin_.arena_.at(begin_.array_index_, i); }

    private:
        iterator begin_;
        iterator end_;
    };

    class ArrayArenaIterator
    {
    public:
        ArrayArenaIterator(ArrayArena& arena, ArrayIndex index)
            : arena_(arena), index_(index) {}

        iterator begin() { return arena_.begin(index_); }
        iterator end() { return arena_.end(index_); }
        const_iterator begin() const { return arena_.begin(index_); }
        const_iterator end() const { return arena_.end(index_); }

        ArrayRange operator*() {
            return ArrayRange(arena_.begin(index_), arena_.end(index_));
        }

        ArrayArenaIterator& operator++() {
            ++index_;
            return *this;
        }

        bool operator==(const ArrayArenaIterator& other) const {
            return &arena_ == &other.arena_ && index_ == other.index_;
        }

        bool operator!=(const ArrayArenaIterator& other) const {
            return !(*this == other);  // NOLINT
        }

        using iterator_category = std::input_iterator_tag;
        using value_type = ArrayRange;
        using difference_type = std::ptrdiff_t;
        using pointer = value_type*;
        using reference = value_type&;

    private:
        ArrayArena& arena_;
        ArrayIndex index_;
    };

    iterator begin(ArrayIndex const& a) { return iterator(*this, a, 0); }
    iterator end(ArrayIndex const& a) { return iterator(*this, a, size(a)); }
    const_iterator begin(ArrayIndex const& a) const { return const_iterator(*this, a, 0); }
    const_iterator end(ArrayIndex const& a) const { return const_iterator(*this, a, size(a)); }

    ArrayArenaIterator begin() { return ArrayArenaIterator(*this, 0); }
    ArrayArenaIterator end() { return ArrayArenaIterator(*this, static_cast<ArrayIndex>(heads_.size())); }
    ArrayArenaIterator begin() const { return ArrayArenaIterator(*this, 0); }
    ArrayArenaIterator end() const { return ArrayArenaIterator(*this, static_cast<ArrayIndex>(heads_.size())); }

    ArrayRange range(ArrayIndex const& array) {return ArrayRange(begin(array), end(array));}

    /// Support fast iteration via callback. The passed lambda needs to return true,
    /// as long as the iteration is supposed to continue.
    template <typename Func>
    void iterate(ArrayIndex const& a, Func&& lambda)
    {
        Chunk const* current = &heads_[a];
        size_t globalIndex = 0;
        while (current != nullptr)
        {
            for (size_t i = 0; i < current->size && i < current->capacity; ++i)
            {
                if constexpr (std::is_invocable_r_v<bool, Func, ElementType_&>) {
                    // If lambda returns bool, break if it returns false
                    if (!lambda(data_[current->offset + i]))
                        return;
                }
                else if constexpr (std::is_invocable_v<Func, ElementType_&, size_t>) {
                    // If lambda takes two arguments, pass the current index
                    lambda(data_[current->offset + i], globalIndex);
                }
                else
                    lambda(data_[current->offset + i]);
                ++globalIndex;
            }
            current = (current->next != InvalidArrayIndex) ? &continuations_[current->next] : nullptr;
        }
    }

private:
    // Represents a chunk of an array in the arena.
    struct Chunk
    {
        SizeType_ offset = 0;      // The starting offset of the chunk in the segmented_vector.
        SizeType_ capacity = 0;    // The maximum number of elements the chunk can hold.
        SizeType_ size = 0;        // The current number of elements in the chunk,
                                  // or the total number of elements of the whole array if this is a head chunk.

        ArrayIndex next = InvalidArrayIndex;  // The index of the next chunk in the sequence, or InvalidArrayIndex if none.
        ArrayIndex last = InvalidArrayIndex;  // The index of the last chunk in the sequence, or InvalidArrayIndex if none.
    };

    sfl::segmented_vector<ArrayArena::Chunk, ChunkPageSize> heads_;         // Head chunks of all arrays.
    sfl::segmented_vector<ArrayArena::Chunk, ChunkPageSize> continuations_; // Continuation chunks of all arrays.
    sfl::segmented_vector<ElementType_, PageSize> data_;  // The underlying segmented_vector storing the array elements.

    #ifdef ARRAY_ARENA_THREAD_SAFE
    mutable std::shared_mutex lock_; // Mutex for synchronizing access to the data structure during growth.
    #endif

    /**
     * Ensures that the specified array has enough capacity to add one more element
     * and returns a reference to the last chunk in the array.
     *
     * If the array's last chunk is full, this function allocates a new chunk with
     * twice the capacity of the previous chunk, and links it to the previous last chunk.
     *
     * @param a The index of the array.
     * @return A reference to the last chunk of the array, after ensuring there's capacity.
     */
    Chunk& ensure_capacity_and_get_last_chunk(ArrayIndex const& a)
    {
        #ifdef ARRAY_ARENA_THREAD_SAFE
        std::shared_lock read_guard(lock_);
        #endif
        Chunk& head = heads_[a];
        Chunk& last = (head.last == InvalidArrayIndex) ? head : continuations_[head.last];
        if (last.size < last.capacity)
            return last;
        #ifdef ARRAY_ARENA_THREAD_SAFE
        read_guard.unlock();
        std::unique_lock guard(lock_);
        #endif
        size_t offset = data_.size();
        size_t newCapacity = std::max((SizeType_)2, (SizeType_)last.capacity * 2);
        data_.resize(offset + newCapacity);
        if (head.capacity == 0) {
            head.offset = (SizeType_)offset;
            head.capacity = static_cast<SizeType_>(newCapacity);
            return head;
        }
        auto newIndex = static_cast<ArrayIndex>(continuations_.size());
        continuations_.push_back({(SizeType_)offset, (SizeType_)newCapacity, 0, InvalidArrayIndex, InvalidArrayIndex});
        last.next = newIndex;
        head.last = newIndex;
        return continuations_[newIndex];
    }

    template <typename ElementTypeRef, typename Self>
    static tl::expected<std::reference_wrapper<ElementTypeRef>, Error>
    at_impl(Self& self, ArrayIndex const& a, size_t const& i)
    {
        #ifdef ARRAY_ARENA_THREAD_SAFE
        std::shared_lock guard(self.lock_);
        #endif
        typename Self::Chunk const* current = &self.heads_[a];
        size_t remaining = i;
        while (true) {
            if (remaining < current->capacity && remaining < current->size)
                return self.data_[current->offset + remaining];
            if (current->next == InvalidArrayIndex)
                return tl::unexpected<Error>(Error::IndexOutOfRange, "index out of range");
            remaining -= current->capacity;
            current = &self.continuations_[current->next];
        }
    }
};

} // namespace simfil
