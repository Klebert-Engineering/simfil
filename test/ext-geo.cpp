#include "simfil/ext-geo.h"
#include "simfil/model/model.h"
#include "simfil/simfil.h"

#include <catch2/catch_test_macros.hpp>

using namespace simfil;
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

TEST_CASE("GeometryCollection", "[geom_collection]") {

    SECTION("Construct GeometryCollection") {
        auto model_pool = std::make_shared<ModelPool>();
        auto geometry_collection = model_pool->newGeometryCollection();

        REQUIRE(geometry_collection->type() == ValueType::Object);
        REQUIRE(geometry_collection->size() == 2); // 'type' and 'geometries' fields
    }

    SECTION("Construct Geometry and Add to GeometryCollection") {
        auto model_pool = std::make_shared<ModelPool>();
        auto geometry_collection = model_pool->newGeometryCollection();
        auto point_geom = geometry_collection->newGeometry(Geometry::GeomType::Points);

        REQUIRE(point_geom->type() == ValueType::Object);
        REQUIRE(point_geom->geomType() == Geometry::GeomType::Points);

        REQUIRE(geometry_collection->size() == 2); // 'type' and 'geometries' fields
        REQUIRE(geometry_collection->at(1)->type() == ValueType::Array); // 'geometries' field
        REQUIRE(geometry_collection->at(1)->size() == 1); // one geometry in the collection
    }
}

TEST_CASE("Spatial Operators", "[spatial_ops]") {
    auto model_pool = std::make_shared<ModelPool>();

    // Create a GeometryCollection with a Point
    auto geometry_collection = model_pool->newGeometryCollection();
    auto point_geom = geometry_collection->newGeometry(Geometry::GeomType::Points);
    point_geom->append({2., 3., 0.});
    model_pool->addRoot(model_pool->newObject()->addField(
        "geometry",
        ModelNode::Ptr(point_geom)));
    Environment env(model_pool->fieldNames());

    SECTION("Point Within BBox") {
        auto ast = compile(env, "geo() within bbox(1, 2, 4, 5)", false);
        INFO("AST: " << ast->toString());

        auto res = eval(env, *ast, *model_pool);
        REQUIRE(res.size() == 1);
        REQUIRE(res[0].as<ValueType::Bool>() == true);
    }

    SECTION("Point Intersects BBox") {
        auto ast = compile(env, "geo() intersects bbox(1, 2, 4, 5)", false);
        INFO("AST: " << ast->toString());

        auto res = eval(env, *ast, *model_pool);
        REQUIRE(res.size() == 1);
        REQUIRE(res[0].as<ValueType::Bool>() == true);
    }

    SECTION("BBox Contains Point") {
        auto ast = compile(env, "bbox(1, 2, 4, 5) contains geo()", false);
        INFO("AST: " << ast->toString());

        auto res = eval(env, *ast, *model_pool);
        REQUIRE(res.size() == 1);
        REQUIRE(res[0].as<ValueType::Bool>() == true);
    }
}
