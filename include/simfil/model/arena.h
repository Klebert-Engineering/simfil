// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <vector>
#include <optional>
#include <shared_mutex>
#include <mutex>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <tl/expected.hpp>
#include "simfil/model/column.h"

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

/// Address of an array within an ArrayArena. Note, that only the lowest 3B may be
/// used. This is to allow passing ArrayIndex as the value of a ModelNodeAddress.
using ArrayIndex = uint32_t;

/// Array index which can be used to indicate a default/invalid value.
constexpr static ArrayIndex InvalidArrayIndex = 0x00ffffffu;
constexpr static ArrayIndex FirstRegularArrayIndex = 1u;
constexpr static ArrayIndex SingletonArrayHandleMask = 0x00800000u;
constexpr static ArrayIndex SingletonArrayHandlePayloadMask = 0x007fffffu;

/**
 * ArrayArena - An arena allocator for append-only vectors.
 *
 * The ArrayArena is a wrapper around paged model columns. It keeps track of
 * forward-linked array chunks. When an array grows beyond the current capacity c
 * of its current last chunk, a new chunk of size c*2 is allocated and becomes
 * the new last chunk. This is then set as linked to the previous last chunk.
 * Usually, appending will be lock-free, and only growth needs the lock.
 *
 * @tparam ElementType_ The type of elements stored in the arrays.
 * @tparam PageSize The number of elements that each storage page can store.
 */
template <class ElementType_, size_t PageSize = 4096, size_t ChunkPageSize = 4096, typename SizeType_ =uint32_t>
class ArrayArena
{
    friend struct bitsery::ext::ArrayArenaExt;

public:
    using ElementType = ElementType_;
    using SizeType = SizeType_;
    struct SingletonStats
    {
        size_t handleCount = 0;
        size_t occupiedCount = 0;
        size_t emptyCount = 0;
        size_t singletonStorageBytes = 0;
        size_t hypotheticalRegularBytes = 0;
        size_t estimatedSavedBytes = 0;
    };

    struct CompactArrayChunk
    {
        MODEL_COLUMN_TYPE(8);

        std::uint32_t offset = 0;
        std::uint32_t size = 0;

        template<typename S>
        void serialize(S& s) {
            s.value4b(offset);
            s.value4b(size);
        }
    };
    using CompactHeadStorage = ModelColumn<CompactArrayChunk, ChunkPageSize>;

    ArrayArena()
    {
        ensure_regular_head_pool();
    }

    static constexpr bool is_singleton_handle(ArrayIndex arrayIndex)
    {
        return arrayIndex != InvalidArrayIndex &&
               (arrayIndex & SingletonArrayHandleMask) != 0;
    }

    static constexpr ArrayIndex singleton_payload(ArrayIndex handle)
    {
        return handle & SingletonArrayHandlePayloadMask;
    }

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
        ensure_runtime_heads_from_compact();

        if (initialCapacity == 1U) {
            auto singletonIndex = to_array_index(singletonValues_.size());
            if (singletonIndex > SingletonArrayHandlePayloadMask) {
                raise<std::out_of_range>("ArrayArena singleton pool exhausted.");
            }
            singletonValues_.emplace_back(ElementType_{});
            singletonOccupied_.emplace_back(static_cast<uint8_t>(0));
            compactHeads_.reset();
            return SingletonArrayHandleMask | singletonIndex;
        }

        ensure_regular_head_pool();
        size_t offset = data_.size();
        data_.resize(offset + initialCapacity);
        auto index = to_array_index(heads_.size());
        if ((index & SingletonArrayHandleMask) != 0) {
            raise<std::out_of_range>("ArrayArena regular head index exceeded handle bit range.");
        }
        heads_.push_back({(SizeType_)offset, (SizeType_)initialCapacity, 0,
             InvalidArrayIndex,
             InvalidArrayIndex});
        compactHeads_.reset();
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
            if (heads_.empty() && compactHeads_)
                return compactHeads_->size();
            return heads_.size();
    }

    [[nodiscard]] size_t singleton_handle_count() const
    {
        return singletonValues_.size();
    }

    [[nodiscard]] size_t singleton_occupied_count() const
    {
        size_t occupiedCount = 0;
        for (auto const occupied : singletonOccupied_) {
            occupiedCount += occupied == 0 ? 0 : 1;
        }
        return occupiedCount;
    }

    [[nodiscard]] SingletonStats singleton_stats() const
    {
        const auto handleCount = singleton_handle_count();
        const auto occupiedCount = singleton_occupied_count();
        const auto emptyCount = handleCount >= occupiedCount ? handleCount - occupiedCount : 0;

        const auto singletonStorageBytes =
            singletonValues_.byte_size() + singletonOccupied_.byte_size();
        const auto hypotheticalRegularBytes =
            handleCount * sizeof(CompactArrayChunk) + occupiedCount * sizeof(ElementType_);

        return SingletonStats{
            .handleCount = handleCount,
            .occupiedCount = occupiedCount,
            .emptyCount = emptyCount,
            .singletonStorageBytes = singletonStorageBytes,
            .hypotheticalRegularBytes = hypotheticalRegularBytes,
            .estimatedSavedBytes = hypotheticalRegularBytes > singletonStorageBytes
                ? hypotheticalRegularBytes - singletonStorageBytes
                : 0};
    }

    [[nodiscard]] bool valid(ArrayIndex a) const
    {
        if (a == InvalidArrayIndex) {
            return false;
        }
        if (is_singleton_handle(a)) {
            auto singletonIndex = singleton_payload(a);
            return singletonIndex < singletonValues_.size() &&
                   singletonIndex < singletonOccupied_.size();
        }
        if (heads_.empty() && compactHeads_) {
            return a < compactHeads_->size();
        }
        return a < heads_.size();
    }

    /**
     * Returns the size of the specified array.
     *
     * @param a The index of the array.
     * @return The size of the array.
     */
    [[nodiscard]] SizeType_ size(ArrayIndex a) const {
        #ifdef ARRAY_ARENA_THREAD_SAFE
        std::shared_lock guard(lock_);
        #endif
        if (is_singleton_handle(a)) {
            auto singletonIndex = singleton_payload(a);
            if (singletonIndex >= singletonOccupied_.size()) {
                raise<std::out_of_range>("ArrayArena singleton handle index out of range.");
            }
            return singletonOccupied_.at(singletonIndex) == 0 ? 0 : 1;
        }

        if (heads_.empty() && compactHeads_) {
            if (a >= compactHeads_->size()) {
                raise<std::out_of_range>("ArrayArena head index out of range.");
            }
            return static_cast<SizeType_>((*compactHeads_)[a].size);
        }
        if (a >= heads_.size()) {
            raise<std::out_of_range>("ArrayArena head index out of range.");
        }
        return heads_[a].size;
    }

    /**
     * @return The current size, in bytes, of the array arena if serialized.
     */
    [[nodiscard]] size_t byte_size() const {
        auto singletonBytes =
            singletonValues_.byte_size() +
            singletonOccupied_.byte_size();
        if (compactHeads_) {
            return compactHeads_->byte_size() + data_.byte_size() + singletonBytes;
        }
        auto result = heads_.size() * sizeof(CompactArrayChunk);
        for (auto const& head : heads_) {
            result += head.size * sizeof(ElementType_);
        }
        return result + singletonBytes;
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
    at(ArrayIndex a, size_t i) {
        return at_impl<ElementType_>(*this, a, i);
    }
    tl::expected<std::reference_wrapper<const ElementType_>, Error>
    at(ArrayIndex a, size_t i) const {
        return at_impl<ElementType_ const>(*this, a, i);
    }

    /**
     * Appends an element to the specified array and returns a reference to it.
     *
     * @param a The index of the array.
     * @param data The element to be appended.
     * @return A reference to the appended element.
     */
    ElementType_& push_back(ArrayIndex a, ElementType_ const& data)
    {
        if (is_singleton_handle(a)) {
            #ifdef ARRAY_ARENA_THREAD_SAFE
            std::unique_lock guard(lock_);
            #endif
            auto singletonIndex = singleton_payload(a);
            if (singletonIndex >= singletonValues_.size() ||
                singletonIndex >= singletonOccupied_.size()) {
                raise<std::out_of_range>("ArrayArena singleton handle index out of range.");
            }
            auto& occupied = singletonOccupied_.at(singletonIndex);
            if (occupied != 0) {
                raise<std::runtime_error>(
                    "Cannot append more than one element to a singleton array handle.");
            }
            singletonValues_.at(singletonIndex) = data;
            occupied = 1;
            compactHeads_.reset();
            return singletonValues_.at(singletonIndex);
        }

        Chunk& updatedLast = ensure_capacity_and_get_last_chunk(a);
        #ifdef ARRAY_ARENA_THREAD_SAFE
        std::shared_lock guard(lock_);
        #endif
        auto& elem = data_[updatedLast.offset + updatedLast.size];
        elem = data;
        ++heads_[a].size;
        if (&heads_[a] != &updatedLast)
            ++updatedLast.size;
        compactHeads_.reset();
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
    ElementType_& emplace_back(ArrayIndex a, Args&&... args)
    {
        if (is_singleton_handle(a)) {
            #ifdef ARRAY_ARENA_THREAD_SAFE
            std::unique_lock guard(lock_);
            #endif
            auto singletonIndex = singleton_payload(a);
            if (singletonIndex >= singletonValues_.size() ||
                singletonIndex >= singletonOccupied_.size()) {
                raise<std::out_of_range>("ArrayArena singleton handle index out of range.");
            }
            auto& occupied = singletonOccupied_.at(singletonIndex);
            if (occupied != 0) {
                raise<std::runtime_error>(
                    "Cannot append more than one element to a singleton array handle.");
            }
            singletonValues_.at(singletonIndex) = ElementType_(std::forward<Args>(args)...);
            occupied = 1;
            compactHeads_.reset();
            return singletonValues_.at(singletonIndex);
        }

        Chunk& updatedLast = ensure_capacity_and_get_last_chunk(a);
        #ifdef ARRAY_ARENA_THREAD_SAFE
        std::shared_lock guard(lock_);
        #endif
        auto& elem = data_[updatedLast.offset + updatedLast.size];
        new (&elem) ElementType_(std::forward<Args>(args)...);
        ++heads_[a].size;
        if (&heads_[a] != &updatedLast)
            ++updatedLast.size;
        compactHeads_.reset();
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
        singletonValues_.clear();
        singletonOccupied_.clear();
        compactHeads_.reset();
        ensure_regular_head_pool();
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
        singletonValues_.shrink_to_fit();
        singletonOccupied_.shrink_to_fit();
        if (compactHeads_) {
            compactHeads_->shrink_to_fit();
        }
    }

    /**
     * Check if a compact chunk representation is available.
     */
    [[nodiscard]] bool is_compact() const {
        return compactHeads_.has_value();
    }

    [[nodiscard]] bool isCompact() const {
        return is_compact();
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
        decltype(auto) operator[] (size_t i) const { return begin_.arena_.at(begin_.array_index_, i); }

    private:
        iterator begin_;
        iterator end_;
    };

    class ArrayArenaIterator
    {
    public:
        ArrayArenaIterator(ArrayArena& arena, size_t ordinal)
            : arena_(arena),
              ordinal_(ordinal)
        {
            update_array_index();
        }

        iterator begin() { return arena_.begin(index_); }
        iterator end() { return arena_.end(index_); }
        const_iterator begin() const { return arena_.begin(index_); }
        const_iterator end() const { return arena_.end(index_); }

        ArrayRange operator*() {
            return ArrayRange(arena_.begin(index_), arena_.end(index_));
        }

        ArrayArenaIterator& operator++() {
            ++ordinal_;
            update_array_index();
            return *this;
        }

        bool operator==(const ArrayArenaIterator& other) const {
            return &arena_ == &other.arena_ && ordinal_ == other.ordinal_;
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
        [[nodiscard]] size_t regular_array_count() const
        {
            if (arena_.heads_.empty() && arena_.compactHeads_) {
                return arena_.compactHeads_->size();
            }
            return arena_.heads_.size();
        }

        [[nodiscard]] size_t visible_regular_array_count() const
        {
            const auto count = regular_array_count();
            return count > FirstRegularArrayIndex ? count - FirstRegularArrayIndex : 0;
        }

        [[nodiscard]] size_t total_visible_array_count() const
        {
            return visible_regular_array_count() + arena_.singleton_handle_count();
        }

        void update_array_index()
        {
            const auto regularCount = visible_regular_array_count();
            if (ordinal_ < regularCount) {
                index_ = to_array_index(FirstRegularArrayIndex + ordinal_);
                return;
            }

            const auto singletonOrdinal = ordinal_ - regularCount;
            if (ordinal_ < total_visible_array_count() &&
                singletonOrdinal <= SingletonArrayHandlePayloadMask) {
                index_ = SingletonArrayHandleMask | to_array_index(singletonOrdinal);
                return;
            }

            index_ = InvalidArrayIndex;
        }

        ArrayArena& arena_;
        size_t ordinal_ = 0;
        ArrayIndex index_;
    };

    iterator begin(ArrayIndex a) { return iterator(*this, a, 0); }
    iterator end(ArrayIndex a) { return iterator(*this, a, size(a)); }
    const_iterator begin(ArrayIndex a) const { return const_iterator(*this, a, 0); }
    const_iterator end(ArrayIndex a) const { return const_iterator(*this, a, size(a)); }

    ArrayArenaIterator begin() { return ArrayArenaIterator(*this, 0); }
    ArrayArenaIterator end()
    {
        const auto regularCount = size();
        const auto visibleRegularCount = regularCount > FirstRegularArrayIndex
            ? regularCount - FirstRegularArrayIndex
            : 0;
        return ArrayArenaIterator(*this, visibleRegularCount + singleton_handle_count());
    }
    ArrayArenaIterator begin() const
    {
        return ArrayArenaIterator(const_cast<ArrayArena&>(*this), 0);
    }
    ArrayArenaIterator end() const
    {
        const auto regularCount = size();
        const auto visibleRegularCount = regularCount > FirstRegularArrayIndex
            ? regularCount - FirstRegularArrayIndex
            : 0;
        return ArrayArenaIterator(
            const_cast<ArrayArena&>(*this),
            visibleRegularCount + singleton_handle_count());
    }

    ArrayRange range(ArrayIndex array) {return ArrayRange(begin(array), end(array));}

    /// Support fast iteration via callback. The passed lambda needs to return true,
    /// as long as the iteration is supposed to continue.
    template <typename Func>
    void iterate(ArrayIndex a, Func&& lambda)
    {
        if (is_singleton_handle(a)) {
            auto singletonIndex = singleton_payload(a);
            if (singletonIndex >= singletonValues_.size() ||
                singletonIndex >= singletonOccupied_.size()) {
                raise<std::out_of_range>("ArrayArena singleton handle index out of range.");
            }
            if (singletonOccupied_.at(singletonIndex) == 0) {
                return;
            }
            if constexpr (std::is_invocable_r_v<bool, Func, ElementType_&>) {
                (void)lambda(singletonValues_.at(singletonIndex));
            }
            else if constexpr (std::is_invocable_v<Func, ElementType_&, size_t>) {
                lambda(singletonValues_.at(singletonIndex), 0);
            }
            else {
                lambda(singletonValues_.at(singletonIndex));
            }
            return;
        }

        if (heads_.empty() && compactHeads_) {
            if (a >= compactHeads_->size()) {
                raise<std::out_of_range>("ArrayArena head index out of range.");
            }
            auto const& compact = (*compactHeads_)[a];
            for (size_t i = 0; i < static_cast<size_t>(compact.size); ++i)
            {
                if constexpr (std::is_invocable_r_v<bool, Func, ElementType_&>) {
                    if (!lambda(data_[static_cast<size_t>(compact.offset) + i]))
                        return;
                }
                else if constexpr (std::is_invocable_v<Func, ElementType_&, size_t>) {
                    lambda(data_[static_cast<size_t>(compact.offset) + i], i);
                }
                else {
                    lambda(data_[static_cast<size_t>(compact.offset) + i]);
                }
            }
            return;
        }

        if (a >= heads_.size()) {
            raise<std::out_of_range>("ArrayArena head index out of range.");
        }
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
        MODEL_COLUMN_TYPE((sizeof(SizeType_) * 3) + (sizeof(ArrayIndex) * 2));

        SizeType_ offset = 0;      // The starting offset of the chunk in the storage buffer.
        SizeType_ capacity = 0;    // The maximum number of elements the chunk can hold.
        SizeType_ size = 0;        // The current number of elements in the chunk,
                                  // or the total number of elements of the whole array if this is a head chunk.

        ArrayIndex next = InvalidArrayIndex;  // The index of the next chunk in the sequence, or InvalidArrayIndex if none.
        ArrayIndex last = InvalidArrayIndex;  // The index of the last chunk in the sequence, or InvalidArrayIndex if none.
    };

    ModelColumn<ArrayArena::Chunk, ChunkPageSize> heads_;         // Head chunks of all arrays.
    ModelColumn<ArrayArena::Chunk, ChunkPageSize> continuations_; // Continuation chunks of all arrays.
    ModelColumn<ElementType_, PageSize> data_;  // Underlying element storage.
    ModelColumn<ElementType_, PageSize> singletonValues_;
    ModelColumn<uint8_t, PageSize> singletonOccupied_;
    std::optional<CompactHeadStorage> compactHeads_;

    #ifdef ARRAY_ARENA_THREAD_SAFE
    mutable std::shared_mutex lock_; // Mutex for synchronizing access to the data structure during growth.
    #endif

    static ArrayIndex to_array_index(size_t value)
    {
        if (value > std::numeric_limits<ArrayIndex>::max()) {
            raise<std::out_of_range>("ArrayArena index exceeds address space.");
        }
        return static_cast<ArrayIndex>(value);
    }

    void ensure_regular_head_pool()
    {
        if (!heads_.empty()) {
            return;
        }
        heads_.push_back({
            0,
            0,
            0,
            InvalidArrayIndex,
            InvalidArrayIndex
        });
    }

    void ensure_runtime_heads_from_compact()
    {
        if (!heads_.empty() || !compactHeads_)
            return;

        heads_.clear();
        heads_.reserve(compactHeads_->size());
        continuations_.clear();
        for (auto const& compactHead : *compactHeads_) {
            heads_.push_back({
                static_cast<SizeType_>(compactHead.offset),
                static_cast<SizeType_>(compactHead.size),
                static_cast<SizeType_>(compactHead.size),
                InvalidArrayIndex,
                InvalidArrayIndex
            });
        }
        ensure_regular_head_pool();
    }

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
    Chunk& ensure_capacity_and_get_last_chunk(ArrayIndex a)
    {
        if (is_singleton_handle(a)) {
            raise<std::runtime_error>("Singleton handles do not use chunk growth.");
        }

        #ifndef ARRAY_ARENA_THREAD_SAFE
        ensure_runtime_heads_from_compact();
        ensure_regular_head_pool();
        #endif

        #ifdef ARRAY_ARENA_THREAD_SAFE
        std::shared_lock read_guard(lock_);
        if (heads_.empty() && compactHeads_) {
            read_guard.unlock();
            std::unique_lock write_guard(lock_);
            ensure_runtime_heads_from_compact();
            write_guard.unlock();
            read_guard.lock();
        }
        if (heads_.empty()) {
            read_guard.unlock();
            std::unique_lock write_guard(lock_);
            ensure_regular_head_pool();
            write_guard.unlock();
            read_guard.lock();
        }
        #endif
        if (a >= heads_.size()) {
            raise<std::out_of_range>("ArrayArena head index out of range.");
        }
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
        auto newIndex = to_array_index(continuations_.size());
        continuations_.push_back({(SizeType_)offset, (SizeType_)newCapacity, 0, InvalidArrayIndex, InvalidArrayIndex});
        last.next = newIndex;
        head.last = newIndex;
        return continuations_[newIndex];
    }

    template <typename ElementTypeRef, typename Self>
    static tl::expected<std::reference_wrapper<ElementTypeRef>, Error>
    at_impl(Self& self, ArrayIndex a, size_t i)
    {
        #ifdef ARRAY_ARENA_THREAD_SAFE
        std::shared_lock guard(self.lock_);
        #endif
        if (is_singleton_handle(a)) {
            auto singletonIndex = singleton_payload(a);
            if (singletonIndex >= self.singletonValues_.size() ||
                singletonIndex >= self.singletonOccupied_.size()) {
                return tl::unexpected<Error>(Error::IndexOutOfRange, "singleton handle index out of range");
            }
            if (self.singletonOccupied_.at(singletonIndex) == 0 || i > 0) {
                return tl::unexpected<Error>(Error::IndexOutOfRange, "index out of range");
            }
            return self.singletonValues_.at(singletonIndex);
        }

        if (self.heads_.empty() && self.compactHeads_) {
            if (a >= self.compactHeads_->size()) {
                return tl::unexpected<Error>(Error::IndexOutOfRange, "array index out of range");
            }
            auto const& compact = (*self.compactHeads_)[a];
            if (i < static_cast<size_t>(compact.size))
                return self.data_[static_cast<size_t>(compact.offset) + i];
            return tl::unexpected<Error>(Error::IndexOutOfRange, "index out of range");
        }

        if (a >= self.heads_.size()) {
            return tl::unexpected<Error>(Error::IndexOutOfRange, "array index out of range");
        }

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
