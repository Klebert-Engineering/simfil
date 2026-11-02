#include "common.hpp"

#include <map>
#include <tuple>

namespace
{
/** Expose borrowed member metadata only during publication, not runtime reads. */
class IndexedSchema : public Schema
{
public:
    ObjectSchema fields;
    bool published = false;

    /** Model an object adapter with the base class's prepared member index. */
    auto kind() const -> Kind override { return Kind::Object; }

    /** Fail if runtime lookup regresses to enumeration of possible child domains. */
    void forEachDirectField(const std::function<void(StringId, std::span<const SchemaId>)>& fn) const override
    {
        if (published)
            throw std::logic_error("Runtime member lookup must not enumerate schema fields");
        fields.forEachDirectField(fn);
    }
};

/** Keep lookup-only aliases out of canonical member enumeration. */
class AliasedSchema : public ObjectSchema
{
public:
    StringId alias = 0, target = 0;
    /** A real declaration takes precedence over the model's convenience alias. */
    auto canonicalField(StringId field) const -> StringId override
    {
        return field == alias && std::ranges::find(directFields(), alias) == directFields().end() ? target : field;
    }
};

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
        /** Resolve the custom domain independently of core column indices. */
        auto schema() const -> SchemaId override { return 42; }
    };
};
}

TEST_CASE("Member lookup reads declared edges without narrowing their domains", "[model.schema-lookup]")
{
    AliasedSchema object;
    object.alias = 10;
    object.target = 11;
    object.setOpen(true);
    object.addField(11, {2});
    object.addField(12, {3, 3});
    object.addField(13);
    object.addField(14, {2, 3});
    object.addField(15, {0, 2});
    object.addField(16, {2});
    object.addField(16, {2});
    CHECK(object.fieldSchema(10) == 2);
    CHECK(object.fieldSchema(11) == 2);
    CHECK(object.fieldSchema(12) == 3);
    CHECK(object.fieldSchema(13) == NoSchemaId);
    CHECK(object.fieldSchema(14) == NoSchemaId);
    CHECK(object.fieldSchema(15) == NoSchemaId);
    CHECK(object.fieldSchema(16) == 2);
    CHECK(object.fieldSchema(17) == NoSchemaId);
    object.addField(10, {3});
    object.addField(16, {3});
    CHECK(object.fieldSchema(10) == 3);
    CHECK(object.fieldSchema(16) == NoSchemaId);

    ArraySchema array;
    CHECK(array.itemSchema() == NoSchemaId);
    array.addElementSchemas({2, 2});
    CHECK(array.itemSchema() == 2);
    array.addElementSchemas({3});
    CHECK(array.itemSchema() == NoSchemaId);
}

TEST_CASE("Finalized union members use point lookup without revisiting the graph", "[model.schema-lookup]")
{
    IndexedSchema left, right;
    left.fields.addField(10, {4});
    right.fields.addField(11, {5});
    right.fields.addField(12, {1}); // A child union remains a declared union, not an inferred alternative.
    CombinedSchema combined(Schema::Composition::OneOf);
    combined.addAlternative(2);
    combined.addAlternative(3);
    ValueSchema integer(Schema::Kind::Int), text(Schema::Kind::String);
    std::map<SchemaId, Schema*> graph{{1, &combined}, {2, &left}, {3, &right}, {4, &integer}, {5, &text}};
    bool published = false;
    auto lookup = [&](SchemaId id) -> Schema* {
        if (published)
            throw std::logic_error("Runtime member lookup must not traverse logical alternatives");
        auto found = graph.find(id);
        return found == graph.end() ? nullptr : found->second;
    };
    left.finalize(lookup);
    right.finalize(lookup);
    combined.finalize(lookup);
    left.published = right.published = published = true;
    for (int i = 0; i < 1000; ++i) {
        CHECK(left.fieldSchema(10) == 4);
        CHECK(combined.fieldSchema(10) == 4);
        CHECK(combined.fieldSchema(11) == 5);
        CHECK(combined.fieldSchema(12) == 1);
        CHECK(combined.fieldSchema(13) == NoSchemaId);
    }
    CHECK(combined.composition() == Schema::Composition::OneOf);
    CHECK(combined.alternatives().size() == 2);
    CHECK(combined.directFields().empty());
}

TEST_CASE("Prepared member lookup agrees with analysis for incomplete and cyclic metadata", "[model.schema-lookup]")
{
    ObjectSchema left, right;
    left.addField(10, {4});
    right.addField(11, {5});
    CombinedSchema combined(Schema::Composition::AnyOf);
    combined.addAlternative(2);
    combined.addAlternative(3);
    ArraySchema array;
    array.addElementSchemas({4});
    ValueSchema scalar(Schema::Kind::Int);
    Schema unknown;
    CombinedSchema emptyAll(Schema::Composition::AllOf);
    std::map<SchemaId, Schema*> graph{{1, &combined}, {2, &left}, {3, &right}, {4, &scalar},
                                    {5, &scalar}, {6, &array}, {7, &unknown}, {8, &emptyAll}};
    SECTION("Closed branches") {}
    SECTION("Open branch without the other member") { right.setOpen(true); }
    SECTION("Open branches retain shared known declarations") {
        left.setOpen(true);
        right.setOpen(true);
        right.addField(10, {4});
        right.addField(10, {4});
    }
    SECTION("Conflicting declarations") { right.addField(10, {5}); }
    SECTION("Unknown declared domain") { right.addField(10); }
    SECTION("Unresolved alternative") { combined.addAlternative(0); }
    SECTION("Missing alternative") { combined.addAlternative(99); }
    SECTION("Unrestricted alternative") { combined.addAlternative(7); }
    SECTION("Empty intersection alternative") { combined.addAlternative(8); }
    SECTION("Self-referential alternative") { combined.addAlternative(1); }
    SECTION("Array alternative") { combined.addAlternative(6); }
    auto lookup = [&](SchemaId id) -> Schema* {
        auto found = graph.find(id);
        return found == graph.end() ? nullptr : found->second;
    };
    for (auto [id, schema] : graph)
        schema->finalize(lookup);
    for (StringId field : {10, 11, 12}) {
        auto domains = Schema::fieldSchemas(1, lookup, field);
        CHECK(combined.fieldSchema(field) == (domains.size() == 1 ? domains.front() : NoSchemaId));
    }
    auto domains = Schema::itemSchemas(1, lookup);
    CHECK(combined.itemSchema() == (domains.size() == 1 ? domains.front() : NoSchemaId));
    combined.addAlternative(0);
    CHECK(combined.fieldSchema(10) == NoSchemaId);
    CHECK(combined.itemSchema() == NoSchemaId);
}

TEST_CASE("Scalar extraction uses the parent's point lookup rather than schema enumeration", "[model.schema-lookup]")
{
    auto strings = std::make_shared<StringPool>();
    auto field = strings->emplace("value").value();
    auto objectSchema = std::make_shared<IndexedSchema>();
    auto scalarSchema = std::make_shared<ValueSchema>(Schema::Kind::Int);
    objectSchema->fields.addField(field, {2});
    auto lookup = [objectSchema, scalarSchema](SchemaId id) -> Schema* {
        if (id == 1)
            return objectSchema.get();
        return id == 2 ? scalarSchema.get() : nullptr;
    };
    objectSchema->finalize(lookup);
    objectSchema->published = true;
    auto binding = std::make_shared<SchemaBinding>("point-lookup", objectSchema, lookup, strings);
    auto model = std::make_shared<ModelPool>(strings);
    model->setSchemaBinding(binding);
    auto object = model->newObject();
    REQUIRE(object->setSchema(1));
    REQUIRE(object->addField("value", int64_t{42}));
    auto child = object->at(0);
    REQUIRE(child);
    CHECK(Value::field(*object, *child, field).schemaReference() == SchemaReference{binding, 2});
    CHECK(Value::at(*object, *child, 0).schemaReference() == SchemaReference{binding, 2});
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
        for (auto const& [node, type, domain] : std::vector<std::tuple<ModelNode::Ptr, ValueType, SchemaId>>{
                 {object, ValueType::Object, 7}, {array, ValueType::Array, 8}}) {
            auto generic = ModelNode::Ptr::make(pool, node->addr());
            CHECK(generic->type() == type);
            CHECK(generic->schema() == domain);
            CHECK(other->type(*generic) == type);
            CHECK(other->schema(*generic) == domain);
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
        CHECK(generic->schema() == NoSchemaId);
    }
    for (auto column : {ModelPool::Int64, ModelPool::Double, ModelPool::String,
                       ModelPool::ByteArray, ModelPool::Bitmask}) {
        auto invalid = pool->resolve(ModelNodeAddress{column, 100000});
        CHECK(invalid->type() == ValueType::Null);
        CHECK(invalid->schema() == NoSchemaId);
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
