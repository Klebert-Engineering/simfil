#include "schema-pool-impl.h"

namespace simfil
{

auto SchemaType::directFields() const -> NameIds
{
    return finalized() ? pool_->schema_->queryNames.range(pool_->schema_->indexes[id_].directNames) : NameIds{};
}

auto SchemaType::nestedFields() const -> NameIds
{
    return finalized() ? pool_->schema_->queryNames.range(pool_->schema_->indexes[id_].recursiveNames) : NameIds{};
}

auto SchemaType::directEnumSymbols() const -> NameIds
{
    return *this ? pool_->schema_->symbols.range(pool_->schema_->types[id_].symbols) : NameIds{};
}

auto SchemaType::nestedEnumSymbols() const -> NameIds
{
    return finalized() ? pool_->schema_->queryNames.range(pool_->schema_->indexes[id_].recursiveSymbols) : NameIds{};
}

auto SchemaType::reachabilityComplete() const -> bool
{
    return finalized() && (pool_->schema_->indexes[id_].flags & 1);
}

auto SchemaType::canHaveField(StringId name) const -> bool
{
    if (!reachabilityComplete())
        return true;
    auto const& graph = *pool_->schema_;
    auto names = graph.indexes[id_].recursiveNames;
    if (graph.containsName(names, name))
        return true;
    auto aliases = graph.queryAliases.range(graph.allAliases);
    size_t first = 0, last = aliases.size();
    while (first < last)
    {
        auto middle = first + (last - first) / 2;
        if (aliases[middle].alias < name)
            first = middle + 1;
        else
            last = middle;
    }
    for (; first < aliases.size() && aliases[first].alias == name; ++first)
        if (graph.containsName(names, aliases[first].canonical))
            return true;
    return false;
}

auto SchemaType::canHaveEnumSymbol(StringId name) const -> bool
{
    // Construction has not prepared the reachable declaration index yet.
    return !finalized() || pool_->schema_->containsName(pool_->schema_->indexes[id_].recursiveSymbols, name);
}

auto SchemaType::canonicalField(StringId name) const -> StringId
{
    if (!*this)
        return name;
    auto parent = id_;
    pool_->schema_->declaredField(parent, name);
    return name;
}

auto SchemaType::fieldRole(StringId name) const -> FieldRole
{
    if (!*this)
        return FieldRole::Data;
    auto const& graph = *pool_->schema_;
    if (graph.memberIndexesReady && (composition() != Composition::None || !alternatives().empty()))
    {
        auto member = graph.combinedField(id_, name);
        return member && member->context ? FieldRole::Context : FieldRole::Data;
    }
    auto parent = id_;
    auto member = graph.declaredField(parent, name);
    return member && (member->flags & 2) ? FieldRole::Context : FieldRole::Data;
}

auto SchemaType::hasDirectEnumSymbol(std::string_view name) const -> bool
{
    if (!*this)
        return false;
    for (auto symbol : directEnumSymbols())
        if (pool_->dictionary().resolve(symbol) == name)
            return true;
    return false;
}

auto SchemaType::symbolEqualityPaths(StringId symbol) const -> std::vector<SchemaPath>
{
    return *this ? pool_->symbolEqualityPaths(id_, symbol) : std::vector<SchemaPath>{};
}

auto SchemaType::scalarFieldPathsForSymbol(StringId symbol) const -> std::vector<SchemaPath>
{
    return *this ? pool_->scalarFieldPathsForSymbol(id_, symbol) : std::vector<SchemaPath>{};
}

/** One bounded traversal of a canonical graph, with path-local rather than global cycle cuts. */
class SchemaType::PathSearch
{
public:
    enum class Match { Field, Enum, Named };

    /** Borrow the graph and match criterion for this synchronous search only. */
    PathSearch(SchemaType root, Match mode, StringId name = 0, std::string_view text = {})
        : root_(root.borrow()), mode_(mode), name_(name), text_(text) {}

    /** Return deterministic canonical paths and report whether finite expansion was exhaustive. */
    auto run(bool* complete) -> std::vector<SchemaPath>
    {
        visit(root_);
        std::ranges::sort(paths_);
        paths_.erase(std::unique(paths_.begin(), paths_.end()), paths_.end());
        if (complete)
            *complete = complete_;
        return std::move(paths_);
    }

private:
    /** Follow declarations, never a synthesized descriptor tree or runtime model. */
    void visit(SchemaType type)
    {
        if (!type || ++visits_ > 10000 || paths_.size() >= 10000)
        {
            complete_ = false;
            return;
        }
        if (!affinities(type.kind()))
            return;
        // An unrelated complete subgraph must not consume the recursion budget.
        if (mode_ == Match::Field && type.reachabilityComplete() && !type.canHaveField(name_))
            return;
        if (active_.size() >= 128 || std::ranges::find(active_, type.id()) != active_.end())
        {
            complete_ = false;
            return;
        }
        active_.push_back(type.id());
        if ((mode_ == Match::Named && type.namedValue(text_)) ||
            (mode_ == Match::Enum && std::ranges::count(type.directEnumSymbols(), name_) != 0))
            paths_.push_back(path_);

        auto canonical = type.canonicalField(name_);
        type.forEachField([&](SchemaField const& field) {
            path_.push_back({SchemaPathSegment::Kind::Field, field.nameId()});
            if (mode_ == Match::Field && field.nameId() == canonical && paths_.size() < 10000)
                paths_.push_back(path_);
            // A context terminal can itself match a field query, but enumeration
            // never enters its children or discovers constants through it.
            if (field.role() != FieldRole::Context)
            {
                auto children = field.types();
                if (children.empty())
                    complete_ = false;
                for (auto child : children)
                    visit(root_.pool()->schemaType(child));
            }
            path_.pop_back();
        });
        auto elements = type.elements();
        for (auto child : elements)
        {
            path_.push_back({SchemaPathSegment::Kind::ArrayElement, 0});
            visit(root_.pool()->schemaType(child));
            path_.pop_back();
        }
        for (auto child : type.alternatives())
            visit(root_.pool()->schemaType(child));
        auto name = kindNameId(type.kind());
        if (type.open() || name == StringPool::SchemaUnknown || name == StringPool::SchemaAny ||
            (type.hasAffinity(ValueType::Array) && elements.empty() && type.composition() == Composition::None) ||
            (type.composition() == Composition::AllOf && type.alternatives().empty()))
            complete_ = false;
        active_.pop_back();
    }

    SchemaType root_;
    Match mode_;
    StringId name_;
    std::string_view text_;
    std::vector<TypeId> active_;
    SchemaPath path_;
    std::vector<SchemaPath> paths_;
    size_t visits_ = 0;
    bool complete_ = true;
};

auto SchemaType::fieldPaths(StringId name, bool* complete) const -> std::vector<SchemaPath>
{
    return PathSearch(*this, PathSearch::Match::Field, name).run(complete);
}

auto SchemaType::enumSymbolPaths(StringId name, bool* complete) const -> std::vector<SchemaPath>
{
    return PathSearch(*this, PathSearch::Match::Enum, name).run(complete);
}

auto SchemaType::namedValuePaths(std::string_view name, bool* complete) const -> std::vector<SchemaPath>
{
    return PathSearch(*this, PathSearch::Match::Named, 0, name).run(complete);
}

auto SchemaType::symbolTypes(std::string_view name, std::span<TypeId const> excluded) const -> std::vector<TypeId>
{
    if (!*this)
        return {};
    std::vector<TypeId> pending{id_}, result;
    std::vector<bool> visited(pool_->typeCount() + 1);
    while (!pending.empty())
    {
        auto id = pending.back();
        pending.pop_back();
        if (!id || visited[id] || std::ranges::find(excluded, id) != excluded.end())
            continue;
        visited[id] = true;
        auto type = pool_->schemaType(id);
        auto declares = type.namedValue(name) || type.hasDirectEnumSymbol(name);
        if (declares)
            result.push_back(id);
        // A named enum's alternatives are physical representations, not new
        // semantic meanings. Still discover independently named child values.
        else
            for (auto child : type.alternatives())
                pending.push_back(child);
        type.forEachField([&](SchemaField const& field) {
            if (field.role() != FieldRole::Context)
                for (auto child : field.types())
                    pending.push_back(child);
        });
        for (auto child : type.elements())
            pending.push_back(child);
    }
    std::ranges::sort(result);
    return result;
}

auto SchemaType::firstScalarFieldPath() const -> std::optional<SchemaPath>
{
    if (!*this)
        return {};
    std::vector<TypeId> active;
    SchemaPath path;
    size_t visits = 0;
    auto visit = [&](auto&& self, SchemaType type) -> std::optional<SchemaPath>
    {
        if (!type || ++visits > 10000 || active.size() >= 128 ||
            std::ranges::find(active, type.id()) != active.end() || !affinities(type.kind()))
            return {};
        if (type.composition() == Composition::None && !type.hasAffinity(ValueType::Object) &&
            !type.hasAffinity(ValueType::Array))
            return path;
        active.push_back(type.id());
        std::optional<SchemaPath> result;
        for (auto alternative : type.alternatives())
            if ((result = self(self, pool_->schemaType(alternative))))
                break;
        if (!result && type.hasAffinity(ValueType::Object))
            type.forEachField([&](SchemaField const& field) {
                if (result || field.role() == FieldRole::Context)
                    return;
                path.push_back({SchemaPathSegment::Kind::Field, field.nameId()});
                if (field.types().empty())
                    result = path;
                else
                    for (auto child : field.types())
                        if ((result = self(self, pool_->schemaType(child))))
                            break;
                path.pop_back();
            });
        if (!result && !type.hasAffinity(ValueType::Object))
            for (auto child : type.elements())
            {
                path.push_back({SchemaPathSegment::Kind::ArrayElement, 0});
                result = self(self, pool_->schemaType(child));
                path.pop_back();
                if (result)
                    break;
            }
        active.pop_back();
        return result;
    };
    return visit(visit, borrow());
}

} // namespace simfil
