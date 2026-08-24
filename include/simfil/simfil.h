// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tl/expected.hpp>
#include <vector>

#include "simfil/expression.h"
#include "simfil/environment.h"
#include "simfil/diagnostics.h"
#include "simfil/value.h"
#include "simfil/error.h"
#include "simfil/model/schema.h"

namespace simfil
{

struct ModelNode;

/**
 * Bind an immutable compiled expression to one execution environment.
 *
 * The binding owns environment-specific field, function, and schema traversal
 * caches. It may be reused for sequential evaluations but is intentionally not
 * thread-safe; concurrent workers should create separate bindings around the
 * same SharedAST. The Environment must outlive this object.
 */
class BoundExpression
{
public:
    /** Bind `ast` to `env`, which must outlive this object. */
    BoundExpression(SharedAST ast, Environment& env);

    /** Release environment-specific runtime caches. */
    ~BoundExpression();

    /** Transfer one binding and all retained runtime caches. */
    BoundExpression(BoundExpression&&) noexcept;

    /** Replace this binding with another binding and its runtime caches. */
    auto operator=(BoundExpression&&) noexcept -> BoundExpression&;

    BoundExpression(const BoundExpression&) = delete;
    auto operator=(const BoundExpression&) -> BoundExpression& = delete;

    /** Evaluate the expression while retaining its environment-bound caches. */
    auto eval(ModelNode const& node, Diagnostics* diag = nullptr)
        -> tl::expected<std::vector<Value>, Error>;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * Rewrite families available during compilation.
 */
enum class RewriteMode {
    None,
    Schema,
};

/**
 * Options used while parsing and rewriting a query.
 */
struct CompileOptions
{
    bool any = true;
    RewriteMode rewriteMode = RewriteMode::None;
    SchemaId rootSchema = NoSchemaId;
};

/**
 * One schema path referenced by a compiled expression.
 *
 * The path is expressed relative to the root schema supplied to
 * `referencedSchemaPaths`. If `viaWildcard` is set, the path came from a
 * recursive wildcard-field lookup such as `**.foo`.
 */
struct ReferencedSchemaPath
{
    SchemaPath path;
    SourceLocation location;
    bool viaWildcard = false;
    std::optional<std::string> equalsStringLiteral;
};

/**
 * Schema references discovered by static AST inspection.
 *
 * The flags make the result conservative: callers can reject automatic scope
 * decisions when the query contains broad wildcards or field access that cannot
 * be tied to concrete schema paths.
 */
struct ReferencedSchemaPaths
{
    std::vector<ReferencedSchemaPath> paths;
    bool hasDynamicAccess = false;
    bool hasUnresolvedAccess = false;
    bool hasBroadWildcardAccess = false;
};

/**
 * One static `field == "value"` comparison discovered in a compiled query.
 *
 * Only direct positive equality comparisons are reported. The field name is
 * the exact AST field node text and is not interpreted by simfil.
 */
struct FieldStringComparison
{
    std::string fieldName;
    std::string value;
};

/**
 * Schema-independent query terms extracted from a compiled AST.
 *
 * `leafFields` contains the final field-like segment of field/path access,
 * including recursive wildcard field names such as `**.speedLimitKmh`.
 * `stringLiterals` contains string constants that appeared in the query.
 */
struct ReferencedQueryTerms
{
    std::set<std::string> leafFields;
    std::set<std::string> stringLiterals;
    std::vector<FieldStringComparison> positiveFieldStringComparisons;
};

/**
 * Compile expression `src`.
 * Param:
 *   env   Environment used for compilation. Register custom functions there.
 * Param:
 *   query Source code to compile into an expression-tree.
 * Param:
 *   any   If true, wrap expression with call to `any(...)`.
 * Param:
 *   autoWildcard  Deprecated compatibility switch. Ignored; use CompileOptions
 *                 with RewriteMode::Schema and a root schema for rewrites.
 */
auto compile(Environment& env, std::string_view query, bool any = true, bool autoWildcard = false) -> tl::expected<ASTPtr, Error>;

/**
 * Compile expression `src` with explicit options.
 *
 * If rootSchema is set and schema rewrites are enabled, shorthand field/enum
 * queries are classified through the schema instead of lexical heuristics.
 */
auto compile(Environment& env, std::string_view query, CompileOptions options) -> tl::expected<ASTPtr, Error>;

/**
 * Collect schema paths that are referenced by a compiled query.
 *
 * This is static analysis over the AST, not runtime evaluation: both sides of
 * `and`/`or` are inspected, and schema-aware rewrites are resolved to the exact
 * paths they can touch.
 */
auto referencedSchemaPaths(Environment& env, const AST& ast, SchemaId rootSchema) -> tl::expected<ReferencedSchemaPaths, Error>;

/**
 * Return the symbol represented by a whole-query bare field or string literal.
 *
 * This is intentionally AST-based: callers that need exact-query shorthand
 * handling should not re-tokenize the source string with ad-hoc rules.
 */
auto standaloneQuerySymbol(Environment& env, std::string_view query) -> tl::expected<std::optional<std::string>, Error>;

/**
 * Collect schema-independent terms referenced by a compiled query AST.
 *
 * This uses simfil's parser/rewriter output, but deliberately does not require
 * a schema root. Callers can use the returned terms with their own schema
 * indices without re-tokenizing the query string.
 */
auto referencedQueryTerms(const AST& ast) -> ReferencedQueryTerms;

/**
 * Evaluate compiled expression.
 * Param:
 *   env    Environment providing the runtime functions, strings, and schemas.
 * Param:
 *   ast    Expression-Tree generated by prior call to `compile(...)`
 * Param:
 *   node   Root node of the data model to query in
 * Param:
 *   diag   Optional pointer to a diagnostics object to merge results into.
 */
auto eval(Environment& env, const AST& ast, ModelNode const& node, Diagnostics* diag) -> tl::expected<std::vector<Value>, Error>;

/**
 * Build messages for diagnostics collected by `eval`.
 * Param:
 *   env    Environment (must be the same as the one passed to compile and eval)
 * Param:
 *   ast    The AST
 * Param:
 *   diag   Diagnostics data filled by eval.
 */
auto diagnostics(const Diagnostics& diag) -> tl::expected<std::vector<Diagnostics::Message>, Error>;

/**
 * Find completion candidates for an expression.
 * Param:
 *   env   Environment used for compilation & evaluation.
 * Param:
 *   query Source code to complete.
 * Param:
 *   point Index to complete at.
 * Param:
 *   node   Root node of the data model to query in
 */
auto complete(Environment& env, std::string_view query, size_t point, ModelNode const& node, CompletionOptions const& options) -> tl::expected<std::vector<CompletionCandidate>, Error>;

}
