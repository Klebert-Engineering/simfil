#include "simfil/model/fields.h"

#include <algorithm>

namespace simfil
{

/** String Pool implementation */

Fields::Fields() {
    addStaticKey(Empty, "");
    addStaticKey(Lon, "lon");
    addStaticKey(Lat, "lat");
    addStaticKey(OverlaySum, "$sum");
    addStaticKey(OverlayValue, "$val");
    addStaticKey(OverlayIndex, "$idx");
    addStaticKey(Geometry, "geometry");
    addStaticKey(Geometries, "geometries");
    addStaticKey(Type, "type");
    addStaticKey(Coordinates, "coordinates");
    addStaticKey(Elevation, "elevation");
}

FieldId Fields::emplace(std::string_view const& str)
{
    /// Unfortunately, we have to create a copy of the string here
    /// on the heap for lower-casing.
    /// Also we must use std::string as lookup type until C++ 20 is used:
    ///   http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2018/p0919r2.html
    auto lowerCaseStr = std::string(str);
    std::transform(
        lowerCaseStr.begin(),
        lowerCaseStr.end(),
        lowerCaseStr.begin(),
        [](auto ch) { return std::tolower(ch); });

    {
        std::shared_lock stringStoreReadAccess_(stringStoreMutex_);
        auto it = idForString_.find(lowerCaseStr);
        if (it != idForString_.end()) {
            ++cacheHits_;
            return it->second;
        }
    }
    {
        std::unique_lock stringStoreWriteAccess_(stringStoreMutex_);
        auto [it, insertionTookPlace] = idForString_.emplace(lowerCaseStr, nextId_);
        if (insertionTookPlace) {
            ++cacheMisses_;
            byteSize_ += (int64_t)str.size();
            stringForId_[nextId_] = str;
            ++nextId_;
        }
        return it->second;
    }
}

FieldId Fields::get(std::string_view const& str)
{
    auto lowerCaseStr = std::string(str);
    std::transform(
        lowerCaseStr.begin(),
        lowerCaseStr.end(),
        lowerCaseStr.begin(),
        [](auto ch) { return std::tolower(ch); });

    std::shared_lock stringStoreReadAccess_(stringStoreMutex_);
    auto it = idForString_.find(lowerCaseStr);
    if (it != idForString_.end())
        return it->second;

    return Fields::Empty;
}

std::optional<std::string_view> Fields::resolve(FieldId const& id)
{
    std::shared_lock stringStoreReadAccess_(stringStoreMutex_);
    auto it = stringForId_.find(id);
    if (it != stringForId_.end())
        return it->second;

    return std::nullopt;
}

size_t Fields::size()
{
    std::shared_lock stringStoreReadAccess_(stringStoreMutex_);
    return idForString_.size();
}

size_t Fields::bytes()
{
    return byteSize_;
}

size_t Fields::hits()
{
    return cacheHits_;
}

size_t Fields::misses()
{
    return cacheMisses_;
}

void Fields::addStaticKey(FieldId k, std::string const& v) {
    auto lowerCaseStr = v;
    std::transform(
        lowerCaseStr.begin(),
        lowerCaseStr.end(),
        lowerCaseStr.begin(),
        [](auto ch) { return std::tolower(ch); });

    idForString_[lowerCaseStr] = k;
    stringForId_[k] = v;
}

}
