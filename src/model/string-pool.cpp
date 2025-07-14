#include "simfil/model/string-pool.h"
#include "simfil/exception-handler.h"

#include <bitsery/adapter/stream.h>
#include <bitsery/bitsery.h>
#include <bitsery/traits/string.h>
#include <fmt/core.h>
#include <algorithm>
#include <cmath>
#include <mutex>

/**
 * Note: This code is taken from bitsery traits/string.h and adopted
 * to handle (de-)serialization of a string view.
 */
namespace bitsery
{
namespace traits
{
template<typename CharT, typename Traits>
struct ContainerTraits<std::basic_string_view<CharT, Traits>>
    : public StdContainer<std::basic_string_view<CharT, Traits>, true, true>
{
};

template<typename CharT, typename Traits>
struct TextTraits<std::basic_string_view<CharT, Traits>>
{
    using TValue = typename ContainerTraits<
        std::basic_string_view<CharT, Traits>>::TValue;
    static constexpr bool addNUL = false;
    static size_t length(const std::basic_string_view<CharT, Traits>& str)
    {
        return str.size();
    }
};
}
}

namespace simfil
{

StringPool::StringPool()
{
    addStaticKey(Empty, "");
    addStaticKey(OverlaySum, "$sum");
    addStaticKey(OverlayValue, "$val");
    addStaticKey(OverlayIndex, "$idx");
}

StringPool::StringPool(const StringPool& other)
{
    std::unique_lock lockThis(stringStoreMutex_, std::defer_lock);
    std::shared_lock lockOther(other.stringStoreMutex_, std::defer_lock);
    std::lock(lockThis, lockOther);

    // Copy storedStrings_.
    storedStrings_ = other.storedStrings_;

    // Map from old string data pointer to new string_view.
    std::unordered_map<const char*, std::string_view> strDataToNewStrView;

    // Build the mapping from old string data pointers to new string_views.
    for (size_t i = 0; i < other.storedStrings_.size(); ++i) {
        strDataToNewStrView[other.storedStrings_[i].data()] = storedStrings_[i];
    }

    // Rebuild idForString_ with new string_views pointing into this->storedStrings_.
    idForString_.clear();
    for (const auto& [oldStrView, id] : other.idForString_) {
        // Get the new string_view corresponding to the old string data pointer.
        auto it = strDataToNewStrView.find(oldStrView.data());
        if (it != strDataToNewStrView.end()) {
            idForString_.emplace(it->second, id);
        }
        else {
            // This should not happen if everything is consistent.
            raise<std::runtime_error>("Failed to rebuild idForString_ in StringPool copy constructor");
        }
    }

    // Copy stringForId_.
    stringForId_ = other.stringForId_;

    // Copy other member variables.
    nextId_ = other.nextId_;
    byteSize_ = other.byteSize_.load();
    cacheHits_ = other.cacheHits_.load();
    cacheMisses_ = other.cacheMisses_.load();
}

StringId StringPool::emplace(std::string_view const& str)
{
    {
        std::shared_lock lock(stringStoreMutex_);
        auto it = idForString_.find(str);
        if (it != idForString_.end()) {
            ++cacheHits_;
            return it->second;
        }
    }
    {
        std::unique_lock lock(stringStoreMutex_);
        // Double-check in case another thread added the string.
        auto it = idForString_.find(str);
        if (it != idForString_.end()) {
            ++cacheHits_;
            return it->second;
        }

        // Store the string to maintain ownership.
        auto& storedString = storedStrings_.emplace_back(str);
        StringId id = nextId_++;
        if (nextId_ < id) {
            raise<std::overflow_error>("StringPool id overflow!");
        }
        idForString_.emplace(storedString, id);
        stringForId_.emplace(id, storedString);
        byteSize_ += static_cast<int64_t>(storedString.size());
        ++cacheMisses_;

        return id;
    }
}

StringId StringPool::get(std::string_view const& str)
{
    std::shared_lock stringStoreReadAccess_(stringStoreMutex_);
    auto it = idForString_.find(str);
    if (it != idForString_.end()) {
        ++cacheHits_;
        return it->second;
    }
    return StringPool::Empty;
}

std::optional<std::string_view> StringPool::resolve(const StringId& id) const
{
    std::shared_lock stringStoreReadAccess_(stringStoreMutex_);
    auto it = stringForId_.find(id);
    if (it != stringForId_.end())
        return it->second;
    return std::nullopt;
}

StringId StringPool::highest() const
{
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

void StringPool::addStaticKey(StringId id, const std::string& value)
{
    std::unique_lock lock(stringStoreMutex_);
    auto& storedString = storedStrings_.emplace_back(value);
    idForString_.emplace(storedString, id);
    stringForId_.emplace(id, storedString);
}

void StringPool::write(std::ostream& outputStream, const StringId offset) const  // NOLINT
{
    std::shared_lock stringStoreReadAccess(stringStoreMutex_);
    bitsery::Serializer<bitsery::OutputStreamAdapter> s(outputStream);

    // Calculate how many strings will be sent
    StringId sendStrCount = 0;
    const auto high = highest();
    for (auto strId = offset; strId <= high; ++strId) {
        auto it = stringForId_.find(strId);
        if (it != stringForId_.end())
            ++sendStrCount;
    }
    s.value2b(sendStrCount);

    // Send the pool's key-string pairs
    for (auto strId = offset; strId <= high; ++strId) {
        auto it = stringForId_.find(strId);
        if (it != stringForId_.end()) {
            s.value2b(strId);
            // Don't support strings longer than 64kB.
            s.text1b(it->second, std::numeric_limits<uint16_t>::max());
        }
    }
}

void StringPool::read(std::istream& inputStream)
{
    std::unique_lock stringStoreWriteAccess_(stringStoreMutex_);
    bitsery::Deserializer<bitsery::InputStreamAdapter> s(inputStream);

    // Determine how many strings are to be received
    StringId rcvStringCount{};
    s.value2b(rcvStringCount);

    // Read strings
    for (auto i = 0; i < rcvStringCount; ++i) {
        // Read string key
        StringId stringId{};
        s.value2b(stringId);

        // Don't support strings longer than 64kB.
        auto& stringValue = storedStrings_.emplace_back();
        s.text1b(stringValue, std::numeric_limits<uint16_t>::max());

        // Insert string into the pool
        auto [it, insertionTookPlace] = idForString_.try_emplace(stringValue, stringId);
        if (insertionTookPlace) {
            stringForId_.try_emplace(stringId, stringValue);
            byteSize_ += static_cast<int64_t>(stringValue.size());
            nextId_ = std::max<StringId>(nextId_, stringId + 1);
        }
    }

    if (s.adapter().error() != bitsery::ReaderError::NoError) {
        raise<std::runtime_error>(fmt::format(
            "Failed to read StringPool: Error {}",
            static_cast<std::underlying_type_t<bitsery::ReaderError>>(s.adapter().error())));
    }
}

bool StringPool::operator==(const StringPool &other) const {
    return idForString_ == other.idForString_;
}

const std::deque<std::string>& StringPool::strings() const {
    return storedStrings_;
}

}
