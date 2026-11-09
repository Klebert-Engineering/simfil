#include <catch2/catch_test_macros.hpp>
#include "simfil/model/schema-node.h"
#include "simfil/model/schema-pool.h"
#include "simfil/simfil.h"
#include "simfil/result.h"

#include <atomic>
#include <sstream>

using namespace simfil;

namespace
{
/** Exercise extension metadata without constructing a parallel descriptor owner. */
class DescriptorSchemaPool : public SchemaPool
{
public:
    using SchemaPool::finalizeSchema;
    using SchemaPool::setIdentity;
    StringId extra = strings()->emplace("extra").value();
    mutable size_t reads = 0;
    bool collision = false;

protected:
    auto descriptorExtensionSize(TypeId) const -> uint32_t override { return 1; }
    auto descriptorExtensionKeyAt(TypeId, uint32_t) const -> StringId override
    {
        return collision ? StringPool::SchemaKind : extra;
    }
    auto descriptorExtensionGet(TypeId, StringId name) const -> ModelNode::Ptr override
    {
        ++reads;
        return name == extra ? Model::resolve(ModelNodeAddress(Model::Scalar), std::string_view("extension"))
                             : ModelNode::Ptr{};
    }
};
}

TEST_CASE("Schema descriptors retain their canonical pool and navigate cycles without state", "[model.schema-node]")
{
    auto pool = std::make_shared<SchemaPool>();
    auto root = pool->newObjectType();
    root.setTypeName("Recursive");
    root.addField("next", root);
    auto leaf = pool->newScalarType(SchemaType::Kind::Int);
    root.addField("count", leaf).setRequired(false);
    REQUIRE_THROWS(root.modelNode());
    pool->finalize();
    auto size = pool->schemaMemoryUsage();
    auto descriptor = root.modelNode();
    REQUIRE(descriptor->owningModel().get() == pool.get());
    REQUIRE(descriptor->type() == ValueType::Object);
    REQUIRE(descriptor->kind() == SchemaType::Kind::Object);
    REQUIRE(descriptor->schema() == NoTypeId);
    REQUIRE_FALSE(descriptor->schemaType());
    REQUIRE_FALSE(Value::field(*descriptor).schemaType());
    REQUIRE(descriptor->describedType() == root);
    REQUIRE(leaf.modelNode()->type() == ValueType::Object);
    REQUIRE(leaf.modelNode()->kind() == SchemaType::Kind::Int);

    auto next = pool->dictionary().get("next");
    ModelNode::Ptr cursor = descriptor;
    auto first = descriptor->get(StringPool::SchemaFields)->get(next)->addr();
    for (size_t i = 0; i < 1000; ++i)
    {
        cursor = cursor->get(StringPool::SchemaFields)->get(next);
        REQUIRE(cursor->addr() == first);
        REQUIRE(cursor->get(StringPool::SchemaRef)->value() == ScalarValueType{int64_t(root.id())});
        REQUIRE_FALSE(cursor->get(StringPool::SchemaTruncated));
    }
    REQUIRE(pool->schemaMemoryUsage().logicalBytes == size.logicalBytes);
    REQUIRE(pool->schemaMemoryUsage().allocatedBytes == size.allocatedBytes);
    auto text = cursor->get(StringPool::SchemaTypeName);
    std::weak_ptr<SchemaPool> weak = pool;
    cursor = {};
    descriptor = {};
    pool.reset();
    REQUIRE_FALSE(weak.expired());
    REQUIRE(text->value() == ScalarValueType{std::string_view("Recursive")});
    text = {};
    REQUIRE(weak.expired());
}

TEST_CASE("Field descriptors preserve inherited occurrences and named array terminals", "[model.schema-node]")
{
    auto pool = std::make_shared<SchemaPool>();
    auto text = pool->newEnumType();
    text.addSymbol("READY");
    auto base = pool->newObjectType();
    base.addField("", text).setMultimap(true);
    base.addField("status", text).setRequired(true);
    auto derived = pool->newObjectType();
    derived.setBase(base);
    auto array = pool->newArrayType();
    array.addElement(derived);
    array.addField("context", base).setRole(SchemaType::FieldRole::Context);
    derived.addField("items", array);
    pool->finalize();
    auto baseFields = base.modelNode()->get(StringPool::SchemaFields);
    auto derivedFields = derived.modelNode()->get(StringPool::SchemaFields);
    REQUIRE(derivedFields->size() == 3);
    REQUIRE(baseFields->at(0)->addr() == derivedFields->at(0)->addr());
    REQUIRE(derivedFields->get(StringPool::Empty)->get(StringPool::SchemaMultimap)->value() == ScalarValueType{true});
    REQUIRE(derivedFields->get(pool->dictionary().get("status"))->get(StringPool::SchemaRequired)->value() == ScalarValueType{true});
    auto terminals = array.modelNode()->get(StringPool::SchemaFields);
    REQUIRE(terminals);
    REQUIRE(terminals->size() == 1);
    REQUIRE(terminals->at(0)->get(StringPool::SchemaRole)->value() == ScalarValueType{std::string_view("context")});
    REQUIRE_FALSE(derivedFields->at(-1));
    REQUIRE_FALSE(derivedFields->at(3));
    REQUIRE(derivedFields->keyAt(-1) == StringPool::Empty);
}

TEST_CASE("Descriptor record addresses are not limited to eight-bit field positions", "[model.schema-node]")
{
    auto pool = std::make_shared<SchemaPool>();
    auto root = pool->newObjectType();
    for (size_t i = 0; i < 1025; ++i)
        root.addField("member" + std::to_string(i));
    pool->finalize();
    auto fields = root.modelNode()->get(StringPool::SchemaFields);
    REQUIRE(fields->size() == 1025);
    REQUIRE(fields->at(0)->addr() != fields->at(1024)->addr());
    REQUIRE(fields->get(pool->dictionary().get("member1024"))->addr() == fields->at(1024)->addr());
    std::ostringstream output;
    REQUIRE(pool->write(output));
    auto bytes = output.str();
    auto decoded = SchemaPool::read(std::span(reinterpret_cast<uint8_t const*>(bytes.data()), bytes.size()));
    auto restored = decoded->typeNode(root.id())->get(StringPool::SchemaFields);
    REQUIRE(restored->at(1024)->addr() == fields->at(1024)->addr());
}

TEST_CASE("Descriptor extensions resolve lazily through the same owner", "[model.schema-node]")
{
    auto pool = std::make_shared<DescriptorSchemaPool>();
    auto root = pool->newScalarType(SchemaType::Kind::String);
    pool->finalize();
    auto node = root.modelNode();
    auto count = node->size();
    REQUIRE(node->keyAt(count - 1) == pool->extra);
    REQUIRE(pool->reads == 0);
    auto value = node->get(pool->extra);
    REQUIRE(value->owningModel().get() == pool.get());
    REQUIRE(value->value() == ScalarValueType{std::string_view("extension")});
    REQUIRE(pool->reads == 1);
    pool->collision = true;
    REQUIRE_THROWS_AS(node->keyAt(count - 1), std::logic_error);
}

TEST_CASE("A schema owns one dictionary namespace and publishes identity once", "[model.schema-node]")
{
    REQUIRE_THROWS_AS(SchemaPool(nullptr), std::invalid_argument);
    auto pool = std::make_shared<DescriptorSchemaPool>();
    pool->newObjectType();
    REQUIRE_FALSE(pool->setStrings(std::make_shared<StringPool>()));
    REQUIRE(pool->setStrings(pool->strings()));
    REQUIRE(&pool->dictionary() == pool->strings().get());
    REQUIRE_THROWS_AS(pool->setIdentity("early"), std::logic_error);
    pool->finalizeSchema();
    REQUIRE_THROWS_AS(pool->setIdentity(""), std::logic_error);
    pool->setIdentity("artifact");
    REQUIRE(pool->identity() == "artifact");
    REQUIRE_THROWS_AS(pool->setIdentity("second"), std::logic_error);
    pool->finalize();
    REQUIRE_THROWS_AS(pool->setIdentity("late"), std::logic_error);
}

TEST_CASE("Cyclic descriptor queries suppress back-edges without changing navigation", "[model.schema-node]")
{
    auto pool = std::make_shared<SchemaPool>();
    auto root = pool->newObjectType();
    root.addField("next", root);
    pool->finalize();
    Environment env(std::make_shared<StringPool>(pool->dictionary()));
    auto expression = compile(env, "**", false);
    REQUIRE(expression);
    auto node = root.modelNode();
    size_t count = 0;
    LambdaResultFn consume([&](Context, Value const&) { ++count; return Result::Continue; });
    auto result = eval(env, **expression, *node, consume,
                       EvaluationOptions{.maxResults = 10000, .maxWork = 100, .maxDepth = 16});
    REQUIRE(result);
    REQUIRE(result->reason == EvaluationSummary::Reason::Complete);
    REQUIRE(count < 10000);
    result = eval(env, **expression, *node, consume, EvaluationOptions{.maxWork = 1});
    REQUIRE(result);
    REQUIRE(result->reason == EvaluationSummary::Reason::WorkLimit);
    result = eval(env, **expression, *node, consume, EvaluationOptions{.maxDepth = 0});
    REQUIRE(result);
    REQUIRE(result->reason == EvaluationSummary::Reason::DepthLimit);
    std::atomic_bool canceled = true;
    count = 0;
    result = eval(env, **expression, *node, consume,
                  EvaluationOptions{.cancel = &canceled});
    REQUIRE(result);
    REQUIRE(result->reason == EvaluationSummary::Reason::Canceled);
    REQUIRE(count == 0);
    REQUIRE_FALSE(node->get(StringPool::SchemaTruncated));
}

TEST_CASE("Descriptor graph traversal preserves sibling order and skips only active ancestors", "[model.schema-node]")
{
    auto pool = std::make_shared<SchemaPool>();
    auto root = pool->newObjectType();
    root.setTypeName("Root");
    auto child = pool->newObjectType();
    child.setTypeName("Child");
    auto leaf = pool->newScalarType(SchemaType::Kind::Int);
    leaf.setTypeName("Leaf");
    root.addField("first", child).setRequired(true);
    root.addField("second", child).setRequired(false);
    child.addField("leaf", leaf);
    child.addField("back", root).setRequired(true);
    pool->finalize();
    Environment env(std::make_shared<StringPool>(pool->dictionary()));
    auto node = root.modelNode();
    std::vector<std::string> names;
    auto all = compile(env, "**", false);
    REQUIRE(all);
    auto summary = eval(env, **all, *node, LambdaResultFn([&](Context, Value const& value) {
        if (auto current = value.node(); current && current->type() == ValueType::Object)
            if (auto name = current->get(StringPool::SchemaTypeName))
                names.emplace_back(std::get<std::string_view>(name->value()));
        return Result::Continue;
    }));
    REQUIRE(summary);
    REQUIRE(summary->reason == EvaluationSummary::Reason::Complete);
    REQUIRE(names == std::vector<std::string>{"Root", "Child", "Leaf", "Child", "Leaf"});

    auto expression = compile(env, "**.typename", false);
    REQUIRE(expression);
    auto values = eval(env, **expression, *node, nullptr);
    REQUIRE(values);
    REQUIRE(values->size() == names.size());
    for (size_t i = 0; i < names.size(); ++i)
        REQUIRE((*values)[i].toString() == names[i]);

    // Recursive back-edge occurrences are not emitted, including their edge metadata.
    expression = compile(env, "**.required", false);
    REQUIRE(expression);
    values = eval(env, **expression, *node, nullptr);
    REQUIRE(values);
    REQUIRE(values->size() == 2);
    REQUIRE((*values)[0].toString() == "true");
    REQUIRE((*values)[1].toString() == "false");

    // Explicit finite navigation is independent of recursive traversal policy.
    expression = compile(env, "fields.first.fields.back.fields.first.fields.back.required", false);
    REQUIRE(expression);
    values = eval(env, **expression, *node, nullptr);
    REQUIRE(values);
    REQUIRE(values->size() == 1);
    REQUIRE(values->front().toString() == "true");
    expression = compile(env, "fields.*.typename", false);
    REQUIRE(expression);
    values = eval(env, **expression, *node, nullptr);
    REQUIRE(values);
    REQUIRE(values->size() == 2);
    REQUIRE(values->front().toString() == "Child");
    REQUIRE(values->back().toString() == "Child");

    expression = compile(env, "**.typename", false);
    REQUIRE(expression);
    summary = eval(env, **expression, *node,
                   LambdaResultFn([](Context, Value const&) { return Result::Stop; }));
    REQUIRE(summary);
    REQUIRE(summary->reason == EvaluationSummary::Reason::ConsumerStopped);
    REQUIRE(summary->results == 1);
    values = eval(env, **expression, *node, nullptr);
    REQUIRE(values);
    REQUIRE(values->size() == names.size());
}

#if defined(SIMFIL_WITH_MODEL_JSON)
TEST_CASE("Schema JSON export owns cycle detection and resource budgets", "[model.schema-node]")
{
    auto pool = std::make_shared<SchemaPool>();
    auto root = pool->newObjectType();
    auto scalar = pool->newEnumType();
    scalar.addSymbol("READY");
    root.addField("first", scalar);
    root.addField("second", scalar);
    root.addField("next", root);
    pool->finalize();
    auto node = root.modelNode();
    auto json = node->toJson();
    REQUIRE(json["fields"]["next"]["truncated"] == "cycle");
    REQUIRE(json["fields"]["next"]["$ref"] == root.id());
    REQUIRE(json["fields"]["first"] == json["fields"]["second"]);
    REQUIRE(json["fields"]["second"]["enum"][0] == "READY");
    REQUIRE(pool->typeNode(root.id())->toJson() == json);
    for (auto limits : {SchemaTypeModelNode::JsonOptions{.maxDepth = 0},
                        SchemaTypeModelNode::JsonOptions{.maxNodes = 1},
                        SchemaTypeModelNode::JsonOptions{.maxBytes = 1}})
        REQUIRE_THROWS_AS(node->toJson(limits), std::length_error);
    REQUIRE_THROWS_AS(node->toJson({.maxDepth = 257}), std::invalid_argument);
    REQUIRE(node->toJson() == json);
    REQUIRE_FALSE(node->get(StringPool::SchemaFields)->get(pool->dictionary().get("next"))->get(StringPool::SchemaTruncated));

    size_t nodes = 0, bytes = 0;
    SchemaTypeModelNode::JsonOptions shared;
    shared.reserve = [&](size_t n, size_t b) { nodes += n; bytes += b; };
    REQUIRE(node->toJson(shared) == json);
    REQUIRE(nodes > 1);
    REQUIRE(bytes >= json.dump().size());
    shared.reserve = [&](size_t, size_t) { throw std::runtime_error("canceled by caller"); };
    REQUIRE_THROWS_AS(node->toJson(shared), std::runtime_error);
    REQUIRE(node->toJson() == json);
}

TEST_CASE("Direct schema descriptors retain exact numeric and named metadata", "[model.schema-node]")
{
    auto pool = std::make_shared<SchemaPool>();
    auto scalar = pool->newScalarType(SchemaType::Kind::Int);
    std::array<SchemaType::NumericRange, 1> ranges{{{uint64_t{1}, UINT64_MAX}}};
    scalar.setNumericDomain({.unit = "m", .ordinaryRanges = std::span<SchemaType::NumericRange const>(ranges)});
    scalar.addNamedValue({.name = "LARGE", .value = UINT64_MAX, .rawValue = uint64_t{7}});
    pool->finalize();
    auto json = scalar.modelNode()->toJson();
    REQUIRE(json["numericDomain"]["ordinaryRanges"][0]["max"] == std::to_string(UINT64_MAX));
    REQUIRE(json["namedValues"][0]["value"] == std::to_string(UINT64_MAX));
    REQUIRE(json["namedValues"][0]["rawValue"] == 7);
    std::ostringstream output;
    REQUIRE(pool->write(output));
    auto bytes = output.str();
    auto decoded = SchemaPool::read(std::span(reinterpret_cast<uint8_t const*>(bytes.data()), bytes.size()));
    REQUIRE(decoded->typeNode(scalar.id())->toJson() == json);
    SchemaTypeModelNode::JsonOptions exact;
    size_t primitiveCalls = 0;
    exact.scalar = [&](ModelNode const& node) {
        ++primitiveCalls;
        REQUIRE(node.type() != ValueType::Object);
        REQUIRE(node.type() != ValueType::Array);
        auto value = node.value();
        if (auto integer = std::get_if<int64_t>(&value))
            return nlohmann::json{{"integer", std::to_string(*integer)}};
        return node.toJson();
    };
    auto tagged = scalar.modelNode()->toJson(exact);
    REQUIRE(primitiveCalls > 1);
    REQUIRE(tagged["namedValues"][0]["rawValue"]["integer"] == "7");
}
#endif
