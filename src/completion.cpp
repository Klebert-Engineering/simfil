#include "completion.h"

#include "expressions.h"
#include "simfil/result.h"
#include <stdexcept>
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
    if (ctx.phase == Context::Phase::Compilation)
        return res(ctx, Value::undef());

    if (val.isa(ValueType::Undef))
        return res(ctx, val);

    for (StringId id : val.node->fieldNames()) {
        if (comp_->size() >= comp_->limit)
            return Result::Stop;

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

namespace
{

struct FindExpressionRange : ExprVisitor
{
    size_t min = 0;
    size_t max = 0;

    auto contains(size_t point) const
    {
        return min <= point && point <= max;
    }

    using ExprVisitor::visit;

    void visit(Expr& expr) override
    {
        ExprVisitor::visit(expr);

        auto loc = expr.sourceLocation();
        if (loc.size > 0) {
            min = std::min<size_t>(min, loc.offset);
            max = std::max<size_t>(max, loc.offset + loc.size);
        }
    }
};

}

CompletionAndExpr::CompletionAndExpr(ExprPtr left, ExprPtr right, const Completion* comp)
    : left_(std::move(left))
    , right_(std::move(right))
{
    FindExpressionRange leftRange;
    left_->accept(leftRange);

    if (!leftRange.contains(comp->point))
        left_ = nullptr;

    FindExpressionRange rightRange;
    right_->accept(rightRange);

    if (!rightRange.contains(comp->point))
        right_ = nullptr;
}

auto CompletionAndExpr::type() const -> Type
{
    return Type::VALUE;
}

auto CompletionAndExpr::ieval(Context ctx, const Value& val, const ResultFn& res) -> Result
{
    if (left_)
        (void)left_->eval(ctx, val, LambdaResultFn([](const Context&, const Value&) {
            return Result::Continue;
        }));

    if (right_)
        (void)right_->eval(ctx, val, LambdaResultFn([](const Context&, const Value&) {
            return Result::Continue;
        }));

    return Result::Continue;
}

void CompletionAndExpr::accept(ExprVisitor& v)
{
    v.visit(*this);
}

auto CompletionAndExpr::clone() const -> ExprPtr
{
    throw std::runtime_error("Cannot clone CompletionAndExpr");
}

auto CompletionAndExpr::toString() const -> std::string
{
    return "(and "s + left_->toString() + " "s + right_->toString() + ")"s;
}

CompletionOrExpr::CompletionOrExpr(ExprPtr left, ExprPtr right, const Completion* comp)
    : left_(std::move(left))
    , right_(std::move(right))
{
    FindExpressionRange leftRange;
    left_->accept(leftRange);

    if (!leftRange.contains(comp->point))
        left_ = nullptr;

    FindExpressionRange rightRange;
    right_->accept(rightRange);

    if (!rightRange.contains(comp->point))
        right_ = nullptr;
}

auto CompletionOrExpr::type() const -> Type
{
    return Type::VALUE;
}

auto CompletionOrExpr::ieval(Context ctx, const Value& val, const ResultFn& res) -> Result
{
    if (left_)
        (void)left_->eval(ctx, val, LambdaResultFn([](const Context&, const Value&) {
            return Result::Continue;
        }));

    if (right_)
        (void)right_->eval(ctx, val, LambdaResultFn([](const Context&, const Value&) {
            return Result::Continue;
        }));

    return Result::Continue;
}

void CompletionOrExpr::accept(ExprVisitor& v)
{
    v.visit(*this);
}

auto CompletionOrExpr::clone() const -> ExprPtr
{
    throw std::runtime_error("Cannot clone CompletionOrExpr");
}

auto CompletionOrExpr::toString() const -> std::string
{
    return "(or "s + left_->toString() + " "s + right_->toString() + ")"s;
}

}
