// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.
#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
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

template <typename T, std::size_t ExpectedSize>
struct ColumnTypeField
{
    static_assert(
        sizeof(T) == ExpectedSize,
        "simfil::ColumnTypeField<T, ExpectedSize> size mismatch");

    T value_{};

    constexpr ColumnTypeField() = default;
    constexpr ColumnTypeField(const T& value) : value_(value) {}  // NOLINT
    constexpr ColumnTypeField(T&& value) : value_(std::move(value)) {}  // NOLINT

    constexpr ColumnTypeField& operator=(const T& value)
    {
        value_ = value;
        return *this;
    }

    constexpr ColumnTypeField& operator=(T&& value)
    {
        value_ = std::move(value);
        return *this;
    }

    constexpr operator T&() noexcept { return value_; }  // NOLINT
    constexpr operator const T&() const noexcept { return value_; }  // NOLINT
};

#ifndef MODEL_COLUMN_TYPE
#define MODEL_COLUMN_TYPE(expected_size)                                                   \
    using IsModelColumnType = void;                                                        \
    static constexpr std::size_t kModelColumnExpectedSize = static_cast<std::size_t>(expected_size)
#endif

namespace detail
{

template <typename T, typename = void>
struct has_model_column_tag : std::false_type
{};

template <typename T>
struct has_model_column_tag<
    T,
    std::void_t<typename T::IsModelColumnType, decltype(T::kModelColumnExpectedSize)>>
    : std::true_type
{};

template <typename T>
inline constexpr bool has_model_column_tag_v =
    has_model_column_tag<std::remove_cv_t<T>>::value;

template <typename T>
struct is_model_column_external_type : std::false_type
{};

template <typename T>
inline constexpr bool is_model_column_external_type_v =
    is_model_column_external_type<std::remove_cv_t<T>>::value;

template <typename T>
inline constexpr bool is_fixed_width_integer_v =
    std::is_same_v<T, std::int8_t> || std::is_same_v<T, std::uint8_t> ||
    std::is_same_v<T, std::int16_t> || std::is_same_v<T, std::uint16_t> ||
    std::is_same_v<T, std::int32_t> || std::is_same_v<T, std::uint32_t> ||
    std::is_same_v<T, std::int64_t> || std::is_same_v<T, std::uint64_t>;

template <typename T, bool = std::is_enum_v<T>>
struct is_fixed_width_enum : std::false_type
{};

template <typename T>
struct is_fixed_width_enum<T, true>
    : std::bool_constant<is_fixed_width_integer_v<std::underlying_type_t<T>>>
{};

template <typename T>
inline constexpr bool is_fixed_width_enum_v = is_fixed_width_enum<T>::value;

template <typename T>
inline constexpr bool is_scalar_model_column_type_v =
    std::is_same_v<T, bool> || std::is_same_v<T, float> ||
    std::is_same_v<T, double> || is_fixed_width_integer_v<T> ||
    is_fixed_width_enum_v<T>;

template <typename T>
inline constexpr bool is_native_pod_wire_candidate_v =
    std::is_trivially_copyable_v<std::remove_cv_t<T>> &&
    std::is_standard_layout_v<std::remove_cv_t<T>>;

template <typename T>
constexpr std::size_t expected_model_column_sizeof()
{
    using U = std::remove_cv_t<T>;
    if constexpr (has_model_column_tag_v<U>) {
        return U::kModelColumnExpectedSize;
    } else {
        return sizeof(U);
    }
}

template <typename T>
constexpr std::uint64_t model_column_schema_hash()
{
    return static_cast<std::uint64_t>(expected_model_column_sizeof<T>());
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
inline constexpr bool is_bitsery_input_archive_v =
    requires(S& archive, bitsery::ReaderError error) {
        archive.adapter().error();
        archive.adapter().error(error);
    };

template <typename S>
void mark_bitsery_invalid_data(S& archive)
{
    if constexpr (is_bitsery_input_archive_v<S>) {
        archive.adapter().error(bitsery::ReaderError::InvalidData);
    }
}

template <typename S>
bool read_bitsery_size_prefix_1b(S& archive, std::size_t& out_size)
{
    std::uint8_t hb = 0;
    archive.adapter().template readBytes<1>(hb);
    if (archive.adapter().error() != bitsery::ReaderError::NoError) {
        return false;
    }

    if (hb < 0x80U) {
        out_size = hb;
        return true;
    }

    std::uint8_t lb = 0;
    archive.adapter().template readBytes<1>(lb);
    if (archive.adapter().error() != bitsery::ReaderError::NoError) {
        return false;
    }

    if ((hb & 0x40U) != 0U) {
        std::uint16_t lw = 0;
        archive.adapter().template readBytes<2>(lw);
        if (archive.adapter().error() != bitsery::ReaderError::NoError) {
            return false;
        }
        out_size = static_cast<std::size_t>(
            ((((hb & 0x3FU) << 8U) | lb) << 16U) | lw);
        return true;
    }

    out_size = static_cast<std::size_t>(((hb & 0x7FU) << 8U) | lb);
    return true;
}

template <template <typename, std::size_t> typename T_StoragePolicy>
struct is_vector_storage_policy : std::false_type
{};

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

template <>
struct detail::is_vector_storage_policy<vector_column_storage> : std::true_type
{};

#if defined(SIMFIL_DEFAULT_VECTOR_COLUMN_STORAGE) && SIMFIL_DEFAULT_VECTOR_COLUMN_STORAGE
template <typename TValue, std::size_t T_PageBytes>
using default_column_storage = vector_column_storage<TValue, T_PageBytes>;
#else
template <typename TValue, std::size_t T_PageBytes>
using default_column_storage = segmented_column_storage<TValue, T_PageBytes>;
#endif

inline constexpr std::size_t k_bitsery_max_column_payload_bytes = 0x3FFFFFFFU;

template <
    typename T,
    std::size_t T_RecordsPerPage = 256,
    template <typename, std::size_t> typename T_StoragePolicy = default_column_storage>
class ModelColumn
{
    using TValue = std::remove_cv_t<T>;

    static_assert(
        detail::is_scalar_model_column_type_v<TValue> ||
            detail::has_model_column_tag_v<TValue> ||
            detail::is_model_column_external_type_v<TValue>,
        "ModelColumn<T> requires a fixed-width scalar type or MODEL_COLUMN_TYPE");
    static_assert(
        detail::is_native_pod_wire_candidate_v<TValue>,
        "ModelColumn<T> requires a trivially copyable, standard-layout type");
    static_assert(
        T_RecordsPerPage > 0,
        "records per page must be greater than zero");
    static_assert(
        T_RecordsPerPage <= (std::numeric_limits<std::size_t>::max() / sizeof(TValue)),
        "records-per-page causes page-size overflow");

public:
    using value_type = TValue;
    using ref = TValue&;
    using const_ref = const TValue&;

    static constexpr std::size_t kRecordSize = sizeof(TValue);
    static constexpr std::size_t kExpectedRecordSize =
        detail::expected_model_column_sizeof<TValue>();
    static_assert(
        kExpectedRecordSize == 0 || kExpectedRecordSize == kRecordSize,
        "MODEL_COLUMN_TYPE expected size does not match sizeof(T)");

    static constexpr std::uint64_t kSchemaHash =
        detail::model_column_schema_hash<TValue>();
    static constexpr std::size_t kPageBytes = T_RecordsPerPage * kRecordSize;
    static_assert(
        kPageBytes % kRecordSize == 0,
        "page size must be a multiple of record size");
    using storage_type = typename T_StoragePolicy<TValue, kPageBytes>::type;

    static constexpr std::size_t kRecordsPerPage = T_RecordsPerPage;
    static constexpr std::size_t kPageSizeBytes = kPageBytes;

    using iterator = decltype(std::declval<storage_type&>().begin());
    using const_iterator = decltype(std::declval<const storage_type&>().begin());

    ModelColumn() = default;

    [[nodiscard]] std::size_t size() const { return values_.size(); }

    [[nodiscard]] std::size_t byte_size() const
    {
        return values_.size() * sizeof(TValue);
    }

    [[nodiscard]] bool empty() const { return values_.empty(); }

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

    template <typename... Args, typename = std::enable_if_t<(sizeof...(Args) > 0)>>
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

    void push_back(const TValue& value) { values_.push_back(value); }
    void push_back(TValue&& value) { values_.push_back(std::move(value)); }

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

    [[nodiscard]] std::vector<std::byte> bytes() const
    {
        std::vector<std::byte> out(byte_size());
        if (out.empty()) {
            return out;
        }

        if constexpr (detail::is_vector_storage_policy<T_StoragePolicy>::value) {
            std::memcpy(out.data(), values_.data(), out.size());
        } else {
            std::size_t offset_records = 0;
            while (offset_records < values_.size()) {
                const std::size_t chunk_records =
                    std::min(kRecordsPerPage, values_.size() - offset_records);
                const std::size_t chunk_bytes = chunk_records * sizeof(TValue);
                std::memcpy(
                    out.data() + (offset_records * sizeof(TValue)),
                    value_ptr(offset_records),
                    chunk_bytes);
                offset_records += chunk_records;
            }
        }
        return out;
    }

    [[nodiscard]] tl::expected<void, model_column_io_error>
    assign_bytes(std::span<const std::byte> payload)
    {
        return assign_bytes_impl(payload);
    }

    [[nodiscard]] tl::expected<void, model_column_io_error>
    assign_bytes(std::span<const std::uint8_t> payload)
    {
        return assign_bytes_impl(payload);
    }

    template <typename T_BitseryInputAdapter>
    bool read_payload_from_bitsery(
        T_BitseryInputAdapter& adapter,
        std::size_t payload_size)
    {
        if ((payload_size % sizeof(TValue)) != 0U) {
            values_.clear();
            return false;
        }

        const std::size_t record_count = payload_size / sizeof(TValue);
        values_.resize(record_count);
        if (payload_size == 0) {
            return true;
        }

        if constexpr (detail::is_vector_storage_policy<T_StoragePolicy>::value) {
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
                    std::min(kRecordsPerPage, record_count - offset_records);
                const std::size_t chunk_bytes = chunk_records * sizeof(TValue);
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
    template <typename ByteType>
    [[nodiscard]] tl::expected<void, model_column_io_error>
    assign_bytes_impl(std::span<const ByteType> payload)
    {
        static_assert(
            sizeof(ByteType) == 1,
            "assign_bytes expects 1-byte payload elements");

        if ((payload.size() % sizeof(TValue)) != 0U) {
            return tl::make_unexpected(model_column_io_error::payload_size_mismatch);
        }

        const std::size_t record_count = payload.size() / sizeof(TValue);
        values_.resize(record_count);
        if (payload.empty()) {
            return {};
        }

        if constexpr (detail::is_vector_storage_policy<T_StoragePolicy>::value) {
            std::memcpy(values_.data(), payload.data(), payload.size());
        } else {
            std::size_t offset_records = 0;
            while (offset_records < record_count) {
                const std::size_t chunk_records =
                    std::min(kRecordsPerPage, record_count - offset_records);
                const std::size_t chunk_bytes = chunk_records * sizeof(TValue);
                std::memcpy(
                    value_ptr(offset_records),
                    payload.data() + (offset_records * sizeof(TValue)),
                    chunk_bytes);
                offset_records += chunk_records;
            }
        }
        return {};
    }

    TValue* value_ptr(std::size_t record_index) { return &values_[record_index]; }
    const TValue* value_ptr(std::size_t record_index) const
    {
        return &values_[record_index];
    }

    storage_type values_;
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
    std::uint64_t schema_hash =
        simfil::ModelColumn<T, T_RecordsPerPage, T_StoragePolicy>::kSchemaHash;
    std::uint64_t record_size = static_cast<std::uint64_t>(
        simfil::ModelColumn<T, T_RecordsPerPage, T_StoragePolicy>::kRecordSize);

    if constexpr (simfil::detail::is_bitsery_input_archive_v<S>) {
        s.value8b(schema_hash);
        s.value8b(record_size);
        if (s.adapter().error() != bitsery::ReaderError::NoError) {
            buffer.clear();
            return;
        }

        const std::uint64_t expected_schema =
            simfil::ModelColumn<T, T_RecordsPerPage, T_StoragePolicy>::kSchemaHash;
        const std::uint64_t expected_record_size = static_cast<std::uint64_t>(
            simfil::ModelColumn<T, T_RecordsPerPage, T_StoragePolicy>::kRecordSize);
        if (schema_hash != expected_schema || record_size != expected_record_size) {
            buffer.clear();
            simfil::detail::mark_bitsery_invalid_data(s);
            return;
        }

        std::size_t payload_size = 0;
        if (!simfil::detail::read_bitsery_size_prefix_1b(s, payload_size)) {
            buffer.clear();
            return;
        }
        if (payload_size > simfil::k_bitsery_max_column_payload_bytes ||
            (payload_size % static_cast<std::size_t>(expected_record_size)) != 0U) {
            buffer.clear();
            simfil::detail::mark_bitsery_invalid_data(s);
            return;
        }

        if (!buffer.read_payload_from_bitsery(s.adapter(), payload_size)) {
            buffer.clear();
            return;
        }
    } else {
        std::vector<std::byte> payload = buffer.bytes();
        s.value8b(schema_hash);
        s.value8b(record_size);
        s.container1b(payload, simfil::k_bitsery_max_column_payload_bytes);
    }
}

}  // namespace bitsery
