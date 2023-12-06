#pragma once

#include "stx/format.h"
#include <cmath>

namespace simfil
{

namespace geo
{

/**
 * Concept which is used to construct points
 * from arbitrary other compatible structures.
 */
template <typename T, typename Precision>
concept HasXY = requires(T t) {
    { t.x } -> std::convertible_to<Precision>;
    { t.y } -> std::convertible_to<Precision>;
};

/** Minimal 3D point structure. */
template <class Precision = double>
struct Point
{
    Precision x = 0, y = 0, z = 0;

    /** Define trivial constructors */
    Point() = default;
    Point(Point const&) = default;
    Point(Precision const& x, Precision const& y, Precision const& z = .0) : x(x), y(y), z(z) {}

    /**
     * Allow constructing a point from any class which has .x and .y members.
     * If the other class has a z member, it will be applied as well.
     */
    template <typename T>
    requires HasXY<T, Precision>
    Point(T const& other) : x(other.x), y(other.y)  // NOLINT: Allow implicit conversion
    {
        if constexpr (requires { {other.z} -> std::convertible_to<Precision>; }) {
            z = other.z;
        }
    }

    auto operator==(const Point& o) const -> bool
    {
        return x == o.x && y == o.y && z == o.z;
    }

    template <class OtherPrecision>
    auto operator+=(const Point<OtherPrecision>& o) -> Point&
    {
        x += o.x;
        y += o.y;
        z += o.z;
        return *this;
    }

    [[nodiscard]] auto toString() const -> std::string
    {
        return stx::format("[{},{}]", x, y);
    }

    template <class OtherPrecision>
    auto angleTo(const Point<OtherPrecision>& o) const -> double
    {
        return std::atan2(o.y - y, o.x - x);
    }

    template <class OtherPrecision>
    auto distanceTo(const Point<OtherPrecision>& o) const -> double
    {
        return std::sqrt((x - o.x) * (x - o.x) + (y - o.y) * (y - o.y));
    }
};

}

}
