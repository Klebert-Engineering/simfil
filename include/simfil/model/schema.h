#pragma once

#include "simfil/model/string-pool.h"
#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstdint>
#include <limits>
#include <sfl/small_vector.hpp>
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
     *
     * @param fieldId The field id to query the schema for
     */
    virtual auto canHaveField(StringId fieldId) const -> bool = 0;

    /**
     * Finalize this schema and all schemas it refers to.
     *
     * @param queryFn Schema Query callback.
     */
    virtual auto finalize(const std::function<Schema*(SchemaId)>& queryFn) -> State
    {
        return State::Clean;
    }

    /**
     * @return All nested field names.
     */
    virtual auto nestedFields() const & -> std::span<const StringId> = 0;
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
        // Be conservative if the schema has not been finalized.
        if (state_ != State::Clean)
            return true;

        auto iter = std::lower_bound(flatFields_.begin(), flatFields_.end(), field);
        return iter != flatFields_.end() && *iter == field;
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
        if (state_ == State::Clean || state_ == State::Finalizing)
            return state_;

        state_ = State::Finalizing;
        flatFields_.clear();
        auto canFinalize = true;

        for (const auto& field : fields_) {
            flatFields_.push_back(field.field);
            for (const auto& fieldSchemaId : field.schemas) {
                if (auto* childSchema = lookup(fieldSchemaId)) {
                    auto childState = childSchema->finalize(lookup);
                    if (childState != State::Clean) {
                        canFinalize = false;
                        continue;
                    }

                    auto childFields = childSchema->nestedFields();
                    flatFields_.insert(flatFields_.end(), childFields.begin(), childFields.end());
                }
            }
        }

        if (!canFinalize) {
            flatFields_.clear();
            state_ = State::Dirty;
            return State::Dirty;
        }

        std::sort(flatFields_.begin(), flatFields_.end());
        flatFields_.erase(std::unique(flatFields_.begin(), flatFields_.end()), flatFields_.end());
        state_ = State::Clean;
        return State::Clean;
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
        if (state_ != State::Clean)
            return true;

        auto iter = std::lower_bound(flatFields_.begin(), flatFields_.end(), field);
        return iter != flatFields_.end() && *iter == field;
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
        if (state_ == State::Clean || state_ == State::Finalizing)
            return state_;

        state_ = State::Finalizing;
        flatFields_.clear();
        auto canFinalize = true;

        for (const auto& schemaId : schemas_) {
            if (auto* childSchema = lookup(schemaId)) {
                auto childState = childSchema->finalize(lookup);
                if (childState != State::Clean) {
                    canFinalize = false;
                    continue;
                }

                auto childFields = childSchema->nestedFields();
                flatFields_.insert(flatFields_.end(), childFields.begin(), childFields.end());
            }
        }

        if (!canFinalize) {
            flatFields_.clear();
            state_ = State::Dirty;
            return State::Dirty;
        }

        std::sort(flatFields_.begin(), flatFields_.end());
        flatFields_.erase(std::unique(flatFields_.begin(), flatFields_.end()), flatFields_.end());
        state_ = State::Clean;
        return State::Clean;
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
    sfl::small_vector<SchemaId, 1> schemas_;
    std::vector<StringId> flatFields_; // Ordered!
    State state_ = State::Dirty;
};

}
