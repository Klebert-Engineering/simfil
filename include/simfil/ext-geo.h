#pragma once

#include "function.h"
#include "typed-meta-type.h"
#include "model/point.h"

/** Simfil GeoJSON Extension */
namespace simfil::geo
{

/* Custom Operator Names */
static constexpr auto OP_NAME_WITHIN     = "within";
static constexpr auto OP_NAME_CONTAINS   = "contains";
static constexpr auto OP_NAME_INTERSECTS = "intersects";

template<class Precision>
struct Point;

struct BBox;
struct LineString;
struct Polygon;

struct BBox
{
    Point<double> p1, p2;

    auto edges() const -> LineString;
    auto normalized() const -> BBox;

    auto contains(const BBox& b) const -> bool;
    auto contains(const Point<double>& p) const -> bool;
    auto contains(const LineString& p) const -> bool;
    auto contains(const Polygon& p) const -> bool;

    auto intersects(const BBox& b) const -> bool;
    auto intersects(const LineString& p) const -> bool;

    auto operator==(const BBox&) const -> bool;
    auto toString() const -> std::string;
};

struct LineString
{
    std::vector<Point<double>> points;

    auto bbox() const -> BBox;

    auto intersects(const BBox&) const -> bool;
    auto intersects(const Point<double>&) const -> bool;
    auto intersects(const LineString&) const -> bool;
    auto intersects(const Polygon&) const -> bool;

    auto operator==(const LineString&) const -> bool;
    auto toString() const -> std::string;
};

struct Polygon
{
    std::vector<LineString> polys;

    auto bbox() const -> BBox;

    auto contains(const BBox&) const -> bool;
    auto contains(const Point<double>&) const -> bool;
    auto contains(const LineString&) const -> bool;

    auto intersects(const BBox&) const -> bool;
    auto intersects(const LineString&) const -> bool;
    auto intersects(const Polygon&) const -> bool;

    auto operator==(const Polygon&) const -> bool;
    auto toString() const -> std::string;
};

/* NOTE: There is no Multi* struct, because `geo()` returns Multi* geometries
 *       as multiple geometries. */

/** Parses a GeoJSON structure into a GeoJSON value */
class GeoFn : public Function
{
public:
    static GeoFn Fn;

    GeoFn() = default;

    auto ident() const -> const FnInfo& override;
    auto eval(Context, Value, const std::vector<ExprPtr>&, const ResultFn&) const -> Result override;
};

/** GeoJSON geometry constructors */
class PointFn : public Function
{
public:
    static PointFn Fn;

    PointFn() = default;

    auto ident() const -> const FnInfo& override;
    auto eval(Context, Value, const std::vector<ExprPtr>&, const ResultFn&) const -> Result override;
};

class BBoxFn : public Function
{
public:
    static BBoxFn Fn;

    BBoxFn() = default;

    auto ident() const -> const FnInfo& override;
    auto eval(Context, Value, const std::vector<ExprPtr>&, const ResultFn&) const -> Result override;
};

class LineStringFn : public Function
{
public:
    static LineStringFn Fn;

    LineStringFn() = default;

    auto ident() const -> const FnInfo& override;
    auto eval(Context, Value, const std::vector<ExprPtr>&, const ResultFn&) const -> Result override;
};

/* TODO: Not implemented
class PolygonFn : public Function
{
public:
    static PolygonFn Fn;

    PolygonFn() = default;

    auto ident() const -> const FnInfo& override;
    auto eval(Context, const std::vector<ExprPtr>&, const ResultFn&) const -> Result override;
};
*/

/** Simfil Meta-Types */
namespace meta
{
class PointType : public TypedMetaType<Point<double>>
{
public:
    static PointType Type;

    PointType();

    auto make(double x, double y) -> Value;

    auto unaryOp(std::string_view op, const Point<double>& p) const -> Value override;
    auto binaryOp(std::string_view op, const Point<double>& p, const Value& r) const -> Value override;
    auto binaryOp(std::string_view op, const Value& l, const Point<double>& r) const -> Value override;
    auto unpack(const Point<double>& p, std::function<bool(Value)> res) const -> void override;
};

class BBoxType : public TypedMetaType<BBox>
{
public:
    static BBoxType Type;

    BBoxType();

    auto make(BBox) -> Value;
    auto make(double x1, double y1, double x2, double y2) -> Value;

    auto unaryOp(std::string_view op, const BBox& b) const -> Value override;
    auto binaryOp(std::string_view op, const BBox& b, const Value& r) const -> Value override;
    auto binaryOp(std::string_view op, const Value& l, const BBox& r) const -> Value override;
    auto unpack(const BBox& b, std::function<bool(Value)> res) const -> void override;
};

class LineStringType : public TypedMetaType<LineString>
{
public:
    static LineStringType Type;

    LineStringType();

    auto make(std::vector<Point<double>> pts) -> Value;

    auto unaryOp(std::string_view op, const LineString& ls) const -> Value override;
    auto binaryOp(std::string_view op, const LineString& ls, const Value& r) const -> Value override;
    auto binaryOp(std::string_view op, const Value& l, const LineString& r) const -> Value override;
    auto unpack(const LineString& ls, std::function<bool(Value)> res) const -> void override;
};

class PolygonType : public TypedMetaType<Polygon>
{
public:
    static PolygonType Type;

    PolygonType();

    auto make(LineString outer) -> Value;
    auto make(std::vector<LineString> full) -> Value;

    auto unaryOp(std::string_view op, const Polygon&) const -> Value override;
    auto binaryOp(std::string_view op, const Polygon& l, const Value& r) const -> Value override;
    auto binaryOp(std::string_view op, const Value& l, const Polygon& r) const -> Value override;
    auto unpack(const Polygon&, std::function<bool(Value)> res) const -> void override;
};

}

}
