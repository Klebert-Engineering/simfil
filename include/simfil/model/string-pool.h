#pragma once

#include <cstddef>
#include <type_traits>
#include <unordered_map>
#include <shared_mutex>
#include <atomic>
#include <optional>
#include <string_view>
#include <string>
#include <ostream>
#include <vector>
#include <deque>
#include <tl/expected.hpp>

#include "simfil/error.h"
#include "simfil/model/memory.h"

namespace simfil
{

class SchemaPool;

using StringId = uint16_t;
static_assert(std::is_unsigned_v<StringId>, "StringId must be unsigned!");

/**
 * Case-sensitive string interner for field names and scalar strings. IDs belong
 * to this dictionary; the same spelling can have another ID in a different pool.
 * Ordinary lookup/insertion locks internally. Borrowed views remain valid while
 * the pool lives, but iterating strings() requires a stable dictionary.
 */
struct StringPool
{
    enum StaticStringIds : StringId {
        Empty = 0,
        OverlaySum,
        OverlayValue,
        OverlayIndex,

        SchemaUnknown,
        SchemaAny,
        SchemaNever,
        SchemaValue,
        SchemaNull,
        SchemaBool,
        SchemaInt,
        SchemaFloat,
        SchemaString,
        SchemaBytes,
        SchemaBitmask,
        SchemaObject,
        SchemaArray,
        SchemaUnion,
        SchemaOneOf,
        SchemaIntersection,

        SchemaKind,
        SchemaTypeName,
        SchemaFields,
        SchemaElements,
        SchemaAlternatives,
        SchemaEnum,
        SchemaRequired,
        SchemaOpen,
        SchemaNullable,
        SchemaRef,
        SchemaTruncated,
        SchemaNamedValues,
        SchemaNumericDomain,
        SchemaBitWidth,
        SchemaName,
        SchemaRole,
        SchemaMeaning,
        SchemaDeprecated,
        SchemaRemoved,
        SchemaEncoding,
        SchemaUnit,
        SchemaMinimum,
        SchemaMaximum,
        SchemaOrdinaryRanges,
        SchemaMin,
        SchemaMax,
        SchemaScale,
        SchemaOffset,
        SchemaComplete,
        SchemaRawValue,
        SchemaMultimap,

        NextStaticId,
        FirstDynamicId = 256
    };

    /// Default constructor initializes strings for static Ids
    StringPool();

    /// Copy constructor
    StringPool(StringPool const& other);

    /// Virtual destructor to allow polymorphism
    virtual ~StringPool() = default;

    /** Seal the dictionary before sharing immutable schema metadata; existing lookups remain valid. */
    void freeze();

    /** A copied dictionary is mutable, even when its source has been frozen. */
    auto frozen() const -> bool;

    /// Use this function to lookup a stored string, or insert it
    /// if it doesn't exist yet.
    auto emplace(std::string_view const& str) -> tl::expected<StringId, Error>;

    /// Returns the ID of the given string, or `Empty` if
    /// no such string was ever inserted.
    StringId get(std::string_view const& str) const;

    /// Get the actual string for the given ID, or
    ///  nullopt if the given ID is invalid.
    /// Virtual to allow defining an inherited StringPool which understands
    /// additional StaticStringIds.
    virtual std::optional<std::string_view> resolve(StringId const& id) const;

    /// Return the highest allocated dynamic ID, or FirstDynamicId - 1 before allocation.
    /// The caller must exclude concurrent insertion while reading this counter.
    StringId highest() const;

    /// Get stats
    size_t size() const;
    size_t bytes() const;
    size_t hits() const;
    size_t misses() const;

    /** Return interned string payload and estimated retained container capacity. */
    [[nodiscard]] MemoryUsage memoryUsage() const;

    /// Register a unique static ID/spelling during construction. This locks storage
    /// but does not validate that the caller's ID and spelling are both unused.
    void addStaticKey(StringId id, std::string const& value);

    /// Write ID/string pairs at or above the inclusive StringId offset for incremental transport.
    virtual auto write(std::ostream& outputStream, StringId offset = {}) const -> tl::expected<void, Error>;  // NOLINT

    /**
     * Merge a complete serialized update starting at a byte offset in input.
     * Malformed/truncated framing is rejected before publishing entries. Existing
     * spellings keep their local IDs, so the sender must share this ID namespace.
     */
    virtual auto read(const std::vector<uint8_t>& input, size_t offset = 0) -> tl::expected<void, Error>;

    /// Check if the content of the string pools is logically identical.
    bool operator== (StringPool const& other) const;

    /// Borrow stored strings; hold the pool alive and exclude mutation throughout iteration.
    const std::deque<std::string>& strings() const;

private:
    friend class SchemaPool;

    /** Logical dictionary tails for SchemaPool's exclusive dynamic-string appends. */
    struct AppendCheckpoint
    {
        StringPool const* owner;
        std::uint32_t nextId;
        size_t storedStrings;
        int64_t byteSize;
    };

    /** No concurrent access, static-key insertion, read, or freeze while active. */
    [[nodiscard]] AppendCheckpoint appendCheckpoint() const;

    /** Erase both indexes before destroying their string views, including partial inserts. */
    void rollbackAppends(AppendCheckpoint const& checkpoint) noexcept;

    bool frozen_ = false;
    mutable std::shared_mutex stringStoreMutex_;
    std::unordered_map<
        std::string_view,
        StringId>
        idForString_;
    std::unordered_map<StringId, std::string_view> stringForId_;
    std::deque<std::string> storedStrings_;
    // The counter must represent one past the final valid id without wrapping.
    std::uint32_t nextId_ = FirstDynamicId;
    std::atomic_int64_t byteSize_{0};
    mutable std::atomic_int64_t cacheHits_{0};
    mutable std::atomic_int64_t cacheMisses_{0};
};

}  // namespace simfil
