// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.
#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iterator>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include <bitsery/bitsery.h>
#include <bitsery/traits/vector.h>
#include <sfl/segmented_vector.hpp>
#include <tl/expected.hpp>

namespace simfil
{

static_assert(
    std::endian::native == std::endian::little,
    "simfil::ModelColumn currently supports little-endian hosts only");

enum class model_column_io_error
{
    payload_size_mismatch,
};

#ifndef MODEL_COLUMN_TYPE
#define MODEL_COLUMN_TYPE(expected_size)                                                   \
    using IsModelColumnType = void;                                                        \
    static constexpr std::size_t model_column_expected_size = expected_size
#endif

template <typename TFirst, typename TSecond>
struct TwoPart
{
    using first_type = std::remove_cv_t<TFirst>;
    using second_type = std::remove_cv_t<TSecond>;

    first_type first_{};
    second_type second_{};

    TwoPart() = default;
    TwoPart(first_type const& first, second_type const& second)
        : first_(first), second_(second)
    {
    }
    TwoPart(first_type&& first, second_type&& second)
        : first_(std::move(first)), second_(std::move(second))
    {
    }

    [[nodiscard]] first_type& first() noexcept { return first_; }
    [[nodiscard]] first_type const& first() const noexcept { return first_; }
    [[nodiscard]] second_type& second() noexcept { return second_; }
    [[nodiscard]] second_type const& second() const noexcept { return second_; }

    bool operator==(TwoPart const&) const = default;
};

namespace detail
{

template <typename T>
struct is_two_part : std::false_type
{};

template <typename TFirst, typename TSecond>
struct is_two_part<TwoPart<TFirst, TSecond>> : std::true_type
{};

template <typename T>
concept two_part_type = is_two_part<std::remove_cv_t<T>>::value;

template <typename T, typename = void>
struct has_model_column_tag_trait : std::false_type
{};

template <typename T>
struct has_model_column_tag_trait<
    T,
    std::void_t<typename T::IsModelColumnType, decltype(T::model_column_expected_size)>>
    : std::true_type
{};

template <typename T>
concept model_column_tagged =
    has_model_column_tag_trait<std::remove_cv_t<T>>::value;

template <typename T>
struct is_model_column_external_type : std::false_type
{};

template <typename T>
concept model_column_external_type =
    is_model_column_external_type<std::remove_cv_t<T>>::value;

template <typename T>
concept fixed_width_integer =
    std::same_as<std::remove_cv_t<T>, std::int8_t> ||
    std::same_as<std::remove_cv_t<T>, std::uint8_t> ||
    std::same_as<std::remove_cv_t<T>, std::int16_t> ||
    std::same_as<std::remove_cv_t<T>, std::uint16_t> ||
    std::same_as<std::remove_cv_t<T>, std::int32_t> ||
    std::same_as<std::remove_cv_t<T>, std::uint32_t> ||
    std::same_as<std::remove_cv_t<T>, std::int64_t> ||
    std::same_as<std::remove_cv_t<T>, std::uint64_t>;

template <typename T>
concept fixed_width_enum =
    std::is_enum_v<std::remove_cv_t<T>> &&
    fixed_width_integer<std::underlying_type_t<std::remove_cv_t<T>>>;

template <typename T>
concept scalar_model_column_type =
    std::same_as<std::remove_cv_t<T>, bool> ||
    std::same_as<std::remove_cv_t<T>, float> ||
    std::same_as<std::remove_cv_t<T>, double> ||
    fixed_width_integer<T> ||
    fixed_width_enum<T>;

template <typename T>
concept native_pod_wire_candidate =
    std::is_trivially_copyable_v<std::remove_cv_t<T>> &&
    std::is_standard_layout_v<std::remove_cv_t<T>>;

template <typename T>
constexpr std::size_t expected_model_column_sizeof()
{
    using U = std::remove_cv_t<T>;
    if constexpr (model_column_tagged<U>) {
        return U::model_column_expected_size;
    } else {
        return sizeof(U);
    }
}

template <typename TValue, std::size_t T_PageBytes>
struct segmented_storage_page_elements
{
    static_assert(T_PageBytes > 0, "page size must be greater than zero");
    static_assert(
        (T_PageBytes % sizeof(TValue)) == 0,
        "page size must be a multiple of element size");
    static constexpr std::size_t value = T_PageBytes / sizeof(TValue);
};

template <typename S>
concept bitsery_input_archive =
    requires(S& archive, bitsery::ReaderError error) {
        archive.adapter().error();
        archive.adapter().error(error);
    };

template <typename S>
void mark_bitsery_invalid_data(S& archive)
{
    if constexpr (bitsery_input_archive<S>) {
        archive.adapter().error(bitsery::ReaderError::InvalidData);
    }
}

template <template <typename, std::size_t> typename T_StoragePolicy>
concept vector_storage_policy =
    std::same_as<
        typename T_StoragePolicy<std::byte, 1>::type,
        std::vector<std::byte>>;

template <typename TValue>
auto arena_access_wrap(TValue&& value)
{
    if constexpr (std::is_lvalue_reference_v<TValue>) {
        if constexpr (std::is_const_v<std::remove_reference_t<TValue>>) {
            return std::cref(value);
        } else {
            return std::ref(value);
        }
    } else {
        return std::forward<TValue>(value);
    }
}

template <typename TValue>
using arena_access_result_t = decltype(arena_access_wrap(std::declval<TValue>()));

}  // namespace detail

template <typename TValue, std::size_t T_PageBytes>
struct segmented_column_storage
{
    using type = sfl::segmented_vector<
        TValue,
        detail::segmented_storage_page_elements<TValue, T_PageBytes>::value>;
};

template <typename TValue, std::size_t T_PageBytes>
struct vector_column_storage
{
    using type = std::vector<TValue>;
};

#if defined(SIMFIL_DEFAULT_VECTOR_COLUMN_STORAGE) && SIMFIL_DEFAULT_VECTOR_COLUMN_STORAGE
template <typename TValue, std::size_t T_PageBytes>
using default_column_storage = vector_column_storage<TValue, T_PageBytes>;
#else
template <typename TValue, std::size_t T_PageBytes>
using default_column_storage = segmented_column_storage<TValue, T_PageBytes>;
#endif

inline constexpr std::size_t bitsery_max_column_payload_bytes = 0x3FFFFFFFU;

template <
    typename T,
    std::size_t T_RecordsPerPage = 256,
    template <typename, std::size_t> typename T_StoragePolicy = default_column_storage>
class ModelColumn
{
public:
    using value_type = std::remove_cv_t<T>;
    using ref = value_type&;
    using const_ref = value_type const&;

    static constexpr std::size_t record_size = sizeof(value_type);
    static constexpr std::size_t expected_record_size =
        detail::expected_model_column_sizeof<value_type>();
    static_assert(
        expected_record_size == 0 || expected_record_size == record_size,
        "MODEL_COLUMN_TYPE expected size does not match sizeof(T)");

    static constexpr std::size_t page_bytes = T_RecordsPerPage * record_size;
    static_assert(
        page_bytes % record_size == 0,
        "page size must be a multiple of record size");
    using storage_type = typename T_StoragePolicy<value_type, page_bytes>::type;

    static constexpr std::size_t records_per_page = T_RecordsPerPage;
    static constexpr std::size_t page_size_bytes = page_bytes;

    using iterator = decltype(std::declval<storage_type&>().begin());
    using const_iterator = decltype(std::declval<const storage_type&>().begin());

    ModelColumn() = default;

    std::size_t size() const { return values_.size(); }

    std::size_t byte_size() const
    {
        return values_.size() * sizeof(value_type);
    }

    bool empty() const { return values_.empty(); }

    void clear() { values_.clear(); }

    void reserve(std::size_t count)
    {
        if constexpr (
            requires(storage_type& storage, std::size_t n) { storage.reserve(n); }) {
            values_.reserve(count);
        }
    }

    void resize(std::size_t count) { values_.resize(count); }

    void shrink_to_fit()
    {
        if constexpr (requires(storage_type& storage) { storage.shrink_to_fit(); }) {
            values_.shrink_to_fit();
        }
    }

    ref emplace_back()
    {
        values_.emplace_back();
        return values_.back();
    }

    template <typename... Args>
        requires (sizeof...(Args) > 0)
    ref emplace_back(Args&&... args)
    {
        values_.emplace_back(std::forward<Args>(args)...);
        return values_.back();
    }

    template <typename... Args>
    ref emplace(Args&&... args)
    {
        values_.emplace_back(std::forward<Args>(args)...);
        return values_.back();
    }

    void push_back(value_type const& value) { values_.push_back(value); }
    void push_back(value_type&& value) { values_.push_back(std::move(value)); }

    ref operator[](std::size_t index) { return values_[index]; }
    const_ref operator[](std::size_t index) const { return values_[index]; }

    ref at(std::size_t index) { return values_.at(index); }
    const_ref at(std::size_t index) const { return values_.at(index); }

    ref back() { return values_.back(); }
    const_ref back() const { return values_.back(); }

    iterator begin() { return values_.begin(); }
    const_iterator begin() const { return values_.begin(); }
    const_iterator cbegin() const { return values_.begin(); }

    iterator end() { return values_.end(); }
    const_iterator end() const { return values_.end(); }
    const_iterator cend() const { return values_.end(); }

    template <typename InputIt>
    iterator insert(const_iterator pos, InputIt first, InputIt last)
    {
        return values_.insert(pos, first, last);
    }

    template <typename Storage = storage_type>
    auto data() -> decltype(std::declval<Storage&>().data())
    {
        return values_.data();
    }

    template <typename Storage = storage_type>
    auto data() const -> decltype(std::declval<const Storage&>().data())
    {
        return values_.data();
    }

    std::vector<std::byte> bytes() const
    {
        std::vector<std::byte> out(byte_size());
        if (out.empty()) {
            return out;
        }

        if constexpr (detail::vector_storage_policy<T_StoragePolicy>) {
            std::memcpy(out.data(), values_.data(), out.size());
        } else {
            std::size_t offset_records = 0;
            while (offset_records < values_.size()) {
                const std::size_t chunk_records =
                    std::min(records_per_page, values_.size() - offset_records);
                const std::size_t chunk_bytes = chunk_records * sizeof(value_type);
                std::memcpy(
                    out.data() + (offset_records * sizeof(value_type)),
                    value_ptr(offset_records),
                    chunk_bytes);
                offset_records += chunk_records;
            }
        }
        return out;
    }

    tl::expected<void, model_column_io_error>
    assign_bytes(std::span<const std::byte> payload)
    {
        return assign_bytes_impl(payload);
    }

    tl::expected<void, model_column_io_error>
    assign_bytes(std::span<const std::uint8_t> payload)
    {
        return assign_bytes_impl(payload);
    }

    template <typename T_BitseryInputAdapter>
    bool read_payload_from_bitsery(
        T_BitseryInputAdapter& adapter,
        std::size_t payload_size)
    {
        if ((payload_size % sizeof(value_type)) != 0U) {
            values_.clear();
            return false;
        }

        const std::size_t record_count = payload_size / sizeof(value_type);
        values_.resize(record_count);
        if (payload_size == 0) {
            return true;
        }

        if constexpr (detail::vector_storage_policy<T_StoragePolicy>) {
            adapter.template readBuffer<1>(
                reinterpret_cast<std::uint8_t*>(values_.data()),
                payload_size);
            if (adapter.error() != bitsery::ReaderError::NoError) {
                values_.clear();
                return false;
            }
            return true;
        } else {
            std::size_t offset_records = 0;
            while (offset_records < record_count) {
                const std::size_t chunk_records =
                    std::min(records_per_page, record_count - offset_records);
                const std::size_t chunk_bytes = chunk_records * sizeof(value_type);
                adapter.template readBuffer<1>(
                    reinterpret_cast<std::uint8_t*>(value_ptr(offset_records)),
                    chunk_bytes);
                if (adapter.error() != bitsery::ReaderError::NoError) {
                    values_.clear();
                    return false;
                }
                offset_records += chunk_records;
            }
            return true;
        }
    }

private:
    static_assert(
        detail::scalar_model_column_type<value_type> ||
            detail::model_column_tagged<value_type> ||
            detail::model_column_external_type<value_type>,
        "ModelColumn<T> requires a fixed-width scalar type or MODEL_COLUMN_TYPE");
    static_assert(
        detail::native_pod_wire_candidate<value_type>,
        "ModelColumn<T> requires a trivially copyable, standard-layout type");
    static_assert(
        T_RecordsPerPage > 0,
        "records per page must be greater than zero");
    static_assert(
        T_RecordsPerPage <= (std::numeric_limits<std::size_t>::max() / sizeof(value_type)),
        "records-per-page causes page-size overflow");

    template <typename ByteType>
    tl::expected<void, model_column_io_error>
    assign_bytes_impl(std::span<const ByteType> payload)
    {
        static_assert(
            sizeof(ByteType) == 1,
            "assign_bytes expects 1-byte payload elements");

        if ((payload.size() % sizeof(value_type)) != 0U) {
            return tl::make_unexpected(model_column_io_error::payload_size_mismatch);
        }

        const std::size_t record_count = payload.size() / sizeof(value_type);
        values_.resize(record_count);
        if (payload.empty()) {
            return {};
        }

        if constexpr (detail::vector_storage_policy<T_StoragePolicy>) {
            std::memcpy(values_.data(), payload.data(), payload.size());
        } else {
            std::size_t offset_records = 0;
            while (offset_records < record_count) {
                const std::size_t chunk_records =
                    std::min(records_per_page, record_count - offset_records);
                const std::size_t chunk_bytes = chunk_records * sizeof(value_type);
                std::memcpy(
                    value_ptr(offset_records),
                    payload.data() + (offset_records * sizeof(value_type)),
                    chunk_bytes);
                offset_records += chunk_records;
            }
        }
        return {};
    }

    value_type* value_ptr(std::size_t record_index) { return &values_[record_index]; }
    value_type const* value_ptr(std::size_t record_index) const
    {
        return &values_[record_index];
    }

    storage_type values_;
};

template <
    typename TFirst,
    typename TSecond,
    std::size_t T_RecordsPerPage,
    template <typename, std::size_t> typename T_StoragePolicy>
class ModelColumn<TwoPart<TFirst, TSecond>, T_RecordsPerPage, T_StoragePolicy>
{
public:
    using value_type = TwoPart<TFirst, TSecond>;
    using first_type = typename value_type::first_type;
    using second_type = typename value_type::second_type;
    using first_column_type =
        ModelColumn<first_type, T_RecordsPerPage, T_StoragePolicy>;
    using second_column_type =
        ModelColumn<second_type, T_RecordsPerPage, T_StoragePolicy>;

    static constexpr std::size_t record_size =
        sizeof(first_type) + sizeof(second_type);
    static constexpr std::size_t expected_record_size = record_size;

    static constexpr std::size_t page_bytes = T_RecordsPerPage * record_size;
    static constexpr std::size_t records_per_page = T_RecordsPerPage;
    static constexpr std::size_t page_size_bytes = page_bytes;

    template <bool T_IsConst>
    class basic_ref
    {
    public:
        using first_ref = std::conditional_t<T_IsConst, first_type const&, first_type&>;
        using second_ref = std::conditional_t<T_IsConst, second_type const&, second_type&>;

        basic_ref(first_ref first, second_ref second)
            : first_(first), second_(second)
        {
        }

        [[nodiscard]] first_ref first() const noexcept { return first_; }
        [[nodiscard]] second_ref second() const noexcept { return second_; }
        [[nodiscard]] operator value_type() const { return value_type{first_, second_}; }

        [[nodiscard]] bool operator==(value_type const& other) const
        {
            return first_ == other.first_ && second_ == other.second_;
        }

        template <bool T_Enable = T_IsConst>
        std::enable_if_t<!T_Enable, basic_ref&>
        operator=(value_type const& value)
        {
            first_ = value.first_;
            second_ = value.second_;
            return *this;
        }

        template <bool T_Enable = T_IsConst>
        std::enable_if_t<!T_Enable, basic_ref&>
        operator=(basic_ref const& other)
        {
            first_ = other.first_;
            second_ = other.second_;
            return *this;
        }

    private:
        first_ref first_;
        second_ref second_;
    };

    using ref = basic_ref<false>;
    using const_ref = basic_ref<true>;

    template <bool T_IsConst>
    class basic_iterator
    {
        friend class basic_iterator<!T_IsConst>;
        friend class ModelColumn;

        using owner_type = std::conditional_t<T_IsConst, const ModelColumn, ModelColumn>;

    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = ModelColumn::value_type;
        using difference_type = std::ptrdiff_t;
        using reference = std::conditional_t<T_IsConst, const_ref, ref>;
        using pointer = void;

        basic_iterator() = default;

        basic_iterator(owner_type* owner, std::size_t index)
            : owner_(owner), index_(index)
        {
        }

        template <bool T_Enable = T_IsConst>
        basic_iterator(basic_iterator<false> const& other)
            requires(T_Enable)
            : owner_(other.owner_), index_(other.index_)
        {
        }

        reference operator*() const { return (*owner_)[index_]; }

        basic_iterator& operator++()
        {
            ++index_;
            return *this;
        }

        basic_iterator operator++(int)
        {
            auto copy = *this;
            ++(*this);
            return copy;
        }

        bool operator==(basic_iterator const& other) const
        {
            return owner_ == other.owner_ && index_ == other.index_;
        }

        bool operator!=(basic_iterator const& other) const
        {
            return !(*this == other);
        }

    private:
        owner_type* owner_ = nullptr;
        std::size_t index_ = 0;
    };

    using iterator = basic_iterator<false>;
    using const_iterator = basic_iterator<true>;

    ModelColumn() = default;

    [[nodiscard]] std::size_t size() const
    {
        assert(first_values_.size() == second_values_.size());
        return first_values_.size();
    }

    [[nodiscard]] std::size_t byte_size() const
    {
        return first_values_.byte_size() + second_values_.byte_size();
    }

    [[nodiscard]] bool empty() const { return size() == 0; }

    void clear()
    {
        first_values_.clear();
        second_values_.clear();
    }

    void reserve(std::size_t count)
    {
        first_values_.reserve(count);
        second_values_.reserve(count);
    }

    void resize(std::size_t count)
    {
        first_values_.resize(count);
        second_values_.resize(count);
    }

    void shrink_to_fit()
    {
        first_values_.shrink_to_fit();
        second_values_.shrink_to_fit();
    }

    template <typename... Args>
    ref emplace_back(Args&&... args)
    {
        value_type value(std::forward<Args>(args)...);
        first_values_.push_back(value.first_);
        second_values_.push_back(value.second_);
        return back();
    }

    template <typename... Args>
    ref emplace(Args&&... args)
    {
        return emplace_back(std::forward<Args>(args)...);
    }

    void push_back(value_type const& value)
    {
        first_values_.push_back(value.first_);
        second_values_.push_back(value.second_);
    }

    void push_back(value_type&& value)
    {
        first_values_.push_back(std::move(value.first_));
        second_values_.push_back(std::move(value.second_));
    }

    ref operator[](std::size_t index)
    {
        return ref(first_values_[index], second_values_[index]);
    }

    const_ref operator[](std::size_t index) const
    {
        return const_ref(first_values_[index], second_values_[index]);
    }

    ref at(std::size_t index)
    {
        return ref(first_values_.at(index), second_values_.at(index));
    }

    const_ref at(std::size_t index) const
    {
        return const_ref(first_values_.at(index), second_values_.at(index));
    }

    ref back()
    {
        return ref(first_values_.back(), second_values_.back());
    }

    const_ref back() const
    {
        return const_ref(first_values_.back(), second_values_.back());
    }

    iterator begin() { return iterator(this, 0); }
    const_iterator begin() const { return const_iterator(this, 0); }
    const_iterator cbegin() const { return const_iterator(this, 0); }

    iterator end() { return iterator(this, size()); }
    const_iterator end() const { return const_iterator(this, size()); }
    const_iterator cend() const { return const_iterator(this, size()); }

    template <typename InputIt>
    iterator insert(const_iterator pos, InputIt first, InputIt last)
    {
        const auto insert_index = pos.index_;
        std::vector<first_type> first_parts;
        std::vector<second_type> second_parts;
        for (auto it = first; it != last; ++it) {
            value_type value = *it;
            first_parts.push_back(value.first_);
            second_parts.push_back(value.second_);
        }

        auto first_pos = first_values_.begin();
        auto second_pos = second_values_.begin();
        std::advance(first_pos, static_cast<std::ptrdiff_t>(insert_index));
        std::advance(second_pos, static_cast<std::ptrdiff_t>(insert_index));
        first_values_.insert(first_pos, first_parts.begin(), first_parts.end());
        second_values_.insert(second_pos, second_parts.begin(), second_parts.end());
        return iterator(this, insert_index);
    }

    std::vector<std::byte> bytes() const
    {
        const auto first_payload = first_values_.bytes();
        const auto second_payload = second_values_.bytes();

        std::vector<std::byte> out(first_payload.size() + second_payload.size());
        if (!first_payload.empty()) {
            std::memcpy(out.data(), first_payload.data(), first_payload.size());
        }
        if (!second_payload.empty()) {
            std::memcpy(
                out.data() + first_payload.size(),
                second_payload.data(),
                second_payload.size());
        }
        return out;
    }

    tl::expected<void, model_column_io_error>
    assign_bytes(std::span<const std::byte> payload)
    {
        return assign_bytes_impl(payload);
    }

    tl::expected<void, model_column_io_error>
    assign_bytes(std::span<const std::uint8_t> payload)
    {
        return assign_bytes_impl(payload);
    }

    template <typename T_BitseryInputAdapter>
    bool read_payload_from_bitsery(
        T_BitseryInputAdapter& adapter,
        std::size_t payload_size)
    {
        std::vector<std::uint8_t> payload(payload_size);
        if (payload_size > 0) {
            adapter.template readBuffer<1>(payload.data(), payload_size);
            if (adapter.error() != bitsery::ReaderError::NoError) {
                clear();
                return false;
            }
        }

        if (!assign_bytes(std::span<const std::uint8_t>(payload.data(), payload.size()))) {
            clear();
            return false;
        }
        return true;
    }

private:
    template <typename ByteType>
    tl::expected<void, model_column_io_error>
    assign_bytes_impl(std::span<const ByteType> payload)
    {
        static_assert(
            sizeof(ByteType) == 1,
            "assign_bytes expects 1-byte payload elements");

        if ((payload.size() % record_size) != 0U) {
            return tl::make_unexpected(model_column_io_error::payload_size_mismatch);
        }

        const auto record_count = payload.size() / record_size;
        const auto first_payload_size = record_count * sizeof(first_type);
        const auto second_payload_size = record_count * sizeof(second_type);

        auto const* payload_ptr = reinterpret_cast<std::byte const*>(payload.data());
        auto const first_payload = std::span<const std::byte>(payload_ptr, first_payload_size);
        auto const second_payload = std::span<const std::byte>(
            payload_ptr + first_payload_size,
            second_payload_size);

        auto first_assign = first_values_.assign_bytes(first_payload);
        if (!first_assign) {
            clear();
            return tl::make_unexpected(first_assign.error());
        }

        auto second_assign = second_values_.assign_bytes(second_payload);
        if (!second_assign) {
            clear();
            return tl::make_unexpected(second_assign.error());
        }

        return {};
    }

    first_column_type first_values_;
    second_column_type second_values_;
};

}  // namespace simfil

namespace bitsery
{

template <
    typename S,
    typename T,
    std::size_t T_RecordsPerPage,
    template <typename, std::size_t> typename T_StoragePolicy>
void serialize(S& s, simfil::ModelColumn<T, T_RecordsPerPage, T_StoragePolicy>& buffer)
{
    using Column = simfil::ModelColumn<T, T_RecordsPerPage, T_StoragePolicy>;

    if constexpr (simfil::detail::bitsery_input_archive<S>) {
        std::uint64_t payload_size_wire = 0;
        s.value8b(payload_size_wire);
        if (s.adapter().error() != bitsery::ReaderError::NoError) {
            buffer.clear();
            return;
        }

        if (payload_size_wire > simfil::bitsery_max_column_payload_bytes ||
            payload_size_wire > std::numeric_limits<std::size_t>::max() ||
            (payload_size_wire % Column::record_size) != 0U) {
            buffer.clear();
            simfil::detail::mark_bitsery_invalid_data(s);
            return;
        }

        const std::size_t payload_size = static_cast<std::size_t>(payload_size_wire);
        if (!buffer.read_payload_from_bitsery(s.adapter(), payload_size)) {
            buffer.clear();
            return;
        }
    } else {
        std::vector<std::byte> payload = buffer.bytes();
        assert(payload.size() <= simfil::bitsery_max_column_payload_bytes);

        std::uint64_t payload_size_wire = payload.size();
        s.value8b(payload_size_wire);

        if (!payload.empty()) {
            s.adapter().template writeBuffer<1>(
                reinterpret_cast<const std::uint8_t*>(payload.data()),
                payload.size());
        }
    }
}

}  // namespace bitsery
