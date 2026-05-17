#pragma once

#include "simfil/value.h"
#include "simfil/expression.h"

#include "expressions.h"

#include <memory>

namespace simfil
{

using RewriteRule = std::function<ExprPtr(ExprPtr&)>;

/**
 * Apply a list of rewrite-rules top-down to an expression (sub-)tree.
 */
inline auto rewriteTopDown(ExprPtr expr, std::span<RewriteRule> rules, const RewriteRule* sourceRule = nullptr) -> ExprPtr
{
    for (const auto& rule : rules) {
        // Prevent rule self-recursion.
        if (&rule == sourceRule)
            continue;

        auto rewrite = rule(expr);
        if (rewrite && rewrite.get() != expr.get()) {
            return rewriteTopDown(std::move(rewrite), rules, &rule); // NOLINT
        }
    }

    const auto count = expr->numChildren();
    for (auto i = 0; i < count; ++i) {
        auto& child = expr->childAt(i);
        child = rewriteTopDown(std::move(child), rules, nullptr);
    }

    return std::move(expr);
}

/** Rewrite `PathExpr(AnyChildExpr, FieldExpr)` -> `WildcardFieldExpr(non-recursive)` */
inline auto rewriteAnyChildField(ExprPtr& expr) -> ExprPtr
{
    if (const auto* path = dynamic_cast<const PathExpr*>(expr.get())) {
        const auto* lhs = dynamic_cast<const AnyChildExpr*>(path->left());
        const auto* rhs = dynamic_cast<const FieldExpr*>(path->right());
        if (lhs && rhs && !rhs->isCurrent()) {
            return std::make_unique<WildcardFieldExpr>(false, rhs->field(), rhs->sourceLocation());
        }
    }

    return nullptr;
}

/** Rewrite `PathExpr(WildcardExpr, _) | PathExpr(_, WildcardExpr)` -> `WildcardExpr` */
inline auto rewriteWildcardThis(ExprPtr& expr) -> ExprPtr
{
    auto rewrite = [](const PathExpr* path, const Expr* left, const Expr* right) -> std::unique_ptr<WildcardExpr> {
        const auto* lhs = dynamic_cast<const WildcardExpr*>(left);
        const auto* rhs = dynamic_cast<const FieldExpr*>(right);
        if (lhs && rhs && rhs->isCurrent()) {
            return std::make_unique<WildcardExpr>(lhs->sourceLocation());
        }
        return nullptr;
    };

    if (const auto* path = dynamic_cast<const PathExpr*>(expr.get())) {
        if (auto replacement = rewrite(path, path->left(), path->right()))
            return std::move(replacement);
        if (auto replacement = rewrite(path, path->right(), path->left()))
            return std::move(replacement);
    }

    return nullptr;
}

/** Rewrite `PathExpr(PathExpr(?, WildcardExpr), FieldExpr)` -> `PathExpr(?, WildcardFieldExpr(field))` */
inline auto rewriteAnyWildcardField(ExprPtr& expr) -> ExprPtr
{
    if (auto* path = dynamic_cast<PathExpr*>(expr.get())) {
        auto* lhs = dynamic_cast<PathExpr*>(path->left());
        auto* rhs = dynamic_cast<FieldExpr*>(path->right());
        if (lhs && rhs) {
            auto* lhsRhs = dynamic_cast<WildcardExpr*>(lhs->right());
            if (lhsRhs) {
                return std::make_unique<PathExpr>(std::move(lhs->left_),
                                                  std::make_unique<WildcardFieldExpr>(true, rhs->field(), rhs->sourceLocation()));
            }
        }
    }
    return nullptr;
}

/** Rewrite `PathExpr(WildcardExpr, FieldExpr)` -> `WildcardFieldExpr(field)` */
inline auto rewriteWildcardField(ExprPtr& expr) -> ExprPtr
{
    if (auto* path = dynamic_cast<PathExpr*>(expr.get())) {
        auto* lhs = dynamic_cast<WildcardExpr*>(path->left());
        auto* rhs = dynamic_cast<FieldExpr*>(path->right());
        if (lhs && rhs && !rhs->isCurrent()) {
            return std::make_unique<WildcardFieldExpr>(true, rhs->field(), rhs->sourceLocation());
        }
    }

    return nullptr;
}

}
