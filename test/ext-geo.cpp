#include "simfil/ext-geo.h"

#include <catch2/catch_test_macros.hpp>

using namespace simfil::geo;

TEST_CASE("Point", "[geo.point]") {
    REQUIRE(Point{0, 1} == Point{0, 1});
}

TEST_CASE("BBox", "[geo.bbox]") {
    REQUIRE(BBox{0, 1, 2, 3} == BBox{0, 1, 2, 3});
}

TEST_CASE("LineString", "[geo.linestring]") {
    SECTION("Two crossing lines") {
        auto l1 = LineString{{Point{-1, -1}, Point{ 1, 1}}};
        auto l2 = LineString{{Point{ 1, -1}, Point{-1, 1}}};
        REQUIRE(l1.intersects(l2));
    };
    SECTION("Two crossing lines vert/horz") {
        auto l1 = LineString{{Point{0, -1}, Point{1, 1}}};
        auto l2 = LineString{{Point{-1, 0}, Point{1, 0}}};
        REQUIRE(l1.intersects(l2));
    };
}

TEST_CASE("Polygon", "[geo.polygon]") {
    SECTION("Point in rectangle polygon") {
        auto p = Polygon{{LineString{{Point{0, 0}, Point{1, 0}, Point{1, 1}, Point{0, 1}}}}};
        REQUIRE(p.contains(Point{0.5, 0.5}));   /* center */
        REQUIRE(p.contains(Point{0, 0}));       /* top-left */
        REQUIRE(!p.contains(Point{-0.5, 0.5})); /* left */
        REQUIRE(!p.contains(Point{ 1.5, 0.5})); /* right */
    };
    SECTION("Point in triangle polygon") {
        auto p = Polygon{{LineString{{Point{0, 0}, Point{1, 1}, Point{0, 1}}}}};
        REQUIRE(p.contains(Point{0.4999, 0.5}));  /* on edge */
        REQUIRE(p.contains(Point{0, 0}));         /* top-left */
        REQUIRE(!p.contains(Point{0.5001, 0.5})); /* left to edge */
    };
}
