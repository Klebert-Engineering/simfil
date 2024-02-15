// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#include <memory>
#include <vector>
#include <string_view>

#include "simfil/token.h"
#include "simfil/expression.h"
#include "simfil/environment.h"
#include "simfil/value.h"
#include "simfil/model/model.h"

namespace simfil
{

/**
 * Compile expression `src`.
 * Param:
 *   env  Environment used for compilation. Register custom functions there.
 * Param:
 *   src  Source code to compile into an expression-tree.
 * Param:
 *   any  If true, wrap expression with call to `any(...)`.
 */
auto compile(Environment& env, std::string_view src, bool any = true) -> ExprPtr;

/**
 * Evaluate compiled expression.
 * Param:
 *   env        Environment (must be same as the one passed to compile!)
 * Param:
 *   ast        Expression-Tree generated by prior call to `compile(...)`
 * Param:
 *   model      Model to query.
 * Param:
 *   rootIndex  Root node index to query.
 */
auto eval(Environment& env, const Expr& ast, ModelPool const& model, size_t rootIndex=0) -> std::vector<Value>;

/**
 * Evaluate compiled expression.
 * Param:
 *   env    Environment (must be same as the one passed to compile!)
 * Param:
 *   ast    Expression-Tree generated by prior call to `compile(...)`
 * Param:
 *   node   Root node of the data model to query in
 */
auto eval(Environment& env, const Expr& ast, ModelNode const& node) -> std::vector<Value>;

}
