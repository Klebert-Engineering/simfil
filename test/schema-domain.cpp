#include "common.hpp"
#include "simfil/model/schema-node.h"
#include "simfil/model/schema-pool.h"
#include "simfil/function.h"
#include "simfil/overlay.h"

#include <catch2/benchmark/catch_benchmark.hpp>
#include <bitsery/adapter/buffer.h>
#include <bitsery/bitsery.h>
#include <bitsery/traits/string.h>

using namespace simfil;

namespace
{

/** Complete against a canonical root without executing sample values or functions. */
auto completeType(SchemaType root, std::string_view query, CompletionOptions options = {},
                  std::optional<std::size_t> cursor = {}) -> std::vector<CompletionCandidate>
{
    Environment env(root.pool()->strings());
    options.showWildcardHints = false;
    auto result = simfil::complete(env, query, cursor.value_or(query.size()), root, options);
    INFO(query);
    REQUIRE(result);
    return *result;
}

/** Model an array whose named lookups do not participate in element iteration. */
class NamedArrayPool : public ModelPool
{
public:
    using ModelPool::ModelPool;
    ModelNodeAddress context;

    /** Resolve a custom view over core array storage, retaining its declared type. */
    auto resolve(ModelNode const& node, ResolveFn const& cb) const -> tl::expected<void, Error> override
    {
        if (node.addr().column() == FirstCustomColumnId) {
            cb(*model_ptr<NamedArray>::make(shared_from_this(), node.addr()));
            return {};
        }
        return ModelPool::resolve(node, cb);
    }

private:
    /** Reuse ordinary array enumeration while adding explicitly addressable context. */
    class NamedArray : public Array
    {
    public:
        using Array::Array;

        /** The context object is deliberately absent from at()/iterate(). */
        auto get(StringId const& key) const -> ModelNode::Ptr override
        {
            auto const& owner = static_cast<NamedArrayPool const&>(model());
            auto name = owner.strings()->resolve(key);
            if (name == "first" || name == "head")
                return at(0);
            if (name == "context")
                return ModelNode::Ptr::make(model_, owner.context);
            return {};
        }
    };
};

/** Check one candidate without depending on incidental function-name suggestions. */
auto contains(const std::vector<CompletionCandidate>& candidates, std::string_view text) -> bool
{
    return std::ranges::any_of(candidates, [&](const auto& candidate) { return candidate.text == text; });
}

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

/** Supply a domain-specific operand alias from the actual canonical owner. */
class AliasPool : public SchemaPool
{
protected:
    auto scalarFieldPathsForSymbol(TypeId root, StringId name) const -> std::vector<SchemaPath> override
    {
        if (root != TypeId{1} || name != dictionary().get("alias"))
            return {};
        return {{{SchemaPathSegment::Kind::Field, dictionary().get("child")},
                 {SchemaPathSegment::Kind::Field, dictionary().get("mode")}}};
    }
};

/** Exercise malformed owner storage that cannot be created through typed public edges. */
class MalformedSchemaPool : public SchemaPool
{
public:
    /** Inject an unresolved nonzero ID for publication's reference validator. */
    void addDanglingField(SchemaObjectType parent)
    {
        addFieldSchema(parent.id(), "bad", MaxTypeId);
    }
};

}

TEST_CASE("Packed schema kinds preserve extension affinity and concrete names", "[model.schema-type]")
{
    auto graph = std::make_shared<SchemaPool>();
    for (auto name : {"identifier", "missing"})
        REQUIRE(graph->strings()->emplace(name));
    auto root = graph->newObjectType();
    auto integer = graph->newScalarType(SchemaType::Kind::Int);
    constexpr StringId featureName = 200;
    graph->strings()->addStaticKey(featureName, "Feature");
    auto kind = SchemaType::makeKind(featureName, valueTypeAffinity(ValueType::Object));
    root.setKindName(featureName);
    REQUIRE(root.kind() == kind);
    root.setTypeName("RoadLink");
    root.addField("identifier", integer).setRequired(true);
    graph->finalize();
    REQUIRE(SchemaType::kindNameId(root.kind()) == featureName);
    REQUIRE(root.hasAffinity(ValueType::Object));
    REQUIRE_FALSE(root.hasAffinity(ValueType::Array));
    REQUIRE(root.field(graph->dictionary().get("identifier")).required() == true);
    REQUIRE_FALSE(root.field(graph->dictionary().get("missing")).required().has_value());
    auto path = root.firstScalarFieldPath();
    REQUIRE(path);
    REQUIRE(path->front().field == graph->dictionary().get("identifier"));
    REQUIRE(contains(completeType(root, "iden"), "identifier"));

    auto node = root.modelNode();
    REQUIRE(node->type() == ValueType::Object);
    REQUIRE(node->schema() == NoTypeId);
    REQUIRE(node->describedType() == root);
    REQUIRE(std::get<std::string_view>(node->get(StringPool::SchemaKind)->value()) == "Feature");
    auto json = node->toJson();
    REQUIRE(json["kind"] == "Feature");
    REQUIRE(json["typename"] == "RoadLink");
    REQUIRE(json["fields"]["identifier"]["required"] == true);
    REQUIRE(json["fields"]["identifier"]["kind"] == "integer");
    REQUIRE_FALSE(json["fields"]["identifier"].contains("typename"));
}

TEST_CASE("Portable enum predicates do not require runtime enum dictionary entries", "[model.schema-type]")
{
    auto compileGraph = std::make_shared<SchemaPool>();
    auto compileRoot = compileGraph->newObjectType();
    auto compileEnum = compileGraph->newEnumType();
    compileRoot.addField("mode", compileEnum);
    compileRoot.setOpen(true);
    compileEnum.addSymbol("READY");
    compileGraph->finalize();
    Environment compileEnv(compileGraph->strings());
    auto ast = compile(compileEnv, "READY", CompileOptions{
        .any = false, .rewriteMode = RewriteMode::Schema, .rootSchema = compileRoot});
    REQUIRE(ast);
    REQUIRE((*ast)->expr().toString().find("schema-enum") != std::string::npos);

    auto runtimeGraph = std::make_shared<SchemaPool>();
    REQUIRE(runtimeGraph->strings()->emplace("padding"));
    auto runtimeRoot = runtimeGraph->newObjectType();
    auto runtimeEnum = runtimeGraph->newEnumType();
    runtimeRoot.addField("mode", runtimeEnum);
    runtimeEnum.addSymbol("READY");
    runtimeGraph->finalize();
    auto model = std::make_shared<ModelPool>();
    model->setSchemaPool(runtimeGraph);
    auto matching = model->newObject();
    REQUIRE(matching->setSchema(runtimeRoot.id()));
    REQUIRE(matching->addField("mode", std::string_view("READY")));
    model->addRoot(matching);
    Environment runtimeEnv(model->strings());
    auto before = model->strings()->size();
    auto values = eval(runtimeEnv, **ast, *matching, nullptr);
    REQUIRE(values);
    REQUIRE(values->size() == 1);
    REQUIRE(values->front().as<ValueType::Bool>());
    REQUIRE(model->strings()->get("READY") == StringPool::Empty);
    REQUIRE(model->strings()->size() == before);
    auto unrelated = model->newObject();
    REQUIRE(unrelated->setSchema(runtimeRoot.id()));
    REQUIRE(unrelated->addField("unrelated", std::string_view("READY")));
    values = eval(runtimeEnv, **ast, *unrelated, nullptr);
    REQUIRE(values);
    REQUIRE_FALSE(values->front().as<ValueType::Bool>());
}

TEST_CASE("Descriptor nodes query metadata and retain graph ownership", "[model.schema-type]")
{
    auto graph = std::make_shared<SchemaPool>();
    for (auto name : {"items", "name", "urban"})
        REQUIRE(graph->strings()->emplace(name));
    auto root = graph->newObjectType();
    auto items = graph->newArrayType();
    auto item = graph->newObjectType();
    auto value = graph->newScalarType(SchemaType::Kind::String);
    root.addField("items", items);
    items.addElement(item);
    item.addField("name", value).setRequired(false);
    value.asEnum().addSymbol("urban");
    value.setNullable(true);
    graph->finalize();
    auto node = root.modelNode();
    std::weak_ptr owner = graph;
    auto strings = graph->strings();
    graph.reset();
    Environment env(strings);
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
    REQUIRE(node->toJson() == json);
    node = {};
    REQUIRE(owner.expired());
}

TEST_CASE("Schema descriptors bound recursion and expansion explicitly", "[model.schema-type]")
{
    auto graph = std::make_shared<SchemaPool>();
    auto root = graph->newObjectType();
    root.addField("next", root);
    graph->finalize();
    auto node = root.modelNode();
    auto json = node->toJson();
    REQUIRE(json["$ref"] == root.id());
    REQUIRE(json["fields"]["next"]["$ref"] == root.id());
    REQUIRE(json["fields"]["next"]["truncated"] == "cycle");
    REQUIRE_THROWS_AS(node->toJson({.maxNodes = 2}), std::length_error);
    // A failed export cannot poison subsequent navigation or serialization.
    REQUIRE(node->toJson() == json);
    auto next = node->get(StringPool::SchemaFields)->get(graph->dictionary().get("next"));
    REQUIRE(next->get(StringPool::SchemaFields));
}

TEST_CASE("Implicit schema unions retain all type references",
          "[model.schema-type]")
{
    auto graph = std::make_shared<SchemaPool>();
    for (auto name : {"choice"})
        REQUIRE(graph->strings()->emplace(name));
    auto root = graph->newObjectType();
    auto integer = graph->newScalarType(SchemaType::Kind::Int);
    auto text = graph->newScalarType(SchemaType::Kind::String);
    root.addField("choice", integer).addType(text);
    graph->finalize();
    auto json = root.modelNode()->toJson();
    REQUIRE(json["fields"]["choice"]["kind"] == "union");
    REQUIRE(json["fields"]["choice"]["$ref"] == nlohmann::json::array({2, 3}));
    REQUIRE(json["fields"]["choice"]["alternatives"][0]["kind"] == "integer");
    REQUIRE(json["fields"]["choice"]["alternatives"][1]["kind"] == "string");
}

TEST_CASE("Schema projections report traversal exhaustion without hiding deep matches", "[model.schema-type]")
{
    auto graph = std::make_shared<SchemaPool>();
    std::vector<SchemaObjectType> chain;
    for (size_t i = 0; i < 79; ++i)
        chain.push_back(graph->newObjectType());
    auto leaf = graph->newScalarType(SchemaType::Kind::String);
    leaf.setTypeName("DeepTarget");
    for (size_t i = 0; i < chain.size(); ++i)
        chain[i].addField("next", i + 1 == chain.size() ? SchemaType(leaf) : chain[i + 1]);
    graph->finalize();
    auto node = chain.front().modelNode();
    Environment env(graph->strings());
    auto ast = compile(env, "**.typename", false);
    REQUIRE(ast);
    auto values = eval(env, **ast, *node, nullptr);
    REQUIRE(values);
    REQUIRE(values->size() == 1);
    REQUIRE(values->front().as<ValueType::String>() == "DeepTarget");

    size_t emitted = 0;
    auto bounded = eval(env, **ast, *node,
        LambdaResultFn([&](Context, Value const&) { ++emitted; return Result::Continue; }),
        EvaluationOptions{.maxWork = 10});
    REQUIRE(bounded);
    REQUIRE(bounded->reason == EvaluationSummary::Reason::WorkLimit);
    REQUIRE(emitted == 0);
    values = eval(env, **ast, *node, nullptr);
    REQUIRE(values);
    REQUIRE(values->front().as<ValueType::String>() == "DeepTarget");
}

TEST_CASE("Type completion follows array indices and logical alternatives", "[completion.type]")
{
    auto graph = std::make_shared<SchemaPool>();
    for (auto name : {"items", "choice", "name"})
        REQUIRE(graph->strings()->emplace(name));
    auto root = graph->newObjectType();
    auto items = graph->newArrayType();
    auto item = graph->newObjectType();
    auto text = graph->newScalarType(SchemaType::Kind::String);
    auto choice = graph->newCompositionType(SchemaType::Composition::AnyOf);
    root.addField("items", items);
    root.addField("choice", choice);
    items.addElement(item);
    items.addElement(text);
    item.addField("name", text);
    choice.addAlternative(item);
    choice.addAlternative(text);
    graph->finalize();
    REQUIRE(contains(completeType(root, "items[17].na"), "name"));
    REQUIRE(contains(completeType(root, "items[-1].na"), "name"));
    REQUIRE(contains(completeType(root, "items.*.na"), "name"));
    REQUIRE(contains(completeType(root, "items.*{name == \"x\"}.na"), "name"));
    REQUIRE(contains(completeType(root, "choice.na"), "name"));
    REQUIRE(contains(completeType(root, "false and choice.na"), "name"));
    REQUIRE(contains(completeType(root, "true or choice.na"), "name"));
    REQUIRE(contains(completeType(root, "missing_function(choice.na)"), "name") == false);
    auto query = std::string("missing_function(choice.na)");
    REQUIRE(contains(completeType(root, query, {}, query.size() - 1), "name"));
    REQUIRE(contains(completeType(root, ""), "items"));
    auto json = root.modelNode()->toJson();
    REQUIRE(json["fields"]["choice"]["kind"] == "union");
    REQUIRE(json["fields"]["choice"]["alternatives"].size() == 2);
    REQUIRE_FALSE(json["fields"]["choice"].contains("elements"));
}

TEST_CASE("Type completion scopes enums to the compared operand", "[completion.type]")
{
    auto graph = std::make_shared<SchemaPool>();
    for (auto name : {"mode", "other", "urban", "rural", "unrelated"})
        REQUIRE(graph->strings()->emplace(name));
    auto root = graph->newObjectType();
    auto mode = graph->newScalarType(SchemaType::Kind::String);
    auto other = graph->newScalarType(SchemaType::Kind::String);
    root.addField("mode", mode);
    root.addField("other", other);
    mode.asEnum().addSymbol("urban");
    mode.asEnum().addSymbol("rural");
    other.asEnum().addSymbol("unrelated");
    graph->finalize();
    auto suggestions = completeType(root, "mode == u");
    REQUIRE(contains(suggestions, "\"urban\""));
    REQUIRE_FALSE(contains(suggestions, "\"unrelated\""));
    REQUIRE(contains(completeType(root, "mode == "), "\"urban\""));
    REQUIRE(contains(completeType(root, "mode == 'u"), "\"urban\""));
    REQUIRE(contains(completeType(root, "mode == r'u"), "\"urban\""));
    REQUIRE(contains(completeType(root, "ur"), "\"urban\""));
    CompletionOptions limited;
    limited.limit = 1;
    REQUIRE(completeType(root, "", limited).size() <= 1);
    limited.maxSchemaVisits = 0;
    REQUIRE(completeType(root, "", limited).empty());
}

TEST_CASE("Open and unresolved descendants do not prove fields absent", "[model.schema-type]")
{
    auto graph = std::make_shared<SchemaPool>();
    auto closed = graph->newObjectType();
    auto unknown = graph->newObjectType();
    auto open = graph->newObjectType();
    auto integer = graph->newScalarType(SchemaType::Kind::Int);
    closed.addField("known", integer);
    unknown.addField("known", integer);
    unknown.addField("unknown");
    open.setOpen(true);
    auto missing = graph->strings()->emplace("missing").value();
    auto anything = graph->strings()->emplace("anything").value();
    graph->finalize();
    REQUIRE(closed.reachabilityComplete());
    REQUIRE_FALSE(closed.canHaveField(missing));
    REQUIRE_FALSE(unknown.reachabilityComplete());
    REQUIRE(unknown.canHaveField(missing));
    REQUIRE(open.canHaveField(anything));
    REQUIRE(std::ranges::equal(open.fieldTypes(anything), std::vector<TypeId>{NoTypeId}));
    REQUIRE_THROWS(closed.addField("unknown"));
}

TEST_CASE("Recursive enum rewrites follow real paths without unrelated string matches", "[model.schema-type]")
{
    auto graph = std::make_shared<SchemaPool>();
    for (auto name : {"status", "next", "unrelated", "READY"})
        REQUIRE(graph->strings()->emplace(name));
    auto root = graph->newObjectType();
    auto status = graph->newScalarType(SchemaType::Kind::String);
    auto text = graph->newScalarType(SchemaType::Kind::String);
    root.addField("status", status);
    root.addField("next", root);
    root.addField("unrelated", text);
    status.asEnum().addSymbol("READY");
    graph->finalize();
    bool complete = true;
    REQUIRE_FALSE(root.enumSymbolPaths(graph->dictionary().get("READY"), &complete).empty());
    REQUIRE_FALSE(complete);
    Environment env(graph->strings());
    env.setSchema(root);
    auto ast = compile(env, "READY", CompileOptions{false, RewriteMode::Schema, root});
    REQUIRE(ast);
    auto model = std::make_shared<ModelPool>(graph->strings());
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
    auto references = referencedSchemaPaths(env, **ast, root);
    REQUIRE(references);
    REQUIRE(references->hasUnresolvedAccess);
    REQUIRE(referencedQueryTerms(**ast).stringLiterals.contains("READY"));
    REQUIRE(contains(completeType(root, "next.next.st"), "status"));
}

TEST_CASE("Combined types retain intersection and exclusive union semantics", "[model.schema-type]")
{
    auto graph = std::make_shared<SchemaPool>();
    auto combined = graph->newCompositionType(SchemaType::Composition::AllOf);
    auto integer = graph->newScalarType(SchemaType::Kind::Int);
    auto text = graph->newScalarType(SchemaType::Kind::String);
    auto exclusive = graph->newCompositionType(SchemaType::Composition::OneOf);
    combined.addAlternative(integer);
    combined.addAlternative(text);
    exclusive.addAlternative(integer);
    exclusive.addAlternative(text);
    graph->finalize();
    REQUIRE(SchemaType::affinities(combined.kind()) == 0);
    auto json = combined.modelNode()->toJson();
    REQUIRE(json["kind"] == "intersection");
    REQUIRE(json["alternatives"].size() == 2);
    REQUIRE(exclusive.hasAffinity(ValueType::Int));
    REQUIRE(exclusive.hasAffinity(ValueType::String));
    REQUIRE_FALSE(exclusive.hasAffinity(ValueType::Object));
}

TEST_CASE("Unrestricted and impossible types do not share pruning semantics", "[model.schema-type]")
{
    auto graph = std::make_shared<SchemaPool>();
    for (auto name : {"x"})
        REQUIRE(graph->strings()->emplace(name));
    auto unrestricted = graph->newCompositionType(SchemaType::Composition::AllOf);
    auto emptyUnion = graph->newCompositionType(SchemaType::Composition::AnyOf);
    auto never = graph->newType(SchemaType::Kind::Never);
    auto nestedIntersection = graph->newCompositionType(SchemaType::Composition::AllOf);
    nestedIntersection.addAlternative(unrestricted);
    graph->finalize();
    auto name = graph->dictionary().get("x");
    REQUIRE(unrestricted.canHaveField(name));
    REQUIRE(nestedIntersection.canHaveField(name));
    REQUIRE_FALSE(emptyUnion.canHaveField(name));
    REQUIRE_FALSE(never.canHaveField(name));
    REQUIRE(std::ranges::equal(unrestricted.fieldTypes(name), std::vector<TypeId>{NoTypeId}));
    REQUIRE(std::ranges::equal(unrestricted.itemTypes(), std::vector<TypeId>{NoTypeId}));
    auto model = std::make_shared<ModelPool>(graph->strings());
    auto root = model->newObject();
    root->addField("x", int64_t{42});
    root->setSchema(1);
    Environment env(graph->strings());
    env.setSchema(unrestricted);
    auto ast = compile(env, "**.x", false);
    REQUIRE(ast);
    auto result = eval(env, **ast, *root, nullptr);
    REQUIRE(result);
    REQUIRE(result->front().toString() == "42");
}

TEST_CASE("Type completion preserves source ranges escaping and literal enums", "[completion.type]")
{
    auto graph = std::make_shared<SchemaPool>();
    for (auto name : {"weird.field", "mode", "READY", "a\"b\\c"})
        REQUIRE(graph->strings()->emplace(name));
    auto root = graph->newObjectType();
    auto value = graph->newScalarType(SchemaType::Kind::Value);
    auto integer = graph->newScalarType(SchemaType::Kind::Int);
    root.addField("weird.field", value);
    root.addField("mode", value);
    root.addField("READY", integer);
    value.asEnum().addSymbol("READY");
    value.asEnum().addSymbol("a\"b\\c");
    value.asEnum().addValue(int64_t{42});
    value.asEnum().addValue(true);
    value.asEnum().addValue(std::monostate{});
    graph->finalize();
    auto stringsBefore = graph->strings()->size();
    REQUIRE(contains(completeType(root, "we"), "[\"weird.field\"]"));
    REQUIRE(contains(completeType(root, "READY"), "READY"));
    REQUIRE_FALSE(contains(completeType(root, "READY"), "\"READY\""));
    REQUIRE(contains(completeType(root, "mode == R"), "\"READY\""));
    REQUIRE(contains(completeType(root, "mode == "), "42"));
    REQUIRE(contains(completeType(root, "mode == "), "true"));
    REQUIRE(contains(completeType(root, "mode == "), "null"));

    auto query = std::string("mode == 'a' and true");
    auto candidates = completeType(root, query, {}, query.find("a'") + 1);
    auto escaped = std::ranges::find(candidates, "\"a\\\"b\\\\c\"", &CompletionCandidate::text);
    REQUIRE(escaped != candidates.end());
    REQUIRE(escaped->location.offset == 8);
    REQUIRE(escaped->location.size == 3);
    Environment env(graph->strings());
    REQUIRE(compile(env, query.replace(escaped->location.offset, escaped->location.size, escaped->text), false));

    candidates = completeType(root, "[\"we\"]", {}, 4);
    auto field = std::ranges::find(candidates, "\"weird.field\"", &CompletionCandidate::text);
    REQUIRE(field != candidates.end());
    REQUIRE(field->location.offset == 1);
    REQUIRE(field->location.size == 4);
    REQUIRE(graph->strings()->size() == stringsBefore);
    REQUIRE_FALSE(compile(env, "mode == 'a", false));
}

TEST_CASE("SchemaPool owns one compact graph across native transport and imports", "[model.schema-pool]")
{
    auto pool = std::make_shared<SchemaPool>();
    auto object = pool->newObjectType();
    auto array = pool->newArrayType();
    auto number = pool->newScalarType(SchemaType::Kind::Int);
    auto mask = pool->newBitmaskType();
    auto context = pool->newObjectType();
    object.setTypeName("Measurement");
    object.addField("samples", array);
    object.addField("flags", mask);
    object.addField("context", context);
    object.field("context").setRole(SchemaType::FieldRole::Context);
    object.field("samples").setRequired(true);
    object.addAlias("values", "samples");
    array.addElement(number);
    context.addField("parent", object);
    context.addField("secret", number);
    context.setOpen(true);
    std::vector<SchemaType::NumericRange> intervals{{int64_t{0}, int64_t{254}}};
    number.setNumericDomain({.encoding="uint8", .unit="km/h", .minimum=int64_t{0},
        .maximum=int64_t{255}, .ordinaryRanges=std::span<SchemaType::NumericRange const>(intervals), .complete=true});
    number.addNamedValue({"UNLIMITED", int64_t{255}, SchemaType::NamedValue::Role::Special,
        SchemaType::NamedValue::Meaning::Unbounded});
    mask.asScalar().addNamedValue({"HIGH", uint64_t{1} << 63, SchemaType::NamedValue::Role::Flag});
    mask.setBitWidth(64);
    pool->finalize();
    REQUIRE(pool->typeCount() == 5);
    REQUIRE(pool->frozen());
    REQUIRE(pool->strings()->frozen());
    REQUIRE_THROWS_AS(pool->newScalarType(SchemaType::Kind::Null), std::logic_error);
    std::ostringstream stream;
    REQUIRE(pool->write(stream));
    auto bytes = stream.str();
    auto decoded = SchemaPool::read(std::span(reinterpret_cast<uint8_t const*>(bytes.data()), bytes.size()));
    auto imported = std::make_shared<SchemaPool>();
    REQUIRE(imported->newScalarType(SchemaType::Kind::Bool).id() == 1);
    auto remap = imported->importGraph(*decoded);
    imported->finalize();
    REQUIRE(remap == std::vector<TypeId>{0, 2, 3, 4, 5, 6});
    for (auto const& owner : {SchemaPool::ConstPtr(pool), decoded}) {
        auto root = owner->schemaType(object.id());
        REQUIRE(root.typeName() == "Measurement");
        REQUIRE(root.fieldSchema(owner->strings()->get("values")) == array.id());
        REQUIRE(root.reachabilityComplete());
        REQUIRE_FALSE(root.canHaveField(owner->strings()->get("secret")));
        REQUIRE(root.fieldRole(owner->strings()->get("context")) == SchemaType::FieldRole::Context);
        REQUIRE(root.field(owner->strings()->get("samples")).required() == true);
        REQUIRE(owner->schemaType(array.id()).itemSchema() == number.id());
        REQUIRE(owner->schemaType(mask.id()).bitWidth() == 64);
        REQUIRE(std::get<uint64_t>(owner->schemaType(mask.id()).namedValue("HIGH")->value) == uint64_t{1} << 63);
        REQUIRE(owner->schemaType(number.id()).numericDomain()->unit == "km/h");
        REQUIRE(owner->schemaType(number.id()).numericDomain()->ordinaryRanges.size() == 1);
        REQUIRE(owner->schemaMemoryUsage().allocatedBytes >= owner->schemaMemoryUsage().logicalBytes);
    }
    REQUIRE(imported->schemaType(remap[object.id()]).fieldSchema(imported->strings()->get("samples")) == remap[array.id()]);
    REQUIRE(imported->schemaType(remap[context.id()]).fieldSchema(imported->strings()->get("parent")) == remap[object.id()]);
    auto retained = pool->schemaType(object.id()).retain();
    auto descriptor = pool->typeNode(object.id());
    std::weak_ptr<SchemaPool> weak = pool;
    pool.reset();
    REQUIRE_FALSE(weak.expired());
    REQUIRE(retained.typeName() == "Measurement");
    REQUIRE(descriptor->get(StringPool::SchemaTypeName)->value() == ScalarValueType{std::string_view("Measurement")});
    descriptor = {}; retained = {};
    REQUIRE(weak.expired());
    for (auto length : {size_t{0}, size_t{3}, bytes.size() - 1})
        REQUIRE_THROWS(SchemaPool::read(std::span(reinterpret_cast<uint8_t const*>(bytes.data()), length)));
}

TEST_CASE("SchemaPool preserves null enum literals across transport and imports", "[model.schema-pool]")
{
    std::vector<SchemaType::EnumValue> expected{std::monostate{}};
    SECTION("Null-only enum") {}
    SECTION("Nulls mixed with other primitive values") {
        expected.insert(expected.end(), {false, true, int64_t{0}, int64_t{-17},
            int64_t{1} << 40, 0.5, ByteArray(std::string(1024, 'x')), std::monostate{}});
    }
    auto pool = std::make_shared<SchemaPool>();
    auto id = pool->newScalarType(SchemaType::Kind::Value);
    for (auto const& value : expected)
        id.asEnum().addValue(value);
    id.asEnum().addSymbol("");
    pool->finalize();

    std::ostringstream encoded;
    REQUIRE(pool->write(encoded));
    auto bytes = encoded.str();
    auto decoded = SchemaPool::read(std::span(reinterpret_cast<uint8_t const*>(bytes.data()), bytes.size()));
    for (auto const& current : {SchemaPool::ConstPtr(pool), decoded}) {
        std::vector<SchemaType::EnumValue> actual;
        current->schemaType(id.id()).forEachEnumValue([&](auto const& value) { actual.push_back(value); });
        REQUIRE(actual == expected);
        REQUIRE(std::ranges::equal(current->schemaType(id.id()).directEnumSymbols(), std::vector<StringId>{StringPool::Empty}));
        auto descriptor = current->typeNode(id.id())->toJson();
        REQUIRE(descriptor["enum"].size() == expected.size() + 1);
        REQUIRE(std::ranges::count(descriptor["enum"], nlohmann::json(nullptr)) ==
            std::ranges::count(expected, SchemaType::EnumValue{std::monostate{}}));
    }
    std::ostringstream reencoded;
    REQUIRE(decoded->write(reencoded));
    REQUIRE(reencoded.str() == bytes);

    auto imported = std::make_shared<SchemaPool>();
    imported->newScalarType(SchemaType::Kind::Bool).id();
    auto remap = imported->importGraph(*decoded);
    imported->finalize();
    REQUIRE(remap.at(id.id()) != id.id());
    std::vector<SchemaType::EnumValue> actual;
    imported->schemaType(remap.at(id.id())).forEachEnumValue([&](auto const& value) { actual.push_back(value); });
    REQUIRE(actual == expected);
    REQUIRE(std::ranges::equal(imported->schemaType(remap.at(id.id())).directEnumSymbols(), std::vector<StringId>{StringPool::Empty}));
}

TEST_CASE("SchemaPool preserves raw ordinals separately from named runtime values", "[model.schema-pool]")
{
    auto pool = std::make_shared<SchemaPool>();
    auto id = pool->newScalarType(SchemaType::Kind::String);
    std::vector<SchemaType::NamedValue> values{
        {.name="UNENCODED", .value=std::string_view("ready")},
        {.name="ZERO", .value=std::string_view("ready"), .rawValue=int64_t{0}},
        {.name="UNSIGNED_ZERO", .value=std::string_view("ready"), .rawValue=uint64_t{0}},
        {.name="NEGATIVE", .value=std::string_view("ready"), .rawValue=int64_t{-7}},
        {.name="MIN", .value=std::string_view("ready"), .rawValue=std::numeric_limits<int64_t>::min()},
        {.name="MAX", .value=std::string_view("ready"), .rawValue=std::numeric_limits<int64_t>::max()},
        {.name="UNSIGNED_MAX", .value=std::string_view("ready"), .rawValue=std::numeric_limits<uint64_t>::max()},
        {.name="ALIAS", .value=std::string_view("ready"), .deprecated=true, .removed=true, .rawValue=int64_t{-7}},
    };
    for (auto const& value : values)
        id.addNamedValue(value);
    auto flags = pool->newBitmaskType();
    flags.setBitWidth(64);
    flags.asScalar().addNamedValue({.name="FLAG", .value=uint64_t{1},
        .role=SchemaType::NamedValue::Role::Flag, .rawValue=uint64_t{17}});
    pool->finalize();

    std::ostringstream encoded;
    REQUIRE(pool->write(encoded));
    auto bytes = encoded.str();
    auto decoded = SchemaPool::read(std::span(reinterpret_cast<uint8_t const*>(bytes.data()), bytes.size()));
    auto imported = std::make_shared<SchemaPool>();
    imported->newScalarType(SchemaType::Kind::Bool).id();
    auto remap = imported->importGraph(*decoded);
    imported->finalize();
    for (auto const& current : {SchemaPool::ConstPtr(pool), decoded, SchemaPool::ConstPtr(imported)}) {
        auto typeId = current == imported ? remap.at(id.id()) : id.id();
        auto descriptor = current->typeNode(typeId)->get(StringPool::SchemaNamedValues);
        REQUIRE(descriptor);
        REQUIRE(descriptor->size() == values.size());
        for (size_t i = 0; i < values.size(); ++i) {
            auto const& expected = values[i];
            auto actual = current->schemaType(typeId).namedValue(expected.name);
            REQUIRE(actual);
            REQUIRE(actual->value == expected.value);
            REQUIRE(actual->rawValue == expected.rawValue);
            REQUIRE(actual->deprecated == expected.deprecated);
            REQUIRE(actual->removed == expected.removed);
            auto item = descriptor->at(i);
            REQUIRE(bool(item->get(StringPool::SchemaRawValue)) == bool(expected.rawValue));
            auto json = item->toJson();
            REQUIRE(json["value"] == "ready");
            REQUIRE(json.contains("rawValue") == bool(expected.rawValue));
            if (expected.rawValue)
                std::visit([&](auto raw) {
                    if constexpr (std::is_same_v<decltype(raw), uint64_t>) {
                        if (raw > uint64_t(std::numeric_limits<int64_t>::max())) {
                            REQUIRE(json["rawValue"] == std::to_string(raw));
                            return;
                        }
                    }
                    REQUIRE(json["rawValue"] == raw);
                }, *expected.rawValue);
        }
        auto flagType = current == imported ? remap.at(flags.id()) : flags.id();
        auto flag = current->typeNode(flagType)->toJson()["namedValues"][0];
        REQUIRE(flag["value"] == "0x0000000000000001");
        REQUIRE(flag["rawValue"] == 17);
        auto runtime = Value::make(std::string("ready")).withSchemaType(current->schemaType(typeId));
        auto resolved = runtime.namedValue("NEGATIVE");
        REQUIRE(resolved);
        REQUIRE(resolved->toString() == "ready");
    }
    std::ostringstream reencoded;
    REQUIRE(decoded->write(reencoded));
    REQUIRE(reencoded.str() == bytes);
}

TEST_CASE("SchemaPool rejects old codecs and malformed raw ordinal storage", "[model.schema-pool]")
{
    auto pool = std::make_shared<SchemaPool>();
    auto id = pool->newScalarType(SchemaType::Kind::String);
    id.addNamedValue({.name="READY", .value=std::string_view("ready"), .rawValue=int64_t{7}});
    pool->finalize();
    std::ostringstream encoded;
    REQUIRE(pool->write(encoded));
    auto bytes = encoded.str();
    using Adapter = bitsery::InputBufferAdapter<std::string>;
    bitsery::Deserializer<Adapter> input(Adapter(bytes.begin(), bytes.end()));
    uint32_t version = 0;
    uint64_t typesSize = 0;
    input.value4b(version);
    input.value8b(typesSize);
    REQUIRE(version == 4);
    input.adapter().currentReadPos(input.adapter().currentReadPos() + typesSize);
    // Fixed field rows and four membership arenas precede fixed named rows.
    auto skipColumn = [&] {
        uint64_t size = 0;
        input.value8b(size);
        input.adapter().currentReadPos(input.adapter().currentReadPos() + size);
    };
    for (size_t column = 0; column < 17; ++column) {
        skipColumn();
    }
    uint64_t namedSize = 0;
    input.value8b(namedSize);
    REQUIRE(namedSize == 32);
    auto named = input.adapter().currentReadPos();
    // A named row has a 16-byte scalar, eight raw ordinal bytes, then its tag.
    constexpr size_t rawTypeOffset = 24;
    SECTION("Old generic payloads are not interpreted with the new row layout") {
        bytes[0] = 2;
    }
    SECTION("Unknown future payloads are rejected") {
        bytes[0] = 5;
    }
    SECTION("Unknown ordinal tags are rejected") {
        bytes.at(named + rawTypeOffset) = 3;
    }
    SECTION("Absent ordinals cannot hide nonzero bits") {
        bytes.at(named + rawTypeOffset) = 0;
    }
    SECTION("Truncated ordinal payloads are rejected") {
        bytes.resize(named + rawTypeOffset);
    }
    REQUIRE_THROWS(SchemaPool::read(std::span(reinterpret_cast<uint8_t const*>(bytes.data()), bytes.size())));
}

TEST_CASE("SchemaPool rejects dangling types and inherited cycles before publication", "[model.schema-pool]")
{
    auto pool = std::make_shared<MalformedSchemaPool>();
    auto root = pool->newObjectType();
    SECTION("Dangling edge") { pool->addDanglingField(root); }
    SECTION("Inheritance cycle") { root.setBase(root); }
    REQUIRE_THROWS_AS(pool->finalize(), std::invalid_argument);
    REQUIRE_FALSE(pool->finalized());
    REQUIRE_THROWS_AS(pool->newScalarType(SchemaType::Kind::Int), std::logic_error);
}

TEST_CASE("SchemaPool preserves empty names unknown alternatives and local combiner context", "[model.schema-pool]")
{
    auto pool = std::make_shared<SchemaPool>();
    auto object = pool->newObjectType();
    auto text = pool->newScalarType(SchemaType::Kind::String);
    auto choice = pool->newCompositionType(SchemaType::Composition::AnyOf);
    object.addField("z", text);
    object.addField("a", text);
    object.addField("", text);
    object.addField("partial", text);
    object.addField("partial");
    text.asEnum().addSymbol("Z"); text.asEnum().addSymbol("A"); text.asEnum().addSymbol("");
    choice.setComposition(SchemaType::Composition::AnyOf);
    choice.addAlternative(object);
    choice.asObject().addField("context", object);
    choice.field("context").setRole(SchemaType::FieldRole::Context);
    pool->finalize();
    std::ostringstream encoded;
    REQUIRE(pool->write(encoded));
    auto bytes = encoded.str();
    auto decoded = SchemaPool::read(std::span(reinterpret_cast<uint8_t const*>(bytes.data()), bytes.size()));
    auto imported = std::make_shared<SchemaPool>();
    auto remap = imported->importGraph(*decoded);
    REQUIRE(remap == std::vector<TypeId>{0, 1, 2, 3});
    imported->finalize();
    for (auto const& current : {SchemaPool::ConstPtr(pool), decoded, SchemaPool::ConstPtr(imported)}) {
        REQUIRE(current->schemaType(object.id()).fieldSchema(StringPool::Empty) == text.id());
        REQUIRE(current->schemaType(object.id()).fieldSchema(current->strings()->get("partial")) == NoTypeId);
        REQUIRE(current->schemaType(choice.id()).fieldRole(current->strings()->get("context")) == SchemaType::FieldRole::Context);
        REQUIRE_FALSE(current->schemaType(object.id()).reachabilityComplete());
        auto fieldsInOrder = current->schemaType(object.id()).directFields();
        REQUIRE(std::ranges::equal(fieldsInOrder, std::vector<StringId>{
            current->dictionary().get("z"), current->dictionary().get("a"),
            StringPool::Empty, current->dictionary().get("partial")}));
        auto sortedFields = std::vector<StringId>(fieldsInOrder.begin(), fieldsInOrder.end());
        std::ranges::sort(sortedFields);
        REQUIRE(std::ranges::equal(current->schemaType(object.id()).nestedFields(), sortedFields));
        auto symbolsInOrder = current->schemaType(text.id()).directEnumSymbols();
        REQUIRE(std::ranges::equal(symbolsInOrder, std::vector<StringId>{
            current->dictionary().get("Z"), current->dictionary().get("A"), StringPool::Empty}));
        auto sortedSymbols = std::vector<StringId>(symbolsInOrder.begin(), symbolsInOrder.end());
        std::ranges::sort(sortedSymbols);
        REQUIRE(std::ranges::equal(current->schemaType(text.id()).nestedEnumSymbols(), sortedSymbols));
        auto descriptor = current->typeNode(choice.id());
        REQUIRE(descriptor->get(StringPool::SchemaAlternatives));
        auto fields = descriptor->get(StringPool::SchemaFields);
        REQUIRE(fields);
        auto context = fields->get(current->strings()->get("context"));
        REQUIRE(context);
        REQUIRE(context->get(StringPool::SchemaRole)->value() == ScalarValueType{std::string_view("context")});
        REQUIRE(current->schemaStatistics().contextFields == 1);
        REQUIRE(current->schemaStatistics().types == 3);
    }
}

TEST_CASE("SchemaPool numeric replacements remain canonical and flags respect their encoding", "[model.schema-pool]")
{
    auto pool = std::make_shared<SchemaPool>();
    auto first = pool->newScalarType(SchemaType::Kind::Int);
    auto second = pool->newScalarType(SchemaType::Kind::Int);
    std::vector<SchemaType::NumericRange> oldRanges{{int64_t{0}, int64_t{100}}};
    std::vector<SchemaType::NumericRange> ranges{{int64_t{1}, int64_t{5}}, {int64_t{10}, int64_t{15}}};
    // Setter order does not dictate wire ownership, and overwritten ranges are discarded.
    second.setNumericDomain({.unit="old", .ordinaryRanges=std::span<SchemaType::NumericRange const>(oldRanges)});
    first.setNumericDomain({.unit="count", .ordinaryRanges=std::span<SchemaType::NumericRange const>(ranges)});
    second.setNumericDomain({.unit="m", .ordinaryRanges=std::span<SchemaType::NumericRange const>(ranges)});
    pool->finalize();
    REQUIRE(pool->schemaStatistics().numericDomains == 2);
    std::ostringstream encoded;
    REQUIRE(pool->write(encoded));
    auto bytes = encoded.str();
    auto restored = SchemaPool::read(std::span(reinterpret_cast<uint8_t const*>(bytes.data()), bytes.size()));
    REQUIRE(restored->schemaType(second.id()).numericDomain()->unit == "m");
    REQUIRE(restored->schemaType(second.id()).numericDomain()->ordinaryRanges.size() == 2);
    std::ostringstream roundtrip;
    REQUIRE(restored->write(roundtrip));
    REQUIRE(roundtrip.str() == bytes);

    SchemaType::NamedValue flag{"FLAG", uint64_t{1}, SchemaType::NamedValue::Role::Flag};
    SECTION("String mask") { flag.value = std::string_view("not-a-mask"); }
    SECTION("Boolean mask") { flag.value = true; }
    SECTION("Negative mask") { flag.value = int64_t{-1}; }
    SECTION("Floating mask") { flag.value = 1.; }
    SECTION("Out-of-width mask") { flag.value = uint64_t{256}; }
    auto invalid = std::make_shared<SchemaPool>();
    auto mask = invalid->newBitmaskType();
    mask.setBitWidth(8);
    mask.asScalar().addNamedValue(flag);
    REQUIRE_THROWS_AS(invalid->finalize(), std::invalid_argument);
}

TEST_CASE("SchemaPool inherited shadows preserve declaration order and member metadata", "[model.schema-pool]")
{
    auto pool = std::make_shared<SchemaPool>();
    auto base = pool->newObjectType();
    auto derived = pool->newObjectType();
    auto integer = pool->newScalarType(SchemaType::Kind::Int);
    auto text = pool->newScalarType(SchemaType::Kind::String);
    derived.setBase(base);
    for (size_t i = 0; i < 2048; ++i)
        base.addField("field" + std::to_string(i), integer);
    for (size_t i = 2048; i-- > 0;)
        derived.addField("field" + std::to_string(i), text);
    derived.field("field0").setRole(SchemaType::FieldRole::Context);
    pool->finalize();
    auto view = pool->schemaType(derived.id());
    REQUIRE(view.directFields().size() == 2048);
    REQUIRE(*pool->dictionary().resolve(view.directFields().front()) == "field2047");
    REQUIRE(*pool->dictionary().resolve(view.directFields().back()) == "field0");
    REQUIRE(view.fieldSchema(pool->strings()->get("field0")) == text.id());
    size_t enumerable = 0;
    view.forEachField([&](SchemaField const& field) {
        if (field.role() == SchemaType::FieldRole::Context)
            return;
        auto name = field.nameId();
        auto children = field.types();
        REQUIRE(name != pool->strings()->get("field0"));
        REQUIRE(children.size() == 1);
        REQUIRE(children.front() == text.id());
        ++enumerable;
    });
    REQUIRE(enumerable == 2047);
}

TEST_CASE("Context edges support explicit access without implicit discovery", "[model.schema-type][schema.context]")
{
    auto graph = std::make_shared<SchemaPool>();
    for (auto name : {"$payload", "context", "$feature", "secret", "hiddenSymbol", "CONTEXT_ONLY", "fields", "role", "$ref"})
        REQUIRE(graph->strings()->emplace(name));
    auto root = graph->newObjectType();
    auto integer = graph->newScalarType(SchemaType::Kind::Int);
    auto context = graph->newObjectType();
    auto enumeration = graph->newScalarType(SchemaType::Kind::String);
    root.addField("$payload", integer);
    root.addField("context", context).setRequired(true);
    root.field("context").setRole(SchemaType::FieldRole::Context);
    root.addField("$feature", context).setRequired(true);
    root.field("$feature").setRole(SchemaType::FieldRole::Context);
    context.addField("secret", integer);
    context.addField("hiddenSymbol", enumeration);
    enumeration.asEnum().addSymbol("CONTEXT_ONLY");
    // Unknown metadata behind a context link cannot make the data index incomplete.
    context.setOpen(true);
    graph->finalize();
    REQUIRE(root.reachabilityComplete());
    REQUIRE_FALSE(root.canHaveField(graph->dictionary().get("secret")));
    REQUIRE(root.canHaveField(graph->dictionary().get("context")));
    REQUIRE(root.fieldSchema(graph->dictionary().get("context")) == 3);
    REQUIRE(root.fieldRole(graph->dictionary().get("context")) == SchemaType::FieldRole::Context);
    REQUIRE(root.fieldRole(graph->dictionary().get("$payload")) == SchemaType::FieldRole::Data);
    bool complete = false;
    REQUIRE(root.fieldPaths(graph->dictionary().get("secret"), &complete).empty());
    REQUIRE(complete);
    REQUIRE(root.fieldPaths(graph->dictionary().get("context")).size() == 1);
    REQUIRE(root.enumSymbolPaths(graph->dictionary().get("CONTEXT_ONLY"), &complete).empty());
    REQUIRE(complete);
    std::vector<StringId> enumerable;
    root.forEachField([&](SchemaField field) {
        if (field.role() == SchemaType::FieldRole::Data)
            enumerable.push_back(field.nameId());
    });
    REQUIRE(enumerable == std::vector<StringId>{graph->dictionary().get("$payload")});
    REQUIRE_FALSE(contains(completeType(root, "sec"), "secret"));
    REQUIRE_FALSE(contains(completeType(root, "CONTEXT_"), "CONTEXT_ONLY"));
    REQUIRE(contains(completeType(root, "context.sec"), "secret"));
    REQUIRE(contains(completeType(root, "$feature.sec"), "secret"));
    REQUIRE_FALSE(contains(completeType(root, "*.sec"), "secret"));

    auto descriptors = root.modelNode();
    auto edge = descriptors->get(graph->dictionary().get("fields"))->get(graph->dictionary().get("context"));
    REQUIRE(edge->get(graph->dictionary().get("role"))->value() == ScalarValueType{std::string_view("context")});
    REQUIRE(edge->get(graph->dictionary().get("$ref"))->value() == ScalarValueType{int64_t{3}});

    auto model = std::make_shared<ModelPool>(graph->strings());
    model->setSchemaPool(graph);
    auto payload = model->newObject();
    REQUIRE(payload->setSchema(1));
    REQUIRE(payload->addField("$payload", int64_t{7}));
    auto hidden = model->newObject();
    REQUIRE(hidden->setSchema(3));
    REQUIRE(hidden->addField("secret", int64_t{42}));
    auto overlay = model_ptr<OverlayNode>::make(Value::field(payload));
    overlay->set(graph->dictionary().get("context"), Value::field(hidden));
    overlay->set(graph->dictionary().get("$feature"), Value::field(hidden));
    for (bool planned : {false, true}) {
        Environment env(graph->strings());
        env.enableWildcardFieldPlans = planned;
        env.setSchema(root);
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

TEST_CASE("Wildcard array plans retain named lookup without traversing context", "[model.schema-pool][schema.context][schema.array-fields]")
{
    auto graph = std::make_shared<SchemaPool>();
    auto array = graph->newArrayType();
    auto named = graph->newArrayType();
    auto number = graph->newScalarType(SchemaType::Kind::Int);
    auto hidden = graph->newObjectType();
    array.addElement(named);
    named.addElement(number);
    named.addField("first", number);
    named.addField("context", hidden);
    named.field("first").setRole(SchemaType::FieldRole::Context);
    named.field("context").setRole(SchemaType::FieldRole::Context);
    named.addAlias("head", "first");
    hidden.addField("secret", number);
    hidden.setOpen(true);
    graph->finalize();
    REQUIRE(graph->schemaType(array.id()).reachabilityComplete());
    REQUIRE_FALSE(graph->schemaType(array.id()).canHaveField(graph->strings()->get("secret")));

    for (bool separateDictionary : {false, true}) {
        auto strings = separateDictionary ? std::make_shared<StringPool>() : graph->strings();
        if (separateDictionary) {
            REQUIRE(strings->emplace("padding"));
            for (auto name : {"secret", "head", "context", "first"})
                REQUIRE(strings->emplace(name));
            REQUIRE(strings->get("first") != graph->strings()->get("first"));
        }
        for (auto mode : {"untyped", "pruning", "plans"}) {
            CAPTURE(separateDictionary, mode);
            auto model = std::make_shared<NamedArrayPool>(strings);
            if (std::string_view(mode) != "untyped")
                model->setSchemaPool(graph);
            auto context = model->newObject();
            REQUIRE(context->addField("secret", int64_t{42}));
            REQUIRE(context->setSchema(hidden.id()));
            model->context = context->addr();
            auto elements = model->newArray();
            elements->append(int64_t{7});
            REQUIRE(elements->setSchema(named.id()));
            auto root = model->newArray();
            root->append(ModelNode::Ptr::make(model,
                ModelNodeAddress{ModelPool::FirstCustomColumnId, elements->addr().index()}));
            REQUIRE(root->setSchema(array.id()));

            Environment env(strings);
            env.enableWildcardFieldPlans = std::string_view(mode) == "plans";
            for (auto [query, expected] : {
                std::pair{"[0][0]", int64_t{7}},
                {"[0].first", 7}, {"**.first", 7}, {"**.head", 7},
                {"*.first", 7}, {"[0].context.secret", 42},
                {"**.context.secret", 42}, {"count(**.secret)", 0},
                {"count(**.first)", 1}, {"count(**.context)", 1}}) {
                CAPTURE(query);
                auto expression = compile(env, query, false);
                REQUIRE(expression);
                // Reuse the binding so the second evaluation exercises the cached plan.
                BoundExpression bound(SharedAST(std::move(*expression)), env);
                for (int repeat = 0; repeat < 2; ++repeat) {
                    auto values = bound.eval(*root);
                    REQUIRE(values);
                    REQUIRE(values->size() == 1);
                    CHECK(values->front().as<ValueType::Int>() == expected);
                }
            }
        }
    }
}

TEST_CASE("Context classification is conservative across repeated and alternative edges", "[model.schema-type][schema.context]")
{
    auto graph = std::make_shared<SchemaPool>();
    auto context = graph->newObjectType();
    auto data = graph->newObjectType();
    auto integer = graph->newScalarType(SchemaType::Kind::Int);
    context.addField("parent", integer).setRole(SchemaType::FieldRole::Context);
    data.addField("parent", integer);
    auto allContext = graph->newCompositionType(SchemaType::Composition::AnyOf);
    allContext.addAlternative(context);
    auto mixed = graph->newCompositionType(SchemaType::Composition::AnyOf);
    mixed.addAlternative(context);
    mixed.addAlternative(data);
    auto unknown = graph->newCompositionType(SchemaType::Composition::AnyOf);
    unknown.addAlternative(context);
    unknown.addAlternative({});
    // Repeated targets belong to one occurrence; its role is explicit edge metadata.
    auto repeated = data.addField("parent", integer);
    repeated.setMultimap(true);
    graph->finalize();
    auto field = graph->dictionary().get("parent");
    REQUIRE(allContext.fieldRole(field) == SchemaType::FieldRole::Context);
    REQUIRE(mixed.fieldRole(field) == SchemaType::FieldRole::Data);
    REQUIRE(unknown.fieldRole(field) == SchemaType::FieldRole::Data);
    REQUIRE(data.fieldRole(field) == SchemaType::FieldRole::Data);
    REQUIRE(repeated.multimap());
    REQUIRE(repeated.types().size() == 2);
    REQUIRE_THROWS(context.field("parent").setRole(SchemaType::FieldRole::Data));
    size_t fields = 0;
    data.forEachField([&](SchemaField const& edge) {
        REQUIRE(edge.nameId() == field);
        REQUIRE(edge.role() == SchemaType::FieldRole::Data);
        ++fields;
    });
    REQUIRE(fields == 1);
}

TEST_CASE("Static schema names agree across isolated pools", "[model.schema-type]")
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
    REQUIRE(left->emplace("dynamic-type-name").value() >= StringPool::FirstDynamicId);
}

TEST_CASE("Canonical schemas participate in sparse plans and retain incomplete metadata", "[model.schema-type]")
{
    auto graph = std::make_shared<SchemaPool>();
    auto rootType = graph->newObjectType();
    auto integer = graph->newScalarType(SchemaType::Kind::Int);
    auto branchType = graph->newObjectType();
    auto parent = graph->newObjectType();
    auto untypedItems = graph->newArrayType();
    branchType.addField("target", integer);
    rootType.addField("branch", branchType);
    parent.addField("wrapped", rootType);
    parent.addField("untypedItems", untypedItems);
    auto model = std::make_shared<ModelPool>();
    auto root = model->newObject();
    REQUIRE(root->setSchema(rootType.id()));
    for (int i = 0; i < 32; ++i) {
        auto name = "noise" + std::to_string(i);
        rootType.addField(name, integer);
        REQUIRE(root->addField(name, int64_t(i)));
    }
    auto branch = model->newObject();
    REQUIRE(branch->setSchema(branchType.id()));
    REQUIRE(branch->addField("target", int64_t{42}));
    REQUIRE(root->addField("branch", branch));
    auto ready = graph->strings()->emplace("READY").value();
    graph->finalize();
    Environment env(model->strings());
    env.setSchema(rootType);
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

    REQUIRE(contains(completeType(rootType, "tar"), "target"));
    REQUIRE_FALSE(parent.reachabilityComplete());
    REQUIRE(parent.canHaveField(graph->dictionary().get("target")));
    REQUIRE(contains(completeType(parent, "tar"), "target"));
    bool exhaustive = true;
    (void)parent.enumSymbolPaths(ready, &exhaustive);
    REQUIRE_FALSE(exhaustive);
}

TEST_CASE("Lookup aliases resolve direct and recursive schema paths canonically", "[model.schema-type]")
{
    auto graph = std::make_shared<SchemaPool>();
    auto root = graph->newObjectType();
    auto child = graph->newObjectType();
    auto integer = graph->newScalarType(SchemaType::Kind::Int);
    auto wrapper = graph->newObjectType();
    root.addAlias("publicName", "storedName");
    root.addField("storedName", child);
    child.addField("value", integer);
    wrapper.addField("wrapper", root);
    auto shadow = graph->newObjectType();
    shadow.setBase(root);
    shadow.addField("publicName", integer);
    graph->finalize();
    auto alias = graph->dictionary().get("publicName");
    auto target = graph->dictionary().get("storedName");
    REQUIRE(std::ranges::equal(root.fieldTypes(alias), std::vector<TypeId>{child.id()}));
    REQUIRE(root.fieldPaths(alias).front().front().field == target);
    REQUIRE(std::ranges::none_of(root.directFields(), [alias](auto field) { return field == alias; }));
    REQUIRE(contains(completeType(root, "publicName.va"), "value"));
    Environment env(graph->strings());
    env.setSchema(root);
    for (auto query : {"publicName.value", "**.publicName.value", "**.publicName.value == 'x'"}) {
        auto ast = compile(env, query, false);
        REQUIRE(ast);
        auto refs = referencedSchemaPaths(env, **ast, root);
        REQUIRE(refs);
        REQUIRE_FALSE(refs->hasUnresolvedAccess);
        REQUIRE(refs->paths.size() == 1);
        REQUIRE(refs->paths.front().path.front().field == target);
    }
    auto ast = compile(env, "**.publicName.value", false);
    REQUIRE(ast);
    auto refs = referencedSchemaPaths(env, **ast, wrapper);
    REQUIRE(refs);
    REQUIRE_FALSE(refs->hasUnresolvedAccess);
    REQUIRE(refs->paths.front().path[1].field == target);
    REQUIRE(std::ranges::equal(shadow.fieldTypes(alias), std::vector<TypeId>{integer.id()}));
    REQUIRE_THROWS(root.addField("publicName", integer));
}

TEST_CASE("Type completion preserves alias scope and never executes custom functions", "[completion.type]")
{
    auto graph = std::make_shared<AliasPool>();
    for (auto name : {"alias", "child", "mode", "urban", "unrelated"})
        REQUIRE(graph->strings()->emplace(name));
    auto root = graph->newObjectType();
    auto child = graph->newObjectType();
    auto mode = graph->newScalarType(SchemaType::Kind::String);
    auto other = graph->newScalarType(SchemaType::Kind::String);
    root.addField("child", child);
    root.addField("alias", other);
    child.addField("mode", mode);
    mode.asEnum().addSymbol("urban");
    other.asEnum().addSymbol("unrelated");
    graph->finalize();
    REQUIRE(contains(completeType(root, "alias == u"), "\"urban\""));
    REQUIRE_FALSE(contains(completeType(root, "alias == u"), "\"unrelated\""));
    REQUIRE_FALSE(contains(completeType(root, "_.alias == u"), "\"urban\""));
    REQUIRE(contains(completeType(root, "_.alias == u"), "\"unrelated\""));

    Environment env(graph->strings());
    env.setSchema(root);
    InvocationProbe probe;
    env.functions["probe"] = &probe;
    for (auto query : {"probe() and child.mo", "probe(child.mo)", "probe().mo"}) {
        auto cursor = std::string_view(query).find("mo") + 2;
        auto result = complete(env, query, cursor, root);
        REQUIRE(result);
        if (std::string_view(query) != "probe().mo")
            REQUIRE(contains(*result, "mode"));
        REQUIRE(probe.calls == 0);
    }
}

TEST_CASE("String enum metadata has one path through descriptors completion and rewrites", "[model.schema-type]")
{
    auto graph = std::make_shared<SchemaPool>();
    for (auto name : {"mode", "READY"})
        REQUIRE(graph->strings()->emplace(name));
    auto root = graph->newObjectType();
    auto value = graph->newScalarType(SchemaType::Kind::String);
    root.addField("mode", value);
    STATIC_REQUIRE_FALSE(std::is_constructible_v<SchemaType::EnumValue, std::string>);
    STATIC_REQUIRE_FALSE(std::is_constructible_v<SchemaType::EnumValue, std::string_view>);
    value.asEnum().addSymbol("READY");
    graph->finalize();
    auto descriptor = root.modelNode();
    REQUIRE(descriptor->toJson()["fields"]["mode"]["enum"][0] == "READY");
    REQUIRE(contains(completeType(root, "REA"), "\"READY\""));
    Environment env(graph->strings());
    env.setSchema(root);
    auto ast = compile(env, "READY", CompileOptions{false, RewriteMode::Schema, root});
    REQUIRE(ast);
    REQUIRE((*ast)->expr().toString().find("mode") != std::string::npos);
}

TEST_CASE("Shared schema branches and repeated field names keep distinct enum paths", "[model.schema-type]")
{
    auto graph = std::make_shared<SchemaPool>();
    for (auto name : {"left", "right", "READY"})
        REQUIRE(graph->strings()->emplace(name));
    auto root = graph->newObjectType();
    auto branch = graph->newObjectType();
    auto enumeration = graph->newScalarType(SchemaType::Kind::String);
    root.addField("left", branch);
    root.addField("right", branch);
    branch.addField("left", enumeration);
    enumeration.asEnum().addSymbol("READY");
    graph->finalize();
    bool exhaustive = false;
    auto paths = root.enumSymbolPaths(graph->dictionary().get("READY"), &exhaustive);
    REQUIRE(exhaustive);
    REQUIRE(paths.size() == 2);
    REQUIRE(paths[0].size() == 2);
    REQUIRE(paths[1].size() == 2);
}

TEST_CASE("Field paths prune unrelated shared graphs without dropping distinct owners", "[model.schema-type][schema.field-paths]")
{
    auto graph = std::make_shared<SchemaPool>();
    auto root = graph->newObjectType();
    auto matched = graph->newObjectType();
    auto integer = graph->newScalarType(SchemaType::Kind::Int);
    std::vector<SchemaObjectType> branches;
    for (size_t i = 0; i < 15; ++i)
        branches.push_back(graph->newObjectType());
    root.addField("conditions", branches.front());
    root.addField("left", matched);
    root.addField("right", matched);
    matched.addField("target", integer);
    // Over 30,000 irrelevant paths would exhaust the request budget without pruning.
    for (size_t i = 0; i + 1 < branches.size(); ++i) {
        branches[i].addField("left", branches[i + 1]);
        branches[i].addField("right", branches[i + 1]);
    }
    SECTION("Acyclic shared graph") {}
    SECTION("Unrelated recursive graph") { branches.back().addField("left", branches.back()); }
    graph->finalize();
    auto left = graph->dictionary().get("left");
    auto right = graph->dictionary().get("right");
    auto target = graph->dictionary().get("target");
    bool exhaustive = false;
    auto paths = root.fieldPaths(target, &exhaustive);
    REQUIRE(exhaustive);
    REQUIRE(paths == std::vector<SchemaPath>{
        {{SchemaPathSegment::Kind::Field, left}, {SchemaPathSegment::Kind::Field, target}},
        {{SchemaPathSegment::Kind::Field, right}, {SchemaPathSegment::Kind::Field, target}}});

    Environment env(graph->strings());
    auto ast = compile(env, "**.target", false);
    REQUIRE(ast);
    auto refs = referencedSchemaPaths(env, **ast, root);
    REQUIRE(refs);
    REQUIRE_FALSE(refs->hasUnresolvedAccess);
    REQUIRE(refs->paths.size() == 2);
}

TEST_CASE("Field path analysis reports partial recursive and bounded results", "[model.schema-type][schema.field-paths]")
{
    auto graph = std::make_shared<SchemaPool>();
    auto root = graph->newObjectType();
    auto group = graph->newObjectType();
    auto text = graph->newScalarType(SchemaType::Kind::String);
    root.addField("group", group);
    group.addField("target", text);
    SECTION("A recursive branch can contain further matches") {
        root.addField("next", root);
    }
    SECTION("A depth-limited branch can contain further matches") {
        auto previous = root;
        for (size_t i = 0; i < 131; ++i) {
            auto next = graph->newObjectType();
            previous.addField("next", next);
            previous = next;
        }
        previous.addField("group", group);
    }
    SECTION("Open metadata cannot exclude further matches") { root.setOpen(true); }
    graph->finalize();

    bool exhaustive = true;
    auto paths = root.fieldPaths(graph->dictionary().get("target"), &exhaustive);
    REQUIRE_FALSE(exhaustive);
    REQUIRE(paths.size() == 1);
    Environment env(graph->strings());
    for (auto query : {"**.target", "**.group.target", "**.group.target == 'x'"}) {
        CAPTURE(query);
        auto ast = compile(env, query, false);
        REQUIRE(ast);
        auto refs = referencedSchemaPaths(env, **ast, root);
        REQUIRE(refs);
        REQUIRE(refs->paths.size() == 1);
        REQUIRE(refs->hasUnresolvedAccess);
    }
}

TEST_CASE("Field path pruning preserves canonical aliases and real-member shadows", "[model.schema-type][schema.field-paths]")
{
    auto graph = std::make_shared<SchemaPool>();
    auto root = graph->newObjectType();
    auto aliased = graph->newObjectType();
    auto integer = graph->newScalarType(SchemaType::Kind::Int);
    root.addField("branch", aliased);
    aliased.addField("target", integer);
    aliased.addAlias("alias", "target");
    std::string_view expected = "target";
    SECTION("Lookup-only alias") {}
    SECTION("A real member wins over the alias") {
        aliased.addField("alias", integer);
        expected = "alias";
    }
    graph->finalize();
    bool exhaustive = false;
    auto paths = root.fieldPaths(graph->dictionary().get("alias"), &exhaustive);
    REQUIRE(exhaustive);
    REQUIRE(paths.size() == 1);
    REQUIRE(paths.front().back().field == graph->dictionary().get(expected));
}

TEST_CASE("Field paths still bound shared graphs whose branches all match", "[model.schema-type][schema.field-paths]")
{
    auto graph = std::make_shared<SchemaPool>();
    std::vector<SchemaObjectType> branches;
    for (size_t i = 0; i < 15; ++i)
        branches.push_back(graph->newObjectType());
    for (size_t i = 0; i + 1 < branches.size(); ++i) {
        branches[i].addField("left", branches[i + 1]);
        branches[i].addField("right", branches[i + 1]);
    }
    branches.back().addField("target", graph->newScalarType(SchemaType::Kind::Int));
    graph->finalize();
    bool exhaustive = true;
    auto paths = branches.front().fieldPaths(graph->dictionary().get("target"), &exhaustive);
    REQUIRE_FALSE(exhaustive);
    REQUIRE_FALSE(paths.empty());
    REQUIRE(paths.size() < 10000);
}

TEST_CASE("Enum matches beyond the path enumeration depth limit remain queryable", "[model.schema-type]")
{
    auto graph = std::make_shared<SchemaPool>();
    auto rootType = graph->newObjectType();
    auto previous = rootType;
    for (size_t i = 1; i < 132; ++i) {
        auto next = graph->newObjectType();
        previous.addField("next", next);
        previous = next;
    }
    auto enumeration = graph->newEnumType();
    enumeration.addSymbol("READY");
    previous.addField("next", enumeration);
    graph->finalize();
    bool exhaustive = true;
    REQUIRE(rootType.enumSymbolPaths(graph->dictionary().get("READY"), &exhaustive).empty());
    REQUIRE_FALSE(exhaustive);
    auto model = std::make_shared<ModelPool>();
    auto root = model->newObject();
    auto current = root;
    for (size_t i = 1; i < 132; ++i) {
        auto child = model->newObject();
        current->addField("next", child);
        current = child;
    }
    current->addField("next", "READY");
    Environment env(model->strings());
    env.setSchema(rootType);
    auto ast = compile(env, "READY", CompileOptions{false, RewriteMode::Schema, rootType});
    REQUIRE(ast);
    auto result = eval(env, **ast, *root, nullptr);
    REQUIRE(result);
    REQUIRE(result->front().isBool(true));
}

TEST_CASE("Type completion performance", "[perf.completion]")
{
    if (RUNNING_ON_VALGRIND)
        SKIP("Skipping benchmarks when running under valgrind");
    auto graph = std::make_shared<SchemaPool>();
    for (auto name : {"items"})
        REQUIRE(graph->strings()->emplace(name));
    auto root = graph->newObjectType();
    auto items = graph->newArrayType();
    auto item = graph->newObjectType();
    auto alternate = graph->newCompositionType(SchemaType::Composition::AnyOf);
    auto integer = graph->newScalarType(SchemaType::Kind::Int);
    root.addField("items", items);
    items.addElement(item);
    items.addElement(alternate);
    alternate.addAlternative(item);
    alternate.addAlternative(integer);
    for (auto i = 0; i < 256; ++i)
        item.addField("field" + std::to_string(i), integer);
    graph->finalize();
    auto model = std::make_shared<ModelPool>(graph->strings());
    model->setSchemaPool(graph);
    auto node = model->newObject();
    node->setSchema(1);
    Environment env(graph->strings());
    env.setSchema(root);
    CompletionOptions options;
    options.showWildcardHints = false;
    options.limit = 1000;
    auto schemaResult = complete(env, "fi", 2, root, options);
    auto modelResult = complete(env, "fi", 2, *node, options);
    REQUIRE(schemaResult);
    REQUIRE(modelResult);
    REQUIRE(schemaResult->size() == 256);
    REQUIRE(schemaResult->size() == modelResult->size());
    auto nested = complete(env, "items[17].fi", 12, root, options);
    REQUIRE(nested);
    REQUIRE(nested->size() == 256);
    BENCHMARK("schema model root completion") { return complete(env, "fi", 2, *node, options); };
    BENCHMARK("direct type root completion") { return complete(env, "fi", 2, root, options); };
    BENCHMARK("direct nested array union completion") { return complete(env, "items[17].fi", 12, root, options); };
}
