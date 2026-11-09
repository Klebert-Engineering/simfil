// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <vector>
#include <optional>
#include <shared_mutex>
#include <mutex>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>
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
 * ArrayArena - An arena allocator for append-oriented, truncatable vectors.
 *
 * The ArrayArena is a wrapper around paged model columns. It keeps track of
 * forward-linked array chunks for regular growable arrays, optional singleton
 * handles for fixed-size 0-or-1 arrays, and an optional compact head
 * representation used during serialization. Access is unsynchronized by default.
 * ARRAY_ARENA_THREAD_SAFE adds locks around individual checked access/allocation
 * operations; borrowed references, iteration, serialization and several metadata
 * queries still require the caller to exclude concurrent structural mutation.
 *
 * @tparam ElementType_ The type of elements stored in the arrays.
 * @tparam PageSize The number of elements that each storage page can store.
 */
template <
    class ElementType_,
    size_t PageSize = 4096,
    size_t ChunkPageSize = 4096,
    typename SizeType_ = uint32_t>
class ArrayArena
{
    struct Chunk;
    friend struct bitsery::ext::ArrayArenaExt;

public:
    using ElementType = ElementType_;
    using SizeType = SizeType_;
    using DataStorage = ModelColumn<ElementType_, PageSize>;
    using DataWriteRef = decltype(std::declval<DataStorage&>()[std::declval<size_t>()]);
    using DataReadRef = decltype(std::declval<DataStorage const&>()[std::declval<size_t>()]);
    using AtValue = detail::arena_access_result_t<DataWriteRef>;
    using ConstAtValue = detail::arena_access_result_t<DataReadRef>;

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

    /** Opaque logical tails for appends to newly allocated arrays only. */
    class AppendCheckpoint
    {
    public:
        AppendCheckpoint(AppendCheckpoint const&) = default;
        AppendCheckpoint& operator=(AppendCheckpoint const&) = default;

    private:
        friend class ArrayArena;

        explicit AppendCheckpoint(ArrayArena const& arena)
            : owner_(&arena), heads_(arena.heads_.size()), continuations_(arena.continuations_.size()),
              data_(arena.data_.size()), singletons_(arena.singletonValues_.size()),
              occupancy_(arena.singletonOccupied_.size())
        {
        }

        ArrayArena const* owner_;
        size_t heads_;
        size_t continuations_;
        size_t data_;
        size_t singletons_;
        size_t occupancy_;
    };

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

    /** Validate a bulk allocation before calling new_array; no storage is changed. */
    [[nodiscard]] bool check_capacity(size_t added, bool fixedSize = false) const
    {
        #ifdef ARRAY_ARENA_THREAD_SAFE
        std::shared_lock guard(lock_);
        #endif
        if (added == 1 && fixedSize) {
            return singletonValues_.size() < SingletonArrayHandlePayloadMask &&
                check_model_column_growth(singletonValues_.size(), 1, DataStorage::record_size) &&
                check_model_column_growth(singletonOccupied_.size(), 1, sizeof(uint8_t));
        }
        auto const headCount = heads_.empty() && compactHeads_ ? compactHeads_->size() : heads_.size();
        return headCount < SingletonArrayHandleMask && check_data_growth(data_.size(), added);
    }

    /** Validate narrow offsets and the serialized payload size without overflowing. */
    [[nodiscard]] static constexpr bool check_data_growth(size_t current, size_t added) noexcept
    {
        auto const maximum = static_cast<size_t>(std::numeric_limits<SizeType_>::max());
        return current <= maximum && added <= maximum - current &&
            check_model_column_growth(current, added, DataStorage::record_size);
    }

    /**
     * Creates a new array with the specified initial capacity.
     *
     * @param initialCapacity The initial capacity of the new array.
     * @param fixedSize If true, `initialCapacity == 1` creates a singleton handle
     *                  instead of a growable chunk-backed array.
     * @pre The payload fits its wire and offset representations. Use check_capacity
     *      to validate an untrusted initial capacity before calling this builder.
     * @return The index of the new array.
     */
    ArrayIndex new_array(size_t initialCapacity, bool fixedSize = false)
    {
        #ifdef ARRAY_ARENA_THREAD_SAFE
        std::unique_lock guard(lock_);
        #endif
        ensure_runtime_heads_from_compact();

        if (initialCapacity == 1U && fixedSize) {
            auto singletonIndex = to_array_index(singletonValues_.size());
            if (singletonIndex >= SingletonArrayHandlePayloadMask) {
                raise<std::out_of_range>("ArrayArena singleton pool exhausted.");
            }
            assert(DataStorage::check_size(singletonValues_.size() + 1));
            assert(singletonOccupied_.check_size(singletonOccupied_.size() + 1));
            singletonValues_.emplace_back(ElementType_{});
            try {
                singletonOccupied_.emplace_back(static_cast<uint8_t>(0));
            } catch (...) {
                singletonValues_.resize(singletonIndex);
                throw;
            }
            compactHeads_.reset();
            return SingletonArrayHandleMask | singletonIndex;
        }

        ensure_regular_head_pool();
        size_t offset = data_.size();
        assert(check_data_growth(offset, initialCapacity));
        auto index = to_array_index(heads_.size());
        if ((index & SingletonArrayHandleMask) != 0) {
            raise<std::out_of_range>("ArrayArena regular head index exceeded handle bit range.");
        }
        assert(heads_.check_size(heads_.size() + 1));
        data_.resize(offset + initialCapacity);
        try {
            heads_.push_back({(SizeType_)offset, (SizeType_)initialCapacity, 0,
                 InvalidArrayIndex, InvalidArrayIndex});
        } catch (...) {
            data_.resize(offset);
            throw;
        }
        compactHeads_.reset();
        return index;
    }

    /**
     * Snapshot append-only allocation tails without allocating.
     * Until rollback/discard, only arrays allocated after this checkpoint may
     * be populated: old arrays must remain untouched, with no concurrent
     * mutation, clear, read, or arena replacement. Compact storage is rejected
     * before any mutation rather than materializing runtime heads here.
     */
    [[nodiscard]] AppendCheckpoint checkpointAppends() const
    {
        if (is_compact()) {
            raise<std::logic_error>("Cannot checkpoint appends to a compact ArrayArena.");
        }
        return AppendCheckpoint(*this);
    }

    /**
     * Restore this arena's checkpoint tails; never repair edits to old arrays.
     * The checkpoint must belong to this arena and still precede its logical
     * tails. New handles/references become invalid and may be reused by later
     * appends. Backing capacity is not shrunk. Requires exclusive construction.
     */
    void rollbackAppends(AppendCheckpoint const& checkpoint) noexcept
    {
        assert(checkpoint.owner_ == this && !is_compact());
        assert(checkpoint.heads_ <= heads_.size());
        assert(checkpoint.continuations_ <= continuations_.size());
        assert(checkpoint.data_ <= data_.size());
        assert(checkpoint.singletons_ <= singletonValues_.size());
        assert(checkpoint.occupancy_ <= singletonOccupied_.size());
        heads_.resize(checkpoint.heads_);
        continuations_.resize(checkpoint.continuations_);
        data_.resize(checkpoint.data_);
        singletonValues_.resize(checkpoint.singletons_);
        singletonOccupied_.resize(checkpoint.occupancy_);
        compactHeads_.reset();
    }

    /**
     * Number of regular head slots, including reserved slot zero.
     * Singleton handles are counted separately by singleton_handle_count().
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
            handleCount * sizeof(CompactArrayChunk) + occupiedCount * DataStorage::record_size;

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
     * Logical compact payload size, excluding column length prefixes on the wire.
     */
    [[nodiscard]] size_t byte_size() const {
        auto singletonBytes =
            singletonValues_.byte_size() +
            singletonOccupied_.byte_size();
        if (heads_.empty() && compactHeads_) {
            // Compact truncation leaves retained holes; serialization repacks
            // only the live slices, so unused payload is not a wire-size cost.
            auto result = compactHeads_->byte_size() + singletonBytes;
            for (auto const& head : *compactHeads_) {
                result += head.size * DataStorage::record_size;
            }
            return result;
        }
        auto result = heads_.size() * sizeof(CompactArrayChunk);
        for (auto const& head : heads_) {
            result += head.size * DataStorage::record_size;
        }
        return result + singletonBytes;
    }

    /**
     * Return compact logical payload and all retained arena backing capacity.
     *
     * Unlike `byte_size()`, allocated bytes include unused array capacity,
     * continuation chunks, and storage retained after clear operations.
     */
    [[nodiscard]] MemoryUsage memory_usage() const
    {
        #ifdef ARRAY_ARENA_THREAD_SAFE
        std::shared_lock guard(lock_);
        #endif
        MemoryUsage result;
        result.logicalBytes = byte_size();
        result.allocatedBytes =
            heads_.memory_usage().allocatedBytes +
            continuations_.memory_usage().allocatedBytes +
            data_.memory_usage().allocatedBytes +
            singletonValues_.memory_usage().allocatedBytes +
            singletonOccupied_.memory_usage().allocatedBytes;
        if (compactHeads_) {
            result.allocatedBytes += compactHeads_->memory_usage().allocatedBytes;
        }
        result.allocatedBytes = std::max(result.logicalBytes, result.allocatedBytes);
        return result;
    }

    /**
     * Returns a reference to the element at the specified index in the array.
     *
     * @param a The index of the array.
     * @param i The index of the element within the array.
     * @return A wrapped reference/proxy, or Error::IndexOutOfRange for an
     *         invalid array handle or element index.
     * The result borrows storage and holds no lock after this call returns.
     */
    tl::expected<AtValue, Error>
    at(ArrayIndex a, size_t i) {
        return at_impl<ArrayArena, AtValue>(*this, a, i);
    }

    tl::expected<ConstAtValue, Error>
    at(ArrayIndex a, size_t i) const {
        return at_impl<ArrayArena const, ConstAtValue>(*this, a, i);
    }

    /**
     * Appends an element to the specified array and returns a reference to it.
     *
     * @param a The index of the array.
     * @param data The element to be appended.
     * @return A reference to the appended element.
     */
    DataWriteRef push_back(ArrayIndex a, ElementType_ const& data)
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
            // Singleton writes leave the regular heads, including compact-only
            // heads, intact.
            return singletonValues_.at(singletonIndex);
        }

        #ifdef ARRAY_ARENA_THREAD_SAFE
        std::unique_lock guard(lock_);
        #endif
        Chunk& updatedLast = ensure_capacity_and_get_last_chunk_unlocked(a);
        DataWriteRef elem = data_[updatedLast.offset + updatedLast.size];
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
    DataWriteRef emplace_back(ArrayIndex a, Args&&... args)
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
            return singletonValues_.at(singletonIndex);
        }

        #ifdef ARRAY_ARENA_THREAD_SAFE
        std::unique_lock guard(lock_);
        #endif
        Chunk& updatedLast = ensure_capacity_and_get_last_chunk_unlocked(a);
        DataWriteRef elem = data_[updatedLast.offset + updatedLast.size];
        elem = ElementType_(std::forward<Args>(args)...);
        ++heads_[a].size;
        if (&heads_[a] != &updatedLast)
            ++updatedLast.size;
        compactHeads_.reset();
        return elem;
    }

    /**
     * Keep the first count elements without allocating or changing the handle.
     * Removed payload and detached chunks remain retained in memory_usage().
     * Appending afterwards reuses the surviving tail's capacity before growing.
     * Invalid handles or counts greater than size(a) throw std::out_of_range.
     * Structural changes invalidate previously obtained ranges and iterators.
     */
    void truncate(ArrayIndex a, size_t count)
    {
        #ifdef ARRAY_ARENA_THREAD_SAFE
        std::unique_lock guard(lock_);
        #endif
        if (is_singleton_handle(a)) {
            auto const index = singleton_payload(a);
            if (index >= singletonValues_.size() || index >= singletonOccupied_.size()) {
                raise<std::out_of_range>("ArrayArena singleton handle index out of range.");
            }
            auto& occupied = singletonOccupied_[index];
            if (count > (occupied == 0 ? 0U : 1U)) {
                raise<std::out_of_range>("ArrayArena truncate count out of range.");
            }
            occupied = static_cast<uint8_t>(count);
            return;
        }

        if (heads_.empty() && compactHeads_) {
            if (a >= compactHeads_->size()) {
                raise<std::out_of_range>("ArrayArena head index out of range.");
            }
            auto& head = (*compactHeads_)[a];
            if (count > head.size) {
                raise<std::out_of_range>("ArrayArena truncate count out of range.");
            }
            // Keep compact heads authoritative: materializing runtime heads
            // here would allocate. Other arrays' offsets must not move.
            head.size = static_cast<uint32_t>(count);
            return;
        }

        if (a >= heads_.size()) {
            raise<std::out_of_range>("ArrayArena head index out of range.");
        }
        auto& head = heads_[a];
        if (count > head.size) {
            raise<std::out_of_range>("ArrayArena truncate count out of range.");
        }
        if (count == head.size) {
            return;
        }

        auto* tail = &head;
        auto tailIndex = InvalidArrayIndex;
        auto remaining = count;
        while (remaining > tail->capacity) {
            remaining -= tail->capacity;
            tailIndex = tail->next;
            tail = &continuations_[tailIndex];
        }
        tail->size = static_cast<SizeType_>(remaining);
        tail->next = InvalidArrayIndex;
        head.last = tailIndex;
        head.size = static_cast<SizeType_>(count);
        compactHeads_.reset();
    }

    /**
     * Remove all arrays and restore reserved head zero. Backing capacity is
     * retained except for compact heads. All handles, ranges and references
     * become invalid; callers must exclude concurrent use of borrowed storage.
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
     * Ask each column to release unused backing capacity. This does not repack
     * holes left by truncation. It may invalidate borrowed references and
     * iterators, so callers must exclude their concurrent use.
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
     * Check if the arena is currently represented by compact heads only.
     */
    [[nodiscard]] bool is_compact() const {
        return heads_.empty() && compactHeads_.has_value();
    }

    // Iterator-related types and functions
    template<typename T, bool is_const>
    class ArrayIterator;
    using iterator = ArrayIterator<ElementType_, false>;
    using const_iterator = ArrayIterator<ElementType_, true>;
    template <bool T_IsConst>
    class BasicArrayRange;
    template <bool T_IsConst>
    class BasicArrayArenaIterator;
    using arena_iterator = BasicArrayArenaIterator<false>;
    using const_arena_iterator = BasicArrayArenaIterator<true>;

    /**
     * Multipass cursor over one array. Dereference and increment are O(1);
     * construction at an arbitrary index walks the preceding chunks.
     * Structural arena mutations invalidate cursors, even when payload addresses
     * survive: paged column iterators can retain a relocated page table.
     */
    template<typename T, bool is_const>
    class ArrayIterator {
        using Arena = std::conditional_t<is_const, const ArrayArena, ArrayArena>;
        using Storage = std::conditional_t<is_const, const DataStorage, DataStorage>;
        using ColumnIterator = std::conditional_t<
            is_const, typename DataStorage::const_iterator, typename DataStorage::iterator>;
        template <typename, bool>
        friend class ArrayIterator;
        template <bool>
        friend class BasicArrayRange;

    public:
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using reference = std::conditional_t<is_const, DataReadRef, DataWriteRef>;
        using pointer = std::conditional_t<
            std::is_lvalue_reference_v<reference>, std::add_pointer_t<reference>, void>;
        using iterator_concept = std::forward_iterator_tag;
        // Legacy forward iterators require actual references; C++20 permits proxies.
        using iterator_category = std::conditional_t<
            std::is_lvalue_reference_v<reference>, std::forward_iterator_tag, std::input_iterator_tag>;

        ArrayIterator() = default;

        /** The handle must be valid and elem_index must be at most size(array_index). */
        ArrayIterator(Arena& arena, ArrayIndex array_index, size_t elem_index)
            : arena_(&arena), array_index_(array_index), elem_index_(elem_index)
        {
            #ifdef ARRAY_ARENA_THREAD_SAFE
            std::shared_lock guard(arena.lock_);
            #endif
            if (is_singleton_handle(array_index)) {
                auto const index = singleton_payload(array_index);
                if (index >= arena.singletonValues_.size() || index >= arena.singletonOccupied_.size()) {
                    raise<std::out_of_range>("ArrayArena singleton handle index out of range.");
                }
                auto const count = arena.singletonOccupied_[index] == 0 ? 0U : 1U;
                assert(elem_index <= count);
                if (elem_index < count) {
                    set_cursor(arena.singletonValues_, index);
                    remaining_ = 1;
                }
                return;
            }

            if (arena.heads_.empty() && arena.compactHeads_) {
                if (array_index >= arena.compactHeads_->size()) {
                    raise<std::out_of_range>("ArrayArena head index out of range.");
                }
                auto const& head = (*arena.compactHeads_)[array_index];
                assert(elem_index <= head.size);
                if (elem_index < head.size) {
                    set_cursor(arena.data_, static_cast<size_t>(head.offset) + elem_index);
                    remaining_ = head.size - elem_index;
                }
                return;
            }

            if (array_index >= arena.heads_.size()) {
                raise<std::out_of_range>("ArrayArena head index out of range.");
            }
            auto const* chunk = &arena.heads_[array_index];
            assert(elem_index <= chunk->size);
            if (elem_index == chunk->size) {
                return;
            }

            auto offset = elem_index;
            while (offset >= chunk->capacity) {
                offset -= chunk->capacity;
                assert(chunk->next != InvalidArrayIndex);
                chunk = &arena.continuations_[chunk->next];
            }
            set_chunk(*chunk, offset);
        }

        template <bool OtherIsConst>
            requires (is_const && !OtherIsConst)
        ArrayIterator(ArrayIterator<T, OtherIsConst> const& other)
            : arena_(other.arena_), array_index_(other.array_index_), elem_index_(other.elem_index_),
              storage_(other.storage_), cursor_(other.cursor_), remaining_(other.remaining_), next_(other.next_)
        {
        }

        reference operator*() const noexcept
        {
            assert(remaining_ != 0);
            if constexpr (std::is_lvalue_reference_v<reference>) {
                return *cursor_;
            } else {
                return (*storage_)[cursor_];
            }
        }

        pointer operator->() const noexcept
            requires std::is_lvalue_reference_v<reference>
        {
            return std::addressof(**this);
        }

        ArrayIterator& operator++()
        {
            assert(remaining_ != 0);
            ++elem_index_;
            if (--remaining_ != 0) {
                ++cursor_;
            } else if (next_ != InvalidArrayIndex) {
                // Follow each link once, never restart from the array's head.
                set_chunk(arena_->continuations_[next_]);
            }
            return *this;
        }

        ArrayIterator operator++(int)
        {
            auto previous = *this;
            ++*this;
            return previous;
        }

        template <bool OtherIsConst>
        bool operator==(ArrayIterator<T, OtherIsConst> const& other) const noexcept
        {
            return arena_ == other.arena_ &&
                array_index_ == other.array_index_ &&
                elem_index_ == other.elem_index_;
        }

    private:
        void set_cursor(Storage& storage, size_t index)
        {
            storage_ = &storage;
            if constexpr (std::is_lvalue_reference_v<reference>) {
                // The column iterator caches the current page and local pointer.
                cursor_ = storage.begin() + static_cast<difference_type>(index);
            } else {
                // Split proxies use O(1) indexing of their two child columns;
                // advancing the split column's forward iterator to index is linear.
                cursor_ = index;
            }
        }

        void set_chunk(Chunk const& chunk, size_t offset = 0)
        {
            set_cursor(arena_->data_, static_cast<size_t>(chunk.offset) + offset);
            remaining_ = std::min(chunk.size, chunk.capacity) - offset;
            next_ = chunk.next;
        }

        Arena* arena_ = nullptr;
        ArrayIndex array_index_ = InvalidArrayIndex;
        size_t elem_index_ = 0;
        Storage* storage_ = nullptr;
        std::conditional_t<std::is_lvalue_reference_v<reference>, ColumnIterator, size_t> cursor_{};
        size_t remaining_ = 0;
        ArrayIndex next_ = InvalidArrayIndex;
    };

    /**
     * Allocation-free, sized forward range borrowing an array (or iterator slice).
     * Constness follows the arena, not the range object, as with std::span.
     * Size is a snapshot; structural arena mutations invalidate the range.
     * Indexing/back walk chunks and are not constant-time random access.
     */
    template <bool T_IsConst>
    class BasicArrayRange
    {
        using Arena = std::conditional_t<T_IsConst, const ArrayArena, ArrayArena>;
        friend class ArrayArena;
        template <bool>
        friend class BasicArrayRange;

    public:
        using element_iterator = std::conditional_t<T_IsConst, const_iterator, iterator>;
        using value_type = ElementType;
        using reference = typename element_iterator::reference;
        using size_type = size_t;
        using checked_result = tl::expected<std::conditional_t<T_IsConst, ConstAtValue, AtValue>, Error>;

        BasicArrayRange() = default;

        BasicArrayRange(element_iterator begin, element_iterator end)
            : begin_(begin), size_(end.elem_index_ - begin.elem_index_)
        {
            assert(begin.arena_ == end.arena_ && begin.array_index_ == end.array_index_);
            assert(begin.elem_index_ <= end.elem_index_);
        }

        template <bool OtherIsConst>
            requires (T_IsConst && !OtherIsConst)
        BasicArrayRange(BasicArrayRange<OtherIsConst> const& other)
            : begin_(other.begin_), size_(other.size_)
        {
        }

        element_iterator begin() const
        {
            // Preserve the starting cursor so even a slice beginning in a late
            // chunk has constant-time begin(), as required of forward ranges.
            return begin_;
        }

        element_iterator end() const
        {
            // End carries identity/position only; it must not walk the chunks.
            element_iterator result;
            result.arena_ = begin_.arena_;
            result.array_index_ = begin_.array_index_;
            result.elem_index_ = begin_.elem_index_ + size_;
            return result;
        }

        [[nodiscard]] size_type size() const noexcept { return size_; }
        [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

        checked_result at(size_type i) const
        {
            if (i >= size_) {
                return tl::unexpected<Error>(Error::IndexOutOfRange, "index out of range");
            }
            return begin_.arena_->at(begin_.array_index_, begin_.elem_index_ + i);
        }

        reference operator[](size_type i) const
        {
            assert(i < size_);
            if (i == 0) {
                return *begin_;
            }
            return *element_iterator(*begin_.arena_, begin_.array_index_, begin_.elem_index_ + i);
        }

        reference front() const { return (*this)[0]; }
        reference back() const { return (*this)[size_ - 1]; }

    private:
        BasicArrayRange(Arena& arena, ArrayIndex array)
            : begin_(arena, array, 0), size_(arena.size(array))
        {
        }

        element_iterator begin_;
        size_type size_ = 0;
    };

    template <bool T_IsConst>
    class BasicArrayArenaIterator
    {
    public:
        using ArrayArenaRef = std::conditional_t<T_IsConst, const ArrayArena&, ArrayArena&>;
        using element_iterator = std::conditional_t<T_IsConst, const_iterator, iterator>;
        using value_type = BasicArrayRange<T_IsConst>;
        using difference_type = std::ptrdiff_t;
        using pointer = void;
        using reference = value_type;
        using iterator_category = std::input_iterator_tag;

        BasicArrayArenaIterator(ArrayArenaRef arena, size_t ordinal)
            : arena_(arena),
              ordinal_(ordinal)
        {
            update_array_index();
        }

        element_iterator begin() const { return arena_.begin(index_); }
        element_iterator end() const { return arena_.end(index_); }

        value_type operator*() const
        {
            return arena_.range(index_);
        }

        BasicArrayArenaIterator& operator++() {
            ++ordinal_;
            update_array_index();
            return *this;
        }

        bool operator==(const BasicArrayArenaIterator& other) const {
            return &arena_ == &other.arena_ && ordinal_ == other.ordinal_;
        }

        bool operator!=(const BasicArrayArenaIterator& other) const {
            return !(*this == other);  // NOLINT
        }

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

            if (auto const singletonOrdinal = ordinal_ - regularCount;
                ordinal_ < total_visible_array_count() &&
                singletonOrdinal <= SingletonArrayHandlePayloadMask) {
                index_ = SingletonArrayHandleMask | to_array_index(singletonOrdinal);
                return;
            }

            index_ = InvalidArrayIndex;
        }

        ArrayArenaRef arena_;
        size_t ordinal_ = 0;
        ArrayIndex index_;
    };

    iterator begin(ArrayIndex a) { return iterator(*this, a, 0); }
    iterator end(ArrayIndex a) { return iterator(*this, a, size(a)); }
    const_iterator begin(ArrayIndex a) const { return const_iterator(*this, a, 0); }
    const_iterator end(ArrayIndex a) const { return const_iterator(*this, a, size(a)); }

    arena_iterator begin() { return arena_iterator(*this, 0); }
    arena_iterator end()
    {
        const auto regularCount = size();
        const auto visibleRegularCount = regularCount > FirstRegularArrayIndex
            ? regularCount - FirstRegularArrayIndex
            : 0;
        return arena_iterator(*this, visibleRegularCount + singleton_handle_count());
    }

    const_arena_iterator begin() const
    {
        return const_arena_iterator(*this, 0);
    }

    const_arena_iterator end() const
    {
        const auto regularCount = size();
        const auto visibleRegularCount = regularCount > FirstRegularArrayIndex
            ? regularCount - FirstRegularArrayIndex
            : 0;
        return const_arena_iterator(
            *this,
            visibleRegularCount + singleton_handle_count());
    }

    BasicArrayRange<false> range(ArrayIndex array) { return BasicArrayRange<false>(*this, array); }
    BasicArrayRange<true> range(ArrayIndex array) const { return BasicArrayRange<true>(*this, array); }

    /**
     * Synchronously visit values, optionally with their zero-based index.
     * A bool-returning callback accepting only (value) stops on false; return
     * values of (value, index) callbacks are ignored. No lock spans traversal:
     * callbacks must not structurally mutate the arena.
     */
    template <typename Func>
    void iterate(ArrayIndex a, Func&& lambda)
    {
        if (is_singleton_handle(a)) {
            iterate_singleton(a, std::forward<Func>(lambda));
            return;
        }

        if (heads_.empty() && compactHeads_) {
            iterate_compact(a, std::forward<Func>(lambda));
            return;
        }

        iterate_chunked(a, std::forward<Func>(lambda));
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
    DataStorage data_;  // Underlying element storage.
    DataStorage singletonValues_;
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

    template <typename Func, typename Value>
    static bool invoke_iter_callback(Func&& lambda, Value&& value, size_t index)
    {
        using Arg = decltype(value);
        if constexpr (std::is_invocable_r_v<bool, Func, Arg>) {
            return lambda(std::forward<Value>(value));
        } else if constexpr (std::is_invocable_v<Func, Arg, size_t>) {
            lambda(std::forward<Value>(value), index);
            return true;
        } else if constexpr (std::is_invocable_v<Func, Arg>) {
            lambda(std::forward<Value>(value));
            return true;
        } else {
            static_assert(
                std::is_invocable_v<Func, Arg>,
                "ArrayArena::iterate callback must accept (value) or (value, index), optionally returning bool");
            return false;
        }
    }

    template <typename Func>
    void iterate_singleton(ArrayIndex a, Func&& lambda)
    {
        auto singletonIndex = singleton_payload(a);
        if (singletonIndex >= singletonValues_.size() ||
            singletonIndex >= singletonOccupied_.size()) {
            raise<std::out_of_range>("ArrayArena singleton handle index out of range.");
        }
        if (singletonOccupied_.at(singletonIndex) == 0) {
            return;
        }

        decltype(auto) value = singletonValues_.at(singletonIndex);
        invoke_iter_callback(lambda, value, 0);
    }

    template <typename Func>
    void iterate_compact(ArrayIndex a, Func&& lambda)
    {
        if (a >= compactHeads_->size()) {
            raise<std::out_of_range>("ArrayArena head index out of range.");
        }

        auto const& compact = (*compactHeads_)[a];
        for (size_t i = 0; i < static_cast<size_t>(compact.size); ++i)
        {
            decltype(auto) value = data_[static_cast<size_t>(compact.offset) + i];
            if (!invoke_iter_callback(lambda, value, i)) {
                return;
            }
        }
    }

    template <typename Func>
    void iterate_chunked(ArrayIndex a, Func&& lambda)
    {
        if (a >= heads_.size()) {
            raise<std::out_of_range>("ArrayArena head index out of range.");
        }

        Chunk const* current = &heads_[a];
        size_t globalIndex = 0;
        while (current != nullptr)
        {
            for (size_t i = 0; i < current->size && i < current->capacity; ++i)
            {
                decltype(auto) value = data_[current->offset + i];
                if (!invoke_iter_callback(lambda, value, globalIndex)) {
                    return;
                }
                ++globalIndex;
            }
            current = (current->next != InvalidArrayIndex)
                ? &continuations_[current->next]
                : nullptr;
        }
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
    // Caller must hold the write lock when ARRAY_ARENA_THREAD_SAFE is enabled.
    Chunk& ensure_capacity_and_get_last_chunk_unlocked(ArrayIndex a)
    {
        if (is_singleton_handle(a)) {
            raise<std::runtime_error>("Singleton handles do not use chunk growth.");
        }

        ensure_runtime_heads_from_compact();
        ensure_regular_head_pool();
        if (a >= heads_.size()) {
            raise<std::out_of_range>("ArrayArena head index out of range.");
        }
        Chunk& head = heads_[a];
        Chunk& last = (head.last == InvalidArrayIndex) ? head : continuations_[head.last];
        if (last.size < last.capacity)
            return last;
        size_t offset = data_.size();
        assert(static_cast<size_t>(last.capacity) <= std::numeric_limits<SizeType_>::max() / 2);
        size_t newCapacity = std::max<size_t>(2, static_cast<size_t>(last.capacity) * 2);
        assert(check_data_growth(offset, newCapacity));
        auto const newIndex = to_array_index(continuations_.size());
        if (head.capacity != 0)
            assert(continuations_.check_size(continuations_.size() + 1));
        data_.resize(offset + newCapacity);
        if (head.capacity == 0) {
            head.offset = (SizeType_)offset;
            head.capacity = static_cast<SizeType_>(newCapacity);
            return head;
        }
        try {
            continuations_.push_back({(SizeType_)offset, (SizeType_)newCapacity, 0, InvalidArrayIndex, InvalidArrayIndex});
        } catch (...) {
            data_.resize(offset);
            throw;
        }
        // Appending a continuation can invalidate vector-backed references.
        (head.last == InvalidArrayIndex ? head : continuations_[head.last]).next = newIndex;
        head.last = newIndex;
        return continuations_[newIndex];
    }

    template <typename Self, typename AccessType>
    static tl::expected<AccessType, Error>
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
            return detail::arena_access_wrap(self.singletonValues_.at(singletonIndex));
        }

        if (self.heads_.empty() && self.compactHeads_) {
            if (a >= self.compactHeads_->size()) {
                return tl::unexpected<Error>(Error::IndexOutOfRange, "array index out of range");
            }
            auto const& compact = (*self.compactHeads_)[a];
            if (i < static_cast<size_t>(compact.size)) {
                return detail::arena_access_wrap(self.data_[static_cast<size_t>(compact.offset) + i]);
            }
            return tl::unexpected<Error>(Error::IndexOutOfRange, "index out of range");
        }

        if (a >= self.heads_.size()) {
            return tl::unexpected<Error>(Error::IndexOutOfRange, "array index out of range");
        }

        typename Self::Chunk const* current = &self.heads_[a];
        size_t remaining = i;
        while (true) {
            if (remaining < current->capacity && remaining < current->size) {
                return detail::arena_access_wrap(self.data_[current->offset + remaining]);
            }
            if (current->next == InvalidArrayIndex)
                return tl::unexpected<Error>(Error::IndexOutOfRange, "index out of range");
            remaining -= current->capacity;
            current = &self.continuations_[current->next];
        }
    }
};

} // namespace simfil
