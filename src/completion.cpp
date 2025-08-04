#include "completion.h"

#include "expressions.h"
#include "simfil/result.h"
#include "simfil/simfil.h"
#include "simfil/function.h"

#include <cctype>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace
{

/// Returns true if the given string contains at least one
/// uppercase character.
auto containsUppercaseCharacter(std::string_view str)
{
    static const auto loc = std::locale();

    return std::ranges::any_of(str, [](auto c) {
        return std::isupper(c, loc);
    });
}

/// Returns if `str` starts with `prefix`.
auto startsWith(std::string_view str, std::string_view prefix, bool caseSensitive)
{
    static const auto loc = std::locale();

    if (prefix.size() > str.size())
        return false;

    for (auto i = 0; i < std::min<std::string_view::size_type>(str.size(), prefix.size()); ++i) {
        if (caseSensitive && str[i] != prefix[i])
            return false;
        if (!caseSensitive && std::tolower(str[i], loc) != std::tolower(prefix[i], loc))
            return false;
    }

    return true;
}

/// Returns true, if the given field name needs
/// escaping by using the index operator: `["<field-name>"]`.
auto needsEscaping(std::string_view str)
{
    if (!str.empty() && isdigit(str[0]))
        return true;

    auto i = 0;
    return std::any_of(str.begin(), str.end(), [&i](const auto chr) {
        if (!((chr >= 'a' && chr <= 'z') ||
              (chr >= 'A' && chr <= 'Z') ||
              (chr == '_') ||
              (i > 0 && chr >= '0' && chr <= '9')))
            return true;

        ++i;
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

/// Complete a function name staritng with `prefix` at `loc`.
auto completeFunctions(const simfil::Context& ctx, std::string_view prefix, simfil::Completion& comp, simfil::SourceLocation loc) -> simfil::Result
{
    using simfil::Result;

    const auto caseSensitive = comp.options.smartCase && containsUppercaseCharacter(prefix);
    for (const auto& [ident, fn] : ctx.env->functions) {
        if (startsWith(ident, prefix, caseSensitive)) {
            comp.add(ident, loc, simfil::CompletionCandidate::Type::FUNCTION, fn->ident().signature);
        }
    }

    return Result::Continue;
}

/// Complete a single WORD starting with `prefix` at `loc`.
auto completeWords(const simfil::Context& ctx, std::string_view prefix, simfil::Completion& comp, simfil::SourceLocation loc) -> simfil::Result
{
    using simfil::Result;

    // Generate completion candidates for uppercase string constants from string pool.
    auto stringPool = ctx.env->strings();
    const auto& strings = stringPool->strings();

    const auto caseSensitive = comp.options.smartCase && containsUppercaseCharacter(prefix);
    for (const auto& str : strings) {
        if (comp.size() >= comp.limit)
            return Result::Stop;

        // Check if string is all uppercase + underscores + digits.
        const auto isWORD = !str.empty() && std::ranges::all_of(str, [](char c) {
            return std::isupper(c) || c == '_' || std::isdigit(c);
        });

        if (isWORD && str.size() >= prefix.size() && startsWith(str, prefix, caseSensitive)) {
            comp.add(str, loc, simfil::CompletionCandidate::Type::CONSTANT);
        }
    }

    return Result::Continue;
}

}

namespace simfil
{

CompletionFieldOrWordExpr::CompletionFieldOrWordExpr(std::string prefix, Completion* comp, const Token& token, bool inPath)
    : Expr(token)
    , prefix_(std::move(prefix))
    , comp_(comp)
    , inPath_(inPath)
{}

auto CompletionFieldOrWordExpr::type() const -> Type
{
    return Type::PATH;
}

auto CompletionFieldOrWordExpr::ieval(Context ctx, const Value& val, const ResultFn& res) -> Result
{
    if (ctx.phase == Context::Phase::Compilation)
        return res(ctx, Value::undef());

    if (val.isa(ValueType::Undef))
        return res(ctx, val);

    const auto caseSensitive = comp_->options.smartCase && containsUppercaseCharacter(prefix_);

    // First we try to complete fields
    for (StringId id : val.node->fieldNames()) {
        if (comp_->size() >= comp_->limit)
            return Result::Stop;

        auto keyPtr = ctx.env->strings()->resolve(id);
        if (!keyPtr || keyPtr->empty())
            continue;
        const auto& key = *keyPtr;

        if (key.size() >= prefix_.size() && startsWith(key, prefix_, caseSensitive)) {
            if (needsEscaping(key)) {
                comp_->add(escapeKey(key), sourceLocation(), CompletionCandidate::Type::FIELD);
            } else {
                comp_->add(std::string{key}, sourceLocation(), CompletionCandidate::Type::FIELD);
            }
        }
    }

    // If not in a path, we try to complete words and functions
    if (!inPath_) {
        if (auto r = completeWords(ctx, prefix_, *comp_, sourceLocation()); r != Result::Continue)
            return r;

        if (auto r = completeFunctions(ctx, prefix_, *comp_, sourceLocation()); r != Result::Continue)
            return r;
    }

    return res(ctx, Value::null());
}

auto CompletionFieldOrWordExpr::toString() const -> std::string
{
    return prefix_;
}

auto CompletionFieldOrWordExpr::clone() const -> std::unique_ptr<Expr>
{
    throw std::runtime_error("Cannot clone CompletionFieldOrWordExpr");
}

auto CompletionFieldOrWordExpr::accept(ExprVisitor& v) -> void
{
    v.visit(*this);
}

namespace
{

struct FindExpressionRange : ExprVisitor
{
    size_t min = std::numeric_limits<size_t>::max();
    size_t max = std::numeric_limits<size_t>::min();

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
    if (left_)
        left_->accept(leftRange);

    if (!leftRange.contains(comp->point))
        left_ = nullptr;

    FindExpressionRange rightRange;
    if (right_)
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
    if (left_ && right_)
        return fmt::format("(and {} {})", left_->toString(), right_->toString());
    else if (left_)
        return fmt::format("(and {} ?)", left_->toString());
    else if (right_)
        return fmt::format("(and ? {})", right_->toString());
    return "(and ? ?)";
}

CompletionOrExpr::CompletionOrExpr(ExprPtr left, ExprPtr right, const Completion* comp)
    : left_(std::move(left))
    , right_(std::move(right))
{
    FindExpressionRange leftRange;
    if (left_)
        left_->accept(leftRange);

    if (!leftRange.contains(comp->point))
        left_ = nullptr;

    FindExpressionRange rightRange;
    if (right_)
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
    if (left_ && right_)
        return fmt::format("(or {} {})", left_->toString(), right_->toString());
    else if (left_)
        return fmt::format("(or {} ?)", left_->toString());
    else if (right_)
        return fmt::format("(or ? {})", right_->toString());
    return "(or ? ?)";
}

CompletionWordExpr::CompletionWordExpr(std::string prefix, Completion* comp, const Token& token)
    : Expr(token)
    , prefix_(std::move(prefix))
    , comp_(comp)
{}

auto CompletionWordExpr::type() const -> Type
{
    return Type::VALUE;
}

auto CompletionWordExpr::ieval(Context ctx, const Value& val, const ResultFn& res) -> Result
{
    if (ctx.phase == Context::Phase::Compilation)
        return res(ctx, Value::undef());

    if (auto r = completeWords(ctx, prefix_, *comp_, sourceLocation()); r != Result::Continue)
        return r;

    return res(ctx, Value::undef());
}

auto CompletionWordExpr::toString() const -> std::string
{
    return prefix_;
}

auto CompletionWordExpr::clone() const -> std::unique_ptr<Expr>
{
    throw std::runtime_error("Cannot clone CompletionWordExpr");
}

auto CompletionWordExpr::accept(ExprVisitor& v) -> void
{
    v.visit(*this);
}

}
