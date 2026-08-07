#include "simfil/model/string-pool.h"
#include "simfil/exception-handler.h"
#include "simfil/error.h"

#include <bitsery/adapter/buffer.h>
#include <bitsery/adapter/stream.h>
#include <bitsery/bitsery.h>
#include <bitsery/traits/string.h>
#include <bitsery/traits/vector.h>
#include <fmt/core.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
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
    // `this` is not observable while its copy constructor runs, so only the
    // source pool needs synchronization. Locking both rwlocks through
    // std::lock trips Helgrind's rwlock bookkeeping on some CI runners.
    std::shared_lock lockOther(other.stringStoreMutex_);

    // Copy storedStrings_.
    storedStrings_ = other.storedStrings_;

    // Map every string from the source pool to the equivalent view owned by this copy.
    std::unordered_map<std::string_view, std::string_view> copiedViewForSourceView;

    for (size_t i = 0; i < other.storedStrings_.size(); ++i) {
        copiedViewForSourceView.emplace(
            std::string_view(other.storedStrings_[i]),
            std::string_view(storedStrings_[i]));
    }

    auto copiedViewFor = [&](std::string_view oldStrView) -> std::string_view {
        auto it = copiedViewForSourceView.find(oldStrView);
        if (it != copiedViewForSourceView.end())
            return it->second;

        // This should not happen if the source pool only references its own storage.
        raise<std::runtime_error>("Failed to rebuild StringPool copy: unresolved stored string view");
    };

    // Rebuild both lookup maps with string_views pointing into this->storedStrings_.
    idForString_.clear();
    for (const auto& [oldStrView, id] : other.idForString_) {
        idForString_.emplace(copiedViewFor(oldStrView), id);
    }

    stringForId_.clear();
    for (const auto& [id, oldStrView] : other.stringForId_) {
        stringForId_.emplace(id, copiedViewFor(oldStrView));
    }

    // Copy other member variables.
    nextId_ = other.nextId_;
    byteSize_ = other.byteSize_.load();
    cacheHits_ = other.cacheHits_.load();
    cacheMisses_ = other.cacheMisses_.load();
}

auto StringPool::emplace(std::string_view const& str) -> tl::expected<StringId, Error>
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
            return tl::unexpected<Error>(Error::StringPoolOverflow, "StringPool id overflow!");
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

MemoryUsage StringPool::memoryUsage() const
{
    std::shared_lock lock(stringStoreMutex_);

    MemoryUsage result;
    result.logicalBytes = static_cast<size_t>(byteSize_.load());

    // deque does not expose capacity. Count occupied string objects plus any
    // character buffers which live outside those objects; block slack remains
    // part of the documented lower-bound gap.
    result.allocatedBytes = storedStrings_.size() * sizeof(std::string);
    for (auto const& string : storedStrings_) {
        auto const objectBegin = reinterpret_cast<std::uintptr_t>(&string);
        auto const objectEnd = objectBegin + sizeof(string);
        auto const data = reinterpret_cast<std::uintptr_t>(string.data());
        if (data < objectBegin || data >= objectEnd) {
            result.allocatedBytes += string.capacity() + 1;
        }
    }

    // Unordered-map buckets and occupied values are stable, useful lower-bound
    // estimates; implementation-specific node and allocator overhead is omitted.
    result.allocatedBytes +=
        idForString_.bucket_count() * sizeof(void*) +
        idForString_.size() * sizeof(decltype(idForString_)::value_type) +
        stringForId_.bucket_count() * sizeof(void*) +
        stringForId_.size() * sizeof(decltype(stringForId_)::value_type);
    result.allocatedBytes = std::max(result.logicalBytes, result.allocatedBytes);
    return result;
}

void StringPool::addStaticKey(StringId id, const std::string& value)
{
    std::unique_lock lock(stringStoreMutex_);
    auto& storedString = storedStrings_.emplace_back(value);
    idForString_.emplace(storedString, id);
    stringForId_.emplace(id, storedString);
}

auto StringPool::write(std::ostream& outputStream, const StringId offset) const -> tl::expected<void, Error> // NOLINT
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

    return {};
}

auto StringPool::read(const std::vector<uint8_t>& input, size_t offset) -> tl::expected<void, Error>
{
    if (offset > input.size()) {
        return tl::unexpected<Error>(Error::EncodeDecodeError, "Failed to read StringPool: invalid input offset.");
    }

    std::unique_lock stringStoreWriteAccess_(stringStoreMutex_);
    using Adapter = bitsery::InputBufferAdapter<std::vector<uint8_t>>;
    bitsery::Deserializer<Adapter> s(Adapter(input.begin() + static_cast<std::ptrdiff_t>(offset), input.end()));

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
      return tl::unexpected<Error>(
          Error::EncodeDecodeError,
          fmt::format("Failed to read StringPool: Error {}",
                      static_cast<std::underlying_type_t<bitsery::ReaderError>>(s.adapter().error())));
    }

    return {};
}

bool StringPool::operator==(const StringPool &other) const {
    return idForString_ == other.idForString_;
}

const std::deque<std::string>& StringPool::strings() const {
    return storedStrings_;
}

}
