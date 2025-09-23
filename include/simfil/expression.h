// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#include "simfil/token.h"
#include "simfil/value.h"
#include "simfil/environment.h"
#include "simfil/diagnostics.h"
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
        assert(token.end >= token.begin);
        sourceLocation_.offset = token.begin;
        sourceLocation_.size = token.end - token.begin;
    }

    virtual ~Expr() = default;

    /* Category */
    virtual auto type() const -> Type = 0;
    virtual auto constant() const -> bool
    {
        return false;
    }

    /* Debug */
    virtual auto toString() const -> std::string = 0;

    auto eval(Context ctx, const Value& val, const ResultFn& res) -> tl::expected<Result, Error>
    {
        if (ctx.canceled())
            return Result::Stop;

        if (auto dbg = ctx.env->debug) [[unlikely]] {
            auto dbgVal = Value{val};
            dbg->evalBegin(*this, ctx, dbgVal, res);
            auto r = ieval(ctx, std::move(dbgVal), res);
            dbg->evalEnd(*this);
            return r;
        }

        return ieval(ctx, val, res);
    }

    auto eval(Context ctx, Value&& val, const ResultFn& res) -> tl::expected<Result, Error>
    {
        if (ctx.canceled())
            return Result::Stop;

        if (auto dbg = ctx.env->debug) [[unlikely]] {
            dbg->evalBegin(*this, ctx, val, res);
            auto r = ieval(ctx, std::move(val), res);
            dbg->evalEnd(*this);
            return r;
        }

        return ieval(ctx, std::move(val), res);
    }

    /* Recursive clone */
    [[nodiscard]]
    virtual auto clone() const -> std::unique_ptr<Expr> = 0;

    /* Accept expression visitor */
    virtual auto accept(ExprVisitor& v) -> void = 0;

    /* Source location the expression got parsed from */
    [[nodiscard]]
    auto sourceLocation() const -> SourceLocation
    {
        return sourceLocation_;
    }

private:
    /* Abstract evaluation implementation */
    virtual auto ieval(Context ctx, const Value& value, const ResultFn& result) -> tl::expected<Result, Error> = 0;
    
    /* Move-optimized evaluation implementation */
    virtual auto ieval(Context ctx, Value&& value, const ResultFn& result) -> tl::expected<Result, Error> {
        return ieval(ctx, value, result);
    }

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

    ~AST();

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
