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
     * Append fields reachable through a schema id, using a finalized child
     * cache when possible and falling back to raw graph traversal for cycles.
     */
    static auto appendSchemaFields(SchemaId schemaId,
                                   const std::function<Schema*(SchemaId)>& queryFn,
                                   SchemaIdStack& visited,
                                   std::vector<StringId>& fields) -> void
    {
        if (schemaId == NoSchemaId || std::ranges::find(visited, schemaId) != visited.end())
            return;

        auto* schema = queryFn(schemaId);
        if (!schema)
            return;

        visited.push_back(schemaId);

        if (schema->finalize(queryFn) == State::Clean) {
            auto childFields = schema->nestedFields();
            fields.insert(fields.end(), childFields.begin(), childFields.end());
            return;
        }

        schema->collectNestedFields(queryFn, visited, fields);
    }

    /**
     * Shared finalization implementation for concrete schema classes.
     */
    template <class CollectFn>
    static auto finalizeFields(State& state,
                               std::vector<StringId>& flatFields,
                               const std::function<Schema*(SchemaId)>& queryFn,
                               CollectFn&& collect) -> State
    {
        if (state == State::Clean || state == State::Finalizing)
            return state;

        state = State::Finalizing;
        flatFields.clear();

        SchemaIdStack visited;
        collect(queryFn, visited, flatFields);

        std::ranges::sort(flatFields);
        auto duplicates = std::ranges::unique(flatFields);
        flatFields.erase(duplicates.begin(), duplicates.end());

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

    /**
     * Add a direct field and optional child schemas reachable through it.
     */
    auto addField(StringId field, std::initializer_list<SchemaId> schemas = {}) -> void
    {
        FieldSummary summary;
        summary.field = field;
        summary.schemas.insert(summary.schemas.end(), schemas.begin(), schemas.end());
        fields_.push_back(std::move(summary));
        state_ = State::Dirty;
    }

    /**
     * Recompute the cached descendant field set from this schema and all
     * reachable child schemas.
     */
    auto finalize(const std::function<Schema*(SchemaId)>& lookup) -> State override
    {
        return finalizeFields(state_, flatFields_, lookup, [this](const auto& queryFn,
                                                                  auto& visited,
                                                                  auto& fields) {
            collectNestedFields(queryFn, visited, fields);
        });
    }

    auto fields() const & -> std::span<const FieldSummary>
    {
        return {fields_.begin(), fields_.end()};
    }

    auto nestedFields() const & -> std::span<const StringId> override
    {
        return {flatFields_.cbegin(), flatFields_.cend()};
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

    sfl::small_vector<FieldSummary, 4> fields_;

    std::vector<StringId> flatFields_; // Ordered!
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

    /**
     * Add possible schemas for elements contained in the array.
     */
    auto addElementSchemas(std::initializer_list<SchemaId> schemas) -> void
    {
        schemas_.insert(schemas_.end(), schemas.begin(), schemas.end());
        state_ = State::Dirty;
    }

    /**
     * Recompute the cached descendant field set from all possible element
     * schemas.
     */
    auto finalize(const std::function<Schema*(SchemaId)>& lookup) -> State override
    {
        return finalizeFields(state_, flatFields_, lookup, [this](const auto& queryFn,
                                                                  auto& visited,
                                                                  auto& fields) {
            collectNestedFields(queryFn, visited, fields);
        });
    }

    auto nestedFields() const & -> std::span<const StringId> override
    {
        return {flatFields_.cbegin(), flatFields_.cend()};
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

    sfl::small_vector<SchemaId, 1> schemas_;
    std::vector<StringId> flatFields_; // Ordered!
    State state_ = State::Dirty;
};

}
