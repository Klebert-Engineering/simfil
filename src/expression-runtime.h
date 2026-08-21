// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#include "simfil/environment.h"
#include "simfil/expression.h"
#include "simfil/function.h"
#include "simfil/model/schema.h"

#include <memory>
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
        SchemaId schemaId,
        const Schema& schema,
        BuildFn&& build) -> const WildcardSchemaPlan*
    {
        if (schemaId == NoSchemaId || !schema.finalized())
            return nullptr;

        auto& plans = stateFor(expressionId).wildcardSchemaPlans;
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

    struct ExpressionState
    {
        StringId fieldId = StringPool::StaticStringIds::Empty;
        const Function* function = nullptr;
        std::vector<std::unique_ptr<CachedWildcardSchemaPlan>> wildcardSchemaPlans;
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
