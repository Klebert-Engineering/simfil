#pragma once

#include "simfil/expression.h"
#include "simfil/model/nodes.h"
#include "simfil/operator.h"
#include "simfil/diagnostics.h"
#include "simfil/expression-visitor.h"

#include <cstdint>
#include <string>

namespace simfil
{

class WildcardExpr : public Expr
{
public:
    WildcardExpr();

    auto type() const -> Type override;
    auto ieval(Context ctx, const Value& val, const ResultFn& ores) const -> tl::expected<Result, Error> override;
    void accept(ExprVisitor& v) const override;
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
    auto ieval(Context ctx, const Value& val, const ResultFn& res) const -> tl::expected<Result, Error> override;
    void accept(ExprVisitor& v) const override;
    auto toString() const -> std::string override;
};

class FieldExpr : public Expr
{
public:
    explicit FieldExpr(std::string name);
    FieldExpr(std::string name, const Token& token);

    auto type() const -> Type override;
    auto ieval(Context ctx, const Value& val, const ResultFn& res) const -> tl::expected<Result, Error> override;
    auto ieval(Context ctx, Value&& val, const ResultFn& res) const -> tl::expected<Result, Error> override;
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
    explicit MultiConstExpr(const std::vector<Value>& vec);
    explicit MultiConstExpr(std::vector<Value>&& vec);

    auto type() const -> Type override;
    auto constant() const -> bool override;
    auto ieval(Context ctx, const Value&, const ResultFn& res) const -> tl::expected<Result, Error> override;
    void accept(ExprVisitor& v) const override;
    auto toString() const -> std::string override;

    const std::vector<Value> values_;
};

class ConstExpr : public Expr
{
public:
    ConstExpr() = delete;
    template <class CType_>
    requires (!std::derived_from<std::remove_cvref_t<CType_>, ConstExpr>)
    explicit ConstExpr(CType_&& value)
        : value_(Value::make(std::forward<CType_>(value)))
    {}
    explicit ConstExpr(Value value);

    auto type() const -> Type override;
    auto constant() const -> bool override;
    auto ieval(Context ctx, const Value&, const ResultFn& res) const -> tl::expected<Result, Error> override;
    void accept(ExprVisitor& v) const override;
    auto toString() const -> std::string override;

    auto value() const -> const Value&;

protected:
    const Value value_;
};

class SubscriptExpr : public Expr
{
public:
    SubscriptExpr(ExprPtr left, ExprPtr index);

    auto type() const -> Type override;
    auto ieval(Context ctx, const Value& val, const ResultFn& ores) const -> tl::expected<Result, Error> override;
    void accept(ExprVisitor& v) const override;
    auto numChildren() const -> std::size_t override;
    auto childAt(std::size_t index) -> ExprPtr& override;
    auto toString() const -> std::string override;

    ExprPtr left_;
    ExprPtr index_;
};

class SubExpr : public Expr
{
public:
    SubExpr(ExprPtr left, ExprPtr sub);

    auto type() const -> Type override;
    auto ieval(Context ctx, const Value& val, const ResultFn& ores) const -> tl::expected<Result, Error> override;
    auto ieval(Context ctx, Value&& val, const ResultFn& ores) const -> tl::expected<Result, Error> override;
    void accept(ExprVisitor& v) const override;
    auto numChildren() const -> std::size_t override;
    auto childAt(std::size_t index) -> ExprPtr& override;
    auto toString() const -> std::string override;

    ExprPtr left_, sub_;
};

class AnyExpr : public Expr
{
public:
    explicit AnyExpr(std::vector<ExprPtr> args);

    auto type() const -> Type override;
    auto ieval(Context ctx, const Value& val, const ResultFn& res) const -> tl::expected<Result, Error> override;
    void accept(ExprVisitor& v) const override;
    auto numChildren() const -> std::size_t override;
    auto childAt(std::size_t index) -> ExprPtr& override;
    auto toString() const -> std::string override;

    std::vector<ExprPtr> args_;
};

class EachExpr : public Expr
{
public:
    explicit EachExpr(std::vector<ExprPtr> args);

    auto type() const -> Type override;
    auto ieval(Context ctx, const Value& val, const ResultFn& res) const -> tl::expected<Result, Error> override;
    void accept(ExprVisitor& v) const override;
    auto numChildren() const -> std::size_t override;
    auto childAt(std::size_t index) -> ExprPtr& override;
    auto toString() const -> std::string override;

    std::vector<ExprPtr> args_;
};

class CallExpression : public Expr
{
public:
    CallExpression(std::string name, std::vector<ExprPtr> args);

    auto type() const -> Type override;
    auto ieval(Context ctx, const Value& val, const ResultFn& res) const -> tl::expected<Result, Error> override;
    auto ieval(Context ctx, Value&& val, const ResultFn& res) const -> tl::expected<Result, Error> override;
    void accept(ExprVisitor& v) const override;
    auto numChildren() const -> std::size_t override;
    auto childAt(std::size_t index) -> ExprPtr& override;
    auto toString() const -> std::string override;

    std::string name_;
    std::vector<ExprPtr> args_;
    mutable const Function* fn_ = nullptr;
};

class PathExpr : public Expr
{
public:
    PathExpr(ExprPtr left, ExprPtr right);

    auto type() const -> Type override;
    auto ieval(Context ctx, const Value& val, const ResultFn& ores) const -> tl::expected<Result, Error> override;
    auto ieval(Context ctx, Value&& val, const ResultFn& ores) const -> tl::expected<Result, Error> override;
    void accept(ExprVisitor& v) const override;
    auto numChildren() const -> std::size_t override;
    auto childAt(std::size_t index) -> ExprPtr& override;
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
    explicit UnpackExpr(ExprPtr sub);

    auto type() const -> Type override;
    auto ieval(Context ctx, const Value& val, const ResultFn& res) const -> tl::expected<Result, Error> override;
    void accept(ExprVisitor& v) const override;
    auto numChildren() const -> std::size_t override;
    auto childAt(std::size_t index) -> ExprPtr& override;
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

    auto ieval(Context ctx, const Value& val, const ResultFn& res) const -> tl::expected<Result, Error> override
    {
        return sub_->eval(ctx, val, LambdaResultFn([&](Context ctx, Value vv) -> tl::expected<Result, Error> {
            auto v = UnaryOperatorDispatcher<Operator>::dispatch(std::move(vv));
            if (!v)
                return tl::unexpected<Error>(std::move(v.error()));
            return res(ctx, std::move(v.value()));
        }));
    }

    auto accept(ExprVisitor& v) const -> void override
    {
        v.visit(*this);
        sub_->accept(v);
    }

    auto numChildren() const -> std::size_t override
    {
        return 1;
    }

    auto childAt(std::size_t index) -> ExprPtr& override
    {
        if (index == 0)
            return sub_;
        return Expr::childAt(index);
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
        : left_(std::move(left))
        , right_(std::move(right))
    {}

    auto type() const -> Type override
    {
        return Type::VALUE;
    }

    auto ieval(Context ctx, const Value& val, const ResultFn& res) const -> tl::expected<Result, Error> override
    {
        return left_->eval(ctx, val, LambdaResultFn([this, &res, &val](Context ctx, Value lv) {
            return right_->eval(ctx, val, LambdaResultFn([this, &res, &lv](Context ctx, Value rv) -> tl::expected<Result, Error> {
                auto v = BinaryOperatorDispatcher<Operator>::dispatch(std::move(lv), std::move(rv));
                if (!v)
                    return tl::unexpected<Error>(std::move(v.error()));
                return res(ctx, std::move(v.value()));
            }));
        }));
    }

    auto accept(ExprVisitor& v) const -> void override
    {
        v.visit(*this);
    }

    auto numChildren() const -> std::size_t override
    {
        return 2;
    }

    auto childAt(std::size_t index) -> ExprPtr& override
    {
        switch (index) {
        case 0:
            return left_;
        case 1:
            return right_;
        default:
            return Expr::childAt(index);
        }
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

    auto numChildren() const -> std::size_t override
    {
        return 2;
    }

    auto childAt(std::size_t index) -> ExprPtr& override
    {
        switch (index) {
        case 0:
            return left_;
        case 1:
            return right_;
        default:
            return Expr::childAt(index);
        }
    }

    ExprPtr left_, right_;
};

template <class Operator, class Child>
class ComparisonExpr : public ComparisonExprBase
{
public:
    using ComparisonExprBase::ComparisonExprBase;

    auto ieval(Context ctx, const Value& val, const ResultFn& res) const -> tl::expected<Result, Error> override
    {
        Diagnostics::ComparisonExprData* diag = nullptr;
        if (ctx.diag)
            diag = &ctx.diag->get<Diagnostics::ComparisonExprData>(*this);
        if (diag) {
            diag->location = sourceLocation();
            diag->evaluations++;
        }

        return left_->eval(ctx, val, LambdaResultFn([this, &res, &val, &diag](Context ctx, Value lv) {
            if (diag)
                diag->leftTypes.set(lv.type);

            return right_->eval(ctx, val, LambdaResultFn([this, &res, &lv, &diag](Context ctx, Value rv) -> tl::expected<Result, Error> {
                if (diag)
                    diag->rightTypes.set(rv.type);

                auto operatorResult = BinaryOperatorDispatcher<Operator>::dispatch(std::move(lv), std::move(rv));
                if (!operatorResult)
                    return tl::unexpected<Error>(std::move(operatorResult.error()));

                if (diag && operatorResult->isa(ValueType::Bool)) {
                    if (operatorResult->template as<ValueType::Bool>())
                        diag->trueResults++;
                    else
                        diag->falseResults++;
                }

                return res(ctx, std::move(operatorResult.value()));
            }));
        }));
    }

    auto accept(ExprVisitor& v) const -> void override
    {
        v.visit(static_cast<const Child&>(*this));
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
    auto ieval(Context ctx, const Value& val, const ResultFn& res) const -> tl::expected<Result, Error> override;
    void accept(ExprVisitor& v) const override;
    auto numChildren() const -> std::size_t override;
    auto childAt(std::size_t index) -> ExprPtr& override;
    auto toString() const -> std::string override;

    std::string ident_;
    ExprPtr left_;
};

class BinaryWordOpExpr : public Expr
{
public:
    BinaryWordOpExpr(std::string ident, ExprPtr left, ExprPtr right);

    auto type() const -> Type override;
    auto ieval(Context ctx, const Value& val, const ResultFn& res) const -> tl::expected<Result, Error> override;
    void accept(ExprVisitor& v) const override;
    auto numChildren() const -> std::size_t override;
    auto childAt(std::size_t index) -> ExprPtr& override;
    auto toString() const -> std::string override;

    std::string ident_;
    ExprPtr left_, right_;
};

class AndExpr : public Expr
{
public:
    AndExpr(ExprPtr left, ExprPtr right);

    auto type() const -> Type override;
    auto ieval(Context ctx, const Value& val, const ResultFn& res) const -> tl::expected<Result, Error> override;
    void accept(ExprVisitor& v) const override;
    auto numChildren() const -> std::size_t override;
    auto childAt(std::size_t index) -> ExprPtr& override;
    auto toString() const -> std::string override;

    ExprPtr left_, right_;
};

class OrExpr : public Expr
{
public:
    OrExpr(ExprPtr left, ExprPtr right);

    auto type() const -> Type override;
    auto ieval(Context ctx, const Value& val, const ResultFn& res) const -> tl::expected<Result, Error> override;
    void accept(ExprVisitor& v) const override;
    auto numChildren() const -> std::size_t override;
    auto childAt(std::size_t index) -> ExprPtr& override;
    auto toString() const -> std::string override;

    ExprPtr left_, right_;
};

/** A specialized expression for queries of the form `**.field`, that
 *  takes object schema information into account.
 */
class WildcardFieldExpr : public Expr
{
public:
    explicit WildcardFieldExpr(std::string name, SourceLocation location = {});

    auto type() const -> Type override;
    auto ieval(Context ctx, const Value& val, const ResultFn& res) const -> tl::expected<Result, Error> override;
    void accept(ExprVisitor& v) const override;
    auto toString() const -> std::string override;

    std::string name_;
    mutable StringId nameId_ = {};
};

}
