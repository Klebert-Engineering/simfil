// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#include "simfil/environment.h"
#include "simfil/expression.h"
#include "simfil/function.h"
#include "simfil/model/schema-pool.h"

#include <memory>
#include <map>
#include <string_view>
#include <vector>

namespace simfil::detail
{

/**
 * Environment-dependent state retained by one expression binding.
 *
 * Keeping these caches outside the AST makes a SharedAST immutable and safe to
 * evaluate concurrently through independent BoundExpression instances.
 */
class ExpressionRuntime
{
public:
    struct WildcardSchemaPlan
    {
        enum class Kind {
            Unknown,
            Object,
            Array,
        };

        Kind kind = Kind::Unknown;
        bool canHaveField = true;
        bool directField = true;
        std::vector<StringId> objectChildFields;
    };

    /** Bind runtime lookups to one environment. */
    explicit ExpressionRuntime(Environment& environment) : environment_(environment) {}

    /** Resolve a field against this binding's string pool, caching successful lookups. */
    auto fieldId(Expr::ExprId expressionId, std::string_view name) -> StringId
    {
        auto& state = stateFor(expressionId);
        if (state.fieldId == StringPool::StaticStringIds::Empty)
            state.fieldId = environment_.strings()->get(name);
        return state.fieldId;
    }

    /** Translate a static name once per graph, including the basic pruning path. */
    auto schemaFieldId(Expr::ExprId expressionId, std::string_view name, SchemaType const& type) -> StringId
    {
        auto& state = stateFor(expressionId).forSchema(type);
        if (!state.fieldResolved)
        {
            state.fieldId = type.pool()->dictionary().get(name);
            state.fieldResolved = true;
        }
        return state.fieldId;
    }

    /** Extract a static member with a cached namespace translation, preserving scalar provenance. */
    auto fieldValue(Expr::ExprId expressionId, std::string_view name,
                    ModelNode const& parent, ModelNode const& child) -> Value
    {
        return Value::field(child, [&]() -> SchemaType {
            auto type = parent.schemaType();
            if (!type)
                return {};
            auto field = schemaFieldId(expressionId, name, type);
            if (field == StringPool::Empty && !name.empty())
                return {};
            return type.fieldType(field);
        });
    }

    /** Resolve a function against this binding's environment, caching successful lookups. */
    auto function(Expr::ExprId expressionId, const std::string& name) -> const Function*
    {
        auto& state = stateFor(expressionId);
        if (!state.function)
            state.function = environment_.findFunction(name);
        return state.function;
    }

    /** Cache one immutable pool/type plan; temporary view addresses are never identity keys. */
    template <typename BuildFn>
    auto wildcardSchemaPlan(Expr::ExprId expressionId, SchemaType const& schema,
                            BuildFn&& build) -> const WildcardSchemaPlan*
    {
        if (!schema.finalized())
            return nullptr;
        auto& plans = stateFor(expressionId).forSchema(schema).plans;
        auto index = size_t(schema.id());
        if (plans.size() <= index)
            plans.resize(index + 1);
        if (!plans[index])
            plans[index] = std::make_unique<WildcardSchemaPlan>(std::forward<BuildFn>(build)());
        return plans[index].get();
    }

private:
    /** One retained graph namespace; per-type plans have stable addresses during recursive traversal. */
    struct BoundSchemaState
    {
        SchemaType owner;
        StringId fieldId = StringPool::Empty;
        bool fieldResolved = false;
        std::vector<std::unique_ptr<WildcardSchemaPlan>> plans;
    };

    /** One movable, noncopyable cache slot; its map may have an allocating move on some libraries. */
    struct ExpressionState
    {
        /** Start an empty environment-local slot. */
        ExpressionState() = default;

        /** Transfer graph-owned entries when the expression slot vector grows. */
        ExpressionState(ExpressionState&&) = default;

        /** Transfer cache ownership when replacing a slot. */
        auto operator=(ExpressionState&&) -> ExpressionState& = default;
        // Explicit deletion prevents vector relocation from selecting a map copy
        // whose nested unique_ptr entries cannot be copied (notably with MSVC).
        ExpressionState(ExpressionState const&) = delete;
        auto operator=(ExpressionState const&) -> ExpressionState& = delete;

        StringId fieldId = StringPool::StaticStringIds::Empty;
        const Function* function = nullptr;
        std::map<SchemaPool const*, BoundSchemaState> boundSchemas;

        /** Fast-path the usual single graph without incrementing its ownership counter. */
        auto forSchema(SchemaType const& type) -> BoundSchemaState&
        {
            if (lastPool != type.pool())
            {
                auto [entry, inserted] = boundSchemas.try_emplace(type.pool());
                if (inserted)
                    entry->second.owner = type.retain();
                lastSchemaState = &entry->second;
                lastPool = type.pool();
            }
            return *lastSchemaState;
        }

    private:
        SchemaPool const* lastPool = nullptr;
        BoundSchemaState* lastSchemaState = nullptr;
    };

    /** Materialize the cache slot assigned by AST::reenumerate(). */
    auto stateFor(Expr::ExprId expressionId) -> ExpressionState&
    {
        const auto index = static_cast<std::size_t>(expressionId);
        if (states_.size() <= index)
            states_.resize(index + 1);
        return states_[index];
    }

    Environment& environment_;
    std::vector<ExpressionState> states_;
};

}  // namespace simfil::detail
