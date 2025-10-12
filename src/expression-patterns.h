#pragma once

#include "expressions.h"
#include <algorithm>
#include <cctype>

namespace simfil
{

/**
 * Checks if the root expression is a single (constant) value.
 */
inline bool isSingleValueExpression(const Expr* expr) {
    if (!expr)
        return false;

    if (dynamic_cast<const ConstExpr*>(expr)) {
        return true;
    }

    return false;
}

/**
 * Checks if the root expression is a comparison of the following form:
 *   `<single-field or enum-string-constant> <comparison-operator> <constant-value>`
 */
inline bool isSimpleFieldComparison(const Expr* expr) {
    if (!expr)
        return false;
    
    auto checkComparison = [](const auto* compExpr) -> bool {
        if (!compExpr)
            return false;

        auto leftIsFieldOrEnum = false;
        if (const auto* left = dynamic_cast<const FieldExpr*>(compExpr->left_.get())) {
            leftIsFieldOrEnum = true;
        } else if (const auto* left = dynamic_cast<const ConstExpr*>(compExpr->left_.get())) {
            // Test if the value is a WORD.
            auto value = left->value();
            if (value.isa(ValueType::String)) {
                auto str = value.as<ValueType::String>();
                auto loc = std::locale();
                leftIsFieldOrEnum = std::all_of(str.begin(), str.end(), [&](auto c) {
                    return c == '_' || std::isupper(c, loc);
                }) && str.size() > 0;
            }
        }

        auto rightIsConstant = compExpr->right_->constant();

        return leftIsFieldOrEnum && rightIsConstant;
    };
    
    if (auto* eqExpr = dynamic_cast<const BinaryExpr<OperatorEq>*>(expr)) {
        return checkComparison(eqExpr);
    }
    if (auto* neqExpr = dynamic_cast<const BinaryExpr<OperatorNeq>*>(expr)) {
        return checkComparison(neqExpr);
    }
    if (auto* ltExpr = dynamic_cast<const BinaryExpr<OperatorLt>*>(expr)) {
        return checkComparison(ltExpr);
    }
    if (auto* lteqExpr = dynamic_cast<const BinaryExpr<OperatorLtEq>*>(expr)) {
        return checkComparison(lteqExpr);
    }
    if (auto* gtExpr = dynamic_cast<const BinaryExpr<OperatorGt>*>(expr)) {
        return checkComparison(gtExpr);
    }
    if (auto* gteqExpr = dynamic_cast<const BinaryExpr<OperatorGtEq>*>(expr)) {
        return checkComparison(gteqExpr);
    }
    
    return false;
}

}
