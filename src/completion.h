#pragma once

#include "simfil/expression.h"
#include "simfil/token.h"
#include "simfil/environment.h"

#include <limits>
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
    explicit Completion(std::size_t point)
        : point(point)
    {}

    auto add(std::string str, SourceLocation location)
    {
        candidates.insert({std::move(str), location});
    }

    auto size() const
    {
        return candidates.size();
    }

    std::size_t point;
    std::size_t limit = 1000;
    std::set<CompletionCandidate> candidates;
};

class CompletionFieldExpr : public Expr
{
public:
    CompletionFieldExpr(std::string prefix, Completion* comp, const Token& token);

    auto type() const -> Type override;
    auto ieval(Context ctx, const Value& value, const ResultFn& result) -> Result override;
    auto clone() const -> std::unique_ptr<Expr> override;
    auto accept(ExprVisitor& v) -> void override;
    auto toString() const -> std::string override;

    std::string prefix_;
    Completion* comp_;
};

class CompletionAndExpr : public Expr
{
public:
    CompletionAndExpr(ExprPtr left, ExprPtr right, Completion* comp);

    auto type() const -> Type override;
    auto ieval(Context ctx, const Value& val, const ResultFn& res) -> Result override;
    void accept(ExprVisitor& v) override;
    auto clone() const -> ExprPtr override;
    auto toString() const -> std::string override;

    ExprPtr left_, right_;
};

class CompletionOrExpr : public Expr
{
public:
    CompletionOrExpr(ExprPtr left, ExprPtr right, Completion* comp);

    auto type() const -> Type override;
    auto ieval(Context ctx, const Value& val, const ResultFn& res) -> Result override;
    void accept(ExprVisitor& v) override;
    auto clone() const -> ExprPtr override;
    auto toString() const -> std::string override;

    ExprPtr left_, right_;
};

}
