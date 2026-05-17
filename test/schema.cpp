#include "simfil/diagnostics.h"
#include "simfil/model/nodes.h"
#include "simfil/simfil.h"
#include "simfil/environment.h"
#include "simfil/model/schema.h"
#include "simfil/model/model.h"
#include "simfil/model/json.h"
#include "common.hpp"

#include <memory>
#include <map>
#include <catch2/catch_test_macros.hpp>
#include <catch2/benchmark/catch_benchmark.hpp>

using namespace simfil;

namespace
{

class SchemaRegistry
{
public:
    std::map<SchemaId, std::unique_ptr<Schema>> schemas;

    // Enable schema lookup.
    //
    // By having this flag we do not cheat the price of
    // the function call for the no-schema benchmarks instead
    // of setting the environments query pointer to null.
    bool enabled = true;

    auto get(SchemaId id) const -> const Schema*
    {
        if (!enabled)
            return nullptr;

        auto i = schemas.find(id);
        if (i != schemas.end())
            return i->second.get();
        return nullptr;
    }

    auto finalize() -> void
    {
        auto& self = *this;
        for (const auto& [key, value] : schemas) {
            value->finalize([&self](auto id) { return self(id); });
        }
    }

    auto operator()(SchemaId id) -> Schema*
    {
        return const_cast<Schema*>(const_cast<const SchemaRegistry*>(this)->get(id));
    }

    auto operator()(SchemaId id) const -> const Schema*
    {
        return get(id);
    }

    auto asFunction() const & -> std::function<const Schema*(SchemaId)>
    {
        return [this](SchemaId id) {
            return (*this)(id);
        };
    }
};

}

TEST_CASE("Object schema id assignment", "[model.schema]") {
    auto model = std::make_shared<ModelPool>();

    auto obj = model->newObject(0);
    REQUIRE(obj->schema() == NoSchemaId);

    obj->setSchema(SchemaId{1});
    REQUIRE(obj->schema() == SchemaId{1});
}

TEST_CASE("Singleton object schema id assignment", "[model.schema]") {
    auto model = std::make_shared<ModelPool>();

    auto obj = model->newObject(1, true);
    REQUIRE(obj->schema() == NoSchemaId);

    REQUIRE(obj->addField("field", int64_t{1}));
    obj->setSchema(SchemaId{1});
    REQUIRE(obj->schema() == SchemaId{1});
    REQUIRE(model->validate());
}

TEST_CASE("Array schema id assignment", "[model.schema]") {
    auto model = std::make_shared<ModelPool>();

    auto arr = model->newArray(0);
    REQUIRE(arr->schema() == NoSchemaId);

    arr->setSchema(SchemaId{1});
    REQUIRE(arr->schema() == SchemaId{1});
}

TEST_CASE("Singleton array schema id assignment", "[model.schema]") {
    auto model = std::make_shared<ModelPool>();

    auto arr = model->newArray(1, true);
    REQUIRE(arr->schema() == NoSchemaId);

    arr->append(int64_t(1));
    arr->setSchema(SchemaId{1});
    REQUIRE(arr->schema() == SchemaId{1});
    REQUIRE(model->validate());
}

TEST_CASE("Object schema finalization", "[model.schema]") {
    auto strings = std::make_shared<StringPool>();
    const auto a = strings->emplace("a").value();
    const auto b = strings->emplace("b").value();
    const auto c = strings->emplace("c").value();
    const auto link = strings->emplace("link").value();
    const auto back = strings->emplace("back").value();
    const auto missing = strings->emplace("missing").value();

    SECTION("dirty schemas are conservative") {
        ObjectSchema schema;
        schema.addField(a);

        // No finalize() called, so canHaveField must return `true`.
        REQUIRE(schema.canHaveField(a));
        REQUIRE(schema.canHaveField(missing));

        schema.finalize([](SchemaId) { return nullptr; });
        REQUIRE(schema.canHaveField(a));
        REQUIRE(!schema.canHaveField(missing));
    }

    SECTION("acyclic schemas finalize fields") {
        std::vector<ObjectSchema> schemas(3);
        schemas[1].addField(a, {SchemaId{2}});
        schemas[2].addField(b);

        auto lookup = [&](SchemaId schemaId) -> ObjectSchema* {
            const auto index = static_cast<std::size_t>(schemaId);
            return index < schemas.size() ? &schemas[index] : nullptr;
        };

        schemas[1].finalize(lookup);

        REQUIRE(schemas[1].canHaveField(a));
        REQUIRE(schemas[1].canHaveField(b));
        REQUIRE_FALSE(schemas[1].canHaveField(c));
    }

    SECTION("cyclic schemas stay conservative") {
        std::vector<ObjectSchema> schemas(3);
        schemas[1].addField(link, {SchemaId{2}});
        schemas[1].addField(c);
        schemas[2].addField(back, {SchemaId{1}});

        auto lookup = [&](SchemaId schemaId) -> ObjectSchema* {
            const auto index = static_cast<std::size_t>(schemaId);
            return index < schemas.size() ? &schemas[index] : nullptr;
        };

        schemas[1].finalize(lookup);

        REQUIRE(schemas[1].canHaveField(missing));
        REQUIRE(schemas[2].canHaveField(missing));
    }

    SECTION("array schemas finalize element fields") {
        ObjectSchema objectA;
        objectA.addField(a);

        ObjectSchema objectB;
        objectB.addField(b);

        ArraySchema arraySchema;
        arraySchema.addElementSchemas({SchemaId{1}, SchemaId{2}});

        auto lookup = [&](SchemaId schemaId) -> Schema* {
            switch (schemaId) {
            case SchemaId{1}:
                return &objectA;
            case SchemaId{2}:
                return &objectB;
            default:
                return nullptr;
            }
        };

        arraySchema.finalize(lookup);

        REQUIRE(arraySchema.canHaveField(a));
        REQUIRE(arraySchema.canHaveField(b));
        REQUIRE_FALSE(arraySchema.canHaveField(c));
    }
}

TEST_CASE("Array schema serialization", "[model.schema]") {
    auto model = std::make_shared<ModelPool>();
    auto arr = model->newArray(1);
    arr->append(int64_t(42));
    REQUIRE(arr->setSchema(SchemaId{7}));
    model->addRoot(arr);

    std::stringstream stream;
    REQUIRE(model->write(stream));

    const auto input = std::vector<uint8_t>(std::istreambuf_iterator<char>(stream), {});
    auto recoveredModel = std::make_shared<ModelPool>();
    REQUIRE(recoveredModel->read(input));

    auto recoveredRoot = recoveredModel->root(0);
    REQUIRE(recoveredRoot);
    REQUIRE((*recoveredRoot)->type() == ValueType::Array);
    REQUIRE((*recoveredRoot)->schema() == SchemaId{7});
}

// A minimal test that makes sure a field not in the schema
// is pruned if we query for it via **.field.
TEST_CASE("WildcardFieldExpr Field Pruning", "[model.schema]")
{
    auto jsonModel = R"json(
    {
      "field": 123
    }
    )json";
    auto model = json::parse(jsonModel).value();
    auto registry = SchemaRegistry{};
    auto strings = model->strings();
    auto fieldId = strings->get("field");

    // We need to add "noField" to the StringPool to prevent
    // evaluation skipping the expression.
    (void)strings->emplace("noField");

    // Build a simple schema
    auto schemaName = strings->emplace("schema1").value();
    auto schema1 = std::make_unique<ObjectSchema>();
    schema1->addField(fieldId, { NoSchemaId });

    registry.schemas[(SchemaId)schemaName] = std::move(schema1);
    registry.finalize();

    // Assign schemas to the model
    auto root = model->root(0);
    REQUIRE(root);

    auto rootObj = model->resolve<Object>(*root.value());
    REQUIRE(rootObj);
    REQUIRE(rootObj->setSchema((SchemaId)schemaName));
    REQUIRE(rootObj->schema() == (SchemaId)schemaName);

    // Run a query and check if pruning of unknown fields works
    Environment env(strings);
    env.querySchemaCallback = registry.asFunction();

    auto ast = compile(env, "**.noField", false, false);
    REQUIRE(ast);

    Diagnostics diagWithPruning;
    registry.enabled = true;
    auto resultWithPruning = eval(env, *ast.value(), *model->root(0).value(), &diagWithPruning);
    REQUIRE(resultWithPruning);

    Diagnostics diagNoPruning;
    registry.enabled = false;
    auto resultNoPruning = eval(env, *ast.value(), *model->root(0).value(), &diagNoPruning);
    REQUIRE(resultNoPruning);

    // We compare field evaluations for both runs
    auto withPruningData = diagWithPruning.fieldData_[0];
    auto noPruningData = diagNoPruning.fieldData_[0];
    REQUIRE(withPruningData.evaluations < noPruningData.evaluations);
}

TEST_CASE("WildcardFieldExpr Array Field Pruning", "[model.schema]")
{
    auto jsonModel = R"json(
    [
      {
        "field": 123
      }
    ]
    )json";
    auto model = json::parse(jsonModel).value();
    auto registry = SchemaRegistry{};
    auto strings = model->strings();
    auto fieldId = strings->get("field");

    (void)strings->emplace("noField");

    constexpr auto objectSchemaId = SchemaId{1};
    constexpr auto arraySchemaId = SchemaId{2};

    auto objectSchema = std::make_unique<ObjectSchema>();
    objectSchema->addField(fieldId, { NoSchemaId });
    registry.schemas[objectSchemaId] = std::move(objectSchema);

    auto arraySchema = std::make_unique<ArraySchema>();
    arraySchema->addElementSchemas({objectSchemaId});
    registry.schemas[arraySchemaId] = std::move(arraySchema);
    registry.finalize();

    auto root = model->root(0);
    REQUIRE(root);
    auto rootArray = model->resolve<Array>(*root.value());

    REQUIRE(rootArray);
    REQUIRE(rootArray->setSchema(arraySchemaId));
    REQUIRE(rootArray->schema() == arraySchemaId);

    Environment env(strings);
    env.querySchemaCallback = registry.asFunction();

    auto ast = compile(env, "**.noField", false, false);
    REQUIRE(ast);

    auto modelRoot = model->root(0);
    REQUIRE(modelRoot);

    Diagnostics diagWithPruning;
    registry.enabled = true;
    auto resultWithPruning = eval(env, **ast, **modelRoot, &diagWithPruning);
    REQUIRE(resultWithPruning);

    Diagnostics diagNoPruning;
    registry.enabled = false;
    auto resultNoPruning = eval(env, **ast, **modelRoot, &diagNoPruning);
    REQUIRE(resultNoPruning);

    auto withPruningData = diagWithPruning.fieldData_[0];
    auto noPruningData = diagNoPruning.fieldData_[0];
    REQUIRE(withPruningData.evaluations < noPruningData.evaluations);
}

TEST_CASE("WildcardFieldExpr non-recursive queries ignore partial root schemas", "[model.schema]")
{
    auto jsonModel = R"json(
    {
      "object": {
        "field": 123
      }
    }
    )json";
    auto model = json::parse(jsonModel).value();
    auto registry = SchemaRegistry{};
    auto strings = model->strings();
    auto objectId = strings->get("object");
    (void)strings->emplace("field");

    const auto rootSchemaId = SchemaId{1};
    auto rootSchema = std::make_unique<ObjectSchema>();
    rootSchema->addField(objectId, { NoSchemaId });
    registry.schemas[rootSchemaId] = std::move(rootSchema);
    registry.finalize();

    auto root = model->root(0);
    REQUIRE(root);
    auto rootObj = model->resolve<Object>(*root.value());
    REQUIRE(rootObj);
    REQUIRE(rootObj->setSchema(rootSchemaId));

    Environment env(strings);
    env.querySchemaCallback = registry.asFunction();

    auto ast = compile(env, "*.field", false, false);
    REQUIRE(ast);

    auto result = eval(env, **ast, **root, nullptr);
    REQUIRE(result);
    REQUIRE(result->size() == 1);
    REQUIRE((*result)[0].toString() == "123");
}

TEST_CASE("Schema query performance", "[perf.schema]") {
    if (RUNNING_ON_VALGRIND) { // NOLINT
        SKIP("Skipping benchmarks when running under valgrind");
    }

    constexpr auto n = std::size_t{10'000};
    static_assert(n % 2 == 0, "n must be even");

    const auto payloadASchemaId = SchemaId{1};
    const auto payloadBSchemaId = SchemaId{2};
    const auto xASchemaId = SchemaId{3};
    const auto xBSchemaId = SchemaId{4};
    const auto yASchemaId = SchemaId{5};
    const auto yBSchemaId = SchemaId{6};
    const auto rootObjASchemaId = SchemaId{7};
    const auto rootObjBSchemaId = SchemaId{8};
    const auto arraySchemaId = SchemaId{9};

    auto strings = std::make_shared<StringPool>();
    auto model = std::make_shared<ModelPool>(strings);
    auto registry = SchemaRegistry{};

    const auto aId = strings->emplace("a").value();
    const auto bId = strings->emplace("b").value();
    const auto yId = strings->emplace("y").value();
    const auto xId = strings->emplace("x").value();
    const auto missingId = strings->emplace("missing").value();
    const auto payloadId = strings->emplace("payload").value();

    auto payloadASchema = std::make_unique<ObjectSchema>();
    payloadASchema->addField(xId, { xASchemaId });
    registry.schemas[payloadASchemaId] = std::move(payloadASchema);

    auto payloadBSchema = std::make_unique<ObjectSchema>();
    payloadBSchema->addField(xId, { xBSchemaId });
    registry.schemas[payloadBSchemaId] = std::move(payloadBSchema);

    auto xASchema = std::make_unique<ObjectSchema>();
    xASchema->addField(yId, { yASchemaId });
    registry.schemas[xASchemaId] = std::move(xASchema);

    auto xBSchema = std::make_unique<ObjectSchema>();
    xBSchema->addField(yId, { yBSchemaId });
    registry.schemas[xBSchemaId] = std::move(xBSchema);

    auto yASchema = std::make_unique<ObjectSchema>();
    yASchema->addField(aId);
    registry.schemas[yASchemaId] = std::move(yASchema);

    auto yBSchema = std::make_unique<ObjectSchema>();
    yBSchema->addField(bId);
    registry.schemas[yBSchemaId] = std::move(yBSchema);

    auto rootObjASchema = std::make_unique<ObjectSchema>();
    rootObjASchema->addField(payloadId, { payloadASchemaId });
    registry.schemas[rootObjASchemaId] = std::move(rootObjASchema);

    auto rootObjBSchema = std::make_unique<ObjectSchema>();
    rootObjBSchema->addField(payloadId, { payloadBSchemaId });
    registry.schemas[rootObjBSchemaId] = std::move(rootObjBSchema);

    auto arraySchema = std::make_unique<ArraySchema>();
    arraySchema->addElementSchemas({ rootObjASchemaId, rootObjBSchemaId });
    registry.schemas[arraySchemaId] = std::move(arraySchema);
    registry.finalize();

    auto root = model->newArray(n);
    for (auto i = 0u; i < n; ++i) {
        auto obj = model->newObject(1, true);
        auto payload = model->newObject(1, true);
        auto x = model->newObject(1, true);
        auto y = model->newObject(1, true);

        if (i % 2 == 0) {
            y->addField("a", int64_t(1));
            y->setSchema(yASchemaId);
            x->setSchema(xASchemaId);
            payload->setSchema(payloadASchemaId);
            obj->setSchema(rootObjASchemaId);
        } else {
            y->addField("b", int64_t(1));
            y->setSchema(yBSchemaId);
            x->setSchema(xBSchemaId);
            payload->setSchema(payloadBSchemaId);
            obj->setSchema(rootObjBSchemaId);
        }

        x->addField("y", y);
        payload->addField("x", x);
        obj->addField("payload", payload);
        root->append(obj);
    }

    REQUIRE(root->setSchema(arraySchemaId));
    model->addRoot(root);

    Environment env(strings);
    env.querySchemaCallback = registry.asFunction();

    auto modelRoot = model->root(0);
    REQUIRE(modelRoot);

    auto aAst = compile(env, "count(**.a == 1)", false, false);
    REQUIRE(aAst);

    auto missingAst = compile(env, "count(**.missing == 1)", false, false);
    REQUIRE(missingAst);

    registry.enabled = false;
    BENCHMARK("Query nested field 'a' recursive without schema") {
        auto res = eval(env, **aAst, **modelRoot, nullptr);
        REQUIRE(res);
        REQUIRE(res->size() == 1);

        auto count = res->front().template as<ValueType::Int>();
        REQUIRE(count == int64_t(n / 2));
        return count;
    };

    BENCHMARK("Query missing field 'missing' without schema") {
        auto res = eval(env, **missingAst, **modelRoot, nullptr);
        REQUIRE(res);
        REQUIRE(res->size() == 1);

        auto count = res->front().template as<ValueType::Int>();
        REQUIRE(count == 0);
        return count;
    };

    registry.enabled = true;
    BENCHMARK("Query nested field 'a' recursive with schema") {
        auto res = eval(env, **aAst, **modelRoot, nullptr);
        REQUIRE(res);
        REQUIRE(res->size() == 1);

        auto count = res->front().template as<ValueType::Int>();
        REQUIRE(count == int64_t(n / 2));
        return count;
    };

    BENCHMARK("Query missing field 'missing' with schema") {
        auto res = eval(env, **missingAst, **modelRoot, nullptr);
        REQUIRE(res);
        REQUIRE(res->size() == 1);

        auto count = res->front().template as<ValueType::Int>();
        REQUIRE(count == 0);
        return count;
    };
}
