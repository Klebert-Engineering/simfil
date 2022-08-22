// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#include "simfil/value.h"
#include "simfil/environment.h"

#include <functional>
#include <memory>

namespace simfil
{

class Expr
{
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

    virtual ~Expr() {}

    /* Category */
    virtual auto type() const -> Type = 0;

    /* Debug */
    virtual auto toString() const -> std::string = 0;

    /* Evaluation wrapper */
    auto eval(Context ctx, Value val, ResultFn res) const -> Result
    {
        auto dbg = ctx.env->debug;
        if (dbg) dbg->evalBegin(*this, ctx, val, res);
        auto r = ieval(std::move(ctx), std::move(val), std::move(res));
        if (dbg) dbg->evalEnd(*this);
        return r;
    }

private:
    /* Abstract evaluation implementation */
    virtual auto ieval(Context, Value, ResultFn) const -> Result = 0;
};

using ExprPtr = std::unique_ptr<Expr>;

}
