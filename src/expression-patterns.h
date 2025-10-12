#pragma once

#include "expressions.h"
#include "simfil/expression.h"
#include "simfil/model/nodes.h"
#include "src/completion.h"

#include <algorithm>
#include <cctype>

namespace simfil
{

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
 * Checks if the root expression is a single (constant) value
 * or a field.
 */
inline auto isSingleValueOrFieldExpression(const Expr* expr) -> bool {
    if (!expr)
        return false;

    if (expr->type() == Expr::Type::FIELD)
        return true;

    if (dynamic_cast<const CompletionWordExpr*>(expr))
        return true;

    if (auto* v = dynamic_cast<const ConstExpr*>(expr)) {
        const auto value = v->value();
        if (value.isa(ValueType::String)) {
            auto str = value.as<ValueType::String>();
            auto loc = std::locale();
            return std::all_of(str.begin(), str.end(), [&](auto c) {
                return c == '_' || std::isupper(c, loc);
            }) && !str.empty();
        }
    }

    return false;
}

/**
 * Checks if the root expression is a comparison of the following form:
 *   `<single-field or enum-string-constant> <comparison-operator> <constant-value>`
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
            // Test if the value is a WORD.
            // This is not optimal
            const auto& value = left->value();
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
