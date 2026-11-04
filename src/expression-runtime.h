// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#include "simfil/environment.h"
#include "simfil/expression.h"
#include "simfil/function.h"
#include "simfil/model/schema.h"

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

    /** Translate this expression's field once per graph, also for basic pruning without plans. */
    auto schemaFieldId(Expr::ExprId expressionId, std::string_view name,
                       std::shared_ptr<const SchemaBinding> const& binding) -> StringId
    {
        auto& state = stateFor(expressionId).forBinding(binding);
        if (state.fieldId == StringPool::Empty)
            state.fieldId = binding->strings()->get(name);
        return state.fieldId;
    }

    /** Extract a static member with the already-bound query name, not a per-value dictionary roundtrip. */
    auto fieldValue(Expr::ExprId expressionId, std::string_view name,
                    ModelNode const& parent, ModelNode const& child) -> Value
    {
        return Value::field(child, [&]() -> SchemaReference {
            auto typeRef = parent.schemaReference();
            auto schema = typeRef.definition();
            if (!schema)
                return {};
            auto field = schemaFieldId(expressionId, name, typeRef.binding);
            if (field == StringPool::Empty && !name.empty())
                return {};
            return {std::move(typeRef.binding), schema->fieldSchema(field)};
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

    /**
     * Return a schema traversal plan, rebuilding it after schema replacement or mutation.
     */
    template <typename BuildFn>
    auto wildcardSchemaPlan(
        Expr::ExprId expressionId,
        TypeId schemaId,
        const Schema& schema,
        BuildFn&& build, std::shared_ptr<const SchemaBinding> const& binding = {}) -> const WildcardSchemaPlan*
    {
        if (schemaId == NoTypeId || !schema.finalized())
            return nullptr;

        auto& state = stateFor(expressionId);
        // Keep types from different immutable graphs distinct even when their local ids coincide.
        auto& plans = binding ? state.forBinding(binding).plans : state.wildcardSchemaPlans;
        const auto planIndex = static_cast<std::size_t>(schemaId);
        const auto schemaRevision = schema.revision();
        if (planIndex < plans.size()) {
            const auto& cachedPlan = plans[planIndex];
            if (cachedPlan && cachedPlan->schema == &schema &&
                cachedPlan->schemaRevision == schemaRevision)
                return &cachedPlan->plan;
        }

        if (plans.size() <= planIndex)
            plans.resize(planIndex + 1);
        plans[planIndex] = std::make_unique<CachedWildcardSchemaPlan>(
            schema,
            schemaRevision,
            std::forward<BuildFn>(build)());
        return &plans[planIndex]->plan;
    }

private:
    struct CachedWildcardSchemaPlan
    {
        /** Retain one plan together with the schema state that produced it. */
        CachedWildcardSchemaPlan(
            const Schema& sourceSchema,
            std::uint64_t sourceRevision,
            WildcardSchemaPlan sourcePlan)
            : schema(&sourceSchema)
            , schemaRevision(sourceRevision)
            , plan(std::move(sourcePlan))
        {
        }

        const Schema* schema = nullptr;
        std::uint64_t schemaRevision = 0;
        WildcardSchemaPlan plan;
    };

    /** A retained graph namespace shared by this expression's translation and traversal plans. */
    struct BoundSchemaState
    {
        StringId fieldId = StringPool::Empty;
        std::vector<std::unique_ptr<CachedWildcardSchemaPlan>> plans;
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
        std::vector<std::unique_ptr<CachedWildcardSchemaPlan>> wildcardSchemaPlans;
        std::map<std::shared_ptr<const SchemaBinding>, BoundSchemaState> boundSchemas;

        /** Repeated nodes normally share one graph; keep other graphs in the owning map. */
        auto forBinding(std::shared_ptr<const SchemaBinding> const& binding) -> BoundSchemaState&
        {
            if (lastBinding != binding.get()) {
                lastSchemaState = &boundSchemas[binding];
                lastBinding = binding.get();
            }
            return *lastSchemaState;
        }

    private:
        // Map entries pin the graph and remain stable when expression slots move.
        SchemaBinding const* lastBinding = nullptr;
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
