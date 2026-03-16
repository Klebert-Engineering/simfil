// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#include "simfil/expression-visitor.h"

#include "expressions.h"

namespace simfil
{

ExprVisitor::ExprVisitor() = default;
ExprVisitor::~ExprVisitor() = default;

void ExprVisitor::visit(const Expr& e)
{
    index_++;
}

void ExprVisitor::visit(const WildcardExpr& expr)
{
    visit(static_cast<const Expr&>(expr));
}

void ExprVisitor::visit(const AnyChildExpr& expr)
{
    visit(static_cast<const Expr&>(expr));
}

void ExprVisitor::visit(const MultiConstExpr& expr)
{
    visit(static_cast<const Expr&>(expr));
}

void ExprVisitor::visit(const ConstExpr& expr)
{
    visit(static_cast<const Expr&>(expr));
}

void ExprVisitor::visit(const SubscriptExpr& expr)
{
    visit(static_cast<const Expr&>(expr));

    if (expr.left_)
        expr.left_->accept(*this);
    if (expr.index_)
        expr.index_->accept(*this);
}

void ExprVisitor::visit(const SubExpr& expr)
{
    visit(static_cast<const Expr&>(expr));

    if (expr.left_)
        expr.left_->accept(*this);
    if (expr.sub_)
        expr.sub_->accept(*this);
}

void ExprVisitor::visit(const AnyExpr& expr)
{
    visit(static_cast<const Expr&>(expr));

    for (const auto& arg : expr.args_)
        if (arg)
            arg->accept(*this);
}

void ExprVisitor::visit(const EachExpr& expr)
{
    visit(static_cast<const Expr&>(expr));

    for (const auto& arg : expr.args_)
        if (arg)
            arg->accept(*this);
}

void ExprVisitor::visit(const CallExpression& expr)
{
    visit(static_cast<const Expr&>(expr));

    for (const auto& arg : expr.args_)
        if (arg)
            arg->accept(*this);
}

void ExprVisitor::visit(const PathExpr& expr)
{
    visit(static_cast<const Expr&>(expr));

    if (expr.left_)
        expr.left_->accept(*this);
    if (expr.right_)
        expr.right_->accept(*this);
}

void ExprVisitor::visit(const FieldExpr& expr)
{
    visit(static_cast<const Expr&>(expr));
}

void ExprVisitor::visit(const UnpackExpr& expr)
{
    visit(static_cast<const Expr&>(expr));

    if (expr.sub_)
        expr.sub_->accept(*this);
}

void ExprVisitor::visit(const UnaryWordOpExpr& expr)
{
    visit(static_cast<const Expr&>(expr));

    if (expr.left_)
        expr.left_->accept(*this);
}

void ExprVisitor::visit(const BinaryWordOpExpr& expr)
{
    visit(static_cast<const Expr&>(expr));

    if (expr.left_)
        expr.left_->accept(*this);
    if (expr.right_)
        expr.right_->accept(*this);
}

void ExprVisitor::visit(const AndExpr& expr)
{
    visit(static_cast<const Expr&>(expr));

    if (expr.left_)
        expr.left_->accept(*this);
    if (expr.right_)
        expr.right_->accept(*this);
}

void ExprVisitor::visit(const OrExpr& expr)
{
    visit(static_cast<const Expr&>(expr));

    if (expr.left_)
        expr.left_->accept(*this);
    if (expr.right_)
        expr.right_->accept(*this);
}

void ExprVisitor::visit(const BinaryExpr<OperatorEq>& e)
{
    visit(static_cast<const Expr&>(e));
}

void ExprVisitor::visit(const BinaryExpr<OperatorNeq>& e)
{
    visit(static_cast<const Expr&>(e));
}

void ExprVisitor::visit(const BinaryExpr<OperatorLt>& e)
{
    visit(static_cast<const Expr&>(e));
}

void ExprVisitor::visit(const BinaryExpr<OperatorLtEq>& e)
{
    visit(static_cast<const Expr&>(e));
}

void ExprVisitor::visit(const BinaryExpr<OperatorGt>& e)
{
    visit(static_cast<const Expr&>(e));
}

void ExprVisitor::visit(const BinaryExpr<OperatorGtEq>& e)
{
    visit(static_cast<const Expr&>(e));
}

auto ExprVisitor::index() const -> std::size_t
{
    return index_;
}


}
