#pragma once

#include "simfil/environment.h"

namespace simfil
{

/**
 * Result value callback.
 * Return `false` to stop evaluation.
 */
enum Result { Continue = 1, Stop = 0 };

struct ResultFn
{
    virtual ~ResultFn() = default;

    virtual auto operator()(Context ctx, Value value) const -> Result = 0;
};

template <class Lambda>
struct LambdaResultFn final : ResultFn
{
    mutable Lambda lambda;

    LambdaResultFn(Lambda ref)
        : lambda(std::move(ref))
    {}

    auto operator()(Context ctx, Value value) const -> Result override
    {
        return lambda(std::move(ctx), std::move(value));
    }
};

}
