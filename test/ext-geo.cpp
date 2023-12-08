#include "simfil/ext-geo.h"
#include "simfil/model/model.h"
#include "simfil/simfil.h"

#include <catch2/catch_test_macros.hpp>

using namespace simfil;
using namespace simfil::geo;
using BigPoint = Point<double>;

#define REQUIRE_EVAL_1(query, type, result) \
    do {\
        Environment env(model_pool->fieldNames()); \
        auto ast = compile(env, query, false); \
        INFO("AST: " << ast->toString()); \
        auto res = eval(env, *ast, *model_pool); \
        REQUIRE(res.size() == 1); \
        REQUIRE(res[0].as<type>() == result); \
    } while (false)

TEST_CASE("Point", "[geo.point]") {
    REQUIRE(BigPoint{0, 1} == BigPoint{0, 1});
}

TEST_CASE("BBox", "[geo.bbox]") {
    REQUIRE(BBox{{0, 1}, {2, 3}} == BBox{{0, 1}, {2, 3}});
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

TEST_CASE("GeometryCollection", "[geom.collection]") {
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
        point_geom->append({.0, .0, .0});
        point_geom->append({.25, .25, .25});
        point_geom->append({.5, .5, .5});
        point_geom->append({1., 1., 1.});

        REQUIRE(point_geom->type() == ValueType::Object);
        REQUIRE(point_geom->geomType() == Geometry::GeomType::Points);
        REQUIRE(point_geom->numPoints() == 4);
        REQUIRE(point_geom->pointAt(0).x == .0);
        REQUIRE(point_geom->pointAt(1).x == .25);
        REQUIRE(point_geom->pointAt(2).x == .5);
        REQUIRE(point_geom->pointAt(3).x == 1.);
        REQUIRE(geometry_collection->numGeometries() == 1);

        // Since the collection only contains one geometry,
        // it hides itself and directly presents the nested geometry,
        // conforming to GeoJSON (a collection must have >1 geometries).
        REQUIRE(geometry_collection->size() == 2); // 'type' and 'geometry' fields
        REQUIRE(geometry_collection->at(1)->type() == ValueType::Array); // 'geometry' field
        REQUIRE(geometry_collection->at(1)->size() == 4); // four points

        // Add nested geometry again two more times, now the view changes.
        geometry_collection->addGeometry(point_geom);
        geometry_collection->addGeometry(point_geom);

        REQUIRE(geometry_collection->numGeometries() == 3);
        REQUIRE(geometry_collection->at(1)->size() == 3); // Three geometries
    }
}

TEST_CASE("Spatial Operators", "[spatial.ops]") {
    auto model_pool = std::make_shared<ModelPool>();

    // Create a GeometryCollection with a Point
    auto geometry_collection = model_pool->newGeometryCollection();
    auto point_geom = geometry_collection->newGeometry(Geometry::GeomType::Points);
    point_geom->append({2., 3., 0.});
    model_pool->addRoot(shared_model_ptr<ModelNode>(model_pool->newObject()->addField(
        "geometry",
        point_geom)));
    Environment env(model_pool->fieldNames());

    SECTION("Point Within BBox") {
        REQUIRE_EVAL_1("geo() within bbox(1, 2, 4, 5)", ValueType::Bool, true);
    }

    SECTION("Point Intersects BBox") {
        REQUIRE_EVAL_1("geo() intersects bbox(1, 2, 4, 5)", ValueType::Bool, true);
    }

    SECTION("BBox Contains Point") {
        REQUIRE_EVAL_1("bbox(1, 2, 4, 5) contains geo()", ValueType::Bool, true);
    }
}

TEST_CASE("GeometryCollection Multiple Geometries", "[geom.collection.multiple]") {
    auto model_pool = std::make_shared<ModelPool>();

    // Points
    BigPoint a{2, 3}, b{1, 1}, c{4, 4}, d{0, 0}, e{5, 0}, f{2.5, 5};

    // Create a GeometryCollection
    auto geometry_collection = model_pool->newGeometryCollection();

    // Create and add Point geometry
    auto point_geom = geometry_collection->newGeometry(Geometry::GeomType::Points);
    point_geom->append(a);

    // Create and add LineString geometry
    auto linestring_geom = geometry_collection->newGeometry(Geometry::GeomType::Line);
    linestring_geom->append(b);
    linestring_geom->append(c);

    // Create and add Polygon geometry
    auto polygon_geom = geometry_collection->newGeometry(Geometry::GeomType::Polygon);
    polygon_geom->append(d);
    polygon_geom->append(e);
    polygon_geom->append(f);

    model_pool->addRoot(shared_model_ptr<ModelNode>(model_pool->newObject()->addField(
        "geometry",
        ModelNode::Ptr(geometry_collection))));

    SECTION("Retrieve points") {
        // Check stored points in Point geometry
        REQUIRE(point_geom->get(Fields::Coordinates)->size() == 1);
        REQUIRE(point_geom->get(Fields::Coordinates)->at(0)->type() == ValueType::Array); // Point
        REQUIRE(point_geom->get(Fields::Coordinates)->at(0)->get(Fields::Lon)->value() == ScalarValueType(a.x));
        REQUIRE(point_geom->get(Fields::Coordinates)->at(0)->get(Fields::Lat)->value() == ScalarValueType(a.y));

        // Check stored points in LineString geometry
        REQUIRE(linestring_geom->get(Fields::Coordinates)->size() == 2);
        REQUIRE(linestring_geom->get(Fields::Coordinates)->at(0)->get(Fields::Lon)->value() == ScalarValueType(b.x));
        REQUIRE(linestring_geom->get(Fields::Coordinates)->at(0)->get(Fields::Lat)->value() == ScalarValueType(b.y));
        REQUIRE(linestring_geom->get(Fields::Coordinates)->at(1)->get(Fields::Lon)->value() == ScalarValueType(c.x));
        REQUIRE(linestring_geom->get(Fields::Coordinates)->at(1)->get(Fields::Lat)->value() == ScalarValueType(c.y));

        // Check stored points in Polygon geometry
        REQUIRE(polygon_geom->get(Fields::Coordinates)->size() == 3);
        REQUIRE(polygon_geom->get(Fields::Coordinates)->at(0)->get(Fields::Lon)->value() == ScalarValueType(d.x));
        REQUIRE(polygon_geom->get(Fields::Coordinates)->at(0)->get(Fields::Lat)->value() == ScalarValueType(d.y));
        REQUIRE(polygon_geom->get(Fields::Coordinates)->at(1)->get(Fields::Lon)->value() == ScalarValueType(e.x));
        REQUIRE(polygon_geom->get(Fields::Coordinates)->at(1)->get(Fields::Lat)->value() == ScalarValueType(e.y));
        REQUIRE(polygon_geom->get(Fields::Coordinates)->at(2)->get(Fields::Lon)->value() == ScalarValueType(f.x));
        REQUIRE(polygon_geom->get(Fields::Coordinates)->at(2)->get(Fields::Lat)->value() == ScalarValueType(f.y));
    }

    SECTION("For-each") {
        auto pts = std::vector<BigPoint>{a, b, c, d, e, f};
        auto numPts = 0;
        auto numGeoms = 0;
        geometry_collection->forEachGeometry(
            [&](auto&& g)
            {
                g->forEachPoint(
                    [&](auto&& p)
                    {
                        REQUIRE(p == pts[numPts]);
                        ++numPts;
                        return true;
                    });
                ++numGeoms;
                return true;
            });
        REQUIRE(numGeoms == 3);
        REQUIRE(numPts == 6);
    }

    SECTION("type") {
        REQUIRE_EVAL_1("geometry.type == 'GeometryCollection'", ValueType::Bool, true);
    }

    SECTION("geo-fn") {
        // Pass sub-geometries unwrapped
        REQUIRE_EVAL_1("count(geo(geometry.geometries.*))", ValueType::Int, 3);
        
        // Implicit resolve "geometry" field
        REQUIRE_EVAL_1("count(geo())", ValueType::Int, 3);

        // Explicit pass geometry object
        REQUIRE_EVAL_1("count(geo(geometry))", ValueType::Int, 3);
    }
}
