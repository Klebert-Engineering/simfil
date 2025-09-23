#pragma once

#include <tl/expected.hpp>

#include "simfil/environment.h"
#include "simfil/error.h"

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

    virtual auto operator()(Context ctx, const Value& value) const noexcept -> tl::expected<Result, Error> = 0;
    virtual auto operator()(Context ctx, Value&& value) const noexcept -> tl::expected<Result, Error> = 0;
};

template <class Lambda>
struct LambdaResultFn final : ResultFn
{
    mutable Lambda lambda;

    LambdaResultFn(Lambda ref)
        : lambda(std::move(ref))
    {}

    auto operator()(Context ctx, const Value& value) const noexcept -> tl::expected<Result, Error> override
    {
        if constexpr (std::is_invocable_v<Lambda, Context, const Value&>) {
            return lambda(std::move(ctx), value);
        } else {
            return lambda(std::move(ctx), Value{value});
        }
    }

    auto operator()(Context ctx, Value&& value) const noexcept -> tl::expected<Result, Error> override
    {
        return lambda(std::move(ctx), std::move(value));
    }
};

}
