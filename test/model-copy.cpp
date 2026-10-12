#include "common.hpp"
#include "simfil/overlay.h"

#include <sstream>

TEST_CASE("Native undefined survives value and binary protocols", "[model.copy][undefined]")
{
    auto model = std::make_shared<ModelPool>();
    auto undefined = model->newUndefined();
    REQUIRE(undefined->type() == ValueType::Undef);
    REQUIRE(Value::field(undefined).type == ValueType::Undef);
    REQUIRE(Value::field(*undefined).type == ValueType::Undef);
    REQUIRE(Value::field(std::move(*model->newUndefined())).type == ValueType::Undef);
    auto root = model->newArray();
    root->append(undefined);
    root->append(model->resolve(ModelNodeAddress{Model::Null, 1}));
    auto literal = json::buildModelNode(nlohmann::json{{"_undefined", true}}, *model);
    REQUIRE(literal);
    root->append(*literal);
    model->addRoot(root);
    REQUIRE(root->at(0)->toJson() == nlohmann::json{{"_undefined", true}});
    REQUIRE(root->at(1)->type() == ValueType::Null);
    REQUIRE(root->at(2)->type() == ValueType::Object);

    std::stringstream bytes;
    REQUIRE(model->write(bytes));
    auto buffer = bytes.str();
    auto restored = std::make_shared<ModelPool>(model->strings());
    REQUIRE(restored->read(std::vector<uint8_t>(buffer.begin(), buffer.end())));
    auto restoredRoot = restored->root(0).value();
    REQUIRE(restoredRoot->at(0)->type() == ValueType::Undef);
    REQUIRE(restoredRoot->at(1)->type() == ValueType::Null);
    REQUIRE(restoredRoot->at(2)->type() == ValueType::Object);
    REQUIRE(restored->validate());
}

TEST_CASE("Native undefined is preserved through runtime paths and bounded evaluation", "[model.copy][undefined]")
{
    auto model = std::make_shared<ModelPool>();
    auto root = model->newObject();
    auto nested = model->newObject();
    REQUIRE(nested->addField("unknown", model->newUndefined()));
    REQUIRE(nested->addField("nothing", model->resolve(ModelNodeAddress{Model::Null, 1})));
    REQUIRE(root->addField("nested", nested));
    Environment env(model->strings());
    for (auto query : {"nested.unknown", "nested.unknown.tail"}) {
        auto ast = compile(env, query, false);
        REQUIRE(ast);
        auto result = eval(env, **ast, *root, nullptr);
        REQUIRE(result);
        REQUIRE(result->size() == 1);
        REQUIRE(result->front().type == ValueType::Undef);
        std::vector<Value> bounded;
        auto summary = eval(env, **ast, *root, LambdaResultFn([&](Context, Value value) {
            bounded.push_back(std::move(value));
            return Result::Continue;
        }), EvaluationOptions{});
        REQUIRE(summary);
        REQUIRE(bounded.size() == 1);
        REQUIRE(bounded.front().type == ValueType::Undef);
    }
    auto ast = compile(env, "nested.missing", false);
    REQUIRE(ast);
    auto missing = eval(env, **ast, *root, nullptr);
    REQUIRE(missing);
    REQUIRE(missing->size() == 1);
    REQUIRE(missing->front().type == ValueType::Null);
}

TEST_CASE("Native copying preserves types and remaps existing field names", "[model.copy]")
{
    auto target = std::make_shared<ModelPool>();
    REQUIRE(target->strings()->emplace("padding"));
    REQUIRE(target->strings()->emplace("name"));
    REQUIRE(target->strings()->emplace("data"));
    auto before = target->strings()->size();
    ModelNode::Ptr copied;
    std::weak_ptr<ModelPool> lifetime;
    {
        auto source = std::make_shared<ModelPool>();
        lifetime = source;
        auto root = source->newObject();
        REQUIRE(root->setSchema(42));
        REQUIRE(root->addField("name", std::string_view("first")));
        REQUIRE(root->addField("name", std::string_view("second")));
        auto array = source->newArray();
        REQUIRE(array->setSchema(43));
        array->append(source->newUndefined());
        array->append(source->resolve(ModelNodeAddress{Model::Null, 1}));
        array->append(source->newValue(ByteArray{std::string("\0\xff", 2)}));
        array->append(source->newSmallValue(true));
        array->append(source->newValue(int64_t{-123456}));
        array->append(source->newValue(4.25));
        REQUIRE(root->addField("data", array));
        auto overlay = model_ptr<OverlayNode>::make(Value::field(root));
        auto result = target->copyNode(*overlay);
        REQUIRE(result);
        copied = *result;
        REQUIRE(copied->schema() == NoSchemaId);
        REQUIRE(copied->at(2)->schema() == NoSchemaId);
        REQUIRE(copied->keyAt(0) != root->keyAt(0));
    }
    REQUIRE(lifetime.expired());
    REQUIRE(target->strings()->size() == before);

    REQUIRE(copied->size() == 3);
    REQUIRE(copied->keyAt(0) == copied->keyAt(1));
    REQUIRE(std::get<std::string_view>(copied->at(0)->value()) == "first");
    REQUIRE(std::get<std::string_view>(copied->at(1)->value()) == "second");
    auto array = copied->at(2);
    REQUIRE(array->at(0)->type() == ValueType::Undef);
    REQUIRE(array->at(1)->type() == ValueType::Null);
    REQUIRE(std::get<ByteArray>(array->at(2)->value()).bytes == std::string("\0\xff", 2));
    REQUIRE(std::get<bool>(array->at(3)->value()));
    REQUIRE(std::get<int64_t>(array->at(4)->value()) == -123456);
    REQUIRE(std::get<double>(array->at(5)->value()) == 4.25);
    target->addRoot(copied);
    REQUIRE(target->validate());
}

TEST_CASE("Native copy rejects missing keys cycles and exhausted budgets", "[model.copy]")
{
    auto source = std::make_shared<ModelPool>();
    auto target = std::make_shared<ModelPool>();
    auto root = source->newObject();
    REQUIRE(root->addField("foreign", int64_t{1}));
    auto before = target->strings()->size();
    auto foreign = target->copyNode(*root);
    REQUIRE_FALSE(foreign);
    REQUIRE(foreign.error().message.find("dictionary") != std::string::npos);
    REQUIRE(target->strings()->size() == before);

    auto invalid = target->copyNode(*source->resolve(ModelNodeAddress{255, 1}));
    REQUIRE_FALSE(invalid);

    auto cycle = source->newArray();
    cycle->append(cycle);
    auto cyclic = target->copyNode(*cycle);
    REQUIRE_FALSE(cyclic);
    REQUIRE(cyclic.error().message.find("Cyclic") != std::string::npos);

    auto children = source->newArray();
    children->append(source->newArray());
    children->append(source->newArray());
    REQUIRE_FALSE(target->copyNode(*children, {.maxDepth = 0}));
    REQUIRE_FALSE(target->copyNode(*children, {.maxNodes = 2}));
    REQUIRE_FALSE(target->copyNode(*source->newValue(std::string_view("large payload")), {.maxBytes = 4}));
    auto shared = source->newArray();
    shared->append(children);
    shared->append(children);
    REQUIRE(target->copyNode(*shared));

    auto emptyName = source->newObject();
    REQUIRE(emptyName->addField("", int64_t{7}));
    auto emptyCopy = target->copyNode(*emptyName);
    REQUIRE(emptyCopy);
    REQUIRE(emptyCopy.value()->toJson() == nlohmann::json{{"", 7}});
}

TEST_CASE("Result sequence copy preserves cardinality and shares budgets", "[model.copy]")
{
    auto source = std::make_shared<ModelPool>();
    auto target = std::make_shared<ModelPool>();
    auto array = source->newArray();
    array->append(int64_t{1});
    array->append(int64_t{2});
    std::vector<Value> values{Value::undef(), Value::null(), Value::field(array),
        Value::make(std::string("long string")), Value::make(ByteArray{"bytes"})};
    auto copied = target->copySequence(values);
    REQUIRE(copied);
    REQUIRE(copied.value()->size() == values.size());
    REQUIRE(copied.value()->at(0)->type() == ValueType::Undef);
    REQUIRE(copied.value()->at(1)->type() == ValueType::Null);
    REQUIRE(copied.value()->at(2)->type() == ValueType::Array);
    REQUIRE(copied.value()->at(2)->size() == 2);
    REQUIRE(copied.value()->at(4)->type() == ValueType::Bytes);
    REQUIRE_FALSE(target->copySequence(values, {.maxNodes = 7}));
    REQUIRE(target->copySequence(values, {.maxNodes = 8}));
    REQUIRE_FALSE(target->copySequence(values, {.maxBytes = 25}));
    auto empty = target->copySequence({});
    REQUIRE(empty);
    REQUIRE(empty.value()->size() == 0);
    REQUIRE(empty.value()->type() == ValueType::Array);
    REQUIRE(Array::Storage::is_singleton_handle(empty.value()->addr().index()));
    auto singleton = target->copySequence(std::vector<Value>{Value::make(int64_t{7})});
    REQUIRE(singleton);
    REQUIRE(Array::Storage::is_singleton_handle(singleton.value()->addr().index()));
    REQUIRE(std::get<int64_t>(singleton.value()->at(0)->value()) == 7);
    REQUIRE_FALSE(target->copySequence({}, {.maxNodes = 0}));
}
