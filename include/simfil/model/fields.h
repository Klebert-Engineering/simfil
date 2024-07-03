#pragma once

#include <unordered_map>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <optional>
#include <string_view>
#include <string>
#include <istream>
#include <ostream>

namespace simfil
{

using FieldId = uint16_t;

/**
 * Fast and efficient case-insensitive field name storage -
 *  used to store object keys.
 */
struct Fields
{
    enum StaticFieldIds: FieldId {
        Empty = 0,
        OverlaySum,
        OverlayValue,
        OverlayIndex,

        NextStaticId,
        FirstDynamicId = 128
    };

    /// Default constructor initializes strings for static Ids
    Fields();

    /// Copy constructor
    Fields(Fields const&);

    /// Virtual destructor to allow polymorphism
    virtual ~Fields() = default;

    /// Use this function to lookup a stored string, or insert it
    /// if it doesn't exist yet.
    FieldId emplace(std::string_view const& str);

    /// Returns the ID of the given string, or `Empty` if
    /// no such string was ever inserted.
    FieldId get(std::string_view const& str);

    /// Get the actual string for the given ID, or
    ///  nullopt if the given ID is invalid.
    /// Virtual to allow defining an inherited StringPool which understands
    /// additional StaticFieldIds.
    virtual std::optional<std::string_view> resolve(FieldId const& id) const;

    /// Get highest stored field id
    FieldId highest() const;

    /// Get stats
    size_t size() const;
    size_t bytes() const;
    size_t hits() const;
    size_t misses() const;

    /// Add a static key-string mapping - Warning: Not thread-safe.
    void addStaticKey(FieldId k, std::string const& v);

    /// Serialization - write to stream, starting from a specific
    ///  id offset if necessary (for partial serialisation).
    virtual void write(std::ostream& outputStream, FieldId offset=0) const; // NOLINT
    virtual void read(std::istream& inputStream);

private:
    mutable std::shared_mutex stringStoreMutex_;
    std::unordered_map<std::string, FieldId> idForString_;
    std::unordered_map<FieldId, std::string> stringForId_;
    FieldId nextId_ = FirstDynamicId;
    std::atomic_int64_t byteSize_{0};
    std::atomic_int64_t cacheHits_{0};
    std::atomic_int64_t cacheMisses_{0};
};

}
