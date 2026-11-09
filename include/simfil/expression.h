// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#include "simfil/token.h"
#include "simfil/value.h"
#include "simfil/environment.h"
#include "simfil/result.h"

#include <memory>
#include <stdexcept>

namespace simfil
{

class Expr;
class ExprVisitor;

using ExprPtr = std::unique_ptr<Expr>;

/**
 * One AST operation producing a stream from an input Value. Concrete operations
 * implement ieval(); eval() supplies cancellation, budgets and debug callbacks.
 * Environment-dependent caches belong to Context, so a published AST can be shared.
 */
class Expr
{
    friend class AST;
public:
    using ExprId = std::uint32_t;

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

    explicit Expr(SourceLocation location)
        : sourceLocation_(location)
    {}

    virtual ~Expr() = default;

    /** AST-local identity used by runtime caches and diagnostic counters. */
    auto id() const -> ExprId
    {
        return id_;
    }

    virtual auto type() const -> Type = 0;

    /** Whether this expression can be evaluated without a runtime input value. */
    virtual auto constant() const -> bool
    {
        return false;
    }

    /* Accept expression visitor */
    virtual auto accept(ExprVisitor& v) const -> void = 0;

    /* Get the number of child expressions */
    virtual auto numChildren() const -> std::size_t
    {
        return 0;
    }

    /* Get the n-th child expression */
    virtual auto childAt(std::size_t) -> ExprPtr&
    {
        throw std::out_of_range("AST child index out of range");
    }

    virtual auto childAt(std::size_t) const -> const ExprPtr&
    {
        throw std::out_of_range("AST child index out of range");
    }

    /* Debug */
    virtual auto toString() const -> std::string = 0;

    /** Evaluate synchronously, forwarding consumer Stop/errors without further emissions. */
    auto eval(Context ctx, const Value& val, const ResultFn& res) const -> tl::expected<Result, Error>
    {
        if (ctx.evaluation) [[unlikely]]
            return evalBounded(ctx, val, res);
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

    auto eval(Context ctx, Value&& val, const ResultFn& res) const -> tl::expected<Result, Error>
    {
        if (ctx.evaluation) [[unlikely]]
            return evalBounded(ctx, std::move(val), res);
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

    /* Source location the expression got parsed from */
    auto sourceLocation() const -> SourceLocation
    {
        return sourceLocation_;
    }

private:
    /** Guard intermediate results and expression entry without wrapping unbounded evaluations. */
    auto evalBounded(Context ctx, const Value& value, const ResultFn& result) const -> tl::expected<Result, Error>;
    auto evalBounded(Context ctx, Value&& value, const ResultFn& result) const -> tl::expected<Result, Error>;

    /** Share bounded dispatch without copying const transient values before budget checks. */
    template<class T>
    auto evalBoundedImpl(Context ctx, T&& value, const ResultFn& result) const -> tl::expected<Result, Error>;
    class BoundedResultFn;

    /** Implement the operation and propagate Stop/errors from every nested evaluation/sink. */
    virtual auto ieval(Context ctx, const Value& value, const ResultFn& result) const -> tl::expected<Result, Error> = 0;
    
    /* Move-optimized evaluation implementation */
    virtual auto ieval(Context ctx, Value&& value, const ResultFn& result) const -> tl::expected<Result, Error>
    {
        return ieval(ctx, value, result);
    }

    ExprId id_ = 0;
    SourceLocation sourceLocation_;
};

/** Own the source query and expression tree, assigning IDs for runtime and diagnostic lookup. */
class AST
{
public:
    AST(std::string query, ExprPtr expr)
        : queryString_(std::move(query))
        , expr_(std::move(expr))
    {
        // Runtime caches use expression IDs, including for manually built ASTs.
        reenumerate();
    }

    ~AST();

    /** Reassign IDs after rewriting children, before creating bindings or diagnostic indices. */
    auto reenumerate() -> void;

    auto expr() const -> const Expr&
    {
        return *expr_;
    }

    auto query() const -> std::string
    {
        return queryString_;
    }

private:
    static auto reenumerate(Expr& expr, Expr::ExprId& nextId) -> void;

    /* The original query string of the AST */
    std::string queryString_;

    /* The actual AST */
    ExprPtr expr_;
};

using ASTPtr = std::unique_ptr<AST>;

/** Immutable compiled expression shared by independently bound evaluators. */
using SharedAST = std::shared_ptr<const AST>;
}
