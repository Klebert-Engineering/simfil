#pragma once

#include <cstdint>
#include <type_traits>

namespace simfil
{

namespace detail
{

template <class Type_, class Enable_ = void>
struct UnderlyingIntegralType
{
    using Type = Type_;
};

template <class Type_>
struct UnderlyingIntegralType<Type_, std::enable_if_t<std::is_enum_v<Type_>>>
{
    using Type = std::underlying_type_t<Type_>;
};

template <class T>
using UnderlyingIntegralTypeT = typename UnderlyingIntegralType<T>::Type;

template <class From, class To>
using IsSafeIntegerConversion =
    std::integral_constant<
        bool,
        std::is_integral_v<UnderlyingIntegralTypeT<From>> &&
        std::is_signed_v<UnderlyingIntegralTypeT<From>> == std::is_signed_v<To> &&
        sizeof(UnderlyingIntegralTypeT<From>) <= sizeof(To)>;

template <class From, class To>
using EnableSafeIntegerConversion = std::enable_if_t<IsSafeIntegerConversion<From, To>::value>;

} // namespace detail

using StringId = std::uint16_t;
static_assert(std::is_unsigned_v<StringId>, "StringId must be unsigned!");

/// StringHandle is a strong typedef for StringId
/// and should be used as a replacement, if possible.
struct StringHandle
{
    using Type = StringId;

    Type value;

    StringHandle() : value(0u) {}
    StringHandle(const StringHandle& id) = default;

    template <typename T, typename = detail::EnableSafeIntegerConversion<T, Type>>
    constexpr StringHandle(const T& value) : value(static_cast<Type>(value)) {}

    explicit operator bool() const {
        return value != (Type)0;
    }

    explicit operator Type() const {
        return value;
    }

    auto operator==(const StringHandle& id) const -> bool {
        return value == id.value;
    }

    template <class T, class = detail::EnableSafeIntegerConversion<T, Type>>
    auto operator==(const T& v) const -> bool {
      return value == v;
    }
};

}

namespace std
{

template <>
struct hash<simfil::StringHandle> {
  auto operator()(const simfil::StringHandle &id) const noexcept -> std::size_t {
    return std::hash<simfil::StringHandle::Type>()(id.value);
  }
};

} // namespace std


namespace bitsery
{

template <typename S>
void serialize(S& s, simfil::StringHandle& v)
{
    s.value2b(v.value);
}

} // namespace bitsery
