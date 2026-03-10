// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#include <cstdint>

namespace simfil
{

class Expr;
class WildcardExpr;
class AnyChildExpr;
class MultiConstExpr;
class ConstExpr;
class SubscriptExpr;
class SubExpr;
class AnyExpr;
class EachExpr;
class CallExpression;
class UnpackExpr;
class UnaryWordOpExpr;
class BinaryWordOpExpr;
class FieldExpr;
class PathExpr;
class AndExpr;
class OrExpr;
struct OperatorEq;
struct OperatorNeq;
struct OperatorLt;
struct OperatorLtEq;
struct OperatorGt;
struct OperatorGtEq;
template <class> class UnaryExpr;
template <class> class BinaryExpr;

/**
 * Visitor base for visiting expressions recursively.
 */
class ExprVisitor
{
public:
    ExprVisitor();
    virtual ~ExprVisitor();

    virtual void visit(Expr& expr);
    virtual void visit(WildcardExpr& expr);
    virtual void visit(AnyChildExpr& expr);
    virtual void visit(MultiConstExpr& expr);
    virtual void visit(ConstExpr& expr);
    virtual void visit(SubscriptExpr& expr);
    virtual void visit(SubExpr& expr);
    virtual void visit(AnyExpr& expr);
    virtual void visit(EachExpr& expr);
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
    std::size_t index() const;

private:
    std::size_t index_ = 0;
};

}
