#pragma once

#include "simfil/expression.h"
#include "simfil/model/nodes.h"
#include "simfil/operator.h"
#include "simfil/diagnostics.h"
#include "simfil/expression-visitor.h"

#include <cstdint>
#include <string>

#define CO_TRY_EXPECTED(value)               \
    do { if (!(value)) { co_yield tl::unexpected(std::move((value).error())); co_return; } } while (false)

namespace simfil
{

/**
 * Returns every child recursive.
 */
class WildcardExpr : public Expr
{
public:
    explicit WildcardExpr(ExprId);

    auto type() const -> Type override;
    auto ieval(Context ctx, Value val) const -> EvalStream override;
    void accept(ExprVisitor& v) const override;
    auto toString() const -> std::string override;
};

/**
 * Returns every child of the current node or null.
 */
class AnyChildExpr : public Expr
{
public:
    explicit AnyChildExpr(ExprId);

    auto type() const -> Type override;
    auto ieval(Context ctx, Value val) const -> EvalStream override;
    void accept(ExprVisitor& v) const override;
    auto toString() const -> std::string override;
};

class FieldExpr : public Expr
{
public:
    FieldExpr(ExprId id, std::string name);
    FieldExpr(ExprId id, std::string name, const Token& token);

    auto type() const -> Type override;
    auto ieval(Context ctx, Value val) const -> EvalStream override;
    void accept(ExprVisitor& v) const override;
    auto toString() const -> std::string override;

    std::string name_;
    mutable StringId nameId_ = {};
};

class MultiConstExpr : public Expr
{
public:
    static constexpr size_t Limit = 10000;

    MultiConstExpr() = delete;
    MultiConstExpr(ExprId id, const std::vector<Value>& vec);
    MultiConstExpr(ExprId id, std::vector<Value>&& vec);

    auto type() const -> Type override;
    auto constant() const -> bool override;
    auto ieval(Context ctx, Value val) const -> EvalStream override;
    void accept(ExprVisitor& v) const override;
    auto toString() const -> std::string override;

    const std::vector<Value> values_;
};

class ConstExpr : public Expr
{
public:
    ConstExpr() = delete;
    template <class CType_>
    ConstExpr(ExprId id, CType_&& value)
        : Expr(id)
        , value_(Value::make(std::forward<CType_>(value)))
    {}
    ConstExpr(ExprId id, Value value);

    auto type() const -> Type override;
    auto constant() const -> bool override;
    auto ieval(Context ctx, Value val) const -> EvalStream override;
    void accept(ExprVisitor& v) const override;
    auto toString() const -> std::string override;

    auto value() const -> const Value&;

protected:
    const Value value_;
};

class SubscriptExpr : public Expr
{
public:
    SubscriptExpr(ExprId id, ExprPtr left, ExprPtr index);

    auto type() const -> Type override;
    auto ieval(Context ctx, Value val) const -> EvalStream override;
    void accept(ExprVisitor& v) const override;
    auto toString() const -> std::string override;

    ExprPtr left_;
    ExprPtr index_;
};

class SubExpr : public Expr
{
public:
    SubExpr(ExprId id, ExprPtr left, ExprPtr sub);

    auto type() const -> Type override;
    auto ieval(Context ctx, Value val) const -> EvalStream override;
    void accept(ExprVisitor& v) const override;
    auto toString() const -> std::string override;

    ExprPtr left_, sub_;
};

class AnyExpr : public Expr
{
public:
    AnyExpr(ExprId id, std::vector<ExprPtr> args);

    auto type() const -> Type override;
    auto ieval(Context ctx, Value val) const -> EvalStream override;
    void accept(ExprVisitor& v) const override;
    auto toString() const -> std::string override;

    std::vector<ExprPtr> args_;
};

class EachExpr : public Expr
{
public:
    EachExpr(ExprId id, std::vector<ExprPtr> args);

    auto type() const -> Type override;
    auto ieval(Context ctx, Value val) const -> EvalStream override;
    void accept(ExprVisitor& v) const override;
    auto toString() const -> std::string override;

    std::vector<ExprPtr> args_;
};

class CallExpression : public Expr
{
public:
    CallExpression(ExprId id, std::string name, std::vector<ExprPtr> args);

    auto type() const -> Type override;
    auto ieval(Context ctx, Value val) const -> EvalStream override;
    void accept(ExprVisitor& v) const override;
    auto toString() const -> std::string override;

    std::string name_;
    std::vector<ExprPtr> args_;
    mutable const Function* fn_ = nullptr;
};

class PathExpr : public Expr
{
public:
    PathExpr(ExprId id, ExprPtr left, ExprPtr right);

    auto type() const -> Type override;
    auto ieval(Context ctx, Value val) const -> EvalStream override;
    void accept(ExprVisitor& v) const override;
    auto toString() const -> std::string override;

    ExprPtr left_, right_;
};

/** Calls `unpack` onto values of type Object. Forwards the value(s) otherwise.
 *
 * 1... => 1
 * range(1, 10)... => 1,2,3,4,5,6,7,8,9,10
 */
class UnpackExpr : public Expr
{
public:
    UnpackExpr(ExprId id, ExprPtr sub);

    auto type() const -> Type override;
    auto ieval(Context ctx, Value val) const -> EvalStream override;
    void accept(ExprVisitor& v) const override;
    auto toString() const -> std::string override;

    ExprPtr sub_;
};

/**
 * Generic unary operator expression.
 */
template <class Operator>
class UnaryExpr : public Expr
{
public:
    UnaryExpr(ExprId id, ExprPtr sub)
        : Expr(id)
        , sub_(std::move(sub))
    {}

    auto type() const -> Type override
    {
        return Type::VALUE;
    }

    auto ieval(Context ctx, Value val) const -> EvalStream override
    {
        for (auto value : sub_->eval(ctx, val)) {
            CO_TRY_EXPECTED(value);

            auto resolved = UnaryOperatorDispatcher<Operator>::dispatch(*value);
            CO_TRY_EXPECTED(resolved);

            co_yield resolved;
        }
    }

    void accept(ExprVisitor& v) const override
    {
        v.visit(*this);
        sub_->accept(v);
    }

    auto toString() const -> std::string override
    {
        return "("s + Operator::name() + " "s + sub_->toString() + ")"s;
    }

    ExprPtr sub_;
};

/**
 * Generic binary operator expression.
 */
template <class Operator>
class BinaryExpr : public Expr
{
public:
    BinaryExpr(ExprId id, ExprPtr left, ExprPtr right)
        : Expr(id)
        , left_(std::move(left))
        , right_(std::move(right))
    {}

    BinaryExpr(ExprId id, const Token& token, ExprPtr left, ExprPtr right)
        : Expr(id, token)
        , left_(std::move(left))
        , right_(std::move(right))
    {}

    auto type() const -> Type override
    {
        return Type::VALUE;
    }

    auto ieval(Context ctx, Value val) const -> EvalStream override
    {
        for (auto left : left_->eval(ctx, val)) {
            CO_TRY_EXPECTED(left);

            for (auto right : right_->eval(ctx, val)) {
                CO_TRY_EXPECTED(right);

                auto resolved = BinaryOperatorDispatcher<Operator>::dispatch(*left, *right);
                CO_TRY_EXPECTED(resolved);

                co_yield resolved;
            }
        }
    }

    void accept(ExprVisitor& v) const override
    {
        v.visit(*this);
        left_->accept(v);
        right_->accept(v);
    }

    auto toString() const -> std::string override
    {
        return "("s + Operator::name() + " "s + left_->toString() + " "s + right_->toString() + ")"s;
    }

    ExprPtr left_, right_;
};

class ComparisonExprBase : public Expr
{
public:

    ComparisonExprBase(ExprId id, ExprPtr left, ExprPtr right)
        : Expr(id)
        , left_(std::move(left))
        , right_(std::move(right))
    {}

    ComparisonExprBase(ExprId id, const Token& token, ExprPtr left, ExprPtr right)
        : Expr(id, token)
        , left_(std::move(left))
        , right_(std::move(right))
    {}

    auto type() const -> Type override
    {
        return Type::VALUE;
    }

    ExprPtr left_, right_;
};

template <class Operator, class Child>
class ComparisonExpr : public ComparisonExprBase
{
public:
    using ComparisonExprBase::ComparisonExprBase;

    auto ieval(Context ctx, Value val) const -> EvalStream override
    {
        Diagnostics::ComparisonExprData* diag = nullptr;
        if (ctx.diag)
            diag = &ctx.diag->get<Diagnostics::ComparisonExprData>(*this);
        if (diag) {
            diag->location = sourceLocation();
            diag->evaluations++;
        }

        for (auto left : left_->eval(ctx, val)) {
            CO_TRY_EXPECTED(left);

            if (diag)
                diag->leftTypes.set(left->type);

            for (auto right : right_->eval(ctx, val)) {
                CO_TRY_EXPECTED(right);

                if (diag)
                    diag->rightTypes.set(right->type);

                auto resolved = BinaryOperatorDispatcher<Operator>::dispatch(*left, *right);
                CO_TRY_EXPECTED(resolved);

                if (diag && resolved->isa(ValueType::Bool)) {
                    if (resolved->template as<ValueType::Bool>())
                        diag->trueResults++;
                    else
                        diag->falseResults++;
                }

                co_yield resolved;
            }
        }
    }

    void accept(ExprVisitor& v) const override
    {
        v.visit(static_cast<const Child&>(*this));
        left_->accept(v);
        right_->accept(v);
    }

    auto toString() const -> std::string override
    {
        return "("s + Operator::name() + " "s + left_->toString() + " "s + right_->toString() + ")"s;
    }
};

template <>
class BinaryExpr<OperatorEq> : public ComparisonExpr<OperatorEq, BinaryExpr<OperatorEq>>
{
    using ComparisonExpr::ComparisonExpr;
};

template <>
class BinaryExpr<OperatorNeq> : public ComparisonExpr<OperatorNeq, BinaryExpr<OperatorNeq>>
{
    using ComparisonExpr::ComparisonExpr;
};

template <>
class BinaryExpr<OperatorLt> : public ComparisonExpr<OperatorLt, BinaryExpr<OperatorLt>>
{
    using ComparisonExpr::ComparisonExpr;
};

template <>
class BinaryExpr<OperatorLtEq> : public ComparisonExpr<OperatorLtEq, BinaryExpr<OperatorLtEq>>
{
    using ComparisonExpr::ComparisonExpr;
};

template <>
class BinaryExpr<OperatorGt> : public ComparisonExpr<OperatorGt, BinaryExpr<OperatorGt>>
{
    using ComparisonExpr::ComparisonExpr;
};

template <>
class BinaryExpr<OperatorGtEq> : public ComparisonExpr<OperatorGtEq, BinaryExpr<OperatorGtEq>>
{
    using ComparisonExpr::ComparisonExpr;
};

class UnaryWordOpExpr : public Expr
{
public:
    UnaryWordOpExpr(ExprId id, std::string ident, ExprPtr left);

    auto type() const -> Type override;
    auto ieval(Context ctx, Value val) const -> EvalStream override;
    void accept(ExprVisitor& v) const override;
    auto toString() const -> std::string override;

    std::string ident_;
    ExprPtr left_;
};

class BinaryWordOpExpr : public Expr
{
public:
    BinaryWordOpExpr(ExprId id, std::string ident, ExprPtr left, ExprPtr right);

    auto type() const -> Type override;
    auto ieval(Context ctx, Value val) const -> EvalStream override;
    void accept(ExprVisitor& v) const override;
    auto toString() const -> std::string override;

    std::string ident_;
    ExprPtr left_, right_;
};

class AndExpr : public Expr
{
public:
    AndExpr(ExprId id, ExprPtr left, ExprPtr right);

    auto type() const -> Type override;
    auto ieval(Context ctx, Value val) const -> EvalStream override;
    void accept(ExprVisitor& v) const override;
    auto toString() const -> std::string override;

    ExprPtr left_, right_;
};

class OrExpr : public Expr
{
public:
    OrExpr(ExprId id, ExprPtr left, ExprPtr right);

    auto type() const -> Type override;
    auto ieval(Context ctx, Value val) const -> EvalStream override;
    void accept(ExprVisitor& v) const override;
    auto toString() const -> std::string override;

    ExprPtr left_, right_;
};

}
