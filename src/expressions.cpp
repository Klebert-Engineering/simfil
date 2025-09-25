#include "expressions.h"

#include "simfil/environment.h"
#include "simfil/result.h"
#include "simfil/value.h"
#include "simfil/function.h"

#include "fmt/core.h"

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

WildcardExpr::WildcardExpr() = default;

auto WildcardExpr::type() const -> Type
{
    return Type::PATH;
}

auto WildcardExpr::ieval(Context ctx, const Value& val, const ResultFn& ores) -> tl::expected<Result, Error>
{
    if (ctx.phase == Context::Phase::Compilation)
        return ores(ctx, Value::undef());

    CountedResultFn<const ResultFn&> res(ores, ctx);

    struct Iterate
    {
        Context& ctx;
        ResultFn& res;

        [[nodiscard]] auto iterate(ModelNode const& val) noexcept -> Result
        {
            if (val.type() == ValueType::Null) [[unlikely]]
                return Result::Continue;

            auto result = res(ctx, Value::field(val));
            if (!result || *result == Result::Stop) [[unlikely]]
                return result ? *result : Result::Stop;

            Result finalResult = Result::Continue;
            val.iterate(ModelNode::IterLambda([&, this](const auto& subNode) -> bool {
                if (iterate(subNode) == Result::Stop) {
                    finalResult = Result::Stop;
                    return false;
                }
                return true;
            }));

            return finalResult;
        }
    };

    auto r = val.nodePtr() ? Iterate{ctx, res}.iterate(**val.nodePtr()) : Result::Continue;
    res.ensureCall();
    return r;
}

auto WildcardExpr::clone() const -> ExprPtr
{
    return std::make_unique<WildcardExpr>();
}

void WildcardExpr::accept(ExprVisitor& v)
{
    v.visit(*this);
}

auto WildcardExpr::toString() const -> std::string
{
    return "**";
}

AnyChildExpr::AnyChildExpr() = default;

auto AnyChildExpr::type() const -> Type
{
    return Type::PATH;
}

auto AnyChildExpr::ieval(Context ctx, const Value& val, const ResultFn& res) -> tl::expected<Result, Error>
{
    if (ctx.phase == Context::Phase::Compilation)
        return res(ctx, Value::undef());

    if (!val.node() || !val.node()->size())
        return res(ctx, Value::null());

    val.node()->iterate(ModelNode::IterLambda([&](auto subNode) -> bool {
        auto result = res(ctx, Value::field(std::move(subNode)));
        if (!result || *result == Result::Stop) {
            return false;
        }
        return true;
    }));

   return Result::Continue;
   //return result;
}

auto AnyChildExpr::clone() const -> ExprPtr
{
    return std::make_unique<AnyChildExpr>();
}

void AnyChildExpr::accept(ExprVisitor& v)
{
    v.visit(*this);
}

auto AnyChildExpr::toString() const -> std::string
{
    return "*";
}

FieldExpr::FieldExpr(std::string name)
    : name_(std::move(name))
{
    if (name_ == "_")
        hits_ = 1;
}

FieldExpr::FieldExpr(std::string name, const Token& token)
    : Expr(token)
    , name_(std::move(name))
{
    if (name_ == "_")
        hits_ = 1;
}

auto FieldExpr::type() const -> Type
{
    return Type::PATH;
}

auto FieldExpr::ieval(Context ctx, const Value& val, const ResultFn& res) -> tl::expected<Result, Error>
{
    return ieval(ctx, Value{val}, res);
}

auto FieldExpr::ieval(Context ctx, Value&& val, const ResultFn& res) -> tl::expected<Result, Error>
{
    if (ctx.phase != Context::Compilation)
        evaluations_++;

    if (val.isa(ValueType::Undef))
        return res(ctx, std::move(val));

    /* Special case: _ points to the current node */
    if (name_ == "_")
        return res(ctx, std::move(val));

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
        hits_++;
        return res(ctx, Value::field(*sub));
    }

    if (ctx.phase == Context::Phase::Compilation)
        return res(ctx, Value::undef());
    return res(ctx, Value::null());
}

auto FieldExpr::clone() const -> ExprPtr
{
    return std::make_unique<FieldExpr>(name_);
}

void FieldExpr::accept(ExprVisitor& v)
{
    v.visit(*this);
}

auto FieldExpr::toString() const -> std::string
{
    return name_;
}

MultiConstExpr::MultiConstExpr(const std::vector<Value>& vec)
    : values_(vec)
{}

MultiConstExpr::MultiConstExpr(std::vector<Value>&& vec)
    : values_(std::move(vec))
{}

auto MultiConstExpr::type() const -> Type
{
    return Type::VALUE;
}

auto MultiConstExpr::constant() const -> bool
{
    return true;
}

auto MultiConstExpr::ieval(Context ctx, const Value&, const ResultFn& res) -> tl::expected<Result, Error>
{
    for (const auto& v : values_) {
        if (res(ctx, v) == Result::Stop)
            return Result::Stop;
    }

    return Result::Continue;
}

auto MultiConstExpr::clone() const -> ExprPtr
{
    return std::make_unique<MultiConstExpr>(values_);
}

void MultiConstExpr::accept(ExprVisitor& v)
{
    v.visit(*this);
}

auto MultiConstExpr::toString() const -> std::string
{
    auto list = ""s;
    for (const auto& v : values_) {
        if (!list.empty())
            list += " ";
        list += v.toString();
    }

    return "{"s + list + "}"s;
}

ConstExpr::ConstExpr(Value value)
    : value_(std::move(value))
{}

auto ConstExpr::type() const -> Type
{
    return Type::VALUE;
}

auto ConstExpr::constant() const -> bool
{
    return true;
}

auto ConstExpr::ieval(Context ctx, const Value&, const ResultFn& res) -> tl::expected<Result, Error>
{
    return res(ctx, value_);
}

auto ConstExpr::clone() const -> ExprPtr
{
    return std::make_unique<ConstExpr>(value_);
}

void ConstExpr::accept(ExprVisitor& v)
{
    v.visit(*this);
}

auto ConstExpr::toString() const -> std::string
{
    if (value_.isa(ValueType::String))
        return "\""s + value_.toString() + "\""s;
    return value_.toString();
}

SubscriptExpr::SubscriptExpr(ExprPtr left, ExprPtr index)
    : left_(std::move(left))
    , index_(std::move(index))
{}

auto SubscriptExpr::type() const -> Type
{
    return Type::SUBSCRIPT;
}

auto SubscriptExpr::ieval(Context ctx, const Value& val, const ResultFn& ores) -> tl::expected<Result, Error>
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
                if (!v)
                    return tl::unexpected<Error>(std::move(v.error()));
                return res(ctx, std::move(v.value()));
            }

            return Result::Continue;
        }));
    }));
    res.ensureCall();
    return r;
}

auto SubscriptExpr::clone() const -> ExprPtr
{
    return std::make_unique<SubscriptExpr>(left_->clone(), index_->clone());
}

void SubscriptExpr::accept(ExprVisitor& v)
{
    v.visit(*this);
}

auto SubscriptExpr::toString() const -> std::string
{
    return "(index "s + left_->toString() + " "s + index_->toString() + ")"s;
}

SubExpr::SubExpr(ExprPtr sub)
    : left_(std::make_unique<FieldExpr>("_"))
    , sub_(std::move(sub))
{}

SubExpr::SubExpr(ExprPtr left, ExprPtr sub)
    : left_(std::move(left))
    , sub_(std::move(sub))
{}

auto SubExpr::type() const -> Type
{
    return Type::SUBEXPR;
}

auto SubExpr::ieval(Context ctx, const Value& val, const ResultFn& ores) -> tl::expected<Result, Error>
{
    return ieval(ctx, Value{val}, ores);
}

auto SubExpr::ieval(Context ctx, Value&& val, const ResultFn& ores) -> tl::expected<Result, Error>
{
    /* Do not return null unless we have _no_ matching value. */
    auto res = CountedResultFn<const ResultFn&>(ores, ctx);

    auto r = left_->eval(ctx, val, LambdaResultFn([this, &res](Context ctx, const Value& lv) -> tl::expected<Result, Error> {
        return sub_->eval(ctx, lv, LambdaResultFn([&res, &lv](const Context& ctx, const Value& vv) -> tl::expected<Result, Error> {
            auto bv = UnaryOperatorDispatcher<OperatorBool>::dispatch(vv);
            if (!bv) [[unlikely]]
                return tl::unexpected<Error>(std::move(bv.error()));

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
    return "(sub "s + left_->toString() + " "s + sub_->toString() + ")"s;
}

auto SubExpr::clone() const -> ExprPtr
{
    return std::make_unique<SubExpr>(left_->clone(), sub_->clone());
}

void SubExpr::accept(ExprVisitor& v)
{
    v.visit(*this);
}

AnyExpr::AnyExpr(std::vector<ExprPtr> args)
    : args_(std::move(args))
{}

auto AnyExpr::type() const -> Type
{
    return Type::VALUE;
}

auto AnyExpr::ieval(Context ctx, const Value& val, const ResultFn& res) -> tl::expected<Result, Error>
{
    auto subctx = ctx;
    auto result = false; /* At least one value is true  */
    auto undef = false;  /* At least one value is undef */

    for (const auto& arg : args_) {
        arg->eval(ctx, val, LambdaResultFn([&](Context, const Value& vv) {
            if (ctx.phase == Context::Phase::Compilation) {
                if (vv.isa(ValueType::Undef)) {
                    undef = true;
                    return Result::Stop;
                }
            }

            result = result || boolify(vv);
            return result ? Result::Stop : Result::Continue;
        }));

        if (result || undef)
            break;
    }

    if (undef)
        return res(subctx, Value::undef());

    if (result)
        ++trueResults_;
    else
        ++falseResults_;
    return res(subctx, Value::make(result));
}

auto AnyExpr::clone() const -> ExprPtr
{
    std::vector<ExprPtr> clonedArgs;
    clonedArgs.resize(args_.size());
    std::transform(args_.cbegin(), args_.cend(), std::make_move_iterator(clonedArgs.begin()), [](const auto& exp) {
        return exp->clone();
    });

    return std::make_unique<AnyExpr>(std::move(clonedArgs));
}

auto AnyExpr::accept(ExprVisitor& v) -> void
{
    v.visit(*this);
}

auto AnyExpr::toString() const -> std::string
{
    if (args_.empty())
        return "any()"s;

    auto s = "(any"s;
    for (const auto& arg : args_) {
        s += " "s + arg->toString();
    }
    return s + ")"s;
}

EachExpr::EachExpr(std::vector<ExprPtr> args)
    : args_(std::move(args))
{}

auto EachExpr::type() const -> Type
{
    return Type::VALUE;
}

auto EachExpr::ieval(Context ctx, const Value& val, const ResultFn& res) -> tl::expected<Result, Error>
{
    auto subctx = ctx;
    auto result = true; /* All values are true  */
    auto undef = false; /* At least one value is undef */

    for (const auto& arg : args_) {
        arg->eval(ctx, val, LambdaResultFn([&](Context, const Value& vv) {
            if (ctx.phase == Context::Phase::Compilation) {
                if (vv.isa(ValueType::Undef)) {
                    undef = true;
                    return Result::Stop;
                }
            }
            result = result && boolify(vv);
            return result ? Result::Continue : Result::Stop;
        }));

        if (!result || undef)
            break;
    }

    if (undef)
        return res(subctx, Value::undef());

    if (result)
        ++trueResults_;
    else
        ++falseResults_;
    return res(subctx, Value::make(result));
}

auto EachExpr::clone() const -> ExprPtr
{
    std::vector<ExprPtr> clonedArgs;
    clonedArgs.resize(args_.size());
    std::transform(args_.cbegin(), args_.cend(), std::make_move_iterator(clonedArgs.begin()), [](const auto& exp) {
        return exp->clone();
    });

    return std::make_unique<EachExpr>(std::move(clonedArgs));
}

auto EachExpr::accept(ExprVisitor& v) -> void
{
    v.visit(*this);
}

auto EachExpr::toString() const -> std::string
{
    if (args_.empty())
        return "each()"s;

    auto s = "(each"s;
    for (const auto& arg : args_) {
        s += " "s + arg->toString();
    }
    return s + ")"s;
}

CallExpression::CallExpression(std::string name, std::vector<ExprPtr> args)
    : name_(std::move(name))
    , args_(std::move(args))
{}

auto CallExpression::type() const -> Type
{
    return Type::VALUE;
}

auto CallExpression::ieval(Context ctx, const Value& val, const ResultFn& res) -> tl::expected<Result, Error>
{
    return ieval(ctx, Value{val}, res);
}

auto CallExpression::ieval(Context ctx, Value&& val, const ResultFn& res) -> tl::expected<Result, Error>
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

auto CallExpression::clone() const -> ExprPtr
{
    std::vector<ExprPtr> clonedArgs;
    clonedArgs.resize(args_.size());
    std::transform(args_.cbegin(), args_.cend(), std::make_move_iterator(clonedArgs.begin()), [](const auto& exp) {
        return exp->clone();
    });

    return std::make_unique<CallExpression>(name_, std::move(clonedArgs));
}

void CallExpression::accept(ExprVisitor& v)
{
    v.visit(*this);
}

auto CallExpression::toString() const -> std::string
{
    if (args_.empty())
        return "("s + name_ + ")"s;

    std::string s = "("s + name_;
    for (const auto& arg : args_) {
        s += " "s + arg->toString();
    }
    return s + ")"s;
}

PathExpr::PathExpr(ExprPtr right)
    : left_(std::make_unique<FieldExpr>("_"))
    , right_(std::move(right))
{}

PathExpr::PathExpr(ExprPtr left, ExprPtr right)
    : left_(std::move(left))
    , right_(std::move(right))
{
    assert(left_.get());
    assert(right_.get());
}

auto PathExpr::type() const -> Type
{
    return Type::PATH;
}

auto PathExpr::ieval(Context ctx, const Value& val, const ResultFn& ores) -> tl::expected<Result, Error>
{
    return ieval(ctx, Value{val}, ores);
}

auto PathExpr::ieval(Context ctx, Value&& val, const ResultFn& ores) -> tl::expected<Result, Error>
{
    auto res = CountedResultFn<const ResultFn&>(ores, ctx);

    auto r = left_->eval(ctx, std::move(val), LambdaResultFn([this, &res](Context ctx, Value&& v) -> tl::expected<Result, Error> {
        if (v.isa(ValueType::Undef))
            return Result::Continue;

        if (v.isa(ValueType::Null) && !v.node())
            return Result::Continue;

        ++hits_;

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

auto PathExpr::clone() const -> ExprPtr
{
    return std::make_unique<PathExpr>(left_->clone(), right_->clone());
}

void PathExpr::accept(ExprVisitor& v)
{
    v.visit(*this);
}

auto PathExpr::toString() const -> std::string
{
    return "(. "s + left_->toString() + " "s + right_->toString() + ")"s;
}

UnpackExpr::UnpackExpr(ExprPtr sub)
    : sub_(std::move(sub))
{}

auto UnpackExpr::type() const -> Type
{
    return Type::VALUE;
}

auto UnpackExpr::ieval(Context ctx, const Value& val, const ResultFn& res) -> tl::expected<Result, Error>
{
    auto anyval = false;
    auto r = sub_->eval(ctx, val, LambdaResultFn([&res, &anyval](Context ctx, Value&& v) {
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
            if (res(ctx, std::move(v)) == Result::Stop)
                return Result::Stop;
        }
        return Result::Continue;
    }));

    if (!anyval)
        r = res(ctx, Value::null());
    return r;
}

auto UnpackExpr::clone() const -> ExprPtr
{
    return std::make_unique<UnpackExpr>(sub_->clone());
}

void UnpackExpr::accept(ExprVisitor& v)
{
    v.visit(*this);
}

auto UnpackExpr::toString() const -> std::string
{
    return "(... "s + sub_->toString() + ")"s;
}

UnaryWordOpExpr::UnaryWordOpExpr(std::string ident, ExprPtr left)
    : ident_(std::move(ident))
    , left_(std::move(left))
{}

auto UnaryWordOpExpr::type() const -> Type
{
    return Type::VALUE;
}

auto UnaryWordOpExpr::ieval(Context ctx, const Value& val, const ResultFn& res) -> tl::expected<Result, Error>
{
    return left_->eval(ctx, val, LambdaResultFn([this, &res](const Context& ctx, Value&& val) -> tl::expected<Result, Error> {
        if (val.isa(ValueType::Undef))
            return res(ctx, std::move(val));

        if (val.isa(ValueType::TransientObject)) {
            const auto& obj = val.as<ValueType::TransientObject>();
            auto v = obj.meta->unaryOp(ident_, obj);
            if (!v)
                return tl::unexpected<Error>(std::move(v.error()));
            return res(ctx, std::move(v.value()));
        }

        return tl::unexpected<Error>(Error::InvalidOperator,
                                     fmt::format("Invalid operator '{}' for value of type {}", ident_, valueType2String(val.type)));
    }));
}

void UnaryWordOpExpr::accept(ExprVisitor& v)
{
    v.visit(*this);
}

auto UnaryWordOpExpr::clone() const -> ExprPtr
{
    return std::make_unique<UnaryWordOpExpr>(ident_, left_->clone());
}

auto UnaryWordOpExpr::toString() const -> std::string
{
    return "("s + ident_ + " "s + left_->toString() + ")"s;
}

BinaryWordOpExpr::BinaryWordOpExpr(std::string ident, ExprPtr left, ExprPtr right)
    : ident_(std::move(ident))
    , left_(std::move(left))
    , right_(std::move(right))
{}

auto BinaryWordOpExpr::type() const -> Type
{
    return Type::VALUE;
}

auto BinaryWordOpExpr::ieval(Context ctx, const Value& val, const ResultFn& res) -> tl::expected<Result, Error>
{
    return left_->eval(ctx, val, LambdaResultFn([this, &res, &val](const Context& ctx, const Value& lval) {
        return right_->eval(ctx, val, LambdaResultFn([this, &res, &lval](const Context& ctx, const Value& rval) -> tl::expected<Result, Error> {
            if (lval.isa(ValueType::Undef) || rval.isa(ValueType::Undef))
                return res(ctx, Value::undef());

            if (lval.isa(ValueType::TransientObject)) {
                const auto& obj = lval.as<ValueType::TransientObject>();
                auto v = obj.meta->binaryOp(ident_, obj, rval);
                if (!v)
                    return tl::unexpected<Error>(std::move(v.error()));
                return res(ctx, std::move(v.value()));
            }

            if (rval.isa(ValueType::TransientObject)) {
                const auto& obj = rval.as<ValueType::TransientObject>();
                auto v = obj.meta->binaryOp(ident_, lval, obj);
                if (!v)
                    return tl::unexpected<Error>(std::move(v.error()));
                return res(ctx, std::move(v.value()));
            }

            return tl::unexpected<Error>(Error::InvalidOperator,
                                         fmt::format("Invalid operator '{}' for values of type {} and {}",
                                                     ident_, valueType2String(lval.type), valueType2String(rval.type)));
        }));
    }));
}

void BinaryWordOpExpr::accept(ExprVisitor& v)
{
    v.visit(*this);
}

auto BinaryWordOpExpr::clone() const -> ExprPtr
{
    return std::make_unique<BinaryWordOpExpr>(ident_, left_->clone(), right_->clone());
}

auto BinaryWordOpExpr::toString() const -> std::string
{
    return "("s + ident_ + " "s + left_->toString() + " "s + right_->toString() + ")"s;
}

AndExpr::AndExpr(ExprPtr left, ExprPtr right)
    : left_(std::move(left))
    , right_(std::move(right))
{
    assert(left_.get());
    assert(right_.get());
}

auto AndExpr::type() const -> Type
{
    return Type::VALUE;
}

auto AndExpr::ieval(Context ctx, const Value& val, const ResultFn& res) -> tl::expected<Result, Error>
{
    /* Operator and behaves like in lua:
     * 'a and b' returns a if 'not a?' else b is returned */
    return left_->eval(ctx, val, LambdaResultFn([this, &res, &val](const Context& ctx, Value&& lval) -> tl::expected<Result, Error> {
        if (lval.isa(ValueType::Undef))
            return res(ctx, lval);

        auto v = UnaryOperatorDispatcher<OperatorBool>::dispatch(lval);
        if (!v)
            return tl::unexpected<Error>(std::move(v.error()));

        if (v->isa(ValueType::Bool))
            if (!v->template as<ValueType::Bool>())
                return res(ctx, std::move(lval));

        return right_->eval(ctx, val, LambdaResultFn([&res](const Context& ctx, Value&& rval) {
            return res(ctx, std::move(rval));
        }));
    }));
}

void AndExpr::accept(ExprVisitor& v)
{
    v.visit(*this);
}

auto AndExpr::clone() const -> ExprPtr
{
    return std::make_unique<AndExpr>(left_->clone(), right_->clone());
}

auto AndExpr::toString() const -> std::string
{
    return "(and "s + left_->toString() + " "s + right_->toString() + ")"s;
}

OrExpr::OrExpr(ExprPtr left, ExprPtr right)
    : left_(std::move(left))
    , right_(std::move(right))
{
    assert(left_.get());
    assert(right_.get());
}

auto OrExpr::type() const -> Type
{
    return Type::VALUE;
}

auto OrExpr::ieval(Context ctx, const Value& val, const ResultFn& res) -> tl::expected<Result, Error>
{
    /* Operator or behaves like in lua:
     * 'a or b' returns a if 'a?' else b is returned */
    return left_->eval(ctx, val, LambdaResultFn([this, &res, &val](Context ctx, Value&& lval) -> tl::expected<Result, Error> {
        if (lval.isa(ValueType::Undef))
            return res(ctx, lval);

        ++leftEvaluations_;

        auto v = UnaryOperatorDispatcher<OperatorBool>::dispatch(lval);
        if (!v)
            return tl::unexpected<Error>(std::move(v.error()));

        if (v->isa(ValueType::Bool))
            if (v->template as<ValueType::Bool>())
                return res(ctx, std::move(lval));

        ++rightEvaluations_;
        return right_->eval(ctx, val, LambdaResultFn([&](Context ctx, Value&& rval) {
            return res(ctx, std::move(rval));
        }));
    }));

}

void OrExpr::accept(ExprVisitor& v)
{
    v.visit(*this);
}

auto OrExpr::clone() const -> ExprPtr
{
    return std::make_unique<OrExpr>(left_->clone(), right_->clone());
}

auto OrExpr::toString() const -> std::string
{
    return "(or "s + left_->toString() + " "s + right_->toString() + ")"s;
}

void ExprVisitor::visit(Expr& e)
{
    index_++;
}

void ExprVisitor::visit(WildcardExpr& expr)
{
    visit(static_cast<Expr&>(expr));
}

void ExprVisitor::visit(AnyChildExpr& expr)
{
    visit(static_cast<Expr&>(expr));
}

void ExprVisitor::visit(MultiConstExpr& expr)
{
    visit(static_cast<Expr&>(expr));
}

void ExprVisitor::visit(ConstExpr& expr)
{
    visit(static_cast<Expr&>(expr));
}

void ExprVisitor::visit(SubscriptExpr& expr)
{
    visit(static_cast<Expr&>(expr));

    if (expr.left_)
        expr.left_->accept(*this);
    if (expr.index_)
        expr.index_->accept(*this);
}

void ExprVisitor::visit(SubExpr& expr)
{
    visit(static_cast<Expr&>(expr));

    if (expr.left_)
        expr.left_->accept(*this);
    if (expr.sub_)
        expr.sub_->accept(*this);
}

void ExprVisitor::visit(AnyExpr& expr)
{
    visit(static_cast<Expr&>(expr));

    for (const auto& arg : expr.args_)
        if (arg)
            arg->accept(*this);
}

void ExprVisitor::visit(EachExpr& expr)
{
    visit(static_cast<Expr&>(expr));

    for (const auto& arg : expr.args_)
        if (arg)
            arg->accept(*this);
}

void ExprVisitor::visit(CallExpression& expr)
{
    visit(static_cast<Expr&>(expr));

    for (const auto& arg : expr.args_)
        if (arg)
            arg->accept(*this);
}

void ExprVisitor::visit(PathExpr& expr)
{
    visit(static_cast<Expr&>(expr));

    if (expr.left_)
        expr.left_->accept(*this);
    if (expr.right_)
        expr.right_->accept(*this);
}

void ExprVisitor::visit(FieldExpr& expr)
{
    visit(static_cast<Expr&>(expr));
}

void ExprVisitor::visit(UnpackExpr& expr)
{
    visit(static_cast<Expr&>(expr));

    if (expr.sub_)
        expr.sub_->accept(*this);
}

void ExprVisitor::visit(UnaryWordOpExpr& expr)
{
    visit(static_cast<Expr&>(expr));

    if (expr.left_)
        expr.left_->accept(*this);
}

void ExprVisitor::visit(BinaryWordOpExpr& expr)
{
    visit(static_cast<Expr&>(expr));

    if (expr.left_)
        expr.left_->accept(*this);
    if (expr.right_)
        expr.right_->accept(*this);
}

void ExprVisitor::visit(AndExpr& expr)
{
    visit(static_cast<Expr&>(expr));

    if (expr.left_)
        expr.left_->accept(*this);
    if (expr.right_)
        expr.right_->accept(*this);
}

void ExprVisitor::visit(OrExpr& expr)
{
    visit(static_cast<Expr&>(expr));

    if (expr.left_)
        expr.left_->accept(*this);
    if (expr.right_)
        expr.right_->accept(*this);
}

void ExprVisitor::visit(BinaryExpr<OperatorEq>& e)
{
    visit(static_cast<Expr&>(e));
}

void ExprVisitor::visit(BinaryExpr<OperatorNeq>& e)
{
    visit(static_cast<Expr&>(e));
}

void ExprVisitor::visit(BinaryExpr<OperatorLt>& e)
{
    visit(static_cast<Expr&>(e));
}

void ExprVisitor::visit(BinaryExpr<OperatorLtEq>& e)
{
    visit(static_cast<Expr&>(e));
}

void ExprVisitor::visit(BinaryExpr<OperatorGt>& e)
{
    visit(static_cast<Expr&>(e));
}

void ExprVisitor::visit(BinaryExpr<OperatorGtEq>& e)
{
    visit(static_cast<Expr&>(e));
}

auto ExprVisitor::index() const -> size_t
{
    return index_;
}

}
