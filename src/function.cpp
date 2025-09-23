#include "simfil/function.h"

#include "simfil/model/nodes.h"
#include "simfil/result.h"
#include "simfil/operator.h"
#include "simfil/environment.h"
#include "simfil/model/model.h"
#include "simfil/types.h"
#include "simfil/overlay.h"
#include "fmt/core.h"
#include "tl/expected.hpp"

#include <optional>

namespace simfil
{

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
    std::optional<Error> error;

    ArgParser(std::string functionName, Context ctx, Value val, const std::vector<ExprPtr>& args, size_t idx = 0)
        : functionName(std::move(functionName))
        , args(args)
        , value(std::move(val))
        , ctx(ctx)
        , idx(idx)
    {
        if (args.size() < idx)
            error = Error(Error::InvalidArguments, fmt::format("too few arguments for function {}", this->functionName));
    }

    auto arg(const char* name, ValueType type, Value& outValue) -> ArgParser&
    {
        if (args.size() <= idx) {
            error = Error(Error::InvalidArguments, fmt::format("missing argumnt {} for function {}", name, this->functionName));
            return *this;
        }

        auto subctx = ctx;
        auto res = args[idx]->eval(subctx, value, LambdaResultFn([&, n = 0](Context, Value&& vv) mutable -> tl::expected<Result, Error> {
            if (++n > 1) [[unlikely]] {
                return tl::unexpected<Error>(Error::ExpectedSingleValue,
                                             fmt::format("expeted single argument value for argument {} for function {}", name, functionName));
            }

            if (vv.isa(ValueType::Undef)) {
                anyUndef = true;
                outValue = std::move(vv);
                return Result::Continue;
            }

            if (!vv.isa(type)) [[unlikely]] {
                return tl::unexpected<Error>(Error::TypeMissmatch,
                                             fmt::format("invalid type for argument {} for function {}", name, functionName));
            }

            outValue = std::move(vv);

            return Result::Continue;
        }));

        if (!res) [[unlikely]]
            error = std::move(res.error());

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
        auto res = args[idx]->eval(subctx, value, LambdaResultFn([&, n = 0](Context, Value&& vv) mutable -> tl::expected<Result, Error> {
            if (++n > 1) {
                return tl::unexpected<Error>(Error::ExpectedSingleValue,
                                             fmt::format("{}: argument {} must return a single value", functionName, name));
            }

            if (vv.isa(ValueType::Undef)) {
                anyUndef = true;
                outValue = std::move(vv);
                return Result::Continue;
            }

            if (!vv.isa(type))
                return tl::unexpected<Error>(Error::TypeMissmatch,
                                             fmt::format("{}: invalid value type for argument", functionName, name));

            outValue = std::move(vv);
            return Result::Continue;
        }));
        if (!res) [[unlikely]]
            error = std::move(res.error());

        ++idx;
        return *this;
    }

    [[nodiscard]]
    auto ok() const -> tl::expected<bool, Error>
    {
        if (error) [[unlikely]]
            return tl::unexpected<Error>(*error);
        return !anyUndef;
    }
};

auto boolify(const Value& v) -> bool
{
    /* Needed because DispatchOperator* returns
     * Undef if any argument is Undef. */
    if (v.isa(ValueType::Undef))
        return false;
    return UnaryOperatorDispatcher<OperatorBool>::dispatch(v).value_or(Value::f()).as<ValueType::Bool>();
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

auto CountFn::eval(Context ctx, Value val, const std::vector<ExprPtr>& args, const ResultFn& res) const -> tl::expected<Result, Error>
{
    if (args.empty())
        return tl::unexpected<Error>(Error::InvalidArguments,
                                     fmt::format("function 'count' expects one argument, got {}", args.size()));

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

auto TraceFn::eval(Context ctx, Value val, const std::vector<ExprPtr>& args, const ResultFn& res) const -> tl::expected<Result, Error>
{
    Value name  = Value::undef();
    Value limit = Value::undef();

    auto ok = ArgParser("trace", ctx, val, args, 1)
        /* Skip arg 0 */
        .opt("limit", ValueType::Int, limit, Value::make(static_cast<int64_t>(-1)))
        .opt("name",  ValueType::String, name, Value::make(args[0]->toString()))
        .ok();
    if (!ok)
        return tl::unexpected<Error>(std::move(ok.error()));

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

auto RangeFn::eval(Context ctx, Value val, const std::vector<ExprPtr>& args, const ResultFn& res) const -> tl::expected<Result, Error>
{
    if (args.size() != 2)
        return tl::unexpected<Error>(Error::InvalidArguments,
                                     fmt::format("function 'range' expects 2 arguments, got {}", args.size()));

    Value begin = Value::undef();
    Value end = Value::undef();

    auto ok = ArgParser("range", ctx, val, args)
        .arg("begin", ValueType::Int, begin)
        .arg("end",   ValueType::Int, end)
        .ok();
    if (!ok)
        return tl::unexpected<Error>(std::move(ok.error()));
    if (!ok.value()) [[unlikely]]
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

auto ReFn::eval(Context ctx, Value val, const std::vector<ExprPtr>& args, const ResultFn& res) const -> tl::expected<Result, Error>
{
    if (args.size() != 1)
        return tl::unexpected<Error>(Error::InvalidArguments,
                                     fmt::format("'re' expects 1 argument, got {}", args.size()));

    auto subctx = ctx;
    return args[0]->eval(subctx, val, LambdaResultFn([&](Context, Value&& vv) -> tl::expected<Result, Error> {
        if (vv.isa(ValueType::Undef))
            return res(ctx, Value::undef());

        if (vv.isa(ValueType::String))
            return res(ctx, ReType:: Type.make(vv.as<ValueType::String>()));

        // Passing another <re> object is a no-op
        if (vv.isa(ValueType::TransientObject))
            if (const auto obj = vv.as<ValueType::TransientObject>(); obj.meta == &ReType::Type)
                return res(ctx, std::move(vv));

        return tl::unexpected<Error>(Error::TypeMissmatch,
                                     fmt::format("invalid type for argument 'expr' for function 're'"));
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


auto ArrFn::eval(Context ctx, Value val, const std::vector<ExprPtr>& args, const ResultFn& res) const -> tl::expected<Result, Error>
{
    if (args.empty())
        return res(ctx, Value::null());

    for (const auto& arg : args) {
        if (auto r = arg->eval(ctx, val, LambdaResultFn([&res](Context ctx, Value&& vv) {
            return res(ctx, std::move(vv));
        })); r && *r == Result::Stop)
            return Result::Stop;
        else if (!r)
            return tl::unexpected<Error>(r.error());
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

auto SplitFn::eval(Context ctx, Value val, const std::vector<ExprPtr>& args, const ResultFn& res) const -> tl::expected<Result, Error>
{
    Value str = Value::undef();
    Value sep = Value::undef();
    Value keepEmpty = Value::undef();

    auto ok = ArgParser("split", ctx, val, args)
        .arg("string", ValueType::String, str)
        .arg("separator", ValueType::String, sep)
        .opt("keepEmpty", ValueType::Bool, keepEmpty, Value::t())
        .ok();
    if (!ok)
        return tl::unexpected<Error>(std::move(ok.error()));

    auto subctx = ctx;
    if (!ok.value()) [[unlikely]]
        return res(subctx, Value::undef());

    auto items = split(str.as<ValueType::String>(), sep.as<ValueType::String>(), !keepEmpty.as<ValueType::Bool>());
    for (auto&& item : items) {
        if (auto r = res(subctx, Value::make(std::move(item))); r && *r == Result::Stop)
            break;
        else if (!r)
            return tl::unexpected<Error>(r.error());
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

auto SelectFn::eval(Context ctx, Value val, const std::vector<ExprPtr>& args, const ResultFn& res) const -> tl::expected<Result, Error>
{
    Value idx = Value::undef();
    Value cnt = Value::undef();

    auto ok = ArgParser("select", ctx, val, args, 1)
        /* Skip arg 0 */
        .arg("index", ValueType::Int, idx)
        .opt("limit", ValueType::Int, cnt, Value::make(static_cast<int64_t>(1)))
        .ok();
    if (!ok)
        return tl::unexpected<Error>(std::move(ok.error()));

    if (!ok.value()) [[unlikely]]
        return res(ctx, Value::undef());

    auto iidx = idx.as<ValueType::Int>();
    auto icnt = cnt.as<ValueType::Int>();
    if (icnt <= 0)
        icnt = std::numeric_limits<int>::max();

    auto result = args[0]->eval(ctx, val, LambdaResultFn([&, n = -1](Context ctx, Value&& vv) mutable -> tl::expected<Result, Error> {
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

auto SumFn::eval(Context ctx, Value val, const std::vector<ExprPtr>& args, const ResultFn& res) const -> tl::expected<Result, Error>
{
    if (args.empty() || args.size() > 3)
        return tl::unexpected<Error>(Error::InvalidArguments,
                                     fmt::format("'sum' expects at least 1 argument, got {}", args.size()));

    Value sum = Value::make(static_cast<int64_t>(0));

    Expr* subexpr = args.size() >= 2 ? args[1].get() : nullptr;
    Expr* initval = args.size() == 3 ? args[2].get() : nullptr;
    if (initval)
        (void)initval->eval(ctx, val, LambdaResultFn([&](Context ctx, Value&& vv) {
            sum = std::move(vv);
            return Result::Continue;
        }));

    (void)args[0]->eval(ctx, val, LambdaResultFn([&, n = 0](Context ctx, Value&& vv) mutable -> tl::expected<Result, Error> {
        if (subexpr) {
            auto ov = model_ptr<OverlayNode>::make(vv);
            ov->set(StringPool::OverlaySum, sum);
            ov->set(StringPool::OverlayValue, vv);
            ov->set(StringPool::OverlayIndex, Value::make(static_cast<int64_t>(n)));
            n += 1;

            subexpr->eval(ctx, Value::field(ov), LambdaResultFn([&ov, &sum](auto ctx, Value&& vv) {
                ov->set(StringPool::OverlaySum, vv);
                sum = vv;
                return Result::Continue;
            }));
        } else {
            if (sum.isa(ValueType::Null)) {
                sum = std::move(vv);
            } else {
                if (auto newSum = BinaryOperatorDispatcher<OperatorAdd>::dispatch(sum, vv)) {
                    sum = std::move(newSum.value());
                } else {
                    return tl::unexpected<Error>(std::move(newSum.error()));
                }
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

auto KeysFn::eval(Context ctx, Value val, const std::vector<ExprPtr>& args, const ResultFn& res) const -> tl::expected<Result, Error>
{
    if (args.size() != 1)
        return tl::unexpected<Error>(Error::InvalidArguments,
                                     fmt::format("'keys' expects 1 argument got {}", args.size()));

    auto result = args[0]->eval(ctx, val, LambdaResultFn([&res](Context ctx, const Value& vv) -> tl::expected<Result, Error> {
        if (ctx.phase == Context::Phase::Compilation)
            if (vv.isa(ValueType::Undef))
                return res(ctx, vv);

        if (vv.nodePtr())
            for (auto&& fieldName : vv.node()->fieldNames()) {
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
