#include "common.hpp"
#include "simfil/model/schema-pool.h"
#include "simfil/model/schema-node.h"

#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

namespace
{
/** Reproduce derived readers that deserialize before shared ownership is established. */
class ConstructorReadPool final : public ModelPool
{
public:
    ConstructorReadPool(std::vector<std::uint8_t> const& bytes,
                        std::shared_ptr<SchemaPool const> pool, std::shared_ptr<StringPool> strings)
        : ModelPool(std::move(strings))
    {
        setSchemaResolver([pool = std::move(pool)](auto identity) {
            return pool->identity() == identity ? pool : nullptr;
        });
        loaded = read(bytes);
    }

    tl::expected<void, Error> loaded;
};

/** Publish a native graph before binding runtime nodes; all test customization precedes freezing. */
struct TypedFixture
{
    std::shared_ptr<StringPool> strings = std::make_shared<StringPool>();
    std::shared_ptr<SchemaPool> pool = std::make_shared<SchemaPool>(strings);
    std::shared_ptr<ModelPool> model = std::make_shared<ModelPool>();
    model_ptr<Object> root;

    explicit TypedFixture(std::string identity = "test-graph-v1", unsigned schemaPadding = 0,
                          std::function<void(SchemaPool&)> configure = {}, int64_t speedCode = 255)
    {
        for (unsigned i = 0; i < schemaPadding; ++i)
            REQUIRE(strings->emplace("schema-padding-" + std::to_string(i)));
        auto object = pool->newObjectType();
        auto speed = pool->newScalarType(SchemaType::Kind::Int);
        auto count = pool->newScalarType(SchemaType::Kind::Int);
        auto flags = pool->newBitmaskType();
        auto array = pool->newArrayType();
        REQUIRE(object.id() == 1);
        REQUIRE(speed.id() == 2);
        REQUIRE(count.id() == 3);
        REQUIRE(flags.id() == 4);
        REQUIRE(array.id() == 5);
        speed.addNamedValue({"SPEED_UNLIMITED", speedCode, SchemaType::NamedValue::Role::Special,
                            SchemaType::NamedValue::Meaning::Unbounded});
        speed.setNumericDomain({.encoding = "uint8", .unit = "km/h",
            .minimum = std::int64_t{0}, .maximum = std::int64_t{255}, .complete = true});
        flags.addFlag("NONE", 0);
        flags.addFlag("PAIR", 3);
        flags.addFlag("HIGH", std::uint64_t{1} << 63);
        flags.addFlag("ALIAS", 3);
        object.addField("speed", speed);
        object.addField("count", count);
        object.addField("flags", flags);
        object.addField("items", array);
        array.addElement(speed);
        if (configure)
            configure(*pool);
        pool->finalize(std::move(identity));
        REQUIRE(model->strings()->emplace("runtime-padding"));
        model->setSchemaPool(pool);
        root = model->newObject();
        REQUIRE(root->setSchema(1));
        REQUIRE(root->addField("speed", std::int64_t{255}));
        REQUIRE(root->addField("count", std::int64_t{255}));
        REQUIRE(root->addField("flags", model->newBitmask((std::uint64_t{1} << 63) | 3, pool->schemaType(4))));
        auto values = model->newArray();
        REQUIRE(values->setSchema(5));
        values->append(std::int64_t{255});
        REQUIRE(root->addField("items", values));
        model->addRoot(root);
    }

    /** Evaluate actual runtime nodes with graph-independent field ids. */
    auto query(std::string_view text) -> tl::expected<std::vector<Value>, Error>
    {
        Environment env(model->strings());
        auto ast = compile(env, text, false);
        if (!ast)
            return tl::unexpected(ast.error());
        return eval(env, **ast, *root, nullptr);
    }

    /** Query-local interning never mutates the published schema or datasource dictionary. */
    auto schemaQuery(std::string_view text) -> tl::expected<std::vector<Value>, Error>
    {
        Environment compiler(std::make_shared<StringPool>(*strings));
        compiler.setSchema(pool->schemaType(1));
        auto ast = compile(compiler, text, CompileOptions{.any = false, .rewriteMode = RewriteMode::Schema,
                                                        .rootSchema = pool->schemaType(1)});
        if (!ast)
            return tl::unexpected(ast.error());
        Environment runtime(model->strings());
        return eval(runtime, **ast, *root, nullptr);
    }

    /** Serialize native payloads together with exact artifact resolver keys. */
    static auto bytes(ModelPool& source) -> std::vector<std::uint8_t>
    {
        std::stringstream out;
        REQUIRE(source.write(out));
        auto text = out.str();
        return {text.begin(), text.end()};
    }
};
}

TEST_CASE("Scalar types survive selection but not numerical construction", "[model.schema-value]")
{
    TypedFixture f;
    REQUIRE(f.root->at(0)->addr() == f.root->at(1)->addr());
    REQUIRE(f.root->at(0)->schema() == NoTypeId);
    for (auto text : {"speed", "_['speed']", "items[0]", "items.*", "**.speed", "select(speed, 0)",
                      "min(speed, count)", "max(speed, count)", "sum(speed, $val)"}) {
        INFO(text);
        auto result = f.query(text);
        INFO((result ? "success" : result.error().message));
        REQUIRE(result);
        REQUIRE(result->size() == 1);
        INFO(result->front().toString());
        REQUIRE(result->front().type == ValueType::Int);
        REQUIRE(result->front().as<ValueType::Int>() == 255);
        REQUIRE(result->front().schemaType() == f.pool->schemaType(2));
    }
    for (auto text : {"speed + 0", "speed * 1", "speed as int", "speed = 255", "count(speed)", "sum(speed)", "255"}) {
        INFO(text);
        auto result = f.query(text);
        REQUIRE(result);
        REQUIRE(result->size() == 1);
        REQUIRE_FALSE(result->front().schemaType());
    }
    REQUIRE(f.query("min(count, speed)")->front().schemaType() == f.pool->schemaType(3));
}

TEST_CASE("Retained expressions bind field types per graph and observe replacement graphs", "[model.schema-value][evaluation.binding]")
{
    std::vector<Value> retained;
    {
        TypedFixture a, b("other-graph", 7);
        REQUIRE(a.strings->get("speed") != b.strings->get("speed"));
        REQUIRE(a.model->strings()->get("speed") != a.strings->get("speed"));
        REQUIRE(b.model->setStrings(a.model->strings()));
        Environment env(a.model->strings());
        for (auto plans : {false, true}) {
            env.enableWildcardFieldPlans = plans;
            for (auto text : {"speed", "**.speed", "_['speed']", "items[0]"}) {
                auto ast = compile(env, text, false);
                REQUIRE(ast);
                BoundExpression expression(SharedAST(std::move(*ast)), env);
                // Equal graph-local ids must never reuse another graph's dictionary or owner.
                for (auto* fixture : {&a, &b, &a, &b}) {
                    auto values = expression.eval(*fixture->root);
                    REQUIRE(values);
                    REQUIRE(values->size() == 1);
                    CHECK(values->front().schemaType() == fixture->pool->schemaType(2));
                    retained.push_back(values->front());
                }
            }
        }

        auto dynamic = a.query("_[arr('speed', 'count')]");
        REQUIRE(dynamic);
        REQUIRE(dynamic->size() == 2);
        CHECK((*dynamic)[0].schemaType() == a.pool->schemaType(2));
        CHECK((*dynamic)[1].schemaType() == a.pool->schemaType(3));

        REQUIRE(a.root->addField("late", int64_t{255}));
        auto ast = compile(env, "late", false);
        REQUIRE(ast);
        BoundExpression expression(SharedAST(std::move(*ast)), env);
        CHECK_FALSE(expression.eval(*a.root)->front().schemaType());
        // Published graphs do not mutate; a retained expression must bind a fresh
        // owner even when graph-local IDs and the runtime dictionary are unchanged.
        for (auto id : {TypeId{2}, TypeId{3}}) {
            TypedFixture replacement("replacement-" + std::to_string(id), 0, [id](SchemaPool& pool) {
                pool.schemaType(1).asObject().addField("late", pool.schemaType(id));
            });
            REQUIRE(replacement.model->setStrings(a.model->strings()));
            REQUIRE(replacement.root->addField("late", int64_t{255}));
            auto values = expression.eval(*replacement.root);
            REQUIRE(values);
            CHECK(values->front().schemaType() == replacement.pool->schemaType(id));
        }
    }
    // Neither the runtime cache, environment nor source model owns these results now.
    for (auto const& value : retained) {
        CHECK(value.as<ValueType::Int>() == 255);
        REQUIRE(value.schemaType());
        CHECK(value.schemaType().namedValue("SPEED_UNLIMITED"));
    }
}

TEST_CASE("Native copies keep compact compounds and qualify detached occurrences", "[model.schema-value]")
{
    TypedFixture a;
    TypedFixture b("test-graph-v2");
    auto destination = std::make_shared<ModelPool>(std::make_shared<StringPool>(*a.model->strings()));
    auto compound = destination->copyNode(*a.root);
    REQUIRE(compound);
    REQUIRE((*compound)->schemaType() == a.pool->schemaType(1));
    REQUIRE((*compound)->at(0)->schema() == NoTypeId);
    REQUIRE((*compound)->at(0)->addr().column() != ModelPool::QualifiedScalar);
    auto values = std::vector<Value>{a.query("speed")->front(), a.query("count")->front(), b.query("speed")->front()};
    auto copied = destination->copySequence(values);
    REQUIRE(copied);
    destination->addRoot(*copied);
    REQUIRE((*copied)->at(0)->addr() != (*copied)->at(1)->addr());
    REQUIRE((*copied)->at(0)->schemaType() == values[0].schemaType());
    REQUIRE((*copied)->at(2)->schemaType() == values[2].schemaType());
    REQUIRE(destination->serializationSizeStats().qualifiedScalarBytes == 3 * (sizeof(ModelNodeAddress) + sizeof(TypeId)));
    auto bytes = TypedFixture::bytes(*destination);
    auto restored = std::make_shared<ModelPool>(destination->strings());
    REQUIRE_FALSE(restored->read(bytes));
    restored->setSchemaResolver([one = a.pool, two = b.pool](std::string_view identity) {
        return identity == one->identity() ? one : identity == two->identity() ? two : nullptr;
    });
    REQUIRE(restored->read(bytes));
    auto root = restored->root(0).value();
    REQUIRE(root->at(0)->schemaType() == a.pool->schemaType(2));
    REQUIRE(root->at(2)->schemaType() == b.pool->schemaType(2));
    a.root = {}; a.model.reset(); a.pool.reset();
    b.root = {}; b.model.reset(); b.pool.reset();
    destination.reset(); values.clear();
    REQUIRE(std::get<int64_t>(root->at(0)->value()) == 255);
    REQUIRE(root->at(0)->schemaType().namedValue("SPEED_UNLIMITED"));
    REQUIRE(restored->validate());
}

TEST_CASE("Models isolate schema reference counts without duplicating graph identity", "[model.schema-value]")
{
    TypedFixture f;
    auto source = f.model->schemaPool(f.pool->identity());
    auto target = std::make_shared<ModelPool>();
    target->setSchemaPool(source);
    auto other = target->schemaPool(f.pool->identity());
    REQUIRE(source.get() == f.pool.get());
    REQUIRE(other.get() == source.get());
    REQUIRE(source.owner_before(other) != other.owner_before(source));
    REQUIRE(source.owner_before(f.pool) != f.pool.owner_before(source));

    auto canonicalCount = f.pool.use_count();
    auto otherCount = other.use_count();
    auto sourceCount = source.use_count();
    auto result = f.query("speed");
    REQUIRE(result);
    std::vector<Value> retained(256, result->front());
    CHECK(f.pool.use_count() == canonicalCount);
    CHECK(other.use_count() == otherCount);
    CHECK(source.use_count() == sourceCount + 257);
    REQUIRE(retained.front().schemaType() == f.pool->schemaType(2));
}

TEST_CASE("Imported schema owners outlive their sources without retaining ownership chains", "[model.schema-value]")
{
    std::weak_ptr<SchemaPool const> sourceGroup, targetGroup, canonical;
    std::weak_ptr<StringPool const> dictionary;
    std::vector<Value> retained;
    {
        TypedFixture f;
        auto local = f.model->schemaPool(f.pool->identity());
        auto localType = f.model->resolveSchemaType(2);
        sourceGroup = local;
        canonical = f.pool;
        dictionary = f.strings;
        auto target = std::make_shared<ModelPool>(f.model->strings());
        SECTION("Direct pool")
        {
            target->setSchemaPool(local);
            target->addRoot(target->newQualifiedValue(std::int64_t{255}, localType));
        }
        SECTION("Copied scalar imports")
        {
            auto values = f.query("speed");
            REQUIRE(values);
            auto copied = target->copySequence(*values);
            REQUIRE(copied);
            target->addRoot((*copied)->at(0));
        }
        SECTION("Native reader resolver")
        {
            auto serialized = std::make_shared<ModelPool>();
            serialized->addRoot(serialized->newQualifiedValue(std::int64_t{255}, localType));
            target->setSchemaResolver([local](auto) { return local; });
            REQUIRE(target->read(TypedFixture::bytes(*serialized)));
            // The resolver's explicit capture must not mask a retained import chain.
            target->setSchemaResolver({});
        }
        auto node = target->root(0).value();
        auto type = node->schemaType();
        REQUIRE(type == f.pool->schemaType(2));
        REQUIRE(type.pool() == local.get());
        targetGroup = target->schemaPool(f.pool->identity());
        retained.push_back(Value::field(*node));
    }
    CHECK(sourceGroup.expired());
    CHECK_FALSE(targetGroup.expired());
    CHECK_FALSE(canonical.expired());
    CHECK_FALSE(dictionary.expired());
    CHECK(retained.front().as<ValueType::Int>() == 255);
    REQUIRE(retained.front().schemaType().namedValue("SPEED_UNLIMITED"));
    retained.clear();
    CHECK(targetGroup.expired());
    // Weak observers retain control blocks, never the graph hidden in their deleter.
    CHECK(canonical.expired());
    CHECK(dictionary.expired());
}

TEST_CASE("Model-local owners preserve alias pointers when flattening imports", "[model.schema-value]")
{
    auto target = std::make_shared<ModelPool>();
    std::weak_ptr<SchemaPool const> sourceGroup, targetGraph;
    {
        TypedFixture first("first-graph"), second("second-graph");
        auto keeper = std::make_shared<std::array<std::shared_ptr<SchemaPool const>, 2>>(
            std::array<std::shared_ptr<SchemaPool const>, 2>{first.pool, second.pool});
        auto source = std::make_shared<ModelPool>();
        source->setSchemaPool({keeper, first.pool.get()});
        auto local = source->schemaPool(first.pool->identity());
        sourceGroup = local;
        targetGraph = second.pool;
        // External aliases may name a different graph retained by the same control block.
        target->setSchemaPool({local, second.pool.get()});
        REQUIRE(target->resolveSchemaType(2).pool() == second.pool.get());
    }
    CHECK(sourceGroup.expired());
    CHECK_FALSE(targetGraph.expired());
    REQUIRE(target->resolveSchemaType(2).pool()->identity() == "second-graph");
    REQUIRE(target->resolveSchemaType(2).namedValue("SPEED_UNLIMITED"));
    target.reset();
    CHECK(targetGraph.expired());
}

TEST_CASE("Concurrent models share schema definitions but retain independent query results", "[model.schema-value][evaluation.binding]")
{
    std::array<Value, 4> retained{Value::undef(), Value::undef(), Value::undef(), Value::undef()};
    {
        TypedFixture f;
        Environment compiler(f.model->strings());
        auto compiled = compile(compiler, "**.speed", false);
        REQUIRE(compiled);
        SharedAST shared(std::move(*compiled));
        std::array<std::shared_ptr<ModelPool>, 4> models;
        for (size_t worker = 0; worker < models.size(); ++worker) {
            auto& model = models[worker];
            model = std::make_shared<ModelPool>();
            REQUIRE(model->strings()->emplace("worker-" + std::to_string(worker)));
            model->setSchemaPool(f.pool);
            auto object = model->newObject();
            REQUIRE(object->setSchema(1));
            REQUIRE(object->addField("speed", int64_t{255} + static_cast<int64_t>(worker)));
            model->addRoot(object);
            model->freeze();
        }
        const auto results = RunConcurrentWorkers<4>([&](size_t worker) {
            Environment env(models[worker]->strings());
            BoundExpression expression(shared, env);
            for (size_t iteration = 0; iteration < 40; ++iteration) {
                // Exercise both traversals against one graph and independently owned data dictionaries.
                env.enableWildcardFieldPlans = iteration % 2 == 0;
                auto result = expression.eval(*models[worker]->root(0).value());
                if (!result || result->size() != 1 || result->front().as<ValueType::Int>() != int64_t{255} + worker ||
                    result->front().schemaType() != f.pool->schemaType(2))
                    return false;
                retained[worker] = result->front();
            }
            return true;
        });
        REQUIRE(std::ranges::all_of(results, [](bool passed) { return passed; }));
    }
    for (size_t worker = 0; worker < retained.size(); ++worker) {
        CHECK(retained[worker].as<ValueType::Int>() == int64_t{255} + worker);
        REQUIRE(retained[worker].schemaType().namedValue("SPEED_UNLIMITED"));
    }
}

TEST_CASE("Open objects retain declared scalar types without guessing unknown alternatives", "[model.schema-value]")
{
    TypedFixture f("open-graph", 0, [](SchemaPool& pool) {
        pool.schemaType(1).asObject().setOpen(true);
        REQUIRE(pool.strings()->emplace("extra"));
    });
    auto speed = f.strings->get("speed");
    auto extra = f.strings->get("extra");
    REQUIRE(f.root->addField("extra", std::int64_t{255}));
    REQUIRE(std::ranges::equal(f.pool->schemaType(1).fieldTypes(speed), std::array<TypeId, 1>{2}));
    REQUIRE(std::ranges::equal(f.pool->schemaType(1).fieldTypes(extra), std::array<TypeId, 1>{NoTypeId}));
    for (auto query : {"speed", "_['speed']", "**.speed"}) {
        INFO(query);
        auto values = f.query(query);
        REQUIRE(values);
        REQUIRE(values->front().schemaType() == f.pool->schemaType(2));
    }
    REQUIRE_FALSE(f.query("extra")->front().schemaType());
    REQUIRE(f.schemaQuery("speed = SPEED_UNLIMITED")->front().as<ValueType::Bool>());
    auto destination = std::make_shared<ModelPool>(std::make_shared<StringPool>(*f.model->strings()));
    auto compound = destination->copyNode(*f.root);
    REQUIRE(compound);
    REQUIRE((*compound)->at(0)->schema() == NoTypeId);
    REQUIRE(Value::at(**compound, *(*compound)->at(0), 0).schemaType() == f.pool->schemaType(2));
    auto scalar = destination->copySequence(*f.query("speed"));
    REQUIRE(scalar);
    REQUIRE((*scalar)->at(0)->schemaType() == f.pool->schemaType(2));

    for (auto child : {NoTypeId, TypeId{3}, TypeId{2}}) {
        TypedFixture alternative("open-alternative-" + std::to_string(child), 0, [child](SchemaPool& pool) {
            pool.schemaType(1).asObject().setOpen(true);
            auto other = pool.newObjectType();
            other.setOpen(true);
            if (child)
                other.addField("speed", pool.schemaType(child));
            auto combined = pool.newCompositionType(SchemaType::Composition::AnyOf);
            combined.addAlternative(pool.schemaType(1));
            combined.addAlternative(other);
            REQUIRE(combined.id() == 7);
        });
        REQUIRE(alternative.root->setSchema(7));
        auto choices = alternative.pool->schemaType(7).fieldTypes(alternative.strings->get("speed"));
        auto observed = std::vector<TypeId>(choices.begin(), choices.end());
        CAPTURE(child, observed);
        // Retain one entry per alternative in declaration order, including equal IDs.
        // Provenance is unambiguous only when every retained entry agrees.
        REQUIRE(std::ranges::equal(choices, std::array<TypeId, 2>{2, child}));
        auto effective = alternative.pool->schemaType(7).fieldType(alternative.strings->get("speed"));
        if (child == 2)
            REQUIRE(effective == alternative.pool->schemaType(2));
        else
            REQUIRE_FALSE(effective);
        auto selected = alternative.query("speed");
        REQUIRE(selected);
        if (child == 2)
            REQUIRE(selected->front().schemaType() == alternative.pool->schemaType(2));
        else
            REQUIRE_FALSE(selected->front().schemaType());
    }
}

TEST_CASE("Bitmasks preserve high and unknown bits through query JSON and binary", "[model.schema-value][bitmask]")
{
    TypedFixture f;
    auto mask = f.root->at(2);
    REQUIRE(mask->type() == ValueType::Bitmask);
    REQUIRE(f.model->serializationSizeStats().bitmaskBytes == sizeof(std::uint64_t) + sizeof(TypeId));
    for (auto query : {"hasFlag(flags, 'PAIR')", "hasFlag(flags, 'HIGH')", "hasFlag(flags, 'ALIAS')",
                      "hasFlag(bitmask(flags, '0'), 'NONE')", "flags = bitmask(flags, '0x8000000000000003')",
                      "(flags & 3) = 3", "(bitmask(flags, '1') << 63) = bitmask(flags, '0x8000000000000000')"}) {
        INFO(query);
        auto result = f.query(query);
        INFO((result ? "success" : result.error().message));
        REQUIRE(result);
        REQUIRE(result->front().as<ValueType::Bool>());
    }
    REQUIRE_FALSE(f.query("hasFlag(bitmask(flags, '1'), 'PAIR')")->front().as<ValueType::Bool>());
    REQUIRE_FALSE(f.query("hasFlag(flags, 'NONE')")->front().as<ValueType::Bool>());
    for (auto query : {"hasFlag(flags, 'MISSING')", "flags & -1", "flags | 1.5", "flags << 64",
                      "flags + 0", "flags as int", "bitmask(flags, '18446744073709551616')"}) {
        INFO(query);
        REQUIRE_FALSE(f.query(query));
    }
    auto golden = nlohmann::json{{"_bitmask", true}, {"bits", "0x8000000000000003"},
        {"schema", "test-graph-v1"}, {"typeId", 4}};
    REQUIRE(mask->toJson() == golden);
    auto imported = json::buildModelNode(golden, *f.model);
    REQUIRE(imported);
    REQUIRE((*imported)->toJson() == golden);
    auto unrelated = std::make_shared<ModelPool>();
    REQUIRE_FALSE(json::buildModelNode(golden, *unrelated));
    golden["application"] = true;
    REQUIRE(json::buildModelNode(golden, *unrelated).value()->type() == ValueType::Object);
    auto restored = std::make_shared<ModelPool>(f.model->strings());
    restored->setSchemaPool(f.pool);
    auto read = restored->read(TypedFixture::bytes(*f.model));
    INFO((read ? "success" : read.error().message));
    REQUIRE(read);
    REQUIRE(restored->root(0).value()->at(2)->toJson() == mask->toJson());
    REQUIRE_THROWS(f.model->newBitmask(0, f.pool->schemaType(2)));
    auto other = TypedFixture("foreign");
    REQUIRE_THROWS(f.model->newQualifiedValue(std::get<Bitmask>(mask->value()), other.pool->schemaType(4)));
}

TEST_CASE("Frozen pools reject mutations through base APIs and retained nodes", "[model.schema-value][model.freeze]")
{
    TypedFixture f;
    auto array = f.model->newArray();
    auto before = TypedFixture::bytes(*f.model);
    f.model->freeze();
    REQUIRE(f.model->frozen());
    REQUIRE(f.model->strings()->frozen());
    REQUIRE_FALSE(f.model->strings()->emplace("new-name"));
    REQUIRE(f.model->strings()->emplace("speed"));
    REQUIRE_THROWS(f.model->strings()->addStaticKey(200, "new-key"));
    REQUIRE_FALSE(f.model->strings()->read({}));
    REQUIRE_THROWS(f.model->newValue(std::string_view("new-string")));
    REQUIRE_THROWS(f.model->newValue(int64_t{3}));
    REQUIRE_THROWS(f.model->newObject());
    REQUIRE_FALSE(f.model->tryNewObject());
    REQUIRE_FALSE(f.model->tryNewArray());
    REQUIRE_FALSE(f.model->tryNewValue(int64_t{3}));
    REQUIRE_FALSE(f.model->tryNewValue(2.5));
    REQUIRE_FALSE(f.model->tryNewValue(std::string_view("new-string")));
    REQUIRE_FALSE(f.model->tryNewValue(ByteArray{"new-bytes"}));
    REQUIRE_FALSE(f.model->tryNewValue(f.model->strings()->get("speed")));
    REQUIRE_THROWS(f.model->newQualifiedValue(int64_t{3}, f.pool->schemaType(2)));
    REQUIRE_THROWS(f.model->newBitmask(3, f.pool->schemaType(4)));
    REQUIRE_FALSE(f.model->copyNode(*f.root));
    REQUIRE_FALSE(json::buildModelNode(nlohmann::json(2.5), *f.model));
    REQUIRE_FALSE(json::buildModelNode(f.root->at(2)->toJson(), *f.model));
    REQUIRE_THROWS(f.model->addRoot(f.root));
    REQUIRE_THROWS(f.model->clear());
    REQUIRE_THROWS(f.model->setSchemaPool(f.pool));
    REQUIRE_THROWS(f.model->setSchemaResolver({}));
    REQUIRE_FALSE(f.model->read(before));
    REQUIRE_FALSE(f.model->setStrings(std::make_shared<StringPool>()));
    REQUIRE_FALSE(f.root->addField("speed", f.model->newSmallValue(true)));
    REQUIRE_THROWS(array->append(f.model->newSmallValue(true)));
    REQUIRE_THROWS(array->append(true));
    REQUIRE_FALSE(f.root->addBool("speed", true));
    REQUIRE_FALSE(f.root->addField("speed", std::uint16_t{1}));
    REQUIRE_FALSE(f.root->extend(f.root));
    REQUIRE_FALSE(array->extend(array));
    REQUIRE_FALSE(f.root->setSchema(2));
    REQUIRE_FALSE(array->setSchema(5));
    REQUIRE(TypedFixture::bytes(*f.model) == before);
    auto completionStrings = std::make_shared<StringPool>(*f.model->strings());
    REQUIRE_FALSE(completionStrings->frozen());
    REQUIRE(completionStrings->emplace("completion-only"));
}

TEST_CASE("Qualified string copying preserves aliased storage and exact types", "[model.schema-value][model.growth]")
{
    TypedFixture f("string-copy", 0, [](SchemaPool& pool) {
        REQUIRE(pool.newScalarType(SchemaType::Kind::String).id() == 6);
    });
    std::string text(65536, 'x');
    text.front() = 'a';
    text.back() = 'z';
    auto original = f.model->tryNewValue(std::string_view(text));
    REQUIRE(original);
    // Qualification appends into the same byte buffer that owns this borrowed view.
    auto qualified = f.model->newQualifiedValue(std::get<std::string_view>((*original)->value()), f.pool->schemaType(6));
    auto copied = f.model->copyNode(*qualified);
    REQUIRE(copied);
    REQUIRE((*copied)->schemaType() == f.pool->schemaType(6));
    REQUIRE(std::get<std::string_view>((*original)->value()) == text);
    REQUIRE(std::get<std::string_view>(qualified->value()) == text);
    REQUIRE(std::get<std::string_view>((*copied)->value()) == text);

    auto target = std::make_shared<ModelPool>();
    auto detached = target->copyNode(**copied);
    REQUIRE(detached);
    target->addRoot(*detached);
    auto restored = std::make_shared<ConstructorReadPool>(TypedFixture::bytes(*target), f.pool, target->strings());
    REQUIRE(restored->loaded);
    auto node = restored->root(0).value();
    REQUIRE(node->schemaType() == f.pool->schemaType(6));
    REQUIRE(std::get<std::string_view>(node->value()) == text);
    REQUIRE(restored->validate());

    auto before = TypedFixture::bytes(*target);
    REQUIRE_THROWS_AS(target->newQualifiedValue(std::string_view(text), f.pool->schemaType(2)), std::invalid_argument);
    REQUIRE_THROWS_AS(target->newQualifiedValue(ScalarValueType{}, f.pool->schemaType(6)), std::invalid_argument);
    REQUIRE_THROWS_AS(target->newBitmask(1, f.pool->schemaType(6)), std::invalid_argument);
    REQUIRE(TypedFixture::bytes(*target) == before);
}

TEST_CASE("Unbound copied schemas and overflowing integer literals fail explicitly", "[model.schema-value]")
{
    auto source = std::make_shared<ModelPool>();
    auto target = std::make_shared<ModelPool>();
    auto root = source->newObject();
    REQUIRE(root->setSchema(42));
    REQUIRE_FALSE(target->copyNode(*root));
    Environment env(target->strings());
    for (auto query : {"9223372036854775808", "0xffffffffffffffff", "18446744073709551616"}) {
        INFO(query);
        REQUIRE_FALSE(compile(env, query, false));
    }
    REQUIRE(compile(env, "9223372036854775807", false));
    REQUIRE(compile(env, "18446744073709551616.0", false));
}

TEST_CASE("Native copies explicitly remap exact types without qualifying ordinary leaves", "[model.copy][model.schema-value]")
{
    TypedFixture source;
    auto pool = std::make_shared<SchemaPool>();
    for (size_t i = 0; i < 10; ++i)
        pool->newType(SchemaType::Kind::Unknown);
    auto object = pool->newObjectType();
    auto speed = pool->newScalarType(SchemaType::Kind::Int);
    auto count = pool->newScalarType(SchemaType::Kind::Int);
    auto flags = pool->newBitmaskType();
    auto array = pool->newArrayType();
    REQUIRE(object.id() == 11);
    REQUIRE(array.id() == 15);
    object.addField("speed", speed);
    object.addField("count", count);
    object.addField("flags", flags);
    object.addField("items", array);
    array.addElement(speed);
    pool->finalize("composed-graph");
    auto destination = std::make_shared<ModelPool>(std::make_shared<StringPool>(*source.model->strings()));
    std::vector<TypeId> mapped;
    ModelPool::CopyOptions options;
    options.schemaRemap = [&](SchemaType const& typeRef) {
        REQUIRE(typeRef.pool() == source.pool.get());
        mapped.push_back(typeRef.id());
        return pool->schemaType(TypeId(typeRef.id() + 10));
    };
    auto copied = destination->copyNode(*source.root, options);
    REQUIRE(copied);
    REQUIRE((*copied)->schemaType() == pool->schemaType(11));
    REQUIRE((*copied)->at(2)->schemaType() == pool->schemaType(14));
    REQUIRE((*copied)->at(3)->schemaType() == pool->schemaType(15));
    REQUIRE(mapped == std::vector<TypeId>{1, 4, 5});
    REQUIRE((*copied)->at(0)->schema() == NoTypeId);
    REQUIRE((*copied)->at(3)->at(0)->schema() == NoTypeId);
    Environment env(destination->strings());
    auto expression = compile(env, "speed", false);
    REQUIRE(expression);
    auto value = eval(env, **expression, **copied, nullptr);
    REQUIRE(value);
    REQUIRE(value->front().schemaType() == pool->schemaType(12));

    auto selected = source.query("speed");
    REQUIRE(selected);
    auto sequence = destination->copySequence(*selected, options);
    REQUIRE(sequence);
    REQUIRE((*sequence)->at(0)->schemaType() == pool->schemaType(12));
    REQUIRE(mapped == std::vector<TypeId>{1, 4, 5, 2});
    destination->addRoot(*copied);
    destination->addRoot(*sequence);
    REQUIRE(destination->validate());

    SECTION("In-place remapping preserves payload addresses and detached old values") {
        auto address = source.root->addr();
        auto scalar = source.model->newQualifiedValue(std::int64_t{255}, source.pool->schemaType(2));
        source.model->addRoot(scalar);
        mapped.clear();
        source.model->remapSchemas(pool, options.schemaRemap);
        REQUIRE(source.model->hasDirectSchemaPool(pool->identity()));
        REQUIRE(source.root->addr() == address);
        REQUIRE(source.root->schemaType() == pool->schemaType(11));
        REQUIRE(source.root->at(2)->schemaType() == pool->schemaType(14));
        REQUIRE(source.root->at(3)->schemaType() == pool->schemaType(15));
        REQUIRE(source.root->at(0)->schema() == NoTypeId);
        REQUIRE(scalar->schemaType() == pool->schemaType(12));
        REQUIRE(selected->front().schemaType() == source.pool->schemaType(2));
        REQUIRE(source.query("speed")->front().schemaType() == pool->schemaType(12));
        REQUIRE(mapped.size() == 4);
        auto restored = std::make_shared<ModelPool>(source.model->strings());
        restored->setSchemaResolver([pool](std::string_view id) {
            return id == pool->identity() ? pool : nullptr;
        });
        REQUIRE(restored->read(TypedFixture::bytes(*source.model)));
        REQUIRE(restored->hasDirectSchemaPool(pool->identity()));
        REQUIRE(restored->validate());
    }
    SECTION("In-place validation failure leaves all bytes and bindings unchanged") {
        auto before = TypedFixture::bytes(*source.model);
        auto fail = [&](SchemaType const& typeRef) {
            return pool->schemaType(TypeId(typeRef.id() == 4 ? 12 : typeRef.id() + 10));
        };
        REQUIRE_THROWS_AS(source.model->remapSchemas(pool, fail), std::invalid_argument);
        REQUIRE(TypedFixture::bytes(*source.model) == before);
        REQUIRE(source.model->hasDirectSchemaPool(source.pool->identity()));
        REQUIRE_THROWS_AS(source.model->remapSchemas(pool, [](auto const& typeRef) { return typeRef; }),
                          std::invalid_argument);
        REQUIRE(TypedFixture::bytes(*source.model) == before);
    }
    SECTION("An unqualified or empty pool still installs the effective artifact") {
        auto empty = std::make_shared<ModelPool>();
        empty->addRoot(empty->newValue(std::int64_t{42}));
        empty->remapSchemas(pool, [](auto const&) -> SchemaType {
            FAIL("Unqualified leaves must not invoke schema remapping");
            return {};
        });
        REQUIRE(empty->hasDirectSchemaPool(pool->identity()));
        REQUIRE(empty->root(0).value()->schema() == NoTypeId);
        empty->freeze();
        REQUIRE_THROWS_AS(empty->remapSchemas(pool, options.schemaRemap), std::logic_error);
    }
    SECTION("Missing or invalid replacement types fail instead of dropping provenance") {
        options.schemaRemap = [](auto const&) { return SchemaType{}; };
        REQUIRE_FALSE(destination->copyNode(*source.root, options));
        REQUIRE_FALSE(destination->copySequence(*selected, options));
        options.schemaRemap = [pool](auto const&) { return pool->schemaType(60000); };
        REQUIRE_FALSE(destination->copyNode(*source.root, options));
    }
    SECTION("Callback failures become copy errors") {
        options.schemaRemap = [](auto const&) -> SchemaType {
            throw std::invalid_argument("incompatible contribution");
        };
        auto failed = destination->copyNode(*source.root, options);
        REQUIRE_FALSE(failed);
        REQUIRE(failed.error().message == "incompatible contribution");
    }
    SECTION("A replacement cannot change the physical payload type") {
        options.schemaRemap = [pool](auto const&) { return pool->schemaType(12); };
        REQUIRE_FALSE(destination->copyNode(*source.root, options));
        options.schemaRemap = [pool](auto const&) { return pool->schemaType(11); };
        REQUIRE_FALSE(destination->copySequence(*selected, options));
    }
}

TEST_CASE("Named queries use the actual numeric or flag type rather than common raw codes", "[model.schema-value][schema.symbol]")
{
    TypedFixture f;
    for (auto query : {"SPEED_UNLIMITED", "speed = SPEED_UNLIMITED", "SPEED_UNLIMITED <= speed",
                      "PAIR", "HIGH", "flags != PAIR"}) {
        INFO(query);
        auto result = f.schemaQuery(query);
        INFO((result ? "success" : result.error().message));
        REQUIRE(result);
        REQUIRE(result->size() == 1);
        REQUIRE(result->front().type == ValueType::Bool);
        REQUIRE(result->front().as<ValueType::Bool>());
    }
    REQUIRE_FALSE(f.schemaQuery("flags = PAIR")->front().as<ValueType::Bool>());
    REQUIRE_FALSE(f.schemaQuery("speed = 'SPEED_UNLIMITED'")->front().as<ValueType::Bool>());
    REQUIRE_FALSE(f.schemaQuery("count = SPEED_UNLIMITED"));

    TypedFixture changed("changed-symbol", 0, {}, 0);
    REQUIRE_FALSE(changed.schemaQuery("speed = SPEED_UNLIMITED")->front().as<ValueType::Bool>());
    REQUIRE_FALSE(changed.schemaQuery("SPEED_UNLIMITED")->front().as<ValueType::Bool>());
    for (auto code : {int64_t{0}, int64_t{255}}) {
        TypedFixture open("open-symbol-" + std::to_string(code), 0, [](SchemaPool& pool) {
            pool.schemaType(1).asObject().setOpen(true);
        }, code);
        REQUIRE(open.schemaQuery("SPEED_UNLIMITED")->front().as<ValueType::Bool>() == (code == 255));
    }
}

TEST_CASE("Open string enum traversal retains the owning dictionary across unbound descendants", "[model.schema-value][schema.symbol]")
{
    TypedFixture f("open-string-enum", 0, [](SchemaPool& pool) {
        auto root = pool.schemaType(1).asObject();
        root.setOpen(true);
        auto values = pool.newEnumType();
        values.addSymbol("LEGACY_ENUM");
        auto nested = pool.newObjectType();
        REQUIRE(values.id() == 6);
        REQUIRE(nested.id() == 7);
        nested.addField("text", values);
        root.addField("nested", nested);
        root.addField("", values);
    });
    auto object = f.model->newObject();
    REQUIRE(object->addField("text", "LEGACY_ENUM"));
    REQUIRE(f.root->addField("nested", object));
    REQUIRE(f.model->strings()->get("LEGACY_ENUM") == StringPool::Empty);
    for (auto bindChild : {false, true}) {
        if (bindChild)
            REQUIRE(object->setSchema(7));
        auto result = f.schemaQuery("LEGACY_ENUM");
        REQUIRE(result);
        REQUIRE(result->size() == 1);
        REQUIRE(result->front().as<ValueType::Bool>());
    }
    REQUIRE(f.model->strings()->get("LEGACY_ENUM") == StringPool::Empty);
    // An undeclared spelling must not borrow the genuinely empty field's schema.
    f.root = f.model->newObject();
    REQUIRE(f.root->setSchema(1));
    REQUIRE(f.root->addField("undeclared", "LEGACY_ENUM"));
    auto unknown = f.schemaQuery("LEGACY_ENUM");
    REQUIRE(unknown);
    REQUIRE_FALSE(unknown->front().as<ValueType::Bool>());
}

TEST_CASE("Schema descriptors and completion expose borrowed named numeric metadata", "[model.schema-value][schema.symbol]")
{
    TypedFixture f;
    auto descriptor = f.pool->schemaType(1).modelNode();
    auto before = f.strings->size();
    auto json = descriptor->toJson();
    REQUIRE(json["fields"]["speed"]["numericDomain"]["unit"] == "km/h");
    REQUIRE(json["fields"]["speed"]["namedValues"][0]["value"] == 255);
    REQUIRE(json["fields"]["speed"]["namedValues"][0]["meaning"] == "unbounded");
    REQUIRE(json["fields"]["flags"]["bitWidth"] == 64);
    REQUIRE(json["fields"]["flags"]["namedValues"][2]["value"] == "0x8000000000000000");
    REQUIRE(f.strings->size() == before);
    Environment env(std::make_shared<StringPool>(*f.strings));
    env.setSchema(f.pool->schemaType(1));
    for (auto query : {"SPEED_", "speed = SPEED_", "flags = HI"}) {
        INFO(query);
        auto candidates = complete(env, query, std::string_view(query).size(), f.pool->schemaType(1));
        REQUIRE(candidates);
        auto wanted = std::string_view(query) == "flags = HI" ? "HIGH" : "SPEED_UNLIMITED";
        REQUIRE(std::ranges::any_of(*candidates, [&](auto const& candidate) { return candidate.text == wanted; }));
    }
    auto candidates = complete(env, "count = SPEED_", 14, f.pool->schemaType(1));
    REQUIRE(candidates);
    REQUIRE_FALSE(std::ranges::any_of(*candidates, [](auto const& candidate) { return candidate.text == "SPEED_UNLIMITED"; }));
    auto quoted = std::string("speed = 'SPEED_");
    candidates = complete(env, quoted, quoted.size(), f.pool->schemaType(1));
    REQUIRE(candidates);
    REQUIRE_FALSE(std::ranges::any_of(*candidates, [](auto const& candidate) {
        return candidate.text.find("SPEED_UNLIMITED") != std::string::npos;
    }));
    auto flag = std::string("hasFlag(flags, 'PA");
    candidates = complete(env, flag, flag.size(), f.pool->schemaType(1));
    REQUIRE(candidates);
    REQUIRE(std::ranges::any_of(*candidates, [](auto const& candidate) { return candidate.text == "\"PAIR\""; }));
}

TEST_CASE("Qualified scalar validation is safe in a model constructor", "[model.schema-value][limits]")
{
    TypedFixture f("constructor-types", 0, [](SchemaPool& pool) {
        for (auto kind : {SchemaType::Kind::Bool, SchemaType::Kind::Float,
                          SchemaType::Kind::String, SchemaType::Kind::Bytes})
            pool.newScalarType(kind);
    });
    std::vector<std::pair<ScalarValueType, TypeId>> values{
        {std::int64_t{255}, 2}, {std::int64_t{-1}, 2}, {std::int64_t{65536}, 2},
        {true, 6}, {2.5, 7}, {std::string("qualified text"), 8}, {ByteArray("raw bytes"), 9}};
    for (auto direct : {false, true}) {
        auto source = std::make_shared<ModelPool>();
        if (direct)
            source->setSchemaPool(f.pool);
        for (auto const& [raw, typeRef] : values)
            source->addRoot(source->newQualifiedValue(raw, f.pool->schemaType(typeRef)));
        auto restored = std::make_shared<ConstructorReadPool>(TypedFixture::bytes(*source), f.pool, source->strings());
        REQUIRE(restored->loaded);
        REQUIRE(restored->numRoots() == values.size());
        for (std::size_t i = 0; i < values.size(); ++i) {
            auto node = restored->root(i).value();
            REQUIRE(node->type() == Value(ScalarValueType(values[i].first)).type);
            REQUIRE(node->toJson() == source->root(i).value()->toJson());
            REQUIRE(node->schemaType() == f.pool->schemaType(values[i].second));
        }
    }

    auto source = std::make_shared<ModelPool>();
    source->addRoot(source->newQualifiedValue(std::int64_t{255}, f.pool->schemaType(2)));
    auto original = TypedFixture::bytes(*source);
    std::array<std::uint8_t, 4> raw{Model::UInt16, 255, 0, 0};
    auto position = std::search(original.begin(), original.end(), raw.begin(), raw.end());
    REQUIRE(position != original.end());
    auto offset = std::size_t(position - original.begin());
    for (auto address : {ModelNodeAddress{Model::UInt16, 65536}, ModelNodeAddress{Model::Int16, 65536},
             ModelNodeAddress{Model::Bool, 2}, ModelNodeAddress{ModelPool::Int64, 0},
             ModelNodeAddress{ModelPool::Double, 0}, ModelNodeAddress{ModelPool::String, 0},
             ModelNodeAddress{ModelPool::ByteArray, 0}, ModelNodeAddress{ModelPool::PooledString, 65536},
             ModelNodeAddress{Model::Null, 1}, ModelNodeAddress{Model::Scalar, 0},
             ModelNodeAddress{ModelPool::Objects, 0}, ModelNodeAddress{ModelPool::QualifiedScalar, 0},
             ModelNodeAddress{ModelPool::Bitmask, 0}, ModelNodeAddress{Model::Bool, 1}}) {
        INFO("column=" << unsigned(address.column()) << " index=" << address.index());
        auto malformed = original;
        for (unsigned i = 0; i < raw.size(); ++i)
            malformed[offset + i] = std::uint8_t(address.value_ >> (i * 8));
        auto restored = std::make_shared<ConstructorReadPool>(malformed, f.pool, source->strings());
        REQUIRE_FALSE(restored->loaded);
    }
}

TEST_CASE("Native readers bound allocations and reject malformed qualified references", "[model.schema-value][limits]")
{
    auto target = std::make_shared<ModelPool>();
    // Three empty fixed columns precede the variable-sized string buffer. The claimed payload is absent.
    std::vector<std::uint8_t> truncated(3 * sizeof(std::uint64_t), 0);
    truncated.insert(truncated.end(), {0xff, 0xff, 0xff, 0xff});
    auto before = target->memoryUsageStats().stringData.allocatedBytes;
    REQUIRE_FALSE(target->read(truncated));
    REQUIRE(target->memoryUsageStats().stringData.allocatedBytes <= before);

    TypedFixture f;
    REQUIRE(f.model->hasDirectSchemaPool("test-graph-v1"));
    REQUIRE_FALSE(f.model->hasDirectSchemaPool("foreign"));
    auto qualified = std::make_shared<ModelPool>();
    auto node = qualified->newQualifiedValue(std::int64_t{255}, f.pool->schemaType(2));
    qualified->addRoot(node);
    REQUIRE_FALSE(qualified->hasDirectSchemaPool(f.pool->identity()));
    auto bytes = TypedFixture::bytes(*qualified);
    std::array<std::uint8_t, 4> raw{Model::UInt16, 255, 0, 0};
    auto position = std::search(bytes.begin(), bytes.end(), raw.begin(), raw.end());
    REQUIRE(position != bytes.end());
    // A qualifier may only point directly to primitive storage, never itself or another qualifier.
    *position = ModelPool::QualifiedScalar;
    *(position + 1) = 0;
    auto restored = std::make_shared<ModelPool>();
    restored->setSchemaResolver([pool = f.pool](auto) { return pool; });
    REQUIRE_FALSE(restored->read(bytes));
    restored = std::make_shared<ModelPool>();
    restored->setSchemaResolver([pool = TypedFixture("wrong-graph").pool](auto) { return pool; });
    REQUIRE_FALSE(restored->read(TypedFixture::bytes(*qualified)));

    // A matching resolver key does not replace structural validation of the wire rows.
    auto incompatible = [&](TypeId changed, SchemaType::Kind replacement) {
        auto pool = std::make_shared<SchemaPool>();
        std::array kinds{SchemaType::Kind::Object, SchemaType::Kind::Int, SchemaType::Kind::Int,
                         SchemaType::Kind::Bitmask, SchemaType::Kind::Array};
        for (TypeId id = 1; id <= kinds.size(); ++id) {
            auto kind = id == changed ? replacement : kinds[id - 1];
            if (kind == SchemaType::Kind::Bitmask)
                pool->newBitmaskType();
            else
                pool->newType(kind);
        }
        pool->finalize(std::string(f.pool->identity()));
        return pool;
    };
    for (auto kind : {SchemaType::Kind::Null, SchemaType::Kind::Bytes}) {
        restored = std::make_shared<ModelPool>();
        restored->setSchemaResolver([pool = incompatible(2, kind)](auto) { return pool; });
        REQUIRE_FALSE(restored->read(TypedFixture::bytes(*qualified)));
    }
    for (auto id : {TypeId{1}, TypeId{5}}) {
        restored = std::make_shared<ModelPool>();
        restored->setSchemaPool(incompatible(id, SchemaType::Kind::Int));
        REQUIRE_FALSE(restored->read(TypedFixture::bytes(*f.model)));
    }
}

TEST_CASE("Bound wildcard plans separate graphs and schema dictionary namespaces", "[model.schema-value]")
{
    TypedFixture one;
    TypedFixture two("second-graph", 0, [](SchemaPool& pool) {
        pool.schemaType(1).asObject().addField("unique", pool.schemaType(2));
    }, 0);
    REQUIRE(two.root->addField("unique", std::int64_t{9}));
    auto strings = std::make_shared<StringPool>(*one.model->strings());
    REQUIRE(strings->emplace("unique"));
    auto output = std::make_shared<ModelPool>(strings);
    std::vector<Value> values{Value::field(one.root), Value::field(two.root)};
    auto sequence = output->copySequence(values);
    REQUIRE(sequence);
    Environment env(strings);
    // A deliberately wrong default root must not override a concrete model's exact pool.
    env.setSchema(one.pool->schemaType(1));
    for (auto plans : {false, true}) {
        env.enableWildcardFieldPlans = plans;
        auto ast = compile(env, "**.unique", false);
        REQUIRE(ast);
        auto result = eval(env, **ast, **sequence, nullptr);
        REQUIRE(result);
        REQUIRE(result->size() == 1);
        REQUIRE(result->front().as<ValueType::Int>() == 9);
        REQUIRE(result->front().schemaType() == two.pool->schemaType(2));
    }
}

TEST_CASE("Symbol alternatives at one path use concrete provenance and survive traces", "[model.schema-value][schema.symbol]")
{
    TypedFixture f("symbol-alternatives", 0, [](SchemaPool& pool) {
        auto second = pool.newObjectType();
        auto other = pool.newScalarType(SchemaType::Kind::Int);
        REQUIRE(second.id() == 6);
        REQUIRE(other.id() == 7);
        other.addNamedValue({"SPEED_UNLIMITED", std::int64_t{0}, SchemaType::NamedValue::Role::Special});
        second.addField("speed", other);
        auto alternatives = pool.newCompositionType(SchemaType::Composition::AnyOf);
        alternatives.addAlternative(pool.schemaType(1));
        alternatives.addAlternative(second);
        REQUIRE(alternatives.id() == 8);
    });
    Environment compiler(std::make_shared<StringPool>(*f.strings));
    compiler.setSchema(f.pool->schemaType(8));
    auto ast = compile(compiler, "speed = SPEED_UNLIMITED",
        CompileOptions{.any = false, .rewriteMode = RewriteMode::Schema, .rootSchema = f.pool->schemaType(8)});
    REQUIRE(ast);
    Environment runtime(f.model->strings());
    for (auto [typeRef, raw, expected] : {std::tuple{1, 255, true}, {6, 255, false}, {6, 0, true}}) {
        auto root = f.model->newObject();
        REQUIRE(root->setSchema(TypeId(typeRef)));
        REQUIRE(root->addField("speed", std::int64_t(raw)));
        auto values = eval(runtime, **ast, *root, nullptr);
        REQUIRE(values);
        REQUIRE(values->front().as<ValueType::Bool>() == expected);
    }
    auto unknown = f.model->newObject();
    REQUIRE(unknown->addField("speed", std::int64_t{255}));
    REQUIRE_FALSE(eval(runtime, **ast, *unknown, nullptr));

    auto traced = compile(runtime, "trace(speed, 1, 'selected')", false);
    REQUIRE(traced);
    REQUIRE(eval(runtime, **traced, *f.root, nullptr));
    auto results = std::make_shared<ModelPool>();
    auto copy = results->copySequence(runtime.traces.at("selected").values);
    REQUIRE(copy);
    REQUIRE((*copy)->at(0)->schemaType() == f.pool->schemaType(2));
    REQUIRE((*copy)->at(0)->toJson() == 255);
}

TEST_CASE("Narrow masks retain unknown bits and constrain complement and mixing", "[model.schema-value][bitmask]")
{
    TypedFixture f("narrow-mask", 0, [](SchemaPool& pool) {
        for (size_t i = 0; i < 3; ++i)
            pool.newType(SchemaType::Kind::Unknown);
        auto type = pool.newBitmaskType(8);
        REQUIRE(type.id() == 9);
        type.addFlag("PAIR", 3);
    });
    auto narrow = f.model->newBitmask(0x81, f.pool->schemaType(9));
    REQUIRE(f.root->addField("narrow", narrow));
    for (auto [query, expected] : {std::pair{"~narrow", 0x7e}, {"narrow >> 7", 1}, {"narrow << 7", 0x80}}) {
        INFO(query);
        auto values = f.query(query);
        REQUIRE(values);
        REQUIRE(values->front().as<ValueType::Bitmask>().bits == expected);
        REQUIRE(values->front().schemaType() == f.pool->schemaType(9));
    }
    for (auto query : {"narrow << 8", "narrow | 256", "narrow & flags", "narrow = flags", "narrow as float"}) {
        INFO(query);
        REQUIRE_FALSE(f.query(query));
    }
    REQUIRE(f.query("narrow != null")->front().as<ValueType::Bool>());
    REQUIRE_FALSE(f.query("null = narrow")->front().as<ValueType::Bool>());
    REQUIRE(f.query("bitmask(narrow, '0')?")->front().as<ValueType::Bool>());
    REQUIRE_THROWS(f.model->newBitmask(256, f.pool->schemaType(9)));
    auto tag = narrow->toJson();
    tag["bits"] = "0x100";
    REQUIRE_FALSE(json::buildModelNode(tag, *f.model));
    tag["bits"] = "-1";
    REQUIRE_FALSE(json::buildModelNode(tag, *f.model));

    for (auto kind : {SchemaType::Kind::Unknown, SchemaType::Kind::Any, SchemaType::Kind::Value}) {
        auto pool = std::make_shared<SchemaPool>();
        auto generic = pool->newType(kind);
        pool->finalize("generic-mask-kind");
        REQUIRE_FALSE(generic.isBitmask());
        REQUIRE_FALSE(generic.modelNode()->toJson().contains("bitWidth"));
        REQUIRE_THROWS(f.model->newBitmask(1, generic));
    }
}

TEST_CASE("Explicit symbol membership survives predicate composition",
          "[model.schema-value][search-preparation]") {
  TypedFixture fixture;
  for (auto expression :
       {"hasSymbol(_, 'SPEED_UNLIMITED')",
        "any(hasSymbol(_, 'SPEED_UNLIMITED'))",
        "true and hasSymbol(speed, 'SPEED_UNLIMITED')",
        "hasSymbol(_, 'SPEED_UNLIMITED', 'test-graph-v1', 2)"}) {
    CAPTURE(expression);
    auto result = fixture.query(expression);
    REQUIRE(result);
    REQUIRE(result->size() == 1);
    CHECK(result->front().as<ValueType::Bool>());
  }
  // An equal untyped number and an unknown enum name are never matches.
  for (auto expression :
       {"hasSymbol(count, 'SPEED_UNLIMITED')", "hasSymbol(_, 'MISSING')",
        "hasSymbol(_, 'SPEED_UNLIMITED', 'different-graph', 2)",
        "hasSymbol(_, 'SPEED_UNLIMITED', 'test-graph-v1', 3)"}) {
    auto result = fixture.query(expression);
    REQUIRE(result);
    REQUIRE(result->size() == 1);
    CHECK_FALSE(result->front().as<ValueType::Bool>());
  }
}

TEST_CASE(
    "Explicit symbol membership finds typed descendants below an untyped root",
    "[search-preparation]") {
  TypedFixture fixture;
  REQUIRE(fixture.root->setSchema(NoTypeId));
  auto result =
      fixture.query("hasSymbol(_, 'SPEED_UNLIMITED', 'test-graph-v1', 2)");
  REQUIRE(result);
  REQUIRE(result->size() == 1);
  CHECK(result->front().as<ValueType::Bool>());
}

TEST_CASE("Predicate symbol discovery preserves wrappers and excludes "
          "comparison literals",
          "[search-preparation]") {
  Environment env(std::make_shared<StringPool>());
  for (auto expression :
       {"ANIMALS", "any(ANIMALS)", "any(\"ANIMALS\")", "not ANIMALS"}) {
    auto symbols = predicateQuerySymbols(env, expression);
    REQUIRE(symbols);
    REQUIRE(symbols->size() == 1);
    CHECK(symbols->front().name == "ANIMALS");
    auto location = symbols->front().location;
    CHECK(std::string_view(expression)
              .substr(location.offset, location.size)
              .find("ANIMALS") != std::string_view::npos);
  }
  auto explicitQuery =
      predicateQuerySymbols(env, "**.warningSign == \"ANIMALS\"");
  REQUIRE(explicitQuery);
  CHECK(explicitQuery->empty());
}
