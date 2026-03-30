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
 * Function interface.
 */
class Function
{
public:
    virtual ~Function() = default;

    virtual auto ident() const -> const FnInfo& = 0;
    virtual auto eval(Context, Value, const std::vector<ExprPtr>&) const -> EvalStream = 0;
};

class CountFn : public Function
{
public:
    static CountFn Fn;

    CountFn();

    auto ident() const -> const FnInfo& override;
    auto eval(Context, Value, const std::vector<ExprPtr>&) const -> EvalStream override;
};

class TraceFn : public Function
{
public:
    static TraceFn Fn;

    TraceFn();

    auto ident() const -> const FnInfo& override;
    auto eval(Context, Value, const std::vector<ExprPtr>&) const -> EvalStream override;
};

class RangeFn : public Function
{
public:
    static RangeFn Fn;

    RangeFn();

    auto ident() const -> const FnInfo& override;
    auto eval(Context, Value, const std::vector<ExprPtr>&) const -> EvalStream override;
};

class ReFn : public Function
{
public:
    static ReFn Fn;

    ReFn();

    auto ident() const -> const FnInfo& override;
    auto eval(Context, Value, const std::vector<ExprPtr>&) const -> EvalStream override;
};

class ArrFn : public Function
{
public:
    static ArrFn Fn;

    ArrFn();

    auto ident() const -> const FnInfo& override;
    auto eval(Context, Value, const std::vector<ExprPtr>&) const -> EvalStream override;
};

class SplitFn : public Function
{
public:
    static SplitFn Fn;

    SplitFn();

    auto ident() const -> const FnInfo& override;
    auto eval(Context, Value, const std::vector<ExprPtr>&) const -> EvalStream override;
};

class SelectFn : public Function
{
public:
    static SelectFn Fn;

    SelectFn();

    auto ident() const -> const FnInfo& override;
    auto eval(Context, Value, const std::vector<ExprPtr>&) const -> EvalStream override;
};

class SumFn : public Function
{
public:
    static SumFn Fn;

    SumFn();

    auto ident() const -> const FnInfo& override;
    auto eval(Context, Value, const std::vector<ExprPtr>&) const -> EvalStream override;
};

class KeysFn : public Function
{
public:
    static KeysFn Fn;

    KeysFn();

    auto ident() const -> const FnInfo& override;
    auto eval(Context, Value, const std::vector<ExprPtr>&) const -> EvalStream override;
};

/** Utility functions for working with arguments*/
namespace util
{

inline auto evalArg1Any(Context ctx, const Value& val, const ExprPtr& expr) -> std::tuple<bool, Value>
{
    if (!expr)
        return {false, Value::undef()};

    auto n = 0u;
    std::optional<Value> out ;
    for (auto value : expr->eval(ctx, val)) {
        n++;
        out.emplace(*value);
    }

    return {n == 1, std::move(out).value_or(Value::undef())};
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
