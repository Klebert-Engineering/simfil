#include "schema-pool-impl.h"

namespace simfil
{

void SchemaPool::Impl::index()
{
    indexFieldOrder();
    indexes.resize(types.size());
    auto& work = indexWork;
    // Deduplication is construction-local. Published sets are immutable arena arrays.
    std::map<std::vector<StringId>, ArrayIndex> nameSets;
    auto nameSet = [&](std::vector<StringId> names, bool sort) -> ArrayIndex
    {
        if (sort)
        {
            std::ranges::sort(names);
            names.erase(std::unique(names.begin(), names.end()), names.end());
        }
        auto [entry, inserted] = nameSets.try_emplace(names, 0);
        if (inserted)
            entry->second = indexArray(queryNames, names);
        return entry->second;
    };
    std::vector<std::vector<TypeId>> children(types.size()), parents(types.size());
    std::vector<bool> localComplete(types.size(), true);

    // Build data-reachability edges and record local unknowns. Completeness is
    // propagated to parents later so a negative name lookup is used only when safe.
    for (size_t id = 1; id < types.size(); ++id)
    {
        auto const& row = types[id];
        auto type = pool.schemaType(TypeId(id));
        std::vector<StringId> direct;
        auto addChild = [&](TypeId child)
        {
            chargeIndexWork();
            if (child)
                children[id].push_back(child);
            else
                localComplete[id] = false;
        };
        for (auto record : effectiveFields.range(effectiveFieldOrders[id]))
        {
            chargeIndexWork();
            auto const& field = fieldRows[record];
            direct.push_back(field.name);
            // Context names are addressable terminals, not enumerable subtrees.
            if (field.flags & 2)
                continue;
            auto targets = edges.range(field.children);
            if (targets.empty())
                localComplete[id] = false;
            for (auto child : targets)
                addChild(child);
        }
        indexes[id].directNames = nameSet(std::move(direct), false);
        for (auto child : edges.range(row.elements))
            addChild(child);
        for (auto child : edges.range(row.alternatives))
            addChild(child);
        auto name = SchemaType::kindNameId(type.kind());
        if (type.open() || name == StringPool::SchemaUnknown || name == StringPool::SchemaAny ||
            (type.hasAffinity(ValueType::Array) && type.composition() == SchemaType::Composition::None && !row.elements) ||
            (type.composition() == SchemaType::Composition::AllOf && !row.alternatives))
            localComplete[id] = false;
        auto& next = children[id];
        std::ranges::sort(next);
        next.erase(std::unique(next.begin(), next.end()), next.end());
        for (auto child : next)
            parents[child].push_back(TypeId(id));
    }
    // Group mutually reachable types into strongly connected components. Within
    // a recursive cycle every type sees the same descendant fields and symbols.
    // The first DFS records finishing order; the reverse-edge pass forms groups.
    std::vector<bool> seen(types.size());
    std::vector<TypeId> order;
    for (size_t root = 1; root < types.size(); ++root)
    {
        if (seen[root])
            continue;
        std::vector<std::pair<TypeId, size_t>> stack{{TypeId(root), 0}};
        seen[root] = true;
        while (!stack.empty())
        {
            auto& [id, position] = stack.back();
            if (position < children[id].size())
            {
                auto child = children[id][position++];
                if (!seen[child])
                {
                    seen[child] = true;
                    stack.emplace_back(child, 0);
                }
            }
            else
            {
                order.push_back(id);
                stack.pop_back();
            }
        }
    }

    std::vector<uint32_t> component(types.size(), UINT32_MAX);
    std::vector<std::vector<TypeId>> members;
    for (auto it = order.rbegin(); it != order.rend(); ++it)
    {
        if (component[*it] != UINT32_MAX)
            continue;
        auto c = uint32_t(members.size());
        members.emplace_back();
        std::vector<TypeId> pending{*it};
        component[*it] = c;
        while (!pending.empty())
        {
            auto id = pending.back();
            pending.pop_back();
            members.back().push_back(id);
            for (auto parent : parents[id])
                if (component[parent] == UINT32_MAX)
                {
                    component[parent] = c;
                    pending.push_back(parent);
                }
        }
    }

    // Process child components before parents and share the resulting sorted
    // name sets across every type in a component. Runtime queries need no DFS.
    for (size_t c = members.size(); c-- > 0;)
    {
        std::vector<StringId> allFields, allSymbols;
        bool complete = true;
        std::set<uint32_t> downstream;
        for (auto id : members[c])
        {
            auto fields = queryNames.range(indexes[id].directNames);
            auto symbols = this->symbols.range(types[id].symbols);
            work += fields.size() + symbols.size() + children[id].size();
            if (work > 64 * 1024 * 1024)
                throw std::length_error("Schema reachability work budget exceeded");
            allFields.insert(allFields.end(), fields.begin(), fields.end());
            allSymbols.insert(allSymbols.end(), symbols.begin(), symbols.end());
            complete = complete && localComplete[id];
            for (auto child : children[id])
                if (component[child] != c)
                    downstream.insert(component[child]);
        }
        for (auto child : downstream)
        {
            auto const& index = indexes[members[child].front()];
            auto fields = queryNames.range(index.recursiveNames);
            auto symbols = queryNames.range(index.recursiveSymbols);
            work += fields.size() + symbols.size();
            if (work > 64 * 1024 * 1024)
                throw std::length_error("Schema reachability work budget exceeded");
            allFields.insert(allFields.end(), fields.begin(), fields.end());
            allSymbols.insert(allSymbols.end(), symbols.begin(), symbols.end());
            complete = complete && bool(index.flags & 1);
        }
        auto fields = nameSet(std::move(allFields), true),
             symbols = nameSet(std::move(allSymbols), true);
        for (auto id : members[c])
        {
            indexes[id].recursiveNames = fields;
            indexes[id].recursiveSymbols = symbols;
            indexes[id].flags |= uint32_t(complete);
        }
    }

    // Publish the remaining point-lookups together, then cache storage statistics
    // so diagnostics need not walk the graph on every request.
    indexMembers();
    indexAliases();
    memberIndexesReady = true;
    statistics.types = types.size() - 1;

    statistics.numericDomains = numerics.size();
    for (auto const& row : types)
    {
        statistics.fields += fields.size(row.fields);
        statistics.namedValues += named.size(row.named);
        for (auto const& field : fieldRecords(row.fields))
        {
            statistics.fieldEdges += edges.size(field.children);
            statistics.contextFields += bool(field.flags & 2);
        }
        statistics.elementEdges += edges.size(row.elements);
        statistics.alternativeEdges += edges.size(row.alternatives);
    }
}


void SchemaPool::Impl::indexMembers()
{
    unknownTypes = indexArray(queryTypes, std::array<TypeId, 1>{NoTypeId});
    std::map<std::vector<uint32_t>, ArrayIndex> lookupSets;
    std::vector<bool> active(types.size());
    for (size_t id = 1; id < types.size(); ++id)
    {
        auto& index = indexes[id];
        auto records = effectiveFields.range(effectiveFieldOrders[id]);
        std::vector<uint32_t> sorted(records.begin(), records.end());
        std::ranges::sort(sorted, {}, [&](uint32_t record) { return fieldRows[record].name; });
        auto [entry, inserted] = lookupSets.try_emplace(sorted, 0);
        if (inserted)
            entry->second = indexArray(fieldLookup, sorted);
        index.fieldLookup = entry->second;
        for (auto base = id; base; base = types[base].base)
            if (types[base].aliases)
                index.flags |= 4;

        auto root = pool.schemaType(TypeId(id));
        if (root.composition() == SchemaType::Composition::None && root.alternatives().empty())
            continue;

        // Only logical alternatives share this value position. A data edge is
        // never traversed while preparing a member's type. Active-path cuts
        // terminate cycles without erasing repeated OneOf alternatives.
        std::vector<TypeId> branches;
        std::vector<std::pair<TypeId, bool>> pending{{TypeId(id), false}};
        while (!pending.empty())
        {
            auto [current, leaving] = pending.back();
            pending.pop_back();
            chargeIndexWork();
            if (leaving)
            {
                active[current] = false;
                continue;
            }
            if (!current)
            {
                branches.push_back(NoTypeId);
                continue;
            }
            if (active[current])
                continue;
            auto type = pool.schemaType(current);
            if (!SchemaType::affinities(type.kind()))
                continue;
            branches.push_back(current);
            active[current] = true;
            pending.emplace_back(current, true);
            auto alternatives = edges.range(types[current].alternatives);
            for (size_t i = alternatives.size(); i-- > 0;)
                pending.emplace_back(alternatives[i], false);
        }

        std::set<StringId> names;
        std::vector<TypeId> items;
        for (auto branch : branches)
        {
            if (!branch)
            {
                index.flags |= 2;
                items.push_back(NoTypeId);
                continue;
            }
            auto type = pool.schemaType(branch);
            auto const& row = types[branch];
            for (auto record : effectiveFields.range(effectiveFieldOrders[branch]))
                names.insert(fieldRows[record].name);
            // Alias spellings get point-lookup entries but never become paths.
            for (auto base = branch; base; base = TypeId(types[base].base))
                for (auto const& alias : aliases.range(types[base].aliases))
                    names.insert(alias.alias);
            auto name = SchemaType::kindNameId(type.kind());
            bool unknown = name == StringPool::SchemaUnknown || name == StringPool::SchemaAny ||
                (type.composition() == SchemaType::Composition::AllOf && !row.alternatives);
            if (type.open() || unknown)
                index.flags |= 2;
            auto children = edges.range(row.elements);
            if (children.empty() && ((type.hasAffinity(ValueType::Array) &&
                type.composition() == SchemaType::Composition::None) || unknown))
                items.push_back(NoTypeId);
            else
                items.insert(items.end(), children.begin(), children.end());
        }
        index.combinedItems = indexArray(queryTypes, items);
        std::vector<QueryField> members;
        for (auto name : names)
        {
            std::vector<TypeId> children;
            bool context = true;
            for (auto branch : branches)
            {
                chargeIndexWork();
                auto type = pool.schemaType(branch);
                auto parent = branch;
                auto canonical = name;
                auto field = declaredField(parent, canonical);
                if (field)
                {
                    auto targets = edges.range(field->children);
                    if (targets.empty())
                        children.push_back(NoTypeId);
                    else
                        children.insert(children.end(), targets.begin(), targets.end());
                    context &= bool(field->flags & 2);
                }
                auto kind = SchemaType::kindNameId(type.kind());
                if (!type || (!field && type.open()) || kind == StringPool::SchemaUnknown ||
                    kind == StringPool::SchemaAny ||
                    (type.composition() == SchemaType::Composition::AllOf && type.alternatives().empty()))
                {
                    children.push_back(NoTypeId);
                    context = false;
                }
            }
            members.push_back({indexArray(queryTypes, children), name, uint8_t(context)});
        }
        index.combinedFields = indexArray(queryFields, members);
    }
}

void SchemaPool::Impl::indexAliases()
{
    std::map<StringId, std::set<StringId>> direct;
    for (auto const& row : types)
        for (auto const& alias : aliases.range(row.aliases))
            direct[alias.alias].insert(alias.canonical);
    std::vector<AliasData> targets;
    for (auto const& [alias, children] : direct)
    {
        std::set<StringId> reached;
        std::vector<StringId> pending(children.begin(), children.end());
        while (!pending.empty())
        {
            auto current = pending.back();
            pending.pop_back();
            chargeIndexWork();
            if (!reached.insert(current).second)
                continue;
            if (auto found = direct.find(current); found != direct.end())
                pending.insert(pending.end(), found->second.begin(), found->second.end());
        }
        for (auto canonical : reached)
            targets.push_back({alias, canonical});
    }
    allAliases = indexArray(queryAliases, targets);
}

} // namespace simfil
