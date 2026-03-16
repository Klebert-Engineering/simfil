#include "expressions.h"

#include "fmt/format.h"
#include "simfil/environment.h"
#include "simfil/result.h"
#include "simfil/value.h"
#include "simfil/function.h"
#include "simfil/diagnostics.h"

#include "fmt/core.h"
#include "fmt/ranges.h"
#include "src/expected.h"
#include <memory>
#include <ranges>

namespace simfil
{
namespace
{

/**
 * Helper for calling the result function if it has never been executed
 * at the time of destruction.
 */
template <class InnerFn = const ResultFn&>
struct CountedResultFn : ResultFn
{
    mutable std::size_t calls = 0;
    bool finished = false;
    InnerFn fn;
    Context nonctx;

    CountedResultFn(InnerFn fn, Context ctx)
        : fn(fn)
        , nonctx(ctx)
    {}

    CountedResultFn(const CountedResultFn&) = delete;
    CountedResultFn(CountedResultFn&&) = delete;

    auto operator()(Context ctx, const Value& vv) const noexcept -> tl::expected<Result, Error> override
    {
        assert(!finished);
        ++calls;
        return fn(ctx, vv);
    }

    auto operator()(Context ctx, Value&& vv) const noexcept -> tl::expected<Result, Error> override
    {
        assert(!finished);
        ++calls;
        return fn(ctx, std::move(vv));
    }

    /* NOTE: You _must_ call finish before destruction! */
    auto ensureCall()
    {
        assert(!finished);
        if (calls == 0 && !finished) {
            finished = true;
            if (nonctx.phase == Context::Phase::Compilation)
                fn(nonctx, Value::undef());
            else
                fn(nonctx, Value::null());
        }
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

WildcardExpr::WildcardExpr(ExprId id)
    : Expr(id)
{}

auto WildcardExpr::type() const -> Type
{
    return Type::PATH;
}

auto WildcardExpr::ieval(Context ctx, const Value& val, const ResultFn& ores) const -> tl::expected<Result, Error>
{
    if (ctx.phase == Context::Phase::Compilation)
        return ores(ctx, Value::undef());

    CountedResultFn<const ResultFn&> res(ores, ctx);

    struct Iterate
    {
        Context& ctx;
        ResultFn& res;

        [[nodiscard]] auto iterate(ModelNode const& val) noexcept -> tl::expected<Result, Error>
        {
            if (val.type() == ValueType::Null) [[unlikely]]
                return Result::Continue;

            auto result = res(ctx, Value::field(val));
            TRY_EXPECTED(result);
            if (*result == Result::Stop) [[unlikely]]
                return *result;

            tl::expected<Result, Error> finalResult = Result::Continue;
            val.iterate(ModelNode::IterLambda([&, this](const auto& subNode) {
                auto subResult = iterate(subNode);
                if (!subResult) {
                    finalResult = std::move(subResult);
                    return false;
                }

                if (*subResult == Result::Stop) {
                    finalResult = Result::Stop;
                    return false;
                }

                return true;
            }));

            return finalResult;
        }
    };

    auto r = val.nodePtr() ? Iterate{ctx, res}.iterate(**val.nodePtr()) : tl::expected<Result, Error>(Result::Continue);
    res.ensureCall();
    return r;
}

void WildcardExpr::accept(ExprVisitor& v) const
{
    v.visit(*this);
}

auto WildcardExpr::toString() const -> std::string
{
    return "**"s;
}

AnyChildExpr::AnyChildExpr(ExprId id)
    : Expr(id)
{}

auto AnyChildExpr::type() const -> Type
{
    return Type::PATH;
}

auto AnyChildExpr::ieval(Context ctx, const Value& val, const ResultFn& res) const -> tl::expected<Result, Error>
{
    if (ctx.phase == Context::Phase::Compilation)
        return res(ctx, Value::undef());

    if (!val.node() || !val.node()->size())
        return res(ctx, Value::null());

    std::optional<Error> error;
    val.node()->iterate(ModelNode::IterLambda([&error, &ctx, &res](auto subNode) -> bool {
        auto result = res(ctx, Value::field(std::move(subNode)));
        if (!result) {
            error = std::move(result.error());
            return false;
        }
        if (*result == Result::Stop)
            return false;
        return true;
    }));
    if (error)
        return tl::unexpected<Error>(std::move(*error));
    return Result::Continue;
}

void AnyChildExpr::accept(ExprVisitor& v) const
{
    v.visit(*this);
}

auto AnyChildExpr::toString() const -> std::string
{
    return "*"s;
}

FieldExpr::FieldExpr(ExprId id, std::string name)
    : Expr(id)
    , name_(std::move(name))
{}

FieldExpr::FieldExpr(ExprId id, std::string name, const Token& token)
    : Expr(id, token)
    , name_(std::move(name))
{}

auto FieldExpr::type() const -> Type
{
    return Type::FIELD;
}

auto FieldExpr::ieval(Context ctx, const Value& val, const ResultFn& res) const -> tl::expected<Result, Error>
{
    return ieval(ctx, Value{val}, res);
}

auto FieldExpr::ieval(Context ctx, Value&& val, const ResultFn& res) const -> tl::expected<Result, Error>
{
    Diagnostics::FieldExprData* diag = nullptr;
    if (ctx.diag)
        diag = &ctx.diag->get<Diagnostics::FieldExprData>(*this);

    if (diag) {
        diag->evaluations++;
        diag->location = sourceLocation();
        if (diag->name.empty())
            diag->name = name_;
    }

    if (val.isa(ValueType::Undef))
        return res(ctx, std::move(val));

    /* Special case: _ points to the current node */
    if (name_ == "_") {
        if (diag)
            diag->hits++;
        return res(ctx, std::move(val));
    }

    if (!val.node())
        return res(ctx, Value::null());

    if (!nameId_) [[unlikely]] {
        nameId_ = ctx.env->strings()->get(name_);
        if (!nameId_)
            /* If the field name is not in the string cache, then there
               is no field with that name. */
            return res(ctx, Value::null());
    }

    /* Enter sub-node */
    if (auto sub = val.node()->get(nameId_)) {
        if (diag)
            diag->hits++;
        return res(ctx, Value::field(*sub));
    }

    if (ctx.phase == Context::Phase::Compilation)
        return res(ctx, Value::undef());
    return res(ctx, Value::null());
}

void FieldExpr::accept(ExprVisitor& v) const
{
    v.visit(*this);
}

auto FieldExpr::toString() const -> std::string
{
    return name_;
}

MultiConstExpr::MultiConstExpr(ExprId id, const std::vector<Value>& vec)
    : Expr(id)
    , values_(vec)
{}

MultiConstExpr::MultiConstExpr(ExprId id, std::vector<Value>&& vec)
    : Expr(id)
    , values_(std::move(vec))
{}

auto MultiConstExpr::type() const -> Type
{
    return Type::VALUE;
}

auto MultiConstExpr::constant() const -> bool
{
    return true;
}

auto MultiConstExpr::ieval(Context ctx, const Value&, const ResultFn& res) const -> tl::expected<Result, Error>
{
    for (const auto& v : values_) {
        auto r = res(ctx, v);
        TRY_EXPECTED(r);
        if (*r == Result::Stop)
            return Result::Stop;
    }

    return Result::Continue;
}

void MultiConstExpr::accept(ExprVisitor& v) const
{
    v.visit(*this);
}

auto MultiConstExpr::toString() const -> std::string
{
    auto items = values_ | std::views::transform([](const auto& arg) {
        return arg.toString();
    });

    return fmt::format("{{{}}}", fmt::join(items, " "));
}

ConstExpr::ConstExpr(ExprId id, Value value)
    : Expr(id)
    , value_(std::move(value))
{}

auto ConstExpr::type() const -> Type
{
    return Type::VALUE;
}

auto ConstExpr::constant() const -> bool
{
    return true;
}

auto ConstExpr::ieval(Context ctx, const Value&, const ResultFn& res) const -> tl::expected<Result, Error>
{
    return res(ctx, value_);
}

void ConstExpr::accept(ExprVisitor& v) const
{
    v.visit(*this);
}

auto ConstExpr::toString() const -> std::string
{
    if (value_.isa(ValueType::String))
        return fmt::format("\"{}\"", value_.toString());
    return value_.toString();
}

auto ConstExpr::value() const -> const Value&
{
    return value_;
}

SubscriptExpr::SubscriptExpr(ExprId id, ExprPtr left, ExprPtr index)
    : Expr(id)
    , left_(std::move(left))
    , index_(std::move(index))
{}

auto SubscriptExpr::type() const -> Type
{
    return Type::SUBSCRIPT;
}

auto SubscriptExpr::ieval(Context ctx, const Value& val, const ResultFn& ores) const -> tl::expected<Result, Error>
{
    auto res = CountedResultFn<const ResultFn&>(ores, ctx);
    auto r = left_->eval(ctx, val, LambdaResultFn([this, &val, &res](Context ctx, const Value& lval) {
        return index_->eval(ctx, val, LambdaResultFn([this, &res, &lval](Context ctx, const Value& ival) -> tl::expected<Result, Error> {
            /* Field subscript */
            if (lval.node()) {
                ModelNode::Ptr node;

                /* Array subscript */
                if (ival.isa(ValueType::Int)) {
                    auto index = ival.as<ValueType::Int>();
                    node = lval.node()->at(index);
                }
                /* String subscript */
                else if (ival.isa(ValueType::String)) {
                    auto key = ival.as<ValueType::String>();
                    if (auto keyStrId = ctx.env->strings()->get(key))
                        node = lval.node()->get(keyStrId);
                }

                if (node)
                    return res(ctx, Value::field(*node));
                else
                    ctx.env->warn("Invalid subscript index type "s + valueType2String(ival.type), this->toString());
            } else {
                auto v = BinaryOperatorDispatcher<OperatorSubscript>::dispatch(lval, ival);
                TRY_EXPECTED(v);
                return res(ctx, std::move(v.value()));
            }

            return Result::Continue;
        }));
    }));
    TRY_EXPECTED(r);
    res.ensureCall();
    return r;
}

void SubscriptExpr::accept(ExprVisitor& v) const
{
    v.visit(*this);
}

auto SubscriptExpr::toString() const -> std::string
{
    return fmt::format("(index {} {})", left_->toString(), index_->toString());
}

SubExpr::SubExpr(ExprId id, ExprPtr left, ExprPtr sub)
    : Expr(id)
    , left_(std::move(left))
    , sub_(std::move(sub))
{}

auto SubExpr::type() const -> Type
{
    return Type::SUBEXPR;
}

auto SubExpr::ieval(Context ctx, const Value& val, const ResultFn& ores) const -> tl::expected<Result, Error>
{
    return ieval(ctx, Value{val}, ores);
}

auto SubExpr::ieval(Context ctx, Value&& val, const ResultFn& ores) const -> tl::expected<Result, Error>
{
    /* Do not return null unless we have _no_ matching value. */
    auto res = CountedResultFn<const ResultFn&>(ores, ctx);

    auto r = left_->eval(ctx, val, LambdaResultFn([this, &res](Context ctx, const Value& lv) -> tl::expected<Result, Error> {
        return sub_->eval(ctx, lv, LambdaResultFn([&res, &lv](const Context& ctx, const Value& vv) -> tl::expected<Result, Error> {
            auto bv = UnaryOperatorDispatcher<OperatorBool>::dispatch(vv);
            TRY_EXPECTED(bv);
            if (bv->isa(ValueType::Undef))
                return Result::Continue;

            if (bv->isa(ValueType::Bool) && bv->template as<ValueType::Bool>())
                return res(ctx, lv);

            return Result::Continue;
        }));
    }));
    res.ensureCall();
    return r;
}

auto SubExpr::toString() const -> std::string
{
    return fmt::format("(sub {} {})", left_->toString(), sub_->toString());
}

void SubExpr::accept(ExprVisitor& v) const
{
    v.visit(*this);
}

AnyExpr::AnyExpr(ExprId id, std::vector<ExprPtr> args)
    : Expr(id)
    , args_(std::move(args))
{}

auto AnyExpr::type() const -> Type
{
    return Type::VALUE;
}

auto AnyExpr::ieval(Context ctx, const Value& val, const ResultFn& res) const -> tl::expected<Result, Error>
{
    auto subctx = ctx;
    auto result = false; /* At least one value is true  */
    auto undef = false;  /* At least one value is undef */

    for (const auto& arg : args_) {
        auto res = arg->eval(ctx, val, LambdaResultFn([&](Context, const Value& vv) {
            if (ctx.phase == Context::Phase::Compilation) {
                if (vv.isa(ValueType::Undef)) {
                    undef = true;
                    return Result::Stop;
                }
            }

            result = result || boolify(vv);
            return result ? Result::Stop : Result::Continue;
        }));
        TRY_EXPECTED(res);
        if (result || undef)
            break;
    }

    if (undef)
        return res(subctx, Value::undef());

    return res(subctx, Value::make(result));
}

auto AnyExpr::accept(ExprVisitor& v) const -> void
{
    v.visit(*this);
}

auto AnyExpr::toString() const -> std::string
{
    if (args_.empty())
        return "any()"s;

    auto items = args_ | std::views::transform([](const auto& arg) {
        return arg->toString();
    });

    return fmt::format("(any {})", fmt::join(items, " "));
}

EachExpr::EachExpr(ExprId id, std::vector<ExprPtr> args)
    : Expr(id)
    , args_(std::move(args))
{}

auto EachExpr::type() const -> Type
{
    return Type::VALUE;
}

auto EachExpr::ieval(Context ctx, const Value& val, const ResultFn& res) const -> tl::expected<Result, Error>
{
    auto subctx = ctx;
    auto result = true; /* All values are true  */
    auto undef = false; /* At least one value is undef */

    for (const auto& arg : args_) {
        auto argRes = arg->eval(ctx, val, LambdaResultFn([&](Context, const Value& vv) {
            if (ctx.phase == Context::Phase::Compilation) {
                if (vv.isa(ValueType::Undef)) {
                    undef = true;
                    return Result::Stop;
                }
            }
            result = result && boolify(vv);
            return result ? Result::Continue : Result::Stop;
        }));
        TRY_EXPECTED(argRes);
        if (!result || undef)
            break;
    }

    if (undef)
        return res(subctx, Value::undef());

    return res(subctx, Value::make(result));
}

auto EachExpr::accept(ExprVisitor& v) const -> void
{
    v.visit(*this);
}

auto EachExpr::toString() const -> std::string
{
    if (args_.empty())
        return "each()"s;

    auto items = args_ | std::views::transform([](const auto& arg) {
        return arg->toString();
    });

    return fmt::format("(each {})", fmt::join(items, " "));
}

CallExpression::CallExpression(ExprId id, std::string name, std::vector<ExprPtr> args)
    : Expr(id)
    , name_(std::move(name))
    , args_(std::move(args))
{}

auto CallExpression::type() const -> Type
{
    return Type::VALUE;
}

auto CallExpression::ieval(Context ctx, const Value& val, const ResultFn& res) const -> tl::expected<Result, Error>
{
    return ieval(ctx, Value{val}, res);
}

auto CallExpression::ieval(Context ctx, Value&& val, const ResultFn& res) const -> tl::expected<Result, Error>
{
    if (!fn_) [[unlikely]] {
        fn_ = ctx.env->findFunction(name_);
        if (!fn_)
            return tl::unexpected<Error>(Error::UnknownFunction, fmt::format("Unknown function '{}'", name_));
    }

    auto anyval = false;
    auto result = fn_->eval(ctx, std::move(val), args_, LambdaResultFn([&res, &anyval](const Context& ctx, Value&& vv) {
        anyval = true;
        return res(ctx, std::move(vv));
    }));
    if (!result)
        return result;
    if (!anyval)
        return tl::unexpected<Error>(Error::InternalError, "Function did not call result callback");

    return result;
}

void CallExpression::accept(ExprVisitor& v) const
{
    v.visit(*this);
}

auto CallExpression::toString() const -> std::string
{
    if (args_.empty())
        return fmt::format("({})", name_);

    auto items = args_ | std::views::transform([](const auto& arg) {
        return arg->toString();
    });

    return fmt::format("({} {})", name_, fmt::join(items, " "));
}

PathExpr::PathExpr(ExprId id, ExprPtr left, ExprPtr right)
    : Expr(id)
    , left_(std::move(left))
    , right_(std::move(right))
{
    assert(left_.get());
    assert(right_.get());
}

auto PathExpr::type() const -> Type
{
    return Type::PATH;
}

auto PathExpr::ieval(Context ctx, const Value& val, const ResultFn& ores) const -> tl::expected<Result, Error>
{
    return ieval(ctx, Value{val}, ores);
}

auto PathExpr::ieval(Context ctx, Value&& val, const ResultFn& ores) const -> tl::expected<Result, Error>
{
    auto res = CountedResultFn<const ResultFn&>(ores, ctx);

    auto r = left_->eval(ctx, std::move(val), LambdaResultFn([this, &res](Context ctx, Value&& v) -> tl::expected<Result, Error> {
        if (v.isa(ValueType::Undef))
            return Result::Continue;

        if (v.isa(ValueType::Null) && !v.node())
            return Result::Continue;

        return right_->eval(ctx, std::move(v), LambdaResultFn([this, &res](Context ctx, Value&& vv) -> tl::expected<Result, Error> {
            if (vv.isa(ValueType::Undef))
                return Result::Continue;

            if (vv.isa(ValueType::Null) && !vv.node())
                return Result::Continue;

            return res(ctx, std::move(vv));
        }));
    }));
    res.ensureCall();
    return r;
};

void PathExpr::accept(ExprVisitor& v) const
{
    v.visit(*this);
}

auto PathExpr::toString() const -> std::string
{
    return fmt::format("(. {} {})", left_->toString(), right_->toString());
}

UnpackExpr::UnpackExpr(ExprId id, ExprPtr sub)
    : Expr(id)
    , sub_(std::move(sub))
{}

auto UnpackExpr::type() const -> Type
{
    return Type::VALUE;
}

auto UnpackExpr::ieval(Context ctx, const Value& val, const ResultFn& res) const -> tl::expected<Result, Error>
{
    auto anyval = false;
    auto r = sub_->eval(ctx, val, LambdaResultFn([&res, &anyval](Context ctx, Value&& v) -> tl::expected<Result, Error> {
        if (v.isa(ValueType::TransientObject)) {
            const auto& obj = v.as<ValueType::TransientObject>();
            auto r = Result::Continue;
            obj.meta->unpack(obj, [&](Value vv) {
                anyval = true;
                return res(ctx, std::move(vv)) == Result::Continue;
            });

            if (r == Result::Stop)
                return Result::Stop;
        } else {
            anyval = true;
            auto r = res(ctx, std::move(v));
            TRY_EXPECTED(r);
            if (*r == Result::Stop)
                return Result::Stop;
        }
        return Result::Continue;
    }));
    TRY_EXPECTED(r);

    if (!anyval)
        r = res(ctx, Value::null());
    return r;
}

void UnpackExpr::accept(ExprVisitor& v) const
{
    v.visit(*this);
}

auto UnpackExpr::toString() const -> std::string
{
    return fmt::format("(... {})", sub_->toString());
}

UnaryWordOpExpr::UnaryWordOpExpr(ExprId id, std::string ident, ExprPtr left)
    : Expr(id)
    , ident_(std::move(ident))
    , left_(std::move(left))
{}

auto UnaryWordOpExpr::type() const -> Type
{
    return Type::VALUE;
}

auto UnaryWordOpExpr::ieval(Context ctx, const Value& val, const ResultFn& res) const -> tl::expected<Result, Error>
{
    return left_->eval(ctx, val, LambdaResultFn([this, &res](const Context& ctx, Value&& val) -> tl::expected<Result, Error> {
        if (val.isa(ValueType::Undef))
            return res(ctx, std::move(val));

        if (val.isa(ValueType::TransientObject)) {
            const auto& obj = val.as<ValueType::TransientObject>();
            auto v = obj.meta->unaryOp(ident_, obj);
            TRY_EXPECTED(v);
            return res(ctx, std::move(v.value()));
        }

        return tl::unexpected<Error>(Error::InvalidOperator,
                                     fmt::format("Invalid operator '{}' for value of type {}", ident_, valueType2String(val.type)));
    }));
}

void UnaryWordOpExpr::accept(ExprVisitor& v) const
{
    v.visit(*this);
}

auto UnaryWordOpExpr::toString() const -> std::string
{
    return fmt::format("({} {})", ident_, left_->toString());
}

BinaryWordOpExpr::BinaryWordOpExpr(ExprId id, std::string ident, ExprPtr left, ExprPtr right)
    : Expr(id)
    , ident_(std::move(ident))
    , left_(std::move(left))
    , right_(std::move(right))
{}

auto BinaryWordOpExpr::type() const -> Type
{
    return Type::VALUE;
}

auto BinaryWordOpExpr::ieval(Context ctx, const Value& val, const ResultFn& res) const -> tl::expected<Result, Error>
{
    return left_->eval(ctx, val, LambdaResultFn([this, &res, &val](const Context& ctx, const Value& lval) {
        return right_->eval(ctx, val, LambdaResultFn([this, &res, &lval](const Context& ctx, const Value& rval) -> tl::expected<Result, Error> {
            if (lval.isa(ValueType::Undef) || rval.isa(ValueType::Undef))
                return res(ctx, Value::undef());

            if (lval.isa(ValueType::TransientObject)) {
                const auto& obj = lval.as<ValueType::TransientObject>();
                auto v = obj.meta->binaryOp(ident_, obj, rval);
                TRY_EXPECTED(v);
                return res(ctx, std::move(v.value()));
            }

            if (rval.isa(ValueType::TransientObject)) {
                const auto& obj = rval.as<ValueType::TransientObject>();
                auto v = obj.meta->binaryOp(ident_, lval, obj);
                TRY_EXPECTED(v);
                return res(ctx, std::move(v.value()));
            }

            return tl::unexpected<Error>(Error::InvalidOperator,
                                         fmt::format("Invalid operator '{}' for values of type {} and {}",
                                                     ident_, valueType2String(lval.type), valueType2String(rval.type)));
        }));
    }));
}

void BinaryWordOpExpr::accept(ExprVisitor& v) const
{
    v.visit(*this);
}

auto BinaryWordOpExpr::toString() const -> std::string
{
    return fmt::format("({} {} {})", ident_, left_->toString(), right_->toString());
}

AndExpr::AndExpr(ExprId id, ExprPtr left, ExprPtr right)
    : Expr(id)
    , left_(std::move(left))
    , right_(std::move(right))
{
    assert(left_.get());
    assert(right_.get());
}

auto AndExpr::type() const -> Type
{
    return Type::VALUE;
}

auto AndExpr::ieval(Context ctx, const Value& val, const ResultFn& res) const -> tl::expected<Result, Error>
{
    /* Operator and behaves like in lua:
     * 'a and b' returns a if 'not a?' else b is returned */
    return left_->eval(ctx, val, LambdaResultFn([this, &res, &val](const Context& ctx, Value&& lval) -> tl::expected<Result, Error> {
        if (lval.isa(ValueType::Undef))
            return res(ctx, lval);

        auto v = UnaryOperatorDispatcher<OperatorBool>::dispatch(lval);
        TRY_EXPECTED(v);
        if (v->isa(ValueType::Bool))
            if (!v->template as<ValueType::Bool>())
                return res(ctx, std::move(lval));

        return right_->eval(ctx, val, LambdaResultFn([&res](const Context& ctx, Value&& rval) {
            return res(ctx, std::move(rval));
        }));
    }));
}

void AndExpr::accept(ExprVisitor& v) const
{
    v.visit(*this);
}

auto AndExpr::toString() const -> std::string
{
    return fmt::format("(and {} {})", left_->toString(), right_->toString());
}

OrExpr::OrExpr(ExprId id, ExprPtr left, ExprPtr right)
    : Expr(id)
    , left_(std::move(left))
    , right_(std::move(right))
{
    assert(left_.get());
    assert(right_.get());
}

auto OrExpr::type() const -> Type
{
    return Type::VALUE;
}

auto OrExpr::ieval(Context ctx, const Value& val, const ResultFn& res) const -> tl::expected<Result, Error>
{
    /* Operator or behaves like in lua:
     * 'a or b' returns a if 'a?' else b is returned */
    return left_->eval(ctx, val, LambdaResultFn([this, &res, &val](Context ctx, Value&& lval) -> tl::expected<Result, Error> {
        if (lval.isa(ValueType::Undef))
            return res(ctx, lval);

        auto v = UnaryOperatorDispatcher<OperatorBool>::dispatch(lval);
        TRY_EXPECTED(v);
        if (v->isa(ValueType::Bool))
            if (v->template as<ValueType::Bool>())
                return res(ctx, std::move(lval));

        return right_->eval(ctx, val, LambdaResultFn([&](Context ctx, Value&& rval) {
            return res(ctx, std::move(rval));
        }));
    }));

}

void OrExpr::accept(ExprVisitor& v) const
{
    v.visit(*this);
}

auto OrExpr::toString() const -> std::string
{
    return fmt::format("(or {} {})", left_->toString(), right_->toString());
}

}
