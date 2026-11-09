#include "simfil/model/model.h"
#include "simfil/model/schema-pool.h"
#include "simfil/model/schema-node.h"
#include "simfil/value.h"

#include <catch2/catch_test_macros.hpp>
#include <array>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace simfil;

static_assert(noexcept(std::declval<Model const&>().supportsRecursiveCycles()));
static_assert(noexcept(std::declval<Model const&>().recursiveTraversalKey(std::declval<ModelNode const&>())));
static_assert(noexcept(std::declval<ModelNode const&>().borrowedModel()));

namespace
{

/** Immutable native graph with deliberately separate schema/data dictionary namespaces. */
struct RuntimeTypes
{
    std::shared_ptr<SchemaPool> pool = std::make_shared<SchemaPool>();
    TypeId object, speed, count, flags, items;

    explicit RuntimeTypes(std::string identity = "runtime-types", uint8_t width = 64)
    {
        auto objectType = pool->newObjectType();
        auto speedType = pool->newScalarType(SchemaType::Kind::Int);
        auto countType = pool->newScalarType(SchemaType::Kind::Int);
        auto flagType = pool->newBitmaskType(width);
        auto itemType = pool->newArrayType();
        object = objectType.id();
        speed = speedType.id();
        count = countType.id();
        flags = flagType.id();
        items = itemType.id();
        speedType.addNamedValue({"UNLIMITED", int64_t{255}, SchemaType::NamedValue::Role::Special});
        flagType.addFlag("NONE", 0);
        flagType.addFlag("LOW", 1);
        if (width >= 2)
            flagType.addFlag("PAIR", 3);
        objectType.addField("speed", speedType);
        objectType.addField("count", countType);
        objectType.addField("flags", flagType);
        objectType.addField("items", itemType);
        itemType.addElement(speedType);
        if (identity.empty())
            pool->finalize();
        else
            pool->finalize(std::move(identity));
    }

    /** Build untyped inline leaves so field/element provenance must come from their parent. */
    auto model() const -> std::shared_ptr<ModelPool>
    {
        auto result = std::make_shared<ModelPool>();
        REQUIRE(result->strings()->emplace("runtime-padding"));
        result->setSchemaPool(pool);
        auto root = result->newObject();
        REQUIRE(root->setSchema(object));
        REQUIRE(root->addField("speed", int64_t{255}));
        REQUIRE(root->addField("count", int64_t{255}));
        REQUIRE(root->addField("flags", result->newBitmask(1, pool->schemaType(flags))));
        auto array = result->newArray();
        REQUIRE(array->setSchema(items));
        array->append(int64_t{255});
        REQUIRE(root->addField("items", array));
        result->addRoot(root);
        return result;
    }
};

/** Capture native bytes only after the model has passed its graph-transport preflight. */
auto bytes(ModelPool& pool) -> std::vector<uint8_t>
{
    std::ostringstream output;
    REQUIRE(pool.write(output));
    auto text = output.str();
    return {text.begin(), text.end()};
}

/** Constructor-time reads must validate raw rows without requiring shared_from_this on this model. */
class ConstructorReader final : public ModelPool
{
public:
    ConstructorReader(std::vector<uint8_t> const& input, SchemaResolver resolver)
    {
        setSchemaResolver(std::move(resolver));
        loaded = read(input);
    }

    tl::expected<void, Error> loaded;
};

}

TEST_CASE("Ordinary models expose allocation-free recursive traversal defaults", "[model.schema-type]")
{
    auto model = std::make_shared<ModelPool>();
    auto node = model->newObject();
    auto references = model.use_count();
    auto owner = node->borrowedModel();
    REQUIRE(owner == model.get());
    REQUIRE_FALSE(owner->supportsRecursiveCycles());
    REQUIRE(owner->recursiveTraversalKey(*node) == node->addr());
    REQUIRE(model.use_count() == references);
}

TEST_CASE("Schema descriptors describe data types without acquiring runtime provenance", "[model.schema-type]")
{
    RuntimeTypes types;
    auto descriptor = types.pool->schemaType(types.object).modelNode();
    REQUIRE(descriptor->describedType() == types.pool->schemaType(types.object));
    REQUIRE(descriptor->schema() == NoTypeId);
    REQUIRE_FALSE(descriptor->schemaType());
    REQUIRE_FALSE(types.pool->resolveSchemaType(*descriptor));
    REQUIRE_FALSE(Value::field(*descriptor).schemaType());
    auto wrapper = types.pool->resolve(descriptor->addr());
    REQUIRE_FALSE(wrapper->schemaType());
    auto fields = descriptor->get(StringPool::SchemaFields);
    auto field = fields->get(types.pool->dictionary().get("speed"));
    REQUIRE_FALSE(fields->schemaType());
    REQUIRE_FALSE(field->schemaType());
    REQUIRE_FALSE(Value::field(*field).schemaType());
    auto kind = descriptor->get(StringPool::SchemaKind);
    REQUIRE(kind);
    REQUIRE_FALSE(kind->schemaType());
    REQUIRE_FALSE(Value::field(*descriptor, *kind, StringPool::SchemaKind).schemaType());
    REQUIRE(types.pool->supportsRecursiveCycles());
    auto target = types.pool->schemaType(types.speed).modelNode();
    REQUIRE(types.pool->recursiveTraversalKey(*field) == target->addr());
#if defined(SIMFIL_WITH_MODEL_JSON)
    REQUIRE(wrapper->toJson() == descriptor->toJson());
#endif
}

TEST_CASE("Escaping scalar values retain borrowed SchemaType provenance", "[model.schema-type][model.schema-value]")
{
    std::weak_ptr<SchemaPool const> lifetime;
    Value scalar = Value::null();
    Bitmask mask;
    {
        RuntimeTypes types;
        lifetime = types.pool;
        auto borrowed = types.pool->schemaType(types.speed);
        REQUIRE_FALSE(borrowed.ownsPool());
        scalar = Value::make(int64_t{255}).withSchemaType(borrowed);
        mask = Bitmask{3, types.pool->schemaType(types.flags)};
        REQUIRE(scalar.schemaType().ownsPool());
        REQUIRE(scalar.schemaType() == borrowed);
        REQUIRE(mask.type.ownsPool());
        REQUIRE_FALSE(Value::null().withSchemaType(borrowed).schemaType());
        REQUIRE_FALSE(Value::undef().withSchemaType(borrowed).schemaType());
    }
    REQUIRE_FALSE(lifetime.expired());
    REQUIRE(scalar.namedValue("UNLIMITED")->as<ValueType::Int>() == 255);
    REQUIRE(mask.hasFlag("PAIR").value());
    auto copy = scalar;
    scalar = Value::null();
    mask = {};
    REQUIRE_FALSE(lifetime.expired());
    copy = Value::null();
    REQUIRE(lifetime.expired());
}

TEST_CASE("Model schema views preserve exact identity and private owner groups", "[model.schema-type]")
{
    RuntimeTypes types;
    auto first = types.model();
    auto second = types.model();
    auto firstOwner = first->schemaPool(types.pool->identity());
    auto secondOwner = second->schemaPool(types.pool->identity());
    REQUIRE(firstOwner.get() == secondOwner.get());
    REQUIRE((firstOwner.owner_before(secondOwner) || secondOwner.owner_before(firstOwner)));
    auto references = types.pool.use_count();
    auto parent = first->resolveSchemaType(types.object);
    auto child = parent.fieldType(types.pool->dictionary().get("speed"));
    auto element = first->resolveSchemaType(types.items).itemType();
    REQUIRE(child.ownsPool());
    REQUIRE(element.ownsPool());
    REQUIRE(child == second->resolveSchemaType(types.speed));
    REQUIRE(element == child);
    REQUIRE(types.pool.use_count() == references);
    REQUIRE_FALSE(first->resolveSchemaType(NoTypeId));
    REQUIRE_FALSE(first->resolveSchemaType(MaxTypeId));

    auto root = first->root(0).value();
    auto name = first->strings()->get("speed");
    REQUIRE(name != types.pool->dictionary().get("speed"));
    auto speed = Value::field(*root, *root->get(name), name);
    auto array = root->get(first->strings()->get("items"));
    auto item = Value::at(*array, *array->at(0), 0);
    REQUIRE(speed.schemaType() == child);
    REQUIRE(item.schemaType() == child);
    REQUIRE(types.pool.use_count() == references);
}

TEST_CASE("Native copies retain the graph without chaining producer model counters", "[model.schema-type][model.copy]")
{
    RuntimeTypes types;
    auto source = types.model();
    std::weak_ptr<ModelPool> sourceLifetime = source;
    std::weak_ptr<SchemaPool const> localLifetime = source->schemaPool(types.pool->identity());
    auto node = source->newTypedValue(int64_t{255}, types.pool->schemaType(types.speed));
    REQUIRE(node->addr().column() == ModelPool::TypedScalar);
    REQUIRE(node->type() == ValueType::Int);
    REQUIRE(node->value() == ScalarValueType{int64_t{255}});
    REQUIRE(source->serializationSizeStats().typedScalarBytes ==
            sizeof(ModelNodeAddress) + sizeof(TypeId));
    REQUIRE(std::holds_alternative<Value::TypedScalar>(Value::field(*node).value));
    auto destination = std::make_shared<ModelPool>();
    auto copied = destination->copyNode(*node);
    REQUIRE(copied);
    REQUIRE((*copied)->schemaType().pool() == types.pool.get());
    node = {};
    source.reset();
    REQUIRE(sourceLifetime.expired());
    REQUIRE(localLifetime.expired());
    REQUIRE(Value::field(**copied).namedValue("UNLIMITED")->as<ValueType::Int>() == 255);
}

TEST_CASE("Artifact names do not collapse runtime pool identity", "[model.schema-type][model.copy]")
{
    RuntimeTypes first("same-artifact"), second("same-artifact");
    auto one = first.pool->schemaType(first.speed);
    auto two = second.pool->schemaType(second.speed);
    REQUIRE(one.id() == two.id());
    REQUIRE(one != two);
    REQUIRE(one == one.retain());
    auto target = std::make_shared<ModelPool>();
    auto left = target->newTypedValue(int64_t{255}, one);
    auto right = target->newTypedValue(int64_t{255}, two);
    REQUIRE(left->schema() != right->schema());
    REQUIRE(left->schemaType() == one);
    REQUIRE(right->schemaType() == two);
    REQUIRE_FALSE(target->schemaPool("same-artifact"));
    std::ostringstream output;
    output << "unchanged";
    REQUIRE_FALSE(target->write(output));
    REQUIRE(output.str() == "unchanged");

    auto direct = first.model();
    REQUIRE_THROWS(direct->setSchemaPool(second.pool));
    REQUIRE_FALSE(direct->importSchema(two));
    REQUIRE(direct->resolveSchemaType(*right) == two);
    auto escaped = Value::field(*left);
    target->remapSchemas(second.pool, [&](SchemaType const& source) {
        return second.pool->schemaType(source.id());
    });
    REQUIRE(left->schemaType() == two);
    REQUIRE(escaped.schemaType() == one);
    REQUIRE(target->hasDirectSchemaPool("same-artifact"));
}

TEST_CASE("Anonymous schema pools support runtime ownership but not typed transport", "[model.schema-type]")
{
    RuntimeTypes types("");
    auto model = types.model();
    auto value = Value::make(int64_t{255}).withSchemaType(types.pool->schemaType(types.speed));
    REQUIRE(value.schemaType().ownsPool());
    REQUIRE(model->importSchema(value.schemaType()));
    std::ostringstream output;
    output << "prefix";
    REQUIRE_FALSE(model->write(output));
    REQUIRE(output.str() == "prefix");
    std::ostringstream schemaPayload;
    REQUIRE(types.pool->write(schemaPayload));
#if defined(SIMFIL_WITH_MODEL_JSON)
    REQUIRE_THROWS((Bitmask{1, types.pool->schemaType(types.flags)}.toJson()));
#endif
}

TEST_CASE("Multigraph transport resolves exact immutable pools before exposing typed rows", "[model.schema-type]")
{
    RuntimeTypes first("first-artifact"), second("second-artifact");
    std::array values{
        Value::make(int64_t{255}).withSchemaType(first.pool->schemaType(first.speed)),
        Value::make(Bitmask{3, second.pool->schemaType(second.flags)})};
    auto destination = std::make_shared<ModelPool>();
    auto sequence = destination->copySequence(values);
    REQUIRE(sequence);
    destination->addRoot(*sequence);
    auto payload = bytes(*destination);
    auto restored = std::make_shared<ModelPool>();
    REQUIRE_FALSE(restored->read(payload));
    restored->setSchemaResolver([wrong = first.pool](std::string_view) { return wrong; });
    REQUIRE_FALSE(restored->read(payload));
    ModelPool::SchemaResolver resolver = [one = first.pool, two = second.pool](std::string_view identity)
        -> std::shared_ptr<SchemaPool const> {
        if (one->identity() == identity)
            return one;
        return two->identity() == identity ? two : nullptr;
    };
    restored->setSchemaResolver(resolver);
    REQUIRE(restored->read(payload));
    restored->setSchemaResolver({});
    auto root = restored->root(0).value();
    REQUIRE(root->at(0)->schemaType() == first.pool->schemaType(first.speed));
    REQUIRE(root->at(1)->schemaType() == second.pool->schemaType(second.flags));
    auto constructor = std::make_shared<ConstructorReader>(payload, resolver);
    REQUIRE(constructor->loaded);
    REQUIRE(constructor->root(0).value()->at(1)->schemaType() == second.pool->schemaType(second.flags));
}

TEST_CASE("Typed string escapes own both text and schema pool", "[model.schema-type][model.schema-value]")
{
    auto pool = std::make_shared<SchemaPool>();
    auto type = pool->newScalarType(SchemaType::Kind::String);
    pool->finalize("string-artifact");
    std::weak_ptr<SchemaPool const> lifetime = pool;
    Value escaped = Value::null();
    {
        std::string text(1024, 'x');
        escaped = Value::make(std::string_view(text)).withSchemaType(type);
        text.assign(1024, 'y');
    }
    pool.reset();
    REQUIRE_FALSE(lifetime.expired());
    REQUIRE(escaped.as<ValueType::String>() == std::string(1024, 'x'));
    escaped = Value::null();
    REQUIRE(lifetime.expired());
}

TEST_CASE("Failed schema remapping preserves core rows and captured provenance", "[model.schema-type]")
{
    RuntimeTypes first("wide-artifact"), second("narrow-artifact", 1);
    auto target = std::make_shared<ModelPool>();
    auto node = target->newBitmask(3, first.pool->schemaType(first.flags));
    target->addRoot(node);
    auto before = bytes(*target);
    auto escaped = Value::field(*node);
    REQUIRE_THROWS(target->remapSchemas(second.pool, [&](SchemaType const&) {
        return second.pool->schemaType(second.flags);
    }));
    REQUIRE(bytes(*target) == before);
    REQUIRE(node->schemaType() == escaped.schemaType());
    REQUIRE(escaped.as<ValueType::Bitmask>().bits == 3);
}
