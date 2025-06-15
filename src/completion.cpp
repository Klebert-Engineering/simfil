#include "completion.h"

#include "expressions.h"

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

        // TODO: Make this case insensitive
        if ((key.size() >= prefix_.size() && key.starts_with(prefix_))) {
            if (isdigit(key[0]) || std::any_of(key.begin(), key.end(), [](auto c) {
                return !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'));
            })) {
                comp_->add(fmt::format("[\"{}\"]", key), sourceLocation());
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
