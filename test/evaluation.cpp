#include "common.hpp"
#include "simfil/types.h"

TEST_CASE("Streaming evaluation bounds results before collecting a vector", "[evaluation.bounded]")
{
    auto model = json::parse(R"({"limit":10000000})").value();
    Environment env(model->strings());
    auto ast = compile(env, "range(0, limit)...", false);
    REQUIRE(ast);
    BoundExpression bound(SharedAST(std::move(*ast)), env);
    EvaluationOptions options;
    options.maxResults = 3;
    std::vector<Value> values;
    LambdaResultFn consume([&](Context, Value&& value) {
        values.push_back(std::move(value));
        return Result::Continue;
    });
    auto result = bound.eval(**model->root(0), consume, options);
    REQUIRE(result);
    REQUIRE(result->reason == EvaluationSummary::Reason::ResultLimit);
    REQUIRE(result->results == 3);
    REQUIRE(values.size() == 3);
    REQUIRE(values.back().toString() == "2");
    REQUIRE(result->work < 100);

    options.maxResults = 0;
    values.clear();
    result = bound.eval(**model->root(0), consume, options);
    REQUIRE(result);
    REQUIRE(result->reason == EvaluationSummary::Reason::ResultLimit);
    REQUIRE(values.empty());
    REQUIRE(result->work == 0);

    options.maxResults = 100;
    result = bound.eval(**model->root(0), LambdaResultFn([](Context, const Value&) { return Result::Stop; }), options);
    REQUIRE(result);
    REQUIRE(result->reason == EvaluationSummary::Reason::ConsumerStopped);
    REQUIRE(result->results == 1);
}

TEST_CASE("Work budgets interrupt aggregates and zero-hit wildcard traversal", "[evaluation.bounded]")
{
    auto model = json::parse(R"({"limit":10000000})").value();
    Environment env(model->strings());
    EvaluationOptions options;
    options.maxWork = 100;
    LambdaResultFn unexpectedResult([](Context, const Value&) {
        FAIL("Budget exhaustion must not emit a misleading aggregate or missing-field null");
        return Result::Continue;
    });
    for (auto query : {"count(range(0, limit)...)", "sum(range(0, limit)...)", "each(range(0, limit)... >= 0)"}) {
        auto ast = compile(env, query, false);
        REQUIRE(ast);
        auto result = eval(env, **ast, **model->root(0), unexpectedResult, options);
        REQUIRE(result);
        REQUIRE(result->reason == EvaluationSummary::Reason::WorkLimit);
        REQUIRE(result->work == options.maxWork);
        REQUIRE(result->results == 0);
    }

    auto array = model->newArray();
    for (auto i = 0; i < 1000; ++i)
        array->append(model->newObject());
    (void)model->strings()->emplace("missing");
    auto ast = compile(env, "**.missing", false);
    REQUIRE(ast);
    auto result = eval(env, **ast, *array, unexpectedResult, options);
    REQUIRE(result);
    REQUIRE(result->reason == EvaluationSummary::Reason::WorkLimit);
    REQUIRE(result->results == 0);
}

TEST_CASE("Streaming evaluation reports cancellation deadlines depth and completion", "[evaluation.bounded]")
{
    auto model = json::parse(R"({"limit":5,"next":{"next":{"value":42}}})").value();
    Environment env(model->strings());
    auto ast = compile(env, "range(0, limit)...", false);
    REQUIRE(ast);
    LambdaResultFn consume([](Context, const Value&) { return Result::Continue; });
    EvaluationOptions options;
    auto result = eval(env, **ast, **model->root(0), consume, options);
    REQUIRE(result);
    REQUIRE(result->reason == EvaluationSummary::Reason::Complete);
    REQUIRE(result->results == 6); // SIMFIL ranges include both endpoints.

    options.deadline = std::chrono::steady_clock::now();
    result = eval(env, **ast, **model->root(0), consume, options);
    REQUIRE(result);
    REQUIRE(result->reason == EvaluationSummary::Reason::Timeout);
    REQUIRE(result->results == 0);
    options.deadline = std::chrono::steady_clock::time_point::max();

    std::atomic_bool cancel = false;
    options.cancel = &cancel;
    result = eval(env, **ast, **model->root(0), LambdaResultFn([&](Context, const Value&) {
        cancel = true;
        return Result::Continue;
    }), options);
    REQUIRE(result);
    REQUIRE(result->reason == EvaluationSummary::Reason::Canceled);
    REQUIRE(result->results == 1);
    options.cancel = nullptr;

    options.maxDepth = 1;
    ast = compile(env, "**.value", false);
    REQUIRE(ast);
    result = eval(env, **ast, **model->root(0), consume, options);
    REQUIRE(result);
    REQUIRE(result->reason == EvaluationSummary::Reason::DepthLimit);
    REQUIRE(result->results == 0);
}

TEST_CASE("Bounded evaluation preserves compound values and callback errors", "[evaluation.bounded]")
{
    auto model = json::parse(R"({"a":{"b":42}})").value();
    Environment env(model->strings());
    auto ast = compile(env, "a", false);
    REQUIRE(ast);
    std::vector<Value> values;
    auto result = eval(env, **ast, **model->root(0), LambdaResultFn([&](Context, Value&& value) {
        values.push_back(std::move(value));
        return Result::Continue;
    }));
    REQUIRE(result);
    REQUIRE(result->reason == EvaluationSummary::Reason::Complete);
    REQUIRE(values.front().isa(ValueType::Object));
    REQUIRE(values.front().node()->get(model->strings()->get("b"))->value() == ScalarValueType(int64_t{42}));

    result = eval(env, **ast, **model->root(0), LambdaResultFn([](Context, const Value&)
        -> tl::expected<Result, Error> {
        return tl::unexpected<Error>(Error::InvalidArguments, "consumer failure");
    }));
    REQUIRE_FALSE(result);
    REQUIRE(result.error().message == "consumer failure");
}

TEST_CASE("Unpacking and splitting preserve callback errors and early stops", "[evaluation.bounded]")
{
    auto model = json::parse(R"({"limit":10000000,"text":"a,b,c"})").value();
    Environment env(model->strings());
    for (auto query : {"range(0, limit)...", "split(text, ',')", "missing.path", "**.missing"}) {
        auto ast = compile(env, query, false);
        REQUIRE(ast);
        auto result = eval(env, **ast, **model->root(0), LambdaResultFn([](Context, const Value&)
            -> tl::expected<Result, Error> {
            return tl::unexpected<Error>(Error::InvalidArguments, "consumer failure");
        }));
        REQUIRE_FALSE(result);
        REQUIRE(result.error().message == "consumer failure");

        EvaluationOptions options;
        options.maxWork = 1;
        result = eval(env, **ast, **model->root(0), LambdaResultFn([](Context, const Value&) { return Result::Continue; }), options);
        REQUIRE(result);
        REQUIRE(result->reason == EvaluationSummary::Reason::WorkLimit);
    }

    // A regexp has no unpack operator; its transient-type error must surface.
    auto expression = std::make_unique<UnpackExpr>(std::make_unique<ConstExpr>(ReType::Type.make("x")));
    AST ast("re('x')...", std::move(expression));
    auto result = eval(env, ast, **model->root(0), LambdaResultFn([](Context, const Value&) { return Result::Continue; }));
    REQUIRE_FALSE(result);
    REQUIRE(result.error().message == "Type has no unpack operator");
}

TEST_CASE("Split streams large inputs and charges skipped empty pieces", "[evaluation.bounded]")
{
    auto model = std::make_shared<ModelPool>();
    auto root = model->newObject();
    root->addField("text", std::string(1000000, ','));
    Environment env(model->strings());
    EvaluationOptions options;
    options.maxResults = 1;
    auto ast = compile(env, "split(text, ',')", false);
    REQUIRE(ast);
    auto result = eval(env, **ast, *root, LambdaResultFn([](Context, const Value& value) {
        CHECK(value.toString().empty());
        return Result::Continue;
    }), options);
    REQUIRE(result);
    REQUIRE(result->reason == EvaluationSummary::Reason::ResultLimit);
    REQUIRE(result->work < 20);
    options.maxWork = 20;
    ast = compile(env, "split(text, ',', false)", false);
    REQUIRE(ast);
    result = eval(env, **ast, *root, LambdaResultFn([](Context, const Value&) { return Result::Continue; }), options);
    REQUIRE(result);
    REQUIRE(result->reason == EvaluationSummary::Reason::WorkLimit);
    REQUIRE(result->results == 0);
}
