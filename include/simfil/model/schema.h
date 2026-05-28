#pragma once

#include "simfil/model/string-pool.h"
#include <algorithm>
#include <cassert>
#include <compare>
#include <concepts>
#include <cstdint>
#include <functional>
#include <limits>
#include <ranges>
#include <sfl/small_vector.hpp>
#include <span>
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

/**
 *
 */
class Schema
{
public:
    /** Schema kind */
    enum class Kind {
        Object,
        Array,
        Value,
    };

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
    virtual auto kind() const -> Kind = 0;

    /**
     * Returns true if this schema or any of the schemas it refers to
     * can possibly contain the given field.
     */
    virtual auto canHaveField(StringId fieldId) const -> bool = 0;

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
    virtual auto nestedFields() const & -> std::span<const StringId> = 0;

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
     * to derive precise schema paths for auto-wildcard rewrites.
     */
    virtual auto directEnumSymbols() const & -> std::span<const StringId>
    {
        return {};
    }

    /**
     * Enumerate precise paths to all fields with the requested name.
     */
    static auto fieldPaths(SchemaId root,
                           const std::function<const Schema*(SchemaId)>& queryFn,
                           StringId field) -> std::vector<SchemaPath>
    {
        std::vector<SchemaPath> paths;
        SchemaIdStack visited;
        SchemaPath current;
        collectFieldPaths(root, queryFn, field, visited, current, paths);
        sortUniquePaths(paths);
        return paths;
    }

    /**
     * Enumerate precise paths to all values that can hold the enum-like symbol.
     */
    static auto enumSymbolPaths(SchemaId root,
                                const std::function<const Schema*(SchemaId)>& queryFn,
                                StringId symbol) -> std::vector<SchemaPath>
    {
        std::vector<SchemaPath> paths;
        SchemaIdStack visited;
        SchemaPath current;
        collectEnumSymbolPaths(root, queryFn, symbol, visited, current, paths);
        sortUniquePaths(paths);
        return paths;
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
        return 0;
    }

protected:
    /**
     * Append all fields reachable from this schema without relying on cached
     * finalization state. This lets cyclic schema graphs still produce an exact
     * field set by cutting recursion at already visited schema ids.
     */
    virtual auto collectNestedFields(const std::function<Schema*(SchemaId)>& queryFn,
                                     SchemaIdStack& visited,
                                     std::vector<StringId>& fields) const -> void = 0;

    /**
     * Append all enum-like string symbols reachable from this schema without
     * relying on cached finalization state.
     */
    virtual auto collectNestedEnumSymbols(const std::function<Schema*(SchemaId)>& queryFn,
                                          SchemaIdStack& visited,
                                          std::vector<StringId>& symbols) const -> void
    {
    }

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

    /**
     * Recursively collect schema paths to matching fields.
     */
    static auto collectFieldPaths(SchemaId schemaId,
                                  const std::function<const Schema*(SchemaId)>& queryFn,
                                  StringId field,
                                  SchemaIdStack& visited,
                                  SchemaPath& current,
                                  std::vector<SchemaPath>& paths) -> void
    {
        if (schemaId == NoSchemaId || std::ranges::find(visited, schemaId) != visited.end())
            return;

        auto const* schema = queryFn(schemaId);
        if (!schema)
            return;

        visited.push_back(schemaId);

        schema->forEachDirectField([&](StringId directField, std::span<const SchemaId> childSchemas) {
            current.push_back({SchemaPathSegment::Kind::Field, directField});
            if (directField == field)
                paths.push_back(current);
            for (auto childSchemaId : childSchemas)
                collectFieldPaths(childSchemaId, queryFn, field, visited, current, paths);
            current.pop_back();
        });

        schema->forEachElementSchema([&](SchemaId elementSchemaId) {
            current.push_back({SchemaPathSegment::Kind::ArrayElement, 0});
            collectFieldPaths(elementSchemaId, queryFn, field, visited, current, paths);
            current.pop_back();
        });

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
                                       std::vector<SchemaPath>& paths) -> void
    {
        if (schemaId == NoSchemaId || std::ranges::find(visited, schemaId) != visited.end())
            return;

        auto const* schema = queryFn(schemaId);
        if (!schema)
            return;

        visited.push_back(schemaId);

        for (auto directSymbol : schema->directEnumSymbols()) {
            if (directSymbol == symbol)
                paths.push_back(current);
        }

        schema->forEachDirectField([&](StringId directField, std::span<const SchemaId> childSchemas) {
            current.push_back({SchemaPathSegment::Kind::Field, directField});
            for (auto childSchemaId : childSchemas)
                collectEnumSymbolPaths(childSchemaId, queryFn, symbol, visited, current, paths);
            current.pop_back();
        });

        schema->forEachElementSchema([&](SchemaId elementSchemaId) {
            current.push_back({SchemaPathSegment::Kind::ArrayElement, 0});
            collectEnumSymbolPaths(elementSchemaId, queryFn, symbol, visited, current, paths);
            current.pop_back();
        });

        visited.pop_back();
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

        if (schema->finalize(queryFn) == State::Clean) {
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

        auto operator<=>(const FieldSummary& other) const
        {
            return field <=> other.field;
        }
    };

    auto kind() const -> Kind override
    {
        return Kind::Object;
    }

    auto canHaveField(StringId field) const -> bool override
    {
        return containsField(state_, flatFields_, field);
    }

    auto canHaveEnumSymbol(StringId symbol) const -> bool override
    {
        return containsField(state_, flatEnumSymbols_, symbol);
    }

    /**
     * Add a direct field and optional child schemas reachable through it.
     */
    auto addField(StringId field, std::initializer_list<SchemaId> schemas = {}) -> void
    {
        FieldSummary summary;
        summary.field = field;
        summary.schemas.insert(summary.schemas.end(), schemas.begin(), schemas.end());
        fields_.push_back(std::move(summary));
        directFields_.push_back(field);
        state_ = State::Dirty;
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
        return revision_;
    }

private:
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
    auto kind() const -> Kind override
    {
        return Kind::Value;
    }

    auto canHaveField(StringId) const -> bool override
    {
        return false;
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
        return revision_;
    }

private:
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
        return Kind::Array;
    }

    auto canHaveField(StringId field) const -> bool override
    {
        return containsField(state_, flatFields_, field);
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
        return revision_;
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

}
