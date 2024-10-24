#pragma once

#include <cstddef>
#include <type_traits>
#include <unordered_map>
#include <shared_mutex>
#include <atomic>
#include <optional>
#include <string_view>
#include <string>
#include <istream>
#include <ostream>
#include <deque>

namespace simfil
{

using StringId = uint16_t;
static_assert(std::is_unsigned_v<StringId>, "StringId must be unsigned!");

namespace detail
{
// Custom hash function for case-insensitive string hashing.
struct CaseInsensitiveHash
{
    size_t operator()(const std::string_view& str) const;
};

// Custom equality comparison for case-insensitive string comparison.
struct CaseInsensitiveEqual
{
    bool operator()(const std::string_view& lhs, const std::string_view& rhs) const;
};
}  // namespace detail

/**
 * Fast and efficient case-insensitive string interner,
 * used to store object keys.
 */
struct StringPool
{
    enum StaticStringIds : StringId {
        Empty = 0,
        OverlaySum,
        OverlayValue,
        OverlayIndex,

        NextStaticId,
        FirstDynamicId = 128
    };

    /// Default constructor initializes strings for static Ids
    StringPool();

    /// Copy constructor
    StringPool(StringPool const& other);

    /// Virtual destructor to allow polymorphism
    virtual ~StringPool() = default;

    /// Use this function to lookup a stored string, or insert it
    /// if it doesn't exist yet.
    StringId emplace(std::string_view const& str);

    /// Returns the ID of the given string, or `Empty` if
    /// no such string was ever inserted.
    StringId get(std::string_view const& str);

    /// Get the actual string for the given ID, or
    ///  nullopt if the given ID is invalid.
    /// Virtual to allow defining an inherited StringPool which understands
    /// additional StaticStringIds.
    virtual std::optional<std::string_view> resolve(StringId const& id) const;

    /// Get highest stored field id
    StringId highest() const;

    /// Get stats
    size_t size() const;
    size_t bytes() const;
    size_t hits() const;
    size_t misses() const;

    /// Add a static key-string mapping - Warning: Not thread-safe.
    void addStaticKey(StringId k, std::string const& v);

    /// Serialization - write to stream, starting from a specific
    ///  id offset if necessary (for partial serialisation).
    virtual void write(std::ostream& outputStream, StringId offset = {}) const;  // NOLINT
    virtual void read(std::istream& inputStream);

private:
    mutable std::shared_mutex stringStoreMutex_;
    std::unordered_map<
        std::string_view,
        StringId,
        detail::CaseInsensitiveHash,
        detail::CaseInsensitiveEqual>
        idForString_;
    std::unordered_map<StringId, std::string_view> stringForId_;
    std::deque<std::string> storedStrings_;
    StringId nextId_ = FirstDynamicId;
    std::atomic_int64_t byteSize_{0};
    std::atomic_int64_t cacheHits_{0};
    std::atomic_int64_t cacheMisses_{0};
};

}  // namespace simfil
