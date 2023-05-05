#pragma once

#include <unordered_map>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <optional>
#include <string_view>
#include <string>

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
        Lon,
        Lat,
        OverlaySum,
        OverlayValue,
        OverlayIndex,
        Geometry,
        Geometries,
        Type,
        Coordinates,
        Elevation,

        NextStaticId,
        FirstDynamicId = 128
    };

public:
    /// Default constructor initializes strings for static Ids
    Fields();

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
    virtual std::optional<std::string_view> resolve(FieldId const& id);

    /// Get stats
    size_t size();
    size_t bytes();
    size_t hits();
    size_t misses();

    /// Add a static key-string mapping - Warning: Not thread-safe.
    void addStaticKey(FieldId k, std::string const& v);

private:
    std::shared_mutex stringStoreMutex_;
    std::unordered_map<std::string, FieldId> idForString_;
    std::unordered_map<FieldId , std::string> stringForId_;
    FieldId nextId_ = FirstDynamicId;
    std::atomic_int64_t byteSize_;
    std::atomic_int64_t cacheHits_;
    std::atomic_int64_t cacheMisses_;
};

}
