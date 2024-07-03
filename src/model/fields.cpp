#include "simfil/model/fields.h"
#include "simfil/exception-handler.h"

#include <algorithm>
#include <bitsery/bitsery.h>
#include <bitsery/adapter/stream.h>
#include <bitsery/traits/string.h>
#include <fmt/core.h>
#include <cmath>

namespace simfil
{

Fields::Fields()
{
    addStaticKey(Empty, "");
    addStaticKey(OverlaySum, "$sum");
    addStaticKey(OverlayValue, "$val");
    addStaticKey(OverlayIndex, "$idx");
}

Fields::Fields(const Fields& other) :
    idForString_(other.idForString_),
    stringForId_(other.stringForId_),
    nextId_(other.nextId_),
    byteSize_(other.byteSize_.load()),
    cacheHits_(other.cacheHits_.load()),
    cacheMisses_(other.cacheMisses_.load())
{
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
            stringForId_.emplace(nextId_, str);
            byteSize_ += (int64_t)str.size();
            ++cacheMisses_;
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

std::optional<std::string_view> Fields::resolve(FieldId const& id) const
{
    std::shared_lock stringStoreReadAccess_(stringStoreMutex_);
    auto it = stringForId_.find(id);
    if (it != stringForId_.end())
        return it->second;

    return std::nullopt;
}

FieldId Fields::highest() const {
    return nextId_ - 1;
}

size_t Fields::size() const
{
    std::shared_lock stringStoreReadAccess_(stringStoreMutex_);
    return idForString_.size();
}

size_t Fields::bytes() const
{
    return byteSize_;
}

size_t Fields::hits() const
{
    return cacheHits_;
}

size_t Fields::misses() const
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

void Fields::write(std::ostream& outputStream, FieldId offset) const  // NOLINT
{
    std::shared_lock stringStoreReadAccess_(stringStoreMutex_);
    bitsery::Serializer<bitsery::OutputStreamAdapter> s(outputStream);

    // Calculate how many fields will be sent
    FieldId sendFieldCount = 0;
    for (FieldId fieldId = offset; fieldId <= highest(); ++fieldId) {
        auto it = stringForId_.find(fieldId);
        if (it != stringForId_.end())
            ++sendFieldCount;
    }
    s.value2b(sendFieldCount);

    // Send the field key-name pairs
    for (FieldId fieldId = offset; fieldId <= highest(); ++fieldId) {
        auto it = stringForId_.find(fieldId);
        if (it != stringForId_.end()) {
            s.value2b(fieldId);
            // Don't support field names longer than 64kB.
            s.text1b(it->second, std::numeric_limits<uint16_t>::max());
        }
    }
}

void Fields::read(std::istream& inputStream)
{
    std::unique_lock stringStoreWriteAccess_(stringStoreMutex_);
    bitsery::Deserializer<bitsery::InputStreamAdapter> s(inputStream);

    // Determine how many fields are to be received
    FieldId rcvFieldCount{};
    s.value2b(rcvFieldCount);

    // Read fields
    for (auto i = 0; i < rcvFieldCount; ++i)
    {
        // Read field key
        FieldId fieldId{};
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
            [](auto ch) { return std::tolower(ch); });
        auto [it, insertionTookPlace] = idForString_.emplace(lowerCaseFieldName, fieldId);
        if (insertionTookPlace) {
            stringForId_.emplace(fieldId, fieldName);
            byteSize_ += (int64_t)fieldName.size();
            nextId_ = std::max((FieldId)nextId_, (FieldId)(fieldId + 1));
        }
    }

    if (s.adapter().error() != bitsery::ReaderError::NoError) {
        raise<std::runtime_error>(fmt::format(
            "Failed to read Fields: Error {}",
            static_cast<std::underlying_type_t<bitsery::ReaderError>>(s.adapter().error())));
    }
}

}
