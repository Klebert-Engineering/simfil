#include "simfil/expression.h"

namespace simfil
{

/** Charge both const and move emissions before forwarding, without an intermediate Value copy. */
class Expr::BoundedResultFn : public ResultFn
{
public:
    /** Borrow the downstream callback only for the current expression invocation. */
    explicit BoundedResultFn(const ResultFn& next) : next_(next) {}
    auto operator()(Context ctx, const Value& value) const noexcept -> tl::expected<Result, Error> override
    {
        return ctx.step() ? next_(ctx, value) : tl::expected<Result, Error>(Result::Stop);
    }
    auto operator()(Context ctx, Value&& value) const noexcept -> tl::expected<Result, Error> override
    {
        return ctx.step() ? next_(ctx, std::move(value)) : tl::expected<Result, Error>(Result::Stop);
    }
private:
    const ResultFn& next_;
};

template<class T>
auto Expr::evalBoundedImpl(Context ctx, T&& value, const ResultFn& result) const -> tl::expected<Result, Error>
{
    if (!ctx.step(++ctx.evaluationDepth))
        return Result::Stop;
    BoundedResultFn guarded(result);
    if (auto debug = ctx.env->debug) {
        auto copy = Value(std::forward<T>(value));
        debug->evalBegin(*this, ctx, copy, guarded);
        auto status = ieval(ctx, std::move(copy), guarded);
        debug->evalEnd(*this);
        return status;
    }
    return ieval(ctx, std::forward<T>(value), guarded);
}

auto Expr::evalBounded(Context ctx, const Value& value, const ResultFn& result) const -> tl::expected<Result, Error>
{
    return evalBoundedImpl(ctx, value, result);
}

auto Expr::evalBounded(Context ctx, Value&& value, const ResultFn& result) const -> tl::expected<Result, Error>
{
    return evalBoundedImpl(ctx, std::move(value), result);
}

}
