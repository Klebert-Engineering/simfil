// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#include "simfil/expression-visitor.h"

#include "expressions.h"

namespace simfil
{

ExprVisitor::ExprVisitor() = default;
ExprVisitor::~ExprVisitor() = default;

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

auto ExprVisitor::index() const -> std::size_t
{
    return index_;
}


}
