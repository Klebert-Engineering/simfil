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

struct ArgumentCountError : std::exception
{
    const Function* fn;
    size_t min;
    size_t max;
    size_t have;

    ArgumentCountError(const Function& fn, size_t min, size_t max, size_t have)
        : fn(&fn), min(min), max(max), have(have)
    {}

    mutable std::string msg;
    auto what() const noexcept -> const char* override;
};

struct ArgumentTypeError : std::exception
{
    const Function* fn;
    size_t index;
    std::string want;
    std::string have;

    ArgumentTypeError(const Function& fn, size_t index, std::string want, std::string have)
        : fn(&fn), index(index), want(std::move(want)), have(std::move(have))
    {}

    mutable std::string msg;
    auto what() const noexcept -> const char* override;
};

struct ArgumentValueCountError : std::exception
{
    const Function* fn;
    size_t index;

    ArgumentValueCountError(const Function& fn, size_t index)
        : fn(&fn), index(index)
    {}

    mutable std::string msg;
    auto what() const noexcept -> const char* override;
};

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
 * Function interface.
 */
class Function
{
public:
    virtual ~Function() = default;

    virtual auto ident() const -> const FnInfo& = 0;
    virtual auto eval(Context, Value, const std::vector<ExprPtr>&, const ResultFn&) const -> Result = 0;
};

class AnyFn : public Function
{
public:
    static AnyFn Fn;

    AnyFn();

    auto ident() const -> const FnInfo& override;
    auto eval(Context, Value, const std::vector<ExprPtr>&, const ResultFn&) const -> Result override;
};

class EachFn : public Function
{
public:
    static EachFn Fn;

    EachFn();

    auto ident() const -> const FnInfo& override;
    auto eval(Context, Value, const std::vector<ExprPtr>&, const ResultFn&) const -> Result override;
};

class CountFn : public Function
{
public:
    static CountFn Fn;

    CountFn();

    auto ident() const -> const FnInfo& override;
    auto eval(Context, Value, const std::vector<ExprPtr>&, const ResultFn&) const -> Result override;
};

class TraceFn : public Function
{
public:
    static TraceFn Fn;

    TraceFn();

    auto ident() const -> const FnInfo& override;
    auto eval(Context, Value, const std::vector<ExprPtr>&, const ResultFn&) const -> Result override;
};

class RangeFn : public Function
{
public:
    static RangeFn Fn;

    RangeFn();

    auto ident() const -> const FnInfo& override;
    auto eval(Context, Value, const std::vector<ExprPtr>&, const ResultFn&) const -> Result override;
};

class ArrFn : public Function
{
public:
    static ArrFn Fn;

    ArrFn();

    auto ident() const -> const FnInfo& override;
    auto eval(Context, Value, const std::vector<ExprPtr>&, const ResultFn&) const -> Result override;
};

class SplitFn : public Function
{
public:
    static SplitFn Fn;

    SplitFn();

    auto ident() const -> const FnInfo& override;
    auto eval(Context, Value, const std::vector<ExprPtr>&, const ResultFn&) const -> Result override;
};

class SelectFn : public Function
{
public:
    static SelectFn Fn;

    SelectFn();

    auto ident() const -> const FnInfo& override;
    auto eval(Context, Value, const std::vector<ExprPtr>&, const ResultFn&) const -> Result override;
};

class SumFn : public Function
{
public:
    static SumFn Fn;

    SumFn();

    auto ident() const -> const FnInfo& override;
    auto eval(Context, Value, const std::vector<ExprPtr>&, const ResultFn&) const -> Result override;
};

class KeysFn : public Function
{
public:
    static KeysFn Fn;

    KeysFn();

    auto ident() const -> const FnInfo& override;
    auto eval(Context, Value, const std::vector<ExprPtr>&, const ResultFn&) const -> Result override;
};

/** Utility functions for working with arguments*/
namespace util
{

inline auto evalArg1Any(Context ctx, Value val, const ExprPtr& expr) -> std::tuple<bool, Value>
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
auto evalArg1(Context ctx, Value val, const ExprPtr& expr, CType fallback = {}) -> std::tuple<bool, CType>
{
    auto&& [ok, value] = evalArg1Any(ctx, val, expr);
    if (ok)
        return {ok, value.as<ValueType4CType<CType>::Type>()};
    return {ok, std::move(fallback)};
}

}
}
