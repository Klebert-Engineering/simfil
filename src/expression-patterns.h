#pragma once

#include "expressions.h"
#include "simfil/expression.h"
#include "simfil/model/nodes.h"
#include "src/completion.h"

#include <algorithm>
#include <cctype>

namespace simfil
{

/** Recognize literal-shaped roots for the optional recursive-search completion hint. */
inline auto isSingleValueExpression(const Expr* expr) -> bool {
    if (!expr)
        return false;

    if (dynamic_cast<const CompletionWordExpr*>(expr))
        return true;

    if (auto* v = dynamic_cast<const ConstExpr*>(expr))
        return true;

    return false;
}

/**
 * Recognize a field or uppercase string shape for a wildcard completion hint.
 * This presentation heuristic does not establish enum membership or rewrite a query.
 */
inline auto isSingleValueOrFieldExpression(const Expr* expr) -> bool {
    if (!expr)
        return false;

    if (expr->type() == Expr::Type::FIELD)
        return true;

    if (dynamic_cast<const CompletionWordExpr*>(expr))
        return true;

    if (auto* v = dynamic_cast<const ConstExpr*>(expr)) {
        const auto& value = v->value();
        if (value.isa(ValueType::String)) {
            auto str = value.as<ValueType::String>();
            auto loc = std::locale();
            return std::ranges::all_of(str, [&](auto c) {
                return c == '_' || std::isupper(c, loc);
            }) && !str.empty();
        }
    }

    return false;
}

/**
 * Recognize comparisons eligible for the optional wildcard completion hint:
 * a field or uppercase string on the left, and a constant expression on the right.
 *   `<single-field or uppercase-string-constant> <comparison-operator> <constant-value>`
 * The spelling check is only a hint heuristic, not schema-based enum resolution.
 */
inline auto isFieldComparison(const Expr* expr) -> bool {
    if (!expr)
        return false;
    
    auto checkComparison = [](const auto* compExpr) -> bool {
        if (!compExpr)
            return false;

        auto leftIsFieldOrEnum = false;
        if (const auto* left = dynamic_cast<const FieldExpr*>(compExpr->left_.get())) {
            leftIsFieldOrEnum = true;
        } else if (const auto* left = dynamic_cast<const ConstExpr*>(compExpr->left_.get())) {
            // Keep the legacy uppercase-string hint consistent with the standalone hint.
            const auto& value = left->value();
            if (value.isa(ValueType::String)) {
                auto str = value.as<ValueType::String>();
                auto loc = std::locale();
                leftIsFieldOrEnum = std::ranges::all_of(str, [&](auto c) {
                    return c == '_' || std::isupper(c, loc);
                }) && str.size() > 0;
            }
        }

        auto rightIsConstant = compExpr->right_->constant();

        return leftIsFieldOrEnum && rightIsConstant;
    };
    
    if (auto* e = dynamic_cast<const BinaryExpr<OperatorEq>*>(expr)) {
        return checkComparison(e);
    }
    if (auto* e = dynamic_cast<const BinaryExpr<OperatorNeq>*>(expr)) {
        return checkComparison(e);
    }
    if (auto* e = dynamic_cast<const BinaryExpr<OperatorLt>*>(expr)) {
        return checkComparison(e);
    }
    if (auto* e = dynamic_cast<const BinaryExpr<OperatorLtEq>*>(expr)) {
        return checkComparison(e);
    }
    if (auto* e = dynamic_cast<const BinaryExpr<OperatorGt>*>(expr)) {
        return checkComparison(e);
    }
    if (auto* e = dynamic_cast<const BinaryExpr<OperatorGtEq>*>(expr)) {
        return checkComparison(e);
    }
    
    return false;
}

}
