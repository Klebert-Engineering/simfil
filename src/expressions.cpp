#include "expressions.h"

#include "simfil/result.h"
#include "simfil/value.h"
#include "simfil/function.h"

#include "fmt/core.h"

namespace
{

/**
 * Helper for calling the result function if it has never been executed
 * at the time of destruction.
 */
template <class InnerFn = const simfil::ResultFn&>
struct CountedResultFn : simfil::ResultFn
{
    mutable std::size_t calls = 0;
    bool finished = false;
    InnerFn fn;
    simfil::Context nonctx;

    CountedResultFn(InnerFn fn, simfil::Context ctx)
        : fn(fn)
        , nonctx(std::move(ctx))
    {}

    CountedResultFn(const CountedResultFn&) = delete;
    CountedResultFn(CountedResultFn&&) = delete;

    auto operator()(simfil::Context ctx, simfil::Value vv) const -> simfil::Result override
    {
        assert(!finished);
        ++calls;
        return fn(std::move(ctx), std::move(vv));
    }

    /* NOTE: You _must_ call finish before destruction! */
    auto ensureCall()
    {
        assert(!finished);
        if (calls == 0 && !finished) {
            finished = true;
            if (nonctx.phase == simfil::Context::Phase::Compilation)
                fn(nonctx, simfil::Value::undef());
            else
                fn(nonctx, simfil::Value::null());
        }
    }
};

}

namespace simfil
{

WildcardExpr::WildcardExpr() = default;

auto WildcardExpr::type() const -> Type
{
    return Type::PATH;
}

auto WildcardExpr::ieval(Context ctx, Value val, const ResultFn& ores) -> Result
{
    if (ctx.phase == Context::Phase::Compilation)
        return ores(ctx, Value::undef());

    CountedResultFn<const ResultFn&> res(ores, ctx);

    struct Iterate
    {
        Context& ctx;
        ResultFn& res;

        auto iterate(ModelNode const& val, int depth)
        {
            if (val.type() == ValueType::Null)
                return Result::Continue;

            if (res(ctx, Value::field(val)) == Result::Stop)
                return Result::Stop;

            auto result = Result::Continue;
            val.iterate(ModelNode::IterLambda([&, this](auto&& subNode) {
                if (iterate(subNode, depth + 1) == Result::Stop) {
                    result = Result::Stop;
                    return false;
                }
                return true;
            }));

            return result;
        };
    };

    auto r = Iterate{ctx, res}.iterate(*val.node, 0);
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

auto AnyChildExpr::ieval(Context ctx, Value val, const ResultFn& res) -> Result
{
    if (ctx.phase == Context::Phase::Compilation)
        return res(ctx, Value::undef());

    if (!val.node || !val.node->size())
        return res(ctx, Value::null());

    auto result = Result::Continue;
    val.node->iterate(ModelNode::IterLambda([&](auto subNode) {
        if (res(ctx, Value::field(std::move(subNode))) == Result::Stop) {
            result = Result::Stop;
            return false;
        }
        return true;
    }));

    return result;
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

auto FieldExpr::ieval(Context ctx, Value val, const ResultFn& res) -> Result
{
    if (val.isa(ValueType::Undef))
        return res(ctx, std::move(val));

    /* Special case: _ points to the current node */
    if (name_ == "_")
        return res(ctx, val);

    if (!val.node)
        return res(ctx, Value::null());

    if (!nameId_)
        nameId_ = ctx.env->strings()->get(name_);

    if (!nameId_)
        /* If the field name is not in the string cache, then there
           is no field with that name. */
        return res(ctx, Value::null());

    /* Enter sub-node */
    if (auto sub = val.node->get(nameId_)) {
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

auto MultiConstExpr::ieval(Context ctx, Value, const ResultFn& res) -> Result
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

auto ConstExpr::ieval(Context ctx, Value, const ResultFn& res) -> Result
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

auto SubscriptExpr::ieval(Context ctx, Value val, const ResultFn& ores) -> Result
{
    auto res = CountedResultFn<const ResultFn&>(ores, ctx);

    auto r = left_->eval(ctx, val, LambdaResultFn([this, &val, &res](Context ctx, Value lval) {
        return index_->eval(ctx, val, LambdaResultFn([this, &res, &lval](Context ctx, const Value& ival) {
            /* Field subscript */
            if (lval.node) {
                ModelNode::Ptr node;

                /* Array subscript */
                if (ival.isa(ValueType::Int)) {
                    auto index = ival.as<ValueType::Int>();
                    node = lval.node->at(index);
                }
                /* String subscript */
                else if (ival.isa(ValueType::String)) {
                    auto key = ival.as<ValueType::String>();
                    if (auto keyStrId = ctx.env->strings()->get(key))
                        node = lval.node->get(keyStrId);
                }

                if (node)
                    return res(ctx, Value::field(*node));
                else
                    ctx.env->warn("Invalid subscript index type "s + valueType2String(ival.type), this->toString());
            } else {
                return res(ctx, BinaryOperatorDispatcher<OperatorSubscript>::dispatch(lval, ival));
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
    if (left_)
        left_->accept(v);
    if (index_)
        index_->accept(v);
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

auto SubExpr::ieval(Context ctx, Value val, const ResultFn& ores) -> Result
{
    /* Do not return null unless we have _no_ matching value. */
    auto res = CountedResultFn<const ResultFn&>(ores, ctx);

    auto r = left_->eval(ctx, val, LambdaResultFn([this, &res](Context ctx, Value lv) {
        return sub_->eval(ctx, lv, LambdaResultFn([&res, &lv](const Context& ctx, const Value& vv) {
            auto bv = UnaryOperatorDispatcher<OperatorBool>::dispatch(vv);
            if (bv.isa(ValueType::Undef))
                return Result::Continue;

            if (bv.isa(ValueType::Bool) && bv.as<ValueType::Bool>())
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
    if (left_)
        left_->accept(v);
    if (sub_)
        sub_->accept(v);
}

CallExpression::CallExpression(std::string name, std::vector<ExprPtr> args)
    : name_(std::move(name))
    , args_(std::move(args))
    , fn_(nullptr)
{}

auto CallExpression::type() const -> Type
{
    return Type::VALUE;
}

auto CallExpression::ieval(Context ctx, Value val, const ResultFn& res) -> Result
{
    if (!fn_)
        fn_ = ctx.env->findFunction(name_);
    if (!fn_)
        raise<std::runtime_error>("Unknown function "s + name_);

    auto anyval = false;
    auto result = fn_->eval(ctx, val, args_, LambdaResultFn([&res, &anyval](const Context& ctx, Value vv) {
        anyval = true;
        return res(ctx, std::move(vv));
    }));

    if (!anyval)
        return res(ctx, Value::null()); /* Expressions _must_ return at least one value! */
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
    for (auto& arg : args_)
        if (arg)
            arg->accept(v);
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

auto PathExpr::ieval(Context ctx, Value val, const ResultFn& ores) -> Result
{
    auto res = CountedResultFn<const ResultFn&>(ores, ctx);

    auto r = left_->eval(ctx, val, LambdaResultFn([this, &res](Context ctx, Value v) {
        if (v.isa(ValueType::Undef))
            return Result::Continue;

        if (v.isa(ValueType::Null) && !v.node)
            return Result::Continue;

        ++hits_;

        return right_->eval(ctx, std::move(v), LambdaResultFn([this, &res](Context ctx, Value vv) {
            if (vv.isa(ValueType::Undef))
                return Result::Continue;

            if (vv.isa(ValueType::Null) && !vv.node)
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
    if (left_)
        left_->accept(v);
    if (right_)
        right_->accept(v);
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

auto UnpackExpr::ieval(Context ctx, Value val, const ResultFn& res) -> Result
{
    auto anyval = false;
    auto r = sub_->eval(ctx, val, LambdaResultFn([&res, &anyval](Context ctx, Value v) {
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
    if (sub_)
        sub_->accept(v);
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

auto UnaryWordOpExpr::ieval(Context ctx, Value val, const ResultFn& res) -> Result
{
    return left_->eval(ctx, val, LambdaResultFn([this, &res](const Context& ctx, Value val) {
        if (val.isa(ValueType::Undef))
            return res(ctx, std::move(val));

        if (val.isa(ValueType::TransientObject)) {
            const auto& obj = val.as<ValueType::TransientObject>();
            return res(ctx, obj.meta->unaryOp(ident_, obj));
        }

        raise<std::runtime_error>(fmt::format("Invalid operator '{}' for value of type {}",
                                                ident_, valueType2String(val.type)));
    }));
}

void UnaryWordOpExpr::accept(ExprVisitor& v)
{
    v.visit(*this);
    if (left_)
        left_->accept(v);
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

auto BinaryWordOpExpr::ieval(Context ctx, Value val, const ResultFn& res) -> Result
{
    return left_->eval(ctx, val, LambdaResultFn([this, &res, &val](const Context& ctx, Value lval) {
        return right_->eval(ctx, val, LambdaResultFn([this, &res, &lval](const Context& ctx, const Value& rval) {
            if (lval.isa(ValueType::Undef) || rval.isa(ValueType::Undef))
                return res(ctx, Value::undef());

            if (lval.isa(ValueType::TransientObject)) {
                const auto& obj = lval.as<ValueType::TransientObject>();
                return res(ctx, obj.meta->binaryOp(ident_, obj, rval));
            }

            if (rval.isa(ValueType::TransientObject)) {
                const auto& obj = rval.as<ValueType::TransientObject>();
                return res(ctx, obj.meta->binaryOp(ident_, lval, obj));
            }

            raise<std::runtime_error>(fmt::format("Invalid operator '{}' for values of type {} and {}",
                                                    ident_, valueType2String(lval.type), valueType2String(rval.type)));
        }));
    }));
}

void BinaryWordOpExpr::accept(ExprVisitor& v)
{
    v.visit(*this);
    if (left_)
        left_->accept(v);
    if (right_)
        right_->accept(v);
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

auto AndExpr::ieval(Context ctx, Value val, const ResultFn& res) -> Result
{
    /* Operator and behaves like in lua:
        * 'a and b' returns a if 'not a?' else b is returned */
    return left_->eval(ctx, val, LambdaResultFn([this, &res, &val](const Context& ctx, Value lval) {
        if (lval.isa(ValueType::Undef))
            return res(ctx, lval);

        if (auto v = UnaryOperatorDispatcher<OperatorBool>::dispatch(lval); v.isa(ValueType::Bool))
            if (!v.as<ValueType::Bool>())
                return res(ctx, std::move(lval));

        return right_->eval(ctx, val, LambdaResultFn([&res](const Context& ctx, Value rval) {
            return res(ctx, std::move(rval));
        }));
    }));
}

void AndExpr::accept(ExprVisitor& v)
{
    v.visit(*this);
    if (left_)
        left_->accept(v);
    if (right_)
        right_->accept(v);
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

auto OrExpr::ieval(Context ctx, Value val, const ResultFn& res) -> Result
{
    /* Operator or behaves like in lua:
        * 'a or b' returns a if 'a?' else b is returned */
    return left_->eval(ctx, val, LambdaResultFn([this, &res, &val](Context ctx, Value lval) {
        if (lval.isa(ValueType::Undef))
            return res(ctx, lval);

        if (auto v = UnaryOperatorDispatcher<OperatorBool>::dispatch(lval); v.isa(ValueType::Bool))
            if (v.as<ValueType::Bool>())
                return res(ctx, std::move(lval));

        return right_->eval(ctx, val, LambdaResultFn([&](Context ctx, Value rval) {
            return res(ctx, std::move(rval));
        }));
    }));

}

void OrExpr::accept(ExprVisitor& v)
{
    v.visit(*this);
    if (left_)
        left_->accept(v);
    if (right_)
        right_->accept(v);
}

auto OrExpr::clone() const -> ExprPtr
{
    return std::make_unique<AndExpr>(left_->clone(), right_->clone());
}

auto OrExpr::toString() const -> std::string
{
    return "(or "s + left_->toString() + " "s + right_->toString() + ")"s;
}

void ExprVisitor::visit(Expr& e)
{
    index_++;
}

void ExprVisitor::visit(FieldExpr& e)
{
    visit(static_cast<Expr&>(e));
}

void ExprVisitor::visit(PathExpr& e)
{
    visit(static_cast<Expr&>(e));
}

auto ExprVisitor::index() const -> size_t
{
    return index_;
}

}
