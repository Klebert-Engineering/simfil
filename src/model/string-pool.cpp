#include "simfil/model/string-pool.h"
#include "simfil/exception-handler.h"

#include <algorithm>
#include <bitsery/bitsery.h>
#include <bitsery/adapter/stream.h>
#include <bitsery/traits/string.h>
#include <fmt/core.h>
#include <cmath>
#include <mutex>
#include <locale>

namespace simfil
{

StringPool::StringPool()
{
    addStaticKey(Empty, "");
    addStaticKey(OverlaySum, "$sum");
    addStaticKey(OverlayValue, "$val");
    addStaticKey(OverlayIndex, "$idx");
}

StringPool::StringPool(const StringPool& other) :
    idForString_(other.idForString_),
    stringForId_(other.stringForId_),
    nextId_(other.nextId_),
    byteSize_(other.byteSize_.load()),
    cacheHits_(other.cacheHits_.load()),
    cacheMisses_(other.cacheMisses_.load())
{
}

StringId StringPool::emplace(std::string_view const& str)
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
        [](auto ch) { return std::tolower(ch, std::locale{}); });

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
        auto [it, insertionTookPlace] = idForString_.try_emplace(lowerCaseStr, nextId_);
        if (insertionTookPlace) {
            (void)stringForId_.try_emplace(nextId_, str);
            byteSize_ += static_cast<int64_t>(str.size());
            ++cacheMisses_;
            ++nextId_;
        }
        return it->second;
    }
}

StringId StringPool::get(std::string_view const& str)
{
    auto lowerCaseStr = std::string(str);
    std::transform(
        lowerCaseStr.begin(),
        lowerCaseStr.end(),
        lowerCaseStr.begin(),
        [](auto ch) { return std::tolower(ch, std::locale{}); });

    std::shared_lock stringStoreReadAccess_(stringStoreMutex_);
    auto it = idForString_.find(lowerCaseStr);
    if (it != idForString_.end())
        return it->second;

    return StringPool::Empty;
}

std::optional<std::string_view> StringPool::resolve(const StringId& id) const
{
    std::shared_lock stringStoreReadAccess_(stringStoreMutex_);
    const auto it = stringForId_.find(id);
    if (it != stringForId_.end())
        return it->second;

    return std::nullopt;
}

StringId StringPool::highest() const {
    return nextId_ - 1;
}

size_t StringPool::size() const
{
    std::shared_lock stringStoreReadAccess_(stringStoreMutex_);
    return idForString_.size();
}

size_t StringPool::bytes() const
{
    return byteSize_;
}

size_t StringPool::hits() const
{
    return cacheHits_;
}

size_t StringPool::misses() const
{
    return cacheMisses_;
}

void StringPool::addStaticKey(StringId k, std::string const& v) {
    auto lowerCaseStr = v;
    std::transform(
        lowerCaseStr.begin(),
        lowerCaseStr.end(),
        lowerCaseStr.begin(),
        [](auto ch) { return std::tolower(ch, std::locale{}); });

    idForString_[lowerCaseStr] = k;
    stringForId_[k] = v;
}

void StringPool::write(std::ostream& outputStream, const StringId offset) const // NOLINT
{
    std::shared_lock stringStoreReadAccess_(stringStoreMutex_);
    bitsery::Serializer<bitsery::OutputStreamAdapter> s(outputStream);

    // Calculate how many fields will be sent
    StringId sendStrCount = 0;
    const auto high = highest();
    for (auto strId = offset; strId <= high; ++strId) {
        auto it = stringForId_.find(strId);
        if (it != stringForId_.end())
            ++sendStrCount;
    }
    s.value2b(sendStrCount);

    // Send the field key-name pairs
    for (auto strId = offset; strId <= high; ++strId) {
        auto it = stringForId_.find(strId);
        if (it != stringForId_.end()) {
            s.value2b(strId);
            // Don't support field names longer than 64kB.
            s.text1b(it->second, std::numeric_limits<uint16_t>::max());
        }
    }
}

void StringPool::read(std::istream& inputStream)
{
    std::unique_lock stringStoreWriteAccess_(stringStoreMutex_);
    bitsery::Deserializer<bitsery::InputStreamAdapter> s(inputStream);

    // Determine how many fields are to be received
    StringId rcvFieldCount{};
    s.value2b(rcvFieldCount);

    // Read fields
    for (auto i = 0; i < rcvFieldCount; ++i)
    {
        // Read field key
        StringId fieldId{};
        s.value2b(fieldId);

        // Don't support field names longer than 64kB.
        std::string fieldName;
        s.text1b(fieldName, std::numeric_limits<uint16_t>::max());
        auto lowerCaseFieldName = std::string(fieldName);

        // Insert field name into pool
        std::transform(
            lowerCaseFieldName.begin(),
            lowerCaseFieldName.end(),
            lowerCaseFieldName.begin(),
            [](auto ch) { return std::tolower(ch, std::locale{}); });
        auto [it, insertionTookPlace] = idForString_.try_emplace(lowerCaseFieldName, fieldId);
        if (insertionTookPlace) {
            stringForId_.try_emplace(fieldId, fieldName);
            byteSize_ += static_cast<int64_t>(fieldName.size());
            nextId_ = std::max<StringId>(nextId_, fieldId + 1);
        }
    }

    if (s.adapter().error() != bitsery::ReaderError::NoError) {
        raise<std::runtime_error>(fmt::format(
            "Failed to read StringPool: Error {}",
            static_cast<std::underlying_type_t<bitsery::ReaderError>>(s.adapter().error())));
    }
}

}
