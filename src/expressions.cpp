#include "expressions.h"

#include "fmt/format.h"
#include "simfil/environment.h"
#include "simfil/expression.h"
#include "simfil/model/nodes.h"
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

auto WildcardExpr::ieval(Context ctx, Value val) const -> EvalStream
{
    if (ctx.compiling()) {
        co_yield Value::undef();
        co_return;
    }

    struct Iterator
    {
        auto operator()(const ModelNode::Ptr& node) const -> asyncpp::generator<Value> {
            if (node->type() != ValueType::Null)
                co_yield Value::field(node);

            for (const auto& ptr : node->iterate())
                for (auto&& sub : (*this)(ptr))
                    co_yield sub;
        }
    };

    if (val.nodePtr()) {
        co_yield Value::field(*val.nodePtr());

        for (const auto& field : val.node()->iterate()) {
            Iterator iter;
            for (auto&& value : iter(field))
                co_yield value;
        }
    } else {
        co_yield Value::null();
    }
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

auto AnyChildExpr::ieval(Context ctx, Value val) const -> EvalStream
{
    if (ctx.compiling()) {
        co_yield Value::undef();
        co_return;
    }

    if (!val.node() || !val.node()->size()) {
        co_yield Value::null();
        co_return;
    }

    for (const auto& ptr : val.node()->iterate())
        co_yield Value::field(ptr);
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

auto FieldExpr::ieval(Context ctx, Value val) const -> EvalStream
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

    if (val.isa(ValueType::Undef)) {
        co_yield std::move(val);
        co_return;
    }

    /* Special case: _ points to the current node */
    if (name_ == "_") {
        if (diag)
            diag->hits++;
        co_yield std::move(val);
        co_return;
    }

    if (!val.node()) {
        co_yield Value::null();
        co_return;
    }

    if (!nameId_) [[unlikely]] {
        nameId_ = ctx.env->strings()->get(name_);
        if (!nameId_) {
            /* If the field name is not in the string cache, then there
               is no field with that name. */
            co_yield Value::null();
            co_return;
        }
    }

    /* Enter sub-node */
    if (auto sub = val.node()->get(nameId_)) {
        if (diag)
            diag->hits++;
        co_yield Value::field(*sub);
        co_return;
    }

    co_yield ctx.compiling() ? Value::undef() : Value::null();
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

auto MultiConstExpr::ieval(Context ctx, Value) const -> EvalStream
{
    for (auto&& value : values_)
        co_yield value;
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

auto ConstExpr::ieval(Context ctx, Value) const -> EvalStream
{
    co_yield value_;
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

auto SubscriptExpr::ieval(Context ctx, Value val) const -> EvalStream
{
    auto empty = true;
    for (auto&& left : left_->eval(ctx, val)) {
        if (!left) {
            co_yield tl::unexpected(std::move(left.error()));
            co_return;
        }

        for (auto&& index : index_->eval(ctx, val)) {
            if (!index) {
                co_yield tl::unexpected(std::move(index.error()));
                co_return;
            }

            /* Field subscript */
            if (left->node()) {
                ModelNode::Ptr node;

                /* Array subscript */
                if (index->isa(ValueType::Int)) {
                    auto idx = index->as<ValueType::Int>();
                    node = left->node()->at(idx);
                }
                /* String subscript */
                else if (index->isa(ValueType::String)) {
                    auto key = index->as<ValueType::String>();
                    if (auto keyStrId = ctx.env->strings()->get(key))
                        node = left->node()->get(keyStrId);
                }

                if (node) {
                    empty = false;
                    co_yield Value::field(*node);
                } else {
                    ctx.env->warn("Invalid subscript index type "s + valueType2String(index->type), this->toString());
                }
            } else {
                auto v = BinaryOperatorDispatcher<OperatorSubscript>::dispatch(*left, *index);
                if (!v) {
                    co_yield tl::unexpected(std::move(v.error()));
                    co_return;
                }

                empty = false;
                co_yield std::move(v.value());
            }
        }
    }

    if (empty)
        co_yield ctx.compiling() ? Value::undef() : Value::null();
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

auto SubExpr::ieval(Context ctx, Value val) const -> EvalStream
{
    auto empty = true;
    for (auto&& left : left_->eval(ctx, val)) {
        CO_TRY_EXPECTED(left);

        if (left->isa(ValueType::Undef)) {
            co_yield Value::undef();
            co_return;
        }

        for (auto&& sub : sub_->eval(ctx, *left)) {
            CO_TRY_EXPECTED(sub);

            if (sub->isa(ValueType::Undef)) {
                co_yield Value::undef();
                co_return;
            }

            auto bv = UnaryOperatorDispatcher<OperatorBool>::dispatch(*sub);
            CO_TRY_EXPECTED(bv);

            if (bv->isa(ValueType::Bool) && bv->template as<ValueType::Bool>()) {
                empty = false;
                co_yield *left;
            }
        }
    }

    if (empty)
        co_yield ctx.compiling() ? Value::undef() : Value::null();
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

auto AnyExpr::ieval(Context ctx, Value val) const -> EvalStream
{
    auto any = false; /* At least one value is true  */

    for (const auto& arg : args_) {
        for (auto&& result : arg->eval(ctx, val)) {
            if (!result) {
                co_yield tl::unexpected(std::move(result.error()));
                co_return;
            }

            if (result->isa(ValueType::Undef)) {
                co_yield Value::undef();
                co_return;
            }

            any = any || boolify(*result);
            if (any)
                break;
        }

        if (any)
            break;
    }

    co_yield Value::make(any);
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

auto EachExpr::ieval(Context ctx, Value val) const -> EvalStream
{
    auto each = true; /* All values are true  */
    auto undef = false; /* At least one value is undef */

    for (const auto& arg : args_) {
        for (auto&& result : arg->eval(ctx, val)) {
            if (!result) {
                co_yield tl::unexpected(std::move(result.error()));
                co_return;
            }

            if (ctx.compiling()) {
                if (result->isa(ValueType::Undef)) {
                    undef = true;
                    break;
                }
            }

            each = each && boolify(*result);
            if (!each || undef)
                break;
        }

        if (!each || undef)
            break;
    }

    if (undef)
        co_yield Value::undef();
    else
        co_yield Value::make(each);
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

auto CallExpression::ieval(Context ctx, Value val) const -> EvalStream
{
    if (!fn_) [[unlikely]] {
        fn_ = ctx.env->findFunction(name_);
        if (!fn_) {
            co_yield tl::unexpected<Error>(Error::UnknownFunction, fmt::format("Unknown function '{}'", name_));
            co_return;
        }
    }

    auto anyval = false;
    for (auto&& result : fn_->eval(ctx, val, args_)) {
        CO_TRY_EXPECTED(result);

        if (!result) {
            co_yield tl::unexpected(std::move(result.error()));
            co_return;
        }

        anyval = true;
        co_yield *result;
    }

    if (!anyval) {
        co_yield tl::unexpected<Error>(Error::InternalError, "Function did not call result callback");
        co_return;
    }
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

auto PathExpr::ieval(Context ctx, Value val) const -> EvalStream
{
    auto empty = true;
    for (auto&& left : left_->eval(ctx, std::move(val))) {
        if (!left) {
            co_yield tl::unexpected(std::move(left.error()));
            co_return;
        }

        if (left->isa(ValueType::Undef)) {
            co_yield Value::undef();
            co_return;
        }

        if (left->isa(ValueType::Null) && !left->node())
            continue;

        for (auto&& right : right_->eval(ctx, std::move(*left))) {
            if (!right) {
                co_yield tl::unexpected(std::move(right.error()));
                co_return;
            }

            if (right->isa(ValueType::Undef))
                continue;

            if (right->isa(ValueType::Null) && !right->node())
                continue;

            empty = false;
            co_yield right;
        }
    }

    if (empty)
        co_yield ctx.compiling() ? Value::undef() : Value::null();
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

auto UnpackExpr::ieval(Context ctx, Value val) const -> EvalStream
{
    auto empty = true;
    for (auto&& result : sub_->eval(ctx, std::move(val))) {
        CO_TRY_EXPECTED(result);

        if (result->isa(ValueType::TransientObject)) {
            const auto& obj = result->as<ValueType::TransientObject>();

            std::vector<Value> values;
            std::optional<Error> error;

            auto unpackResult = obj.meta->unpack(obj, [&values, &error](Value value) -> bool {
                values.push_back(std::move(value));
                return true;
            });
            CO_TRY_EXPECTED(unpackResult);

            for (auto&& value : values) {
                empty = false;
                co_yield value;
            }
        }
        else {
            empty = false;
            co_yield result;
        }
    }

    if (empty)
        co_yield ctx.compiling() ? Value::undef() : Value::null();
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

auto UnaryWordOpExpr::ieval(Context ctx, Value val) const -> EvalStream
{
    auto empty = true;
    for (auto&& left : left_->eval(ctx, val)) {
        CO_TRY_EXPECTED(left);

        if (left->isa(ValueType::Undef)) {
            empty = false;
            co_yield left;
        }
        else if (left->isa(ValueType::TransientObject)) {
            const auto& obj = left->as<ValueType::TransientObject>();
            auto resolved = obj.meta->unaryOp(ident_, obj);
            CO_TRY_EXPECTED(resolved);

            empty = false;
            co_yield std::move(resolved.value());
        }
        else {
            co_yield tl::unexpected<Error>(Error::InvalidOperator,
                                           fmt::format("Invalid operator '{}' for value of type {}", ident_, valueType2String(left->type)));
            co_return;
        }
    }

    if (empty)
        co_yield ctx.compiling() ? Value::undef() : Value::null();
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

auto BinaryWordOpExpr::ieval(Context ctx, Value val) const -> EvalStream
{
    auto empty = false;
    for (auto&& left : left_->eval(ctx, val)) {
        CO_TRY_EXPECTED(left);

        for (auto&& right : right_->eval(ctx, val)) {
            CO_TRY_EXPECTED(right);

            if (left->isa(ValueType::Undef) || right->isa(ValueType::Undef)) {
                co_yield Value::undef();
            }
            else if (left->isa(ValueType::TransientObject)) {
                const auto& obj = left->as<ValueType::TransientObject>();
                auto v = obj.meta->binaryOp(ident_, obj, *right);
                CO_TRY_EXPECTED(v);

                co_yield std::move(v.value());
            }
            else if (right->isa(ValueType::TransientObject)) {
                const auto& obj = right->as<ValueType::TransientObject>();
                auto v = obj.meta->binaryOp(ident_, *left, obj);
                CO_TRY_EXPECTED(v);

                co_yield std::move(v.value());
            }
            else {
                co_yield tl::unexpected<Error>(Error::InvalidOperator,
                                               fmt::format("Invalid operator '{}' for values of type {} and {}",
                                                           ident_, valueType2String(left->type), valueType2String(right->type)));
            }

            empty = false;
        }
    }

    if (empty)
        co_yield ctx.compiling() ? Value::undef() : Value::null();
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

auto AndExpr::ieval(Context ctx, Value val) const -> EvalStream
{
    /* Operator and behaves like in lua:
     * 'a and b' returns a if 'not a?' else b is returned */
    for (auto&& left : left_->eval(ctx, val)) {
        CO_TRY_EXPECTED(left);

        if (left->isa(ValueType::Undef)) {
            co_yield left;
        }
        else {
            auto boolean = UnaryOperatorDispatcher<OperatorBool>::dispatch(*left);
            CO_TRY_EXPECTED(boolean);

            if (boolean->isa(ValueType::Bool)) {
                if (!boolean->template as<ValueType::Bool>()) {
                    co_yield std::move(*left);
                    co_return; // Short circuit
                }
            }

            for (auto&& right : right_->eval(ctx, val)) {
                CO_TRY_EXPECTED(right);
                co_yield *right;
            }
        }
    }
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

auto OrExpr::ieval(Context ctx, Value val) const -> EvalStream
{
    /* Operator or behaves like in lua:
     * 'a or b' returns a if 'a?' else b is returned */
    for (auto&& left : left_->eval(ctx, val)) {
        CO_TRY_EXPECTED(left);

        if (left->isa(ValueType::Undef)) {
            co_yield left;
        }
        else {
            auto boolean = UnaryOperatorDispatcher<OperatorBool>::dispatch(*left);
            CO_TRY_EXPECTED(boolean);

            if (boolean->isa(ValueType::Bool)) {
                if (boolean->template as<ValueType::Bool>()) {
                    co_yield std::move(*left);
                    co_return; // Short circuit
                }
            }

            for (auto&& right : right_->eval(ctx, val)) {
                CO_TRY_EXPECTED(right);
                co_yield *right;
            }
        }
    }
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
