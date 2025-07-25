#include "simfil/function.h"

#include "simfil/model/nodes.h"
#include "simfil/result.h"
#include "simfil/operator.h"
#include "simfil/environment.h"
#include "simfil/model/model.h"
#include "simfil/types.h"
#include "simfil/overlay.h"
#include "fmt/core.h"

#include <optional>
#include <stdexcept>

namespace simfil
{

using namespace std::string_literals;

auto ArgumentCountError::what() const noexcept -> const char*
{
    if (min < max)
        msg = fmt::format("{}: Expected {} to {} arguments; got {}",
                          fn->ident().ident, min, max, have);
    else
        msg = fmt::format("{}: Expected {} arguments; got {}",
                          fn->ident().ident, min, have);
    return msg.c_str();
}

auto ArgumentValueCountError::what() const noexcept -> const char*
{
    msg = fmt::format("{}: Argument {} must be a single value",
                      fn->ident().ident, index);
    return msg.c_str();
}

auto ArgumentTypeError::what() const noexcept -> const char*
{
    msg = fmt::format("{}: Expected argument {} to be of type {}; got {}",
                      fn->ident().ident, index, want, have);
    return msg.c_str();
}

namespace
{

struct ArgParser
{
    const std::string functionName;
    const std::vector<ExprPtr>& args;
    Value value;
    Context ctx;
    std::size_t idx = 0;
    bool anyUndef = false;

    ArgParser(const std::string& functionName, Context ctx, Value val, const std::vector<ExprPtr>& args, size_t idx = 0)
        : functionName(functionName)
        , args(args)
        , value(std::move(val))
        , ctx(ctx)
        , idx(idx)
    {
        if (args.size() < idx)
            raise<std::runtime_error>(functionName + ": too few arguments"s);
    }

    auto arg(const char* name, ValueType type, Value& outValue) -> ArgParser&
    {
        if (args.size() <= idx)
            raise<std::runtime_error>(functionName + ": missing argument "s + name);

        auto subctx = ctx;
        args[idx]->eval(subctx, value, LambdaResultFn([&, n = 0](Context, Value vv) mutable {
            if (++n > 1)
                raise<std::runtime_error>(functionName + ": argument "s + name + " must return a single value"s);

            if (vv.isa(ValueType::Undef)) {
                anyUndef = true;
                outValue = std::move(vv);
                return Result::Continue;
            }

            if (!vv.isa(type))
                raise<std::runtime_error>(functionName + ": invalid value type for argument '"s + name + "'"s);

            outValue = std::move(vv);

            return Result::Continue;
        }));

        ++idx;
        return *this;
    }

    auto opt(std::string_view name, ValueType type, Value& outValue, Value def) -> ArgParser&
    {
        if (args.size() <= idx) {
            outValue = std::move(def);
            return *this;
        }

        auto subctx = ctx;
        args[idx]->eval(subctx, value, LambdaResultFn([&, n = 0](Context, Value vv) mutable {
            if (++n > 1)
                raise<std::runtime_error>(fmt::format("{}: argument {} must return a single value", functionName, name));

            if (vv.isa(ValueType::Undef)) {
                anyUndef = true;
                outValue = std::move(vv);
                return Result::Continue;
            }

            if (!vv.isa(type))
                raise<std::runtime_error>(fmt::format("{}: invalid value type for argument", functionName, name));

            outValue = std::move(vv);
            return Result::Continue;
        }));

        ++idx;
        return *this;
    }

    [[nodiscard]]
    auto ok() const -> bool
    {
        return !anyUndef;
    }
};

auto boolify(const Value& v) -> bool
{
    /* Needed because DispatchOperator* returns
     * Undef if any argument is Undef. */
    if (v.isa(ValueType::Undef))
        return false;
    return UnaryOperatorDispatcher<OperatorBool>::dispatch(v).as<ValueType::Bool>();
}
}

CountFn CountFn::Fn;
CountFn::CountFn() = default;

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
    if (args.empty())
        raise<std::runtime_error>("count(...) expects one argument; got "s + std::to_string(args.size()));

    auto subctx = ctx;
    auto undef = false; /* At least one value is undef */
    int64_t count = 0;

    for (const auto& arg : args) {
        arg->eval(ctx, val, LambdaResultFn([&](Context, const Value& vv) {
            if (ctx.phase == Context::Phase::Compilation) {
                if (vv.isa(ValueType::Undef)) {
                    undef = true;
                    return Result::Stop;
                }
            }
            count += boolify(vv) ? 1 : 0;
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
TraceFn::TraceFn() = default;

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

    (void)ArgParser("trace", ctx, val, args, 1)
        /* Skip arg 0 */
        .opt("limit", ValueType::Int, limit, Value::make(static_cast<int64_t>(-1)))
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
        return res(ctx, std::move(vv));
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
RangeFn::RangeFn() = default;

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
        raise<std::runtime_error>("range(begin, end) expects 2 arguments; got "s + std::to_string(args.size()));

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

ReFn ReFn::Fn;
ReFn::ReFn() = default;

auto ReFn::ident() const -> const FnInfo&
{
    static const FnInfo info{
        "re",
        "Returns a compiled regular expression.",
        "re(expr) -> <re>"
    };
    return info;
}

auto ReFn::eval(Context ctx, Value val, const std::vector<ExprPtr>& args, const ResultFn& res) const -> Result
{
    if (args.size() != 1)
        raise<std::runtime_error>("re(expr) expects 1 arguments; got "s + std::to_string(args.size()));

    auto subctx = ctx;
    return args[0]->eval(subctx, val, LambdaResultFn([&](Context, Value vv) {
        if (vv.isa(ValueType::Undef))
            return res(ctx, Value::undef());

        if (vv.isa(ValueType::String))
            return res(ctx, ReType:: Type.make(vv.as<ValueType::String>()));

        // Passing another <re> object is a no-op
        if (vv.isa(ValueType::TransientObject))
            if (const auto obj = vv.as<ValueType::TransientObject>(); obj.meta == &ReType::Type)
                return res(ctx, std::move(vv));

        raise<std::runtime_error>("re: invalid value type for argument 'expr'"s);
    }));
}

ArrFn ArrFn::Fn;
ArrFn::ArrFn() = default;

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
SplitFn::SplitFn() = default;

auto SplitFn::ident() const -> const FnInfo&
{
    static const FnInfo info{
        "split",
        "Splits a string into substrings, divided at separator.",
        "split(string, separator, keepEmpty = true) -> <string>"
    };
    return info;
}

namespace {
template <class ContainerType = std::vector<std::string>>
ContainerType split(std::string_view what,
                 std::string_view at,
                 bool removeEmpty = true)
{
    using ResultType = typename ContainerType::value_type;

    ContainerType container;
    auto out = std::back_inserter(container);

    /* Special case: empty `what` */
    if (what.empty())
        return container;

    /* Special case: empty `at` */
    if (at.empty()) {
        *out++ = ResultType(what);
        return container;
    }

    std::string::size_type begin{};
    std::string::size_type end{};

    auto next = [&]() {
        if ((end = what.find(at, begin)) != std::string::npos) {
            if ((end - begin) != 0 || !removeEmpty)
                *out++ = ResultType(what).substr(begin, end - begin);

            begin = end + at.size();
            return true;
        }

        if (what.size() - begin || !removeEmpty)
            *out++ = ResultType(what).substr(begin);

        return false;
    };

    while (next()) { /* noop */ }

    return container;
}
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

    auto items = split(str.as<ValueType::String>(), sep.as<ValueType::String>(), !keepEmpty.as<ValueType::Bool>());
    for (auto&& item : items) {
        if (res(subctx, Value::make(std::move(item))) == Result::Stop)
            break;
    }

    return Result::Continue;
};

SelectFn SelectFn::Fn;
SelectFn::SelectFn() = default;

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
        .opt("limit", ValueType::Int, cnt, Value::make(static_cast<int64_t>(1)))
        .ok();

    if (!ok)
        return res(ctx, Value::undef());

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
SumFn::SumFn() = default;

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
    if (args.empty() || args.size() > 3)
        raise<std::runtime_error>("sum: Expected at least 1 argument; got "s + std::to_string(args.size()));

    Value sum = Value::make(static_cast<int64_t>(0));

    Expr* subexpr = args.size() >= 2 ? args[1].get() : nullptr;
    Expr* initval = args.size() == 3 ? args[2].get() : nullptr;
    if (initval)
        (void)initval->eval(ctx, val, LambdaResultFn([&](Context ctx, Value vv) {
            sum = std::move(vv);
            return Result::Continue;
        }));

    (void)args[0]->eval(ctx, val, LambdaResultFn([&, n = 0](Context ctx, Value vv) mutable {
        if (subexpr) {
            auto ov = model_ptr<OverlayNode>::make(vv);
            ov->set(StringPool::OverlaySum, sum);
            ov->set(StringPool::OverlayValue, vv);
            ov->set(StringPool::OverlayIndex, Value::make(static_cast<int64_t>(n)));
            n += 1;

            subexpr->eval(ctx, Value::field(ov), LambdaResultFn([&ov, &sum](auto ctx, auto vv) {
                ov->set(StringPool::OverlaySum, vv);
                sum = vv;
                return Result::Continue;
            }));
        } else {
            if (sum.isa(ValueType::Null)) {
                sum = std::move(vv);
            } else {
                sum = BinaryOperatorDispatcher<OperatorAdd>::dispatch(sum, vv);
            }
        }

        return Result::Continue;
    }));

    return res(ctx, sum);
}

KeysFn KeysFn::Fn;
KeysFn::KeysFn() = default;

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
        raise<std::runtime_error>("keys: Expected 1 argument; got "s + std::to_string(args.size()));

    auto result = args[0]->eval(ctx, val, LambdaResultFn([&res](Context ctx, Value vv) {
        if (ctx.phase == Context::Phase::Compilation)
            if (vv.isa(ValueType::Undef))
                return res(ctx, std::move(vv));

        if (vv.node)
            for (auto&& fieldName : vv.node->fieldNames()) {
                if (auto key = ctx.env->stringPool->resolve(fieldName)) {
                    if (res(ctx, Value::strref(*key)) == Result::Stop)
                        return Result::Stop;
                }
            }
        return Result::Continue;
    }));

    return result;
}

}
