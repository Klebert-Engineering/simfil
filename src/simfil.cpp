#include "simfil/simfil.h"
#include "simfil/model/schema-pool.h"
#include "simfil/model/nodes.h"
#include "simfil/model/string-pool.h"
#include "simfil/sourcelocation.h"
#include "simfil/token.h"
#include "simfil/operator.h"
#include "simfil/value.h"
#include "simfil/function.h"
#include "simfil/expression.h"
#include "simfil/parser.h"
#include "simfil/environment.h"
#include "simfil/model/model.h"
#include "simfil/types.h"
#include "simfil/error.h"
#include "fmt/core.h"

#include "completion.h"
#include "evaluation-control.h"
#include "expected.h"
#include "expression-patterns.h"
#include "expression-runtime.h"
#include "expressions.h"
#include "rewrite-rules.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <deque>
#include <cassert>
#include <vector>

namespace simfil
{

using namespace std::string_literals;

namespace strings
{
static constexpr std::string_view TypenameNull("null");
static constexpr std::string_view TypenameBool("bool");
static constexpr std::string_view TypenameInt("int");
static constexpr std::string_view TypenameFloat("float");
static constexpr std::string_view TypenameString("string");
static constexpr std::string_view TypenameBytes("bytes");
}

static const std::array<RewriteRule, 2> bottomUpRewriteRules = {
    rewriteWildcardThis,
    rewriteWildcardField,
};

static const std::array<RewriteRule, 2> topDownRewriteRules = {
    rewriteAnyWildcardField,
    rewriteAnyChildField,
};

/**
 * Parser precedence groups.
 */
enum Precedence {
    PATH        = 12, // a.b
    SUBEXPR     = 11, // a{b}
    SUBSCRIPT   = 10, // a[b]
    POST_UNARY  = 9,  // a?, a exists, a...
    UNARY       = 8,  // not, -, ~
    CAST        = 7,  // a as b
    CUSTOM      = 7,  //
    PRODUCT     = 6,  // *, /, %
    TERM        = 5,  // +, -
    BITWISE     = 4,  // <<, >>, &, |, ^
    COMPARISON  = 3,  // <, <=, >, >=
    EQUALITY    = 2,  // =, ==, !=
    LOGIC       = 1,  // and, or
};

/**
 * Extract the user-facing string from a single field or string-literal query.
 */
static auto schemaLookupName(const Expr& expr) -> std::optional<std::string>
{
    if (auto const* field = dynamic_cast<const FieldExpr*>(&expr)) {
        return field->field();
    }

    if (auto const* constant = dynamic_cast<const ConstExpr*>(&expr)) {
        auto const& value = constant->value();
        if (value.isa(ValueType::String)) {
            return value.as<ValueType::String>();
        }
    }

    return std::nullopt;
}

/**
 * Return true when a parsed string constant came from a named environment constant.
 *
 * The parser substitutes environment constants before schema rewriting. Looking only
 * at the resulting ConstExpr would therefore mistake string-valued bindings for
 * unquoted schema shorthand.
 */
static auto isEnvironmentConstantReference(
    const Environment& env,
    const Expr& expr,
    std::string_view query) -> bool
{
    auto const* constant = dynamic_cast<const ConstExpr*>(&expr);
    if (!constant)
        return false;

    auto const loc = constant->sourceLocation();
    if (loc.size == 0 || loc.offset + loc.size > query.size())
        return false;

    auto const token = query.substr(loc.offset, loc.size);
    if (token.empty() || token.front() == '"' || token.front() == '\'')
        return false;

    return env.findConstant(std::string(token)) != nullptr;
}

/**
 * Return names eligible for operand rewrites. Quoted string literals stay
 * values; unquoted words are parsed as fields and may be reinterpreted by
 * schema metadata below.
 */
static auto schemaOperandShorthandName(
    const Environment& env,
    const Expr& expr,
    std::string_view query) -> std::optional<std::string>
{
    if (auto const* field = dynamic_cast<const FieldExpr*>(&expr)) {
        return field->field();
    }

    if (auto const* constant = dynamic_cast<const ConstExpr*>(&expr)) {
        // Named constants have already won parser name resolution and must not
        // be reinterpreted from their string value as schema shorthand.
        if (isEnvironmentConstantReference(env, expr, query)) {
            return std::nullopt;
        }
        auto const loc = constant->sourceLocation();
        if (loc.size == 0 || loc.offset + loc.size > query.size()) {
            return std::nullopt;
        }
        if (loc.offset < query.size() && (query[loc.offset] == '"' || query[loc.offset] == '\'')) {
            return std::nullopt;
        }
        auto const& value = constant->value();
        if (value.isa(ValueType::String)) {
            return value.as<ValueType::String>();
        }
    }

    return std::nullopt;
}

/**
 * Convert a schema path to a SIMFIL path expression.
 */
static auto pathExpressionFromSchemaPath(SchemaType rootSchema, const SchemaPath& path, SourceLocation location) -> expected<ExprPtr, Error>
{
    ExprPtr expr = std::make_unique<FieldExpr>("_");
    for (auto const& segment : path) {
        ExprPtr next;
        switch (segment.kind) {
        case SchemaPathSegment::Kind::Field: {
            auto fieldName = rootSchema.pool()->dictionary().resolve(segment.field);
            if (!fieldName) {
                return unexpected<Error>(Error::ParserError, "Schema path contains an unknown field string id.");
            }
            next = std::make_unique<FieldExpr>(std::string(*fieldName));
            break;
        }
        case SchemaPathSegment::Kind::ArrayElement:
            next = std::make_unique<AnyChildExpr>();
            break;
        }
        expr = std::make_unique<PathExpr>(std::move(expr), std::move(next), location);
    }
    return expr;
}

/**
 * Build `exact.path == enumValue` expressions for all schema-derived paths.
 */
static auto enumPathExpression(
    SchemaType rootSchema,
    std::vector<SchemaPath> const& paths,
    std::string enumValue,
    SourceLocation location) -> expected<ExprPtr, Error>
{
    ExprPtr result;
    for (auto const& path : paths) {
        auto lhs = pathExpressionFromSchemaPath(rootSchema, path, location);
        TRY_EXPECTED(lhs);

        auto comparison = std::make_unique<BinaryExpr<OperatorEq>>(
            std::move(*lhs),
            std::make_unique<ConstExpr>(Value::make(std::string(enumValue))));

        if (!result)
            result = std::move(comparison);
        else
            result = std::make_unique<OrExpr>(std::move(result), std::move(comparison));
    }
    return result;
}

/** Look up schema vocabulary without inserting completion-only names into a runtime dictionary. */
static auto stringIdForSchemaLookup(SchemaType root, std::string_view name) -> std::optional<StringId>
{
    if (!root)
        return {};
    auto id = root.pool()->dictionary().get(name);
    return id || name.empty() ? std::optional(id) : std::nullopt;
}

static auto expressionForSingleSchemaPath(
    SchemaType rootSchema,
    SchemaPath const& path,
    SourceLocation location) -> expected<ExprPtr, Error>
{
    return pathExpressionFromSchemaPath(rootSchema, path, location);
}

/** Preserve the union of exact operand paths; no alternatives means leave the original operand intact. */
static auto expressionForSchemaPathAlternatives(
    SchemaType rootSchema,
    std::vector<SchemaPath> const& paths,
    SourceLocation location,
    std::string_view) -> expected<ExprPtr, Error>
{
    if (paths.empty()) {
        return nullptr;
    }

    if (paths.size() == 1) {
        return expressionForSingleSchemaPath(rootSchema, paths.front(), location);
    }

    std::vector<ExprPtr> alternatives;
    alternatives.reserve(paths.size());
    for (auto const& path : paths) {
        auto alternative = expressionForSingleSchemaPath(rootSchema, path, location);
        TRY_EXPECTED(alternative);
        if (*alternative) {
            alternatives.push_back(std::move(*alternative));
        }
    }

    if (alternatives.empty()) {
        return nullptr;
    }
    return std::make_unique<PathAlternativesExpr>(std::move(alternatives), location);
}

/**
 * Rewrite a single field/enum query by using schema metadata as source of truth.
 */
static auto rewriteStandaloneNameBySchema(
    Environment& env,
    std::string_view query,
    ExprPtr expr,
    SchemaType rootSchema) -> expected<ExprPtr, Error>
{
    if (!rootSchema || !expr)
        return expr;

    // Environment constants have parser-level precedence over schema aliases.
    if (isEnvironmentConstantReference(env, *expr, query))
        return expr;

    auto name = schemaLookupName(*expr);
    if (!name)
        return expr;

    auto stringId = stringIdForSchemaLookup(rootSchema, *name);
    if (!stringId)
        return expr;

    auto root = rootSchema;
    if (root) {
        auto symbolEqualityPaths = root.symbolEqualityPaths(*stringId);
        if (!symbolEqualityPaths.empty())
            return enumPathExpression(rootSchema, symbolEqualityPaths, std::move(*name), expr->sourceLocation());
    }

    auto fieldPaths = rootSchema.fieldPaths(*stringId);
    const bool knownField = root && std::ranges::binary_search(root.nestedFields(), *stringId);
    if (!fieldPaths.empty() || knownField)
        return std::make_unique<WildcardFieldExpr>(true, std::move(*name), expr->sourceLocation());

    bool namedComplete = true;
    auto namedPaths = rootSchema.namedValuePaths(*name, &namedComplete);
    if (!namedPaths.empty()) {
        if (!namedComplete)
            return std::make_unique<SchemaEnumExpr>(rootSchema, std::move(*name), expr->sourceLocation(), true);
        ExprPtr result;
        for (auto const& path : namedPaths) {
            auto selected = pathExpressionFromSchemaPath(rootSchema, path, expr->sourceLocation());
            TRY_EXPECTED(selected);
            ExprPtr predicate = std::make_unique<SchemaSymbolExpr>(std::move(*selected), *name, "member", false, expr->sourceLocation());
            if (result)
                result = std::make_unique<OrExpr>(std::move(result), std::move(predicate));
            else
                result = std::move(predicate);
        }
        return result;
    }

    bool complete = true;
    auto enumPaths = rootSchema.enumSymbolPaths(*stringId, &complete);
    const bool knownEnum = root && std::ranges::binary_search(root.nestedEnumSymbols(), *stringId);
    if ((!enumPaths.empty() || knownEnum) && !complete)
        return std::make_unique<SchemaEnumExpr>(rootSchema, std::move(*name), expr->sourceLocation());
    if (!enumPaths.empty())
        return enumPathExpression(rootSchema, enumPaths, std::move(*name), expr->sourceLocation());

    return expr;
}

/**
 * Resolve bare operands from schema metadata after whole-query shorthand. Explicit
 * paths keep their member meaning; comparisons can bind named values to the other
 * operand while domain hooks expand scalar aliases into exact path alternatives.
 */
static auto rewriteOperandShorthandBySchema(
    Environment& env,
    std::string_view query,
    ExprPtr expr,
    SchemaType rootSchema,
    bool isRoot,
    bool insidePath) -> expected<ExprPtr, Error>
{
    if (!expr || !rootSchema) {
        return expr;
    }

    auto const entersPath = insidePath || dynamic_cast<const PathExpr*>(expr.get()) != nullptr;
    if (auto comparison = dynamic_cast<ComparisonExprBase*>(expr.get())) {
        for (auto side : {1u, 0u}) {
            auto name = schemaOperandShorthandName(env, *comparison->childAt(side), query);
            if (!name)
                continue;
            auto field = stringIdForSchemaLookup(rootSchema, *name);
            auto root = rootSchema;
            // Real fields and model-specific scalar aliases retain their existing precedence.
            if (!field || !root || !rootSchema.fieldPaths(*field).empty() ||
                !root.scalarFieldPathsForSymbol(*field).empty() ||
                std::ranges::binary_search(root.nestedFields(), *field))
                continue;
            if (rootSchema.namedValuePaths(*name).empty())
                continue;
            std::string operation;
            if (dynamic_cast<BinaryExpr<OperatorEq>*>(comparison)) operation = "==";
            else if (dynamic_cast<BinaryExpr<OperatorNeq>*>(comparison)) operation = "!=";
            else if (dynamic_cast<BinaryExpr<OperatorLt>*>(comparison)) operation = "<";
            else if (dynamic_cast<BinaryExpr<OperatorLtEq>*>(comparison)) operation = "<=";
            else if (dynamic_cast<BinaryExpr<OperatorGt>*>(comparison)) operation = ">";
            else if (dynamic_cast<BinaryExpr<OperatorGtEq>*>(comparison)) operation = ">=";
            else continue;
            auto input = rewriteOperandShorthandBySchema(env, query, std::move(comparison->childAt(1u - side)),
                rootSchema, false, insidePath);
            TRY_EXPECTED(input);
            return std::make_unique<SchemaSymbolExpr>(std::move(*input), std::move(*name), std::move(operation),
                side == 0, expr->sourceLocation());
        }
    }
    if (!isRoot && !insidePath) {
        if (auto name = schemaOperandShorthandName(env, *expr, query)) {
            if (auto stringId = stringIdForSchemaLookup(rootSchema, *name)) {
                if (auto root = rootSchema) {
                    auto paths = root.scalarFieldPathsForSymbol(*stringId);
                    if (!paths.empty()) {
                        auto replacement = expressionForSchemaPathAlternatives(
                            rootSchema,
                            paths,
                            expr->sourceLocation(),
                            *name);
                        TRY_EXPECTED(replacement);
                        if (*replacement) {
                            return std::move(*replacement);
                        }
                    }

                    auto enumPaths = rootSchema.enumSymbolPaths(*stringId);
                    const bool knownEnum = std::ranges::binary_search(root.nestedEnumSymbols(), *stringId);
                    if (!enumPaths.empty() || knownEnum) {
                        auto fieldPaths = rootSchema.fieldPaths(*stringId);
                        if (!fieldPaths.empty()
                            || std::ranges::binary_search(root.nestedFields(), *stringId)) {
                            return expr;
                        }

                        return std::make_unique<ConstExpr>(Value::make(std::move(*name)));
                    }
                }
            }
        }
    }

    auto const count = expr->numChildren();
    for (auto i = 0u; i < count; ++i) {
        auto& child = expr->childAt(i);
        auto rewritten = rewriteOperandShorthandBySchema(env, query, std::move(child), rootSchema, false, entersPath);
        TRY_EXPECTED(rewritten);
        child = std::move(*rewritten);
    }

    return expr;
}

static auto fieldPathSegment(SchemaType rootSchema, std::string_view fieldName) -> std::optional<SchemaPathSegment>
{
    auto fieldId = rootSchema.pool()->dictionary().get(fieldName);
    if (fieldId == StringPool::Empty) {
        return std::nullopt;
    }
    return SchemaPathSegment{SchemaPathSegment::Kind::Field, fieldId};
}

static auto stringConstValue(const Expr& expr) -> std::optional<std::string>
{
    auto const* constant = dynamic_cast<const ConstExpr*>(&expr);
    if (!constant) {
        return std::nullopt;
    }
    auto const& value = constant->value();
    if (!value.isa(ValueType::String)) {
        return std::nullopt;
    }
    return value.as<ValueType::String>();
}

static auto fieldNodeName(const Expr& expr) -> std::optional<std::string>
{
    if (auto const* field = dynamic_cast<const FieldExpr*>(&expr)) {
        return field->field();
    }
    return std::nullopt;
}

static auto addReferencedQueryStringLiteral(ReferencedQueryTerms& terms, std::string literal) -> void
{
    if (!literal.empty()) {
        terms.stringLiterals.insert(std::move(literal));
    }
}

static auto addReferencedQueryLeafField(ReferencedQueryTerms& terms, std::string fieldName) -> void
{
    if (!fieldName.empty()) {
        terms.leafFields.insert(std::move(fieldName));
    }
}

static auto collectReferencedQueryTermsFromExpr(const Expr& expr, ReferencedQueryTerms& terms) -> void;

/** Inventory the terminal member of a path, rather than treating every parent name as a search term. */
static auto collectReferencedQueryTermsFromPathLeaf(const Expr& expr, ReferencedQueryTerms& terms) -> void
{
    if (auto const* field = dynamic_cast<const FieldExpr*>(&expr)) {
        addReferencedQueryLeafField(terms, field->field());
        return;
    }
    if (auto const* wildcardField = dynamic_cast<const WildcardFieldExpr*>(&expr)) {
        addReferencedQueryLeafField(terms, wildcardField->name_);
        return;
    }
    if (auto const* subscript = dynamic_cast<const SubscriptExpr*>(&expr)) {
        if (auto literal = stringConstValue(*subscript->index_)) {
            addReferencedQueryLeafField(terms, *literal);
            addReferencedQueryStringLiteral(terms, std::move(*literal));
            return;
        }
    }
    collectReferencedQueryTermsFromExpr(expr, terms);
}

/** Record a syntactic direct-field equality; parent boolean operators are not logical proof here. */
static auto collectReferencedQueryComparison(
    ReferencedQueryTerms& terms,
    const Expr& lhs,
    const Expr& rhs) -> void
{
    auto fieldName = fieldNodeName(lhs);
    auto literal = stringConstValue(rhs);
    if (fieldName && literal) {
        terms.positiveFieldStringComparisons.push_back({std::move(*fieldName), std::move(*literal)});
    }
}

/** Collect static spellings for downstream discovery without consulting schema or runtime data. */
static auto collectReferencedQueryTermsFromExpr(const Expr& expr, ReferencedQueryTerms& terms) -> void
{
    if (auto predicate = dynamic_cast<const SchemaEnumExpr*>(&expr)) {
        terms.stringLiterals.emplace(predicate->symbol());
        return;
    }
    if (auto const* constant = dynamic_cast<const ConstExpr*>(&expr)) {
        if (auto literal = stringConstValue(*constant)) {
            addReferencedQueryStringLiteral(terms, std::move(*literal));
        }
        return;
    }
    if (auto const* field = dynamic_cast<const FieldExpr*>(&expr)) {
        addReferencedQueryLeafField(terms, field->field());
        return;
    }
    if (auto const* wildcardField = dynamic_cast<const WildcardFieldExpr*>(&expr)) {
        addReferencedQueryLeafField(terms, wildcardField->name_);
        return;
    }
    if (auto const* path = dynamic_cast<const PathExpr*>(&expr)) {
        collectReferencedQueryTermsFromPathLeaf(*path->right(), terms);
        return;
    }
    if (auto const* subscript = dynamic_cast<const SubscriptExpr*>(&expr)) {
        if (auto literal = stringConstValue(*subscript->index_)) {
            addReferencedQueryLeafField(terms, *literal);
            addReferencedQueryStringLiteral(terms, std::move(*literal));
            return;
        }
    }
    if (auto const* eq = dynamic_cast<const BinaryExpr<OperatorEq>*>(&expr)) {
        collectReferencedQueryComparison(terms, *eq->left_, *eq->right_);
        collectReferencedQueryComparison(terms, *eq->right_, *eq->left_);
    }
    for (auto i = 0u; i < expr.numChildren(); ++i) {
        collectReferencedQueryTermsFromExpr(*expr.childAt(i), terms);
    }
}

/**
 * Flatten a static field path expression to a schema path. Returns nullopt for
 * dynamic expressions, broad wildcards, or operators that cannot name one path.
 */
static auto flattenReferencedPath(SchemaType rootSchema, const Expr& expr) -> expected<std::optional<SchemaPath>, Error>
{
    if (auto const* field = dynamic_cast<const FieldExpr*>(&expr)) {
        if (field->isCurrent()) {
            return SchemaPath{};
        }
        auto segment = fieldPathSegment(rootSchema, field->field());
        if (!segment) {
            return std::nullopt;
        }
        return SchemaPath{*segment};
    }

    if (auto const* wildcardField = dynamic_cast<const WildcardFieldExpr*>(&expr)) {
        if (!wildcardField->recurse_) {
            return std::nullopt;
        }
        auto segment = fieldPathSegment(rootSchema, wildcardField->name_);
        if (!segment) {
            return std::nullopt;
        }
        return SchemaPath{*segment};
    }

    if (auto const* path = dynamic_cast<const PathExpr*>(&expr)) {
        auto left = flattenReferencedPath(rootSchema, *path->left());
        TRY_EXPECTED(left);
        if (!*left) {
            return std::nullopt;
        }

        SchemaPath result = std::move(**left);
        if (auto const* field = dynamic_cast<const FieldExpr*>(path->right())) {
            auto segment = fieldPathSegment(rootSchema, field->field());
            if (!segment) {
                return std::nullopt;
            }
            result.push_back(*segment);
            return result;
        }
        if (dynamic_cast<const AnyChildExpr*>(path->right())) {
            result.push_back({SchemaPathSegment::Kind::ArrayElement, 0});
            return result;
        }
        if (auto const* subscript = dynamic_cast<const SubscriptExpr*>(path->right())) {
            auto right = flattenReferencedPath(rootSchema, *subscript);
            TRY_EXPECTED(right);
            if (!*right) {
                return std::nullopt;
            }
            result.insert(result.end(), (*right)->begin(), (*right)->end());
            return result;
        }
        return std::nullopt;
    }

    if (auto const* subscript = dynamic_cast<const SubscriptExpr*>(&expr)) {
        auto left = flattenReferencedPath(rootSchema, *subscript->left_);
        TRY_EXPECTED(left);
        if (!*left) {
            return std::nullopt;
        }
        auto index = stringConstValue(*subscript->index_);
        if (!index) {
            return std::nullopt;
        }
        SchemaPath result = std::move(**left);
        auto segment = fieldPathSegment(rootSchema, *index);
        if (!segment) {
            return std::nullopt;
        }
        result.push_back(*segment);
        return result;
    }

    return std::nullopt;
}

/** Whether a flattened path is relative to a recursive descendant, not the root. */
static auto hasLeadingRecursiveWildcard(const Expr& expr) -> bool
{
    if (auto const* wildcard = dynamic_cast<const WildcardFieldExpr*>(&expr)) {
        return wildcard->recurse_;
    }
    if (auto const* path = dynamic_cast<const PathExpr*>(&expr)) {
        return hasLeadingRecursiveWildcard(*path->left());
    }
    if (auto const* subscript = dynamic_cast<const SubscriptExpr*>(&expr)) {
        return hasLeadingRecursiveWildcard(*subscript->left_);
    }
    return false;
}

/** Coalesce equivalent access evidence while keeping the first source span for that evidence. */
static auto addReferencedPath(
    ReferencedSchemaPaths& result,
    SchemaPath path,
    SourceLocation location,
    bool viaWildcard,
    std::optional<std::string> equalsStringLiteral = std::nullopt,
    bool predicateCanMatch = true) -> void
{
    if (path.empty()) {
        return;
    }
    if (std::ranges::any_of(result.paths, [&](auto const& existing) {
        return existing.path == path
            && existing.viaWildcard == viaWildcard
            && existing.equalsStringLiteral == equalsStringLiteral
            && existing.predicateCanMatch == predicateCanMatch;
    })) {
        return;
    }
    result.paths.push_back({std::move(path), location, viaWildcard,
                            std::move(equalsStringLiteral), predicateCanMatch});
}

/**
 * Test only proven incompatibility with a string equality. A recursive `value`
 * selector can reach numeric attributes and unrelated enums as well as the
 * requested symbol. Those paths remain references, but do not identify useful
 * owners of a positive search predicate. Open strings and incomplete metadata
 * remain possible; named numeric values do not make an exhaustive enum.
 */
static auto schemaPathCanEqualString(
    SchemaType root, const SchemaPath& path, std::string_view literal) -> bool
{
    if (!root.finalized())
        return true;
    std::vector<TypeId> types{root.id()};
    std::size_t visits = 0;
    for (auto const& segment : path) {
        std::vector<TypeId> next;
        for (auto id : types) {
            if (id == NoTypeId || ++visits > 10000)
                return true;
            auto type = root.pool()->schemaType(id);
            auto children = segment.kind == SchemaPathSegment::Kind::Field
                ? type.fieldTypes(segment.field) : type.itemTypes();
            // On a published graph, unknown/open targets are explicit zero
            // entries. An empty lookup means this alternative has no such
            // member, not that the member could have an arbitrary type. A
            // heterogeneous reference array must not turn its numeric IDs
            // into possible string matches because another variant lacks one.
            next.insert(next.end(), children.begin(), children.end());
        }
        std::ranges::sort(next);
        next.erase(std::unique(next.begin(), next.end()), next.end());
        types = std::move(next);
    }

    // Composition alternatives describe the same value position. Any possible
    // string branch is sufficient; cycles or unknown alternatives cannot prove
    // absence. Intersections are deliberately conservative here as well.
    std::vector<TypeId> active, impossible;
    auto canMatch = [&](auto&& self, TypeId id) -> bool {
        if (id == NoTypeId || ++visits > 10000 || active.size() >= 128)
            return true;
        if (std::ranges::find(active, id) != active.end())
            return true;
        if (std::ranges::find(impossible, id) != impossible.end())
            return false;
        auto type = root.pool()->schemaType(id);
        if (!type)
            return true;
        if (!type.hasAffinity(ValueType::String) &&
            !type.hasAffinity(ValueType::TransientObject))
            return false;
        if (!type.directEnumSymbols().empty() || type.enumValueCount()) {
            return type.hasDirectEnumSymbol(literal);
        }
        auto alternatives = type.alternatives();
        if (alternatives.empty())
            return true;
        active.push_back(id);
        auto possible = std::ranges::any_of(alternatives,
            [&](auto child) { return self(self, child); });
        active.pop_back();
        if (possible)
            return true;
        impossible.push_back(id);
        return false;
    };
    return std::ranges::any_of(types, [&](auto id) { return canMatch(canMatch, id); });
}

/** Walk a concrete path, retaining canonical aliases and avoiding whole-graph
 * enumeration. */
static auto canonicalReferencedPath(SchemaType rootSchema,
                                    SchemaPath path)
    -> std::optional<SchemaPath>
{
    std::vector<TypeId> types{rootSchema.id()};
    std::size_t visits = 0;
    for (auto& segment : path) {
        std::vector<TypeId> next, visited;
        std::optional<StringId> canonical;
        while (!types.empty()) {
            if (++visits > 10000)
                return std::nullopt;
            auto id = types.back();
            types.pop_back();
            if (id == NoTypeId || std::ranges::find(visited, id) != visited.end())
                continue;
            visited.push_back(id);
            auto schema = rootSchema.pool()->schemaType(id);
            if (!schema)
                continue;
            if (schema.composition() != SchemaType::Composition::None) {
                auto alternatives = schema.alternatives();
                types.insert(types.end(), alternatives.begin(), alternatives.end());
                continue;
            }
            auto field = schema.canonicalField(segment.field);
            auto children = segment.kind == SchemaPathSegment::Kind::Field
                                ? schema.fieldTypes(field)
                                : schema.itemTypes();
            if (children.empty())
                continue;
            // Open/unknown membership alone does not prove that the field is
            // declared.
            if (segment.kind == SchemaPathSegment::Kind::Field) {
                bool declared = false;
                schema.forEachField([&](SchemaField const& occurrence) { declared |= occurrence.nameId() == field; });
                if (!declared)
                    continue;
                if (canonical && *canonical != field)
                    return std::nullopt;
                canonical = field;
            }
            next.insert(next.end(), children.begin(), children.end());
        }
        if (next.empty())
            return std::nullopt;
        if (canonical)
            segment.field = *canonical;
        std::ranges::sort(next);
        next.erase(std::unique(next.begin(), next.end()), next.end());
        types = std::move(next);
    }
    return path;
}

static auto schemaPathEndsWith(const SchemaPath& path, const SchemaPath& suffix) -> bool
{
    if (suffix.size() > path.size()) {
        return false;
    }
    return std::equal(suffix.rbegin(), suffix.rend(), path.rbegin());
}

/** Expand a recursive suffix while preserving whether all possible owners were visited. */
static auto schemaPathsMatchingSuffix(SchemaType rootSchema, const SchemaPath& suffix, bool& complete) -> std::vector<SchemaPath>
{
    auto leafField = std::ranges::find_if(
        suffix.rbegin(),
        suffix.rend(),
        [](auto const& segment) {
            return segment.kind == SchemaPathSegment::Kind::Field;
        });
    if (leafField == suffix.rend()) {
        complete = false;
        return {};
    }

    auto possiblePaths = rootSchema.fieldPaths(leafField->field, &complete);
    std::erase_if(possiblePaths, [&](auto const& path) {
        if (schemaPathEndsWith(path, suffix))
            return false;
        if (path.size() < suffix.size())
            return true;
        // Resolve aliases at their actual owner, not globally by field spelling.
        auto candidate = path;
        std::copy(suffix.begin(), suffix.end(), candidate.end() - suffix.size());
        auto canonical = canonicalReferencedPath(rootSchema, std::move(candidate));
        return !canonical || *canonical != path;
    });
    return possiblePaths;
}

/** Collect exact access evidence and flag every path shape that needs conservative scope selection. */
static auto collectReferencedSchemaPaths(
    Environment& env,
    const Expr& expr,
    SchemaType rootSchema,
    ReferencedSchemaPaths& result,
    bool positivePredicate = true) -> expected<void, Error>
{
    if (dynamic_cast<const SchemaEnumExpr*>(&expr)) {
        // A recursive type predicate cannot be summarized by a finite path set.
        result.hasUnresolvedAccess = true;
        return {};
    }
    if (auto const* eq = dynamic_cast<const BinaryExpr<OperatorEq>*>(&expr)) {
        auto addComparisonPath = [&](Expr const& maybePath, Expr const& maybeLiteral) -> expected<bool, Error> {
            auto literal = stringConstValue(maybeLiteral);
            if (!literal) {
                return false;
            }

            auto path = flattenReferencedPath(rootSchema, maybePath);
            TRY_EXPECTED(path);
            if (!*path) {
                return false;
            }

            auto pathValue = std::move(**path);
            auto recursive = hasLeadingRecursiveWildcard(maybePath);
            auto canonical = recursive
                ? std::nullopt : canonicalReferencedPath(rootSchema, pathValue);
            if (canonical) {
                auto canMatch = !positivePredicate ||
                    schemaPathCanEqualString(rootSchema, *canonical, *literal);
                addReferencedPath(result, std::move(*canonical),
                                  maybePath.sourceLocation(), false,
                                  std::move(*literal), canMatch);
            } else {
                bool complete = true;
                auto expandedPaths =
                    schemaPathsMatchingSuffix(rootSchema, pathValue, complete);
                if (!complete || expandedPaths.empty()) {
                    result.hasUnresolvedAccess = true;
                }
                for (auto& expandedPath : expandedPaths) {
                    // A comparison preserves its operand's recursive scope.
                    // Consumers may use these paths for styling, but must not
                    // mistake them for explicit root/owner restrictions.
                    auto canMatch = !positivePredicate ||
                        schemaPathCanEqualString(rootSchema, expandedPath, *literal);
                    addReferencedPath(result, std::move(expandedPath),
                                      maybePath.sourceLocation(), recursive, *literal, canMatch);
                }
            }
            return true;
        };

        auto leftAdded = addComparisonPath(*eq->left_, *eq->right_);
        TRY_EXPECTED(leftAdded);
        if (*leftAdded) {
            return {};
        }

        auto rightAdded = addComparisonPath(*eq->right_, *eq->left_);
        TRY_EXPECTED(rightAdded);
        if (*rightAdded) {
            return {};
        }
    }

    if (dynamic_cast<const WildcardExpr*>(&expr)) {
        result.hasBroadWildcardAccess = true;
        return {};
    }

    if (auto const* wildcardField = dynamic_cast<const WildcardFieldExpr*>(&expr)) {
        // Non-recursive child wildcards (`*.foo`) cannot currently be mapped
        // to exact schema paths without exposing child traversal internals.
        if (!wildcardField->recurse_) {
            result.hasDynamicAccess = true;
            return {};
        }

        auto fieldId = rootSchema.pool()->dictionary().get(wildcardField->name_);
        if (fieldId == StringPool::Empty) {
            result.hasUnresolvedAccess = true;
            return {};
        }

        bool complete = true;
        auto paths = rootSchema.fieldPaths(fieldId, &complete);
        // Known paths remain useful, but a truncated set is not proof that
        // every reference belongs to the owners we happened to reach first.
        if (!complete || paths.empty()) {
            result.hasUnresolvedAccess = true;
        }
        for (auto& path : paths) {
            addReferencedPath(result, std::move(path), wildcardField->sourceLocation(), true);
        }
        return {};
    }

    if (dynamic_cast<const FieldExpr*>(&expr)
        || dynamic_cast<const PathExpr*>(&expr)
        || dynamic_cast<const SubscriptExpr*>(&expr)) {
        auto path = flattenReferencedPath(rootSchema, expr);
        TRY_EXPECTED(path);
        if (*path) {
            if (hasLeadingRecursiveWildcard(expr)) {
                // `**.parent.child` flattens to a suffix, not a root path.
                // Expand all matching owners, even when the suffix also exists
                // at the root; otherwise scope inference can miss nested data.
                bool complete = true;
                auto paths = schemaPathsMatchingSuffix(rootSchema, **path, complete);
                if (!complete || paths.empty()) {
                    result.hasUnresolvedAccess = true;
                }
                for (auto& expanded : paths) {
                    addReferencedPath(result, std::move(expanded), expr.sourceLocation(), true);
                }
            } else if (auto canonical =
                           canonicalReferencedPath(rootSchema, **path)) {
                addReferencedPath(result, std::move(*canonical),
                                  expr.sourceLocation(), false);
            } else {
                result.hasUnresolvedAccess = true;
            }
            return {};
        }
        if (dynamic_cast<const SubscriptExpr*>(&expr)) {
            result.hasDynamicAccess = true;
        }
        else {
            result.hasUnresolvedAccess = true;
        }
    }

    // Only boolean conjunction/disjunction and any(...) preserve the need for
    // a positive match. In `not(value == "X")`, a proven nonmatch is relevant;
    // arbitrary functions may also inspect or invert the comparison's value.
    auto childrenArePredicates = positivePredicate &&
        (dynamic_cast<const AndExpr*>(&expr) || dynamic_cast<const OrExpr*>(&expr) ||
         dynamic_cast<const AnyExpr*>(&expr));
    for (auto i = 0u; i < expr.numChildren(); ++i) {
        auto childResult = collectReferencedSchemaPaths(
            env, *expr.childAt(i), rootSchema, result, childrenArePredicates);
        TRY_EXPECTED(childResult);
    }
    return {};
}

/**
 * RAII helper for calling a function at destruction.
 *
 * Run a scope-exit callback, transferring responsibility when the guard is moved.
 */
template <class Fun>
struct scoped {
    Fun f;
    bool call = true;

    explicit scoped(Fun f) : f(std::move(f)) {}
    scoped(scoped&& s) noexcept : f(std::move(s.f)) { s.call = false; }
    scoped(const scoped& s) = delete;
    ~scoped() {
        if (call)
            f();
    }
};

/**
 * Temporarily set the parser context to be not in a path expression.
 */
[[nodiscard]]
static auto scopedNotInPath(Parser& p) {
    auto inPath = false;
    std::swap(p.ctx.inPath, inPath);

    return scoped([&p, inPath]() {
        p.ctx.inPath = inPath;
    });
}

/**
 * Evaluate against an undefined compilation input and fold a finite constant
 * sequence. Undefined or excessive output keeps the expression for runtime;
 * evaluation errors propagate. Then try the bottom-up structural rewrite rules.
 */
static auto simplifyOrForward(const RewriteRule* currentRule, Environment* env, expected<ExprPtr, Error> expr) -> expected<ExprPtr, Error>
{
    if (!expr)
        return expr;
    if (!*expr)
        return nullptr;

    std::deque<Value> values;
    auto stub = Context(env, nullptr, Context::Phase::Compilation);
    auto res = (*expr)->eval(stub, Value::undef(), LambdaResultFn([&, n = 0](Context ctx, Value&& vv) mutable {
        n += 1;
        if ((n <= MultiConstExpr::Limit) && (!vv.isa(ValueType::Undef) || vv.nodePtr())) {
            values.push_back(std::move(vv));
            return Result::Continue;
        }

        values.clear();
        return Result::Stop;
    }));
    TRY_EXPECTED(res);

    /* Warn about constant results */
    if (!values.empty() && std::ranges::all_of(values.begin(), values.end(), [](const Value& v) {
        return v.isa(ValueType::Null);
    }))
        env->warn("Expression is always null"s, (*expr)->toString());

    if (!values.empty() && values[0].isa(ValueType::Bool) && std::ranges::all_of(values.begin(), values.end(), [&](const Value& v) {
        return v.isBool(values[0].as<ValueType::Bool>());
    }))
        env->warn("Expression is always "s + values[0].toString(), (*expr)->toString());

    if (values.size() == 1)
        return std::make_unique<ConstExpr>(std::move(values[0]));
    if (values.size() > 1)
        return std::make_unique<MultiConstExpr>(std::vector<Value>(std::make_move_iterator(values.begin()),
                                                                   std::make_move_iterator(values.end())));

    /* Apply bottom-up rewrite rules */
    for (const auto& rule : bottomUpRewriteRules) {
        /* Prevent rule self-recursion */
        if (&rule == currentRule)
            continue;

        if (auto rewrite = rule(*expr)) {
            /* If a rewrite rule matched we try to simplify and re-write its output again */
            return simplifyOrForward(&rule, env, std::move(rewrite));
        }
    }

    return expr;
}

static auto simplifyOrForward(Environment* env, expected<ExprPtr, Error> expr) -> expected<ExprPtr, Error>
{
    return simplifyOrForward(nullptr, env, std::move(expr));
}


/** Skip all evaluation-based simplification while parsing schema completion. */
static auto simplifyOrForward(Parser& parser, expected<ExprPtr, Error> expr) -> expected<ExprPtr, Error>
{
    return parser.simplify ? simplifyOrForward(parser.env, std::move(expr)) : std::move(expr);
}


AST::~AST() = default;

auto AST::reenumerate() -> void
{
    if (!expr_)
        return;

    auto nextId = Expr::ExprId{0};
    reenumerate(*expr_, nextId);
}

auto AST::reenumerate(Expr& expr, Expr::ExprId& nextId) -> void
{
    expr.id_ = nextId++;

    const auto count = expr.numChildren();
    for (auto i = 0u; i < count; ++i)
        reenumerate(*expr.childAt(i), nextId);
}

/**
 * Parser wrapper for parsing and & or operators.
 *
 * <expr> [and|or] <expr>
 */
class AndOrParser : public InfixParselet
{
public:
    auto parse(Parser& p, ExprPtr left, Token t) const -> expected<ExprPtr, Error> override
    {
        auto right = p.parsePrecedence(precedence());
        if (!right)
            return right;

        if (t.type == Token::OP_AND)
            return simplifyOrForward(p, std::make_unique<AndExpr>(std::move(left),
                                                                      std::move(*right)));
        else if (t.type == Token::OP_OR)
            return simplifyOrForward(p, std::make_unique<OrExpr>(std::move(left),
                                                                     std::move(*right)));
        assert(0);
        return nullptr;
    }

    int precedence() const override
    {
        return Precedence::LOGIC;
    }
};

/** Parse boolean syntax into cursor-aware operands so data truth values do not hide suggestions. */
class CompletionAndOrParser : public InfixParselet
{
public:
    explicit CompletionAndOrParser(const Completion* comp)
        : comp_(comp)
    {}

    auto parse(Parser& p, ExprPtr left, Token t) const -> expected<ExprPtr, Error> override
    {
        auto right = p.parsePrecedence(precedence());
        if (!right)
            return right;

        if (t.type == Token::OP_AND)
            return simplifyOrForward(p, std::make_unique<CompletionAndExpr>(std::move(left),
                                                                                std::move(*right), comp_));
        else if (t.type == Token::OP_OR)
            return simplifyOrForward(p, std::make_unique<CompletionOrExpr>(std::move(left),
                                                                               std::move(*right), comp_));
        assert(0);
        return nullptr;
    }

    int precedence() const override
    {
        return Precedence::LOGIC;
    }

    const Completion* comp_;
};

/** Translate `as <type>` syntax into the corresponding scalar conversion operation. */
class CastParser : public InfixParselet
{
public:
    auto parse(Parser& p, ExprPtr left, Token t) const -> expected<ExprPtr, Error> override
    {
        auto type = p.consume();
        if (type.type == Token::C_NULL)
            return std::make_unique<ConstExpr>(Value::null());

        if (type.type != Token::Type::WORD)
            return unexpected<Error>(Error::InvalidType, fmt::format("'as' expected typename got {}", type.toString()));

        auto name = std::get<std::string>(type.value);
        return simplifyOrForward(p, [&]() -> expected<ExprPtr, Error> {
            if (name == strings::TypenameNull)
                return std::make_unique<ConstExpr>(Value::null());
            if (name == strings::TypenameBool)
                return std::make_unique<UnaryExpr<OperatorBool>>(std::move(left));
            if (name == strings::TypenameInt)
                return std::make_unique<UnaryExpr<OperatorAsInt>>(std::move(left));
            if (name == strings::TypenameFloat)
                return std::make_unique<UnaryExpr<OperatorAsFloat>>(std::move(left));
            if (name == strings::TypenameString)
                return std::make_unique<UnaryExpr<OperatorAsString>>(std::move(left));
            if (name == strings::TypenameBytes)
                return std::make_unique<UnaryExpr<OperatorAsBytes>>(std::move(left));

            return unexpected<Error>(Error::InvalidType, fmt::format("Invalid type name for cast '{}'", name));
        }());
    }

    int precedence() const override
    {
        return Precedence::CAST;
    }
};

/**
 * Parser wrapper for parsing infix operators.
 *
 * <expr> OP <expr>
 */
template <class Operator,
          int Precedence>
class BinaryOpParser : public InfixParselet
{
public:
    auto parse(Parser& p, ExprPtr left, Token t) const -> expected<ExprPtr, Error> override
    {
        auto right = p.parsePrecedence(precedence());
        if (!right)
            return right;

        return simplifyOrForward(p, std::make_unique<BinaryExpr<Operator>>(t,
                                                                               std::move(left),
                                                                               std::move(*right)));
    }

    int precedence() const override
    {
        return Precedence;
    }
};

/**
 * Parser for unary operators.
 *
 * ('-' | '~' | 'not') <expr>
 */
template <class Operator>
class UnaryOpParser : public PrefixParselet
{
    auto parse(Parser& p, Token t) const -> expected<ExprPtr, Error> override
    {
        auto sub = p.parsePrecedence(Precedence::UNARY);
        if (!sub)
            return sub;

        return simplifyOrForward(p, std::make_unique<UnaryExpr<Operator>>(std::move(*sub)));
    }
};

/**
 * Parse postfix unary operator.
 */
template <class Operator>
class UnaryPostOpParser : public InfixParselet
{
    auto parse(Parser& p, ExprPtr left, Token t) const -> expected<ExprPtr, Error> override
    {
        return p.parseInfix(simplifyOrForward(p, std::make_unique<UnaryExpr<Operator>>(std::move(left))), 0);
    }

    auto precedence() const -> int override
    {
        return Precedence::POST_UNARY;
    }
};

/**
 * Parse unpack (...) operator.
 */
class UnpackOpParser : public InfixParselet
{
    auto parse(Parser& p, ExprPtr left, Token t) const -> expected<ExprPtr, Error> override
    {
        return p.parseInfix(simplifyOrForward(p, std::make_unique<UnpackExpr>(std::move(left))), 0);
    }

    auto precedence() const -> int override
    {
        return Precedence::POST_UNARY;
    }
};

/**
 * Parse any word as unary or binary-postfix operator
 */
class WordOpParser : public InfixParselet
{
    auto parse(Parser& p, ExprPtr left, Token t) const -> expected<ExprPtr, Error> override
    {
        /* Try parse as binary operator */
        auto right = p.parsePrecedence(precedence(), true);
        if (!right)
            return right;

        if (*right)
            return simplifyOrForward(p, std::make_unique<BinaryWordOpExpr>(std::get<std::string>(t.value),
                                                                               std::move(left),
                                                                               std::move(*right)));

        /* Parse as unary operator */
        return p.parseInfix(simplifyOrForward(p, std::make_unique<UnaryWordOpExpr>(std::get<std::string>(t.value),
                                                                                       std::move(left))), 0);
    }

    auto precedence() const -> int override
    {
        return Precedence::CUSTOM;
    }
};

/**
 * Parser for parsing scalars.
 *
 * <token>
 */
template <class Type>
class ScalarParser : public PrefixParselet
{
    auto parse(Parser& p, Token t) const -> expected<ExprPtr, Error> override
    {
        return std::make_unique<ConstExpr>(std::get<Type>(t.value), t);
    }
};

/**
 * Parser for parsing regular expression literals.
 *
 * <token>
 */
class RegExpParser : public PrefixParselet
{
    auto parse(Parser& p, Token t) const -> expected<ExprPtr, Error> override
    {
        auto value = ReType::Type.make(std::get<std::string>(t.value));
        return std::make_unique<ConstExpr>(std::move(value), t);
    }
};

/**
 * Parser emitting constant expressions.
 */
class ConstParser : public PrefixParselet
{
public:
    template <class ValueType>
    explicit ConstParser(ValueType value)
        : value_(Value::make(value))
    {}

    explicit ConstParser(Value value)
        : value_(std::move(value))
    {}

    auto parse(Parser& p, Token t) const -> expected<ExprPtr, Error> override
    {
        return std::make_unique<ConstExpr>(value_, t);
    }

    Value value_;
};

/**
 * Parser for parsing grouping parentheses.
 *
 * '(' <expr> ')'
 */
class ParenParser : public PrefixParselet
{
    auto parse(Parser& p, Token t) const -> expected<ExprPtr, Error> override
    {
        auto _ = scopedNotInPath(p);
        return p.parseTo(Token::RPAREN);
    }
};

/**
 * Parser for parsing subscripts.
 *
 * '[' <expr> ']'
 * <expr> '[' <expr> ']'
 */
class SubscriptParser : public PrefixParselet, public InfixParselet
{
    auto parse(Parser& p, Token t) const -> expected<ExprPtr, Error> override
    {
        auto _ = scopedNotInPath(p);
        auto body = p.parseTo(Token::RBRACK);
        if (!body)
            return body;

        return simplifyOrForward(p, std::make_unique<SubscriptExpr>(std::make_unique<FieldExpr>("_"),
                                                                        std::move(*body)));
    }

    auto parse(Parser& p, ExprPtr left, Token t) const -> expected<ExprPtr, Error> override
    {
        auto _ = scopedNotInPath(p);
        auto body = p.parseTo(Token::RBRACK);
        if (!body)
            return body;

        return simplifyOrForward(p, std::make_unique<SubscriptExpr>(std::move(left),
                                                                        std::move(*body)));
    }

    auto precedence() const -> int override
    {
        return Precedence::SUBSCRIPT;
    }
};

/**
 * Parser for parsing sub-expressions
 *
 * '{' <expr> '}'
 * <expr> '{' <expr> '}'
 */
class SubSelectParser : public PrefixParselet, public InfixParselet
{
    auto parse(Parser& p, Token t) const -> expected<ExprPtr, Error> override
    {
        auto _ = scopedNotInPath(p);
        /* Prefix sub-selects are transformed to a right side path expression,
         * with the current node on the left. As "standalone" sub-selects are not useful. */
        auto body = p.parseTo(Token::RBRACE);
        TRY_EXPECTED(body);

        return simplifyOrForward(p, std::make_unique<SubExpr>(std::make_unique<FieldExpr>("_"),
                                                                  std::move(*body)));
    }

    auto parse(Parser& p, ExprPtr left, Token t) const -> expected<ExprPtr, Error> override
    {
        auto _ = scopedNotInPath(p);
        auto body = p.parseTo(Token::RBRACE);
        TRY_EXPECTED(body);
        return simplifyOrForward(p, std::make_unique<SubExpr>(std::move(left),
                                                                  std::move(*body)));
    }

    auto precedence() const -> int override
    {
        return Precedence::SUBEXPR;
    }
};

/**
 * Parser for parsing words into either paths or function calls.
 *
 * <word> ['(' [<expr>] {',' <expr>} ')']
 */
class WordParser : public PrefixParselet
{
public:
    auto parse(Parser& p, Token t) const -> expected<ExprPtr, Error> override
    {
        /* Self */
        if (t.type == Token::SELF)
            return std::make_unique<FieldExpr>("_", t);

        /* Any Child */
        if (t.type == Token::OP_TIMES)
            return std::make_unique<AnyChildExpr>();

        /* Wildcard */
        if (t.type == Token::WILDCARD)
            return std::make_unique<WildcardExpr>();

        auto word = std::get<std::string>(t.value);

        /* Function call */
        if (p.match(Token::LPAREN)) {
            auto _ = scopedNotInPath(p);
            p.consume();

            auto arguments = p.parseList(Token::RPAREN);
            TRY_EXPECTED(arguments);

            if (word == "any") {
                return simplifyOrForward(p, std::make_unique<AnyExpr>(std::move(*arguments)));
            } else if (word == "each" || word == "all") {
                return simplifyOrForward(p, std::make_unique<EachExpr>(std::move(*arguments)));
            } else {
                return simplifyOrForward(p, std::make_unique<CallExpression>(word, std::move(*arguments)));
            }
        } else if (!p.ctx.inPath) {
            /* Constant */
            if (auto constant = p.env->findConstant(word)) {
                return std::make_unique<ConstExpr>(*constant, t);
            }
        }

        /* Single field name */
        return simplifyOrForward(p, std::make_unique<FieldExpr>(std::move(word), t));
    }
};

/**
 * Parser for word (field or function name) completion.
 */
class CompletionWordParser : public WordParser
{
public:
    explicit CompletionWordParser(Completion* comp)
        : comp_(comp)
    {}

    auto parse(Parser& p, Token t) const -> expected<ExprPtr, Error> override
    {
        /* Self */
        if (t.type == Token::SELF)
            return std::make_unique<FieldExpr>("_");

        /* Any Child */
        if (t.type == Token::OP_TIMES)
            return std::make_unique<AnyChildExpr>();

        /* Wildcard */
        if (t.type == Token::WILDCARD)
            return std::make_unique<WildcardExpr>();

        auto word = std::get<std::string>(t.value);

        /* Function call */
        if (p.match(Token::LPAREN)) {
            p.consume();

            /* Downcase function name */
            std::ranges::transform(word.begin(), word.end(), word.begin(), [](auto c) {
                return tolower(c);
            });

            auto arguments = p.parseList(Token::RPAREN);
            TRY_EXPECTED(arguments);

            return simplifyOrForward(p, std::make_unique<CallExpression>(word, std::move(*arguments)));
        } else if (!p.ctx.inPath) {
            /* Constant */
            if (auto constant = p.env->findConstant(word)) {
                return std::make_unique<ConstExpr>(*constant, t);
            }
        }

        /* Single field name */
        if (t.containsPoint(comp_->point)) {
            return std::make_unique<CompletionFieldOrWordExpr>(word.substr(0, comp_->point - t.begin), comp_, t, p.ctx.inPath);
        }
        return simplifyOrForward(p, std::make_unique<FieldExpr>(std::move(word)));
    }

    Completion* comp_;
};
;

/** Reuse tokenization to decode a cursor prefix without executing a literal expression. */
class CompletionStringParser : public PrefixParselet
{
public:
    /** Retain source text only for the duration of this completion parse. */
    CompletionStringParser(Completion& completion, std::string_view query)
        : completion_(completion), query_(query) {}

    /** Preserve whole-token replacement ranges, including any raw-string prefix. */
    auto parse(Parser&, Token token) const -> expected<ExprPtr, Error> override
    {
        if (!token.containsPoint(completion_.point))
            return std::make_unique<ConstExpr>(Value::make(std::get<std::string>(token.value)), token);
        auto prefix = tokenize(query_.substr(0, completion_.point), true);
        TRY_EXPECTED(prefix);
        std::string text;
        if (prefix->size() > 1 && (*prefix)[prefix->size() - 2].type == Token::STRING)
            text = std::get<std::string>((*prefix)[prefix->size() - 2].value);
        if (token.begin > 0 && (query_[token.begin - 1] == 'r' || query_[token.begin - 1] == 'R'))
            --token.begin;
        return std::make_unique<CompletionWordExpr>(std::move(text), &completion_, token);
    }

private:
    Completion& completion_;
    std::string_view query_;
};

/**
 * Parser for parsing '.' separated paths.
 *
 * <expr> '.' <expr>
 */
class PathParser : public InfixParselet
{
public:
    /** Return a source range covering `left . right` for downstream AST rewrites. */
    static auto pathSourceLocation(Expr const& left, Expr const& right, Token const& dot) -> SourceLocation
    {
        auto leftLocation = left.sourceLocation();
        auto rightLocation = right.sourceLocation();
        auto dotBegin = static_cast<std::uint32_t>(dot.begin);
        auto dotEnd = static_cast<std::uint32_t>(dot.end);
        auto begin = leftLocation.size == 0 ? dotBegin : std::min(leftLocation.offset, dotBegin);
        auto end = rightLocation.size == 0
            ? dotEnd
            : std::max(rightLocation.offset + rightLocation.size, dotEnd);
        return SourceLocation(begin, end - begin);
    }

    auto parse(Parser& p, ExprPtr left, Token t) const -> expected<ExprPtr, Error> override
    {
        auto inPath = true;
        std::swap(p.ctx.inPath, inPath);

        scoped _([&p, inPath]() {
            p.ctx.inPath = inPath;
        });

        auto right = p.parsePrecedence(precedence());
        TRY_EXPECTED(right);

        auto location = pathSourceLocation(*left, **right, t);
        return simplifyOrForward(p, std::make_unique<PathExpr>(std::move(left), std::move(*right), location));
    }

    auto precedence() const -> int override
    {
        return Precedence::PATH;
    }
};

/** Supply an empty member placeholder when the cursor follows a dot, for example `road.`. */
class CompletionPathParser : public PathParser
{
public:
    explicit CompletionPathParser(Completion* comp)
        : comp_(comp)
    {}

    auto parse(Parser& p, ExprPtr left, Token t) const -> expected<ExprPtr, Error> override
    {
        auto inPath = true;
        std::swap(p.ctx.inPath, inPath);

        scoped _([&p, inPath]() {
            p.ctx.inPath = inPath;
        });

        auto right = p.parsePrecedence(precedence(), t.containsPoint(comp_->point));
        if (!right)
            return right;

        if (!*right) {
            Token expectedWord(Token::WORD, "", t.end, t.end);
            right = std::make_unique<CompletionFieldOrWordExpr>("", comp_, expectedWord, p.ctx.inPath);
        }

        auto location = pathSourceLocation(*left, **right, t);
        return simplifyOrForward(p, std::make_unique<PathExpr>(std::move(left), std::move(*right), location));
    }

    Completion* comp_;
};

namespace
{
// Stateless parselets are shared; cursor-specific completion parselets are request-local.
const ScalarParser<int64_t> intParser;
const ScalarParser<double> floatParser;
const ScalarParser<std::string> stringParser;
const ScalarParser<ByteArray> bytesParser;
const RegExpParser regexpParser;
const UnaryOpParser<OperatorNegate> negateParser;
const UnaryOpParser<OperatorBitInv> bitInvParser;
const UnaryOpParser<OperatorNot> notParser;
const UnaryOpParser<OperatorLen> lenParser;
const UnaryOpParser<OperatorTypeof> typeofParser;
const UnaryPostOpParser<OperatorBool> boolParser;
const BinaryOpParser<OperatorAdd, Precedence::TERM> addParser;
const BinaryOpParser<OperatorSub, Precedence::TERM> subParser;
const BinaryOpParser<OperatorMul, Precedence::PRODUCT> mulParser;
const BinaryOpParser<OperatorDiv, Precedence::PRODUCT> divParser;
const BinaryOpParser<OperatorMod, Precedence::PRODUCT> modParser;
const BinaryOpParser<OperatorBitAnd, Precedence::BITWISE> bitAndParser;
const BinaryOpParser<OperatorBitOr, Precedence::BITWISE> bitOrParser;
const BinaryOpParser<OperatorBitXor, Precedence::BITWISE> bitXorParser;
const BinaryOpParser<OperatorShl, Precedence::BITWISE> shlParser;
const BinaryOpParser<OperatorShr, Precedence::BITWISE> shrParser;
const BinaryOpParser<OperatorEq, Precedence::EQUALITY> eqParser;
const BinaryOpParser<OperatorNeq, Precedence::EQUALITY> neqParser;
const BinaryOpParser<OperatorLt, Precedence::EQUALITY> ltParser;
const BinaryOpParser<OperatorLtEq, Precedence::EQUALITY> lteqParser;
const BinaryOpParser<OperatorGt, Precedence::EQUALITY> gtParser;
const BinaryOpParser<OperatorGtEq, Precedence::EQUALITY> gteqParser;
const AndOrParser andOrParser;
const CastParser castParser;
const ParenParser parenParser;
const SubSelectParser subSelectParser;
const SubscriptParser subscriptParser;
const WordParser wordParser;
const PathParser pathParser;
const UnpackOpParser unpackParser;
const WordOpParser wordOpParser;
const ConstParser trueParser{Value::t()};
const ConstParser falseParser{Value::f()};
const ConstParser nullParser{Value::null()};
}

/** Register ordinary syntax; completion replaces only the handlers that must observe the cursor. */
static auto setupParser(Parser& p)
{
    /* Scalars */
    p.prefixParsers[Token::C_TRUE]  = &trueParser;
    p.prefixParsers[Token::C_FALSE] = &falseParser;
    p.prefixParsers[Token::C_NULL]  = &nullParser;
    p.prefixParsers[Token::INT]     = &intParser;
    p.prefixParsers[Token::FLOAT]   = &floatParser;
    p.prefixParsers[Token::STRING]  = &stringParser;
    p.prefixParsers[Token::BYTES]   = &bytesParser;
    p.prefixParsers[Token::REGEXP]  = &regexpParser;

    /* Unary Operators */
    p.prefixParsers[Token::OP_SUB]    = &negateParser;
    p.prefixParsers[Token::OP_BITINV] = &bitInvParser;
    p.prefixParsers[Token::OP_NOT]    = &notParser;
    p.prefixParsers[Token::OP_LEN]    = &lenParser;
    p.infixParsers[Token::OP_BOOL]    = &boolParser;
    p.prefixParsers[Token::OP_TYPEOF] = &typeofParser;
    p.infixParsers[Token::OP_UNPACK]  = &unpackParser;
    p.infixParsers[Token::WORD]       = &wordOpParser;

    /* Binary Operators */
    p.infixParsers[Token::OP_ADD]   = &addParser;
    p.infixParsers[Token::OP_SUB]   = &subParser;
    p.infixParsers[Token::OP_TIMES] = &mulParser;
    p.infixParsers[Token::OP_DIV]   = &divParser;
    p.infixParsers[Token::OP_MOD]   = &modParser;

    /* Bit Operators */
    p.infixParsers[Token::OP_BITAND] = &bitAndParser;
    p.infixParsers[Token::OP_BITOR]  = &bitOrParser;
    p.infixParsers[Token::OP_BITXOR] = &bitXorParser;
    p.infixParsers[Token::OP_LSHIFT] = &shlParser;
    p.infixParsers[Token::OP_RSHIFT] = &shrParser;

    /* Comparison/Test */
    p.infixParsers[Token::OP_EQ]     = &eqParser;
    p.infixParsers[Token::OP_NOT_EQ] = &neqParser;
    p.infixParsers[Token::OP_LT]     = &ltParser;
    p.infixParsers[Token::OP_LTEQ]   = &lteqParser;
    p.infixParsers[Token::OP_GT]     = &gtParser;
    p.infixParsers[Token::OP_GTEQ]   = &gteqParser;
    p.infixParsers[Token::OP_AND]    = &andOrParser;
    p.infixParsers[Token::OP_OR]     = &andOrParser;

    /* Cast */
    p.infixParsers[Token::OP_CAST]   = &castParser;

    /* Subexpressions/Subscript */
    p.prefixParsers[Token::LPAREN] = &parenParser;     /* (...) */
    p.prefixParsers[Token::LBRACE] = &subSelectParser; /* {...} */
    p.infixParsers[Token::LBRACE] = &subSelectParser;
    p.prefixParsers[Token::LBRACK] = &subscriptParser; /* [...] */
    p.infixParsers[Token::LBRACK] = &subscriptParser;

    /* Ident/Function */
    p.prefixParsers[Token::WORD] = &wordParser;
    p.prefixParsers[Token::SELF] = &wordParser;

    /* Wildcards */
    p.prefixParsers[Token::WILDCARD] = &wordParser;
    p.prefixParsers[Token::OP_TIMES] = &wordParser;

    /* Paths */
    p.infixParsers[Token::DOT]  = &pathParser;
}

auto compile(Environment& env, std::string_view query, bool any, bool) -> expected<ASTPtr, Error>
{
    return compile(
        env,
        query,
        CompileOptions{
            .any = any,
            .rewriteMode = RewriteMode::None});
}

auto compile(Environment& env, std::string_view query, CompileOptions options) -> expected<ASTPtr, Error>
{
    auto tokens = tokenize(query);
    TRY_EXPECTED(tokens);

    Parser p(&env, *tokens, Parser::Mode::Strict);
    setupParser(p);

    auto expr = [&]() -> expected<ExprPtr, Error> {
        auto root = p.parse();
        TRY_EXPECTED(root);

        if (options.rewriteMode == RewriteMode::Schema && bool(options.rootSchema)) {
            root = rewriteStandaloneNameBySchema(
                env,
                query,
                std::move(*root),
                options.rootSchema);
            TRY_EXPECTED(root);
        }

        if (!*root)
            return unexpected<Error>(Error::ParserError, "Expression is null");

        if (options.any) {
            std::vector<ExprPtr> args;
            args.emplace_back(std::move(*root));
            return simplifyOrForward(p, std::make_unique<AnyExpr>(std::move(args)));
        } else {
            return root;
        }
    }();
    TRY_EXPECTED(expr);

    if (options.rewriteMode == RewriteMode::Schema && bool(options.rootSchema)) {
        expr = rewriteOperandShorthandBySchema(env, query, std::move(*expr), options.rootSchema, true, false);
        TRY_EXPECTED(expr);
    }

    /* Apply AST rewrite rules */
    expr = rewriteTopDown(std::move(*expr), topDownRewriteRules);

    if (!p.match(Token::Type::NIL))
        return unexpected<Error>(Error::ExpectedEOF, "Expected end-of-input; got "s + p.current().toString());

    return std::make_unique<AST>(std::string(query), std::move(*expr));
}

/** Share tokenization, relaxed parselets and candidate presentation for both completion roots. */
static auto completeImpl(Environment& env, std::string_view query, size_t point,
                         const ModelNode* node, SchemaType rootSchema, const CompletionOptions& options)
    -> expected<std::vector<CompletionCandidate>, Error>
{
    if (point > query.size())
        return unexpected<Error>(Error::InvalidArguments, "Completion cursor is outside the query");
    auto tokens = tokenize(query, !node);
    TRY_EXPECTED(tokens);

    if (!node) {
        // An empty operand at the cursor still needs a completion-aware AST node.
        // Tokenization always ends with NIL, including an otherwise empty query.
        auto missingOperand = tokens->size() == 1;
        if (tokens->size() > 1) {
            auto last = (*tokens)[tokens->size() - 2].type;
            missingOperand = last == Token::OP_EQ || last == Token::OP_NOT_EQ || last == Token::DOT
                || last == Token::OP_LT || last == Token::OP_LTEQ || last == Token::OP_GT || last == Token::OP_GTEQ
                || last == Token::LPAREN || last == Token::COMMA || last == Token::OP_AND || last == Token::OP_OR;
        }
        if (missingOperand && point == query.size()) {
            tokens->pop_back();
            tokens->emplace_back(Token::WORD, "", point, point);
            tokens->emplace_back(Token::NIL, point, point);
        }
    }
    Parser p(&env, *tokens, Parser::Mode::Relaxed);
    p.simplify = node != nullptr;
    setupParser(p);

    Completion comp(point, options);
    if (options.limit > 0)
        comp.limit = options.limit;

    CompletionWordParser wordCompletionParser(&comp);
    CompletionStringParser stringCompletionParser(comp, query);
    CompletionPathParser pathCompletionParser(&comp);
    CompletionAndOrParser andOrCompletionParser(&comp);
    p.prefixParsers[Token::WORD]  = &wordCompletionParser;
    if (!node)
        p.prefixParsers[Token::STRING] = &stringCompletionParser;
    p.infixParsers[Token::DOT]    = &pathCompletionParser;
    p.infixParsers[Token::OP_AND] = &andOrCompletionParser;
    p.infixParsers[Token::OP_OR]  = &andOrCompletionParser;

    auto astResult = p.parse();
    if (!p.match(Token::Type::NIL))
        return unexpected<Error>(Error::ExpectedEOF, "Expected end-of-input; got "s + p.current().toString());

    TRY_EXPECTED(astResult);
    auto ast = std::move(*astResult);

    // Determine which hints to show.
    auto showConstantWildcardHint = false;
    auto showFieldWildcardHint = false;
    auto showComparisonWildcardHint = false;
    if (options.showWildcardHints) {
        // Test the query for patterns and hint for converting it
        // to a wildcard query by prepending `**.` to the query.
        if (isSingleValueExpression(ast.get()))
            showConstantWildcardHint = true;

        if (isSingleValueOrFieldExpression(ast.get()))
            showFieldWildcardHint = true;

        if (isFieldComparison(ast.get()))
            showComparisonWildcardHint = true;
    }

    Context ctx(&env, nullptr);
    if (options.timeoutMs > 0)
        ctx.timeout = std::chrono::steady_clock::now() + std::chrono::milliseconds(options.timeoutMs);

    if (node) {
        ast->eval(ctx, Value::field(*node), LambdaResultFn([](Context, const Value&) {
            return Result::Continue;
        }));
    }
    else if (ast) {
        comp.completeType(ctx, *ast, rootSchema);
    }

    auto candidates = std::vector<CompletionCandidate>(comp.candidates.begin(), comp.candidates.end());
    if (options.sorted)
        std::ranges::stable_sort(candidates, [](const auto& left, const auto& right) {
            return left.text < right.text;
        });

    // Show special hints for wildcard expansion.
    if (showFieldWildcardHint)
        candidates.emplace_back(fmt::format("**.{}", query),
                                SourceLocation(0, query.size()),
                                CompletionCandidate::Type::HINT,
                                fmt::format("Query field '{}' recursive", query));

    if (showConstantWildcardHint)
        candidates.emplace_back(fmt::format("** = {}", query),
                                SourceLocation(0, query.size()),
                                CompletionCandidate::Type::HINT,
                                fmt::format("Query fields matching '{}' recursive", query));

    if (showComparisonWildcardHint)
        candidates.emplace_back(fmt::format("**.{}", query),
                                SourceLocation(0, query.size()),
                                CompletionCandidate::Type::HINT,
                                "Expand to recursive query");

    if (!node && candidates.size() > comp.limit)
        candidates.erase(candidates.begin() + comp.limit, candidates.end());
    return candidates;
}

auto complete(Environment& env, std::string_view query, size_t point, const ModelNode& node,
              const CompletionOptions& options) -> expected<std::vector<CompletionCandidate>, Error>
{
    return completeImpl(env, query, point, &node, {}, options);
}

auto complete(Environment& env, std::string_view query, size_t point, SchemaType rootSchema,
              const CompletionOptions& options) -> expected<std::vector<CompletionCandidate>, Error>
{
    return completeImpl(env, query, point, nullptr, rootSchema, options);
}

auto referencedSchemaPaths(Environment& env, const AST& ast, SchemaType rootSchema) -> expected<ReferencedSchemaPaths, Error>
{
    ReferencedSchemaPaths result;
    if (!rootSchema) {
        result.hasUnresolvedAccess = true;
        return result;
    }

    auto collected = collectReferencedSchemaPaths(env, ast.expr(), rootSchema, result);
    TRY_EXPECTED(collected);
    return result;
}

auto referencedQueryTerms(const AST& ast) -> ReferencedQueryTerms
{
    ReferencedQueryTerms result;
    collectReferencedQueryTermsFromExpr(ast.expr(), result);
    return result;
}

auto standaloneQuerySymbol(Environment& env, std::string_view query) -> expected<std::optional<std::string>, Error>
{
    auto ast = compile(
        env,
        query,
        CompileOptions{
            .any = false,
            .rewriteMode = RewriteMode::None});
    TRY_EXPECTED(ast);

    auto const& expr = (*ast)->expr();
    if (auto const* field = dynamic_cast<const FieldExpr*>(&expr)) {
        return field->field();
    }
    if (auto literal = stringConstValue(expr)) {
        return literal;
    }
    return std::nullopt;
}

auto predicateQuerySymbols(Environment& env, std::string_view query)
    -> expected<std::vector<QueryPredicateSymbol>, Error>
{
    auto tokens = tokenize(query);
    TRY_EXPECTED(tokens);

    // Preserve source spans and predicate structure for the caller's substitutions.
    Parser parser(&env, *tokens, Parser::Mode::Strict);
    setupParser(parser);
    parser.simplify = false;
    auto expression = parser.parse();
    TRY_EXPECTED(expression);
    if (!parser.match(Token::Type::NIL))
        return unexpected<Error>(Error::ExpectedEOF,
                                 "Unexpected input after search predicate");

    std::vector<QueryPredicateSymbol> result;
    std::function<void(Expr const&)> visit = [&](Expr const& expr) {
        if (auto name = schemaLookupName(expr); name && !name->starts_with("$")) {
            result.push_back({*name, expr.sourceLocation()});
            return;
        }
        // Only these wrappers preserve a child's role as a predicate. In a
        // comparison such as kind == "ANIMALS", the operands already have roles.
        if (dynamic_cast<AndExpr const*>(&expr) ||
            dynamic_cast<OrExpr const*>(&expr) ||
            dynamic_cast<AnyExpr const*>(&expr) ||
            dynamic_cast<UnaryExpr<OperatorNot> const*>(&expr)) {
            for (size_t i = 0; i < expr.numChildren(); ++i)
                visit(*expr.childAt(i));
        }
    };

    if (*expression)
        visit(**expression);
    return result;
}

/** Evaluate with invocation-local diagnostic counters and the caller/binding's runtime caches. */
static auto evaluate(
    Environment& env,
    const AST& ast,
    const ModelNode& node,
    Diagnostics* diag,
    detail::ExpressionRuntime& runtime,
    const ResultFn& consumer,
    detail::EvaluationControl* control = nullptr) -> expected<Result, Error>
{
    if (!node.owningModel())
        return unexpected<Error>(Error::NullModel, "ModelNode must have a model!");

    // Collect diagnostics only when requested. Preparing AST indices and
    // recording every field/comparison is otherwise discarded on this hot path.
    // Requested diagnostics remain local until the thread-safe merge below.
    Diagnostics localDiag;
    if (diag)
        localDiag.prepareIndices(ast.expr());

    Context ctx(&env, diag ? &localDiag : nullptr);
    ctx.runtime = &runtime;
    ctx.evaluation = control;
    auto res = ast.expr().eval(ctx, Value::field(node), consumer);

    // Merge diagnostics
    if (diag)
        diag->append(localDiag);

    return res;
}

/** Preserve the unbounded vector convenience without duplicating context/cache setup. */
static auto evaluate(Environment& env, const AST& ast, const ModelNode& node, Diagnostics* diag,
                     detail::ExpressionRuntime& runtime) -> expected<std::vector<Value>, Error>
{
    std::vector<Value> values;
    auto result = evaluate(env, ast, node, diag, runtime, LambdaResultFn([&](Context, Value&& value) {
        values.push_back(std::move(value));
        return Result::Continue;
    }));
    TRY_EXPECTED(result);
    return values;
}

/** Stop during enumeration, keeping partial output with the caller instead of accumulating it. */
static auto evaluate(Environment& env, const AST& ast, const ModelNode& node, Diagnostics* diag,
                     detail::ExpressionRuntime& runtime, const ResultFn& consumer,
                     const EvaluationOptions& options) -> expected<EvaluationSummary, Error>
{
    detail::EvaluationControl control(options);
    if (options.maxResults == 0)
        control.stop(EvaluationSummary::Reason::ResultLimit);
    auto result = evaluate(env, ast, node, diag, runtime, LambdaResultFn([&](Context ctx, auto&& value)
        -> expected<Result, Error> {
        if (!control.running())
            return Result::Stop;
        ++control.summary.results;
        auto next = consumer(ctx, std::forward<decltype(value)>(value));
        TRY_EXPECTED(next);
        if (*next == Result::Stop)
            control.stop(EvaluationSummary::Reason::ConsumerStopped);
        else if (control.summary.results >= options.maxResults)
            control.stop(EvaluationSummary::Reason::ResultLimit);
        return control.running() ? Result::Continue : Result::Stop;
    }), &control);
    // An interrupted argument stream may look incomplete to an aggregate. The
    // first cooperative stop reason, not a secondary argument error, is decisive.
    if (control.summary.reason != EvaluationSummary::Reason::Complete)
        return control.summary;
    TRY_EXPECTED(result);
    return control.summary;
}

class BoundExpression::Impl
{
public:
    Impl(SharedAST ast, Environment& env) : ast_(std::move(ast)), env_(env), runtime_(env)
    {
        if (!ast_)
            raise<std::invalid_argument>("Cannot bind a null simfil AST.");
    }

    auto eval(const ModelNode& node, Diagnostics* diag) -> expected<std::vector<Value>, Error>
    {
        return evaluate(env_, *ast_, node, diag, runtime_);
    }

    SharedAST ast_;
    Environment& env_;
    detail::ExpressionRuntime runtime_;
};

BoundExpression::BoundExpression(SharedAST ast, Environment& env)
    : impl_(std::make_unique<Impl>(std::move(ast), env))
{
}

BoundExpression::~BoundExpression() = default;
BoundExpression::BoundExpression(BoundExpression&&) noexcept = default;
auto BoundExpression::operator=(BoundExpression&&) noexcept -> BoundExpression& = default;

auto BoundExpression::eval(const ModelNode& node, Diagnostics* diag)
    -> expected<std::vector<Value>, Error>
{
    return impl_->eval(node, diag);
}

auto BoundExpression::eval(const ModelNode& node, const ResultFn& consumer,
                           const EvaluationOptions& options, Diagnostics* diag)
    -> expected<EvaluationSummary, Error>
{
    return evaluate(impl_->env_, *impl_->ast_, node, diag, impl_->runtime_, consumer, options);
}

auto eval(Environment& env, const AST& ast, const ModelNode& node, Diagnostics* diag)
    -> expected<std::vector<Value>, Error>
{
    detail::ExpressionRuntime runtime(env);
    return evaluate(env, ast, node, diag, runtime);
}

auto eval(Environment& env, const AST& ast, const ModelNode& node, const ResultFn& consumer,
          const EvaluationOptions& options, Diagnostics* diag) -> expected<EvaluationSummary, Error>
{
    detail::ExpressionRuntime runtime(env);
    return evaluate(env, ast, node, diag, runtime, consumer, options);
}

auto diagnostics(const Diagnostics& diag) -> expected<std::vector<Diagnostics::Message>, Error>
{
    return diag.buildMessages();
}

}
