#pragma once

#include "simfil/expression.h"
#include "simfil/model/nodes.h"
#include "simfil/operator.h"

#include <cstdint>
#include <string>

namespace simfil
{

class WildcardExpr;
class AnyChildExpr;
class MultiConstExpr;
class ConstExpr;
class SubscriptExpr;
class SubExpr;
class AnyCallExpr;
class CallExpression;
class UnpackExpr;
class UnaryWordOpExpr;
class BinaryWordOpExpr;
class FieldExpr;
class PathExpr;
class AndExpr;
class OrExpr;
template <class> class UnaryExpr;
template <class> class BinaryExpr;

/**
 * Visitor base for visiting expressions recursively.
 */
class ExprVisitor
{
public:
    virtual ~ExprVisitor() = default;

    virtual void visit(Expr& expr);
    virtual void visit(WildcardExpr& expr);
    virtual void visit(AnyChildExpr& expr);
    virtual void visit(MultiConstExpr& expr);
    virtual void visit(ConstExpr& expr);
    virtual void visit(SubscriptExpr& expr);
    virtual void visit(SubExpr& expr);
    virtual void visit(AnyCallExpr& expr);
    virtual void visit(CallExpression& expr);
    virtual void visit(PathExpr& expr);
    virtual void visit(FieldExpr& expr);
    virtual void visit(UnpackExpr& expr);
    virtual void visit(UnaryWordOpExpr& expr);
    virtual void visit(BinaryWordOpExpr& expr);
    virtual void visit(AndExpr& expr);
    virtual void visit(OrExpr& expr);
    virtual void visit(BinaryExpr<OperatorEq>& expr);
    virtual void visit(BinaryExpr<OperatorNeq>& expr);
    virtual void visit(BinaryExpr<OperatorLt>& expr);
    virtual void visit(BinaryExpr<OperatorLtEq>& expr);
    virtual void visit(BinaryExpr<OperatorGt>& expr);
    virtual void visit(BinaryExpr<OperatorGtEq>& expr);

protected:
    /* Returns the index of the current expression */
    [[nodiscard]]
    size_t index() const;

private:
    size_t index_ = 0;
};

class WildcardExpr : public Expr
{
public:
    WildcardExpr();

    auto type() const -> Type override;
    auto ieval(Context ctx, const Value& val, const ResultFn& ores) -> Result override;
    auto clone() const -> ExprPtr override;
    void accept(ExprVisitor& v) override;
    auto toString() const -> std::string override;
};

/**
 * Returns every child of the current node or null.
 */
class AnyChildExpr : public Expr
{
public:
    AnyChildExpr();

    auto type() const -> Type override;
    auto ieval(Context ctx, const Value& val, const ResultFn& res) -> Result override;
    auto clone() const -> ExprPtr override;
    void accept(ExprVisitor& v) override;
    auto toString() const -> std::string override;
};

class FieldExpr : public Expr
{
public:
    explicit FieldExpr(std::string name);
    FieldExpr(std::string name, const Token& token);

    auto type() const -> Type override;
    auto ieval(Context ctx, const Value& val, const ResultFn& res) -> Result override;
    auto clone() const -> ExprPtr override;
    void accept(ExprVisitor& v) override;
    auto toString() const -> std::string override;

    std::string name_;
    StringId nameId_ = {};

    size_t hits_ = 0;
    size_t evaluations_ = 0;
};

class MultiConstExpr : public Expr
{
public:
    static constexpr size_t Limit = 10000;

    explicit MultiConstExpr(const std::vector<Value>& vec);
    explicit MultiConstExpr(std::vector<Value>&& vec);

    auto type() const -> Type override;
    auto constant() const -> bool override;
    auto ieval(Context ctx, const Value&, const ResultFn& res) -> Result override;
    auto clone() const -> ExprPtr override;
    void accept(ExprVisitor& v) override;
    auto toString() const -> std::string override;

    const std::vector<Value> values_;
};

class ConstExpr : public Expr
{
public:
    template <class CType_>
    explicit ConstExpr(CType_&& value)
        : value_(Value::make(std::forward<CType_>(value)))
    {}

    explicit ConstExpr(Value value);

    auto type() const -> Type override;
    auto constant() const -> bool override;
    auto ieval(Context ctx, const Value&, const ResultFn& res) -> Result override;
    auto clone() const -> ExprPtr override;
    void accept(ExprVisitor& v) override;
    auto toString() const -> std::string override;

protected:
    const Value value_;
};

class SubscriptExpr : public Expr
{
public:
    SubscriptExpr(ExprPtr left, ExprPtr index);

    auto type() const -> Type override;
    auto ieval(Context ctx, const Value& val, const ResultFn& ores) -> Result override;
    auto clone() const -> ExprPtr override;
    void accept(ExprVisitor& v) override;
    auto toString() const -> std::string override;

    ExprPtr left_;
    ExprPtr index_;
};

class SubExpr : public Expr
{
public:
    explicit SubExpr(ExprPtr sub);
    SubExpr(ExprPtr left, ExprPtr sub);

    auto type() const -> Type override;
    auto ieval(Context ctx, const Value& val, const ResultFn& ores) -> Result override;
    auto toString() const -> std::string override;
    auto clone() const -> ExprPtr override;
    void accept(ExprVisitor& v) override;

    ExprPtr left_, sub_;
};

class AnyCallExpr : public Expr
{
public:
    explicit AnyCallExpr(std::vector<ExprPtr> args);

    auto type() const -> Type override;
    auto ieval(Context ctx, const Value& val, const ResultFn& res) -> Result override;
    auto clone() const -> ExprPtr override;
    void accept(ExprVisitor& v) override;
    auto toString() const -> std::string override;

    std::vector<ExprPtr> args_;

    /* Runtime Data */
    std::uint32_t trueResults_ = 0;
};

class CallExpression : public Expr
{
public:
    CallExpression(std::string name, std::vector<ExprPtr> args);

    auto type() const -> Type override;
    auto ieval(Context ctx, const Value& val, const ResultFn& res) -> Result override;
    auto clone() const -> ExprPtr override;
    void accept(ExprVisitor& v) override;
    auto toString() const -> std::string override;

    std::string name_;
    std::vector<ExprPtr> args_;
    const Function* fn_ = nullptr;
};

class PathExpr : public Expr
{
public:
    explicit PathExpr(ExprPtr right);
    PathExpr(ExprPtr left, ExprPtr right);

    auto type() const -> Type override;
    auto ieval(Context ctx, const Value& val, const ResultFn& ores) -> Result override;
    auto clone() const -> ExprPtr override;
    void accept(ExprVisitor& v) override;
    auto toString() const -> std::string override;

    ExprPtr left_, right_;

    /* Evaluation data */
    uint32_t hits_ = 0;
};

/** Calls `unpack` onto values of type Object. Forwards the value(s) otherwise.
 *
 * 1... => 1
 * range(1, 10)... => 1,2,3,4,5,6,7,8,9,10
 */
class UnpackExpr : public Expr
{
public:
    explicit UnpackExpr(ExprPtr sub);

    auto type() const -> Type override;
    auto ieval(Context ctx, const Value& val, const ResultFn& res) -> Result override;
    auto clone() const -> ExprPtr override;
    void accept(ExprVisitor& v) override;
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
    explicit UnaryExpr(ExprPtr sub)
        : sub_(std::move(sub))
    {}

    auto type() const -> Type override
    {
        return Type::VALUE;
    }

    auto ieval(Context ctx, const Value& val, const ResultFn& res) -> Result override
    {
        return sub_->eval(ctx, val, LambdaResultFn([&](Context ctx, Value vv) {
            return res(ctx, UnaryOperatorDispatcher<Operator>::dispatch(std::move(vv)));
        }));
    }

    void accept(ExprVisitor& v) override
    {
        v.visit(*this);
        sub_->accept(v);
    }

    auto clone() const -> ExprPtr override
    {
        return std::make_unique<UnaryExpr>(sub_->clone());
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
    BinaryExpr(ExprPtr left, ExprPtr right)
        : left_(std::move(left))
        , right_(std::move(right))
    {}

    BinaryExpr(const Token& token, ExprPtr left, ExprPtr right)
        : Expr(token)
        , left_(std::move(left))
        , right_(std::move(right))
    {}

    auto type() const -> Type override
    {
        return Type::VALUE;
    }

    auto ieval(Context ctx, const Value& val, const ResultFn& res) -> Result override
    {
        return left_->eval(ctx, val, LambdaResultFn([this, &res, &val](Context ctx, Value lv) {
            leftTypes_.set(lv.type);
            return right_->eval(ctx, val, LambdaResultFn([this, &res, &lv](Context ctx, Value rv) {
                rightTypes_.set(rv.type);

                return res(ctx, BinaryOperatorDispatcher<Operator>::dispatch(std::move(lv),
                                                                             std::move(rv)));
            }));
        }));
    }

    auto clone() const -> ExprPtr override
    {
        return std::make_unique<BinaryExpr>(left_->clone(), right_->clone());
    }

    void accept(ExprVisitor& v) override
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
    TypeFlags leftTypes_, rightTypes_;
};

class ComparisonExprBase : public Expr
{
public:
    ComparisonExprBase(ExprPtr left, ExprPtr right)
        : left_(std::move(left))
        , right_(std::move(right))
    {}

    ComparisonExprBase(const Token& token, ExprPtr left, ExprPtr right)
        : Expr(token)
        , left_(std::move(left))
        , right_(std::move(right))
    {}

    auto type() const -> Type override
    {
        return Type::VALUE;
    }

    auto operandTypes() const -> std::tuple<TypeFlags, TypeFlags>
    {
        return {leftTypes_, rightTypes_};
    }

    auto resultCounts() const -> std::tuple<uint32_t, uint32_t>
    {
        return {falseResults_, trueResults_};
    }

    ExprPtr left_, right_;
    TypeFlags leftTypes_;
    TypeFlags rightTypes_;
    uint32_t falseResults_ = 0;
    uint32_t trueResults_ = 0;
};

template <class Operator, class Child>
class ComparisonExpr : public ComparisonExprBase
{
public:
    using ComparisonExprBase::ComparisonExprBase;

    auto ieval(Context ctx, const Value& val, const ResultFn& res) -> Result override
    {
        return left_->eval(ctx, val, LambdaResultFn([this, &res, &val](Context ctx, Value lv) {
            leftTypes_.set(lv.type);
            return right_->eval(ctx, val, LambdaResultFn([this, &res, &lv](Context ctx, Value rv) {
                rightTypes_.set(rv.type);

                auto operatorResult = BinaryOperatorDispatcher<Operator>::dispatch(std::move(lv), std::move(rv));
                if (operatorResult.isa(ValueType::Bool)) {
                    if (operatorResult.template as<ValueType::Bool>())
                        ++trueResults_;
                    else
                        ++falseResults_;
                }

                return res(ctx, operatorResult);
            }));
        }));
    }

    auto clone() const -> ExprPtr override
    {
        return std::make_unique<Child>(left_->clone(), right_->clone());
    }

    void accept(ExprVisitor& v) override
    {
        v.visit(static_cast<Child&>(*this));
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
    UnaryWordOpExpr(std::string ident, ExprPtr left);

    auto type() const -> Type override;
    auto ieval(Context ctx, const Value& val, const ResultFn& res) -> Result override;
    void accept(ExprVisitor& v) override;
    auto clone() const -> ExprPtr override;
    auto toString() const -> std::string override;

    std::string ident_;
    ExprPtr left_;
};

class BinaryWordOpExpr : public Expr
{
public:
    BinaryWordOpExpr(std::string ident, ExprPtr left, ExprPtr right);

    auto type() const -> Type override;
    auto ieval(Context ctx, const Value& val, const ResultFn& res) -> Result override;
    void accept(ExprVisitor& v) override;
    auto clone() const -> ExprPtr override;
    auto toString() const -> std::string override;

    std::string ident_;
    ExprPtr left_, right_;
};

class AndExpr : public Expr
{
public:
    AndExpr(ExprPtr left, ExprPtr right);

    auto type() const -> Type override;
    auto ieval(Context ctx, const Value& val, const ResultFn& res) -> Result override;
    void accept(ExprVisitor& v) override;
    auto clone() const -> ExprPtr override;
    auto toString() const -> std::string override;

    ExprPtr left_, right_;
};

class OrExpr : public Expr
{
public:
    OrExpr(ExprPtr left, ExprPtr right);

    auto type() const -> Type override;
    auto ieval(Context ctx, const Value& val, const ResultFn& res) -> Result override;
    void accept(ExprVisitor& v) override;
    auto clone() const -> ExprPtr override;
    auto toString() const -> std::string override;

    ExprPtr left_, right_;
};

}
