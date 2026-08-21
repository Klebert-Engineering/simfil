#include "simfil/diagnostics.h"
#include "simfil/model/nodes.h"
#include "simfil/simfil.h"
#include "simfil/environment.h"
#include "simfil/model/schema.h"
#include "simfil/model/model.h"
#include "simfil/model/json.h"
#include "common.hpp"

#include "fmt/format.h"

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <vector>

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

        if (auto i = schemas.find(id); i != schemas.end())
            return i->second.get();
        return nullptr;
    }

    auto get(SchemaId id) -> Schema*
    {
        if (!enabled)
            return nullptr;

        if (auto i = schemas.find(id); i != schemas.end())
            return i->second.get();
        return nullptr;
    }

    auto finalize() -> void
    {
        auto& self = *this;
        for (const auto& [_, value] : schemas) {
            value->finalize([&self](auto id) { return self(id); });
        }
    }

    auto operator()(SchemaId id) -> Schema*
    {
        return get(id);
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
    const auto enumA = strings->emplace("ENUM_A").value();
    const auto enumB = strings->emplace("ENUM_B").value();
    const auto missingEnum = strings->emplace("MISSING_ENUM").value();

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

        auto lookup = [&schemas](SchemaId schemaId) {
            const auto index = static_cast<std::size_t>(schemaId);
            return index < schemas.size() ? &schemas[index] : nullptr;
        };

        schemas[1].finalize(lookup);

        REQUIRE(schemas[1].canHaveField(a));
        REQUIRE(schemas[1].canHaveField(b));
        REQUIRE_FALSE(schemas[1].canHaveField(c));
    }

    SECTION("cyclic schemas collect reachable fields") {
        std::vector<ObjectSchema> schemas(3);
        schemas[1].addField(link, {SchemaId{2}});
        schemas[1].addField(c);
        schemas[2].addField(back, {SchemaId{1}});

        auto lookup = [&schemas](SchemaId schemaId) {
            const auto index = static_cast<std::size_t>(schemaId);
            return index < schemas.size() ? &schemas[index] : nullptr;
        };

        schemas[1].finalize(lookup);
        schemas[2].finalize(lookup);

        REQUIRE(schemas[1].canHaveField(link));
        REQUIRE(schemas[1].canHaveField(back));
        REQUIRE(schemas[1].canHaveField(c));
        REQUIRE_FALSE(schemas[1].canHaveField(missing));

        REQUIRE(schemas[2].canHaveField(link));
        REQUIRE(schemas[2].canHaveField(back));
        REQUIRE(schemas[2].canHaveField(c));
        REQUIRE_FALSE(schemas[2].canHaveField(missing));
    }

    SECTION("array schemas finalize element fields") {
        ObjectSchema objectA;
        objectA.addField(a);

        ObjectSchema objectB;
        objectB.addField(b);

        ArraySchema arraySchema;
        arraySchema.addElementSchemas({SchemaId{1}, SchemaId{2}});

        auto lookup = [&objectA, &objectB](SchemaId schemaId) -> Schema* {
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

    SECTION("value schemas finalize enum symbols") {
        ValueSchema schema;
        schema.addEnumSymbol(enumB);
        schema.addEnumSymbol(enumA);
        schema.addEnumSymbol(enumA);

        // Dirty value schemas are conservative until finalized.
        REQUIRE(schema.canHaveEnumSymbol(missingEnum));

        schema.finalize([](SchemaId) { return nullptr; });
        REQUIRE(schema.canHaveEnumSymbol(enumA));
        REQUIRE(schema.canHaveEnumSymbol(enumB));
        REQUIRE_FALSE(schema.canHaveEnumSymbol(missingEnum));
        REQUIRE(schema.nestedEnumSymbols().size() == 2);
    }

    SECTION("object and array schemas collect reachable enum symbols") {
        ObjectSchema objectSchema;
        objectSchema.addField(a, {SchemaId{1}});

        ArraySchema arraySchema;
        arraySchema.addElementSchemas({SchemaId{2}});

        ValueSchema enumSchema;
        enumSchema.addEnumSymbol(enumA);
        enumSchema.addEnumSymbol(enumB);

        auto lookup = [&](SchemaId schemaId) -> Schema* {
            switch (schemaId) {
            case SchemaId{1}:
                return &enumSchema;
            case SchemaId{2}:
                return &objectSchema;
            default:
                return nullptr;
            }
        };

        objectSchema.finalize(lookup);
        arraySchema.finalize(lookup);

        REQUIRE(objectSchema.canHaveEnumSymbol(enumA));
        REQUIRE(objectSchema.canHaveEnumSymbol(enumB));
        REQUIRE_FALSE(objectSchema.canHaveEnumSymbol(missingEnum));

        REQUIRE(arraySchema.canHaveEnumSymbol(enumA));
        REQUIRE(arraySchema.canHaveEnumSymbol(enumB));
        REQUIRE_FALSE(arraySchema.canHaveEnumSymbol(missingEnum));
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

TEST_CASE("Schema rewrites enum symbols to exact paths", "[model.schema]")
{
    auto model = json::parse(R"json(
    {
      "status": "Other",
      "items": [
        {"kind": "Other"}
      ],
      "unrelated": {
        "value": "Carrier"
      },
      "CARRIER": 7
    }
    )json").value();

    auto registry = SchemaRegistry{};
    auto strings = model->strings();
    auto status = strings->get("status");
    auto items = strings->get("items");
    auto kind = strings->get("kind");
    auto carrierField = strings->get("CARRIER");
    auto carrierEnum = strings->get("Carrier");

    auto rootSchema = std::make_unique<ObjectSchema>();
    rootSchema->addField(status, {SchemaId{2}});
    rootSchema->addField(items, {SchemaId{3}});
    rootSchema->addField(carrierField);

    auto enumSchema = std::make_unique<ValueSchema>();
    enumSchema->addEnumSymbol(carrierEnum);

    auto arraySchema = std::make_unique<ArraySchema>();
    arraySchema->addElementSchemas({SchemaId{4}});

    auto itemSchema = std::make_unique<ObjectSchema>();
    itemSchema->addField(kind, {SchemaId{2}});

    registry.schemas[SchemaId{1}] = std::move(rootSchema);
    registry.schemas[SchemaId{2}] = std::move(enumSchema);
    registry.schemas[SchemaId{3}] = std::move(arraySchema);
    registry.schemas[SchemaId{4}] = std::move(itemSchema);
    registry.finalize();

    auto root = model->root(0);
    REQUIRE(root);
    auto rootObj = model->resolve<Object>(**root);
    REQUIRE(rootObj);
    REQUIRE(rootObj->setSchema(SchemaId{1}));

    Environment env(strings);
    env.querySchemaCallback = registry.asFunction();

    auto enumAst = compile(env, "Carrier", CompileOptions{
        .any = false,
        .rewriteMode = RewriteMode::Schema,
        .rootSchema = SchemaId{1}});
    REQUIRE(enumAst);
    INFO((*enumAst)->expr().toString());
    REQUIRE((*enumAst)->expr().toString().find("**") == std::string::npos);
    REQUIRE((*enumAst)->expr().toString().find("status") != std::string::npos);
    REQUIRE((*enumAst)->expr().toString().find("kind") != std::string::npos);

    auto enumResult = eval(env, **enumAst, **root, nullptr);
    REQUIRE(enumResult);
    REQUIRE(enumResult->size() == 1);
    REQUIRE(enumResult->front().isa(ValueType::Bool));
    REQUIRE_FALSE(enumResult->front().as<ValueType::Bool>());

    auto fieldAst = compile(env, "CARRIER", CompileOptions{
        .any = false,
        .rewriteMode = RewriteMode::Schema,
        .rootSchema = SchemaId{1}});
    REQUIRE(fieldAst);
    REQUIRE((*fieldAst)->expr().toString() == "**.CARRIER");

    auto enumRefs = referencedSchemaPaths(env, **enumAst, SchemaId{1});
    REQUIRE(enumRefs);
    REQUIRE_FALSE(enumRefs->hasBroadWildcardAccess);
    REQUIRE_FALSE(enumRefs->hasDynamicAccess);
    REQUIRE(enumRefs->paths.size() == 2);
    REQUIRE(std::ranges::all_of(enumRefs->paths, [](auto const& ref) {
        return ref.location == SourceLocation{0, 7};
    }));
    REQUIRE(std::ranges::any_of(enumRefs->paths, [&](auto const& ref) {
        return ref.path.size() == 1 && ref.path[0].field == status && !ref.viaWildcard;
    }));
    REQUIRE(std::ranges::any_of(enumRefs->paths, [&](auto const& ref) {
        return ref.path.size() == 3
            && ref.path[0].field == items
            && ref.path[1].kind == SchemaPathSegment::Kind::ArrayElement
            && ref.path[2].field == kind
            && !ref.viaWildcard;
    }));

    auto fieldRefs = referencedSchemaPaths(env, **fieldAst, SchemaId{1});
    REQUIRE(fieldRefs);
    REQUIRE(fieldRefs->paths.size() == 1);
    REQUIRE(fieldRefs->paths.front().viaWildcard);
    REQUIRE(fieldRefs->paths.front().location == SourceLocation{0, 7});
    REQUIRE(fieldRefs->paths.front().path.size() == 1);
    REQUIRE(fieldRefs->paths.front().path.front().field == carrierField);

    auto unresolvedAst = compile(env, "unrelated.value", CompileOptions{
        .any = false,
        .rewriteMode = RewriteMode::None,
        .rootSchema = SchemaId{1}});
    REQUIRE(unresolvedAst);
    auto unresolvedRefs = referencedSchemaPaths(env, **unresolvedAst, SchemaId{1});
    REQUIRE(unresolvedRefs);
    REQUIRE(unresolvedRefs->paths.empty());
    REQUIRE(unresolvedRefs->hasUnresolvedAccess);

    REQUIRE(strings->get("absent") == StringPool::Empty);
    auto absentAst = compile(env, "absent", CompileOptions{
        .any = false,
        .rewriteMode = RewriteMode::None,
        .rootSchema = SchemaId{1}});
    REQUIRE(absentAst);
    auto absentRefs = referencedSchemaPaths(env, **absentAst, SchemaId{1});
    REQUIRE(absentRefs);
    REQUIRE(absentRefs->paths.empty());
    REQUIRE(absentRefs->hasUnresolvedAccess);
    REQUIRE(strings->get("absent") == StringPool::Empty);

    auto childWildcardAst = compile(env, "*.CARRIER", CompileOptions{
        .any = false,
        .rewriteMode = RewriteMode::None,
        .rootSchema = SchemaId{1}});
    REQUIRE(childWildcardAst);
    auto childWildcardRefs = referencedSchemaPaths(env, **childWildcardAst, SchemaId{1});
    REQUIRE(childWildcardRefs);
    REQUIRE(childWildcardRefs->paths.empty());
    REQUIRE(childWildcardRefs->hasDynamicAccess);
}

TEST_CASE("Schema operand shorthand rewrites only source tokens", "[model.schema]")
{
    auto model = json::parse(R"json(
    {
      "$name": "speed",
      "value": 50,
      "primary": 70,
      "secondary": 90,
      "unit": "MPH"
    }
    )json").value();

    auto strings = model->strings();
    auto alias = strings->emplace("speed").value();
    auto multiAlias = strings->emplace("limit").value();
    auto enumSymbol = strings->emplace("MPH").value();
    auto name = strings->emplace("$name").value();
    auto value = strings->emplace("value").value();
    auto primary = strings->emplace("primary").value();
    auto secondary = strings->emplace("secondary").value();
    auto unit = strings->emplace("unit").value();

    class AliasSchema final : public ObjectSchema
    {
    public:
        AliasSchema(StringId alias, StringId multiAlias, StringId name, StringId value, StringId primary, StringId secondary, StringId unit)
            : alias_(alias)
            , multiAlias_(multiAlias)
            , name_(name)
            , value_(value)
            , primary_(primary)
            , secondary_(secondary)
            , unit_(unit)
        {
            addField(name_);
            addField(value_);
            addField(primary_);
            addField(secondary_);
            addField(unit_, {SchemaId{2}});
        }

        auto symbolEqualityPaths(
            StringId symbol,
            const std::function<const Schema*(SchemaId)>&) const -> std::vector<SchemaPath> override
        {
            if (symbol != alias_)
                return {};
            return {SchemaPath{{SchemaPathSegment::Kind::Field, name_}}};
        }

        auto scalarFieldPathsForSymbol(
            StringId symbol,
            const std::function<const Schema*(SchemaId)>&) const -> std::vector<SchemaPath> override
        {
            if (symbol == alias_) {
                return {SchemaPath{{SchemaPathSegment::Kind::Field, value_}}};
            }
            if (symbol == multiAlias_) {
                return {
                    SchemaPath{{SchemaPathSegment::Kind::Field, primary_}},
                    SchemaPath{{SchemaPathSegment::Kind::Field, secondary_}},
                };
            }
            else {
                return {};
            }
        }

    private:
        StringId alias_;
        StringId multiAlias_;
        StringId name_;
        StringId value_;
        StringId primary_;
        StringId secondary_;
        StringId unit_;
    };

    SchemaRegistry registry;
    auto enumSchema = std::make_unique<ValueSchema>();
    enumSchema->addEnumSymbol(enumSymbol);
    registry.schemas[SchemaId{1}] = std::make_unique<AliasSchema>(alias, multiAlias, name, value, primary, secondary, unit);
    registry.schemas[SchemaId{2}] = std::move(enumSchema);
    registry.finalize();

    auto root = model->root(0);
    REQUIRE(root);
    auto rootObj = model->resolve<Object>(**root);
    REQUIRE(rootObj);
    REQUIRE(rootObj->setSchema(SchemaId{1}));

    Environment env(strings);
    env.querySchemaCallback = registry.asFunction();

    env.constants.insert_or_assign("boundName", Value::make(std::string("speed")));

    auto boundStandaloneAst = compile(env, "boundName", CompileOptions{
        .any = false,
        .rewriteMode = RewriteMode::Schema,
        .rootSchema = SchemaId{1}});
    REQUIRE(boundStandaloneAst);
    auto boundStandaloneResult = eval(env, **boundStandaloneAst, **root, nullptr);
    REQUIRE(boundStandaloneResult);
    REQUIRE(boundStandaloneResult->size() == 1);
    REQUIRE(boundStandaloneResult->front().isa(ValueType::String));
    REQUIRE(boundStandaloneResult->front().as<ValueType::String>() == "speed");

    auto boundOperandAst = compile(env, "$name == boundName", CompileOptions{
        .any = false,
        .rewriteMode = RewriteMode::Schema,
        .rootSchema = SchemaId{1}});
    REQUIRE(boundOperandAst);
    auto boundOperandResult = eval(env, **boundOperandAst, **root, nullptr);
    REQUIRE(boundOperandResult);
    REQUIRE(boundOperandResult->size() == 1);
    REQUIRE(boundOperandResult->front().isa(ValueType::Bool));
    REQUIRE(boundOperandResult->front().as<ValueType::Bool>());

    auto standaloneAst = compile(env, "speed", CompileOptions{
        .any = false,
        .rewriteMode = RewriteMode::Schema,
        .rootSchema = SchemaId{1}});
    REQUIRE(standaloneAst);
    INFO((*standaloneAst)->expr().toString());
    REQUIRE((*standaloneAst)->expr().toString().find("$name") != std::string::npos);
    REQUIRE((*standaloneAst)->expr().toString().find("value") == std::string::npos);

    auto standaloneResult = eval(env, **standaloneAst, **root, nullptr);
    REQUIRE(standaloneResult);
    REQUIRE(standaloneResult->size() == 1);
    REQUIRE(standaloneResult->front().isa(ValueType::Bool));
    REQUIRE(standaloneResult->front().as<ValueType::Bool>());

    auto expressionAst = compile(env, "speed > 40", CompileOptions{
        .any = false,
        .rewriteMode = RewriteMode::Schema,
        .rootSchema = SchemaId{1}});
    REQUIRE(expressionAst);
    INFO((*expressionAst)->expr().toString());
    REQUIRE((*expressionAst)->expr().toString().find("value") != std::string::npos);

    auto expressionResult = eval(env, **expressionAst, **root, nullptr);
    REQUIRE(expressionResult);
    REQUIRE(expressionResult->size() == 1);
    REQUIRE(expressionResult->front().isa(ValueType::Bool));
    REQUIRE(expressionResult->front().as<ValueType::Bool>());

    auto enumOperandAst = compile(env, "unit == MPH", CompileOptions{
        .any = false,
        .rewriteMode = RewriteMode::Schema,
        .rootSchema = SchemaId{1}});
    REQUIRE(enumOperandAst);
    INFO((*enumOperandAst)->expr().toString());
    REQUIRE((*enumOperandAst)->expr().toString().find("\"MPH\"") != std::string::npos);

    auto enumOperandResult = eval(env, **enumOperandAst, **root, nullptr);
    REQUIRE(enumOperandResult);
    REQUIRE(enumOperandResult->size() == 1);
    REQUIRE(enumOperandResult->front().isa(ValueType::Bool));
    REQUIRE(enumOperandResult->front().as<ValueType::Bool>());

    auto quotedAst = compile(env, R"("speed" == speed)", CompileOptions{
        .any = false,
        .rewriteMode = RewriteMode::Schema,
        .rootSchema = SchemaId{1}});
    REQUIRE(quotedAst);
    INFO((*quotedAst)->expr().toString());
    REQUIRE((*quotedAst)->expr().toString().find("\"speed\"") != std::string::npos);
    REQUIRE((*quotedAst)->expr().toString().find("value") != std::string::npos);

    auto anyAlternativeAst = compile(env, "limit > 80", CompileOptions{
        .any = true,
        .rewriteMode = RewriteMode::Schema,
        .rootSchema = SchemaId{1}});
    REQUIRE(anyAlternativeAst);
    INFO((*anyAlternativeAst)->expr().toString());
    REQUIRE((*anyAlternativeAst)->expr().toString().find("(paths") != std::string::npos);
    REQUIRE((*anyAlternativeAst)->expr().toString().find("primary") != std::string::npos);
    REQUIRE((*anyAlternativeAst)->expr().toString().find("secondary") != std::string::npos);

    auto anyAlternativeResult = eval(env, **anyAlternativeAst, **root, nullptr);
    REQUIRE(anyAlternativeResult);
    REQUIRE(anyAlternativeResult->size() == 1);
    REQUIRE(anyAlternativeResult->front().isa(ValueType::Bool));
    REQUIRE(anyAlternativeResult->front().as<ValueType::Bool>());

    auto eachAlternativeAst = compile(env, "each(limit > 80)", CompileOptions{
        .any = false,
        .rewriteMode = RewriteMode::Schema,
        .rootSchema = SchemaId{1}});
    REQUIRE(eachAlternativeAst);
    auto eachAlternativeResult = eval(env, **eachAlternativeAst, **root, nullptr);
    REQUIRE(eachAlternativeResult);
    REQUIRE(eachAlternativeResult->size() == 1);
    REQUIRE(eachAlternativeResult->front().isa(ValueType::Bool));
    REQUIRE_FALSE(eachAlternativeResult->front().as<ValueType::Bool>());

    auto eachAllAlternativeAst = compile(env, "each(limit < 100)", CompileOptions{
        .any = false,
        .rewriteMode = RewriteMode::Schema,
        .rootSchema = SchemaId{1}});
    REQUIRE(eachAllAlternativeAst);
    auto eachAllAlternativeResult = eval(env, **eachAllAlternativeAst, **root, nullptr);
    REQUIRE(eachAllAlternativeResult);
    REQUIRE(eachAllAlternativeResult->size() == 1);
    REQUIRE(eachAllAlternativeResult->front().isa(ValueType::Bool));
    REQUIRE(eachAllAlternativeResult->front().as<ValueType::Bool>());
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

TEST_CASE("WildcardFieldExpr schema plan cache follows schema mutations", "[model.schema]")
{
    auto jsonModel = R"json(
    {
      "target": 123
    }
    )json";
    auto model = json::parse(jsonModel).value();
    auto registry = SchemaRegistry{};
    auto strings = model->strings();
    auto targetId = strings->get("target");
    auto otherId = strings->emplace("other").value();

    const auto rootSchemaId = SchemaId{1};
    auto rootSchema = std::make_unique<ObjectSchema>();
    auto* rootSchemaPtr = rootSchema.get();
    rootSchema->addField(otherId);
    registry.schemas[rootSchemaId] = std::move(rootSchema);
    registry.finalize();

    auto root = model->root(0);
    REQUIRE(root);
    auto rootObj = model->resolve<Object>(*root.value());
    REQUIRE(rootObj);
    REQUIRE(rootObj->setSchema(rootSchemaId));

    Environment env(strings);
    env.querySchemaCallback = registry.asFunction();

    auto ast = compile(env, "**.target", false, false);
    REQUIRE(ast);
    auto sharedAst = SharedAST(std::move(*ast));
    BoundExpression expression(sharedAst, env);

    auto beforeSchemaUpdate = expression.eval(**root);
    REQUIRE(beforeSchemaUpdate);
    REQUIRE(beforeSchemaUpdate->size() == 1);
    REQUIRE((*beforeSchemaUpdate)[0].isa(ValueType::Null));

    rootSchemaPtr->addField(targetId);
    registry.finalize();

    auto afterSchemaUpdate = expression.eval(**root);
    REQUIRE(afterSchemaUpdate);
    REQUIRE(afterSchemaUpdate->size() == 1);
    REQUIRE((*afterSchemaUpdate)[0].toString() == "123");
}

TEST_CASE(
    "Wildcard schema plans are local to concurrent expression bindings",
    "[model.schema][evaluation.binding]")
{
    auto compileModel = json::parse(R"({"target": 0})");
    REQUIRE(compileModel);
    Environment compileEnvironment(compileModel.value()->strings());
    auto compiled = compile(compileEnvironment, "**.target", false, false);
    REQUIRE(compiled);
    auto shared = SharedAST(std::move(*compiled));

    std::vector<std::future<bool>> workers;
    for (auto worker = 0; worker < 8; ++worker) {
        workers.push_back(std::async(
            std::launch::async,
            [shared, worker]
            {
                auto model = json::parse(fmt::format(R"({{"target": {}}})", worker));
                if (!model)
                    return false;

                auto strings = model.value()->strings();
                auto targetId = strings->get("target");
                SchemaRegistry registry;
                auto rootSchema = std::make_unique<ObjectSchema>();
                rootSchema->addField(targetId);
                registry.schemas[SchemaId{1}] = std::move(rootSchema);
                registry.finalize();

                auto root = model.value()->root(0);
                if (!root)
                    return false;
                auto object = model.value()->resolve<Object>(**root);
                if (!object || !object->setSchema(SchemaId{1}))
                    return false;

                Environment environment(strings);
                environment.querySchemaCallback = registry.asFunction();
                BoundExpression expression(shared, environment);
                for (auto iteration = 0; iteration < 100; ++iteration) {
                    auto result = expression.eval(**root);
                    if (!result || result->size() != 1 ||
                        result->front().toString() != std::to_string(worker))
                        return false;
                }
                return true;
            }));
    }

    for (auto& worker : workers)
        CHECK(worker.get());
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

TEST_CASE("Sparse wide schema query performance", "[perf.schema]") {
    if (RUNNING_ON_VALGRIND) { // NOLINT
        SKIP("Skipping benchmarks when running under valgrind");
    }

    constexpr auto objectCount = std::size_t{2'000};
    constexpr auto branchCount = std::size_t{32};

    const auto targetBranchSchemaId = SchemaId{1};
    const auto targetPayloadSchemaId = SchemaId{2};
    const auto noiseBranchSchemaId = SchemaId{3};
    const auto rootObjectSchemaId = SchemaId{4};
    const auto arraySchemaId = SchemaId{5};

    auto strings = std::make_shared<StringPool>();
    auto model = std::make_shared<ModelPool>(strings);
    auto registry = SchemaRegistry{};

    const auto targetId = strings->emplace("target").value();
    const auto payloadId = strings->emplace("payload").value();
    const auto noiseId = strings->emplace("noise").value();

    std::vector<std::string> branchNames;
    std::vector<StringId> branchIds;
    branchNames.reserve(branchCount);
    branchIds.reserve(branchCount);
    for (auto branchIndex = std::size_t{0}; branchIndex < branchCount; ++branchIndex) {
        branchNames.push_back("branch" + std::to_string(branchIndex));
        branchIds.push_back(strings->emplace(branchNames.back()).value());
    }

    auto targetBranchSchema = std::make_unique<ObjectSchema>();
    targetBranchSchema->addField(payloadId, { targetPayloadSchemaId });
    registry.schemas[targetBranchSchemaId] = std::move(targetBranchSchema);

    auto targetPayloadSchema = std::make_unique<ObjectSchema>();
    targetPayloadSchema->addField(targetId);
    registry.schemas[targetPayloadSchemaId] = std::move(targetPayloadSchema);

    auto noiseBranchSchema = std::make_unique<ObjectSchema>();
    noiseBranchSchema->addField(noiseId);
    registry.schemas[noiseBranchSchemaId] = std::move(noiseBranchSchema);

    auto rootObjectSchema = std::make_unique<ObjectSchema>();
    rootObjectSchema->addField(branchIds.front(), { targetBranchSchemaId });
    for (auto branchIndex = std::size_t{1}; branchIndex < branchCount; ++branchIndex)
        rootObjectSchema->addField(branchIds[branchIndex], { noiseBranchSchemaId });
    registry.schemas[rootObjectSchemaId] = std::move(rootObjectSchema);

    auto arraySchema = std::make_unique<ArraySchema>();
    arraySchema->addElementSchemas({ rootObjectSchemaId });
    registry.schemas[arraySchemaId] = std::move(arraySchema);
    registry.finalize();

    auto root = model->newArray(objectCount);
    for (auto objectIndex = std::size_t{0}; objectIndex < objectCount; ++objectIndex) {
        auto obj = model->newObject(branchCount, true);

        auto targetBranch = model->newObject(1, true);
        auto targetPayload = model->newObject(1, true);
        targetPayload->addField("target", int64_t(1));
        REQUIRE(targetPayload->setSchema(targetPayloadSchemaId));
        targetBranch->addField("payload", targetPayload);
        REQUIRE(targetBranch->setSchema(targetBranchSchemaId));
        obj->addField(branchNames.front(), targetBranch);

        for (auto branchIndex = std::size_t{1}; branchIndex < branchCount; ++branchIndex) {
            auto noiseBranch = model->newObject(1, true);
            noiseBranch->addField("noise", static_cast<int64_t>(objectIndex + branchIndex));
            REQUIRE(noiseBranch->setSchema(noiseBranchSchemaId));
            obj->addField(branchNames[branchIndex], noiseBranch);
        }

        REQUIRE(obj->setSchema(rootObjectSchemaId));
        root->append(obj);
    }

    REQUIRE(root->setSchema(arraySchemaId));
    model->addRoot(root);

    Environment env(strings);
    env.querySchemaCallback = registry.asFunction();

    auto modelRoot = model->root(0);
    REQUIRE(modelRoot);

    auto targetAst = compile(env, "count(**.target == 1)", false, false);
    REQUIRE(targetAst);

    auto exactPathAst = compile(env, "count(*." + branchNames.front() + ".payload.target == 1)", false, false);
    REQUIRE(exactPathAst);

    registry.enabled = false;
    BENCHMARK("Query sparse wide field 'target' recursive without schema") {
        auto res = eval(env, **targetAst, **modelRoot, nullptr);
        REQUIRE(res);
        REQUIRE(res->size() == 1);

        auto count = res->front().template as<ValueType::Int>();
        REQUIRE(count == int64_t(objectCount));
        return count;
    };

    registry.enabled = true;
    env.enableWildcardFieldPlans = false;
    BENCHMARK("Query sparse wide field 'target' recursive with basic schema pruning") {
        auto res = eval(env, **targetAst, **modelRoot, nullptr);
        REQUIRE(res);
        REQUIRE(res->size() == 1);

        auto count = res->front().template as<ValueType::Int>();
        REQUIRE(count == int64_t(objectCount));
        return count;
    };

    env.enableWildcardFieldPlans = true;
    BENCHMARK("Query sparse wide field 'target' recursive with schema field plans") {
        auto res = eval(env, **targetAst, **modelRoot, nullptr);
        REQUIRE(res);
        REQUIRE(res->size() == 1);

        auto count = res->front().template as<ValueType::Int>();
        REQUIRE(count == int64_t(objectCount));
        return count;
    };

    registry.enabled = false;
    env.enableWildcardFieldPlans = false;
    BENCHMARK("Query sparse wide field 'target' via exact path without schema") {
        auto res = eval(env, **exactPathAst, **modelRoot, nullptr);
        REQUIRE(res);
        REQUIRE(res->size() == 1);

        auto count = res->front().template as<ValueType::Int>();
        REQUIRE(count == int64_t(objectCount));
        return count;
    };
}
