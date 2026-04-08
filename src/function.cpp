#include "simfil/function.h"

#include "simfil/expression.h"
#include "simfil/model/nodes.h"
#include "simfil/result.h"
#include "simfil/operator.h"
#include "simfil/environment.h"
#include "simfil/model/model.h"
#include "simfil/types.h"
#include "simfil/overlay.h"
#include "fmt/core.h"
#include "src/expressions.h"
#include "tl/expected.hpp"
#include "expected.h"

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
            error.emplace(Error::InvalidArguments, fmt::format("missing argument {} for function {}", name, this->functionName));
            return *this;
        }

        auto subctx = ctx;
        auto n = 0u;
        for (auto&& value : args[idx]->eval(subctx, value)) {
            if (!value) {
                error.emplace(std::move(value.error()));
                outValue = std::move(*value);
                break;
            }

            if (++n > 1) [[unlikely]] {
                error.emplace(Error::ExpectedSingleValue,
                              fmt::format("expected single argument value for argument {} for function {}", name, functionName));
                break;
            }

            if (value->isa(ValueType::Undef)) {
                anyUndef = true;
                outValue = std::move(*value);
            }
            else if (!value->isa(type)) [[unlikely]] {
                error.emplace(Error::TypeMissmatch,
                              fmt::format("invalid type for argument {} for function {}", name, functionName));
                break;
            }

            outValue = std::move(*value);
        }

        ++idx;
        return *this;
    }

    auto opt(std::string_view name, ValueType type, Value& outValue, Value def) -> ArgParser&
    {
        if (args.size() <= idx) {
            outValue = std::move(def);
            return *this;
        }

        auto n = 0u;
        auto subctx = ctx;
        for (auto&& value : args[idx]->eval(subctx, value)) {
            if (++n > 1) {
                error.emplace(Error::ExpectedSingleValue,
                              fmt::format("{}: argument {} must return a single value", functionName, name));
                break;
            }

            if (value->isa(ValueType::Undef)) {
                anyUndef = true;
                outValue = std::move(*value);
            }

            if (!value->isa(type)) {
                error.emplace(Error::TypeMissmatch,
                              fmt::format("{}: invalid value type for argument", functionName, name));
                break;
            }

            outValue = std::move(*value);
        };

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

auto CountFn::eval(Context ctx, Value val, const std::vector<ExprPtr>& args) const -> EvalStream
{
    if (args.empty()) {
        co_yield tl::unexpected<Error>(Error::InvalidArguments,
                                       fmt::format("function 'count' expects one argument, got {}", args.size()));
        co_return;
    }

    int64_t count = 0;
    for (const auto& arg : args) {
        for (const auto& value : arg->eval(ctx, val)) {
            CO_TRY_EXPECTED(value);

            if (ctx.compiling()) {
                if (value->isa(ValueType::Undef)) {
                    co_yield Value::undef();
                    co_return;
                }
            }

            count += boolify(*value) ? 1 : 0;
        }
    }

    co_yield Value::make(count);
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

auto TraceFn::eval(Context ctx, Value val, const std::vector<ExprPtr>& args) const -> EvalStream
{
    /* Never run in compilation phase */
    if (ctx.compiling()) {
        co_yield Value::undef();
        co_return;
    }

    Value name  = Value::undef();
    Value limit = Value::undef();

    auto ok = ArgParser("trace", ctx, val, args, 1)
        /* Skip arg 0 */
        .opt("limit", ValueType::Int, limit, Value::make(static_cast<int64_t>(-1)))
        .opt("name",  ValueType::String, name, Value::make(args[0]->toString()))
        .ok();
    CO_TRY_EXPECTED(ok);

    auto sname = name.as<ValueType::String>();
    auto ilimit = limit.as<ValueType::Int>();
    auto values = std::vector<Value>();

    auto n = 0;
    auto start = std::chrono::steady_clock::now();
    for (auto&& value : args[0]->eval(ctx, val)) {
        CO_TRY_EXPECTED(value);

        n++;
        if (ilimit < 0 || n <= ilimit) {
            // Do not allow string view to leak into the trace result.
            if (auto sv = value->stringViewValue())
                value = Value::make(std::string(*sv));
            values.emplace_back(*value);
        }

        co_yield std::move(*value);
    }
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

    if (n == 0)
        co_yield Value::undef();
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

auto RangeFn::eval(Context ctx, Value val, const std::vector<ExprPtr>& args) const -> EvalStream
{
    if (args.size() != 2) {
        co_yield tl::unexpected<Error>(Error::InvalidArguments,
                                       fmt::format("function 'range' expects 2 arguments, got {}", args.size()));
        co_return;
    }

    Value begin = Value::undef();
    Value end = Value::undef();

    auto ok = ArgParser("range", ctx, val, args)
        .arg("begin", ValueType::Int, begin)
        .arg("end",   ValueType::Int, end)
        .ok();
    CO_TRY_EXPECTED(ok);

    if (!ok.value()) [[unlikely]] {
        co_yield Value::undef();
        co_return;
    }

    auto ibegin = begin.as<ValueType::Int>();
    auto iend = end.as<ValueType::Int>();
    co_yield IRangeType::Type.make(ibegin, iend);
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

auto ReFn::eval(Context ctx, Value val, const std::vector<ExprPtr>& args) const -> EvalStream
{
    if (args.size() != 0) {
        co_yield tl::unexpected<Error>(Error::InvalidArguments,
                                       fmt::format("'re' expects 1 argument, got {}", args.size()));
        co_return;
    }

    for (auto&& value : args[0]->eval(ctx, val)) {
        CO_TRY_EXPECTED(value);

        if (value->isa(ValueType::Undef)) {
            co_yield Value::undef();
            co_return;
        }

        if (value->isa(ValueType::String)) {
            co_yield  ReType::Type.make(value->as<ValueType::String>());
            co_return;
        }

        // Passing another <re> object is a no-op
        if (value->isa(ValueType::TransientObject)) {
            if (const auto obj = value->as<ValueType::TransientObject>(); obj.meta == &ReType::Type) {
                co_yield std::move(value);
                co_return;
            }
        }

        co_yield tl::unexpected<Error>(Error::TypeMissmatch,
                                       fmt::format("invalid type for argument 'expr' for function 're'"));
        co_return;
    }

    assert(0 && "unreachable");
    co_yield Value::null();
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


auto ArrFn::eval(Context ctx, Value val, const std::vector<ExprPtr>& args) const -> EvalStream
{
    if (args.empty()) {
        co_yield Value::null();
        co_return;
    }

    auto empty = true;
    for (const auto& arg : args) {
        for (auto&& value : arg->eval(ctx, val)) {
            CO_TRY_EXPECTED(value);
            empty = false;
            co_yield std::move(*value);
        }
    }

    if (empty)
        co_yield ctx.compiling() ? Value::undef() : Value::null();
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

auto SplitFn::eval(Context ctx, Value val, const std::vector<ExprPtr>& args) const -> EvalStream
{
    Value str = Value::undef();
    Value sep = Value::undef();
    Value keepEmpty = Value::undef();

    auto ok = ArgParser("split", ctx, val, args)
        .arg("string", ValueType::String, str)
        .arg("separator", ValueType::String, sep)
        .opt("keepEmpty", ValueType::Bool, keepEmpty, Value::t())
        .ok();
    CO_TRY_EXPECTED(ok);

    if (!ok.value()) [[unlikely]] {
        co_yield Value::undef();
        co_return;
    }

    auto items = split(str.as<ValueType::String>(), sep.as<ValueType::String>(), !keepEmpty.as<ValueType::Bool>());
    if (!items.empty()) {
        for (auto&& item : items)
            co_yield Value::make(std::move(item));
    }
    else {
        co_yield std::move(str);
    }
}

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

auto SelectFn::eval(Context ctx, Value val, const std::vector<ExprPtr>& args) const -> EvalStream
{
    Value idx = Value::undef();
    Value cnt = Value::undef();

    auto ok = ArgParser("select", ctx, val, args, 1)
        /* Skip arg 0 */
        .arg("index", ValueType::Int, idx)
        .opt("limit", ValueType::Int, cnt, Value::make(static_cast<int64_t>(1)))
        .ok();
    CO_TRY_EXPECTED(ok);

    if (!ok.value()) [[unlikely]] {
        co_yield Value::undef();
        co_return;
    }

    auto iidx = idx.as<ValueType::Int>();
    auto icnt = cnt.as<ValueType::Int>();
    if (icnt <= 0)
        icnt = std::numeric_limits<int>::max();

    auto empty = true;
    auto n = -1;
    for (auto&& value : args[0]->eval(ctx, val)) {
        CO_TRY_EXPECTED(value);

        n++;
        // if (ctx.compiling()) {
        //     if (value->isa(ValueType::Undef)) {
        //         co_yield Value::undef();
        //         co_return;
        //     }
        // }

        if (n >= iidx + icnt)
            co_return;

        if (n >= iidx) {
            empty = false;
            co_yield std::move(value);
        }
    }

    if (empty)
        co_yield ctx.compiling() ? Value::undef() : Value::null();
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

auto SumFn::eval(Context ctx, Value val, const std::vector<ExprPtr>& args) const -> EvalStream
{
    if (args.empty() || args.size() > 3) {
        co_yield tl::unexpected<Error>(Error::InvalidArguments,
                                       fmt::format("'sum' expects at least 1 argument, got {}", args.size()));
        co_return;
    }

    Value sum = Value::make(static_cast<int64_t>(0));

    Expr* subexpr = args.size() >= 2 ? args[1].get() : nullptr;
    Expr* initval = args.size() == 3 ? args[2].get() : nullptr;
    if (initval) {
        for (auto&& value : initval->eval(ctx, val)) {
            CO_TRY_EXPECTED(value);
            sum = std::move(*value);
        }

        if (sum.isa(ValueType::Undef)) {
            co_yield Value::undef();
            co_return;
        }
    }

    auto n = 0;
    for (auto&& value : args[0]->eval(ctx, val)) {
        if (subexpr) {
            auto ov = model_ptr<OverlayNode>::make(*value);
            ov->set(StringPool::OverlaySum, sum);
            ov->set(StringPool::OverlayValue, *value);
            ov->set(StringPool::OverlayIndex, Value::make(static_cast<int64_t>(n)));

            n++;
            for (auto&& subValue : subexpr->eval(ctx, Value::field(ov))) {
                CO_TRY_EXPECTED(subValue);

                ov->set(StringPool::OverlaySum, *subValue);
                sum = *subValue;
            }
        }
        else {
            if (sum.isa(ValueType::Null)) {
                sum = std::move(*value);
            }
            else {
                auto subSum = BinaryOperatorDispatcher<OperatorAdd>::dispatch(sum, *value);
                CO_TRY_EXPECTED(subSum);
                sum = std::move(*subSum);
            }
        }
    }

    co_yield std::move(sum);
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

auto KeysFn::eval(Context ctx, Value val, const std::vector<ExprPtr>& args) const -> EvalStream
{
    if (args.size() != 1) {
        co_yield tl::unexpected<Error>(Error::InvalidArguments,
                                       fmt::format("'keys' expects 1 argument got {}", args.size()));
        co_return;
    }

    for (auto&& value : args[0]->eval(ctx, val)) {
        CO_TRY_EXPECTED(value);

        if (ctx.compiling() && value->isa(ValueType::Undef)) {
            co_yield Value::undef();
            co_return;
        }

        if (value->nodePtr()) {
            for (const auto& name : value->node()->fieldNames()) {
               if (auto key = ctx.env->stringPool->resolve(name)) {
                   co_yield Value::strref(*key);
               }
            }
        }
    }
}

}
