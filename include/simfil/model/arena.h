// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#include <vector>
#include <mutex>

#include <sfl/segmented_vector.hpp>

namespace simfil
{

/// Address of an array within an ArrayArena
using ArrayIndex = int32_t;

/**
 * ArrayArena - An arena allocator for append-only vectors.
 *
 * The ArrayArena is a wrapper around a segmented_vector. It keeps track of
 * forward-linked array chunks. When an array grows beyond the current capacity c
 * of its current last chunk, a new chunk of size c*2 is allocated and becomes
 * the new last chunk. This is then set as linked to the previous last chunk.
 * Usually, appending will be lock-free, and only growth needs the lock.
 *
 * @tparam ElementType The type of elements stored in the arrays.
 * @tparam PageSize The number of elements that each segment in the
 *         segmented_vector can store.
 */
template <class ElementType, size_t PageSize = 4096>
class ArrayArena
{
public:
    /**
     * Creates a new array with the specified initial capacity.
     *
     * @param initialCapacity The initial capacity of the new array.
     * @return The index of the new array.
     */
    ArrayIndex newArray(size_t initialCapacity)
    {
        std::unique_lock<std::mutex> guard(lock_);

        size_t offset = data_.size();
        data_.resize(offset + initialCapacity);

        auto index = static_cast<ArrayIndex>(heads_.size());
        heads_.push_back({offset, initialCapacity, 0, -1, -1});

        return index;
    }

    /**
     * Returns the size of the specified array.
     *
     * @param a The index of the array.
     * @return The size of the array.
     */
    size_t size(ArrayIndex const& a) { return heads_[a].size; }

    /**
     * Returns a reference to the element at the specified index in the array.
     *
     * @param a The index of the array.
     * @param i The index of the element within the array.
     * @return A reference to the element at the specified index.
     * @throws std::out_of_range if the index is out of the array bounds.
     */
    ElementType& at(ArrayIndex const& a, size_t const& i)
    {
        Chunk const& head = heads_[a];

        ArrayIndex current = a;
        size_t remaining = i;

        while (current != -1) {
            Chunk const& chunk = (current == a) ? head : continuations_[current];
            if (remaining < chunk.size) {
                return data_[chunk.offset + remaining];
            }
            remaining -= chunk.size;
            current = chunk.next;
        }

        throw std::out_of_range("Index out of range");
    }

    /**
     * Appends an element to the specified array and returns a reference to it.
     *
     * @param a The index of the array.
     * @param data The element to be appended.
     * @return A reference to the appended element.
     */
    ElementType& push_back(ArrayIndex const& a, ElementType const& data)
    {
        Chunk& updatedLast = ensure_capacity_and_get_last_chunk(a);

        auto elementIt = data_.insert(
            data_.begin() + updatedLast.offset + updatedLast.size,
            data);
        ++heads_[a].size;
        if (&heads_[a] != &updatedLast)
            ++updatedLast.size;

        return *elementIt;
    }

    /**
     * Constructs and appends an element to the specified array and returns a reference to it.
     *
     * @tparam Args The types of the constructor arguments for ElementType.
     * @param a The index of the array.
     * @param args The constructor arguments for ElementType.
     * @return A reference to the appended element.
     */
    template <typename... Args>
    ElementType& emplace_back(ArrayIndex const& a, Args&&... args)
    {
        Chunk& updatedLast = ensure_capacity_and_get_last_chunk(a);

        auto elementIt = data_.emplace(
            data_.begin() + updatedLast.offset + updatedLast.size,
            std::forward<Args>(args)...);
        ++heads_[a].size;
        if (&heads_[a] != &updatedLast)
            ++updatedLast.size;

        return *elementIt;
    }

    /**
     * Clears the ArrayArena by removing all its arrays and releasing their memory.
     *
     * This operation is not thread-safe and should be used with caution.
     * Make sure no other threads are accessing the ArrayArena while calling this method.
     */
    void clear() {
        std::unique_lock<std::mutex> guard(lock_);
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
        std::unique_lock<std::mutex> guard(lock_);
        heads_.shrink_to_fit();
        continuations_.shrink_to_fit();
        data_.shrink_to_fit();
    }

    // Iterator-related types and functions
    template<typename T, bool is_const>
    class ArrayIterator;
    using iterator = ArrayIterator<ElementType, false>;
    using const_iterator = ArrayIterator<ElementType, true>;

    template<typename T, bool is_const>
    class ArrayIterator {
        using ArrayRef = std::conditional_t<is_const, const ArrayArena&, ArrayArena&>;
        using ElementRef = std::conditional_t<is_const, const T&, T&>;

    public:
        ArrayIterator(ArrayRef arena, ArrayIndex array_index, size_t global_index)
            : arena_(arena), array_index_(array_index), global_index_(global_index) {}

        ElementRef operator*() {
            return arena_.at(array_index_, global_index_);
        }

        ArrayIterator& operator++() {
            ++global_index_;
            return *this;
        }

        bool operator==(const ArrayIterator& other) const {
            return &arena_ == &other.arena_ &&
                array_index_ == other.array_index_ &&
                global_index_ == other.global_index_;
        }

        bool operator!=(const ArrayIterator& other) const {
            return !(*this == other);  // NOLINT
        }

    private:
        ArrayRef arena_;
        ArrayIndex array_index_;
        size_t global_index_;
    };

    class ArrayRange
    {
    public:
        ArrayRange(iterator begin, iterator end) : begin_(begin), end_(end) {}

        iterator begin() const { return begin_; }
        iterator end() const { return end_; }

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

private:
    // Represents a chunk of an array in the arena.
    struct Chunk
    {
        size_t offset = 0;      // The starting offset of the chunk in the segmented_vector.
        size_t capacity = 0;    // The maximum number of elements the chunk can hold.
        size_t size = 0;        // The current number of elements in the chunk.

        ArrayIndex next = -1;  // The index of the next chunk in the sequence, or -1 if none.
        ArrayIndex last = -1;  // The index of the last chunk in the sequence, or -1 if none.
    };

    std::vector<ArrayArena::Chunk> heads_;          // A vector holding the head chunks of all arrays.
    std::vector<ArrayArena::Chunk> continuations_;  // A vector holding the continuation chunks of all arrays.

    sfl::segmented_vector<ElementType, PageSize> data_;  // The underlying segmented_vector storing the array elements.

    std::mutex lock_;  // Mutex for synchronizing access to the data structure during growth.

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
        Chunk& head = heads_[a];
        Chunk& last = (head.last == -1) ? head : continuations_[head.last];

        if (last.size == last.capacity) {
            std::unique_lock<std::mutex> guard(lock_);

            size_t offset = data_.size();
            size_t newCapacity = last.capacity * 2;
            data_.resize(offset + newCapacity);

            auto newIndex = static_cast<ArrayIndex>(continuations_.size());
            continuations_.push_back({offset, newCapacity, 0, -1, -1});

            last.next = newIndex;
            head.last = newIndex;
        }

        return (head.last == -1) ? head : continuations_[head.last];
    }
};

} // namespace simfil
