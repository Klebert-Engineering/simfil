#include "common.hpp"
#include "catch2/catch_test_macros.hpp"
#include "simfil/environment.h"
#include "src/completion.h"

static const PanicFn panicFn{};

auto Compile(std::string_view query, bool autoWildcard) -> ASTPtr
{
    Environment env(Environment::WithNewStringCache);
    env.constants.emplace("a_number", simfil::Value::make((int64_t)123));

    auto ast = compile(env, query, false, autoWildcard);
    if (!ast)
        INFO(ast.error().message);
    REQUIRE(ast.has_value());

    return std::move(*ast);
}

auto JoinedResult(std::string_view query) -> std::string
{
    auto model = simfil::json::parse(TestModel);
    Environment env(model->strings());

    env.functions["panic"] = &panicFn;

    auto ast = compile(env, query, false);
    if (!ast)
        INFO(ast.error().message);
    REQUIRE(ast.has_value());

    INFO("AST: " << (*ast)->expr().toString());

    auto res = eval(env, **ast, *model->root(0), nullptr);
    if (!res)
        INFO(res.error().message);
    REQUIRE(res);

    std::string vals;
    for (const auto& vv : *res) {
        if (!vals.empty())
            vals.push_back('|');
        vals += vv.toString();
    }
    return vals;
}

auto CompleteQuery(std::string_view query, size_t point) -> std::vector<CompletionCandidate>
{
    auto model = simfil::json::parse(TestModel);
    Environment env(model->strings());

    CompletionOptions opts;
    return complete(env, query, point, *model->root(0), opts).value_or(std::vector<CompletionCandidate>());
}

auto GetDiagnosticMessages(std::string_view query) -> std::vector<Diagnostics::Message>
{
    auto model = simfil::json::parse(TestModel);
    Environment env(model->strings());

    env.functions["panic"] = &panicFn;

    auto ast = compile(env, query, false);
    if (!ast)
        INFO(ast.error().message);
    REQUIRE(ast);

    INFO("AST: " << (*ast)->expr().toString());

    Diagnostics diag;
    auto res = eval(env, **ast, *model->root(0), &diag);
    if (!res)
        INFO(res.error().message);
    REQUIRE(res);

    return diagnostics(env, **ast, diag).value_or(std::vector<Diagnostics::Message>());
}
