#include "completion.h"

#include "expressions.h"
#include "simfil/model/string-pool.h"
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

CompletionFieldOrWordExpr::CompletionFieldOrWordExpr(ExprId id, std::string prefix, Completion* comp, const Token& token, bool inPath)
    : Expr(id, token)
    , prefix_(std::move(prefix))
    , comp_(comp)
    , inPath_(inPath)
{}

auto CompletionFieldOrWordExpr::type() const -> Type
{
    return Type::FIELD;
}

auto CompletionFieldOrWordExpr::ieval(Context ctx, Value val) const -> EvalStream
{
    if (ctx.phase == Context::Phase::Compilation) {
        co_yield Value::undef();
        co_return;
    }

    if (val.isa(ValueType::Undef)) {
        co_yield val;
        co_return;
    }

    const auto node = val.node();
    if (!node) {
        co_yield val;
        co_return;
    }

    const auto caseSensitive = comp_->options.smartCase && containsUppercaseCharacter(prefix_);

    // First we try to complete fields
    for (StringId id : node->fieldNames()) {
        if (comp_->size() >= comp_->limit) {
            co_return;
        }

        if (id == StringPool::Empty)
            continue;

        auto keyPtr = ctx.env->strings()->resolve(id);
        if (!keyPtr || keyPtr->empty())
            continue;

        const auto& key = *keyPtr;
        if (startsWith(key, prefix_, caseSensitive)) {
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
            co_return;

        if (auto r = completeFunctions(ctx, prefix_, *comp_, sourceLocation()); r != Result::Continue)
            co_return;
    }

    co_yield Value::undef();
}

auto CompletionFieldOrWordExpr::toString() const -> std::string
{
    return prefix_;
}

auto CompletionFieldOrWordExpr::accept(ExprVisitor& v) const -> void
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

    void visit(const Expr& expr) override
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

CompletionAndExpr::CompletionAndExpr(ExprId id, ExprPtr left, ExprPtr right, const Completion* comp)
    : Expr(id)
    , left_(std::move(left))
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

auto CompletionAndExpr::ieval(Context ctx, Value val) const -> EvalStream
{
    if (left_)
        for (auto&& left : left_->eval(ctx, val)) {
            CO_TRY_EXPECTED(left);
        }

    if (right_)
        for (auto&& right : right_->eval(ctx, val)) {
            CO_TRY_EXPECTED(right);
        }
}

void CompletionAndExpr::accept(ExprVisitor& v) const
{
    v.visit(*this);
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

CompletionOrExpr::CompletionOrExpr(ExprId id, ExprPtr left, ExprPtr right, const Completion* comp)
    : Expr(id)
    , left_(std::move(left))
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

auto CompletionOrExpr::ieval(Context ctx, Value val) const -> EvalStream
{
    if (left_)
        for (auto&& left : left_->eval(ctx, val)) {
            CO_TRY_EXPECTED(left);
        }

    if (right_)
        for (auto&& right : right_->eval(ctx, val)) {
            CO_TRY_EXPECTED(right);
        }
}

void CompletionOrExpr::accept(ExprVisitor& v) const
{
    v.visit(*this);
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

CompletionWordExpr::CompletionWordExpr(ExprId id, std::string prefix, Completion* comp, const Token& token)
    : Expr(id, token)
    , prefix_(std::move(prefix))
    , comp_(comp)
{}

auto CompletionWordExpr::type() const -> Type
{
    return Type::VALUE;
}

auto CompletionWordExpr::constant() const -> bool
{
    return true;
}

auto CompletionWordExpr::ieval(Context ctx, Value val) const -> EvalStream
{
    if (ctx.phase == Context::Phase::Compilation) {
        co_yield Value::undef();
        co_return;
    }

    else if (auto r = completeWords(ctx, prefix_, *comp_, sourceLocation()); r != Result::Continue) {
        co_yield val;
    }
    else {
        co_yield Value::undef();
    }
}

auto CompletionWordExpr::toString() const -> std::string
{
    return prefix_;
}

auto CompletionWordExpr::accept(ExprVisitor& v) const -> void
{
    v.visit(*this);
}

auto CompletionConstExpr::constant() const -> bool
{
    return false;
}

auto CompletionConstExpr::ieval(Context ctx, Value) const -> EvalStream
{
    if (ctx.phase == Context::Compilation) {
        co_yield Value::undef();
    }
    else {
        co_yield value_;
    }
}

}
