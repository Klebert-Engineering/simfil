#include "simfil/model/schema.h"

#include <stdexcept>
#include <unordered_set>

namespace simfil
{

auto Schema::computeReachabilityComplete(const std::function<Schema*(SchemaId)>& lookup) const -> bool
{
    std::vector<const Schema*> pending{this};
    std::unordered_set<const Schema*> visited;
    while (!pending.empty()) {
        auto schema = pending.back();
        pending.pop_back();
        if (!schema || schema->open() || kindNameId(schema->kind()) == kindNameId(Kind::Unknown) || kindNameId(schema->kind()) == kindNameId(Kind::Any))
            return false;
        // An adapter's finalized-but-partial index is not an absence proof.
        // Builtin ancestors still being finalized are handled by graph traversal.
        if (schema != this && schema->finalized() && !schema->reachabilityComplete())
            return false;
        // Cycles have a finite reachable field set; only exact path enumeration is incomplete.
        if (!visited.insert(schema).second)
            continue;
        if (schema->composition() == Composition::AllOf && schema->alternatives().empty())
            return false; // Empty intersection is unrestricted, not an empty object.
        bool complete = true;
        auto append = [&](SchemaId id) {
            auto child = id == NoSchemaId ? nullptr : lookup(id);
            if (!child)
                complete = false;
            else
                pending.push_back(child);
        };
        schema->forEachDirectField([&](StringId, std::span<const SchemaId> children) {
            if (children.empty())
                complete = false;
            for (auto child : children)
                append(child);
        });
        bool hasElements = false;
        schema->forEachElementSchema([&](SchemaId id) { hasElements = true; append(id); });
        if (schema->hasAffinity(ValueType::Array) && schema->composition() == Composition::None && !hasElements)
            complete = false;
        for (auto id : schema->alternatives())
            append(id);
        if (!complete)
            return false;
    }
    return true;
}

auto Schema::fieldSchemas(SchemaId root, const Lookup& lookup, StringId field) -> std::vector<SchemaId>
{
    std::vector<SchemaId> result, pending{root}, visited;
    while (!pending.empty()) {
        auto id = pending.back();
        pending.pop_back();
        if (std::ranges::find(visited, id) != visited.end())
            continue;
        visited.push_back(id);
        auto schema = id == NoSchemaId ? nullptr : lookup(id);
        if (!schema) {
            result.push_back(NoSchemaId);
            continue;
        }
        if (!affinities(schema->kind()))
            continue;
        schema->forEachDirectField([&](StringId name, std::span<const SchemaId> children) {
            if (name != field)
                return;
            if (children.empty())
                result.push_back(NoSchemaId);
            result.insert(result.end(), children.begin(), children.end());
        });
        if (schema->open() || kindNameId(schema->kind()) == kindNameId(Kind::Unknown) || kindNameId(schema->kind()) == kindNameId(Kind::Any))
            result.push_back(NoSchemaId);
        if (schema->composition() == Composition::AllOf && schema->alternatives().empty())
            result.push_back(NoSchemaId);
        for (auto child : schema->alternatives())
            pending.push_back(child);
    }
    std::ranges::sort(result);
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

auto Schema::itemSchemas(SchemaId root, const Lookup& lookup) -> std::vector<SchemaId>
{
    std::vector<SchemaId> result, pending{root}, visited;
    while (!pending.empty()) {
        auto id = pending.back();
        pending.pop_back();
        if (std::ranges::find(visited, id) != visited.end())
            continue;
        visited.push_back(id);
        auto schema = id == NoSchemaId ? nullptr : lookup(id);
        if (!schema) {
            result.push_back(NoSchemaId);
            continue;
        }
        if (!affinities(schema->kind()))
            continue;
        bool found = false;
        schema->forEachElementSchema([&](SchemaId child) { found = true; result.push_back(child); });
        if (!found && schema->hasAffinity(ValueType::Array) && schema->composition() == Composition::None)
            result.push_back(NoSchemaId);
        if (schema->composition() == Composition::AllOf && schema->alternatives().empty())
            result.push_back(NoSchemaId);
        for (auto child : schema->alternatives())
            pending.push_back(child);
    }
    std::ranges::sort(result);
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

CombinedSchema::CombinedSchema(Composition mode) : mode_(mode)
{
    if (mode == Composition::None)
        throw std::invalid_argument("CombinedSchema requires a logical operator");
}

void CombinedSchema::addAlternative(SchemaId schema)
{
    alternatives_.push_back(schema);
    invalidate();
}

auto CombinedSchema::kind() const -> Kind
{
    auto name = mode_ == Composition::AllOf ? StringPool::SchemaIntersection
        : mode_ == Composition::OneOf ? StringPool::SchemaOneOf : StringPool::SchemaUnion;
    return domainKind(makeKind(name, state_ == State::Clean ? affinities_ : AllAffinities));
}

auto CombinedSchema::canHaveField(StringId field) const -> bool
{
    return !reachabilityComplete() || containsField(state_, fields_, field);
}

auto CombinedSchema::canHaveEnumSymbol(StringId symbol) const -> bool
{
    return containsField(state_, symbols_, symbol);
}

auto CombinedSchema::finalize(const std::function<Schema*(SchemaId)>& lookup) -> State
{
    if (state_ != State::Dirty)
        return state_;
    auto result = finalizeReachableMetadata(state_, fields_, symbols_, lookup, *this);
    affinities_ = mode_ == Composition::AllOf ? AllAffinities : 0;
    for (auto id : alternatives_) {
        auto child = id == NoSchemaId ? nullptr : lookup(id);
        auto bits = child && child != this ? affinities(child->kind()) : AllAffinities;
        if (mode_ == Composition::AllOf)
            affinities_ &= bits;
        else
            affinities_ |= bits;
    }
    return result;
}

void CombinedSchema::collectNestedFields(const std::function<Schema*(SchemaId)>& lookup,
                                        SchemaIdStack& visited, std::vector<StringId>& fields) const
{
    for (auto id : alternatives_)
        appendSchemaFields(id, lookup, visited, fields);
}

void CombinedSchema::collectNestedEnumSymbols(const std::function<Schema*(SchemaId)>& lookup,
                                             SchemaIdStack& visited, std::vector<StringId>& symbols) const
{
    for (auto id : alternatives_)
        appendSchemaEnumSymbols(id, lookup, visited, symbols);
}

}
