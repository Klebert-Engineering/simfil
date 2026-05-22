#pragma once

#include "simfil/model/string-pool.h"
#include <algorithm>
#include <cassert>
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
    using SchemaIdStack = sfl::small_vector<SchemaId, 8>;

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
