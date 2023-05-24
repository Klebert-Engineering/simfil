#pragma once

#include "stx/format.h"
#include <cmath>

namespace simfil
{

namespace geo
{

template <class Precision = double>
struct Point
{
    Precision x = 0, y = 0, z = 0;

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
