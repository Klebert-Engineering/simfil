// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#include <cstdlib>

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

    virtual void visit(const Expr& expr);
    virtual void visit(const WildcardExpr& expr);
    virtual void visit(const AnyChildExpr& expr);
    virtual void visit(const MultiConstExpr& expr);
    virtual void visit(const ConstExpr& expr);
    virtual void visit(const SubscriptExpr& expr);
    virtual void visit(const SubExpr& expr);
    virtual void visit(const AnyExpr& expr);
    virtual void visit(const EachExpr& expr);
    virtual void visit(const CallExpression& expr);
    virtual void visit(const PathExpr& expr);
    virtual void visit(const FieldExpr& expr);
    virtual void visit(const UnpackExpr& expr);
    virtual void visit(const UnaryWordOpExpr& expr);
    virtual void visit(const BinaryWordOpExpr& expr);
    virtual void visit(const AndExpr& expr);
    virtual void visit(const OrExpr& expr);
    virtual void visit(const BinaryExpr<OperatorEq>& expr);
    virtual void visit(const BinaryExpr<OperatorNeq>& expr);
    virtual void visit(const BinaryExpr<OperatorLt>& expr);
    virtual void visit(const BinaryExpr<OperatorLtEq>& expr);
    virtual void visit(const BinaryExpr<OperatorGt>& expr);
    virtual void visit(const BinaryExpr<OperatorGtEq>& expr);

protected:
    /* Returns the index of the current expression */
    std::size_t index() const;

private:
    std::size_t index_ = 0;
};

}
