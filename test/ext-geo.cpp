#include "simfil/ext-geo.h"

#include <catch2/catch_test_macros.hpp>

using namespace simfil::geo;
using BigPoint = Point<double>;

TEST_CASE("Point", "[geo.point]") {
    REQUIRE(BigPoint{0, 1} == BigPoint{0, 1});
}

TEST_CASE("BBox", "[geo.bbox]") {
    REQUIRE(BBox{0, 1, 2, 3} == BBox{0, 1, 2, 3});
}

TEST_CASE("LineString", "[geo.linestring]") {
    SECTION("Two crossing lines") {
        auto l1 = LineString{{BigPoint{-1, -1}, BigPoint{ 1, 1}}};
        auto l2 = LineString{{BigPoint{ 1, -1}, BigPoint{-1, 1}}};
        REQUIRE(l1.intersects(l2));
    };
    SECTION("Two crossing lines vert/horz") {
        auto l1 = LineString{{BigPoint{0, -1}, BigPoint{1, 1}}};
        auto l2 = LineString{{BigPoint{-1, 0}, BigPoint{1, 0}}};
        REQUIRE(l1.intersects(l2));
    };
}

TEST_CASE("Polygon", "[geo.polygon]") {
    SECTION("Point in rectangle polygon") {
        auto p = Polygon{{LineString{{BigPoint{0, 0}, BigPoint{1, 0}, BigPoint{1, 1}, BigPoint{0, 1}}}}};
        REQUIRE(p.contains(BigPoint{0.5, 0.5}));   /* center */
        REQUIRE(p.contains(BigPoint{0, 0}));       /* top-left */
        REQUIRE(!p.contains(BigPoint{-0.5, 0.5})); /* left */
        REQUIRE(!p.contains(BigPoint{ 1.5, 0.5})); /* right */
    };
    SECTION("Point in triangle polygon") {
        auto p = Polygon{{LineString{{BigPoint{0, 0}, BigPoint{1, 1}, BigPoint{0, 1}}}}};
        REQUIRE(p.contains(BigPoint{0.4999, 0.5}));  /* on edge */
        REQUIRE(p.contains(BigPoint{0, 0}));         /* top-left */
        REQUIRE(!p.contains(BigPoint{0.5001, 0.5})); /* left to edge */
    };
}
