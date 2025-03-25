#pragma once

#include "simfil/expression.h"
#include "simfil/operator.h"

#include <string>

namespace simfil
{

class FieldExpr;
class PathExpr;

/**
 * Visitor base for visiting expressions recursively.
 */
class ExprVisitor
{
public:
    virtual void visit(Expr& exp);
    virtual void visit(FieldExpr& exp);
    virtual void visit(PathExpr& exp);

protected:
    /* Returns the index of the current expression */
    size_t index() const;

private:
    size_t index_ = 0;
};

class WildcardExpr : public Expr
{
public:
    WildcardExpr();

    auto type() const -> Type override;
    auto ieval(Context ctx, Value val, const ResultFn& ores) -> Result override;
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
    auto ieval(Context ctx, Value val, const ResultFn& res) -> Result override;
    auto clone() const -> ExprPtr override;
    void accept(ExprVisitor& v) override;
    auto toString() const -> std::string override;
};

class FieldExpr : public Expr
{
public:
    FieldExpr(std::string name);
    FieldExpr(std::string name, const Token& token);

    auto type() const -> Type override;
    auto ieval(Context ctx, Value val, const ResultFn& res) -> Result override;
    auto clone() const -> ExprPtr override;
    void accept(ExprVisitor& v) override;
    auto toString() const -> std::string override;

    std::string name_;
    StringId nameId_ = {};
    size_t hits_ = 0;
};

class MultiConstExpr : public Expr
{
public:
    static constexpr size_t Limit = 10000;

    explicit MultiConstExpr(const std::vector<Value>& vec);
    explicit MultiConstExpr(std::vector<Value>&& vec);

    auto type() const -> Type override;
    auto constant() const -> bool override;
    auto ieval(Context ctx, Value, const ResultFn& res) -> Result override;
    auto clone() const -> ExprPtr override;
    void accept(ExprVisitor& v) override;
    auto toString() const -> std::string override;

    const std::vector<Value> values_;
};

class ConstExpr : public Expr
{
public:
    template <class _CType>
    explicit ConstExpr(_CType&& value)
        : value_(Value::make(std::forward<_CType>(value)))
    {}

    explicit ConstExpr(Value value);

    auto type() const -> Type override;
    auto constant() const -> bool override;
    auto ieval(Context ctx, Value, const ResultFn& res) -> Result override;
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
    auto ieval(Context ctx, Value val, const ResultFn& ores) -> Result override;
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
    auto ieval(Context ctx, Value val, const ResultFn& ores) -> Result override;
    auto toString() const -> std::string override;
    auto clone() const -> ExprPtr override;
    void accept(ExprVisitor& v) override;

    ExprPtr left_, sub_;
};

class CallExpression : public Expr
{
public:
    CallExpression(std::string name, std::vector<ExprPtr> args);

    auto type() const -> Type override;
    auto ieval(Context ctx, Value val, const ResultFn& res) -> Result override;
    auto clone() const -> ExprPtr override;
    void accept(ExprVisitor& v) override;
    auto toString() const -> std::string override;

    std::string name_;
    std::vector<ExprPtr> args_;
    const Function* fn_;
};

class PathExpr : public Expr
{
public:
    PathExpr(ExprPtr right);
    PathExpr(ExprPtr left, ExprPtr right);

    auto type() const -> Type override;
    auto ieval(Context ctx, Value val, const ResultFn& ores) -> Result override;
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
    auto ieval(Context ctx, Value val, const ResultFn& res) -> Result override;
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
    UnaryExpr(ExprPtr sub)
        : sub_(std::move(sub))
    {}

    auto type() const -> Type override
    {
        return Type::VALUE;
    }

    auto ieval(Context ctx, Value val, const ResultFn& res) -> Result override
    {
        return sub_->eval(ctx, val, LambdaResultFn([&](Context ctx, Value vv) {
            return res(ctx, UnaryOperatorDispatcher<Operator>::dispatch(std::move(vv)));
        }));
    }

    void accept(ExprVisitor& v) override
    {
        v.visit(*this);
        if (sub_)
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
    {
        assert(left_.get());
        assert(right_.get());
    }

    auto type() const -> Type override
    {
        return Type::VALUE;
    }

    auto ieval(Context ctx, Value val, const ResultFn& res) -> Result override
    {
        return left_->eval(ctx, val, LambdaResultFn([this, &res, &val](Context ctx, Value lv) {
            return right_->eval(ctx, val, LambdaResultFn([&res, &lv](Context ctx, Value rv) {
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
        if (left_)
            left_->accept(v);
        if (right_)
            right_->accept(v);
    }

    auto toString() const -> std::string override
    {
        return "("s + Operator::name() + " "s + left_->toString() + " "s + right_->toString() + ")"s;
    }

    ExprPtr left_, right_;
};

class UnaryWordOpExpr : public Expr
{
public:
    UnaryWordOpExpr(std::string ident, ExprPtr left);

    auto type() const -> Type override;
    auto ieval(Context ctx, Value val, const ResultFn& res) -> Result override;
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
    auto ieval(Context ctx, Value val, const ResultFn& res) -> Result override;
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
    auto ieval(Context ctx, Value val, const ResultFn& res) -> Result override;
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
    auto ieval(Context ctx, Value val, const ResultFn& res) -> Result override;
    void accept(ExprVisitor& v) override;
    auto clone() const -> ExprPtr override;
    auto toString() const -> std::string override;

    ExprPtr left_, right_;
};

}
