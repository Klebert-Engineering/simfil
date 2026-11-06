#pragma once

#include "simfil/model/schema-pool.h"

#include <bit>
#include <cmath>
#include <map>
#include <set>
#include <unordered_map>

namespace simfil
{

/** Canonical schema records are constructed directly in columns and arenas.
 * Finalization derives lookup indexes without replacing the graph's storage. */
struct SchemaPool::Impl
{
    static constexpr size_t MaxEdges = 4 * 1024 * 1024;
    static constexpr uint32_t CodecVersion = 4;

    /** Fixed-width identity and collection handles; zero handles denote empty arrays. */
    struct TypeData
    {
        MODEL_COLUMN_TYPE(48);
        uint32_t kind = uint32_t(SchemaType::Kind::Unknown), flags = 0;
        ModelNodeAddress typeName;
        uint32_t numeric = 0, base = 0;
        ArrayIndex fields = 0, elements = 0, alternatives = 0, symbols = 0;
        ArrayIndex literals = 0, named = 0, aliases = 0;
    };

    /** Presence, multiplicity and enumeration role belong to the field edge. */
    struct FieldData
    {
        MODEL_COLUMN_TYPE(12);
        ArrayIndex children = 0;
        StringId name = 0;
        TypeId owner = NoTypeId;
        uint8_t required = 0, flags = 0;
        uint16_t reserved = 0;
    };

    /** Exact scalar storage shared by numeric bounds and named runtime values. */
    struct ScalarData
    {
        MODEL_COLUMN_TYPE(16);
        uint64_t bits = 0;
        ModelNodeAddress text;
        StringId name = 0;
        uint8_t type = 0, flags = 0;
    };

    /** An ordinal's presence and signedness are independent of its runtime literal. */
    struct NamedData
    {
        MODEL_COLUMN_TYPE(32);
        uint64_t bits = 0;
        ModelNodeAddress text;
        StringId name = 0;
        uint8_t type = 0, flags = 0;
        uint64_t rawBits = 0;
        uint8_t rawType = 0;
        uint8_t reserved[7]{};
    };

    /** Stable interval identity for descriptor navigation; retired rows are zeroed. */
    struct IntervalData
    {
        MODEL_COLUMN_TYPE(32);
        ScalarData minimum, maximum;
    };

    /** Numeric interpretation and intervals are independent of named sentinels. */
    struct NumericData
    {
        MODEL_COLUMN_TYPE(64);
        ModelNodeAddress encoding, unit;
        ScalarData minimum, maximum;
        double scale = 1, offset = 0;
        ArrayIndex ranges = 0;
        uint32_t flags = 0;
    };

    /** Alias edges affect lookup, not enumerable member order. */
    struct AliasData
    {
        MODEL_COLUMN_TYPE(4);
        StringId alias = 0, canonical = 0;
    };

    /** Published query indexes contain arena handles, never per-type heap objects. */
    struct TypeIndex
    {
        MODEL_COLUMN_TYPE(28);
        ArrayIndex directNames = 0, recursiveNames = 0, recursiveSymbols = 0;
        ArrayIndex fieldLookup = 0, combinedFields = 0, combinedItems = 0;
        uint32_t flags = 0;
    };

    /** Combined member targets borrow a derived type-list arena; roles remain edge metadata. */
    struct QueryField
    {
        MODEL_COLUMN_TYPE(8);
        ArrayIndex children = 0;
        StringId name = 0;
        uint8_t context = 0, reserved = 0;
    };

    explicit Impl(SchemaPool& pool) : pool(pool), dictionary(*pool.strings()) { types.emplace_back(); }
    SchemaPool& pool;
    StringPool& dictionary;
    std::string identity;
    bool packed = false, packing = false;
    ModelColumn<TypeData> types;
    ModelColumn<FieldData> fieldRows;
    ArrayArena<uint32_t> fields;
    ArrayArena<TypeId> edges;
    ArrayArena<StringId> symbols;
    ArrayArena<ModelNodeAddress> literals;
    ModelColumn<NamedData> namedRows;
    ArrayArena<uint32_t> named;
    ModelColumn<IntervalData> intervalRows;
    ArrayArena<uint32_t> numbers;
    ModelColumn<NumericData> numerics;
    ArrayArena<AliasData> aliases;
    ModelColumn<ArrayIndex> effectiveFieldOrders;
    ArrayArena<uint32_t> effectiveFields;
    ModelColumn<TypeIndex> indexes;
    ArrayArena<StringId> queryNames;
    ArrayArena<uint32_t> fieldLookup;
    ArrayArena<TypeId> queryTypes;
    ArrayArena<QueryField> queryFields;
    ArrayArena<AliasData> queryAliases;
    ArrayIndex allAliases = 0, unknownTypes = 0;
    size_t edgeCount = 0;
    size_t activeAppends = 0;
    size_t appendFloor = 0;
    bool memberIndexesReady = false;
    mutable size_t indexWork = 0;
    std::unordered_map<std::string, ModelNodeAddress> textIndex;
    SchemaPool::Statistics statistics;

    /** Bound inherited/combined preparation, including edges hidden by overrides. */
    void chargeIndexWork() const
    {
        if (!memberIndexesReady && ++indexWork > 64 * 1024 * 1024)
            throw std::length_error("Schema index work budget exceeded");
    }

    /** Reject mutation once publication starts, including a failed finalization. */
    auto mutableType(TypeId id) -> TypeData&
    {
        if (packed || packing || pool.frozen())
            throw std::logic_error("SchemaPool is immutable after finalization");
        if (!valid(id))
            throw std::invalid_argument("Unknown schema type");
        if (types[id].flags & 8)
            throw std::logic_error("Schema type is sealed");
        if (activeAppends && id < appendFloor)
            throw std::logic_error("An append callback cannot modify an existing schema type");
        return types[id];
    }

    /** Lazily allocate a collection and append without copying its existing elements. */
    template <class T>
    auto append(ArrayArena<T>& arena, ArrayIndex& handle, T const& value) -> T&
    {
        if (edgeCount >= MaxEdges)
            throw std::length_error("Schema collection exceeds its edge budget");
        if (!handle)
            handle = arena.new_array(2);
        auto& result = arena.push_back(handle, value);
        ++edgeCount;
        return result;
    }

    /** Allocate one stable record and append its identity, rolling back a failed membership append. */
    template <class T>
    void appendRecord(ModelColumn<T>& rows, ArrayArena<uint32_t>& arena,
                      ArrayIndex& handle, T const& row)
    {
        if (rows.size() >= MaxEdges)
            throw std::length_error("Schema record budget exceeded");
        auto index = uint32_t(rows.size());
        rows.push_back(row);
        try
        {
            append(arena, handle, index);
        }
        catch (...)
        {
            rows.resize(index);
            throw;
        }
    }

    /** Read canonical field rows through ordered membership IDs, including untrusted input. */
    auto fieldRecords(ArrayIndex handle) const
    {
        return fields.range(handle) | std::views::transform([this](uint32_t index) -> FieldData const& {
            if (index >= fieldRows.size())
                throw std::invalid_argument("Dangling schema field record");
            return fieldRows[index];
        });
    }

    /** Read named declarations without copying their payloads into each type. */
    auto namedRecords(ArrayIndex handle) const
    {
        return named.range(handle) | std::views::transform([this](uint32_t index) -> NamedData const& {
            if (index >= namedRows.size())
                throw std::invalid_argument("Dangling schema named record");
            return namedRows[index];
        });
    }

    /** Read interval rows by stable identity rather than alternating unaddressed bounds. */
    auto intervalRecords(ArrayIndex handle) const
    {
        return numbers.range(handle) | std::views::transform([this](uint32_t index) -> IntervalData const& {
            if (index >= intervalRows.size())
                throw std::invalid_argument("Dangling schema interval record");
            return intervalRows[index];
        });
    }

    /** Discard superseded interval payloads without relocating any surviving address. */
    void retireIntervals(ArrayIndex handle)
    {
        for (auto index : numbers.range(handle))
            intervalRows[index] = IntervalData{};
        if (handle)
            numbers.truncate(handle, 0);
    }

    /** Append metadata names only during construction. */
    auto intern(std::string_view name) -> StringId
    {
        auto result = dictionary.emplace(name);
        if (!result)
            throw std::length_error(result.error().message);
        return *result;
    }
    /** Deduplicate immutable metadata strings without widening the name dictionary. */
    auto text(std::string_view value) -> ModelNodeAddress
    {
        if (value.empty())
            return {};
        auto [entry, inserted] = textIndex.try_emplace(std::string(value));
        if (inserted)
            entry->second = pool.newValue(value)->addr();
        return entry->second;
    }
    /** Only string columns may supply descriptor text. */
    auto text(ModelNodeAddress value) const -> std::string_view
    {
        if (!value)
            return {};
        if (value.column() != ModelPool::String)
            throw std::invalid_argument("Schema text does not reference string storage");
        auto scalar = primitive(value);
        if (auto text = std::get_if<std::string_view>(&scalar))
            return *text;
        throw std::invalid_argument("Schema text is not a borrowed string-buffer value");
    }
    /** Decode primitive metadata, validating stored-node addresses through core resolution. */
    auto primitive(ModelNodeAddress address) const -> ScalarValueType
    {
        switch (address.column())
        {
        case Model::Null:
            // Null literals use the zero-address sentinel, not a dereferenceable pool node.
            return std::monostate{};
        case Model::Bool:
        case Model::UInt16:
        case Model::Int16:
        case ModelPool::Int64:
        case ModelPool::Double:
        case ModelPool::String:
        case ModelPool::ByteArray:
            break;
        default:
            throw std::invalid_argument(
                "Schema metadata must reference core unqualified primitives");
        }
        ScalarValueType value;
        auto node = pool.resolve(address);
        auto result = pool.resolve(
            *node, Model::Lambda([&](auto const& resolved) { value = resolved.value(); }));
        if (!result)
            throw std::invalid_argument(result.error().message);
        return value;
    }
    /** Preserve exact primitive bits while moving borrowed text into owner storage. */
    template <class T> auto scalar(T const& value) -> ScalarData
    {
        ScalarData result;
        if constexpr (std::is_same_v<T, bool>)
        {
            result.type = 0;
            result.bits = value;
        }
        else if constexpr (std::is_same_v<T, int64_t>)
        {
            result.type = 1;
            result.bits = std::bit_cast<uint64_t>(value);
        }
        else if constexpr (std::is_same_v<T, uint64_t>)
        {
            result.type = 2;
            result.bits = value;
        }
        else if constexpr (std::is_same_v<T, double>)
        {
            if (!std::isfinite(value))
                throw std::invalid_argument("Schema numeric metadata must be finite");
            result.type = 3;
            result.bits = std::bit_cast<uint64_t>(value);
        }
        else
        {
            result.type = 4;
            result.text = text(value);
        }
        return result;
    }
    /** Decode numeric metadata without routing uint64 values through double. */
    static auto number(ScalarData const& value) -> SchemaType::NumericValue
    {
        switch (value.type)
        {
        case 1:
            return std::bit_cast<int64_t>(value.bits);
        case 2:
            return value.bits;
        case 3:
            if (std::isfinite(std::bit_cast<double>(value.bits)))
                return std::bit_cast<double>(value.bits);
            break;
        default:
            break;
        }
        throw std::invalid_argument("Invalid numeric schema metadata");
    }
    /** Reject runtime-only scalars rather than attaching another graph to enum metadata. */
    auto enumValue(ModelNodeAddress address) const -> SchemaType::EnumValue
    {
        return std::visit([](auto const& value) -> SchemaType::EnumValue {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_constructible_v<SchemaType::EnumValue, T>)
                return value;
            else
                throw std::invalid_argument("Invalid non-string schema literal");
        }, primitive(address));
    }
    /** Copy a primitive literal into common model storage. */
    auto literal(SchemaType::EnumValue const& value) -> ModelNodeAddress
    {
        return std::visit(
            [&](auto const& item) -> ModelNodeAddress
            {
                using T = std::decay_t<decltype(item)>;
                if constexpr (std::is_same_v<T, std::monostate>)
                    return {};
                else if constexpr (std::is_same_v<T, bool>)
                    return pool.newSmallValue(item)->addr();
                else
                    return pool.newValue(item)->addr();
            },
            value);
    }
    /** Coalesce declarations of one direct field without changing insertion order. */
    auto field(TypeId id, std::string_view name) -> FieldData&
    {
        auto& row = mutableType(id);
        auto key = intern(name);
        for (auto index : fields.range(row.fields))
            if (fieldRows[index].name == key)
                return fieldRows[index];
        auto index = fieldRows.size();
        appendRecord(fieldRows, fields, row.fields, FieldData{.name = key, .owner = id});
        return fieldRows[index];
    }

    /** Reduce repeated declarations without allocating or guessing between different types. */
    auto uniqueType(ArrayIndex children) const -> TypeId
    {
        auto values = edges.range(children);
        if (values.empty())
            return NoTypeId;
        auto id = values.front();
        for (auto value : values)
            if (value != id)
                return NoTypeId;
        return id;
    }

    /** Resolve an inherited field without requiring any finalized index. */
    auto declaredField(TypeId& id, StringId& name) const -> FieldData const*
    {
        // The common runtime path is one indexed parent lookup. Alias-bearing
        // inheritance retains its declaration-shadowing rules below.
        if (memberIndexesReady && valid(id) && !(indexes[id].flags & 4))
        {
            auto values = fieldLookup.range(indexes[id].fieldLookup);
            size_t first = 0, last = values.size();
            while (first < last)
            {
                auto middle = first + (last - first) / 2;
                if (fieldRows[values[middle]].name < name)
                    first = middle + 1;
                else
                    last = middle;
            }
            if (first == values.size() || fieldRows[values[first]].name != name)
                return nullptr;
            auto const& field = fieldRows[values[first]];
            id = field.owner;
            return &field;
        }
        for (size_t depth = 0; valid(id); ++depth)
        {
            if (depth > 256)
                throw std::invalid_argument("Cyclic or excessive schema inheritance");
            auto const& row = types[id];
            for (auto const& field : fieldRecords(row.fields))
                if (field.name == name)
                    return &field;
            // Actual declarations shadow aliases; aliases are lookup-only.
            for (auto const& alias : aliases.range(row.aliases))
                if (alias.alias == name)
                {
                    name = alias.canonical;
                    for (auto const& field : fieldRecords(row.fields))
                        if (field.name == name)
                            return &field;
                    break;
                }
            id = TypeId(row.base);
        }
        return nullptr;
    }

    /** Borrow one precomputed combined member; no child graph traversal occurs here. */
    auto combinedField(TypeId id, StringId name) const -> QueryField const*
    {
        auto members = queryFields.range(indexes[id].combinedFields);
        size_t first = 0, last = members.size();
        while (first < last)
        {
            auto middle = first + (last - first) / 2;
            if (members[middle].name < name)
                first = middle + 1;
            else
                last = middle;
        }
        return first < members.size() && members[first].name == name ? &members[first] : nullptr;
    }

    /** Binary-search a fixed-size name index without forward-iterator linear advancement. */
    auto containsName(ArrayIndex handle, StringId name) const -> bool
    {
        auto values = queryNames.range(handle);
        size_t first = 0, last = values.size();
        while (first < last)
        {
            auto middle = first + (last - first) / 2;
            if (values[middle] < name)
                first = middle + 1;
            else
                last = middle;
        }
        return first < values.size() && values[first] == name;
    }

    /** Test identity presence independently of publication state. */
    auto valid(TypeId id) const -> bool { return id && id < types.size(); }
    /** Copy direct records while reacquiring source positions after each arena mutation. */
    void copyDirectType(TypeId target, SchemaPool const& source, TypeId sourceId,
                        std::span<TypeId const> remap = {});
    /** Decode one named declaration without retaining a secondary metadata object. */
    auto decodeNamed(NamedData const& row) const -> SchemaType::NamedValue;
    /** Intern one named declaration and encode its exact primitive metadata. */
    auto encodeNamed(SchemaType::NamedValue const& value) -> NamedData;
    struct AppendRollback;
    void prepareAffinities();
    void validate(size_t literalBudget = SIZE_MAX) const;
    void index();
    /** Prepare inherited descriptor membership without flattening canonical declarations. */
    void indexFieldOrder();
    /** Prepare effective composition members and sorted direct point lookups. */
    void indexMembers();
    /** Prepare a conservative alias reachability index without retaining a map. */
    void indexAliases();
    /** Append one fixed-size derived array; callers may share its handle. */
    template <class T, class Range>
    auto indexArray(ArrayArena<T>& arena, Range const& values) -> ArrayIndex
    {
        if (values.empty())
            return 0;
        indexWork += values.size();
        if (indexWork > 64 * 1024 * 1024)
            throw std::length_error("Schema index work budget exceeded");
        auto handle = arena.new_array(values.size());
        for (auto const& value : values)
            arena.push_back(handle, value);
        return handle;
    }
    template <class S> void readWrite(S& archive, size_t budget);
    template <class S>
    static void string(S& archive, std::string& value, size_t limit, size_t& memory);
};

/** Roll back only appended canonical records; existing graph records are never edited in scope. */
struct SchemaPool::Impl::AppendRollback
{
    Impl& graph;
    size_t types, numerics, fieldCount, namedCount, intervalCount, edges, previousFloor;
    ModelPool::ScalarAppendCheckpoint primitives;
    StringPool::AppendCheckpoint dictionary;
    ArrayArena<uint32_t>::AppendCheckpoint fieldArrays;
    ArrayArena<TypeId>::AppendCheckpoint edgeArrays;
    ArrayArena<StringId>::AppendCheckpoint symbolArrays;
    ArrayArena<ModelNodeAddress>::AppendCheckpoint literalArrays;
    ArrayArena<uint32_t>::AppendCheckpoint namedArrays;
    ArrayArena<uint32_t>::AppendCheckpoint numberArrays;
    ArrayArena<AliasData>::AppendCheckpoint aliasArrays;
    bool committed = false;

    /** Snapshot logical tails without allocating a second graph or copying existing arrays. */
    explicit AppendRollback(Impl& graph)
        : graph(graph), types(graph.types.size()), numerics(graph.numerics.size()),
          fieldCount(graph.fieldRows.size()), namedCount(graph.namedRows.size()),
          intervalCount(graph.intervalRows.size()), edges(graph.edgeCount),
          previousFloor(graph.appendFloor),
          primitives(graph.pool.scalarAppendCheckpoint()), dictionary(graph.dictionary.appendCheckpoint()),
          fieldArrays(graph.fields.checkpointAppends()), edgeArrays(graph.edges.checkpointAppends()),
          symbolArrays(graph.symbols.checkpointAppends()), literalArrays(graph.literals.checkpointAppends()),
          namedArrays(graph.named.checkpointAppends()), numberArrays(graph.numbers.checkpointAppends()),
          aliasArrays(graph.aliases.checkpointAppends())
    {
        ++graph.activeAppends;
        graph.appendFloor = types;
    }

    /** Restore logical contents and dictionary IDs; retained capacity remains accounted for. */
    ~AppendRollback()
    {
        --graph.activeAppends;
        graph.appendFloor = previousFloor;
        if (committed)
            return;
        graph.types.resize(types);
        graph.numerics.resize(numerics);
        graph.fieldRows.resize(fieldCount);
        graph.namedRows.resize(namedCount);
        graph.intervalRows.resize(intervalCount);
        graph.fields.rollbackAppends(fieldArrays);
        graph.edges.rollbackAppends(edgeArrays);
        graph.symbols.rollbackAppends(symbolArrays);
        graph.literals.rollbackAppends(literalArrays);
        graph.named.rollbackAppends(namedArrays);
        graph.numbers.rollbackAppends(numberArrays);
        graph.aliases.rollbackAppends(aliasArrays);
        graph.edgeCount = edges;
        for (auto it = graph.textIndex.begin(); it != graph.textIndex.end();)
        {
            if (!it->second || it->second.index() >= primitives.stringRows)
                it = graph.textIndex.erase(it);
            else
                ++it;
        }
        graph.dictionary.rollbackAppends(dictionary);
        graph.pool.rollbackScalarAppends(primitives);
    }
};

} // namespace simfil
