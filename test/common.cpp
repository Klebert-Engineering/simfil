#include "common.hpp"
#include "catch2/catch_test_macros.hpp"
#include "simfil/environment.h"
#include "src/completion.h"

static const PanicFn panicFn{};

auto CompileError(std::string_view query, bool autoWildcard) -> Error
{
    Environment env(Environment::WithNewStringCache);
    env.constants.try_emplace("a_number", simfil::Value::make((int64_t)123));
    env.functions["panic"] = &panicFn;

    auto ast = compile(env, query, false, autoWildcard);
    REQUIRE(!ast.has_value());

    return std::move(ast.error());
}

auto Compile(std::string_view query, bool autoWildcard) -> ASTPtr
{
    Environment env(Environment::WithNewStringCache);
    env.constants.try_emplace("a_number", simfil::Value::make((int64_t)123));
    env.functions["panic"] = &panicFn;

    auto ast = compile(env, query, false, autoWildcard);
    if (!ast)
        INFO(ast.error().message);
    REQUIRE(ast.has_value());

    return std::move(*ast);
}

auto JoinedResult(std::string_view query, std::optional<std::string> json) -> std::string
{
    auto model = simfil::json::parse(std::string(json.value_or(TestModel)));
    REQUIRE(model);
    Environment env(model.value()->strings());

    env.functions["panic"] = &panicFn;

    auto ast = compile(env, query, false);
    if (!ast) {
        INFO("ERROR: " << ast.error().message);
        return fmt::format("ERROR: {}", ast.error().message);
    }

    INFO("AST: " << (*ast)->expr().toString());

    auto root = model.value()->root(0);
    REQUIRE(root);

    auto res = eval(env, **ast, **root, nullptr);
    if (!res) {
        INFO("ERROR: " << res.error().message);
        return fmt::format("ERROR: {}", res.error().message);
    }

    std::string vals;
    for (const auto& vv : *res) {
        if (!vals.empty())
            vals.push_back('|');
        vals += vv.toString();
    }
    return vals;
}

auto CompleteQuery(std::string_view query, size_t point, std::optional<std::string> json, const CompletionOptions* options) -> std::vector<CompletionCandidate>
{
    auto model = simfil::json::parse(json.value_or(TestModel));
    REQUIRE(model);
    Environment env(model.value()->strings());

    CompletionOptions opts;
    opts.showWildcardHints = true;
    if (options)
        opts = *options;

    auto root = model.value()->root(0);
    REQUIRE(root);
    return complete(env, query, point, **root, opts).value_or(std::vector<CompletionCandidate>());
}

auto GetDiagnosticMessages(std::string_view query) -> std::vector<Diagnostics::Message>
{
    auto model = simfil::json::parse(TestModel);
    REQUIRE(model);
    Environment env(model.value()->strings());

    env.functions["panic"] = &panicFn;

    auto ast = compile(env, query, false);
    if (!ast)
        INFO(ast.error().message);
    REQUIRE(ast);

    INFO("AST: " << (*ast)->expr().toString());

    Diagnostics diag;
    auto root = model.value()->root(0);
    REQUIRE(root);
    auto res = eval(env, **ast, **root, &diag);
    if (!res)
        INFO(res.error().message);
    REQUIRE(res);

    return diagnostics(env, **ast, diag).value_or(std::vector<Diagnostics::Message>());
}
