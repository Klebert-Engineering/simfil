#include "common.hpp"
#include "simfil/model/schema-pool.h"

#include <map>
#include <tuple>

namespace
{
/** Count wrapper resolution while retaining a custom column's virtual protocol. */
class CountingPool : public ModelPool
{
public:
    using ModelPool::resolve;
    mutable unsigned resolutions = 0;

    /** Core metadata reads must bypass this path; custom columns must still reach it. */
    auto resolve(ModelNode const& node, ResolveFn const& cb) const -> tl::expected<void, Error> override
    {
        ++resolutions;
        if (node.addr().column() == FirstCustomColumnId) {
            cb(*model_ptr<CustomNode>::make(node));
            return {};
        }
        return ModelPool::resolve(node, cb);
    }

private:
    /** The column index is deliberately unrelated to the core schema arenas. */
    class CustomNode : public ModelNodeBase
    {
    public:
        /** Interpret a custom address without changing its owner. */
        CustomNode(ModelNode const& node, detail::mp_key key) : ModelNodeBase(node, key) {}
        /** Custom nodes remain authoritative for their structural type. */
        auto type() const -> ValueType override { return ValueType::Bool; }
        /** Resolve the custom type independently of core column indices. */
        auto schema() const -> TypeId override { return 42; }
    };
};
}

TEST_CASE("Member lookup reads declared edges without narrowing their types", "[model.schema-lookup]")
{
    auto graph = std::make_shared<SchemaPool>();
    auto object = graph->newObjectType();
    auto integer = graph->newScalarType(SchemaType::Kind::Int);
    auto text = graph->newScalarType(SchemaType::Kind::String);
    object.setOpen(true);
    object.addField("target", integer);
    object.addAlias("alias", "target");
    object.addField("repeated", text).addType(text);
    object.addField("unknown");
    object.addField("mixed", integer).addType(text);
    object.addField("partial").addType(integer);
    object.addField("same", integer);
    object.addField("same", integer);
    auto key = [&](std::string_view name) { return graph->dictionary().get(name); };
    CHECK(object.fieldSchema(key("alias")) == integer.id());
    CHECK(object.fieldSchema(key("target")) == integer.id());
    CHECK(object.fieldSchema(key("repeated")) == text.id());
    CHECK(object.fieldSchema(key("unknown")) == NoTypeId);
    CHECK(object.fieldSchema(key("mixed")) == NoTypeId);
    CHECK(object.fieldSchema(key("partial")) == NoTypeId);
    CHECK(object.fieldSchema(key("same")) == integer.id());
    CHECK_FALSE(object.field("absent"));
    object.addField("alias", text);
    object.addField("same", text);
    CHECK(object.fieldSchema(key("alias")) == text.id());
    CHECK(object.fieldSchema(key("same")) == NoTypeId);

    auto array = graph->newArrayType();
    CHECK(array.itemSchema() == NoTypeId);
    array.addElement(integer);
    array.addElement(integer);
    CHECK(array.itemSchema() == integer.id());
    array.addElement(text);
    CHECK(array.itemSchema() == NoTypeId);
    graph->finalize();
    CHECK(object.fieldSchema(key("alias")) == text.id());
    CHECK(object.fieldSchema(key("target")) == integer.id());
    CHECK(object.fieldSchema(key("partial")) == NoTypeId);
}

TEST_CASE("Finalized union members use canonical point lookups", "[model.schema-lookup]")
{
    auto graph = std::make_shared<SchemaPool>();
    auto combined = graph->newCompositionType(SchemaType::Composition::OneOf);
    auto left = graph->newObjectType(), right = graph->newObjectType();
    auto integer = graph->newScalarType(SchemaType::Kind::Int);
    auto text = graph->newScalarType(SchemaType::Kind::String);
    left.addField("left", integer);
    right.addField("right", text);
    right.addField("union", combined);
    combined.addAlternative(left);
    combined.addAlternative(right);
    graph->finalize();
    auto leftId = graph->dictionary().get("left");
    auto rightId = graph->dictionary().get("right");
    auto unionId = graph->dictionary().get("union");
    auto before = graph->schemaMemoryUsage();
    for (int i = 0; i < 1000; ++i)
    {
        CHECK(left.fieldSchema(leftId) == integer.id());
        CHECK(combined.fieldSchema(leftId) == integer.id());
        CHECK(combined.fieldSchema(rightId) == text.id());
        CHECK(combined.fieldSchema(unionId) == combined.id());
        CHECK(combined.fieldSchema(StringPool::Empty) == NoTypeId);
    }
    CHECK(graph->schemaMemoryUsage().allocatedBytes == before.allocatedBytes);
    CHECK(combined.composition() == SchemaType::Composition::OneOf);
    CHECK(combined.alternatives().size() == 2);
    CHECK(combined.directFields().empty());
}

TEST_CASE("Prepared member lookup preserves incomplete and cyclic metadata", "[model.schema-lookup]")
{
    auto graph = std::make_shared<SchemaPool>();
    auto combined = graph->newCompositionType(SchemaType::Composition::AnyOf);
    auto left = graph->newObjectType(), right = graph->newObjectType();
    auto integer = graph->newScalarType(SchemaType::Kind::Int);
    auto text = graph->newScalarType(SchemaType::Kind::String);
    auto array = graph->newArrayType();
    auto unknown = graph->newType(SchemaType::Kind::Unknown);
    auto emptyAll = graph->newCompositionType(SchemaType::Composition::AllOf);
    left.addField("left", integer);
    right.addField("right", text);
    array.addElement(integer);
    combined.addAlternative(left);
    combined.addAlternative(right);
    SECTION("Closed branches") {}
    SECTION("Open branch without the other member") { right.setOpen(true); }
    SECTION("Open branches retain shared declarations") {
        left.setOpen(true);
        right.setOpen(true);
        right.addField("left", integer).addType(integer);
    }
    SECTION("Conflicting declarations") { right.addField("left", text); }
    SECTION("Unknown declared type") { right.addField("left"); }
    SECTION("Unresolved alternative") { combined.addAlternative({}); }
    SECTION("Unrestricted alternative") { combined.addAlternative(unknown); }
    SECTION("Empty intersection alternative") { combined.addAlternative(emptyAll); }
    SECTION("Self-referential alternative") { combined.addAlternative(combined); }
    SECTION("Array alternative") { combined.addAlternative(array); }
    graph->finalize();
    auto unique = [](auto types) {
        if (types.empty())
            return NoTypeId;
        auto id = types.front();
        return std::ranges::all_of(types, [id](auto other) { return id == other; }) ? id : NoTypeId;
    };
    for (auto name : {"left", "right", "missing"})
    {
        auto field = graph->dictionary().get(name);
        CHECK(combined.fieldSchema(field) == unique(combined.fieldTypes(field)));
    }
    CHECK(combined.itemSchema() == unique(combined.itemTypes()));
    REQUIRE_THROWS_AS(combined.addAlternative({}), std::logic_error);
}

TEST_CASE("Scalar extraction retains the parent's canonical point-lookup type", "[model.schema-lookup]")
{
    auto graph = std::make_shared<SchemaPool>();
    auto objectType = graph->newObjectType();
    auto integer = graph->newScalarType(SchemaType::Kind::Int);
    objectType.addField("measurement", integer);
    graph->finalize("point-lookup");
    auto model = std::make_shared<ModelPool>();
    REQUIRE(model->strings()->emplace("padding"));
    model->setSchemaPool(graph);
    auto object = model->newObject();
    REQUIRE(object->setSchema(objectType.id()));
    REQUIRE(object->addField("measurement", int64_t{42}));
    auto child = object->at(0);
    REQUIRE(child);
    auto field = model->strings()->get("measurement");
    REQUIRE(field != graph->dictionary().get("measurement"));
    CHECK(Value::field(*object, *child, field).schemaType() == integer);
    CHECK(Value::at(*object, *child, 0).schemaType() == integer);
}

TEST_CASE("Core node metadata avoids wrappers without bypassing custom or foreign owners", "[model.schema-lookup]")
{
    auto pool = std::make_shared<CountingPool>();
    auto other = std::make_shared<ModelPool>();
    for (bool singleton : {false, true}) {
        auto object = pool->newObject(singleton ? 1 : 2, singleton);
        auto array = pool->newArray(singleton ? 1 : 2, singleton);
        REQUIRE(object->setSchema(7));
        REQUIRE(array->setSchema(8));
        for (auto const& [node, type, typeRef] : std::vector<std::tuple<ModelNode::Ptr, ValueType, TypeId>>{
                 {object, ValueType::Object, 7}, {array, ValueType::Array, 8}}) {
            auto generic = ModelNode::Ptr::make(pool, node->addr());
            CHECK(generic->type() == type);
            CHECK(generic->schema() == typeRef);
            CHECK(other->type(*generic) == type);
            CHECK(other->schema(*generic) == typeRef);
        }
    }
    for (auto const& [node, type] : std::vector<std::pair<ModelNode::Ptr, ValueType>>{
             {pool->newUndefined(), ValueType::Undef},
             {pool->newSmallValue(true), ValueType::Bool},
             {pool->newValue(int64_t{-1}), ValueType::Int},
             {pool->newValue(int64_t{1}), ValueType::Int},
             {pool->newValue(int64_t{100000}), ValueType::Int},
             {pool->newValue(1.5), ValueType::Float},
             {pool->newValue(std::string_view("text")), ValueType::String},
             {pool->newValue(pool->strings()->emplace("pooled").value()), ValueType::String},
             {pool->newValue(ByteArray("bytes")), ValueType::Bytes}}) {
        auto generic = ModelNode::Ptr::make(pool, node->addr());
        CHECK(generic->type() == type);
        CHECK(generic->schema() == NoTypeId);
    }
    for (auto column : {ModelPool::Int64, ModelPool::Double, ModelPool::String,
                       ModelPool::ByteArray, ModelPool::Bitmask}) {
        auto invalid = pool->resolve(ModelNodeAddress{column, 100000});
        CHECK(invalid->type() == ValueType::Null);
        CHECK(invalid->schema() == NoTypeId);
    }
    CHECK(pool->resolutions == 0);

    auto custom = pool->resolve(ModelNodeAddress{ModelPool::FirstCustomColumnId, 100000});
    CHECK(custom->type() == ValueType::Bool);
    CHECK(custom->schema() == 42);
    CHECK(pool->resolutions == 2);
    CHECK(other->type(*custom) == ValueType::Bool);
    CHECK(other->schema(*custom) == 42);
    CHECK(pool->resolutions == 4);
    REQUIRE(other->resolve(*custom, Model::Lambda([](ModelNode const& node) {
        CHECK(node.type() == ValueType::Bool);
        CHECK(node.schema() == 42);
    })));
    CHECK(pool->resolutions == 5);
}
