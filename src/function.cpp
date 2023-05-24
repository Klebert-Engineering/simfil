#include "simfil/function.h"

#include "simfil/result.h"
#include "simfil/operator.h"
#include "simfil/environment.h"
#include "simfil/model/model.h"
#include "simfil/types.h"
#include "simfil/overlay.h"
#include "stx/string.h"
#include "stx/format.h"

#include <iostream>
#include <optional>

namespace simfil
{

using namespace std::string_literals;

auto ArgumentCountError::what() const noexcept -> const char*
{
    if (min < max)
        msg = stx::format("{}: Expected {} to {} arguments; got {}",
                          fn->ident().ident, min, max, have);
    else
        msg = stx::format("{}: Expected {} arguments; got {}",
                          fn->ident().ident, min, have);
    return msg.c_str();
}

auto ArgumentValueCountError::what() const noexcept -> const char*
{
    msg = stx::format("{}: Argument {} must be a single value",
                      fn->ident().ident, index);
    return msg.c_str();
}

auto ArgumentTypeError::what() const noexcept -> const char*
{
    msg = stx::format("{}: Expected argument {} to be of type {}; got {}",
                      fn->ident().ident, index, want, have);
    return msg.c_str();
}

namespace
{

struct ArgParser
{
    const std::string fname;
    const std::vector<ExprPtr>& args;
    Value value;
    Context ctx;
    std::size_t idx = 0;
    bool anyUndef = false;

    ArgParser(std::string fname, Context ctx, Value val, const std::vector<ExprPtr>& args, size_t idx = 0)
        : fname(fname)
        , args(args)
        , ctx(ctx)
        , value(std::move(val))
        , idx(idx)
    {
        if (args.size() < idx)
            throw std::runtime_error(fname + ": too few arguments"s);
    }

    auto arg(const char* name, ValueType type, Value& outValue) -> ArgParser&
    {
        if (args.size() <= idx)
            throw std::runtime_error(fname + ": missing argument "s + name);

        auto subctx = ctx;
        args[idx]->eval(subctx, value, LambdaResultFn([&, n = 0](Context, Value vv) mutable {
            if (++n > 1)
                throw std::runtime_error(fname + ": argument "s + name + " must return a single value"s);

            if (vv.isa(ValueType::Undef)) {
                anyUndef = true;
                outValue = std::move(vv);
                return Result::Continue;
            }

            if (!vv.isa(type))
                throw std::runtime_error(fname + ": invalid value type for argument '"s + name + "'"s);

            outValue = std::move(vv);

            return Result::Continue;
        }));

        ++idx;
        return *this;
    }

    auto opt(const char* name, ValueType type, Value& outValue, Value def, bool* set = nullptr) -> ArgParser&
    {
        if (args.size() <= idx) {
            outValue = std::move(def);
            if (set) *set = false;
            return *this;
        }

        auto subctx = ctx;
        args[idx]->eval(subctx, value, LambdaResultFn([&, n = 0](Context, Value vv) mutable {
            if (++n > 1)
                throw std::runtime_error(fname + ": argument "s + name + " must return a single value"s);

            if (vv.isa(ValueType::Undef)) {
                anyUndef = true;
                outValue = std::move(vv);
                return Result::Continue;
            }

            if (!vv.isa(type))
                throw std::runtime_error(fname + ": invalid value type for argument '"s + name + "'"s);

            outValue = std::move(vv);
            if (set) *set = true;
            return Result::Continue;
        }));

        ++idx;
        return *this;
    }

    auto ok() const -> bool
    {
        return !anyUndef;
    }
};

auto boolify(Value v) -> bool
{
    /* Needed because DispatchOperator* returns
     * Undef if any argument is Undef. */
    if (v.isa(ValueType::Undef))
        return false;
    return UnaryOperatorDispatcher<OperatorBool>::dispatch(std::move(v)).as<ValueType::Bool>();
}
}

AnyFn AnyFn::Fn;
AnyFn::AnyFn()
{}

auto AnyFn::ident() const -> const FnInfo&
{
    static const FnInfo info{
        "any",
        "Returns true if any expr returned a positive result.",
        "any(expr...) -> <bool>"
    };
    return info;
}

auto AnyFn::eval(Context ctx, Value val, const std::vector<ExprPtr>& args, const ResultFn& res) const -> Result
{
    if (args.size() < 1)
        throw std::runtime_error("any(...) expects one argument; got "s + std::to_string(args.size()));

    auto subctx = ctx;
    auto result = false; /* At least one value is true  */
    auto undef = false;  /* At least one value is undef */

    for (const auto& arg : args) {
        arg->eval(ctx, val, LambdaResultFn([&](Context, Value vv) {
            if (ctx.phase == Context::Phase::Compilation) {
                if (vv.isa(ValueType::Undef)) {
                    undef = true;
                    return Result::Stop;
                }
            }

            result = result || boolify(std::move(vv));
            return result ? Result::Stop : Result::Continue;
        }));

        if (result || undef)
            break;
    }

    if (undef)
        return res(subctx, Value::undef());
    return res(subctx, Value::make(result));
}

EachFn EachFn::Fn;
EachFn::EachFn()
{}

auto EachFn::ident() const -> const FnInfo&
{
    static const FnInfo info{
        "each",
        "Returns true if all expr returned a positive result.",
        "each(expr...) -> <bool>"
    };
    return info;
}

auto EachFn::eval(Context ctx, Value val, const std::vector<ExprPtr>& args, const ResultFn& res) const -> Result
{
    if (args.size() < 1)
        throw std::runtime_error("each(...) expects one argument; got "s + std::to_string(args.size()));

    auto subctx = ctx;
    auto result = true; /* All values are true  */
    auto undef = false; /* At least one value is undef */

    for (const auto& arg : args) {
        arg->eval(ctx, val, LambdaResultFn([&](Context, Value vv) {
            if (ctx.phase == Context::Phase::Compilation) {
                if (vv.isa(ValueType::Undef)) {
                    undef = true;
                    return Result::Stop;
                }
            }
            result = result && boolify(std::move(vv));
            return result ? Result::Continue : Result::Stop;
        }));

        if (!result || undef)
            break;
    }

    if (undef)
        return res(subctx, Value::undef());
    return res(subctx, Value::make(result));
}

CountFn CountFn::Fn;
CountFn::CountFn()
{}

auto CountFn::ident() const -> const FnInfo&
{
    static const FnInfo info{
        "count",
        "Counts positive results (non-null and non-false)",
        "count(expr...) -> <int>"
    };
    return info;
}

auto CountFn::eval(Context ctx, Value val, const std::vector<ExprPtr>& args, const ResultFn& res) const -> Result
{
    if (args.size() < 1)
        throw std::runtime_error("count(...) expects one argument; got "s + std::to_string(args.size()));

    auto subctx = ctx;
    auto undef = false; /* At least one value is undef */
    int64_t count = 0;

    for (const auto& arg : args) {
        arg->eval(ctx, val, LambdaResultFn([&](Context, Value vv) {
            if (ctx.phase == Context::Phase::Compilation) {
                if (vv.isa(ValueType::Undef)) {
                    undef = true;
                    return Result::Stop;
                }
            }
            count += boolify(std::move(vv)) ? 1 : 0;
            return Result::Continue;
        }));

        if (undef)
            break;
    }

    if (undef)
        return res(subctx, Value::undef());
    return res(subctx, Value::make(count));
}

TraceFn TraceFn::Fn;
TraceFn::TraceFn()
{}

auto TraceFn::ident() const -> const FnInfo&
{
    static const FnInfo info{
        "trace",
        "Meassures execution time of expr and collects up to limit results.",
        "trace(expr, [limit = -1], [name = auto]) -> <irange>"
    };
    return info;
}

auto TraceFn::eval(Context ctx, Value val, const std::vector<ExprPtr>& args, const ResultFn& res) const -> Result
{
    Value name  = Value::undef();
    Value limit = Value::undef();

    auto ok = ArgParser("trace", ctx, val, args, 1)
        /* Skip arg 0 */
        .opt("limit", ValueType::Int, limit, Value::make((int64_t)-1))
        .opt("name",  ValueType::String, name, Value::make(args[0]->toString()))
        .ok();

    /* Never run in compilation phase */
    if (ctx.phase == Context::Phase::Compilation)
        return res(ctx, Value::undef());

    auto sname = name.as<ValueType::String>();
    auto ilimit = limit.as<ValueType::Int>();
    auto values = std::vector<Value>();

    auto start = std::chrono::steady_clock::now();
    auto result = args[0]->eval(ctx, val, LambdaResultFn([&, n = 0](Context ctx, Value vv) mutable {
        if (ilimit < 0 || n++ <= ilimit) {
            // Do not allow string view to leak into the trace result.
            auto copy = vv;
            if (auto sv = vv.stringViewValue())
                copy = Value::make(std::string(*sv));
            values.emplace_back(std::move(copy));
        }
        return res(std::move(ctx), std::move(vv));
    }));
    auto duration = std::chrono::steady_clock::now() - start;

    ctx.env->trace(sname, [&](auto& t) {
        t.totalus += std::chrono::duration_cast<std::chrono::microseconds>(duration);
        t.calls += 1;
        if (ilimit < 0 || (int)t.values.size() < ilimit)
            t.values.insert(t.values.end(),
                            std::make_move_iterator(values.begin()),
                            std::make_move_iterator(values.end()));
        if (ilimit >= 0 && (int)t.values.size() > ilimit)
            t.values.resize(ilimit, Value::undef());
    });

    return result;
}


RangeFn RangeFn::Fn;
RangeFn::RangeFn()
{}

auto RangeFn::ident() const -> const FnInfo&
{
    static const FnInfo info{
        "range",
        "Returns an integer-range object from begin to end.",
        "range(start, end) -> <irange>"
    };
    return info;
}

auto RangeFn::eval(Context ctx, Value val, const std::vector<ExprPtr>& args, const ResultFn& res) const -> Result
{
    if (args.size() != 2)
        throw std::runtime_error("range(begin, end) expects 2 arguments; got "s + std::to_string(args.size()));

    Value begin = Value::undef();
    Value end = Value::undef();

    auto ok = ArgParser("range", ctx, val, args)
        .arg("begin", ValueType::Int, begin)
        .arg("end",   ValueType::Int, end)
        .ok();

    if (!ok)
        return res(ctx, Value::undef());

    auto ibegin = begin.as<ValueType::Int>();
    auto iend = end.as<ValueType::Int>();
    return res(ctx, IRangeType::Type.make(ibegin, iend));
}

ArrFn ArrFn::Fn;
ArrFn::ArrFn()
{}

auto ArrFn::ident() const -> const FnInfo&
{
    static const FnInfo info{
        "arr",
        "Returns a list of values.",
        "arr(values...) -> <any>"
    };
    return info;
}


auto ArrFn::eval(Context ctx, Value val, const std::vector<ExprPtr>& args, const ResultFn& res) const -> Result
{
    if (args.empty())
        return res(ctx, Value::null());

    for (const auto& arg : args) {
        if (arg->eval(ctx, val, LambdaResultFn([&res](Context ctx, Value vv) {
            return res(ctx, std::move(vv));
        })) == Result::Stop)
            return Result::Stop;
    }

    return Result::Continue;
}

SplitFn SplitFn::Fn;
SplitFn::SplitFn()
{}

auto SplitFn::ident() const -> const FnInfo&
{
    static const FnInfo info{
        "split",
        "Splits a string into substrings, divided at separator.",
        "split(string, separator, keepEmpty = true) -> <string>"
    };
    return info;
}

auto SplitFn::eval(Context ctx, Value val, const std::vector<ExprPtr>& args, const ResultFn& res) const -> Result
{
    Value str = Value::undef();
    Value sep = Value::undef();
    Value keepEmpty = Value::undef();

    auto ok = ArgParser("split", ctx, val, args)
        .arg("string", ValueType::String, str)
        .arg("separator", ValueType::String, sep)
        .opt("keepEmpty", ValueType::Bool, keepEmpty, Value::t())
        .ok();

    auto subctx = ctx;
    if (!ok)
        return res(subctx, Value::undef());

    auto items = stx::split(str.as<ValueType::String>(), sep.as<ValueType::String>(), !keepEmpty.as<ValueType::Bool>());
    for (auto&& item : std::move(items)) {
        if (res(subctx, Value::make(std::move(item))) == Result::Stop)
            break;
    }

    return Result::Continue;
};

SelectFn SelectFn::Fn;
SelectFn::SelectFn()
{}

auto SelectFn::ident() const -> const FnInfo&
{
    static const FnInfo info{
        "select",
        "Selects a range of input values.",
        "select(input, start, [length = 1]) -> <any>"
    };
    return info;
}

auto SelectFn::eval(Context ctx, Value val, const std::vector<ExprPtr>& args, const ResultFn& res) const -> Result
{
    Value idx = Value::undef();
    Value cnt = Value::undef();

    auto ok = ArgParser("select", ctx, val, args, 1)
        /* Skip arg 0 */
        .arg("index", ValueType::Int, idx)
        .opt("limit", ValueType::Int, cnt, Value::make((int64_t)1))
        .ok();

    if (!ok)
        res(ctx, Value::undef());

    auto iidx = idx.as<ValueType::Int>();
    auto icnt = cnt.as<ValueType::Int>();
    if (icnt <= 0)
        icnt = std::numeric_limits<int>::max();

    auto result = args[0]->eval(ctx, val, LambdaResultFn([&, n = -1](Context ctx, Value vv) mutable {
        ++n;
        if (ctx.phase == Context::Phase::Compilation)
            if (vv.isa(ValueType::Undef))
                return res(ctx, std::move(vv));
        if (n >= iidx + icnt)
            return Result::Stop;
        if (n >= iidx)
            return res(ctx, std::move(vv));
        return Result::Continue;
    }));

    return result;
}

SumFn SumFn::Fn;
SumFn::SumFn()
{}

auto SumFn::ident() const -> const FnInfo&
{
    static const FnInfo info{
        "sum",
        "Sum its $input values using expresison $expr, starting at $init.",
        "sum(input, [expr = `$sum + $val`], [init = 0]) -> <any>"
    };
    return info;
}

auto SumFn::eval(Context ctx, Value val, const std::vector<ExprPtr>& args, const ResultFn& res) const -> Result
{
    if (args.size() < 1 || args.size() > 3)
        throw std::runtime_error("sum: Expected at least 1 argument; got "s + std::to_string(args.size()));

    Value sum = Value::make((int64_t)0);

    const Expr* subexpr = args.size() >= 2 ? args[1].get() : nullptr;
    const Expr* initval = args.size() == 3 ? args[2].get() : nullptr;
    if (initval)
        (void)initval->eval(ctx, val, LambdaResultFn([&](Context ctx, Value vv) {
            sum = std::move(vv);
            return Result::Continue;
        }));

    (void)args[0]->eval(ctx, val, LambdaResultFn([&, n = 0](Context ctx, Value vv) mutable {
        if (subexpr) {
            auto ov = shared_model_ptr<OverlayNode>::make(vv);
            ov->set(Fields::OverlaySum, sum);
            ov->set(Fields::OverlayValue, vv);
            ov->set(Fields::OverlayIndex, Value::make((int64_t)n++));

            subexpr->eval(ctx, Value::field(ov), LambdaResultFn([&ov, &sum](auto ctx, auto vv) {
                ov->set(Fields::OverlaySum, vv);
                sum = vv;
                return Result::Continue;
            }));
        } else {
            if (sum.isa(ValueType::Null)) {
                sum = std::move(vv);
            } else {
                sum = BinaryOperatorDispatcher<OperatorAdd>::dispatch(sum, std::move(vv));
            }
        }

        return Result::Continue;
    }));

    return res(std::move(ctx), sum);
}

KeysFn KeysFn::Fn;
KeysFn::KeysFn()
{}

auto KeysFn::ident() const -> const FnInfo&
{
    static const FnInfo info{
        "keys",
        "Returns the keys of its input objects.",
        "keys(<any>) -> <string...>"
    };
    return info;
}

auto KeysFn::eval(Context ctx, Value val, const std::vector<ExprPtr>& args, const ResultFn& res) const -> Result
{
    if (args.size() != 1)
        throw std::runtime_error("keys: Expected 1 argument; got "s + std::to_string(args.size()));

    auto result = args[0]->eval(ctx, val, LambdaResultFn([&res](Context ctx, Value vv) {
        if (ctx.phase == Context::Phase::Compilation)
            if (vv.isa(ValueType::Undef))
                return res(ctx, std::move(vv));

        if (vv.node)
            for (auto&& fieldName : vv.node->fieldNames()) {
                if (auto key = ctx.env->fieldNames_->resolve(fieldName)) {
                    if (res(ctx, Value::strref(*key)) == Result::Stop)
                        return Result::Stop;
                }
            }
        return Result::Continue;
    }));

    return result;
}

}
