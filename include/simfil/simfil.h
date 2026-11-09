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
#include "simfil/evaluation.h"
#include "simfil/environment.h"
#include "simfil/diagnostics.h"
#include "simfil/value.h"
#include "simfil/error.h"
#include "simfil/model/schema-type.h"

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

    /** Stream results with cooperative limits and retain only environment-bound caches. */
    auto eval(ModelNode const& node, const ResultFn& consumer, const EvaluationOptions& options = {},
              Diagnostics* diag = nullptr) -> tl::expected<EvaluationSummary, Error>;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * Select schema-driven shorthand rewriting. Ordinary constant folding and
 * syntax-preserving AST optimizations still run with None.
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
    bool any = true; ///< Wrap the result in any(...) to produce a predicate.
    RewriteMode rewriteMode = RewriteMode::None;
    SchemaType rootSchema;
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
 * be tied to concrete schema paths. `hasUnresolvedAccess` also marks incomplete
 * enumeration (unknown metadata, cycles or traversal limits), even when some
 * concrete paths were found; those paths are not an exhaustive ownership proof.
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
 * Only a direct FieldExpr/constant equality node is reported; paths and `!=`
 * are excluded. This is syntactic evidence, not a proof that the equality must
 * hold: its parent may be `not` or `or`. The field spelling is not interpreted.
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
 * Parse and compile `query` using the functions registered in `env`.
 *
 * By default, wrap the expression in `any(...)` to produce a predicate. Set
 * `any` to false to preserve the expression's result sequence. Parse and
 * compilation failures are returned as Error values.
 *
 * `autoWildcard` is a deprecated, ignored compatibility argument. Schema-based
 * shorthand uses the CompileOptions overload with RewriteMode::Schema.
 */
auto compile(Environment& env, std::string_view query, bool any = true, bool autoWildcard = false) -> tl::expected<ASTPtr, Error>;

/**
 * Parse and compile `query`, applying the selected rewrites before evaluation.
 *
 * With RewriteMode::Schema and a nonempty rootSchema, shorthand fields and enum
 * symbols are resolved against that schema. For example, a bare field name may
 * become a recursive field lookup. An empty rootSchema selects schema-free
 * compilation. Set options.any to false for value-producing queries; its default
 * wraps the result in `any(...)`.
 */
auto compile(Environment& env, std::string_view query, CompileOptions options) -> tl::expected<ASTPtr, Error>;

/**
 * Collect schema paths that are referenced by a compiled query.
 *
 * This is static analysis over the AST, not runtime evaluation.
 * Used by consumers that choose a search scope from the fields a query touches.
 * Both sides of `and`/`or` are inspected, including paths introduced by schema
 * rewrites. Path field IDs belong to rootSchema's dictionary.
 *
 * Check the returned flags before treating paths as exhaustive. Dynamic access,
 * broad wildcards and incomplete schema traversal can leave potential accesses
 * unresolved even when some concrete paths were found. An empty rootSchema sets
 * hasUnresolvedAccess; an empty path list alone cannot prove that no fields are used.
 */
auto referencedSchemaPaths(Environment& env, const AST& ast, SchemaType rootSchema) -> tl::expected<ReferencedSchemaPaths, Error>;

/**
 * Extract one shorthand name from a query whose compiled root is a field or string.
 *
 * For example, `ANIMALS` and `"ANIMALS"` return the string "ANIMALS"; `road.kind`
 * and `kind == "ANIMALS"` return an empty optional. Consumers can use the name to
 * offer an application-specific interpretation before running the query.
 *
 * This is intentionally AST-based: callers that need exact-query shorthand
 * handling should not re-tokenize the source string with ad-hoc rules.
 *
 * Compilation uses ordinary simplification, with schema rewrites and the automatic
 * any(...) wrapper disabled. The name is returned without checking whether a field
 * or enum symbol exists. A valid compound query returns an empty optional; invalid
 * syntax returns a compilation Error. Use predicateQuerySymbols to inspect names
 * inside boolean predicates while preserving their original source locations.
 */
auto standaloneQuerySymbol(Environment& env, std::string_view query) -> tl::expected<std::optional<std::string>, Error>;

/**
 * A bare or quoted symbol used as a predicate, not as a comparison operand.
 * Locations refer to the original, unsimplified input so a consumer can bind
 * its meaning before any(...) or boolean composition changes its AST role.
 * Consumers use the span to substitute a chosen meaning before compilation.
 */
struct QueryPredicateSymbol {
    std::string name;
    SourceLocation location;
};

/**
 * Collect shorthand names through `and`, `or`, `not` and `any(...)` predicates.
 *
 * `any(ANIMALS) and "SPEED_UNLIMITED"` yields those two names with their original
 * source spans. `kind == "ANIMALS"` yields none: comparison operands already have
 * an explicit role. Names beginning with `$` are excluded. Parsing preserves the
 * unsimplified expression, and malformed input returns an Error.
 */
auto predicateQuerySymbols(Environment& env, std::string_view query)
    -> tl::expected<std::vector<QueryPredicateSymbol>, Error>;

/**
 * Collect schema-independent terms referenced by a compiled query AST.
 *
 * This uses simfil's parser/rewriter output, but deliberately does not require
 * a schema root. Callers can use the returned terms with their own schema
 * indices without re-tokenizing the query string.
 */
auto referencedQueryTerms(const AST& ast) -> ReferencedQueryTerms;

/**
 * Evaluate a compiled expression and collect every value produced from node.
 * This convenience overload has no result/work limit; use the callback overload
 * for bounded work.
 * env provides the runtime functions, strings and schemas, and must use the node
 * model's field dictionary. Optional diag receives counters merged from this
 * evaluation; reuse it only for this same AST/index layout.
 */
auto eval(Environment& env, const AST& ast, ModelNode const& node, Diagnostics* diag) -> tl::expected<std::vector<Value>, Error>;

/**
 * Stream values without an intermediate result vector. Work counts expression
 * entries, intermediate emissions and wildcard traversal, including zero-hit work.
 * Custom functions must cooperate with Context::step() and Result::Stop; these
 * limits cannot preempt a blocking function or bound a single value's byte size.
 */
auto eval(Environment& env, const AST& ast, const ModelNode& node, const ResultFn& consumer,
          const EvaluationOptions& options = {}, Diagnostics* diag = nullptr)
    -> tl::expected<EvaluationSummary, Error>;

/**
 * Build messages from collected field/comparison counters. Finish concurrent
 * evaluation/merging before reading diag: message construction does not lock it.
 */
auto diagnostics(const Diagnostics& diag) -> tl::expected<std::vector<Diagnostics::Message>, Error>;

/**
 * Suggest source-span replacements at byte offset point using actual model data
 * and its attached schema. This overload evaluates the relaxed query and may run
 * registered functions; its timeout is cooperative. Use the same field dictionary
 * in env and node. Results may be partial, and appended wildcard hints may exceed
 * options.limit. The SchemaType overload supports completion without executing data.
 */
auto complete(Environment& env, std::string_view query, size_t point, ModelNode const& node, CompletionOptions const& options) -> tl::expected<std::vector<CompletionCandidate>, Error>;

/**
 * Suggest replacements at byte offset `point` in `query` using rootSchema.
 *
 * For example, a schema declaring `road.speed` can suggest `speed` for the `sp`
 * span in `road.sp`. Each candidate supplies its replacement text and source span;
 * the caller replaces that span rather than appending the whole candidate.
 *
 * This overload follows schema fields, array elements and enum metadata. It can
 * run before sample data is loaded and does not execute custom functions. Names
 * are read from the schema's dictionary; returned candidates own their strings.
 * This overload never mutates the datasource dictionary.
 *
 * CompletionOptions controls sorting, smart-case matching, candidate count,
 * timeout and schema visit/depth limits. The vector contains the candidates found
 * within those limits and carries no separate completeness indicator. An empty
 * result therefore does not establish that the schema has no matching fields.
 */
auto complete(Environment& env, std::string_view query, size_t point, SchemaType rootSchema,
              CompletionOptions const& options = {}) -> tl::expected<std::vector<CompletionCandidate>, Error>;

}
