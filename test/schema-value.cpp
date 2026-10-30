#include "common.hpp"
#include "simfil/model/schema-model.h"

#include <map>
#include <sstream>

namespace
{
/** Exercise the public borrowed-domain adapter rather than duplicating MapSchema storage in tests. */
class ScalarDomain : public ValueSchema
{
public:
    explicit ScalarDomain(Kind kind) : ValueSchema(kind) {}
    std::vector<NamedValue> names;
    std::optional<NumericDomain> numeric;
    std::uint8_t width = 64;
    void forEachNamedValue(const std::function<void(NamedValue const&)>& fn) const override
    {
        for (auto const& name : names)
            fn(name);
    }
    auto numericDomain() const -> std::optional<NumericDomain> override { return numeric; }
    auto bitWidth() const -> std::uint8_t override { return width; }
};

/** Reproduce derived model readers that deserialize before shared ownership is established. */
class ConstructorReadPool final : public ModelPool
{
public:
    /** Install the exact graph resolver and read without constructing any owning nodes. */
    ConstructorReadPool(std::vector<std::uint8_t> const& bytes,
                        std::shared_ptr<const SchemaBinding> binding, std::shared_ptr<StringPool> strings)
        : ModelPool(std::move(strings))
    {
        setSchemaResolver([binding = std::move(binding)](auto identity) {
            return binding->identity() == identity ? binding : nullptr;
        });
        loaded = read(bytes);
    }

    tl::expected<void, Error> loaded;
};

/** Own graph definitions independently from runtime field ids and source model lifetime. */
struct TypedFixture
{
    using Graph = std::map<SchemaId, std::unique_ptr<Schema>>;
    std::shared_ptr<Graph> graph = std::make_shared<Graph>();
    std::shared_ptr<StringPool> strings = std::make_shared<StringPool>();
    std::shared_ptr<const SchemaBinding> binding;
    std::shared_ptr<ModelPool> model = std::make_shared<ModelPool>();
    model_ptr<Object> root;

    /** Use a deliberately different runtime dictionary; both fields share one inline address. */
    explicit TypedFixture(std::string identity = "test-graph-v1")
    {
        auto object = std::make_unique<ObjectSchema>();
        for (auto [name, id] : {std::pair{"speed", 2}, {"count", 3}, {"flags", 4}, {"items", 5}})
            object->addField(strings->emplace(name).value(), {SchemaId(id)});
        (*graph)[1] = std::move(object);
        auto speed = std::make_unique<ScalarDomain>(Schema::Kind::Int);
        speed->names = {{"SPEED_UNLIMITED", std::int64_t{255}, Schema::NamedValue::Role::Special,
                        Schema::NamedValue::Meaning::Unbounded}};
        speed->numeric = Schema::NumericDomain{.encoding = "uint8", .unit = "km/h",
            .minimum = std::int64_t{0}, .maximum = std::int64_t{255}, .complete = true};
        (*graph)[2] = std::move(speed);
        (*graph)[3] = std::make_unique<ValueSchema>(Schema::Kind::Int);
        auto flags = std::make_unique<ScalarDomain>(Schema::Kind::Bitmask);
        flags->names = {{"NONE", std::uint64_t{0}, Schema::NamedValue::Role::Flag},
            {"PAIR", std::uint64_t{3}, Schema::NamedValue::Role::Flag},
            {"HIGH", std::uint64_t{1} << 63, Schema::NamedValue::Role::Flag},
            {"ALIAS", std::uint64_t{3}, Schema::NamedValue::Role::Flag}};
        (*graph)[4] = std::move(flags);
        auto array = std::make_unique<ArraySchema>();
        array->addElementSchemas({2});
        (*graph)[5] = std::move(array);
        for (auto& [id, schema] : *graph)
            schema->finalize([this](SchemaId child) { return graph->contains(child) ? graph->at(child).get() : nullptr; });
        binding = std::make_shared<SchemaBinding>(std::move(identity), graph,
            [graph = graph](SchemaId id) { return graph->contains(id) ? graph->at(id).get() : nullptr; }, strings);
        REQUIRE(model->strings()->emplace("runtime-padding"));
        model->setSchemaBinding(binding);
        root = model->newObject();
        REQUIRE(root->setSchema(1));
        REQUIRE(root->addField("speed", std::int64_t{255}));
        REQUIRE(root->addField("count", std::int64_t{255}));
        REQUIRE(root->addField("flags", model->newBitmask((std::uint64_t{1} << 63) | 3, {binding, 4})));
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

    /** Compile against schema-owned names, then bind the portable AST to the separate runtime namespace. */
    auto schemaQuery(std::string_view text) -> tl::expected<std::vector<Value>, Error>
    {
        Environment compiler(strings);
        compiler.querySchemaCallback = [binding = binding](SchemaId id) { return binding->lookup(id); };
        auto ast = compile(compiler, text, CompileOptions{.any = false, .rewriteMode = RewriteMode::Schema, .rootSchema = 1});
        if (!ast)
            return tl::unexpected(ast.error());
        Environment runtime(model->strings());
        return eval(runtime, **ast, *root, nullptr);
    }

    /** Serialize the native storage, including common exact graph bindings. */
    static auto bytes(ModelPool& source) -> std::vector<std::uint8_t>
    {
        std::stringstream out;
        REQUIRE(source.write(out));
        auto text = out.str();
        return {text.begin(), text.end()};
    }
};
}

TEST_CASE("Scalar domains survive selection but not numerical construction", "[model.schema-value]")
{
    TypedFixture f;
    REQUIRE(f.root->at(0)->addr() == f.root->at(1)->addr());
    REQUIRE(f.root->at(0)->schema() == NoSchemaId);
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
        REQUIRE(result->front().schemaReference() == SchemaReference{f.binding, 2});
    }
    for (auto text : {"speed + 0", "speed * 1", "speed as int", "speed = 255", "count(speed)", "sum(speed)", "255"}) {
        INFO(text);
        auto result = f.query(text);
        REQUIRE(result);
        REQUIRE(result->size() == 1);
        REQUIRE_FALSE(result->front().schemaReference());
    }
    REQUIRE(f.query("min(count, speed)")->front().schemaReference() == SchemaReference{f.binding, 3});
}

TEST_CASE("Native copies keep compact compounds and qualify detached occurrences", "[model.schema-value]")
{
    TypedFixture a;
    TypedFixture b("test-graph-v2");
    auto destination = std::make_shared<ModelPool>(std::make_shared<StringPool>(*a.model->strings()));
    auto compound = destination->copyNode(*a.root);
    REQUIRE(compound);
    REQUIRE((*compound)->schemaReference() == SchemaReference{a.binding, 1});
    REQUIRE((*compound)->at(0)->schema() == NoSchemaId);
    REQUIRE((*compound)->at(0)->addr().column() != ModelPool::QualifiedScalar);
    auto values = std::vector<Value>{a.query("speed")->front(), a.query("count")->front(), b.query("speed")->front()};
    auto copied = destination->copySequence(values);
    REQUIRE(copied);
    destination->addRoot(*copied);
    REQUIRE((*copied)->at(0)->addr() != (*copied)->at(1)->addr());
    REQUIRE((*copied)->at(0)->schemaReference() == values[0].schemaReference());
    REQUIRE((*copied)->at(2)->schemaReference() == values[2].schemaReference());
    REQUIRE(destination->serializationSizeStats().qualifiedScalarBytes == 3 * (sizeof(ModelNodeAddress) + sizeof(SchemaId)));
    auto bytes = TypedFixture::bytes(*destination);
    auto restored = std::make_shared<ModelPool>(destination->strings());
    REQUIRE_FALSE(restored->read(bytes));
    restored->setSchemaResolver([one = a.binding, two = b.binding](std::string_view identity) {
        return identity == one->identity() ? one : identity == two->identity() ? two : nullptr;
    });
    REQUIRE(restored->read(bytes));
    auto root = restored->root(0).value();
    REQUIRE(root->at(0)->schemaReference() == SchemaReference{a.binding, 2});
    REQUIRE(root->at(2)->schemaReference() == SchemaReference{b.binding, 2});
    a.root = {}; a.model.reset(); a.graph.reset(); a.binding.reset();
    b.root = {}; b.model.reset(); b.graph.reset(); b.binding.reset();
    destination.reset(); values.clear();
    REQUIRE(std::get<int64_t>(root->at(0)->value()) == 255);
    REQUIRE(root->at(0)->schemaReference().definition()->namedValue("SPEED_UNLIMITED"));
    REQUIRE(restored->validate());
}

TEST_CASE("Open objects retain declared scalar domains without guessing unknown alternatives", "[model.schema-value]")
{
    TypedFixture f;
    f.graph->at(1)->setOpen(true);
    auto lookup = [&](SchemaId id) { return f.binding->lookup(id); };
    auto speed = f.strings->get("speed");
    auto extra = f.strings->emplace("extra").value();
    REQUIRE(f.root->addField("extra", std::int64_t{255}));
    REQUIRE(Schema::fieldSchemas(1, lookup, speed) == std::vector<SchemaId>{2});
    REQUIRE(Schema::fieldSchemas(1, lookup, extra) == std::vector<SchemaId>{NoSchemaId});
    for (auto query : {"speed", "_['speed']", "**.speed"}) {
        INFO(query);
        auto values = f.query(query);
        REQUIRE(values);
        REQUIRE(values->front().schemaReference() == SchemaReference{f.binding, 2});
    }
    REQUIRE_FALSE(f.query("extra")->front().schemaReference());
    REQUIRE(f.schemaQuery("speed = SPEED_UNLIMITED")->front().as<ValueType::Bool>());

    auto destination = std::make_shared<ModelPool>(std::make_shared<StringPool>(*f.model->strings()));
    auto compound = destination->copyNode(*f.root);
    REQUIRE(compound);
    REQUIRE((*compound)->at(0)->schema() == NoSchemaId);
    REQUIRE(Value::at(**compound, *(*compound)->at(0), 0).schemaReference() == SchemaReference{f.binding, 2});
    auto scalar = destination->copySequence(*f.query("speed"));
    REQUIRE(scalar);
    REQUIRE((*scalar)->at(0)->schemaReference() == SchemaReference{f.binding, 2});

    auto other = std::make_unique<ObjectSchema>();
    other->setOpen(true);
    (*f.graph)[6] = std::move(other);
    auto alternatives = std::make_unique<CombinedSchema>(Schema::Composition::AnyOf);
    alternatives->addAlternative(1);
    alternatives->addAlternative(6);
    (*f.graph)[7] = std::move(alternatives);
    REQUIRE(f.root->setSchema(7));
    REQUIRE(Schema::fieldSchemas(7, lookup, speed) == std::vector<SchemaId>{NoSchemaId, 2});
    REQUIRE_FALSE(f.query("speed")->front().schemaReference());
    // Differently declared alternatives are still ambiguous, even with identical raw values.
    static_cast<ObjectSchema&>(*f.graph->at(6)).addField(speed, {3});
    REQUIRE(Schema::fieldSchemas(7, lookup, speed) == std::vector<SchemaId>{2, 3});
    REQUIRE_FALSE(f.query("speed")->front().schemaReference());
    auto matching = std::make_unique<ObjectSchema>();
    matching->setOpen(true);
    matching->addField(speed, {2});
    (*f.graph)[6] = std::move(matching);
    REQUIRE(Schema::fieldSchemas(7, lookup, speed) == std::vector<SchemaId>{2});
    REQUIRE(f.query("speed")->front().schemaReference() == SchemaReference{f.binding, 2});
}

TEST_CASE("Bitmasks preserve high and unknown bits through query JSON and binary", "[model.schema-value][bitmask]")
{
    TypedFixture f;
    auto mask = f.root->at(2);
    REQUIRE(mask->type() == ValueType::Bitmask);
    REQUIRE(f.model->serializationSizeStats().bitmaskBytes == sizeof(std::uint64_t) + sizeof(SchemaId));
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
        {"schema", "test-graph-v1"}, {"domain", 4}};
    REQUIRE(mask->toJson() == golden);
    auto imported = json::buildModelNode(golden, *f.model);
    REQUIRE(imported);
    REQUIRE((*imported)->toJson() == golden);
    auto unrelated = std::make_shared<ModelPool>();
    REQUIRE_FALSE(json::buildModelNode(golden, *unrelated));
    golden["application"] = true;
    REQUIRE(json::buildModelNode(golden, *unrelated).value()->type() == ValueType::Object);
    auto restored = std::make_shared<ModelPool>(f.model->strings());
    restored->setSchemaBinding(f.binding);
    auto read = restored->read(TypedFixture::bytes(*f.model));
    INFO((read ? "success" : read.error().message));
    REQUIRE(read);
    REQUIRE(restored->root(0).value()->at(2)->toJson() == mask->toJson());
    REQUIRE_THROWS(f.model->newBitmask(0, {f.binding, 2}));
    auto other = TypedFixture("foreign");
    REQUIRE_THROWS(f.model->newQualifiedValue(std::get<Bitmask>(mask->value()), {other.binding, 4}));
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
    REQUIRE_THROWS(f.model->newQualifiedValue(int64_t{3}, {f.binding, 2}));
    REQUIRE_THROWS(f.model->newBitmask(3, {f.binding, 4}));
    REQUIRE_FALSE(f.model->copyNode(*f.root));
    REQUIRE_FALSE(json::buildModelNode(nlohmann::json(2.5), *f.model));
    REQUIRE_FALSE(json::buildModelNode(f.root->at(2)->toJson(), *f.model));
    REQUIRE_THROWS(f.model->addRoot(f.root));
    REQUIRE_THROWS(f.model->clear());
    REQUIRE_THROWS(f.model->setSchemaBinding(f.binding));
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

TEST_CASE("Qualified string copying preserves aliased storage and exact domains", "[model.schema-value][model.growth]")
{
    TypedFixture f;
    (*f.graph)[6] = std::make_unique<ValueSchema>(Schema::Kind::String);
    std::string text(65536, 'x');
    text.front() = 'a';
    text.back() = 'z';
    auto original = f.model->tryNewValue(std::string_view(text));
    REQUIRE(original);
    // Qualification appends into the same byte buffer that owns this borrowed view.
    auto qualified = f.model->newQualifiedValue(std::get<std::string_view>((*original)->value()), {f.binding, 6});
    auto copied = f.model->copyNode(*qualified);
    REQUIRE(copied);
    REQUIRE((*copied)->schemaReference() == SchemaReference{f.binding, 6});
    REQUIRE(std::get<std::string_view>((*original)->value()) == text);
    REQUIRE(std::get<std::string_view>(qualified->value()) == text);
    REQUIRE(std::get<std::string_view>((*copied)->value()) == text);

    auto target = std::make_shared<ModelPool>();
    auto detached = target->copyNode(**copied);
    REQUIRE(detached);
    target->addRoot(*detached);
    auto restored = std::make_shared<ConstructorReadPool>(TypedFixture::bytes(*target), f.binding, target->strings());
    REQUIRE(restored->loaded);
    auto node = restored->root(0).value();
    REQUIRE(node->schemaReference() == SchemaReference{f.binding, 6});
    REQUIRE(std::get<std::string_view>(node->value()) == text);
    REQUIRE(restored->validate());

    auto before = TypedFixture::bytes(*target);
    REQUIRE_THROWS_AS(target->newQualifiedValue(std::string_view(text), {f.binding, 2}), std::invalid_argument);
    REQUIRE_THROWS_AS(target->newQualifiedValue(ScalarValueType{}, {f.binding, 6}), std::invalid_argument);
    REQUIRE_THROWS_AS(target->newBitmask(1, {f.binding, 6}), std::invalid_argument);
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

TEST_CASE("Named queries use the actual numeric or flag domain rather than common raw codes", "[model.schema-value][schema.symbol]")
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

    auto& other = static_cast<ScalarDomain&>(*f.graph->at(2));
    other.names.front().value = std::int64_t{0};
    REQUIRE_FALSE(f.schemaQuery("speed = SPEED_UNLIMITED")->front().as<ValueType::Bool>());
    REQUIRE_FALSE(f.schemaQuery("SPEED_UNLIMITED")->front().as<ValueType::Bool>());
    static_cast<ObjectSchema&>(*f.graph->at(1)).setOpen(true);
    REQUIRE_FALSE(f.schemaQuery("SPEED_UNLIMITED")->front().as<ValueType::Bool>());
    other.names.front().value = std::int64_t{255};
    REQUIRE(f.schemaQuery("SPEED_UNLIMITED")->front().as<ValueType::Bool>());
}

TEST_CASE("Open string enum traversal retains the owning dictionary across unbound descendants", "[model.schema-value][schema.symbol]")
{
    TypedFixture f;
    auto& root = static_cast<ObjectSchema&>(*f.graph->at(1));
    root.setOpen(true);
    auto symbol = f.strings->emplace("LEGACY_ENUM").value();
    auto values = std::make_unique<ValueSchema>(Schema::Kind::String);
    values->addEnumSymbol(symbol);
    (*f.graph)[6] = std::move(values);
    auto nested = std::make_unique<ObjectSchema>();
    nested->addField(f.strings->emplace("text").value(), {6});
    (*f.graph)[7] = std::move(nested);
    root.addField(f.strings->emplace("nested").value(), {7});
    auto object = f.model->newObject();
    REQUIRE(object->addField("text", "LEGACY_ENUM"));
    REQUIRE(f.root->addField("nested", object));
    REQUIRE(f.model->strings()->get("LEGACY_ENUM") == StringPool::Empty);
    for (auto& [id, schema] : *f.graph)
        schema->finalize([&](SchemaId child) { return f.graph->contains(child) ? f.graph->at(child).get() : nullptr; });
    for (auto bindChild : {false, true}) {
        if (bindChild)
            REQUIRE(object->setSchema(7));
        auto result = f.schemaQuery("LEGACY_ENUM");
        REQUIRE(result);
        REQUIRE(result->size() == 1);
        REQUIRE(result->front().as<ValueType::Bool>());
    }
    REQUIRE(f.model->strings()->get("LEGACY_ENUM") == StringPool::Empty);
    // An undeclared field must not borrow the schema of the genuinely empty field name.
    root.addField(StringPool::Empty, {6});
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
    auto descriptors = std::make_shared<SchemaModel>(f.strings,
        [binding = f.binding](SchemaId id) { return binding->lookup(id); });
    auto before = f.strings->size();
    auto json = descriptors->root(1)->toJson();
    REQUIRE(json["fields"]["speed"]["numericDomain"]["unit"] == "km/h");
    REQUIRE(json["fields"]["speed"]["namedValues"][0]["value"] == 255);
    REQUIRE(json["fields"]["speed"]["namedValues"][0]["meaning"] == "unbounded");
    REQUIRE(json["fields"]["flags"]["bitWidth"] == 64);
    REQUIRE(json["fields"]["flags"]["namedValues"][2]["value"] == "0x8000000000000000");
    REQUIRE(f.strings->size() == before);
    Environment env(f.strings);
    env.querySchemaCallback = [binding = f.binding](SchemaId id) { return binding->lookup(id); };
    for (auto query : {"SPEED_", "speed = SPEED_", "flags = HI"}) {
        INFO(query);
        auto candidates = complete(env, query, std::string_view(query).size(), SchemaId{1});
        REQUIRE(candidates);
        auto wanted = std::string_view(query) == "flags = HI" ? "HIGH" : "SPEED_UNLIMITED";
        REQUIRE(std::ranges::any_of(*candidates, [&](auto const& candidate) { return candidate.text == wanted; }));
    }
    auto candidates = complete(env, "count = SPEED_", 14, SchemaId{1});
    REQUIRE(candidates);
    REQUIRE_FALSE(std::ranges::any_of(*candidates, [](auto const& candidate) { return candidate.text == "SPEED_UNLIMITED"; }));
    auto quoted = std::string("speed = 'SPEED_");
    candidates = complete(env, quoted, quoted.size(), SchemaId{1});
    REQUIRE(candidates);
    REQUIRE_FALSE(std::ranges::any_of(*candidates, [](auto const& candidate) {
        return candidate.text.find("SPEED_UNLIMITED") != std::string::npos;
    }));
    auto flag = std::string("hasFlag(flags, 'PA");
    candidates = complete(env, flag, flag.size(), SchemaId{1});
    REQUIRE(candidates);
    REQUIRE(std::ranges::any_of(*candidates, [](auto const& candidate) { return candidate.text == "\"PAIR\""; }));
}

TEST_CASE("Qualified scalar validation is safe in a model constructor", "[model.schema-value][limits]")
{
    TypedFixture f;
    (*f.graph)[6] = std::make_unique<ValueSchema>(Schema::Kind::Bool);
    (*f.graph)[7] = std::make_unique<ValueSchema>(Schema::Kind::Float);
    (*f.graph)[8] = std::make_unique<ValueSchema>(Schema::Kind::String);
    (*f.graph)[9] = std::make_unique<ValueSchema>(Schema::Kind::Bytes);
    std::vector<std::pair<ScalarValueType, SchemaId>> values{
        {std::int64_t{255}, 2}, {std::int64_t{-1}, 2}, {std::int64_t{65536}, 2},
        {true, 6}, {2.5, 7}, {std::string("qualified text"), 8}, {ByteArray("raw bytes"), 9}};
    for (auto direct : {false, true}) {
        auto source = std::make_shared<ModelPool>();
        if (direct)
            source->setSchemaBinding(f.binding);
        for (auto const& [raw, domain] : values)
            source->addRoot(source->newQualifiedValue(raw, {f.binding, domain}));
        auto restored = std::make_shared<ConstructorReadPool>(TypedFixture::bytes(*source), f.binding, source->strings());
        REQUIRE(restored->loaded);
        REQUIRE(restored->numRoots() == values.size());
        for (std::size_t i = 0; i < values.size(); ++i) {
            auto node = restored->root(i).value();
            REQUIRE(node->type() == Value(ScalarValueType(values[i].first)).type);
            REQUIRE(node->toJson() == source->root(i).value()->toJson());
            REQUIRE(node->schemaReference() == SchemaReference{f.binding, values[i].second});
        }
    }

    auto source = std::make_shared<ModelPool>();
    source->addRoot(source->newQualifiedValue(std::int64_t{255}, {f.binding, 2}));
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
        auto restored = std::make_shared<ConstructorReadPool>(malformed, f.binding, source->strings());
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
    REQUIRE(f.model->hasDirectSchemaBinding("test-graph-v1"));
    REQUIRE_FALSE(f.model->hasDirectSchemaBinding("foreign"));
    auto qualified = std::make_shared<ModelPool>();
    auto node = qualified->newQualifiedValue(std::int64_t{255}, {f.binding, 2});
    qualified->addRoot(node);
    REQUIRE_FALSE(qualified->hasDirectSchemaBinding(f.binding->identity()));
    auto bytes = TypedFixture::bytes(*qualified);
    std::array<std::uint8_t, 4> raw{Model::UInt16, 255, 0, 0};
    auto position = std::search(bytes.begin(), bytes.end(), raw.begin(), raw.end());
    REQUIRE(position != bytes.end());
    // A qualifier may only point directly to primitive storage, never itself or another qualifier.
    *position = ModelPool::QualifiedScalar;
    *(position + 1) = 0;
    auto restored = std::make_shared<ModelPool>();
    restored->setSchemaResolver([binding = f.binding](auto) { return binding; });
    REQUIRE_FALSE(restored->read(bytes));
    restored = std::make_shared<ModelPool>();
    restored->setSchemaResolver([binding = TypedFixture("wrong-graph").binding](auto) { return binding; });
    REQUIRE_FALSE(restored->read(TypedFixture::bytes(*qualified)));

    // A resolver's token must identify the same graph, including structural affinities.
    for (auto kind : {Schema::Kind::Null, Schema::Kind::Bytes}) {
        static_cast<ValueSchema&>(*f.graph->at(2)).setKind(kind);
        restored = std::make_shared<ModelPool>();
        restored->setSchemaResolver([binding = f.binding](auto) { return binding; });
        REQUIRE_FALSE(restored->read(TypedFixture::bytes(*qualified)));
    }
    for (auto id : {SchemaId{1}, SchemaId{5}}) {
        f.graph->at(id)->setKind(Schema::Kind::Int);
        restored = std::make_shared<ModelPool>();
        restored->setSchemaBinding(f.binding);
        REQUIRE_FALSE(restored->read(TypedFixture::bytes(*f.model)));
        f.graph->at(id)->setKind(id == 1 ? Schema::Kind::Object : Schema::Kind::Array);
    }
}

TEST_CASE("Bound wildcard plans separate graphs and schema dictionary namespaces", "[model.schema-value]")
{
    TypedFixture one;
    TypedFixture two("second-graph");
    auto& root = static_cast<ObjectSchema&>(*two.graph->at(1));
    auto& speed = static_cast<ScalarDomain&>(*two.graph->at(2));
    speed.names.front().value = std::int64_t{0};
    root.addField(two.strings->emplace("unique").value(), {2});
    root.finalize([&](SchemaId id) { return two.graph->contains(id) ? two.graph->at(id).get() : nullptr; });
    REQUIRE(two.root->addField("unique", std::int64_t{9}));
    auto strings = std::make_shared<StringPool>(*one.model->strings());
    REQUIRE(strings->emplace("unique"));
    auto output = std::make_shared<ModelPool>(strings);
    std::vector<Value> values{Value::field(one.root), Value::field(two.root)};
    auto sequence = output->copySequence(values);
    REQUIRE(sequence);
    Environment env(strings);
    // Deliberately wrong callback: concrete model bindings must take precedence over bare numeric ids.
    env.querySchemaCallback = [binding = one.binding](SchemaId id) { return binding->lookup(id); };
    for (auto plans : {false, true}) {
        env.enableWildcardFieldPlans = plans;
        auto ast = compile(env, "**.unique", false);
        REQUIRE(ast);
        auto result = eval(env, **ast, **sequence, nullptr);
        REQUIRE(result);
        REQUIRE(result->size() == 1);
        REQUIRE(result->front().as<ValueType::Int>() == 9);
        REQUIRE(result->front().schemaReference() == SchemaReference{two.binding, 2});
    }
}

TEST_CASE("Symbol alternatives at one path use concrete provenance and survive traces", "[model.schema-value][schema.symbol]")
{
    TypedFixture f;
    auto other = std::make_unique<ScalarDomain>(Schema::Kind::Int);
    other->names = {{"SPEED_UNLIMITED", std::int64_t{0}, Schema::NamedValue::Role::Special}};
    (*f.graph)[7] = std::move(other);
    auto second = std::make_unique<ObjectSchema>();
    second->addField(f.strings->get("speed"), {7});
    (*f.graph)[6] = std::move(second);
    auto alternatives = std::make_unique<CombinedSchema>(Schema::Composition::AnyOf);
    alternatives->addAlternative(1);
    alternatives->addAlternative(6);
    (*f.graph)[8] = std::move(alternatives);
    for (auto& [id, schema] : *f.graph)
        schema->finalize([&](SchemaId child) { return f.graph->contains(child) ? f.graph->at(child).get() : nullptr; });
    Environment compiler(f.strings);
    compiler.querySchemaCallback = [binding = f.binding](SchemaId id) { return binding->lookup(id); };
    auto ast = compile(compiler, "speed = SPEED_UNLIMITED",
        CompileOptions{.any = false, .rewriteMode = RewriteMode::Schema, .rootSchema = 8});
    REQUIRE(ast);
    Environment runtime(f.model->strings());
    for (auto [domain, raw, expected] : {std::tuple{1, 255, true}, {6, 255, false}, {6, 0, true}}) {
        auto root = f.model->newObject();
        REQUIRE(root->setSchema(SchemaId(domain)));
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
    REQUIRE((*copy)->at(0)->schemaReference() == SchemaReference{f.binding, 2});
    REQUIRE((*copy)->at(0)->toJson() == 255);
}

TEST_CASE("Narrow masks retain unknown bits and constrain complement and mixing", "[model.schema-value][bitmask]")
{
    TypedFixture f;
    auto domain = std::make_unique<ScalarDomain>(Schema::Kind::Bitmask);
    domain->width = 8;
    domain->names = {{"PAIR", std::uint64_t{3}, Schema::NamedValue::Role::Flag}};
    (*f.graph)[9] = std::move(domain);
    auto narrow = f.model->newBitmask(0x81, {f.binding, 9});
    REQUIRE(f.root->addField("narrow", narrow));
    for (auto [query, expected] : {std::pair{"~narrow", 0x7e}, {"narrow >> 7", 1}, {"narrow << 7", 0x80}}) {
        INFO(query);
        auto values = f.query(query);
        REQUIRE(values);
        REQUIRE(values->front().as<ValueType::Bitmask>().bits == expected);
        REQUIRE(values->front().schemaReference() == SchemaReference{f.binding, 9});
    }
    for (auto query : {"narrow << 8", "narrow | 256", "narrow & flags", "narrow = flags", "narrow as float"}) {
        INFO(query);
        REQUIRE_FALSE(f.query(query));
    }
    REQUIRE(f.query("narrow != null")->front().as<ValueType::Bool>());
    REQUIRE_FALSE(f.query("null = narrow")->front().as<ValueType::Bool>());
    REQUIRE(f.query("bitmask(narrow, '0')?")->front().as<ValueType::Bool>());
    REQUIRE_THROWS(f.model->newBitmask(256, {f.binding, 9}));
    auto tag = narrow->toJson();
    tag["bits"] = "0x100";
    REQUIRE_FALSE(json::buildModelNode(tag, *f.model));
    tag["bits"] = "-1";
    REQUIRE_FALSE(json::buildModelNode(tag, *f.model));

    auto descriptors = std::make_shared<SchemaModel>(f.strings,
        [binding = f.binding](SchemaId id) { return binding->lookup(id); });
    for (auto kind : {Schema::Kind::Unknown, Schema::Kind::Any, Schema::Kind::Value}) {
        f.graph->at(9)->setKind(kind);
        REQUIRE_FALSE(f.graph->at(9)->isBitmask());
        REQUIRE_FALSE(descriptors->root(9)->toJson().contains("bitWidth"));
        REQUIRE_THROWS(f.model->newBitmask(1, {f.binding, 9}));
    }
}
