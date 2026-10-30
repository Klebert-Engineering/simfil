#include "simfil/model/schema.h"

#include <stdexcept>
#include <unordered_set>

namespace simfil
{

SchemaBinding::SchemaBinding(std::string identity, std::shared_ptr<const void> graphOwner,
                             Schema::Lookup lookup, std::shared_ptr<const StringPool> strings)
    : identity_(std::move(identity)), owner_(std::move(graphOwner)),
      lookup_(std::move(lookup)), strings_(std::move(strings))
{
    if (identity_.empty() || identity_.size() > 65535 || !owner_ || !lookup_ || !strings_)
        throw std::invalid_argument("SchemaBinding requires an identity, graph owner, lookup and dictionary");
}

auto SchemaBinding::lookup(SchemaId id) const -> const Schema*
{
    return id == NoSchemaId ? nullptr : lookup_(id);
}

auto SchemaReference::definition() const -> const Schema*
{
    return binding ? binding->lookup(id) : nullptr;
}

auto SchemaReference::operator==(SchemaReference const& other) const noexcept -> bool
{
    if (id != other.id)
        return false;
    if (binding == other.binding)
        return true;
    return binding && other.binding && binding->identity() == other.binding->identity();
}

auto Schema::namedValue(std::string_view name) const -> std::optional<NamedValue>
{
    std::optional<NamedValue> result;
    forEachNamedValue([&](NamedValue const& value) {
        if (value.name == name)
            result = value;
    });
    return result;
}

auto Schema::namedValuePaths(SchemaId root, const Lookup& lookup, std::string_view name, bool* complete)
    -> std::vector<SchemaPath>
{
    std::vector<SchemaPath> result;
    std::vector<SchemaId> active;
    SchemaPath path;
    std::size_t visits = 0;
    bool exhaustive = true;
    std::function<void(SchemaId)> visit = [&](SchemaId id) {
        auto schema = id ? lookup(id) : nullptr;
        if (!schema || ++visits > 10000 || active.size() >= 128 || result.size() >= 10000 ||
            std::ranges::find(active, id) != active.end()) {
            exhaustive = false;
            return;
        }
        if (!affinities(schema->kind()))
            return;
        active.push_back(id);
        if (schema->namedValue(name) && std::ranges::find(result, path) == result.end())
            result.push_back(path);
        schema->forEachDirectField([&](StringId field, std::span<const SchemaId> children) {
            path.push_back({SchemaPathSegment::Kind::Field, field});
            if (children.empty())
                exhaustive = false;
            for (auto child : children)
                visit(child);
            path.pop_back();
        });
        bool elements = false;
        schema->forEachElementSchema([&](SchemaId child) {
            elements = true;
            path.push_back({SchemaPathSegment::Kind::ArrayElement, 0});
            visit(child);
            path.pop_back();
        });
        for (auto child : schema->alternatives())
            visit(child);
        if (schema->open() || kindNameId(schema->kind()) == kindNameId(Kind::Unknown) ||
            kindNameId(schema->kind()) == kindNameId(Kind::Any) ||
            (schema->hasAffinity(ValueType::Array) && !elements && schema->composition() == Composition::None))
            exhaustive = false;
        active.pop_back();
    };
    visit(root);
    if (complete)
        *complete = exhaustive;
    return result;
}

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
        auto canonical = schema->canonicalField(field);
        bool declared = false;
        schema->forEachDirectField(
            [&](StringId name, std::span<const SchemaId> children) {
                if (name != canonical)
                    return;
                declared = true;
                if (children.empty())
                    result.push_back(NoSchemaId);
                result.insert(result.end(), children.begin(), children.end());
            });
        // Openness admits undeclared fields; it does not weaken an explicit field's domain.
        // Each alternative is checked independently, so an undeclared open branch stays unknown.
        if ((!declared && schema->open()) || kindNameId(schema->kind()) == kindNameId(Kind::Unknown) || kindNameId(schema->kind()) == kindNameId(Kind::Any))
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
