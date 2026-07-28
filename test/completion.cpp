#include "src/completion.h"
#include <algorithm>
#include <map>
#include <string_view>
#include <optional>

#include "common.hpp"
#include "simfil/environment.h"
#include "simfil/model/schema.h"
#include "src/expected.h"

const auto model = R"json(
{
  "f": 1,
  "fi": 2,
  "fie": 3,
  "fiel": 4,
  "field": "CONSTANT_1",
  "other": "CONSTANT_2",
  "constant": 5,
  "__long__name__": "long text value",
  "this needs escaping": 6,
  "sub": {
    "child": 7
  },
  "with a space": 1
}
)json";

using Type = simfil::CompletionCandidate::Type;

auto GetCompletion(std::string_view query, std::optional<size_t> point, const CompletionOptions* options = nullptr)
{
    return CompleteQuery(query, point.value_or(query.size()), model, options);
}

auto EXPECT_COMPLETION(std::string_view query, std::optional<size_t> point, std::string_view what, std::optional<Type> type = {}, size_t count = 0)
{
    INFO("Query: " << query);
    INFO("Expected completion: " << what);

    auto found = false;
    auto comp = GetCompletion(query, point);
    for (const auto& item : comp) {
        INFO("  Item: " << item.text);
        if (item.text == what && (!type || item.type == *type)) {
            found = true;
        }
    }

    REQUIRE(found);
    if (count > 0) {
        REQUIRE(comp.size() == count);
    }
}

auto CompleteSchemaQuery(std::string_view query, std::optional<size_t> point = {})
{
    auto parsed = simfil::json::parse(R"json({"present": 1})json");
    REQUIRE(parsed);

    auto model = std::move(*parsed);
    auto strings = model->strings();
    auto schemaOnlyField = strings->emplace("schemaOnlyField").value();
    auto specialField = strings->emplace("schema field").value();
    auto enumCarrier = strings->emplace("Carrier").value();
    auto enumFast = strings->emplace("FAST_MODE").value();
    auto nested = strings->emplace("nested").value();

    auto valueSchema = std::make_unique<ValueSchema>();
    valueSchema->addEnumSymbol(enumCarrier);
    valueSchema->addEnumSymbol(enumFast);

    auto objectSchema = std::make_unique<ObjectSchema>();
    objectSchema->addField(schemaOnlyField, {SchemaId{2}});
    objectSchema->addField(specialField);
    objectSchema->addField(nested, {SchemaId{3}});

    auto nestedSchema = std::make_unique<ObjectSchema>();
    nestedSchema->addField(enumFast);

    auto registry = std::make_shared<std::map<SchemaId, std::unique_ptr<Schema>>>();
    (*registry)[SchemaId{1}] = std::move(objectSchema);
    (*registry)[SchemaId{2}] = std::move(valueSchema);
    (*registry)[SchemaId{3}] = std::move(nestedSchema);

    auto lookup = [registry](SchemaId schemaId) -> Schema* {
        if (auto it = registry->find(schemaId); it != registry->end())
            return it->second.get();
        return nullptr;
    };
    for (auto const& [_, schema] : *registry)
        schema->finalize(lookup);

    auto root = model->root(0);
    REQUIRE(root);
    auto rootObj = model->resolve<Object>(**root);
    REQUIRE(rootObj);
    REQUIRE(rootObj->setSchema(SchemaId{1}));

    Environment env(strings);
    env.querySchemaCallback = [registry](SchemaId schemaId) -> const Schema* {
        if (auto it = registry->find(schemaId); it != registry->end())
            return it->second.get();
        return nullptr;
    };

    CompletionOptions opts;
    opts.showWildcardHints = false;
    return complete(env, query, point.value_or(query.size()), **root, opts).value();
}

auto EXPECT_SCHEMA_COMPLETION(std::string_view query, std::string_view what, Type type)
{
    auto found = false;
    for (const auto& item : CompleteSchemaQuery(query)) {
        INFO("  Item: " << item.text);
        if (item.text == what && item.type == type)
            found = true;
    }
    REQUIRE(found);
}

TEST_CASE("CompleteField", "[completion.field.incompleteQuery]") {
    EXPECT_COMPLETION("((oth", {}, "other");
}

TEST_CASE("CompleteField", "[completion.field]") {
    EXPECT_COMPLETION("oth", {}, "other");
    EXPECT_COMPLETION("__", {}, "__long__name__");
}

TEST_CASE("CompleteField", "[completion.field.escape]") {
    EXPECT_COMPLETION("this", {}, "[\"this needs escaping\"]");
}

TEST_CASE("CompleteField", "[completion.field.midQuery]") {
    EXPECT_COMPLETION("oth > 123", 3, "other");
    //                    ^- cursor
    EXPECT_COMPLETION("oth>123", 3, "other");
    //                    ^- cursor
}

TEST_CASE("CompleteField", "[completion.sub-field]") {
    EXPECT_COMPLETION("sub.", {}, "child");
}

TEST_CASE("CompleteString", "[completion.string-const]") {
    EXPECT_COMPLETION("1 > C", {}, "\"CONSTANT_1\"");
}

TEST_CASE("Complete Function", "[completion.function]") {
    EXPECT_COMPLETION("cou", {}, "count");
    EXPECT_COMPLETION("ma", {}, "max");
    EXPECT_COMPLETION("mi", {}, "min");
    EXPECT_COMPLETION("su", {}, "sum");
}

TEST_CASE("Completion Limit", "[completion.option-limit]") {
    CompletionOptions opts;
    opts.limit = 3;

    auto comp = GetCompletion("", {}, &opts);
    REQUIRE(comp.size() <= opts.limit);
}

TEST_CASE("Complete in Expression", "[completion.complete-mid-expression]") {
    EXPECT_COMPLETION("(field + oth)", 12, "other");
    EXPECT_COMPLETION("count(fie", 9, "field");
    EXPECT_COMPLETION("sub.ch > 123", 6, "child");
}

TEST_CASE("Complete in unclosed expression", "[completion.complete-in-unclosed-expr]") {
    EXPECT_COMPLETION("(field + oth", {}, "other");
    EXPECT_COMPLETION("_{field + oth", {}, "other");
    EXPECT_COMPLETION("*[field + oth", {}, "other");
}

TEST_CASE("Complete SmartCas", "[completion.smart-case]") {
    // Complete both the field and string literals from the model.
    EXPECT_COMPLETION("cons", {}, "constant", Type::FIELD, 4);
    EXPECT_COMPLETION("cons", {}, "\"CONSTANT_1\"", Type::CONSTANT);

    // Do not complete the field
    EXPECT_COMPLETION("CONS", {}, "\"CONSTANT_1\"", Type::CONSTANT, 3);
    EXPECT_COMPLETION("CONS", {}, "\"CONSTANT_2\"", Type::CONSTANT);
}

TEST_CASE("Complete Field with Special Characters", "[copletion.escape-field]") {
    EXPECT_COMPLETION("with", {}, "[\"with a space\"]", Type::FIELD);
}

TEST_CASE("Complete And/Or", "[copletion.and-or]") {
    EXPECT_COMPLETION("true and f", {}, "field");
    EXPECT_COMPLETION("f and true", 1, "field");
    EXPECT_COMPLETION("false and f", {}, "field");
    EXPECT_COMPLETION("f and false", 1, "field");
    EXPECT_COMPLETION("true or f", {}, "field");
    EXPECT_COMPLETION("f or true", 1, "field");
    EXPECT_COMPLETION("false or f", {}, "field");
    EXPECT_COMPLETION("f or false", 1, "field");
}

TEST_CASE("Complete Wildcard Hint", "[completion.generate-eq-value-hint]") {
    EXPECT_COMPLETION("A_CONST", {}, "**.A_CONST", Type::HINT);
    EXPECT_COMPLETION("\"A_CONST\"", {}, "** = \"A_CONST\"", Type::HINT);
    EXPECT_COMPLETION("field", {}, "**.field", Type::HINT);
}

TEST_CASE("Complete Wildcard Comparison", "[completion.generate-compare-recursive-hint]") {
    EXPECT_COMPLETION("field == 123", {}, "**.field == 123", Type::HINT);
    EXPECT_COMPLETION("name != \"Mulldrifter\"", {}, "**.name != \"Mulldrifter\"", Type::HINT);
    EXPECT_COMPLETION("count < 10", {}, "**.count < 10", Type::HINT);
    EXPECT_COMPLETION("value >= 5.0", {}, "**.value >= 5.0", Type::HINT);
    EXPECT_COMPLETION("A_FIELD == \"Value\"", {}, "**.A_FIELD == \"Value\"", Type::HINT);
    EXPECT_COMPLETION("A_CONST != 42", {}, "**.A_CONST != 42", Type::HINT);
}

TEST_CASE("Sort Completion", "[completion.sorted]") {
    CompletionOptions opts;
    opts.showWildcardHints = false;

    auto comp = GetCompletion("f", {}, &opts);
    REQUIRE(std::is_sorted(comp.begin(), comp.end(), [](const auto& l, const auto& r) {
        return l.text < r.text;
    }));
}

TEST_CASE("Complete schema fields", "[completion.schema-field]") {
    EXPECT_SCHEMA_COMPLETION("schema", "schemaOnlyField", Type::FIELD);
    EXPECT_SCHEMA_COMPLETION("schema", "[\"schema field\"]", Type::FIELD);
}

TEST_CASE("Complete schema enum symbols", "[completion.schema-enum]") {
    EXPECT_SCHEMA_COMPLETION("Car", "\"Carrier\"", Type::CONSTANT);
    EXPECT_SCHEMA_COMPLETION("FAST", "FAST_MODE", Type::FIELD);

    for (const auto& item : CompleteSchemaQuery("FAST")) {
        INFO("  Item: " << item.text);
        REQUIRE(item.text != "\"FAST_MODE\"");
    }
}
