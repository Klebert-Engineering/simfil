#pragma once

#include "simfil/model/string-pool.h"
#include "simfil/model/value-type.h"
#include <algorithm>
#include <cassert>
#include <compare>
#include <concepts>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <ranges>
#include <sfl/small_vector.hpp>
#include <span>
#include <stdexcept>
#include <vector>

namespace simfil
{

class Schema;

using SchemaId = std::uint16_t;
constexpr SchemaId NoSchemaId = SchemaId{0};
constexpr SchemaId MaxSchemaId = SchemaId{std::numeric_limits<SchemaId>::max()};

/**
 * One segment in a schema-derived query path.
 *
 * Field segments address object members. Array-element segments represent the
 * non-recursive `*` operator needed to traverse array elements precisely.
 */
struct SchemaPathSegment
{
    enum class Kind {
        Field,
        ArrayElement,
    };

    Kind kind = Kind::Field;
    StringId field = 0;

    auto operator<=>(const SchemaPathSegment&) const = default;
};

/** Sequence of schema path segments from a root schema to a reachable value. */
using SchemaPath = std::vector<SchemaPathSegment>;

/**
 * Concept defining a callback to query a Schema* by SchemaId.
 */
template <class Fn>
concept QuerySchemaFn = requires(const Fn& fn) {
    { fn(SchemaId{}) } -> std::convertible_to<const Schema*>;
};
template <class Fn>
concept QueryMutableSchemaFn = requires(const Fn& fn) {
    { fn(SchemaId{}) } -> std::convertible_to<Schema*>;
};

/** Read-only domain interface shared by schema queries, completion and pruning. */
class Schema
{
public:
    /** Possible present values; absence is field metadata, not a null value. */
    static constexpr std::uint16_t AllAffinities = (1u << unsigned(ValueType::LAST_)) - 2;
    static constexpr std::uint16_t ScalarAffinities = (1u << unsigned(ValueType::TransientObject)) - 2;

    /** Open packed kind: static name StringId above structural-affinity bits. */
    enum class Kind : std::uint32_t {
        Unknown = (StringPool::SchemaUnknown << 16) | AllAffinities,
        Any = (StringPool::SchemaAny << 16) | AllAffinities,
        Never = StringPool::SchemaNever << 16,
        Value = (StringPool::SchemaValue << 16) | ScalarAffinities,
        Null = (StringPool::SchemaNull << 16) | valueTypeAffinity(ValueType::Null),
        Bool = (StringPool::SchemaBool << 16) | valueTypeAffinity(ValueType::Bool),
        Int = (StringPool::SchemaInt << 16) | valueTypeAffinity(ValueType::Int),
        Float = (StringPool::SchemaFloat << 16) | valueTypeAffinity(ValueType::Float),
        String = (StringPool::SchemaString << 16) | valueTypeAffinity(ValueType::String),
        Bytes = (StringPool::SchemaBytes << 16) | valueTypeAffinity(ValueType::Bytes),
        Object = (StringPool::SchemaObject << 16) | valueTypeAffinity(ValueType::Object),
        Array = (StringPool::SchemaArray << 16) | valueTypeAffinity(ValueType::Array),
        Union = (StringPool::SchemaUnion << 16) | AllAffinities,
        OneOf = (StringPool::SchemaOneOf << 16) | AllAffinities,
        Intersection = (StringPool::SchemaIntersection << 16) | AllAffinities,
    };

    /** Logical alternatives apply at one value position, never an array level. */
    enum class Composition { None, AnyOf, OneOf, AllOf };

    /** Bind identities to borrowed definitions whose owner outlives all traversal. */
    using Lookup = std::function<const Schema*(SchemaId)>;

    /** Construct an extension kind without registering its semantics in simfil. */
    static constexpr auto makeKind(StringId name, std::uint16_t affinities) -> Kind
    {
        assert(name != StringPool::Empty && name < StringPool::FirstDynamicId);
        return Kind((std::uint32_t(name) << 16) | affinities);
    }

    /** Return the static name id, independent of the represented structure. */
    static constexpr auto kindNameId(Kind kind) -> StringId { return std::uint32_t(kind) >> 16; }

    /** Return the coarse set of possible runtime types, not full constraints. */
    static constexpr auto affinities(Kind kind) -> std::uint16_t { return std::uint32_t(kind) & 0xffff; }

    /** Recognize generic structure even for an unfamiliar specialized kind. */
    auto hasAffinity(ValueType type) const -> bool { return affinities(kind()) & valueTypeAffinity(type); }

    /** Preserve the concrete producer type name; anonymous domains leave it empty. */
    virtual auto typeName() const -> std::string_view { return typeName_; }
    /** Assign a concrete producer name and invalidate metadata-derived indexes. */
    void setTypeName(std::string name) { typeName_ = std::move(name); invalidate(); }

    /** Change semantic classification while retaining the implementation's traversal. */
    void setKind(Kind kind) { kindOverride_ = kind; invalidate(); }

    /** Whether undeclared object fields are allowed by this domain. */
    virtual auto open() const -> bool { return open_; }
    /** Permit or forbid undeclared object members independently of known fields. */
    void setOpen(bool open) { open_ = open; invalidate(); }

    /** Known nullability is separate from a field's optional presence. */
    virtual auto nullable() const -> std::optional<bool> { return nullable_; }
    /** Record known nullability and update the coarse affinity summary. */
    void setNullable(bool nullable) { nullable_ = nullable; invalidate(); }

    /** Known presence requirement for a direct field, or unknown metadata. */
    virtual auto fieldRequired(StringId) const -> std::optional<bool> { return {}; }

    /** Resolve a model's lookup-only field alias without adding duplicate
     * enumerated paths. */
    virtual auto canonicalField(StringId field) const -> StringId
    {
        return field;
    }

    /** Non-string enum/constant values; all string choices use directEnumSymbols in the binding's pool. */
    virtual auto enumValues() const & -> std::span<const ScalarValueType> { return {}; }

    /** Preserve the combiner rather than merging away exclusive/intersection semantics. */
    virtual auto composition() const -> Composition { return Composition::None; }
    /** Domain identities combined at this value position, not child array elements. */
    virtual auto alternatives() const & -> std::span<const SchemaId> { return {}; }

    /** Cached proof that field reachability contains no unknown or open descendants. */
    virtual auto reachabilityComplete() const -> bool { return reachabilityComplete_; }

    /** Resolve the possible domains of a member, flattening only logical alternatives. */
    static auto fieldSchemas(SchemaId root, const Lookup& lookup, StringId field) -> std::vector<SchemaId>;

    /** Resolve item domains independently of a prospective runtime array index. */
    static auto itemSchemas(SchemaId root, const Lookup& lookup) -> std::vector<SchemaId>;

    /** Finalization state */
    enum class State {
        Dirty,
        Finalizing,
        Clean,
    };

    using SchemaIdStack = sfl::small_vector<SchemaId, 8>;

    virtual ~Schema() = default;

    /**
     * Return this schemas kind.
     */
    virtual auto kind() const -> Kind { return domainKind(Kind::Unknown); }

    /**
     * Returns true if this schema or any of the schemas it refers to
     * can possibly contain the given field.
     */
    virtual auto canHaveField(StringId) const -> bool
    {
        return hasAffinity(ValueType::Object) || hasAffinity(ValueType::Array);
    }

    /**
     * Returns true if this schema or any of the schemas it refers to
     * can possibly contain the given enum-like string symbol.
     */
    virtual auto canHaveEnumSymbol(StringId symbolId) const -> bool
    {
        return false;
    }

    /**
     * Finalize this schema and all schemas it refers to.
     */
    virtual auto finalize(const std::function<Schema*(SchemaId)>& queryFn) -> State
    {
        return State::Clean;
    }

    /**
     * @return All nested field names.
     */
    virtual auto nestedFields() const & -> std::span<const StringId> { return {}; }

    /**
     * Return field names directly available on this schema node.
     *
     * Completion uses this to suggest fields valid at the current node without
     * also suggesting fields that only occur deeper in the schema graph.
     */
    virtual auto directFields() const & -> std::span<const StringId>
    {
        return nestedFields();
    }

    /**
     * @return All nested enum-like string symbols.
     */
    virtual auto nestedEnumSymbols() const & -> std::span<const StringId>
    {
        return {};
    }

    /**
     * Return enum-like string symbols accepted directly by this schema node.
     *
     * Unlike nestedEnumSymbols(), this does not include descendants and is used
     * to derive precise schema paths for schema-backed rewrites.
     */
    virtual auto directEnumSymbols() const & -> std::span<const StringId>
    {
        return {};
    }

    /** Match a direct enum by text without inserting schema-only symbols into a runtime pool. */
    virtual auto hasDirectEnumSymbol(std::string_view symbol, StringPool const& strings) const -> bool
    {
        for (auto id : directEnumSymbols()) {
            auto text = strings.resolve(id);
            if (text && *text == symbol)
                return true;
        }
        return false;
    }

    /**
     * Enumerate precise paths to all fields with the requested name.
     */
    static auto fieldPaths(SchemaId root,
                           const std::function<const Schema*(SchemaId)>& queryFn,
                           StringId field,
                           bool* complete = nullptr) -> std::vector<SchemaPath>
    {
        std::vector<SchemaPath> paths;
        SchemaIdStack visited;
        SchemaPath current;
        bool exhaustive = true;
        std::size_t visits = 0;
        collectFieldPaths(root, queryFn, field, visited, current, paths, exhaustive, visits);
        if (complete)
            *complete = exhaustive;
        sortUniquePaths(paths);
        return paths;
    }

    /**
     * Enumerate precise paths to all values that can hold the enum-like symbol.
     */
    static auto enumSymbolPaths(SchemaId root,
                                const std::function<const Schema*(SchemaId)>& queryFn,
                                StringId symbol,
                                bool* complete = nullptr) -> std::vector<SchemaPath>
    {
        std::vector<SchemaPath> paths;
        SchemaIdStack visited;
        SchemaPath current;
        bool exhaustive = true;
        std::size_t visits = 0;
        collectEnumSymbolPaths(root, queryFn, symbol, visited, current, paths, exhaustive, visits);
        if (complete)
            *complete = exhaustive;
        sortUniquePaths(paths);
        return paths;
    }

    /**
     * Return paths that should be compared with the supplied string symbol when
     * the symbol appears as a full standalone query. Embedders can use this for
     * schema-native aliases such as attribute type-code predicates.
     */
    virtual auto symbolEqualityPaths(
        StringId,
        const std::function<const Schema*(SchemaId)>&) const -> std::vector<SchemaPath>
    {
        return {};
    }

    /**
     * Return scalar field paths that should replace the supplied symbol when it
     * appears as a standalone operand inside a larger expression.
     */
    virtual auto scalarFieldPathsForSymbol(
        StringId,
        const std::function<const Schema*(SchemaId)>&) const -> std::vector<SchemaPath>
    {
        return {};
    }

    /**
     * Return the first reachable scalar path below the supplied schema.
     */
    static auto firstScalarFieldPath(
        SchemaId root,
        const std::function<const Schema*(SchemaId)>& queryFn) -> std::optional<SchemaPath>
    {
        SchemaIdStack visited;
        SchemaPath current;
        return firstScalarFieldPath(root, queryFn, visited, current);
    }

    /**
     * Return true once `canHaveField` is backed by finalized field caches.
     */
    virtual auto finalized() const -> bool
    {
        return true;
    }

    /**
     * Monotonic counter for cache invalidation after schema mutations.
     */
    virtual auto revision() const -> std::uint64_t
    {
        return metadataRevision_;
    }

protected:
    /** Apply optional metadata without losing the kind's static symbolic name. */
    auto domainKind(Kind fallback) const -> Kind
    {
        auto result = kindOverride_.value_or(fallback);
        if (nullable_)
            result = makeKind(kindNameId(result), *nullable_
                ? affinities(result) | valueTypeAffinity(ValueType::Null)
                : affinities(result) & ~valueTypeAffinity(ValueType::Null));
        return result;
    }

    /** Derived caches must be invalidated when structural metadata changes. */
    virtual void invalidate() { ++metadataRevision_; reachabilityComplete_ = false; }

    /** Compute completeness from the graph, independently of cyclic finalization state. */
    auto computeReachabilityComplete(const std::function<Schema*(SchemaId)>& lookup) const -> bool;

    std::uint64_t metadataRevision_ = 0;
    mutable bool reachabilityComplete_ = false;
    std::optional<Kind> kindOverride_;
    std::string typeName_;
    bool open_ = false;
    std::optional<bool> nullable_;

    /**
     * Append all fields reachable from this schema without relying on cached
     * finalization state. This lets cyclic schema graphs still produce an exact
     * field set by cutting recursion at already visited schema ids.
     */
    virtual auto collectNestedFields(const std::function<Schema*(SchemaId)>& queryFn,
                                     SchemaIdStack& visited,
                                     std::vector<StringId>& fields) const -> void
    {
        forEachDirectField([&](StringId field, std::span<const SchemaId> children) {
            fields.push_back(field);
            for (auto child : children)
                appendSchemaFields(child, queryFn, visited, fields);
        });
        forEachElementSchema([&](SchemaId child) { appendSchemaFields(child, queryFn, visited, fields); });
        for (auto child : alternatives())
            appendSchemaFields(child, queryFn, visited, fields);
    }

    /**
     * Append all enum-like string symbols reachable from this schema without
     * relying on cached finalization state.
     */
    virtual auto collectNestedEnumSymbols(const std::function<Schema*(SchemaId)>& queryFn,
                                          SchemaIdStack& visited,
                                          std::vector<StringId>& symbols) const -> void
    {
        auto symbolsHere = directEnumSymbols();
        symbols.insert(symbols.end(), symbolsHere.begin(), symbolsHere.end());
        forEachDirectField([&](StringId, std::span<const SchemaId> children) {
            for (auto child : children)
                appendSchemaEnumSymbols(child, queryFn, visited, symbols);
        });
        forEachElementSchema([&](SchemaId child) { appendSchemaEnumSymbols(child, queryFn, visited, symbols); });
        for (auto child : alternatives())
            appendSchemaEnumSymbols(child, queryFn, visited, symbols);
    }

public:
    /**
     * Visit fields declared directly by this schema and their possible child
     * schemas. The default is empty for scalar schemas.
     */
    virtual auto forEachDirectField(
        const std::function<void(StringId, std::span<const SchemaId>)>&) const -> void
    {
    }

    /**
     * Visit possible array element schemas. The default is empty for non-arrays.
     */
    virtual auto forEachElementSchema(const std::function<void(SchemaId)>&) const -> void
    {
    }

protected:
    /**
     * Recursively collect schema paths to matching fields.
     */
    static auto collectFieldPaths(SchemaId schemaId,
                                  const std::function<const Schema*(SchemaId)>& queryFn,
                                  StringId field,
                                  SchemaIdStack& visited,
                                  SchemaPath& current,
                                  std::vector<SchemaPath>& paths,
                                  bool& complete, std::size_t& visits) -> void
    {
        if (++visits > 10000 || schemaId == NoSchemaId || std::ranges::find(visited, schemaId) != visited.end()
            || visited.size() >= 128 || paths.size() >= 10000) {
            complete = false;
            return;
        }

        auto const* schema = queryFn(schemaId);
        if (!schema) {
            complete = false;
            return;
        }

        if (!affinities(schema->kind()))
            return;
        visited.push_back(schemaId);

        auto canonical = schema->canonicalField(field);
        schema->forEachDirectField([&](StringId directField, std::span<const SchemaId> childSchemas) {
            current.push_back({SchemaPathSegment::Kind::Field, directField});
            if (directField == canonical && paths.size() < 10000)
                paths.push_back(current);
            if (childSchemas.empty())
                complete = false;
            for (auto childSchemaId : childSchemas)
                collectFieldPaths(childSchemaId, queryFn, field, visited, current, paths, complete, visits);
            current.pop_back();
        });

        bool hasElements = false;
        schema->forEachElementSchema([&](SchemaId elementSchemaId) {
            hasElements = true;
            current.push_back({SchemaPathSegment::Kind::ArrayElement, 0});
            collectFieldPaths(elementSchemaId, queryFn, field, visited, current, paths, complete, visits);
            current.pop_back();
        });

        if (schema->hasAffinity(ValueType::Array) && !hasElements && schema->composition() == Composition::None)
            complete = false;
        if (schema->composition() == Composition::AllOf && schema->alternatives().empty())
            complete = false;

        for (auto alternative : schema->alternatives())
            collectFieldPaths(alternative, queryFn, field, visited, current, paths, complete, visits);
        if (schema->open() || kindNameId(schema->kind()) == kindNameId(Kind::Unknown) || kindNameId(schema->kind()) == kindNameId(Kind::Any))
            complete = false;
        visited.pop_back();
    }

    /**
     * Recursively collect schema paths to values accepting a matching enum-like
     * string symbol.
     */
    static auto collectEnumSymbolPaths(SchemaId schemaId,
                                       const std::function<const Schema*(SchemaId)>& queryFn,
                                       StringId symbol,
                                       SchemaIdStack& visited,
                                       SchemaPath& current,
                                       std::vector<SchemaPath>& paths,
                                  bool& complete, std::size_t& visits) -> void
    {
        if (++visits > 10000 || schemaId == NoSchemaId || std::ranges::find(visited, schemaId) != visited.end()
            || visited.size() >= 128 || paths.size() >= 10000) {
            complete = false;
            return;
        }

        auto const* schema = queryFn(schemaId);
        if (!schema) {
            complete = false;
            return;
        }

        if (!affinities(schema->kind()))
            return;
        visited.push_back(schemaId);

        for (auto directSymbol : schema->directEnumSymbols()) {
            if (directSymbol == symbol)
                paths.push_back(current);
        }

        schema->forEachDirectField([&](StringId directField, std::span<const SchemaId> childSchemas) {
            current.push_back({SchemaPathSegment::Kind::Field, directField});
            if (childSchemas.empty())
                complete = false;
            for (auto childSchemaId : childSchemas)
                collectEnumSymbolPaths(childSchemaId, queryFn, symbol, visited, current, paths, complete, visits);
            current.pop_back();
        });

        bool hasElements = false;
        schema->forEachElementSchema([&](SchemaId elementSchemaId) {
            hasElements = true;
            current.push_back({SchemaPathSegment::Kind::ArrayElement, 0});
            collectEnumSymbolPaths(elementSchemaId, queryFn, symbol, visited, current, paths, complete, visits);
            current.pop_back();
        });

        if (schema->hasAffinity(ValueType::Array) && !hasElements && schema->composition() == Composition::None)
            complete = false;
        if (schema->composition() == Composition::AllOf && schema->alternatives().empty())
            complete = false;

        for (auto alternative : schema->alternatives())
            collectEnumSymbolPaths(alternative, queryFn, symbol, visited, current, paths, complete, visits);
        if (schema->open() || kindNameId(schema->kind()) == kindNameId(Kind::Unknown) || kindNameId(schema->kind()) == kindNameId(Kind::Any))
            complete = false;
        visited.pop_back();
    }

    /**
     * Recursively find the first scalar field path in schema declaration order.
     */
    static auto firstScalarFieldPath(SchemaId schemaId,
                                     const std::function<const Schema*(SchemaId)>& queryFn,
                                     SchemaIdStack& visited,
                                     SchemaPath& current) -> std::optional<SchemaPath>
    {
        if (schemaId == NoSchemaId || std::ranges::find(visited, schemaId) != visited.end())
            return std::nullopt;

        auto const* schema = queryFn(schemaId);
        if (!schema || !affinities(schema->kind()))
            return std::nullopt;

        if (schema->composition() == Composition::None
            && !schema->hasAffinity(ValueType::Object) && !schema->hasAffinity(ValueType::Array)
            && affinities(schema->kind()) != 0)
            return current;

        visited.push_back(schemaId);

        for (auto alternative : schema->alternatives()) {
            if (auto result = firstScalarFieldPath(alternative, queryFn, visited, current)) {
                visited.pop_back();
                return result;
            }
        }

        if (schema->hasAffinity(ValueType::Object)) {
            std::optional<SchemaPath> result;
            schema->forEachDirectField([&](StringId directField, std::span<const SchemaId> childSchemas) {
                if (result)
                    return;

                current.push_back({SchemaPathSegment::Kind::Field, directField});
                if (childSchemas.empty()) {
                    result = current;
                }
                else {
                    for (auto childSchemaId : childSchemas) {
                        result = firstScalarFieldPath(childSchemaId, queryFn, visited, current);
                        if (result)
                            break;
                    }
                }
                current.pop_back();
            });
            visited.pop_back();
            return result;
        }

        std::optional<SchemaPath> result;
        schema->forEachElementSchema([&](SchemaId elementSchemaId) {
            if (result)
                return;

            current.push_back({SchemaPathSegment::Kind::ArrayElement, 0});
            result = firstScalarFieldPath(elementSchemaId, queryFn, visited, current);
            current.pop_back();
        });

        visited.pop_back();
        return result;
    }

    /**
     * Keep path rewrites deterministic and avoid duplicate paths from combined
     * schemas or shared references.
     */
    static auto sortUniquePaths(std::vector<SchemaPath>& paths) -> void
    {
        std::ranges::sort(paths);
        auto duplicates = std::ranges::unique(paths);
        paths.erase(duplicates.begin(), duplicates.end());
    }

    /**
     * Append reachable values through a schema id, using finalized child
     * caches when possible and falling back to raw graph traversal for cycles.
     */
    template <class CachedValuesFn, class CollectValuesFn>
    static auto appendSchemaValues(SchemaId schemaId,
                                   const std::function<Schema*(SchemaId)>& queryFn,
                                   SchemaIdStack& visited,
                                   std::vector<StringId>& values,
                                   CachedValuesFn&& cachedValues,
                                   CollectValuesFn&& collectValues) -> void
    {
        if (schemaId == NoSchemaId || std::ranges::find(visited, schemaId) != visited.end())
            return;

        auto* schema = queryFn(schemaId);
        if (!schema)
            return;

        visited.push_back(schemaId);

        if (schema->finalize(queryFn) == State::Clean && schema->reachabilityComplete()) {
            auto childValues = std::invoke(cachedValues, *schema);
            values.insert(values.end(), childValues.begin(), childValues.end());
            return;
        }

        std::invoke(collectValues, *schema, queryFn, visited, values);
    }

    /**
     * Append fields reachable through a schema id.
     */
    static auto appendSchemaFields(SchemaId schemaId,
                                   const std::function<Schema*(SchemaId)>& queryFn,
                                   SchemaIdStack& visited,
                                   std::vector<StringId>& fields) -> void
    {
        appendSchemaValues(
            schemaId,
            queryFn,
            visited,
            fields,
            [](const Schema& schema) { return schema.nestedFields(); },
            [](const Schema& schema, const auto& query, auto& visitedSchemas, auto& values) {
                schema.collectNestedFields(query, visitedSchemas, values);
            });
    }

    /**
     * Append enum-like string symbols reachable through a schema id.
     */
    static auto appendSchemaEnumSymbols(SchemaId schemaId,
                                        const std::function<Schema*(SchemaId)>& queryFn,
                                        SchemaIdStack& visited,
                                        std::vector<StringId>& symbols) -> void
    {
        appendSchemaValues(
            schemaId,
            queryFn,
            visited,
            symbols,
            [](const Schema& schema) { return schema.nestedEnumSymbols(); },
            [](const Schema& schema, const auto& query, auto& visitedSchemas, auto& values) {
                schema.collectNestedEnumSymbols(query, visitedSchemas, values);
            });
    }

    /**
     * Sort ids and remove duplicates.
     */
    static auto sortUnique(std::vector<StringId>& values) -> void
    {
        std::ranges::sort(values);
        auto duplicates = std::ranges::unique(values);
        values.erase(duplicates.begin(), duplicates.end());
    }

    /**
     * Shared finalization implementation for schemas that cache descendant
     * fields and enum-like string symbols.
     */
    static auto finalizeReachableMetadata(State& state,
                                          std::vector<StringId>& flatFields,
                                          std::vector<StringId>& flatEnumSymbols,
                                          const std::function<Schema*(SchemaId)>& queryFn,
                                          const Schema& schema) -> State
    {
        if (state == State::Clean || state == State::Finalizing)
            return state;

        state = State::Finalizing;
        flatFields.clear();
        flatEnumSymbols.clear();

        SchemaIdStack visitedFields;
        schema.collectNestedFields(queryFn, visitedFields, flatFields);
        sortUnique(flatFields);

        SchemaIdStack visitedEnumSymbols;
        schema.collectNestedEnumSymbols(queryFn, visitedEnumSymbols, flatEnumSymbols);
        sortUnique(flatEnumSymbols);

        schema.reachabilityComplete_ = schema.computeReachabilityComplete(queryFn);
        state = State::Clean;
        return State::Clean;
    }

    /**
     * Shared membership test; dirty schemas remain conservative.
     */
    static auto containsField(State state, const std::vector<StringId>& flatFields, StringId field) -> bool
    {
        if (state != State::Clean)
            return true;

        auto iter = std::ranges::lower_bound(flatFields, field);
        return iter != flatFields.end() && *iter == field;
    }
};

/**
 * Schema for object nodes.
 *
 * Stores direct fields and optional child schema ids per field. After
 * `finalize()` it also caches all reachable child fields.
 */
class ObjectSchema : public Schema
{
public:
    struct FieldSummary {
        StringId field = 0;
        sfl::small_vector<SchemaId, 1> schemas;
        std::optional<bool> required;

        auto operator<=>(const FieldSummary& other) const
        {
            return field <=> other.field;
        }
    };

    auto kind() const -> Kind override
    {
        return domainKind(Kind::Object);
    }

    auto canHaveField(StringId field) const -> bool override
    {
        return !reachabilityComplete() || containsField(state_, flatFields_, field);
    }

    auto canHaveEnumSymbol(StringId symbol) const -> bool override
    {
        return containsField(state_, flatEnumSymbols_, symbol);
    }

    /**
     * Add a direct field and optional child schemas reachable through it.
     */
    auto addField(StringId field, std::initializer_list<SchemaId> schemas = {},
                  std::optional<bool> required = {}) -> void
    {
        FieldSummary summary;
        summary.field = field;
        summary.required = required;
        summary.schemas.insert(summary.schemas.end(), schemas.begin(), schemas.end());
        fields_.push_back(std::move(summary));
        directFields_.push_back(field);
        state_ = State::Dirty;
        reachabilityComplete_ = false;
        ++revision_;
    }

    /**
     * Recompute the cached descendant field set from this schema and all
     * reachable child schemas.
     */
    auto finalize(const std::function<Schema*(SchemaId)>& lookup) -> State override
    {
        return finalizeReachableMetadata(state_, flatFields_, flatEnumSymbols_, lookup, *this);
    }

    /** Presence metadata is specific to this parent-field edge. */
    auto fieldRequired(StringId field) const -> std::optional<bool> override
    {
        for (const auto& item : fields_)
            if (item.field == field)
                return item.required;
        return {};
    }

    auto fields() const & -> std::span<const FieldSummary>
    {
        return {fields_.begin(), fields_.end()};
    }

    auto forEachDirectField(
        const std::function<void(StringId, std::span<const SchemaId>)>& fn) const -> void override
    {
        for (auto const& field : fields_)
            fn(field.field, {field.schemas.begin(), field.schemas.end()});
    }

    auto nestedFields() const & -> std::span<const StringId> override
    {
        return {flatFields_.cbegin(), flatFields_.cend()};
    }

    auto directFields() const & -> std::span<const StringId> override
    {
        return {directFields_.cbegin(), directFields_.cend()};
    }

    auto nestedEnumSymbols() const & -> std::span<const StringId> override
    {
        return {flatEnumSymbols_.cbegin(), flatEnumSymbols_.cend()};
    }

    auto finalized() const -> bool override
    {
        return state_ == State::Clean;
    }

    auto revision() const -> std::uint64_t override
    {
        return revision_ + metadataRevision_;
    }

private:
    /** Invalidate derived indexes when domain metadata changes. */
    void invalidate() override { Schema::invalidate(); state_ = State::Dirty; }

    auto collectNestedFields(const std::function<Schema*(SchemaId)>& lookup,
                             SchemaIdStack& visited,
                             std::vector<StringId>& fields) const -> void override
    {
        for (const auto& field : fields_) {
            fields.push_back(field.field);
            for (const auto& fieldSchemaId : field.schemas)
                appendSchemaFields(fieldSchemaId, lookup, visited, fields);
        }
    }

    auto collectNestedEnumSymbols(const std::function<Schema*(SchemaId)>& lookup,
                                  SchemaIdStack& visited,
                                  std::vector<StringId>& symbols) const -> void override
    {
        for (const auto& field : fields_) {
            for (const auto& fieldSchemaId : field.schemas)
                appendSchemaEnumSymbols(fieldSchemaId, lookup, visited, symbols);
        }
    }

    sfl::small_vector<FieldSummary, 4> fields_;

    std::vector<StringId> directFields_;
    std::vector<StringId> flatFields_; // Ordered!
    std::vector<StringId> flatEnumSymbols_; // Ordered!
    std::uint64_t revision_ = 0;
    State state_ = State::Dirty;
};

/**
 * Schema for scalar value nodes.
 *
 * Stores optional enum-like string symbols for schema-aware completion and
 * parsing. Value schemas never contribute nested fields.
 */
class ValueSchema : public Schema
{
public:
    /** An untyped scalar domain, or a precise builtin scalar kind. */
    explicit ValueSchema(Kind kind = Kind::Value) { setKind(kind); }

    /** Scalar domains have no descendant fields; a widened extension kind remains conservative. */
    auto reachabilityComplete() const -> bool override
    {
        return !open() && !hasAffinity(ValueType::Object) && !hasAffinity(ValueType::Array);
    }

    /** Retain non-string literals. String choices must use addEnumSymbol so rewrites share the same index. */
    void addEnumValue(ScalarValueType value)
    {
        if (std::holds_alternative<std::string>(value) || std::holds_alternative<std::string_view>(value))
            throw std::invalid_argument("String enum values require addEnumSymbol in the binding's string pool");
        enumValues_.push_back(std::move(value));
        invalidate();
    }

    auto enumValues() const & -> std::span<const ScalarValueType> override { return enumValues_; }

    auto kind() const -> Kind override
    {
        return domainKind(Kind::Value);
    }

    auto canHaveField(StringId) const -> bool override
    {
        return !reachabilityComplete();
    }

    auto canHaveEnumSymbol(StringId symbol) const -> bool override
    {
        return containsField(state_, enumSymbols_, symbol);
    }

    /**
     * Add an enum-like string symbol accepted by this value schema.
     */
    auto addEnumSymbol(StringId symbol) -> void
    {
        enumSymbols_.push_back(symbol);
        state_ = State::Dirty;
        reachabilityComplete_ = false;
        ++revision_;
    }

    auto finalize(const std::function<Schema*(SchemaId)>&) -> State override
    {
        if (state_ == State::Clean || state_ == State::Finalizing)
            return state_;

        state_ = State::Finalizing;
        sortUnique(enumSymbols_);
        state_ = State::Clean;
        return State::Clean;
    }

    auto nestedFields() const & -> std::span<const StringId> override
    {
        return {};
    }

    auto nestedEnumSymbols() const & -> std::span<const StringId> override
    {
        return {enumSymbols_.cbegin(), enumSymbols_.cend()};
    }

    auto directEnumSymbols() const & -> std::span<const StringId> override
    {
        return {enumSymbols_.cbegin(), enumSymbols_.cend()};
    }

    auto finalized() const -> bool override
    {
        return state_ == State::Clean;
    }

    auto revision() const -> std::uint64_t override
    {
        return revision_ + metadataRevision_;
    }

private:
    /** Invalidate derived indexes when domain metadata changes. */
    void invalidate() override { Schema::invalidate(); state_ = State::Dirty; }

    auto collectNestedFields(const std::function<Schema*(SchemaId)>&,
                             SchemaIdStack&,
                             std::vector<StringId>&) const -> void override
    {
    }

    auto collectNestedEnumSymbols(const std::function<Schema*(SchemaId)>&,
                                  SchemaIdStack&,
                                  std::vector<StringId>& symbols) const -> void override
    {
        symbols.insert(symbols.end(), enumSymbols_.begin(), enumSymbols_.end());
    }

    std::vector<ScalarValueType> enumValues_;
    std::vector<StringId> enumSymbols_; // Ordered after finalize().
    std::uint64_t revision_ = 0;
    State state_ = State::Dirty;
};

/**
 * Schema for array nodes.
 *
 * Stores the set of possible element schemas. After `finalize()` it caches
 * all fields reachable through any element schema.
 */
class ArraySchema : public Schema
{
public:
    auto kind() const -> Kind override
    {
        return domainKind(Kind::Array);
    }

    auto canHaveField(StringId field) const -> bool override
    {
        return !reachabilityComplete() || containsField(state_, flatFields_, field);
    }

    auto canHaveEnumSymbol(StringId symbol) const -> bool override
    {
        return containsField(state_, flatEnumSymbols_, symbol);
    }

    /**
     * Add possible schemas for elements contained in the array.
     */
    auto addElementSchemas(std::initializer_list<SchemaId> schemas) -> void
    {
        schemas_.insert(schemas_.end(), schemas.begin(), schemas.end());
        state_ = State::Dirty;
        reachabilityComplete_ = false;
        ++revision_;
    }

    /**
     * Recompute the cached descendant field set from all possible element
     * schemas.
     */
    auto finalize(const std::function<Schema*(SchemaId)>& lookup) -> State override
    {
        return finalizeReachableMetadata(state_, flatFields_, flatEnumSymbols_, lookup, *this);
    }

    auto nestedFields() const & -> std::span<const StringId> override
    {
        return {flatFields_.cbegin(), flatFields_.cend()};
    }

    auto nestedEnumSymbols() const & -> std::span<const StringId> override
    {
        return {flatEnumSymbols_.cbegin(), flatEnumSymbols_.cend()};
    }

    auto finalized() const -> bool override
    {
        return state_ == State::Clean;
    }

    auto revision() const -> std::uint64_t override
    {
        return revision_ + metadataRevision_;
    }

    auto elementSchemas() const & -> std::span<const SchemaId>
    {
        return {schemas_.begin(), schemas_.end()};
    }

    auto forEachElementSchema(const std::function<void(SchemaId)>& fn) const -> void override
    {
        for (auto schemaId : schemas_)
            fn(schemaId);
    }

private:
    /** Invalidate derived indexes when domain metadata changes. */
    void invalidate() override { Schema::invalidate(); state_ = State::Dirty; }

    auto collectNestedFields(const std::function<Schema*(SchemaId)>& lookup,
                             SchemaIdStack& visited,
                             std::vector<StringId>& fields) const -> void override
    {
        for (const auto& schemaId : schemas_)
            appendSchemaFields(schemaId, lookup, visited, fields);
    }

    auto collectNestedEnumSymbols(const std::function<Schema*(SchemaId)>& lookup,
                                  SchemaIdStack& visited,
                                  std::vector<StringId>& symbols) const -> void override
    {
        for (const auto& schemaId : schemas_)
            appendSchemaEnumSymbols(schemaId, lookup, visited, symbols);
    }

    sfl::small_vector<SchemaId, 1> schemas_;
    std::vector<StringId> flatFields_; // Ordered!
    std::vector<StringId> flatEnumSymbols_; // Ordered!
    std::uint64_t revision_ = 0;
    State state_ = State::Dirty;
};

/** Union, exclusive union or intersection of domains at the same value position. */
class CombinedSchema : public Schema
{
public:
    /** Keep the logical operator explicit; an empty intersection is unrestricted. */
    explicit CombinedSchema(Composition mode);

    /** Add an alternative by identity without copying its definition. */
    void addAlternative(SchemaId schema);
    /** Combine affinity summaries while preserving this domain's logical kind name. */
    auto kind() const -> Kind override;
    /** Report the explicit logical operator rather than a flattened approximation. */
    auto composition() const -> Composition override { return mode_; }
    /** Expose immutable alternative identities in declaration order. */
    auto alternatives() const & -> std::span<const SchemaId> override { return alternatives_; }
    /** Cached names reachable through any alternative. */
    auto nestedFields() const & -> std::span<const StringId> override { return fields_; }
    /** Cached string symbols reachable through any alternative. */
    auto nestedEnumSymbols() const & -> std::span<const StringId> override { return symbols_; }
    /** Members belong to alternatives, not to the combiner itself. */
    auto directFields() const & -> std::span<const StringId> override { return {}; }
    /** Indicate whether the current derived indexes are prepared. */
    auto finalized() const -> bool override { return state_ == State::Clean; }
    /** Invalidate environment-local plans after alternative or metadata edits. */
    auto revision() const -> std::uint64_t override { return metadataRevision_; }
    /** Prune only when every relevant domain supports the absence proof. */
    auto canHaveField(StringId field) const -> bool override;
    /** Test known symbol reachability, remaining conservative while dirty. */
    auto canHaveEnumSymbol(StringId symbol) const -> bool override;
    /** Derive reachability and coarse affinity without merging away alternatives. */
    auto finalize(const std::function<Schema*(SchemaId)>& lookup) -> State override;

private:
    /** Alternative mutations invalidate both affinity and reachability summaries. */
    void invalidate() override { Schema::invalidate(); state_ = State::Dirty; }
    /** Walk alternative edges to assemble the field index. */
    void collectNestedFields(const std::function<Schema*(SchemaId)>& lookup,
                             SchemaIdStack& visited, std::vector<StringId>& fields) const override;
    /** Walk alternative edges to assemble the symbol index. */
    void collectNestedEnumSymbols(const std::function<Schema*(SchemaId)>& lookup,
                                  SchemaIdStack& visited, std::vector<StringId>& symbols) const override;
    Composition mode_;
    std::vector<SchemaId> alternatives_;
    std::vector<StringId> fields_, symbols_;
    State state_ = State::Dirty;
    std::uint16_t affinities_ = AllAffinities;
};

}
