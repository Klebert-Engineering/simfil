// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#include "simfil/result.h"
#include "simfil/value.h"
#include "simfil/expression.h"
#include "simfil/environment.h"

#include <vector>
#include <tuple>

namespace simfil
{
struct Context;

/**
 * Function info.
 */
struct FnInfo
{
    std::string ident;
    std::string description;
    std::string signature;
};

/**
 * Registered query function receiving unevaluated argument expressions and a
 * synchronous result sink. Implementations choose argument cardinality, propagate
 * errors/Stop and return Undef during compilation when runtime input is needed.
 * A successful call must emit at least one value; emit null for an empty result.
 * CallExpression rejects successful returns that never invoked the sink.
 * Long non-emitting loops must cooperate through Context::step().
 */
class Function
{
public:
    virtual ~Function() = default;

    virtual auto ident() const -> const FnInfo& = 0;
    virtual auto eval(Context, const Value&, const std::vector<ExprPtr>&, const ResultFn&) const -> tl::expected<Result, Error> = 0;
};

/** Count truthy values across argument streams, preserving unknown compilation inputs. */
class CountFn : public Function
{
public:
    static CountFn Fn;

    CountFn();

    auto ident() const -> const FnInfo& override;
    auto eval(Context, const Value&, const std::vector<ExprPtr>&, const ResultFn&) const -> tl::expected<Result, Error> override;
};

/** Pass through an expression's stream while recording execution time and retained sample values. */
class TraceFn : public Function
{
public:
    static TraceFn Fn;

    TraceFn();

    auto ident() const -> const FnInfo& override;
    auto eval(Context, const Value&, const std::vector<ExprPtr>&, const ResultFn&) const -> tl::expected<Result, Error> override;
};

/** Construct an inclusive ascending or descending integer range; unpack it with `...`. */
class RangeFn : public Function
{
public:
    static RangeFn Fn;

    RangeFn();

    auto ident() const -> const FnInfo& override;
    auto eval(Context, const Value&, const std::vector<ExprPtr>&, const ResultFn&) const -> tl::expected<Result, Error> override;
};

/** Compile each string argument value into a regex, or pass through an existing regex object. */
class ReFn : public Function
{
public:
    static ReFn Fn;

    ReFn();

    auto ident() const -> const FnInfo& override;
    auto eval(Context, const Value&, const std::vector<ExprPtr>&, const ResultFn&) const -> tl::expected<Result, Error> override;
};

/** Concatenate argument result streams in order; zero arguments emit one null value. */
class ArrFn : public Function
{
public:
    static ArrFn Fn;

    ArrFn();

    auto ident() const -> const FnInfo& override;
    auto eval(Context, const Value&, const std::vector<ExprPtr>&, const ResultFn&) const -> tl::expected<Result, Error> override;
};

/** Stream separated string pieces, optionally retaining empties; an empty input emits no values. */
class SplitFn : public Function
{
public:
    static SplitFn Fn;

    SplitFn();

    auto ident() const -> const FnInfo& override;
    auto eval(Context, const Value&, const std::vector<ExprPtr>&, const ResultFn&) const -> tl::expected<Result, Error> override;
};

/** Select a zero-based window from an argument stream and stop upstream once the window is exhausted. */
class SelectFn : public Function
{
public:
    static SelectFn Fn;

    SelectFn();

    auto ident() const -> const FnInfo& override;
    auto eval(Context, const Value&, const std::vector<ExprPtr>&, const ResultFn&) const -> tl::expected<Result, Error> override;
};

/** Fold an input stream with addition or a custom expression using $sum, $val and $idx. */
class SumFn : public Function
{
public:
    static SumFn Fn;

    SumFn();

    auto ident() const -> const FnInfo& override;
    auto eval(Context, const Value&, const std::vector<ExprPtr>&, const ResultFn&) const -> tl::expected<Result, Error> override;
};

/** Returns the smallest non-null value emitted by its argument expressions. */
class MinFn : public Function
{
public:
    static MinFn Fn;

    MinFn();

    auto ident() const -> const FnInfo& override;
    auto eval(Context, const Value&, const std::vector<ExprPtr>&, const ResultFn&) const -> tl::expected<Result, Error> override;
};

/** Returns the largest non-null value emitted by its argument expressions. */
class MaxFn : public Function
{
public:
    static MaxFn Fn;

    MaxFn();

    auto ident() const -> const FnInfo& override;
    auto eval(Context, const Value&, const std::vector<ExprPtr>&, const ResultFn&) const -> tl::expected<Result, Error> override;
};

/** Stream names of enumerable members from each input node, using the environment's dictionary. */
class KeysFn : public Function
{
public:
    static KeysFn Fn;

    KeysFn();

    auto ident() const -> const FnInfo& override;
    auto eval(Context, const Value&, const std::vector<ExprPtr>&, const ResultFn&) const -> tl::expected<Result, Error> override;
};

/**
 * Explicit schema-declared enum/named-value membership below a value. Unlike
 * shorthand, this predicate keeps its meaning inside generated boolean filters.
 */
class HasSymbolFn : public Function
{
public:
    static HasSymbolFn Fn;

    auto ident() const -> const FnInfo & override;
    auto eval(Context, const Value &, const std::vector<ExprPtr> &,
              const ResultFn &) const -> tl::expected<Result, Error> override;
};

/** Explicit all-bits membership using the mask's exact type and declared flag name. */
class HasFlagFn : public Function
{
public:
    static HasFlagFn Fn;

    /** Describe the membership primitive for completion and help. */
    auto ident() const -> const FnInfo& override;

    /** Evaluate one typed mask and one declared flag name, propagating errors and undefined values. */
    auto eval(Context, const Value&, const std::vector<ExprPtr>&, const ResultFn&) const -> tl::expected<Result, Error> override;
};

/** Construct exact unsigned bits using an existing mask's type, without relabeling another mask. */
class BitmaskFn : public Function
{
public:
    static BitmaskFn Fn;

    /** Describe lossless string construction for completion and help. */
    auto ident() const -> const FnInfo& override;

    /** Parse one raw-bits string and validate the resulting mask against the selected type width. */
    auto eval(Context, const Value&, const std::vector<ExprPtr>&, const ResultFn&) const -> tl::expected<Result, Error> override;
};

/** Utility functions for working with arguments*/
namespace util
{

/**
 * Evaluate an argument and report whether exactly one value was observed, retaining
 * the last value. This legacy convenience discards evaluation errors; use direct
 * callback evaluation when failures must be distinguished from missing arguments.
 */
inline auto evalArg1Any(Context ctx, const Value& val, const ExprPtr& expr) -> std::tuple<bool, Value>
{
    if (!expr)
        return {false, Value::undef()};
    auto n = 0;
    auto out = Value::undef();
    (void)expr->eval(ctx, val, LambdaResultFn([&n, &out](auto, Value v) {
        ++n;
        out = std::move(v);
        return Result::Continue;
    }));

    return {n == 1, std::move(out)};
}

template <class CType>
auto evalArg1(Context ctx, const Value& val, const ExprPtr& expr, CType fallback = {}) -> std::tuple<bool, CType>
{
    auto&& [ok, value] = evalArg1Any(ctx, val, expr);
    if (ok)
        return {ok, value.as<ValueType4CType<CType>::Type>()};
    return {ok, std::move(fallback)};
}

}
}
