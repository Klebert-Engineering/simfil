#pragma once

#include "simfil/token.h"
#include "simfil/environment.h"

#include "expressions.h"

#include <string>
#include <set>
#include <memory>

namespace simfil
{

class ExprVisitor;

/**
 * List of completion candidates.
 */
struct Completion
{
    Completion(std::size_t point, CompletionOptions options)
        : point(point), options(std::move(options))
    {}

    Completion(const Completion&) = delete;
    Completion& operator=(const Completion&) = delete;

    auto add(std::string str, SourceLocation location, CompletionCandidate::Type type, std::string hint = {})
    {
        candidates.emplace(std::move(str), location, type, hint);
    }

    auto size() const
    {
        return candidates.size();
    }

    std::size_t point;
    std::size_t limit = 1000;
    std::set<CompletionCandidate> candidates;
    CompletionOptions options;
};

class CompletionFieldOrWordExpr : public Expr
{
public:
    CompletionFieldOrWordExpr(ExprId id, std::string prefix, Completion* comp, const Token& token, bool inPath);

    auto type() const -> Type override;
    auto ieval(Context ctx, const Value& value, const ResultFn& result) const -> tl::expected<Result, Error> override;
    auto accept(ExprVisitor& v) const -> void override;
    auto toString() const -> std::string override;

    std::string prefix_;
    Completion* comp_;
    bool inPath_;
};

class CompletionAndExpr : public Expr
{
public:
    CompletionAndExpr(ExprId id, ExprPtr left, ExprPtr right, const Completion* comp);

    auto type() const -> Type override;
    auto ieval(Context ctx, const Value& val, const ResultFn& res) const -> tl::expected<Result, Error> override;
    void accept(ExprVisitor& v) const override;
    auto toString() const -> std::string override;

    ExprPtr left_, right_;
};

class CompletionOrExpr : public Expr
{
public:
    CompletionOrExpr(ExprId id, ExprPtr left, ExprPtr right, const Completion* comp);

    auto type() const -> Type override;
    auto ieval(Context ctx, const Value& val, const ResultFn& res) const -> tl::expected<Result, Error> override;
    void accept(ExprVisitor& v) const override;
    auto toString() const -> std::string override;

    ExprPtr left_, right_;
};

class CompletionWordExpr : public Expr
{
public:
    CompletionWordExpr(ExprId id, std::string prefix, Completion* comp, const Token& token);

    auto type() const -> Type override;
    auto constant() const -> bool override;
    auto ieval(Context ctx, const Value& value, const ResultFn& result) const -> tl::expected<Result, Error> override;
    auto accept(ExprVisitor& v) const -> void override;
    auto toString() const -> std::string override;

    std::string prefix_;
    Completion* comp_;
};

/**
 * A special expression to prevent constant value
 * evaluation during completion.
 */
class CompletionConstExpr : public ConstExpr
{
public:
    using ConstExpr::ConstExpr;

    auto constant() const -> bool override;
    auto ieval(Context ctx, const Value&, const ResultFn& res) const -> tl::expected<Result, Error> override;
};

}
