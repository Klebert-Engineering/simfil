#include "common.hpp"

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
