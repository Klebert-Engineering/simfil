#include "simfil/diagnostics.h"
#include "simfil/model/nodes.h"
#include "simfil/simfil.h"
#include "simfil/environment.h"
#include "simfil/model/schema-pool.h"
#include "simfil/model/model.h"
#include "simfil/model/json.h"
#include "common.hpp"

#include "fmt/format.h"

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace simfil;

namespace
{

/** Test domain hooks keep symbolic aliases on the canonical graph owner. */
class AliasPool final : public SchemaPool
{
public:
    explicit AliasPool(std::shared_ptr<StringPool> strings) : SchemaPool(std::move(strings)) {}

protected:
    auto symbolEqualityPaths(TypeId root, StringId symbol) const -> std::vector<SchemaPath> override
    {
        if (root != TypeId{1} || symbol != dictionary().get("speed"))
            return {};
        return {SchemaPath{{SchemaPathSegment::Kind::Field, dictionary().get("$name")}}};
    }

    auto scalarFieldPathsForSymbol(TypeId root, StringId symbol) const -> std::vector<SchemaPath> override
    {
        if (root != TypeId{1})
            return {};
        if (symbol == dictionary().get("speed"))
            return {SchemaPath{{SchemaPathSegment::Kind::Field, dictionary().get("value")}}};
        if (symbol == dictionary().get("limit")) {
            return {
                SchemaPath{{SchemaPathSegment::Kind::Field, dictionary().get("primary")}},
                SchemaPath{{SchemaPathSegment::Kind::Field, dictionary().get("secondary")}},
            };
        }
        return {};
    }
};

/** Evaluate a shared wildcard AST against one worker-local model and schema. */
auto evaluateSharedSchemaExpression(const SharedAST& shared, std::size_t worker) -> bool
{
    // Exercise independent schema bindings, not the JSON parser's process-global locale state.
    auto model = std::make_shared<ModelPool>();
    auto object = model->newObject();
    if (!object->addField("target", static_cast<int64_t>(worker)) || !object->setSchema(TypeId{1}))
        return false;
    model->addRoot(object);

    auto strings = model->strings();
    auto graph = std::make_shared<SchemaPool>();
    auto rootSchema = graph->newObjectType();
    rootSchema.addField("target", graph->newScalarType(SchemaType::Kind::Int));
    graph->finalize();

    Environment environment(strings);
    environment.setSchema(rootSchema);
    BoundExpression expression(shared, environment);
    for (auto iteration = 0; iteration < 100; ++iteration) {
        auto result = expression.eval(*object);
        if (!result || result->size() != 1 ||
            result->front().toString() != std::to_string(worker))
            return false;
    }
    return true;
}

}

TEST_CASE("Object schema id assignment", "[model.schema]") {
    auto model = std::make_shared<ModelPool>();

    auto obj = model->newObject(0);
    REQUIRE(obj->schema() == NoTypeId);

    obj->setSchema(TypeId{1});
    REQUIRE(obj->schema() == TypeId{1});
}

TEST_CASE("Singleton object schema id assignment", "[model.schema]") {
    auto model = std::make_shared<ModelPool>();

    auto obj = model->newObject(1, true);
    REQUIRE(obj->schema() == NoTypeId);

    REQUIRE(obj->addField("field", int64_t{1}));
    obj->setSchema(TypeId{1});
    REQUIRE(obj->schema() == TypeId{1});
    REQUIRE(model->validate());
}

TEST_CASE("Array schema id assignment", "[model.schema]") {
    auto model = std::make_shared<ModelPool>();

    auto arr = model->newArray(0);
    REQUIRE(arr->schema() == NoTypeId);

    arr->setSchema(TypeId{1});
    REQUIRE(arr->schema() == TypeId{1});
}

TEST_CASE("Singleton array schema id assignment", "[model.schema]") {
    auto model = std::make_shared<ModelPool>();

    auto arr = model->newArray(1, true);
    REQUIRE(arr->schema() == NoTypeId);

    arr->append(int64_t(1));
    arr->setSchema(TypeId{1});
    REQUIRE(arr->schema() == TypeId{1});
    REQUIRE(model->validate());
}

TEST_CASE("Object schema finalization", "[model.schema]") {
    auto graph = std::make_shared<SchemaPool>();
    auto strings = graph->strings();
    const auto a = strings->emplace("a").value();
    const auto b = strings->emplace("b").value();
    const auto c = strings->emplace("c").value();
    const auto link = strings->emplace("link").value();
    const auto back = strings->emplace("back").value();
    const auto missing = strings->emplace("missing").value();
    const auto enumA = strings->emplace("ENUM_A").value();
    const auto enumB = strings->emplace("ENUM_B").value();
    const auto missingEnum = strings->emplace("MISSING_ENUM").value();
    auto scalar = graph->newScalarType(SchemaType::Kind::Value);

    SECTION("dirty schemas are conservative") {
        auto schema = graph->newObjectType();
        schema.addField("a");
        REQUIRE(schema.canHaveField(a));
        REQUIRE(schema.canHaveField(missing));

        graph->finalize();
        REQUIRE(schema.canHaveField(a));
        // Finalization cannot turn an unspecified child type into a scalar.
        REQUIRE(schema.canHaveField(missing));
    }

    SECTION("acyclic schemas finalize fields") {
        auto parent = graph->newObjectType();
        auto child = graph->newObjectType();
        parent.addField("a", child);
        child.addField("b", scalar);
        graph->finalize();

        REQUIRE(parent.canHaveField(a));
        REQUIRE(parent.canHaveField(b));
        REQUIRE_FALSE(parent.canHaveField(c));
    }

    SECTION("cyclic schemas collect reachable fields") {
        auto parent = graph->newObjectType();
        auto child = graph->newObjectType();
        parent.addField("link", child);
        parent.addField("c", scalar);
        child.addField("back", parent);
        graph->finalize();

        for (auto schema : {parent, child}) {
            REQUIRE(schema.canHaveField(link));
            REQUIRE(schema.canHaveField(back));
            REQUIRE(schema.canHaveField(c));
            REQUIRE_FALSE(schema.canHaveField(missing));
        }
    }

    SECTION("array schemas finalize element fields") {
        auto objectA = graph->newObjectType();
        objectA.addField("a", scalar);
        auto objectB = graph->newObjectType();
        objectB.addField("b", scalar);
        auto arraySchema = graph->newArrayType();
        arraySchema.addElement(objectA);
        arraySchema.addElement(objectB);
        graph->finalize();

        REQUIRE(arraySchema.canHaveField(a));
        REQUIRE(arraySchema.canHaveField(b));
        REQUIRE_FALSE(arraySchema.canHaveField(c));
    }

    SECTION("value schemas finalize enum symbols") {
        auto schema = graph->newEnumType();
        schema.addSymbol("ENUM_B");
        schema.addSymbol("ENUM_A");
        schema.addSymbol("ENUM_A");

        // Unpublished graphs cannot safely exclude a symbol yet.
        REQUIRE(schema.canHaveEnumSymbol(missingEnum));
        graph->finalize();
        REQUIRE(schema.canHaveEnumSymbol(enumA));
        REQUIRE(schema.canHaveEnumSymbol(enumB));
        REQUIRE_FALSE(schema.canHaveEnumSymbol(missingEnum));
        REQUIRE(schema.nestedEnumSymbols().size() == 2);
    }

    SECTION("object and array schemas collect reachable enum symbols") {
        auto objectSchema = graph->newObjectType();
        auto arraySchema = graph->newArrayType();
        auto enumSchema = graph->newEnumType();
        objectSchema.addField("a", enumSchema);
        arraySchema.addElement(objectSchema);
        enumSchema.addSymbol("ENUM_A");
        enumSchema.addSymbol("ENUM_B");
        graph->finalize();

        for (SchemaType schema : {SchemaType(objectSchema), SchemaType(arraySchema)}) {
            REQUIRE(schema.canHaveEnumSymbol(enumA));
            REQUIRE(schema.canHaveEnumSymbol(enumB));
            REQUIRE_FALSE(schema.canHaveEnumSymbol(missingEnum));
        }
    }
}

TEST_CASE("Array schema serialization", "[model.schema]") {
    auto model = std::make_shared<ModelPool>();
    auto arr = model->newArray(1);
    arr->append(int64_t(42));
    REQUIRE(arr->setSchema(TypeId{7}));
    model->addRoot(arr);

    std::stringstream stream;
    REQUIRE(model->write(stream));

    const auto input = std::vector<uint8_t>(std::istreambuf_iterator<char>(stream), {});
    auto recoveredModel = std::make_shared<ModelPool>();
    REQUIRE(recoveredModel->read(input));

    auto recoveredRoot = recoveredModel->root(0);
    REQUIRE(recoveredRoot);
    REQUIRE((*recoveredRoot)->type() == ValueType::Array);
    REQUIRE((*recoveredRoot)->schema() == TypeId{7});
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

    auto strings = model->strings();
    auto status = strings->get("status");
    auto items = strings->get("items");
    auto kind = strings->get("kind");
    auto carrierField = strings->get("CARRIER");
    auto graph = std::make_shared<SchemaPool>(std::make_shared<StringPool>(*strings));
    auto rootSchema = graph->newObjectType();
    auto enumSchema = graph->newEnumType();
    auto arraySchema = graph->newArrayType();
    auto itemSchema = graph->newObjectType();
    auto scalarSchema = graph->newScalarType(SchemaType::Kind::Value);
    rootSchema.addField("status", enumSchema);
    rootSchema.addField("items", arraySchema);
    rootSchema.addField("CARRIER", scalarSchema);
    enumSchema.addSymbol("Carrier");
    arraySchema.addElement(itemSchema);
    itemSchema.addField("kind", enumSchema);
    graph->finalize();

    auto root = model->root(0);
    REQUIRE(root);
    auto rootObj = model->resolve<Object>(**root);
    REQUIRE(rootObj);
    REQUIRE(rootObj->setSchema(TypeId{1}));

    Environment env(strings);
    env.setSchema(rootSchema);

    auto enumAst = compile(env, "Carrier", CompileOptions{
        .any = false,
        .rewriteMode = RewriteMode::Schema,
        .rootSchema = rootSchema});
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
        .rootSchema = rootSchema});
    REQUIRE(fieldAst);
    REQUIRE((*fieldAst)->expr().toString() == "**.CARRIER");

    auto enumRefs = referencedSchemaPaths(env, **enumAst, rootSchema);
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

    auto fieldRefs = referencedSchemaPaths(env, **fieldAst, rootSchema);
    REQUIRE(fieldRefs);
    REQUIRE(fieldRefs->paths.size() == 1);
    REQUIRE(fieldRefs->paths.front().viaWildcard);
    REQUIRE(fieldRefs->paths.front().location == SourceLocation{0, 7});
    REQUIRE(fieldRefs->paths.front().path.size() == 1);
    REQUIRE(fieldRefs->paths.front().path.front().field == carrierField);

    auto unresolvedAst = compile(env, "unrelated.value", CompileOptions{
        .any = false,
        .rewriteMode = RewriteMode::None,
        .rootSchema = rootSchema});
    REQUIRE(unresolvedAst);
    auto unresolvedRefs = referencedSchemaPaths(env, **unresolvedAst, rootSchema);
    REQUIRE(unresolvedRefs);
    REQUIRE(unresolvedRefs->paths.empty());
    REQUIRE(unresolvedRefs->hasUnresolvedAccess);

    REQUIRE(strings->get("absent") == StringPool::Empty);
    auto absentAst = compile(env, "absent", CompileOptions{
        .any = false,
        .rewriteMode = RewriteMode::None,
        .rootSchema = rootSchema});
    REQUIRE(absentAst);
    auto absentRefs = referencedSchemaPaths(env, **absentAst, rootSchema);
    REQUIRE(absentRefs);
    REQUIRE(absentRefs->paths.empty());
    REQUIRE(absentRefs->hasUnresolvedAccess);
    REQUIRE(strings->get("absent") == StringPool::Empty);

    auto childWildcardAst = compile(env, "*.CARRIER", CompileOptions{
        .any = false,
        .rewriteMode = RewriteMode::None,
        .rootSchema = rootSchema});
    REQUIRE(childWildcardAst);
    auto childWildcardRefs = referencedSchemaPaths(env, **childWildcardAst, rootSchema);
    REQUIRE(childWildcardRefs);
    REQUIRE(childWildcardRefs->paths.empty());
    REQUIRE(childWildcardRefs->hasDynamicAccess);
}

TEST_CASE("Schema references expand recursive wildcard paths with a static suffix", "[model.schema]")
{
    auto model = json::parse(R"({
        "numLanes": {"normalLanes": 1},
        "nested": {"numLanes": {"normalLanes": 2}, "other": {"normalLanes": 3}}
    })").value();
    auto strings = model->strings();
    auto numLanes = strings->get("numLanes");
    auto normalLanes = strings->get("normalLanes");
    auto nested = strings->get("nested");
    auto other = strings->get("other");
    auto graph = std::make_shared<SchemaPool>(std::make_shared<StringPool>(*strings));
    auto rootSchema = graph->newObjectType();
    auto container = graph->newObjectType();
    auto lanes = graph->newObjectType();
    auto scalar = graph->newScalarType(SchemaType::Kind::Value);
    rootSchema.addField("numLanes", lanes);
    rootSchema.addField("nested", container);
    container.addField("numLanes", lanes);
    container.addField("other", lanes);
    lanes.addField("normalLanes", scalar);
    graph->finalize();
    Environment env(strings);
    env.setSchema(rootSchema);

    for (auto query : {"**.numLanes.normalLanes", "**.numLanes[\"normalLanes\"]",
                       "**.numLanes.normalLanes > 1", "**.numLanes.normalLanes == 2",
                       "**.numLanes.normalLanes == \"TWO\"", "\"TWO\" == **.numLanes.normalLanes"}) {
        CAPTURE(query);
        auto ast = compile(env, query, CompileOptions{
            .any = false, .rewriteMode = RewriteMode::Schema, .rootSchema = rootSchema});
        REQUIRE(ast);
        auto refs = referencedSchemaPaths(env, **ast, rootSchema);
        REQUIRE(refs);
        REQUIRE_FALSE(refs->hasUnresolvedAccess);
        REQUIRE_FALSE(refs->hasBroadWildcardAccess);
        REQUIRE(refs->paths.size() == 2);
        for (auto const& ref : refs->paths) {
            REQUIRE(ref.viaWildcard);
            REQUIRE(ref.path.back().field == normalLanes);
            REQUIRE(ref.path[ref.path.size() - 2].field == numLanes);
        }
        REQUIRE(std::ranges::any_of(refs->paths, [](auto const& ref) {return ref.path.size() == 2;}));
        REQUIRE(std::ranges::any_of(refs->paths, [&](auto const& ref) {
            return ref.path.size() == 3 && ref.path.front().field == nested;
        }));
    }

    SECTION("Static paths still refer only to the root") {
        auto ast = compile(env, "numLanes.normalLanes", CompileOptions{
            .any = false, .rewriteMode = RewriteMode::Schema, .rootSchema = rootSchema});
        REQUIRE(ast);
        auto refs = referencedSchemaPaths(env, **ast, rootSchema);
        REQUIRE(refs);
        REQUIRE(refs->paths.size() == 1);
        REQUIRE_FALSE(refs->paths.front().viaWildcard);
        REQUIRE(refs->paths.front().path.size() == 2);
    }
}

TEST_CASE("String equality references distinguish possible search owners", "[model.schema]")
{
    auto graph = std::make_shared<SchemaPool>();
    auto root = graph->newObjectType();
    auto number = graph->newScalarType(SchemaType::Kind::Int);
    auto text = graph->newScalarType(SchemaType::Kind::String);
    auto sign = graph->newEnumType();
    sign.addSymbol("SPEED_LIMIT");
    auto other = graph->newEnumType();
    other.addSymbol("ANIMALS");
    auto choice = graph->newCompositionType(SchemaType::Composition::AnyOf);
    choice.addAlternative(number);
    choice.addAlternative(other);
    auto possibleChoice = graph->newCompositionType(SchemaType::Composition::AnyOf);
    possibleChoice.addAlternative(choice);
    possibleChoice.addAlternative(sign);
    auto recursive = graph->newCompositionType(SchemaType::Composition::AnyOf);
    recursive.addAlternative(recursive);
    for (auto const& [name, type] : std::vector<std::pair<std::string, SchemaType>>{
            {"number", number}, {"other", other}, {"choice", choice},
            {"sign", sign}, {"text", text}, {"possibleChoice", possibleChoice},
            {"unknown", {}}, {"recursive", recursive}}) {
        auto owner = graph->newObjectType();
        owner.addField("value", type);
        root.addField(name, owner);
    }
    graph->finalize();
    auto strings = std::make_shared<StringPool>(graph->dictionary());
    Environment env(strings);
    env.setSchema(root);

    for (auto query : {"**.value == \"SPEED_LIMIT\"", "\"SPEED_LIMIT\" == **.value",
                       "(**.value == \"SPEED_LIMIT\") or false",
                       "any(**.value == \"SPEED_LIMIT\")"}) {
        CAPTURE(query);
        auto ast = compile(env, query, {.any = false, .rewriteMode = RewriteMode::Schema, .rootSchema = root});
        REQUIRE(ast);
        auto refs = referencedSchemaPaths(env, **ast, root);
        REQUIRE(refs);
        REQUIRE(refs->paths.size() == 8);
        for (auto const& reference : refs->paths) {
            auto owner = graph->dictionary().resolve(reference.path.front().field).value();
            CAPTURE(owner);
            CHECK(reference.predicateCanMatch ==
                  (owner != "number" && owner != "other" && owner != "choice"));
            CHECK(reference.viaWildcard);
            CHECK(reference.equalsStringLiteral == "SPEED_LIMIT");
        }
    }

    // False comparisons are relevant under negation and when used as values.
    // Preserve their owner evidence instead of interpreting every equality as
    // an affirmative filter. Ordinary field access also keeps every owner.
    for (auto query : {"not (**.value == \"SPEED_LIMIT\")",
                       "(**.value == \"SPEED_LIMIT\") == false", "**.value"}) {
        CAPTURE(query);
        auto ast = compile(env, query, {.any = false, .rewriteMode = RewriteMode::Schema, .rootSchema = root});
        REQUIRE(ast);
        auto refs = referencedSchemaPaths(env, **ast, root);
        REQUIRE(refs);
        CHECK(std::ranges::all_of(refs->paths, [](auto const& ref) {return ref.predicateCanMatch;}));
    }
}

TEST_CASE("Missing members in closed alternatives do not imply unknown string values", "[model.schema]")
{
    auto graph = std::make_shared<SchemaPool>();
    auto root = graph->newObjectType();
    auto number = graph->newScalarType(SchemaType::Kind::Int);
    auto reference = graph->newObjectType();
    reference.addField("value", number);
    auto closed = graph->newObjectType();
    closed.addField("unrelated", number);
    auto open = graph->newObjectType();
    open.setOpen(true);
    for (auto const& [name, alternative] : std::vector<std::pair<std::string, SchemaType>>{
            {"closed", closed}, {"open", open}, {"unknown", {}}}) {
        auto array = graph->newArrayType();
        array.addElement(reference);
        array.addElement(alternative);
        root.addField(name, array);
    }
    graph->finalize();
    auto strings = std::make_shared<StringPool>(graph->dictionary());
    Environment env(strings);
    auto ast = compile(env, "**.value == \"SPEED_LIMIT\"", {.any = false});
    REQUIRE(ast);
    auto refs = referencedSchemaPaths(env, **ast, root);
    REQUIRE(refs);
    REQUIRE(refs->paths.size() == 3);
    for (auto const& referencePath : refs->paths) {
        auto owner = graph->dictionary().resolve(referencePath.path.front().field).value();
        CAPTURE(owner);
        CHECK(referencePath.predicateCanMatch == (owner != "closed"));
    }
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
    auto graph = std::make_shared<AliasPool>(std::make_shared<StringPool>(*strings));
    (void)graph->strings()->emplace("speed");
    (void)graph->strings()->emplace("limit");
    auto rootSchema = graph->newObjectType();
    auto enumSchema = graph->newEnumType();
    for (auto name : {"$name", "value", "primary", "secondary"})
        rootSchema.addField(name);
    rootSchema.addField("unit", enumSchema);
    enumSchema.addSymbol("MPH");
    graph->finalize();

    auto root = model->root(0);
    REQUIRE(root);
    auto rootObj = model->resolve<Object>(**root);
    REQUIRE(rootObj);
    REQUIRE(rootObj->setSchema(TypeId{1}));

    Environment env(strings);
    env.setSchema(rootSchema);

    env.constants.insert_or_assign("boundName", Value::make(std::string("speed")));

    auto boundStandaloneAst = compile(env, "boundName", CompileOptions{
        .any = false,
        .rewriteMode = RewriteMode::Schema,
        .rootSchema = rootSchema});
    REQUIRE(boundStandaloneAst);
    auto boundStandaloneResult = eval(env, **boundStandaloneAst, **root, nullptr);
    REQUIRE(boundStandaloneResult);
    REQUIRE(boundStandaloneResult->size() == 1);
    REQUIRE(boundStandaloneResult->front().isa(ValueType::String));
    REQUIRE(boundStandaloneResult->front().as<ValueType::String>() == "speed");

    auto boundOperandAst = compile(env, "$name == boundName", CompileOptions{
        .any = false,
        .rewriteMode = RewriteMode::Schema,
        .rootSchema = rootSchema});
    REQUIRE(boundOperandAst);
    auto boundOperandResult = eval(env, **boundOperandAst, **root, nullptr);
    REQUIRE(boundOperandResult);
    REQUIRE(boundOperandResult->size() == 1);
    REQUIRE(boundOperandResult->front().isa(ValueType::Bool));
    REQUIRE(boundOperandResult->front().as<ValueType::Bool>());

    auto standaloneAst = compile(env, "speed", CompileOptions{
        .any = false,
        .rewriteMode = RewriteMode::Schema,
        .rootSchema = rootSchema});
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
        .rootSchema = rootSchema});
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
        .rootSchema = rootSchema});
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
        .rootSchema = rootSchema});
    REQUIRE(quotedAst);
    INFO((*quotedAst)->expr().toString());
    REQUIRE((*quotedAst)->expr().toString().find("\"speed\"") != std::string::npos);
    REQUIRE((*quotedAst)->expr().toString().find("value") != std::string::npos);

    auto anyAlternativeAst = compile(env, "limit > 80", CompileOptions{
        .any = true,
        .rewriteMode = RewriteMode::Schema,
        .rootSchema = rootSchema});
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
        .rootSchema = rootSchema});
    REQUIRE(eachAlternativeAst);
    auto eachAlternativeResult = eval(env, **eachAlternativeAst, **root, nullptr);
    REQUIRE(eachAlternativeResult);
    REQUIRE(eachAlternativeResult->size() == 1);
    REQUIRE(eachAlternativeResult->front().isa(ValueType::Bool));
    REQUIRE_FALSE(eachAlternativeResult->front().as<ValueType::Bool>());

    auto eachAllAlternativeAst = compile(env, "each(limit < 100)", CompileOptions{
        .any = false,
        .rewriteMode = RewriteMode::Schema,
        .rootSchema = rootSchema});
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
    auto strings = model->strings();
    // Make evaluation reach the field expression even though no runtime node has it.
    (void)strings->emplace("noField");
    auto graph = std::make_shared<SchemaPool>();
    auto rootSchema = graph->newObjectType();
    rootSchema.addField("field", graph->newScalarType(SchemaType::Kind::Int));
    graph->finalize();

    // Assign schemas to the model
    auto root = model->root(0);
    REQUIRE(root);

    auto rootObj = model->resolve<Object>(*root.value());
    REQUIRE(rootObj);
    REQUIRE(rootObj->setSchema(rootSchema.id()));
    REQUIRE(rootObj->schema() == rootSchema.id());

    // Run a query and check if pruning of unknown fields works
    Environment env(strings);
    env.setSchema(rootSchema);

    auto ast = compile(env, "**.noField", false, false);
    REQUIRE(ast);

    Diagnostics diagWithPruning;
    env.setSchema(rootSchema);
    auto resultWithPruning = eval(env, *ast.value(), *model->root(0).value(), &diagWithPruning);
    REQUIRE(resultWithPruning);

    Diagnostics diagNoPruning;
    env.setSchema({});
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
    auto strings = model->strings();
    (void)strings->emplace("noField");
    auto graph = std::make_shared<SchemaPool>();
    auto objectSchema = graph->newObjectType();
    auto rootSchema = graph->newArrayType();
    objectSchema.addField("field", graph->newScalarType(SchemaType::Kind::Int));
    rootSchema.addElement(objectSchema);
    const auto arraySchemaId = rootSchema.id();
    graph->finalize();

    auto root = model->root(0);
    REQUIRE(root);
    auto rootArray = model->resolve<Array>(*root.value());

    REQUIRE(rootArray);
    REQUIRE(rootArray->setSchema(arraySchemaId));
    REQUIRE(rootArray->schema() == arraySchemaId);

    Environment env(strings);
    env.setSchema(rootSchema);

    auto ast = compile(env, "**.noField", false, false);
    REQUIRE(ast);

    auto modelRoot = model->root(0);
    REQUIRE(modelRoot);

    Diagnostics diagWithPruning;
    env.setSchema(rootSchema);
    auto resultWithPruning = eval(env, **ast, **modelRoot, &diagWithPruning);
    REQUIRE(resultWithPruning);

    Diagnostics diagNoPruning;
    env.setSchema({});
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
    auto strings = model->strings();
    auto graph = std::make_shared<SchemaPool>();
    auto rootSchema = graph->newObjectType();
    rootSchema.addField("object");
    const auto rootSchemaId = rootSchema.id();
    graph->finalize();

    auto root = model->root(0);
    REQUIRE(root);
    auto rootObj = model->resolve<Object>(*root.value());
    REQUIRE(rootObj);
    REQUIRE(rootObj->setSchema(rootSchemaId));

    Environment env(strings);
    env.setSchema(rootSchema);

    auto ast = compile(env, "*.field", false, false);
    REQUIRE(ast);

    auto result = eval(env, **ast, **root, nullptr);
    REQUIRE(result);
    REQUIRE(result->size() == 1);
    REQUIRE((*result)[0].toString() == "123");
}

TEST_CASE("WildcardFieldExpr schema plan cache follows schema replacements", "[model.schema]")
{
    auto jsonModel = R"json(
    {
      "target": 123
    }
    )json";
    auto model = json::parse(jsonModel).value();
    auto strings = model->strings();
    (void)strings->emplace("other");
    auto graph = std::make_shared<SchemaPool>();
    auto rootSchema = graph->newObjectType();
    rootSchema.addField("other", graph->newScalarType(SchemaType::Kind::Int));
    const auto rootSchemaId = rootSchema.id();
    graph->finalize();

    auto root = model->root(0);
    REQUIRE(root);
    auto rootObj = model->resolve<Object>(*root.value());
    REQUIRE(rootObj);
    REQUIRE(rootObj->setSchema(rootSchemaId));

    Environment env(strings);
    env.setSchema(rootSchema);

    auto ast = compile(env, "**.target", false, false);
    REQUIRE(ast);
    auto sharedAst = SharedAST(std::move(*ast));
    BoundExpression expression(sharedAst, env);

    auto beforeSchemaUpdate = expression.eval(**root);
    REQUIRE(beforeSchemaUpdate);
    REQUIRE(beforeSchemaUpdate->size() == 1);
    REQUIRE((*beforeSchemaUpdate)[0].isa(ValueType::Null));

    REQUIRE_THROWS(rootSchema.addField("target"));
    auto replacement = std::make_shared<SchemaPool>();
    auto replacementRoot = replacement->newObjectType();
    auto scalar = replacement->newScalarType(SchemaType::Kind::Int);
    replacementRoot.addField("other", scalar);
    replacementRoot.addField("target", scalar);
    REQUIRE(replacementRoot.id() == rootSchema.id());
    replacement->finalize();
    env.setSchema(replacementRoot);

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

    const auto results = RunConcurrentWorkers<8>(
        [&shared](std::size_t worker)
        { return evaluateSharedSchemaExpression(shared, worker); });
    for (auto result : results)
        CHECK(result);
}

TEST_CASE("Schema query performance", "[perf.schema]") {
    if (RUNNING_ON_VALGRIND) { // NOLINT
        SKIP("Skipping benchmarks when running under valgrind");
    }

    constexpr auto n = std::size_t{10'000};
    static_assert(n % 2 == 0, "n must be even");

    const auto payloadASchemaId = TypeId{1};
    const auto payloadBSchemaId = TypeId{2};
    const auto xASchemaId = TypeId{3};
    const auto xBSchemaId = TypeId{4};
    const auto yASchemaId = TypeId{5};
    const auto yBSchemaId = TypeId{6};
    const auto rootObjASchemaId = TypeId{7};
    const auto rootObjBSchemaId = TypeId{8};
    const auto arraySchemaId = TypeId{9};

    auto strings = std::make_shared<StringPool>();
    auto model = std::make_shared<ModelPool>(strings);

    const auto aId = strings->emplace("a").value();
    const auto bId = strings->emplace("b").value();
    const auto yId = strings->emplace("y").value();
    const auto xId = strings->emplace("x").value();
    const auto missingId = strings->emplace("missing").value();
    const auto payloadId = strings->emplace("payload").value();

    auto graph = std::make_shared<SchemaPool>(std::make_shared<StringPool>(*strings));
    auto payloadASchema = graph->newObjectType();
    auto payloadBSchema = graph->newObjectType();
    auto xASchema = graph->newObjectType();
    auto xBSchema = graph->newObjectType();
    auto yASchema = graph->newObjectType();
    auto yBSchema = graph->newObjectType();
    auto rootObjASchema = graph->newObjectType();
    auto rootObjBSchema = graph->newObjectType();
    auto rootSchema = graph->newArrayType();
    auto scalarSchema = graph->newScalarType(SchemaType::Kind::Value);
    REQUIRE(payloadASchema.id() == payloadASchemaId);
    REQUIRE(payloadBSchema.id() == payloadBSchemaId);
    REQUIRE(xASchema.id() == xASchemaId);
    REQUIRE(xBSchema.id() == xBSchemaId);
    REQUIRE(yASchema.id() == yASchemaId);
    REQUIRE(yBSchema.id() == yBSchemaId);
    REQUIRE(rootObjASchema.id() == rootObjASchemaId);
    REQUIRE(rootObjBSchema.id() == rootObjBSchemaId);
    REQUIRE(rootSchema.id() == arraySchemaId);
    payloadASchema.addField("x", xASchema);
    payloadBSchema.addField("x", xBSchema);
    xASchema.addField("y", yASchema);
    xBSchema.addField("y", yBSchema);
    yASchema.addField("a", scalarSchema);
    yBSchema.addField("b", scalarSchema);
    rootObjASchema.addField("payload", payloadASchema);
    rootObjBSchema.addField("payload", payloadBSchema);
    rootSchema.addElement(rootObjASchema);
    rootSchema.addElement(rootObjBSchema);
    graph->finalize();

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
    env.setSchema(rootSchema);

    auto modelRoot = model->root(0);
    REQUIRE(modelRoot);

    auto aAst = compile(env, "count(**.a == 1)", false, false);
    REQUIRE(aAst);

    auto missingAst = compile(env, "count(**.missing == 1)", false, false);
    REQUIRE(missingAst);

    env.setSchema({});
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

    env.setSchema(rootSchema);
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

    // Match worker processes: some C++ libraries skip shared_ptr atomics until
    // the process has started a thread. Thread setup is outside timed evaluation.
    std::thread([] {}).join();

    constexpr auto objectCount = std::size_t{2'000};
    constexpr auto branchCount = std::size_t{32};

    const auto targetBranchSchemaId = TypeId{1};
    const auto targetPayloadSchemaId = TypeId{2};
    const auto noiseBranchSchemaId = TypeId{3};
    const auto rootObjectSchemaId = TypeId{4};
    const auto arraySchemaId = TypeId{5};

    auto strings = std::make_shared<StringPool>();
    auto model = std::make_shared<ModelPool>(strings);

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

    auto graph = std::make_shared<SchemaPool>(std::make_shared<StringPool>(*strings));
    auto targetBranchSchema = graph->newObjectType();
    auto targetPayloadSchema = graph->newObjectType();
    auto noiseBranchSchema = graph->newObjectType();
    auto rootObjectSchema = graph->newObjectType();
    auto rootSchema = graph->newArrayType();
    auto scalarSchema = graph->newScalarType(SchemaType::Kind::Value);
    REQUIRE(targetBranchSchema.id() == targetBranchSchemaId);
    REQUIRE(targetPayloadSchema.id() == targetPayloadSchemaId);
    REQUIRE(noiseBranchSchema.id() == noiseBranchSchemaId);
    REQUIRE(rootObjectSchema.id() == rootObjectSchemaId);
    REQUIRE(rootSchema.id() == arraySchemaId);
    targetBranchSchema.addField("payload", targetPayloadSchema);
    targetPayloadSchema.addField("target", scalarSchema);
    noiseBranchSchema.addField("noise", scalarSchema);
    for (size_t i = 0; i < branchNames.size(); ++i)
        rootObjectSchema.addField(branchNames[i], i == 0 ? targetBranchSchema : noiseBranchSchema);
    rootSchema.addElement(rootObjectSchema);
    graph->finalize("sparse-wide-benchmark");

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
    env.setSchema(rootSchema);

    auto modelRoot = model->root(0);
    REQUIRE(modelRoot);

    auto targetAst = compile(env, "count(**.target == 1)", false, false);
    REQUIRE(targetAst);

    auto exactPathAst = compile(env, "count(*." + branchNames.front() + ".payload.target == 1)", false, false);
    REQUIRE(exactPathAst);

    // Match hot query users: the immutable AST and environment binding survive each evaluation.
    SharedAST targetExpression(std::move(*targetAst));
    SharedAST exactExpression(std::move(*exactPathAst));
    BoundExpression targetQuery(targetExpression, env);
    BoundExpression exactQuery(exactExpression, env);

    env.setSchema({});
    BENCHMARK("Query sparse wide field 'target' recursive without schema") {
        auto res = targetQuery.eval(**modelRoot);
        REQUIRE(res);
        REQUIRE(res->size() == 1);

        auto count = res->front().template as<ValueType::Int>();
        REQUIRE(count == int64_t(objectCount));
        return count;
    };

    env.setSchema(rootSchema);
    env.enableWildcardFieldPlans = false;
    BENCHMARK("Query sparse wide field 'target' recursive with basic schema pruning") {
        auto res = targetQuery.eval(**modelRoot);
        REQUIRE(res);
        REQUIRE(res->size() == 1);

        auto count = res->front().template as<ValueType::Int>();
        REQUIRE(count == int64_t(objectCount));
        return count;
    };

    env.enableWildcardFieldPlans = true;
    BENCHMARK("Query sparse wide field 'target' recursive with schema field plans") {
        auto res = targetQuery.eval(**modelRoot);
        REQUIRE(res);
        REQUIRE(res->size() == 1);

        auto count = res->front().template as<ValueType::Int>();
        REQUIRE(count == int64_t(objectCount));
        return count;
    };

    env.setSchema({});
    env.enableWildcardFieldPlans = false;
    BENCHMARK("Query sparse wide field 'target' via exact path without schema") {
        auto res = exactQuery.eval(**modelRoot);
        REQUIRE(res);
        REQUIRE(res->size() == 1);

        auto count = res->front().template as<ValueType::Int>();
        REQUIRE(count == int64_t(objectCount));
        return count;
    };

    // Concrete runtime provenance is measured separately from the Environment fallback.
    env.setSchema(rootSchema);
    model->setSchemaPool(graph);
    BENCHMARK("Query sparse wide field 'target' recursive with bound basic pruning") {
        auto res = targetQuery.eval(**modelRoot);
        REQUIRE(res);
        REQUIRE(res->size() == 1);
        auto count = res->front().template as<ValueType::Int>();
        REQUIRE(count == int64_t(objectCount));
        return count;
    };

    env.enableWildcardFieldPlans = true;
    BENCHMARK("Query sparse wide field 'target' recursive with bound field plans") {
        auto res = targetQuery.eval(**modelRoot);
        REQUIRE(res);
        REQUIRE(res->size() == 1);
        auto count = res->front().template as<ValueType::Int>();
        REQUIRE(count == int64_t(objectCount));
        return count;
    };

    BENCHMARK("Query sparse wide field 'target' via exact path with bound provenance") {
        auto res = exactQuery.eval(**modelRoot);
        REQUIRE(res);
        REQUIRE(res->size() == 1);
        auto count = res->front().template as<ValueType::Int>();
        REQUIRE(count == int64_t(objectCount));
        return count;
    };

    // Datasources normally use a different field namespace than the immutable schema.
    auto runtimeStrings = std::make_shared<StringPool>();
    REQUIRE(runtimeStrings->emplace("runtime-padding"));
    REQUIRE(model->setStrings(runtimeStrings));
    REQUIRE(runtimeStrings->get("target") != targetId);
    Environment translatedEnv(runtimeStrings);
    BoundExpression translatedQuery(targetExpression, translatedEnv);
    BoundExpression translatedExact(exactExpression, translatedEnv);
    translatedEnv.enableWildcardFieldPlans = false;
    BENCHMARK("Query sparse wide field 'target' with separate dictionaries and basic pruning") {
        auto res = translatedQuery.eval(**modelRoot);
        REQUIRE(res);
        REQUIRE(res->size() == 1);
        auto count = res->front().template as<ValueType::Int>();
        REQUIRE(count == int64_t(objectCount));
        return count;
    };

    translatedEnv.enableWildcardFieldPlans = true;
    BENCHMARK("Query sparse wide field 'target' with separate dictionaries and field plans") {
        auto res = translatedQuery.eval(**modelRoot);
        REQUIRE(res);
        REQUIRE(res->size() == 1);
        auto count = res->front().template as<ValueType::Int>();
        REQUIRE(count == int64_t(objectCount));
        return count;
    };

    BENCHMARK("Query sparse wide field 'target' with separate dictionaries via exact path") {
        auto res = translatedExact.eval(**modelRoot);
        REQUIRE(res);
        REQUIRE(res->size() == 1);
        auto count = res->front().template as<ValueType::Int>();
        REQUIRE(count == int64_t(objectCount));
        return count;
    };

    // Separate tiles share one graph, not one model. Restore identical native
    // layouts outside timing so this also exercises imported ownership groups.
    std::ostringstream serialized;
    REQUIRE(model->write(serialized));
    auto data = serialized.str();
    std::vector<uint8_t> bytes(data.begin(), data.end());
    std::array<std::shared_ptr<ModelPool>, 4> workerModels;
    for (auto& workerModel : workerModels) {
        workerModel = std::make_shared<ModelPool>(runtimeStrings);
        workerModel->setSchemaPool(graph);
        REQUIRE(workerModel->read(bytes));
        workerModel->freeze();
    }
    for (auto mode : {"basic", "plans", "exact"}) {
        BENCHMARK(fmt::format("Sparse wide, 4 workers, {}, 16 queries each", mode)) {
            // Batch timing includes worker/binding startup. Keep the binding for
            // all 16 queries; pinned steady-state A/B measurements are separate.
            auto passed = RunConcurrentWorkers<4>([&](size_t worker) {
                Environment workerEnv(workerModels[worker]->strings());
                workerEnv.enableWildcardFieldPlans = std::string_view(mode) == "plans";
                BoundExpression query(std::string_view(mode) == "exact" ? exactExpression : targetExpression, workerEnv);
                auto workerRoot = workerModels[worker]->root(0).value();
                for (size_t i = 0; i < 16; ++i) {
                    auto result = query.eval(*workerRoot);
                    if (!result || result->size() != 1 || result->front().as<ValueType::Int>() != int64_t(objectCount))
                        return false;
                }
                return true;
            });
            REQUIRE(std::ranges::all_of(passed, [](bool success) { return success; }));
            return passed;
        };
    }

    // All schemas now use canonical storage. Exercise independently decoded
    // ownership and both dictionary layouts, rather than a retired registry backend.
    std::ostringstream graphStream;
    REQUIRE(graph->write(graphStream));
    auto graphBytes = graphStream.str();
    auto compact = SchemaPool::read(
        std::span(reinterpret_cast<uint8_t const*>(graphBytes.data()), graphBytes.size()),
        "decoded-sparse-wide-benchmark");
    model->remapSchemas(compact, [&](SchemaType const& reference) {
        return compact->schemaType(reference.id());
    });
    for (auto sharedDictionary : {false, true}) {
        if (sharedDictionary)
            REQUIRE(model->setStrings(compact->strings()));
        Environment compactEnv(model->strings());
        for (auto mode : {"basic", "plans", "exact"}) {
            compactEnv.enableWildcardFieldPlans = std::string_view(mode) == "plans";
            BoundExpression query(std::string_view(mode) == "exact" ? exactExpression : targetExpression, compactEnv);
            BENCHMARK(fmt::format("Sparse wide, SchemaPool, {} dictionary, {}",
                                 sharedDictionary ? "shared" : "separate", mode)) {
                auto result = query.eval(**modelRoot);
                REQUIRE(result);
                REQUIRE(result->size() == 1);
                auto count = result->front().as<ValueType::Int>();
                REQUIRE(count == int64_t(objectCount));
                return count;
            };
        }
    }
}
