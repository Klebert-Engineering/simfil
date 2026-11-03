#include "common.hpp"
#include "simfil/model/schema-model.h"
#include "simfil/model/schema-pool.h"
#include "simfil/function.h"
#include "simfil/overlay.h"

#include <catch2/benchmark/catch_benchmark.hpp>
#include <map>

using namespace simfil;

namespace
{

/** Own a small graph and its namespace exactly as an external registry binding would. */
struct Domains
{
    std::shared_ptr<StringPool> strings = std::make_shared<StringPool>();
    std::shared_ptr<std::map<SchemaId, std::unique_ptr<Schema>>> graph =
        std::make_shared<std::map<SchemaId, std::unique_ptr<Schema>>>();

    /** Add a named field/enum string to this fixture's private namespace. */
    auto name(std::string_view name) -> StringId { return strings->emplace(name).value(); }

    /** Construct a definition without imposing another registry implementation on the API. */
    template<class T, class... Args>
    auto add(SchemaId id, Args&&... args) -> T&
    {
        auto schema = std::make_unique<T>(std::forward<Args>(args)...);
        auto& result = *schema;
        (*graph)[id] = std::move(schema);
        return result;
    }

    /** Capture graph ownership rather than a temporary Environment reference. */
    auto lookup() const -> Schema::Lookup
    {
        return [graph = graph](SchemaId id) -> const Schema* {
            auto found = graph->find(id);
            return found == graph->end() ? nullptr : found->second.get();
        };
    }

    /** Prepare reachability once, outside completion/evaluation hot loops. */
    void finalize()
    {
        for (auto& [id, schema] : *graph)
            schema->finalize([this](SchemaId id) -> Schema* {
                auto found = graph->find(id);
                return found == graph->end() ? nullptr : found->second.get();
            });
    }

    /** Return owning completion strings without ever creating sample ModelNodes. */
    auto complete(std::string_view query, SchemaId root = 1, CompletionOptions options = {},
                  std::optional<std::size_t> cursor = {}) -> std::vector<CompletionCandidate>
    {
        Environment env(strings);
        env.querySchemaCallback = lookup();
        options.showWildcardHints = false;
        auto result = simfil::complete(env, query, cursor.value_or(query.size()), root, options);
        INFO(query);
        REQUIRE(result);
        return *result;
    }
};

/** Check one candidate without depending on incidental function-name suggestions. */
auto contains(const std::vector<CompletionCandidate>& candidates, std::string_view text) -> bool
{
    return std::ranges::any_of(candidates, [&](const auto& candidate) { return candidate.text == text; });
}

/** Emulate a registry binding that does not inherit the builtin ObjectSchema implementation. */
class ForeignSchema : public Schema
{
public:
    ObjectSchema definition;
    bool indexed = true;
    auto kind() const -> Kind override { return definition.kind(); }
    auto canHaveField(StringId name) const -> bool override { return !indexed || definition.canHaveField(name); }
    auto reachabilityComplete() const -> bool override { return indexed && definition.reachabilityComplete(); }
    auto nestedFields() const & -> std::span<const StringId> override
    {
        return indexed ? definition.nestedFields() : std::span<const StringId>{};
    }
    auto directFields() const & -> std::span<const StringId> override { return definition.directFields(); }
    auto fieldSchema(StringId field) const -> SchemaId override { return definition.fieldSchema(field); }
    auto finalize(const std::function<Schema*(SchemaId)>& lookup) -> State override { return definition.finalize(lookup); }
    void forEachDirectField(const std::function<void(StringId, std::span<const SchemaId>)>& fn) const override
    {
        definition.forEachDirectField(fn);
    }
};

/** Verify that completion does not invoke even registered constant-looking functions. */
class InvocationProbe : public Function
{
public:
    mutable unsigned calls = 0;
    auto ident() const -> const FnInfo& override
    {
        static const FnInfo info{"probe", "Test-only side effect", "probe()"};
        return info;
    }
    auto eval(Context ctx, const Value&, const std::vector<ExprPtr>&, const ResultFn& emit) const
        -> tl::expected<Result, Error> override
    {
        ++calls;
        return emit(ctx, Value::t());
    }
};

/** Keep operand alias behavior on the root schema, as registry-backed attribute bindings do. */
class AliasSchema : public ObjectSchema
{
public:
    StringId alias = 0;
    SchemaPath path;
    auto scalarFieldPathsForSymbol(StringId name, const Lookup&) const -> std::vector<SchemaPath> override
    {
        return name == alias ? std::vector<SchemaPath>{path} : std::vector<SchemaPath>{};
    }
};

/** A lookup-only alias stays out of canonical field enumeration and yields to real members. */
class LookupAliasSchema : public ObjectSchema
{
public:
    StringId alias = 0, target = 0;
    auto canonicalField(StringId field) const -> StringId override
    {
        return field == alias && std::ranges::find(directFields(), field) == directFields().end()
            ? target : field;
    }
    auto canHaveField(StringId field) const -> bool override
    {
        return ObjectSchema::canHaveField(canonicalField(field));
    }
};

/** Runtime adapter whose declared literals intentionally are not interned in its data pool. */
class TextEnumDomain : public ValueSchema
{
public:
    /** Match the producer-owned literal without writing it to the data dictionary. */
    auto hasDirectEnumSymbol(std::string_view symbol, StringPool const&) const -> bool override
    {
        return symbol == "READY";
    }
};

}

TEST_CASE("Packed schema kinds preserve extension affinity and concrete names", "[model.schema-domain]")
{
    Domains d;
    constexpr StringId featureName = 200;
    d.strings->addStaticKey(featureName, "Feature");
    auto kind = Schema::makeKind(featureName, valueTypeAffinity(ValueType::Object));
    auto& root = d.add<ObjectSchema>(1);
    root.setKind(kind);
    root.setTypeName("RoadLink");
    root.addField(d.name("identifier"), {2}, true);
    d.add<ValueSchema>(2, Schema::Kind::Int);
    d.finalize();
    REQUIRE(Schema::kindNameId(root.kind()) == featureName);
    REQUIRE(root.hasAffinity(ValueType::Object));
    REQUIRE_FALSE(root.hasAffinity(ValueType::Array));
    REQUIRE(root.fieldRequired(d.name("identifier")) == true);
    REQUIRE_FALSE(root.fieldRequired(d.name("missing")).has_value());
    auto path = Schema::firstScalarFieldPath(1, d.lookup());
    REQUIRE(path);
    REQUIRE(path->front().field == d.name("identifier"));
    REQUIRE(contains(d.complete("iden"), "identifier"));

    auto model = std::make_shared<SchemaModel>(d.strings, d.lookup());
    auto node = model->root(1);
    REQUIRE(node->type() == ValueType::Object);
    REQUIRE(node->schema() == NoSchemaId);
    REQUIRE(model->materializedNodeCount() == 2);
    REQUIRE(std::get<std::string>(node->get(StringPool::SchemaKind)->value()) == "Feature");
    auto json = node->toJson();
    REQUIRE(json["kind"] == "Feature");
    REQUIRE(json["typename"] == "RoadLink");
    REQUIRE(json["fields"]["identifier"]["required"] == true);
    REQUIRE(json["fields"]["identifier"]["kind"] == "integer");
    REQUIRE_FALSE(json["fields"]["identifier"].contains("typename"));
}

TEST_CASE("Portable enum predicates do not require runtime enum dictionary entries", "[model.schema-domain]")
{
    Domains compileDomains;
    auto& root = compileDomains.add<ObjectSchema>(1);
    root.addField(compileDomains.name("mode"), {2});
    root.setOpen(true);
    compileDomains.add<ValueSchema>(2, Schema::Kind::String).addEnumSymbol(compileDomains.name("READY"));
    compileDomains.finalize();
    Environment compileEnv(compileDomains.strings);
    compileEnv.querySchemaCallback = compileDomains.lookup();
    auto ast = compile(compileEnv, "READY", CompileOptions{
        .any = false, .rewriteMode = RewriteMode::Schema, .rootSchema = 1});
    REQUIRE(ast);
    REQUIRE((*ast)->expr().toString().find("schema-enum") != std::string::npos);

    Domains runtimeDomains;
    runtimeDomains.add<ObjectSchema>(1).addField(runtimeDomains.name("mode"), {2});
    runtimeDomains.add<TextEnumDomain>(2);
    runtimeDomains.finalize();
    Environment runtimeEnv(runtimeDomains.strings);
    runtimeEnv.querySchemaCallback = runtimeDomains.lookup();
    auto model = std::make_shared<ModelPool>(runtimeDomains.strings);
    auto matching = model->newObject();
    REQUIRE(matching->addField("mode", std::string_view("READY")));
    model->addRoot(matching);
    auto before = runtimeDomains.strings->size();
    auto values = eval(runtimeEnv, **ast, *model->root(0).value(), nullptr);
    REQUIRE(values);
    REQUIRE(values->size() == 1);
    REQUIRE(values->front().as<ValueType::Bool>());
    REQUIRE(runtimeDomains.strings->get("READY") == StringPool::Empty);
    REQUIRE(runtimeDomains.strings->size() == before);
    auto unrelated = model->newObject();
    REQUIRE(unrelated->addField("unrelated", std::string_view("READY")));
    model->addRoot(unrelated);
    values = eval(runtimeEnv, **ast, *model->root(1).value(), nullptr);
    REQUIRE(values);
    REQUIRE_FALSE(values->front().as<ValueType::Bool>());
}

TEST_CASE("Descriptor nodes query metadata and retain graph ownership", "[model.schema-domain]")
{
    Domains d;
    d.add<ObjectSchema>(1).addField(d.name("items"), {2});
    d.add<ArraySchema>(2).addElementSchemas({3});
    d.add<ObjectSchema>(3).addField(d.name("name"), {4}, false);
    auto& value = d.add<ValueSchema>(4, Schema::Kind::String);
    value.addEnumSymbol(d.name("urban"));
    value.setNullable(true);
    d.finalize();
    auto model = std::make_shared<SchemaModel>(d.strings, d.lookup());
    auto node = model->root(1);
    std::weak_ptr owner = d.graph;
    d.graph.reset();
    Environment env(model->strings());
    auto query = compile(env, "fields.items.elements[0].fields.name.kind", false);
    REQUIRE(query);
    auto values = eval(env, **query, *node, nullptr);
    REQUIRE(values);
    REQUIRE(values->size() == 1);
    REQUIRE(values->front().as<ValueType::String>() == "string");
    auto json = node->toJson();
    REQUIRE(json["fields"]["items"]["elements"][0]["fields"]["name"]["required"] == false);
    REQUIRE(json["fields"]["items"]["elements"][0]["fields"]["name"]["nullable"] == true);
    REQUIRE_FALSE(owner.expired());
    model.reset();
    REQUIRE(node->toJson() == json);
    node = {};
    REQUIRE(owner.expired());
}

TEST_CASE("Schema descriptors bound recursion and expansion explicitly", "[model.schema-domain]")
{
    Domains d;
    d.add<ObjectSchema>(1).addField(d.name("next"), {1});
    d.finalize();
    auto model = std::make_shared<SchemaModel>(d.strings, d.lookup());
    auto json = model->root(1)->toJson();
    REQUIRE(json["$ref"] == 1);
    REQUIRE(json["fields"]["next"]["$ref"] == 1);
    REQUIRE(json["fields"]["next"]["truncated"] == "cycle");
    REQUIRE_FALSE(model->exhausted());
    auto tiny = std::make_shared<SchemaModel>(d.strings, d.lookup(), 2);
    REQUIRE(tiny->root(1)->toJson()["fields"]["truncated"] == "node-budget");
    REQUIRE(tiny->materializedNodeCount() == 2);
    REQUIRE(tiny->exhausted());
}

TEST_CASE("Implicit schema unions retain all domain references",
          "[model.schema-domain]")
{
    Domains d;
    d.add<ObjectSchema>(1).addField(d.name("choice"), {2, 3});
    d.add<ValueSchema>(2, Schema::Kind::Int);
    d.add<ValueSchema>(3, Schema::Kind::String);
    d.finalize();
    auto model = std::make_shared<SchemaModel>(d.strings, d.lookup());
    auto json = model->root(1)->toJson();
    REQUIRE(json["fields"]["choice"]["kind"] == "union");
    REQUIRE(json["fields"]["choice"]["$ref"] == nlohmann::json::array({2, 3}));
    REQUIRE(json["fields"]["choice"]["alternatives"][0]["kind"] == "integer");
    REQUIRE(json["fields"]["choice"]["alternatives"][1]["kind"] == "string");
}

TEST_CASE("Schema projections do not hide deep matches or allocation exhaustion", "[model.schema-domain]")
{
    Domains d;
    for (SchemaId id = 1; id < 80; ++id)
        d.add<ObjectSchema>(id).addField(d.name("next"), {SchemaId(id + 1)});
    d.add<ValueSchema>(80, Schema::Kind::String).setTypeName("DeepTarget");
    d.finalize();
    Environment env(d.strings);
    auto ast = compile(env, "**.typename", false);
    REQUIRE(ast);
    auto model = std::make_shared<SchemaModel>(d.strings, d.lookup());
    auto values = eval(env, **ast, *model->root(1), nullptr);
    REQUIRE(values);
    REQUIRE(values->size() == 1);
    REQUIRE(values->front().as<ValueType::String>() == "DeepTarget");
    REQUIRE_FALSE(model->exhausted());

    auto tiny = std::make_shared<SchemaModel>(d.strings, d.lookup(), 10);
    REQUIRE(eval(env, **ast, *tiny->root(1), nullptr));
    // A scalar projection never emits the terminal marker. The caller still needs the signal.
    REQUIRE(tiny->exhausted());
}

TEST_CASE("Domain completion follows array indices and logical alternatives", "[completion.domain]")
{
    Domains d;
    auto& root = d.add<ObjectSchema>(1);
    root.addField(d.name("items"), {2});
    root.addField(d.name("choice"), {5});
    d.add<ArraySchema>(2).addElementSchemas({3, 4});
    d.add<ObjectSchema>(3).addField(d.name("name"), {4});
    d.add<ValueSchema>(4, Schema::Kind::String);
    auto& choice = d.add<CombinedSchema>(5, Schema::Composition::AnyOf);
    choice.addAlternative(3);
    choice.addAlternative(4);
    d.finalize();
    REQUIRE(contains(d.complete("items[17].na"), "name"));
    REQUIRE(contains(d.complete("items[-1].na"), "name"));
    REQUIRE(contains(d.complete("items.*.na"), "name"));
    REQUIRE(contains(d.complete("items.*{name == \"x\"}.na"), "name"));
    REQUIRE(contains(d.complete("choice.na"), "name"));
    REQUIRE(contains(d.complete("false and choice.na"), "name"));
    REQUIRE(contains(d.complete("true or choice.na"), "name"));
    REQUIRE(contains(d.complete("missing_function(choice.na)"), "name") == false);
    auto query = std::string("missing_function(choice.na)");
    REQUIRE(contains(d.complete(query, 1, {}, query.size() - 1), "name"));
    REQUIRE(contains(d.complete(""), "items"));
    auto json = std::make_shared<SchemaModel>(d.strings, d.lookup())->root(1)->toJson();
    REQUIRE(json["fields"]["choice"]["kind"] == "union");
    REQUIRE(json["fields"]["choice"]["alternatives"].size() == 2);
    REQUIRE_FALSE(json["fields"]["choice"].contains("elements"));
}

TEST_CASE("Domain completion scopes enums to the compared operand", "[completion.domain]")
{
    Domains d;
    auto& root = d.add<ObjectSchema>(1);
    root.addField(d.name("mode"), {2});
    root.addField(d.name("other"), {3});
    auto& mode = d.add<ValueSchema>(2, Schema::Kind::String);
    mode.addEnumSymbol(d.name("urban"));
    mode.addEnumSymbol(d.name("rural"));
    d.add<ValueSchema>(3, Schema::Kind::String).addEnumSymbol(d.name("unrelated"));
    d.finalize();
    auto suggestions = d.complete("mode == u");
    REQUIRE(contains(suggestions, "\"urban\""));
    REQUIRE_FALSE(contains(suggestions, "\"unrelated\""));
    REQUIRE(contains(d.complete("mode == "), "\"urban\""));
    REQUIRE(contains(d.complete("mode == 'u"), "\"urban\""));
    REQUIRE(contains(d.complete("mode == r'u"), "\"urban\""));
    REQUIRE(contains(d.complete("ur"), "\"urban\""));
    CompletionOptions limited;
    limited.limit = 1;
    REQUIRE(d.complete("", 1, limited).size() <= 1);
    limited.maxSchemaVisits = 0;
    REQUIRE(d.complete("", 1, limited).empty());
}

TEST_CASE("Open and unresolved descendants do not prove fields absent", "[model.schema-domain]")
{
    Domains d;
    auto& root = d.add<ObjectSchema>(1);
    root.addField(d.name("known"), {2});
    d.add<ValueSchema>(2, Schema::Kind::Int);
    d.finalize();
    REQUIRE(root.reachabilityComplete());
    REQUIRE_FALSE(root.canHaveField(d.name("missing")));
    root.addField(d.name("unknown"));
    d.finalize();
    REQUIRE_FALSE(root.reachabilityComplete());
    REQUIRE(root.canHaveField(d.name("missing")));
    auto& open = d.add<ObjectSchema>(3);
    open.setOpen(true);
    d.finalize();
    REQUIRE(open.canHaveField(d.name("anything")));
    REQUIRE(Schema::fieldSchemas(3, d.lookup(), d.name("anything")) == std::vector<SchemaId>{NoSchemaId});
}

TEST_CASE("Recursive enum rewrites follow real paths without unrelated string matches", "[model.schema-domain]")
{
    Domains d;
    auto& root = d.add<ObjectSchema>(1);
    root.addField(d.name("status"), {2});
    root.addField(d.name("next"), {1});
    root.addField(d.name("unrelated"), {3});
    d.add<ValueSchema>(2, Schema::Kind::String).addEnumSymbol(d.name("READY"));
    d.add<ValueSchema>(3, Schema::Kind::String);
    d.finalize();
    bool complete = true;
    REQUIRE_FALSE(Schema::enumSymbolPaths(1, d.lookup(), d.name("READY"), &complete).empty());
    REQUIRE_FALSE(complete);
    Environment env(d.strings);
    env.querySchemaCallback = d.lookup();
    auto ast = compile(env, "READY", CompileOptions{false, RewriteMode::Schema, 1});
    REQUIRE(ast);
    auto model = std::make_shared<ModelPool>(d.strings);
    auto first = model->newObject();
    auto second = model->newObject();
    auto third = model->newObject();
    first->addField("next", second);
    second->addField("next", third);
    first->addField("unrelated", std::string("READY"));
    auto result = eval(env, **ast, *first, nullptr);
    REQUIRE(result);
    REQUIRE(result->front().isBool(false));
    third->addField("status", std::string("READY"));
    result = eval(env, **ast, *first, nullptr);
    REQUIRE(result);
    REQUIRE(result->front().isBool(true));
    auto references = referencedSchemaPaths(env, **ast, 1);
    REQUIRE(references);
    REQUIRE(references->hasUnresolvedAccess);
    REQUIRE(referencedQueryTerms(**ast).stringLiterals.contains("READY"));
    REQUIRE(contains(d.complete("next.next.st"), "status"));
}

TEST_CASE("Combined domains retain intersection and exclusive union semantics", "[model.schema-domain]")
{
    Domains d;
    auto& combined = d.add<CombinedSchema>(1, Schema::Composition::AllOf);
    combined.addAlternative(2);
    combined.addAlternative(3);
    d.add<ValueSchema>(2, Schema::Kind::Int);
    d.add<ValueSchema>(3, Schema::Kind::String);
    d.finalize();
    REQUIRE(Schema::affinities(combined.kind()) == 0);
    auto json = std::make_shared<SchemaModel>(d.strings, d.lookup())->root(1)->toJson();
    REQUIRE(json["kind"] == "intersection");
    REQUIRE(json["alternatives"].size() == 2);
    auto& exclusive = d.add<CombinedSchema>(4, Schema::Composition::OneOf);
    exclusive.addAlternative(2);
    exclusive.addAlternative(3);
    d.finalize();
    REQUIRE(exclusive.hasAffinity(ValueType::Int));
    REQUIRE(exclusive.hasAffinity(ValueType::String));
    REQUIRE_FALSE(exclusive.hasAffinity(ValueType::Object));
}

TEST_CASE("Unrestricted and impossible domains do not share pruning semantics", "[model.schema-domain]")
{
    Domains d;
    d.add<CombinedSchema>(1, Schema::Composition::AllOf);
    d.add<CombinedSchema>(2, Schema::Composition::AnyOf);
    d.add<Schema>(3).setKind(Schema::Kind::Never);
    d.add<CombinedSchema>(4, Schema::Composition::AllOf).addAlternative(1);
    d.finalize();
    auto name = d.name("x");
    REQUIRE(d.lookup()(1)->canHaveField(name));
    REQUIRE(d.lookup()(4)->canHaveField(name));
    REQUIRE_FALSE(d.lookup()(2)->canHaveField(name));
    REQUIRE_FALSE(d.lookup()(3)->canHaveField(name));
    REQUIRE(Schema::fieldSchemas(1, d.lookup(), name) == std::vector<SchemaId>{NoSchemaId});
    REQUIRE(Schema::itemSchemas(1, d.lookup()) == std::vector<SchemaId>{NoSchemaId});
    auto model = std::make_shared<ModelPool>(d.strings);
    auto root = model->newObject();
    root->addField("x", int64_t{42});
    root->setSchema(1);
    Environment env(d.strings);
    env.querySchemaCallback = d.lookup();
    auto ast = compile(env, "**.x", false);
    REQUIRE(ast);
    auto result = eval(env, **ast, *root, nullptr);
    REQUIRE(result);
    REQUIRE(result->front().toString() == "42");
}

TEST_CASE("Domain completion preserves source ranges escaping and literal enums", "[completion.domain]")
{
    Domains d;
    auto& root = d.add<ObjectSchema>(1);
    root.addField(d.name("weird.field"), {2});
    root.addField(d.name("mode"), {2});
    root.addField(d.name("READY"), {3});
    auto& value = d.add<ValueSchema>(2);
    value.addEnumSymbol(d.name("READY"));
    value.addEnumSymbol(d.name("a\"b\\c"));
    value.addEnumValue(int64_t{42});
    value.addEnumValue(true);
    value.addEnumValue(std::monostate{});
    d.add<ValueSchema>(3, Schema::Kind::Int);
    d.finalize();
    auto stringsBefore = d.strings->size();
    REQUIRE(contains(d.complete("we"), "[\"weird.field\"]"));
    REQUIRE(contains(d.complete("READY"), "READY"));
    REQUIRE_FALSE(contains(d.complete("READY"), "\"READY\""));
    REQUIRE(contains(d.complete("mode == R"), "\"READY\""));
    REQUIRE(contains(d.complete("mode == "), "42"));
    REQUIRE(contains(d.complete("mode == "), "true"));
    REQUIRE(contains(d.complete("mode == "), "null"));

    auto query = std::string("mode == 'a' and true");
    auto candidates = d.complete(query, 1, {}, query.find("a'") + 1);
    auto escaped = std::ranges::find(candidates, "\"a\\\"b\\\\c\"", &CompletionCandidate::text);
    REQUIRE(escaped != candidates.end());
    REQUIRE(escaped->location.offset == 8);
    REQUIRE(escaped->location.size == 3);
    Environment env(d.strings);
    REQUIRE(compile(env, query.replace(escaped->location.offset, escaped->location.size, escaped->text), false));

    candidates = d.complete("[\"we\"]", 1, {}, 4);
    auto field = std::ranges::find(candidates, "\"weird.field\"", &CompletionCandidate::text);
    REQUIRE(field != candidates.end());
    REQUIRE(field->location.offset == 1);
    REQUIRE(field->location.size == 4);
    REQUIRE(d.strings->size() == stringsBefore);
    REQUIRE_FALSE(compile(env, "mode == 'a", false));
}

TEST_CASE("SchemaPool owns one compact graph across native transport and imports", "[model.schema-pool]")
{
    auto pool = std::make_shared<SchemaPool>();
    auto object = pool->addSchema(Schema::Kind::Object);
    auto array = pool->addSchema(Schema::Kind::Array);
    auto number = pool->addSchema(Schema::Kind::Int);
    auto mask = pool->addSchema(Schema::Kind::Bitmask);
    auto context = pool->addSchema(Schema::Kind::Object);
    pool->setTypeName(object, "Measurement");
    pool->addFieldSchema(object, "samples", array);
    pool->addFieldSchema(object, "flags", mask);
    pool->addFieldSchema(object, "context", context);
    pool->setFieldRole(object, "context", Schema::FieldRole::Context);
    pool->setFieldRequired(object, "samples", true);
    pool->addFieldAlias(object, "values", "samples");
    pool->addElementSchema(array, number);
    pool->addFieldSchema(context, "parent", object);
    pool->addFieldSchema(context, "secret", number);
    pool->setOpen(context, true);
    std::vector<Schema::NumericRange> intervals{{int64_t{0}, int64_t{254}}};
    pool->setNumericDomain(number, {.encoding="uint8", .unit="km/h", .minimum=int64_t{0},
        .maximum=int64_t{255}, .ordinaryRanges=intervals, .complete=true});
    pool->addNamedValue(number, {"UNLIMITED", int64_t{255}, Schema::NamedValue::Role::Special,
        Schema::NamedValue::Meaning::Unbounded});
    pool->addNamedValue(mask, {"HIGH", uint64_t{1} << 63, Schema::NamedValue::Role::Flag});
    pool->setBitWidth(mask, 64);
    pool->finalize();
    REQUIRE(pool->domainCount() == 5);
    REQUIRE(pool->frozen());
    REQUIRE(pool->strings()->frozen());
    REQUIRE_THROWS_AS(pool->addSchema(Schema::Kind::Null), std::logic_error);
    std::ostringstream stream;
    REQUIRE(pool->write(stream));
    auto bytes = stream.str();
    auto decoded = SchemaPool::read(std::span(reinterpret_cast<uint8_t const*>(bytes.data()), bytes.size()));
    auto imported = std::make_shared<SchemaPool>();
    REQUIRE(imported->addSchema(Schema::Kind::Bool) == 1);
    auto remap = imported->importGraph(*decoded);
    imported->finalize();
    REQUIRE(remap == std::vector<SchemaId>{0, 2, 3, 4, 5, 6});
    for (auto const& owner : {SchemaPool::ConstPtr(pool), decoded}) {
        auto const* root = owner->domain(object);
        REQUIRE(root->typeName() == "Measurement");
        REQUIRE(root->fieldSchema(owner->strings()->get("values")) == array);
        REQUIRE(root->reachabilityComplete());
        REQUIRE_FALSE(root->canHaveField(owner->strings()->get("secret")));
        REQUIRE(root->fieldRole(owner->strings()->get("context")) == Schema::FieldRole::Context);
        REQUIRE(root->fieldRequired(owner->strings()->get("samples")) == true);
        REQUIRE(owner->domain(array)->itemSchema() == number);
        REQUIRE(owner->domain(mask)->bitWidth() == 64);
        REQUIRE(std::get<uint64_t>(owner->domain(mask)->namedValue("HIGH")->value) == uint64_t{1} << 63);
        REQUIRE(owner->domain(number)->numericDomain()->unit == "km/h");
        REQUIRE(owner->domain(number)->numericDomain()->ordinaryRanges.size() == 1);
        REQUIRE(owner->schemaMemoryUsage().allocatedBytes >= owner->schemaMemoryUsage().logicalBytes);
    }
    REQUIRE(imported->domain(remap[object])->fieldSchema(imported->strings()->get("samples")) == remap[array]);
    REQUIRE(imported->domain(remap[context])->fieldSchema(imported->strings()->get("parent")) == remap[object]);
    auto binding = pool->binding("test-schema-pool");
    auto descriptor = pool->domainNode(object);
    std::weak_ptr<SchemaPool> weak = pool;
    pool.reset();
    REQUIRE_FALSE(weak.expired());
    REQUIRE(binding->lookup(object)->typeName() == "Measurement");
    REQUIRE(descriptor->get(StringPool::SchemaTypeName)->value() == ScalarValueType{std::string("Measurement")});
    descriptor = {}; binding.reset();
    REQUIRE(weak.expired());
    for (auto length : {size_t{0}, size_t{3}, bytes.size() - 1})
        REQUIRE_THROWS(SchemaPool::read(std::span(reinterpret_cast<uint8_t const*>(bytes.data()), length)));
}

TEST_CASE("SchemaPool rejects dangling domains and inherited cycles before publication", "[model.schema-pool]")
{
    auto pool = std::make_shared<SchemaPool>();
    auto root = pool->addSchema(Schema::Kind::Object);
    SECTION("Dangling edge") { pool->addFieldSchema(root, "bad", 42); }
    SECTION("Inheritance cycle") { pool->setBaseSchema(root, root); }
    REQUIRE_THROWS_AS(pool->finalize(), std::invalid_argument);
    REQUIRE_FALSE(pool->finalized());
    REQUIRE_THROWS_AS(pool->addSchema(Schema::Kind::Int), std::logic_error);
}

TEST_CASE("SchemaPool preserves empty names unknown alternatives and local combiner context", "[model.schema-pool]")
{
    auto pool = std::make_shared<SchemaPool>();
    auto object = pool->addSchema(Schema::Kind::Object);
    auto text = pool->addSchema(Schema::Kind::String);
    auto choice = pool->addSchema(Schema::Kind::Union);
    pool->addFieldSchema(object, "z", text);
    pool->addFieldSchema(object, "a", text);
    pool->addFieldSchema(object, "", text);
    pool->addFieldSchema(object, "partial", text);
    pool->addFieldSchema(object, "partial", NoSchemaId);
    pool->addEnumSymbol(text, "Z"); pool->addEnumSymbol(text, "A"); pool->addEnumSymbol(text, "");
    pool->setComposition(choice, Schema::Composition::AnyOf);
    pool->addAlternative(choice, object);
    pool->addFieldSchema(choice, "context", object);
    pool->setFieldRole(choice, "context", Schema::FieldRole::Context);
    pool->finalize();
    std::ostringstream encoded;
    REQUIRE(pool->write(encoded));
    auto bytes = encoded.str();
    auto decoded = SchemaPool::read(std::span(reinterpret_cast<uint8_t const*>(bytes.data()), bytes.size()));
    auto imported = std::make_shared<SchemaPool>();
    auto remap = imported->importGraph(*decoded);
    REQUIRE(remap == std::vector<SchemaId>{0, 1, 2, 3});
    imported->finalize();
    for (auto const& current : {SchemaPool::ConstPtr(pool), decoded, SchemaPool::ConstPtr(imported)}) {
        REQUIRE(current->domain(object)->fieldSchema(StringPool::Empty) == text);
        REQUIRE(current->domain(object)->fieldSchema(current->strings()->get("partial")) == NoSchemaId);
        REQUIRE(current->domain(choice)->fieldRole(current->strings()->get("context")) == Schema::FieldRole::Context);
        REQUIRE_FALSE(current->domain(object)->reachabilityComplete());
        REQUIRE(std::ranges::equal(current->fieldNames(object), std::vector<std::string_view>{"z", "a", "", "partial"}));
        REQUIRE(std::ranges::equal(current->fieldNames(object, true), std::vector<std::string_view>{"", "a", "partial", "z"}));
        REQUIRE(std::ranges::equal(current->enumNames(text), std::vector<std::string_view>{"Z", "A", ""}));
        REQUIRE(std::ranges::equal(current->enumNames(text, true), std::vector<std::string_view>{"", "A", "Z"}));
        auto descriptor = current->domainNode(choice);
        REQUIRE(descriptor->get(StringPool::SchemaAlternatives));
        auto fields = descriptor->get(StringPool::SchemaFields);
        REQUIRE(fields);
        auto context = fields->get(current->strings()->get("context"));
        REQUIRE(context);
        REQUIRE(context->get(StringPool::SchemaRole)->value() == ScalarValueType{std::string("context")});
        REQUIRE(current->schemaStatistics().contextFields == 1);
        REQUIRE(current->schemaStatistics().domains == 3);
    }
}

TEST_CASE("SchemaPool numeric replacements remain canonical and flags respect their encoding", "[model.schema-pool]")
{
    auto pool = std::make_shared<SchemaPool>();
    auto first = pool->addSchema(Schema::Kind::Int);
    auto second = pool->addSchema(Schema::Kind::Int);
    std::vector<Schema::NumericRange> oldRanges{{int64_t{0}, int64_t{100}}};
    std::vector<Schema::NumericRange> ranges{{int64_t{1}, int64_t{5}}, {int64_t{10}, int64_t{15}}};
    // Setter order does not dictate wire ownership, and overwritten ranges are discarded.
    pool->setNumericDomain(second, {.unit="old", .ordinaryRanges=oldRanges});
    pool->setNumericDomain(first, {.unit="count", .ordinaryRanges=ranges});
    pool->setNumericDomain(second, {.unit="m", .ordinaryRanges=ranges});
    pool->finalize();
    REQUIRE(pool->schemaStatistics().numericDomains == 2);
    std::ostringstream encoded;
    REQUIRE(pool->write(encoded));
    auto bytes = encoded.str();
    auto restored = SchemaPool::read(std::span(reinterpret_cast<uint8_t const*>(bytes.data()), bytes.size()));
    REQUIRE(restored->domain(second)->numericDomain()->unit == "m");
    REQUIRE(restored->domain(second)->numericDomain()->ordinaryRanges.size() == 2);
    std::ostringstream roundtrip;
    REQUIRE(restored->write(roundtrip));
    REQUIRE(roundtrip.str() == bytes);

    Schema::NamedValue flag{"FLAG", uint64_t{1}, Schema::NamedValue::Role::Flag};
    SECTION("String mask") { flag.value = std::string_view("not-a-mask"); }
    SECTION("Boolean mask") { flag.value = true; }
    SECTION("Negative mask") { flag.value = int64_t{-1}; }
    SECTION("Floating mask") { flag.value = 1.; }
    SECTION("Out-of-width mask") { flag.value = uint64_t{256}; }
    auto invalid = std::make_shared<SchemaPool>();
    auto mask = invalid->addSchema(Schema::Kind::Bitmask);
    invalid->setBitWidth(mask, 8);
    invalid->addNamedValue(mask, flag);
    REQUIRE_THROWS_AS(invalid->finalize(), std::invalid_argument);
}

TEST_CASE("SchemaPool inherited shadows preserve declaration order and member metadata", "[model.schema-pool]")
{
    auto pool = std::make_shared<SchemaPool>();
    auto base = pool->addSchema(Schema::Kind::Object);
    auto derived = pool->addSchema(Schema::Kind::Object);
    auto integer = pool->addSchema(Schema::Kind::Int);
    auto text = pool->addSchema(Schema::Kind::String);
    pool->setBaseSchema(derived, base);
    for (size_t i = 0; i < 2048; ++i)
        pool->addFieldSchema(base, "field" + std::to_string(i), integer);
    for (size_t i = 2048; i-- > 0;)
        pool->addFieldSchema(derived, "field" + std::to_string(i), text);
    pool->setFieldRole(derived, "field0", Schema::FieldRole::Context);
    pool->finalize();
    auto view = pool->domain(derived);
    REQUIRE(view->directFields().size() == 2048);
    REQUIRE(pool->fieldNames(derived).front() == "field2047");
    REQUIRE(pool->fieldNames(derived).back() == "field0");
    REQUIRE(view->fieldSchema(pool->strings()->get("field0")) == text);
    size_t enumerable = 0;
    view->forEachEnumerableField([&](StringId name, auto children) {
        REQUIRE(name != pool->strings()->get("field0"));
        REQUIRE(children.size() == 1);
        REQUIRE(children.front() == text);
        ++enumerable;
    });
    REQUIRE(enumerable == 2047);
}

TEST_CASE("Context edges support explicit access without implicit discovery", "[model.schema-domain][schema.context]")
{
    Domains d;
    auto& root = d.add<ObjectSchema>(1);
    root.addField(d.name("$payload"), {2});
    root.addField(d.name("context"), {3}, true, Schema::FieldRole::Context);
    root.addField(d.name("$feature"), {3}, true, Schema::FieldRole::Context);
    d.add<ValueSchema>(2, Schema::Kind::Int);
    auto& context = d.add<ObjectSchema>(3);
    context.addField(d.name("secret"), {2});
    context.addField(d.name("hiddenSymbol"), {4});
    d.add<ValueSchema>(4, Schema::Kind::String).addEnumSymbol(d.name("CONTEXT_ONLY"));
    // Unknown metadata behind a context link cannot make the data index incomplete.
    context.setOpen(true);
    d.finalize();
    REQUIRE(root.reachabilityComplete());
    REQUIRE_FALSE(root.canHaveField(d.name("secret")));
    REQUIRE(root.canHaveField(d.name("context")));
    REQUIRE(root.fieldSchema(d.name("context")) == 3);
    REQUIRE(root.fieldRole(d.name("context")) == Schema::FieldRole::Context);
    REQUIRE(root.fieldRole(d.name("$payload")) == Schema::FieldRole::Data);
    bool complete = false;
    REQUIRE(Schema::fieldPaths(1, d.lookup(), d.name("secret"), &complete).empty());
    REQUIRE(complete);
    REQUIRE(Schema::fieldPaths(1, d.lookup(), d.name("context")).size() == 1);
    REQUIRE(Schema::enumSymbolPaths(1, d.lookup(), d.name("CONTEXT_ONLY"), &complete).empty());
    REQUIRE(complete);
    std::vector<StringId> enumerable;
    root.forEachEnumerableField([&](StringId field, auto) { enumerable.push_back(field); });
    REQUIRE(enumerable == std::vector<StringId>{d.name("$payload")});
    REQUIRE_FALSE(contains(d.complete("sec"), "secret"));
    REQUIRE_FALSE(contains(d.complete("CONTEXT_"), "CONTEXT_ONLY"));
    REQUIRE(contains(d.complete("context.sec"), "secret"));
    REQUIRE(contains(d.complete("$feature.sec"), "secret"));
    REQUIRE_FALSE(contains(d.complete("*.sec"), "secret"));

    auto descriptors = std::make_shared<SchemaModel>(d.strings, d.lookup());
    auto edge = descriptors->root(1)->get(d.name("fields"))->get(d.name("context"));
    REQUIRE(edge->get(d.name("role"))->value() == ScalarValueType{std::string("context")});
    REQUIRE(edge->get(d.name("$ref"))->value() == ScalarValueType{int64_t{3}});

    auto model = std::make_shared<ModelPool>(d.strings);
    auto binding = std::make_shared<SchemaBinding>("context-test", d.graph, d.lookup(), d.strings);
    model->setSchemaBinding(binding);
    auto payload = model->newObject();
    REQUIRE(payload->setSchema(1));
    REQUIRE(payload->addField("$payload", int64_t{7}));
    auto hidden = model->newObject();
    REQUIRE(hidden->setSchema(3));
    REQUIRE(hidden->addField("secret", int64_t{42}));
    auto overlay = model_ptr<OverlayNode>::make(Value::field(payload));
    overlay->set(d.name("context"), Value::field(hidden));
    overlay->set(d.name("$feature"), Value::field(hidden));
    for (bool planned : {false, true}) {
        Environment env(d.strings);
        env.enableWildcardFieldPlans = planned;
        env.querySchemaCallback = d.lookup();
        for (auto query : {"context.secret", "**.context.secret", "$feature.secret"}) {
            INFO(query);
            auto expression = compile(env, query, false);
            REQUIRE(expression);
            auto values = eval(env, **expression, *overlay, nullptr);
            REQUIRE(values);
            REQUIRE(values->size() == 1);
            REQUIRE(values->front().as<ValueType::Int>() == 42);
        }
        auto expression = compile(env, "count(**.secret)", false);
        REQUIRE(expression);
        auto values = eval(env, **expression, *overlay, nullptr);
        REQUIRE(values);
        REQUIRE(values->front().as<ValueType::Int>() == 0);
    }
}

TEST_CASE("Context classification is conservative across repeated and alternative edges", "[model.schema-domain][schema.context]")
{
    Domains d;
    auto field = d.name("parent");
    auto& context = d.add<ObjectSchema>(1);
    context.addField(field, {3}, {}, Schema::FieldRole::Context);
    auto& data = d.add<ObjectSchema>(2);
    data.addField(field, {3});
    d.add<ValueSchema>(3, Schema::Kind::Int);
    auto& allContext = d.add<CombinedSchema>(4, Schema::Composition::AnyOf);
    allContext.addAlternative(1);
    auto& mixed = d.add<CombinedSchema>(5, Schema::Composition::AnyOf);
    mixed.addAlternative(1);
    mixed.addAlternative(2);
    auto& unknown = d.add<CombinedSchema>(6, Schema::Composition::AnyOf);
    unknown.addAlternative(1);
    unknown.addAlternative(NoSchemaId);
    d.finalize();
    REQUIRE(allContext.fieldRole(field) == Schema::FieldRole::Context);
    REQUIRE(mixed.fieldRole(field) == Schema::FieldRole::Data);
    REQUIRE(unknown.fieldRole(field) == Schema::FieldRole::Data);
    context.addField(field, {3});
    REQUIRE(context.fieldRole(field) == Schema::FieldRole::Data);
    std::vector<StringId> fields;
    context.forEachEnumerableField([&](StringId name, auto) { fields.push_back(name); });
    REQUIRE(fields.size() == 2);
}

TEST_CASE("Static schema names agree across isolated pools", "[model.schema-domain]")
{
    // Heap lifetimes let Helgrind distinguish these locks from reused stack mutex addresses.
    auto left = std::make_shared<StringPool>();
    auto right = std::make_shared<StringPool>();
    for (StringId id = StringPool::SchemaUnknown; id < StringPool::NextStaticId; ++id) {
        auto name = left->resolve(id);
        REQUIRE(name);
        REQUIRE(right->resolve(id) == name);
        REQUIRE(left->get(*name) == id);
        REQUIRE(right->get(*name) == id);
    }
    REQUIRE(StringPool::NextStaticId < StringPool::FirstDynamicId);
    REQUIRE(left->emplace("dynamic-domain-name").value() >= StringPool::FirstDynamicId);
}

TEST_CASE("Registry adapters participate in sparse plans and retain incomplete metadata", "[model.schema-domain]")
{
    Domains d;
    auto& foreign = d.add<ForeignSchema>(1);
    d.add<ValueSchema>(2, Schema::Kind::Int);
    d.add<ObjectSchema>(3).addField(d.name("target"), {2});
    auto model = std::make_shared<ModelPool>(d.strings);
    auto root = model->newObject();
    root->setSchema(1);
    for (auto i = 0; i < 32; ++i) {
        auto name = "noise" + std::to_string(i);
        foreign.definition.addField(d.name(name), {2});
        root->addField(name, int64_t(i));
    }
    foreign.definition.addField(d.name("branch"), {3});
    auto branch = model->newObject();
    branch->setSchema(3);
    branch->addField("target", int64_t{42});
    root->addField("branch", branch);
    d.finalize();
    Environment env(d.strings);
    env.querySchemaCallback = d.lookup();
    auto ast = compile(env, "**.target", false);
    REQUIRE(ast);
    Diagnostics planned, basic;
    auto optimized = eval(env, **ast, *root, &planned);
    env.enableWildcardFieldPlans = false;
    auto unplanned = eval(env, **ast, *root, &basic);
    REQUIRE(optimized);
    REQUIRE(unplanned);
    REQUIRE(optimized->size() == unplanned->size());
    REQUIRE(optimized->front().toString() == "42");
    REQUIRE(planned.fieldData_[0].evaluations < basic.fieldData_[0].evaluations);

    foreign.indexed = false;
    REQUIRE(contains(d.complete("tar"), "target"));
    auto& parent = d.add<ObjectSchema>(4);
    parent.addField(d.name("wrapped"), {1});
    d.finalize();
    REQUIRE_FALSE(parent.reachabilityComplete());
    REQUIRE(parent.canHaveField(d.name("target")));
    REQUIRE(contains(d.complete("tar", 4), "target"));
    d.add<ArraySchema>(5);
    parent.addField(d.name("untypedItems"), {5});
    d.finalize();
    bool exhaustive = true;
    (void)Schema::enumSymbolPaths(4, d.lookup(), d.name("READY"), &exhaustive);
    REQUIRE_FALSE(exhaustive);
}

TEST_CASE("Lookup aliases resolve direct and recursive schema paths canonically", "[model.schema-domain]")
{
    Domains d;
    auto& root = d.add<LookupAliasSchema>(1);
    root.alias = d.name("publicName");
    root.target = d.name("storedName");
    root.addField(root.target, {2});
    d.add<ObjectSchema>(2).addField(d.name("value"), {3});
    d.add<ValueSchema>(3, Schema::Kind::Int);
    d.add<ObjectSchema>(4).addField(d.name("wrapper"), {1});
    d.finalize();
    REQUIRE(Schema::fieldSchemas(1, d.lookup(), root.alias) == std::vector<SchemaId>{2});
    REQUIRE(Schema::fieldPaths(1, d.lookup(), root.alias).front().front().field == root.target);
    REQUIRE(std::ranges::find(root.directFields(), root.alias) == root.directFields().end());
    REQUIRE(contains(d.complete("publicName.va"), "value"));
    Environment env(d.strings);
    env.querySchemaCallback = d.lookup();
    for (auto query : {"publicName.value", "**.publicName.value", "**.publicName.value == 'x'"}) {
        auto ast = compile(env, query, false);
        REQUIRE(ast);
        auto refs = referencedSchemaPaths(env, **ast, 1);
        REQUIRE(refs);
        REQUIRE_FALSE(refs->hasUnresolvedAccess);
        REQUIRE(refs->paths.size() == 1);
        REQUIRE(refs->paths.front().path.front().field == root.target);
    }
    auto ast = compile(env, "**.publicName.value", false);
    REQUIRE(ast);
    auto refs = referencedSchemaPaths(env, **ast, 4);
    REQUIRE(refs);
    REQUIRE_FALSE(refs->hasUnresolvedAccess);
    REQUIRE(refs->paths.front().path[1].field == root.target);
    root.addField(root.alias, {3});
    d.finalize();
    REQUIRE(Schema::fieldSchemas(1, d.lookup(), root.alias) == std::vector<SchemaId>{3});
}

TEST_CASE("Domain completion preserves alias scope and never executes custom functions", "[completion.domain]")
{
    Domains d;
    auto& root = d.add<AliasSchema>(1);
    root.alias = d.name("alias");
    root.path = {{SchemaPathSegment::Kind::Field, d.name("child")}, {SchemaPathSegment::Kind::Field, d.name("mode")}};
    root.addField(d.name("child"), {2});
    root.addField(root.alias, {4});
    d.add<ObjectSchema>(2).addField(d.name("mode"), {3});
    d.add<ValueSchema>(3, Schema::Kind::String).addEnumSymbol(d.name("urban"));
    d.add<ValueSchema>(4, Schema::Kind::String).addEnumSymbol(d.name("unrelated"));
    d.finalize();
    REQUIRE(contains(d.complete("alias == u"), "\"urban\""));
    REQUIRE_FALSE(contains(d.complete("alias == u"), "\"unrelated\""));
    REQUIRE_FALSE(contains(d.complete("_.alias == u"), "\"urban\""));
    REQUIRE(contains(d.complete("_.alias == u"), "\"unrelated\""));

    Environment env(d.strings);
    env.querySchemaCallback = d.lookup();
    InvocationProbe probe;
    env.functions["probe"] = &probe;
    for (auto query : {"probe() and child.mo", "probe(child.mo)", "probe().mo"}) {
        auto cursor = std::string_view(query).find("mo") + 2;
        auto result = complete(env, query, cursor, SchemaId{1});
        REQUIRE(result);
        if (std::string_view(query) != "probe().mo")
            REQUIRE(contains(*result, "mode"));
        REQUIRE(probe.calls == 0);
    }
}

TEST_CASE("String enum metadata has one path through descriptors completion and rewrites", "[model.schema-domain]")
{
    Domains d;
    d.add<ObjectSchema>(1).addField(d.name("mode"), {2});
    auto& value = d.add<ValueSchema>(2, Schema::Kind::String);
    REQUIRE_THROWS_AS(value.addEnumValue(std::string("READY")), std::invalid_argument);
    REQUIRE_THROWS_AS(value.addEnumValue(std::string_view("READY")), std::invalid_argument);
    value.addEnumSymbol(d.name("READY"));
    d.finalize();
    auto descriptor = std::make_shared<SchemaModel>(d.strings, d.lookup());
    REQUIRE(descriptor->root(1)->toJson()["fields"]["mode"]["enum"][0] == "READY");
    REQUIRE(contains(d.complete("REA"), "\"READY\""));
    Environment env(d.strings);
    env.querySchemaCallback = d.lookup();
    auto ast = compile(env, "READY", CompileOptions{false, RewriteMode::Schema, 1});
    REQUIRE(ast);
    REQUIRE((*ast)->expr().toString().find("mode") != std::string::npos);
}

TEST_CASE("Shared schema branches and repeated field names keep distinct enum paths", "[model.schema-domain]")
{
    Domains d;
    auto& root = d.add<ObjectSchema>(1);
    root.addField(d.name("left"), {2});
    root.addField(d.name("right"), {2});
    d.add<ObjectSchema>(2).addField(d.name("left"), {3});
    d.add<ValueSchema>(3, Schema::Kind::String).addEnumSymbol(d.name("READY"));
    d.finalize();
    bool exhaustive = false;
    auto paths = Schema::enumSymbolPaths(1, d.lookup(), d.name("READY"), &exhaustive);
    REQUIRE(exhaustive);
    REQUIRE(paths.size() == 2);
    REQUIRE(paths[0].size() == 2);
    REQUIRE(paths[1].size() == 2);
}

TEST_CASE("Field paths prune unrelated shared graphs without dropping distinct owners", "[model.schema-domain][schema.field-paths]")
{
    Domains d;
    auto left = d.name("left");
    auto right = d.name("right");
    auto target = d.name("target");
    auto& root = d.add<ObjectSchema>(1);
    root.addField(d.name("conditions"), {10});
    root.addField(left, {2});
    root.addField(right, {2});
    d.add<ObjectSchema>(2).addField(target, {3});
    d.add<ValueSchema>(3, Schema::Kind::Int);

    // A tiny shared graph has over 30,000 paths, none relevant to this field.
    for (SchemaId id = 10; id < 24; ++id) {
        auto& branch = d.add<ObjectSchema>(id);
        branch.addField(left, {SchemaId(id + 1)});
        branch.addField(right, {SchemaId(id + 1)});
    }
    d.add<ValueSchema>(24, Schema::Kind::Int);
    SECTION("Acyclic shared graph") {}
    SECTION("Unrelated recursive graph") {
        d.add<ObjectSchema>(24).addField(left, {24});
    }
    d.finalize();

    std::size_t lookups = 0;
    auto lookup = [&](SchemaId id) {
        ++lookups;
        return d.lookup()(id);
    };
    bool exhaustive = false;
    auto paths = Schema::fieldPaths(1, lookup, target, &exhaustive);
    REQUIRE(exhaustive);
    REQUIRE(paths == std::vector<SchemaPath>{
        {{SchemaPathSegment::Kind::Field, left}, {SchemaPathSegment::Kind::Field, target}},
        {{SchemaPathSegment::Kind::Field, right}, {SchemaPathSegment::Kind::Field, target}}});
    REQUIRE(lookups < 16);

    Environment env(d.strings);
    env.querySchemaCallback = lookup;
    auto ast = compile(env, "**.target", false);
    REQUIRE(ast);
    auto refs = referencedSchemaPaths(env, **ast, 1);
    REQUIRE(refs);
    REQUIRE_FALSE(refs->hasUnresolvedAccess);
    REQUIRE(refs->paths.size() == 2);
}

TEST_CASE("Field path analysis reports partial recursive and bounded results", "[model.schema-domain][schema.field-paths]")
{
    Domains d;
    auto next = d.name("next");
    auto& root = d.add<ObjectSchema>(1);
    root.addField(d.name("group"), {2});
    d.add<ObjectSchema>(2).addField(d.name("target"), {3});
    d.add<ValueSchema>(3, Schema::Kind::String);

    SECTION("A recursive branch can contain further matches") {
        root.addField(next, {1});
    }
    SECTION("A depth-limited branch can contain further matches") {
        root.addField(next, {10});
        for (SchemaId id = 10; id < 140; ++id)
            d.add<ObjectSchema>(id).addField(next, {SchemaId(id + 1)});
        d.add<ObjectSchema>(140).addField(d.name("group"), {2});
    }
    SECTION("Open metadata cannot exclude further matches") {
        root.setOpen(true);
    }
    d.finalize();

    bool exhaustive = true;
    auto paths = Schema::fieldPaths(1, d.lookup(), d.name("target"), &exhaustive);
    REQUIRE_FALSE(exhaustive);
    REQUIRE(paths.size() == 1);
    Environment env(d.strings);
    env.querySchemaCallback = d.lookup();
    for (auto query : {"**.target", "**.group.target", "**.group.target == 'x'"}) {
        CAPTURE(query);
        auto ast = compile(env, query, false);
        REQUIRE(ast);
        auto refs = referencedSchemaPaths(env, **ast, 1);
        REQUIRE(refs);
        REQUIRE(refs->paths.size() == 1);
        REQUIRE(refs->hasUnresolvedAccess);
    }
}

TEST_CASE("Field path pruning preserves aliases and unindexed adapter fields", "[model.schema-domain][schema.field-paths]")
{
    Domains d;
    auto& foreign = d.add<ForeignSchema>(1);
    foreign.definition.addField(d.name("branch"), {2});
    auto& aliased = d.add<LookupAliasSchema>(2);
    aliased.alias = d.name("alias");
    aliased.target = d.name("target");
    aliased.addField(aliased.target, {3});
    d.add<ValueSchema>(3, Schema::Kind::Int);
    d.finalize();
    foreign.indexed = false;

    bool exhaustive = false;
    auto paths = Schema::fieldPaths(1, d.lookup(), aliased.alias, &exhaustive);
    REQUIRE(exhaustive);
    REQUIRE(paths.size() == 1);
    REQUIRE(paths.front().back().field == aliased.target);

    // A real member wins over the lookup-only alias, including after mutation.
    aliased.addField(aliased.alias, {3});
    paths = Schema::fieldPaths(1, d.lookup(), aliased.alias, &exhaustive);
    REQUIRE(exhaustive);
    REQUIRE(paths.size() == 1);
    REQUIRE(paths.front().back().field == aliased.alias);
}

TEST_CASE("Field paths still bound shared graphs whose branches all match", "[model.schema-domain][schema.field-paths]")
{
    Domains d;
    auto left = d.name("left");
    auto right = d.name("right");
    for (SchemaId id = 1; id < 15; ++id) {
        auto& branch = d.add<ObjectSchema>(id);
        branch.addField(left, {SchemaId(id + 1)});
        branch.addField(right, {SchemaId(id + 1)});
    }
    d.add<ObjectSchema>(15).addField(d.name("target"), {16});
    d.add<ValueSchema>(16, Schema::Kind::Int);
    d.finalize();

    std::size_t lookups = 0;
    auto lookup = [&](SchemaId id) {
        ++lookups;
        return d.lookup()(id);
    };
    bool exhaustive = true;
    auto paths = Schema::fieldPaths(1, lookup, d.name("target"), &exhaustive);
    REQUIRE_FALSE(exhaustive);
    REQUIRE_FALSE(paths.empty());
    REQUIRE(paths.size() < 10000);
    REQUIRE(lookups <= 10000);
}

TEST_CASE("Enum matches beyond the path enumeration depth limit remain queryable", "[model.schema-domain]")
{
    Domains d;
    auto next = d.name("next");
    for (SchemaId id = 1; id <= 132; ++id)
        d.add<ObjectSchema>(id).addField(next, {SchemaId(id + 1)});
    d.add<ValueSchema>(133, Schema::Kind::String).addEnumSymbol(d.name("READY"));
    d.finalize();
    bool exhaustive = true;
    REQUIRE(Schema::enumSymbolPaths(1, d.lookup(), d.name("READY"), &exhaustive).empty());
    REQUIRE_FALSE(exhaustive);
    auto model = std::make_shared<ModelPool>(d.strings);
    auto root = model->newObject();
    auto current = root;
    for (auto i = 1; i < 132; ++i) {
        auto child = model->newObject();
        current->addField("next", child);
        current = child;
    }
    current->addField("next", "READY");
    Environment env(d.strings);
    env.querySchemaCallback = d.lookup();
    auto ast = compile(env, "READY", CompileOptions{false, RewriteMode::Schema, 1});
    REQUIRE(ast);
    auto result = eval(env, **ast, *root, nullptr);
    REQUIRE(result);
    REQUIRE(result->front().isBool(true));
}

TEST_CASE("Domain completion performance", "[perf.completion]")
{
    if (RUNNING_ON_VALGRIND)
        SKIP("Skipping benchmarks when running under valgrind");
    Domains d;
    auto& root = d.add<ObjectSchema>(1);
    root.addField(d.name("items"), {2});
    d.add<ArraySchema>(2).addElementSchemas({3, 4});
    auto& item = d.add<ObjectSchema>(3);
    auto& alternate = d.add<CombinedSchema>(4, Schema::Composition::AnyOf);
    alternate.addAlternative(3);
    alternate.addAlternative(5);
    d.add<ValueSchema>(5, Schema::Kind::Int);
    for (auto i = 0; i < 256; ++i)
        item.addField(d.name("field" + std::to_string(i)), {5});
    d.finalize();
    auto model = std::make_shared<ModelPool>(d.strings);
    auto node = model->newObject();
    node->setSchema(1);
    Environment env(d.strings);
    env.querySchemaCallback = d.lookup();
    CompletionOptions options;
    options.showWildcardHints = false;
    options.limit = 1000;
    auto schemaResult = complete(env, "fi", 2, SchemaId{1}, options);
    auto modelResult = complete(env, "fi", 2, *node, options);
    REQUIRE(schemaResult);
    REQUIRE(modelResult);
    REQUIRE(schemaResult->size() == 256);
    REQUIRE(schemaResult->size() == modelResult->size());
    auto nested = complete(env, "items[17].fi", 12, SchemaId{1}, options);
    REQUIRE(nested);
    REQUIRE(nested->size() == 256);
    BENCHMARK("schema model root completion") { return complete(env, "fi", 2, *node, options); };
    BENCHMARK("direct domain root completion") { return complete(env, "fi", 2, SchemaId{1}, options); };
    BENCHMARK("direct nested array union completion") { return complete(env, "items[17].fi", 12, SchemaId{1}, options); };
}
