#include "common.hpp"
#include "simfil/environment.h"
#include "src/completion.h"

static const PanicFn panicFn{};

auto JoinedResult(std::string_view query) -> std::string
{
    auto model = simfil::json::parse(TestModel);
    Environment env(model->strings());

    env.functions["panic"] = &panicFn;

    auto ast = compile(env, query, false);
    INFO("AST: " << ast->expr().toString());

    auto res = eval(env, *ast, *model->root(0), nullptr);

    std::string vals;
    for (const auto& vv : res) {
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
    return complete(env, query, point, *model->root(0), opts);
}

auto GetDiagnosticMessages(std::string_view query) -> std::vector<Diagnostics::Message>
{
    auto model = simfil::json::parse(TestModel);
    Environment env(model->strings());

    env.functions["panic"] = &panicFn;

    auto ast = compile(env, query, false);
    INFO("AST: " << ast->expr().toString());

    Diagnostics diag;
    auto res = eval(env, *ast, *model->root(0), &diag);

    return diagnostics(env, *ast, diag);
}
