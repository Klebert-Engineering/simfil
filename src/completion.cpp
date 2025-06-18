#include "completion.h"

#include "expressions.h"
#include <string_view>

namespace
{

auto startsWith(std::string_view str, std::string_view prefix)
{
    for (auto i = 0; i < std::min<std::string_view::size_type>(str.size(), prefix.size()); ++i)
        if (std::tolower(str[i], std::locale()) != std::tolower(prefix[i], std::locale()))
            return false;
    return true;
}

auto needsEscaping(std::string_view str)
{
    if (!str.empty() && isdigit(str[0]))
        return true;

    return std::any_of(str.begin(), str.end(), [](const auto chr) mutable {
        if (!((chr >= 'a' && chr <= 'z') || (chr >= 'A' && chr <= 'Z')))
            return true;
        return false;
    });
}

auto escapeKey(std::string_view str)
{
    std::string escaped = "[\"";
    escaped.reserve(str.size() + 4);

    for (auto i = 0; i < str.size(); ++i) {
        if (str[i] == '"' || str[i] == '\\')
            escaped.push_back('\\');
        escaped.push_back(str[i]);
    }

    escaped += "\"]";
    return escaped;
}

}

namespace simfil
{

CompletionFieldExpr::CompletionFieldExpr(std::string prefix, Completion* comp, const Token& token)
    : Expr(token)
    , prefix_(std::move(prefix))
    , comp_(comp)
{}

auto CompletionFieldExpr::type() const -> Type
{
    return Type::PATH;
}

auto CompletionFieldExpr::ieval(Context ctx, const Value& val, const ResultFn& res) -> Result
{
    if (val.isa(ValueType::Undef))
        return res(ctx, val);

    for (StringId id : val.node->fieldNames()) {
        auto keyPtr = ctx.env->strings()->resolve(id);
        if (!keyPtr || keyPtr->empty())
            continue;
        const auto& key = *keyPtr;

        if (key.size() >= prefix_.size() && startsWith(key, prefix_)) {
            if (needsEscaping(key)) {
                comp_->add(escapeKey(key), sourceLocation());
            } else {
                comp_->add(std::string{key}, sourceLocation());
            }
        }
    }

    if (ctx.phase == Context::Phase::Compilation)
        return res(ctx, Value::undef());
    return res(ctx, Value::null());
}

auto CompletionFieldExpr::toString() const -> std::string
{
    return prefix_;
}

auto CompletionFieldExpr::clone() const -> std::unique_ptr<Expr>
{
    throw std::runtime_error("Cannot clone CompletionFieldExpr");
}

auto CompletionFieldExpr::accept(ExprVisitor& v) -> void
{
    v.visit(*this);
}

}
