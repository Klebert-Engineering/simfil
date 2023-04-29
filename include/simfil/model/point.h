#pragma once

#include "stx/format.h"

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

    auto operator+=(const Point& o) -> Point&
    {
        x += o.x;
        y += o.y;
        z += o.z;
    }

    [[nodiscard]] auto toString() const -> std::string
    {
        return stx::format("[{},{}]", x, y);
    }

    auto angleTo(const Point& o) const -> double
    {
        return std::atan2(o.y - y, o.x - x);
    }

    auto distanceTo(const Point& o) const -> double
    {
        return std::sqrt((x - o.x) * (x - o.x) + (y - o.y) * (y - o.y));
    }
};

}

}
