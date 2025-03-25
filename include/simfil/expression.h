// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#include "simfil/token.h"
#include "simfil/value.h"
#include "simfil/environment.h"
#include "simfil/result.h"

#include <memory>

namespace simfil
{

class ExprVisitor;

class Expr
{
    friend class AST;
public:
    /**
     * Type of an expression.
     */
    enum Type {
        FIELD,
        PATH,
        SUBEXPR,
        SUBSCRIPT,
        VALUE,
    };

    Expr() = default;
    explicit Expr(const Token& token)
    {
        sourceLocation_.begin = token.begin;
        sourceLocation_.size = token.end - token.begin;
    }

    virtual ~Expr() {}

    /* Category */
    virtual auto type() const -> Type = 0;
    virtual auto constant() const -> bool
    {
        return false;
    }

    /* Debug */
    virtual auto toString() const -> std::string = 0;

    /* Evaluation wrapper */
    auto eval(Context ctx, Value val, const ResultFn& res) -> Result
    {
        auto dbg = ctx.env->debug;
        if (dbg) dbg->evalBegin(*this, ctx, val, res);
        auto r = ieval(std::move(ctx), std::move(val), res);
        if (dbg) dbg->evalEnd(*this);
        return r;
    }

    /* Recursive clone */
    virtual auto clone() const -> std::unique_ptr<Expr> = 0;

    /* Accept expression visitor */
    virtual void accept(ExprVisitor&) = 0;

    /* Source location the expression got parsed from */
    auto sourceLocation() const -> SourceLocation
    {
        return sourceLocation_;
    }

private:
    /* Abstract evaluation implementation */
    virtual auto ieval(Context, Value, const ResultFn&) -> Result = 0;

    SourceLocation sourceLocation_;
};

using ExprPtr = std::unique_ptr<Expr>;

class AST
{
public:
    AST(std::string query, ExprPtr expr)
        : queryString_(std::move(query))
        , expr_(std::move(expr))
    {}

    auto expr() const -> Expr&
    {
        return *expr_;
    }

    auto query() const -> std::string
    {
        return queryString_;
    }

private:
    /* The original query string of the AST */
    std::string queryString_;

    /* The actuall AST */
    ExprPtr expr_;
};

using ASTPtr = std::unique_ptr<AST>;

}
