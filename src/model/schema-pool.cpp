#include "simfil/model/schema-pool.h"
#include "simfil/model/schema-model.h"

#include <bit>
#include <cmath>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

#include <bitsery/adapter/buffer.h>
#include <bitsery/adapter/stream.h>
#include <bitsery/traits/string.h>
#include <bitsery/traits/vector.h>

namespace simfil
{

/** Canonical generic columns. Drafts exist only while constructing the graph;
 * finalized views borrow these columns and never own a second schema tree. */
struct SchemaPool::Impl
{
    static constexpr size_t MaxEdges = 4 * 1024 * 1024;
    static constexpr uint32_t CodecVersion = 1;

    /** Checked slice into an owner-wide column. */
    struct Range
    {
        uint32_t begin = 0, count = 0;
        template <class S> void serialize(S& s)
        {
            s.value4b(begin);
            s.value4b(count);
        }
        template <class T> auto in(std::vector<T> const& values) const -> std::span<T const>
        {
            if (begin > values.size() || count > values.size() - begin)
                throw std::invalid_argument("Schema column range exceeds its storage");
            return std::span<T const>(values).subspan(begin, count);
        }
    };
    /** Fixed-width domain record; metadata text lives in the inherited ModelPool. */
    struct DomainData
    {
        MODEL_COLUMN_TYPE(76);
        uint32_t kind = uint32_t(Schema::Kind::Unknown), flags = 0;
        ModelNodeAddress typeName;
        uint32_t numeric = 0, base = 0;
        Range fields, elements, alternatives, symbols, literals, named, aliases;
    };
    /** Presence, multiplicity and enumeration role belong to the field edge. */
    struct FieldData
    {
        Range children;
        StringId name = 0;
        uint8_t required = 0, flags = 0;
        template <class S> void serialize(S& s)
        {
            s.object(children);
            s.value2b(name);
            s.value1b(required);
            s.value1b(flags);
        }
    };
    /** Exact named scalar storage without borrowed caller strings. */
    struct NamedData
    {
        uint64_t bits = 0;
        ModelNodeAddress text;
        StringId name = 0;
        uint8_t type = 0, flags = 0;
        template <class S> void serialize(S& s)
        {
            s.value8b(bits);
            s.value4b(text.value_);
            s.value2b(name);
            s.value1b(type);
            s.value1b(flags);
        }
    };
    /** Numeric interpretation and intervals are independent of named sentinels. */
    struct NumericData
    {
        ModelNodeAddress encoding, unit;
        NamedData minimum, maximum;
        double scale = 1, offset = 0;
        Range ranges;
        uint32_t flags = 0, reserved = 0;
        template <class S> void serialize(S& s)
        {
            s.value4b(encoding.value_);
            s.value4b(unit.value_);
            s.object(minimum);
            s.object(maximum);
            s.value8b(scale);
            s.value8b(offset);
            s.object(ranges);
            s.value4b(flags);
            s.value4b(reserved);
        }
    };
    /** Alias edges affect lookup, not enumerable member order. */
    struct AliasData
    {
        StringId alias = 0, canonical = 0;
        template <class S> void serialize(S& s)
        {
            s.value2b(alias);
            s.value2b(canonical);
        }
    };
    /** Append-friendly mutable field storage, discarded when packed. */
    struct DraftField
    {
        FieldData data;
        std::vector<SchemaId> children;
    };
    /** Construction state contains only the graph being built, not a JSON projection. */
    struct Draft
    {
        DomainData data;
        std::vector<DraftField> fields;
        std::vector<SchemaId> elements, alternatives;
        std::vector<StringId> symbols;
        std::vector<ModelNodeAddress> literals;
        std::vector<NamedData> named;
        std::vector<AliasData> aliases;
    };
    /** Shared immutable name sets avoid repeating wide descendant indexes. */
    struct NameSet
    {
        std::vector<StringId> ids;
        std::vector<std::string_view> names;
    };
    struct Domain;

    explicit Impl(SchemaPool& pool) : pool(pool), drafts(1) {}
    SchemaPool& pool;
    bool packed = false, packing = false;
    std::vector<Draft> drafts;
    ModelColumn<DomainData> domains;
    std::vector<FieldData> fields;
    std::vector<SchemaId> edges;
    std::vector<StringId> symbols;
    std::vector<ModelNodeAddress> literals;
    std::vector<NamedData> named, numbers;
    std::vector<NumericData> numerics;
    std::vector<AliasData> aliases;
    std::vector<std::unique_ptr<Domain>> views;
    std::vector<NameSet> sets;
    std::map<std::pair<bool, std::vector<StringId>>, size_t> setIndex;
    bool memberIndexesReady = false;
    mutable size_t indexWork = 0;
    std::map<StringId, std::vector<StringId>> aliasTargets;
    std::unordered_map<std::string, ModelNodeAddress> textIndex;
    SchemaPool::Statistics statistics;

    /** Bound inherited/combined preparation, including edges hidden by overrides. */
    void chargeIndexWork() const
    {
        if (!memberIndexesReady && ++indexWork > 64 * 1024 * 1024)
            throw std::length_error("Schema index work budget exceeded");
    }

    /** Reject mutation once publication has started, even before the derived owner freezes. */
    auto draft(SchemaId id) -> Draft&
    {
        if (packed || packing || pool.frozen())
            throw std::logic_error("SchemaPool is immutable after finalization");
        if (!id || id >= drafts.size())
            throw std::invalid_argument("Unknown schema domain");
        return drafts[id];
    }
    /** Append metadata names only during construction. */
    auto intern(std::string_view name) -> StringId
    {
        auto result = pool.strings()->emplace(name);
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
    /** Validate addresses through core resolution before reading primitive metadata. */
    auto primitive(ModelNodeAddress address) const -> ScalarValueType
    {
        switch (address.column())
        {
        case Model::Null:
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
    template <class T> auto scalar(T const& value) -> NamedData
    {
        NamedData result;
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
    static auto number(NamedData const& value) -> Schema::NumericValue
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
    /** Copy a primitive literal into common model storage. */
    auto literal(ScalarValueType const& value) -> ModelNodeAddress
    {
        return std::visit(
            [&](auto const& item) -> ModelNodeAddress
            {
                using T = std::decay_t<decltype(item)>;
                if constexpr (std::is_same_v<T, std::monostate>)
                    return {};
                else if constexpr (std::is_same_v<T, bool>)
                    return pool.newSmallValue(item)->addr();
                else if constexpr (std::is_same_v<T, simfil::Bitmask>)
                    throw std::invalid_argument("Bitmask schema literals require named masks");
                else if constexpr (std::is_same_v<T, std::string>)
                    return pool.newValue(std::string_view(item))->addr();
                else
                    return pool.newValue(item)->addr();
            },
            value);
    }
    /** Coalesce declarations of one direct field without changing insertion order. */
    auto field(SchemaId id, std::string_view name) -> DraftField&
    {
        auto& value = draft(id);
        auto key = intern(name);
        auto found =
            std::ranges::find_if(value.fields, [&](auto const& f) { return f.data.name == key; });
        if (found != value.fields.end())
            return *found;
        value.fields.push_back({FieldData{.name = key}, {}});
        return value.fields.back();
    }
    /** Pack one variable column with a checked uint32 range. */
    template <class T> static auto append(std::vector<T>& output, std::span<T const> input) -> Range
    {
        if (input.size() > MaxEdges - output.size())
            throw std::length_error("Schema edge budget exceeded");
        Range range{uint32_t(output.size()), uint32_t(input.size())};
        output.insert(output.end(), input.begin(), input.end());
        return range;
    }
    auto valid(SchemaId id) const -> bool { return id && id < domains.size(); }
    void pack();
    void validate(size_t literalBudget = SIZE_MAX) const;
    void index();
    auto nameSet(std::vector<StringId> names, bool sort) -> size_t;
    template <class S> void readWrite(S& archive, size_t budget);
    template <class S, class T, class F>
    static void container(S& archive, std::vector<T>& values, size_t limit, size_t& memory,
                          F&& item);
    template <class S>
    static void string(S& archive, std::string& value, size_t limit, size_t& memory);
};

/** A borrowed view over one canonical domain row, pinned externally by bindings. */
struct SchemaPool::Impl::Domain final : Schema
{
    Impl const& graph;
    SchemaId id;
    std::array<size_t, 5> names{};
    bool complete = false, contextFields = false;
    std::vector<ScalarValueType> values;
    std::vector<NumericRange> ranges;
    std::optional<NumericDomain> numeric;
    Domain(Impl const& graph, SchemaId id) : graph(graph), id(id) {}
    auto data() const -> DomainData const& { return graph.domains[id]; }
    auto kind() const -> Kind override { return Kind(data().kind); }
    auto typeName() const -> std::string_view override { return graph.text(data().typeName); }
    auto composition() const -> Composition override
    {
        return Composition((data().flags >> 4) & 3);
    }
    auto open() const -> bool override { return data().flags & 1; }
    auto nullable() const -> std::optional<bool> override
    {
        return data().flags & 2 ? std::optional<bool>(data().flags & 4) : std::nullopt;
    }
    auto reachabilityComplete() const -> bool override { return complete; }
    auto finalized() const -> bool override { return true; }
    auto alternatives() const& -> std::span<SchemaId const> override
    {
        return data().alternatives.in(graph.edges);
    }
    auto enumValues() const& -> std::span<ScalarValueType const> override { return values; }
    auto directFields() const& -> std::span<StringId const> override
    {
        return graph.sets[names[0]].ids;
    }
    auto nestedFields() const& -> std::span<StringId const> override
    {
        return graph.sets[names[1]].ids;
    }
    auto directEnumSymbols() const& -> std::span<StringId const> override
    {
        return graph.sets[names[2]].ids;
    }
    auto nestedEnumSymbols() const& -> std::span<StringId const> override
    {
        return graph.sets[names[3]].ids;
    }
    auto canHaveField(StringId field) const -> bool override
    {
        auto fields = nestedFields();
        if (!complete || std::ranges::binary_search(fields, field))
            return true;
        // Alias absence must remain conservative through descendants without
        // making aliases additional enumerable fields or duplicating paths.
        auto found = graph.aliasTargets.find(field);
        return found != graph.aliasTargets.end() &&
               std::ranges::any_of(found->second, [&](StringId target)
                                   { return std::ranges::binary_search(fields, target); });
    }
    auto canHaveEnumSymbol(StringId symbol) const -> bool override
    {
        return std::ranges::binary_search(nestedEnumSymbols(), symbol);
    }
    auto canonicalField(StringId field) const -> StringId override
    {
        if (!data().aliases.count && !data().base)
            return field;
        for (auto const& own : data().fields.in(graph.fields))
            if (own.name == field)
                return field;
        for (auto const& alias : data().aliases.in(graph.aliases))
            if (alias.alias == field)
                return alias.canonical;
        return data().base ? graph.views[data().base]->canonicalField(field) : field;
    }
    auto fieldRequired(StringId field) const -> std::optional<bool> override
    {
        field = canonicalField(field);
        for (auto const& own : data().fields.in(graph.fields))
            if (own.name == field)
                return own.required ? std::optional<bool>(own.required == 2) : std::nullopt;
        return data().base ? graph.views[data().base]->fieldRequired(field) : std::nullopt;
    }
    auto fieldRole(StringId field) const -> FieldRole override
    {
        if (graph.memberIndexesReady &&
            (composition() != Composition::None || !alternatives().empty()))
            return Schema::fieldRole(field);
        if (!contextFields && !data().base)
            return FieldRole::Data;
        field = canonicalField(field);
        for (auto const& own : data().fields.in(graph.fields))
            if (own.name == field)
                return own.flags & 2 ? FieldRole::Context : FieldRole::Data;
        return data().base ? graph.views[data().base]->fieldRole(field) : FieldRole::Data;
    }
    auto fieldSchema(StringId field) const -> SchemaId override
    {
        if (composition() != Composition::None || !alternatives().empty())
            return Schema::fieldSchema(field);
        field = canonicalField(field);
        for (auto const& own : data().fields.in(graph.fields))
            if (own.name == field)
                return uniqueSchema(own.children.in(graph.edges));
        return data().base ? graph.views[data().base]->fieldSchema(field) : NoSchemaId;
    }
    auto itemSchema() const -> SchemaId override
    {
        return composition() != Composition::None || !alternatives().empty()
                   ? Schema::itemSchema()
                   : uniqueSchema(data().elements.in(graph.edges));
    }
    /** Inheritance changes the current object, not the depth of its field paths. */
    void fields(std::function<void(StringId, std::span<SchemaId const>)> const& fn,
                bool enumerable) const
    {
        graph.chargeIndexWork();
        if (data().base)
            graph.views[data().base]->fields(
                [&](StringId field, auto children)
                {
                    auto const& own = graph.sets[names[4]].ids;
                    if (!std::ranges::binary_search(own, field))
                        fn(field, children);
                },
                enumerable);
        for (auto const& field : data().fields.in(graph.fields))
        {
            graph.chargeIndexWork();
            if (!enumerable || !(field.flags & 2))
                fn(field.name, field.children.in(graph.edges));
        }
    }
    void forEachDirectField(
        std::function<void(StringId, std::span<SchemaId const>)> const& fn) const override
    {
        fields(fn, false);
    }
    void forEachEnumerableField(
        std::function<void(StringId, std::span<SchemaId const>)> const& fn) const override
    {
        fields(fn, true);
    }
    void forEachElementSchema(std::function<void(SchemaId)> const& fn) const override
    {
        for (auto id : data().elements.in(graph.edges))
            fn(id);
    }
    auto numericDomain() const -> std::optional<NumericDomain> override { return numeric; }
    auto bitWidth() const -> uint8_t override { return uint8_t(data().flags >> 8); }
    void forEachNamedValue(std::function<void(NamedValue const&)> const& fn) const override
    {
        for (auto const& row : data().named.in(graph.named))
        {
            NamedValue value;
            value.name = graph.pool.strings()->resolve(row.name).value();
            value.role = NamedValue::Role(row.flags & 3);
            value.meaning = NamedValue::Meaning((row.flags >> 2) & 7);
            value.deprecated = row.flags & 32;
            value.removed = row.flags & 64;
            switch (row.type)
            {
            case 0:
                value.value = bool(row.bits);
                break;
            case 1:
                value.value = std::bit_cast<int64_t>(row.bits);
                break;
            case 2:
                value.value = row.bits;
                break;
            case 3:
                value.value = std::bit_cast<double>(row.bits);
                break;
            case 4:
                value.value = graph.text(row.text);
                break;
            default:
                throw std::invalid_argument("Invalid schema named value type");
            }
            fn(value);
        }
    }
    auto indexMemory() const -> MemoryUsage
    {
        return {fieldSchemas_.size() * sizeof(decltype(fieldSchemas_)::value_type) +
                    contextFields_.size() * sizeof(StringId),
                fieldSchemas_.capacity() * sizeof(decltype(fieldSchemas_)::value_type) +
                    contextFields_.capacity() * sizeof(StringId)};
    }
};

SchemaPool::SchemaPool(std::shared_ptr<StringPool> strings)
    : ModelPool(std::move(strings)), schema_(std::make_unique<Impl>(*this))
{
}
SchemaPool::~SchemaPool() = default;

auto SchemaPool::addSchema(Schema::Kind kind) -> SchemaId
{
    requireMutable();
    if (schema_->packed || schema_->packing)
        throw std::logic_error("SchemaPool is immutable after finalization");
    if (schema_->drafts.size() > MaxSchemaId)
        throw std::length_error("SchemaPool exhausted its uint16 domain namespace");
    Impl::Draft draft;
    draft.data.kind = uint32_t(kind);
    auto affinity = Schema::affinities(kind);
    auto mask = valueTypeAffinity(ValueType::Bitmask);
    if ((affinity & mask) && !(affinity & ~(mask | valueTypeAffinity(ValueType::Null))))
        draft.data.flags |= 64u << 8;
    auto id = SchemaId(schema_->drafts.size());
    schema_->drafts.push_back(std::move(draft));
    return id;
}
void SchemaPool::addFieldSchema(SchemaId parent, std::string_view name, SchemaId child,
                                bool multimap)
{
    auto& field = schema_->field(parent, name);
    field.children.push_back(child);
    field.data.flags |= uint8_t(multimap);
}
void SchemaPool::setFieldRequired(SchemaId id, std::string_view name, std::optional<bool> required)
{
    schema_->field(id, name).data.required = required ? (*required ? 2 : 1) : 0;
}
void SchemaPool::setFieldRole(SchemaId id, std::string_view name, Schema::FieldRole role)
{
    auto& field = schema_->field(id, name);
    field.data.flags =
        (field.data.flags & ~uint8_t{2}) | (role == Schema::FieldRole::Context ? 2 : 0);
}
void SchemaPool::addFieldAlias(SchemaId id, std::string_view alias, std::string_view canonical)
{
    auto& draft = schema_->draft(id);
    draft.aliases.push_back({schema_->intern(alias), schema_->intern(canonical)});
}
void SchemaPool::setBaseSchema(SchemaId id, SchemaId base)
{
    schema_->draft(id).data.base = base;
}
void SchemaPool::addElementSchema(SchemaId id, SchemaId child)
{
    schema_->draft(id).elements.push_back(child);
}
void SchemaPool::addAlternative(SchemaId id, SchemaId child)
{
    schema_->draft(id).alternatives.push_back(child);
}
void SchemaPool::setComposition(SchemaId id, Schema::Composition composition)
{
    auto& row = schema_->draft(id).data;
    row.flags = (row.flags & ~0x30u) | (uint32_t(composition) << 4);
}
void SchemaPool::setTypeName(SchemaId id, std::string_view name)
{
    schema_->draft(id).data.typeName = schema_->text(name);
}
void SchemaPool::setOpen(SchemaId id, bool open)
{
    auto& row = schema_->draft(id).data;
    row.flags = (row.flags & ~1u) | uint32_t(open);
}
void SchemaPool::setNullable(SchemaId id, std::optional<bool> nullable)
{
    auto& row = schema_->draft(id).data;
    row.flags = (row.flags & ~6u) | (nullable ? 2u | (uint32_t(*nullable) << 2) : 0u);
}
void SchemaPool::addEnumSymbol(SchemaId id, std::string_view symbol)
{
    schema_->draft(id).symbols.push_back(schema_->intern(symbol));
}
void SchemaPool::addEnumValue(SchemaId id, ScalarValueType const& value)
{
    if (std::holds_alternative<std::string>(value) ||
        std::holds_alternative<std::string_view>(value))
        throw std::invalid_argument("String choices require addEnumSymbol");
    schema_->draft(id).literals.push_back(schema_->literal(value));
}
void SchemaPool::setNumericDomain(SchemaId id, Schema::NumericDomain const& value)
{
    auto& draft = schema_->draft(id);
    Impl::NumericData row;
    row.encoding = schema_->text(value.encoding);
    row.unit = schema_->text(value.unit);
    row.scale = value.scale;
    row.offset = value.offset;
    row.flags = (value.minimum ? 1u : 0u) | (value.maximum ? 2u : 0u) | (value.complete ? 4u : 0u);
    if (value.minimum)
        row.minimum = std::visit([&](auto v) { return schema_->scalar(v); }, *value.minimum);
    if (value.maximum)
        row.maximum = std::visit([&](auto v) { return schema_->scalar(v); }, *value.maximum);
    row.ranges.begin = schema_->numbers.size();
    for (auto const& range : value.ordinaryRanges)
    {
        schema_->numbers.push_back(
            std::visit([&](auto v) { return schema_->scalar(v); }, range.min));
        schema_->numbers.push_back(
            std::visit([&](auto v) { return schema_->scalar(v); }, range.max));
    }
    row.ranges.count = schema_->numbers.size() - row.ranges.begin;
    schema_->numerics.push_back(row);
    draft.data.numeric = schema_->numerics.size();
}
void SchemaPool::addNamedValue(SchemaId id, Schema::NamedValue const& value)
{
    auto& draft = schema_->draft(id);
    auto row = std::visit([&](auto v) { return schema_->scalar(v); }, value.value);
    row.name = schema_->intern(value.name);
    row.flags = uint8_t(value.role) | (uint8_t(value.meaning) << 2) |
                (value.deprecated ? 32u : 0u) | (value.removed ? 64u : 0u);
    draft.named.push_back(row);
}
void SchemaPool::setBitWidth(SchemaId id, uint8_t width)
{
    auto& row = schema_->draft(id).data;
    auto bits = Schema::affinities(Schema::Kind(row.kind));
    if (!width || width > 64 || !(bits & valueTypeAffinity(ValueType::Bitmask)) ||
        (bits & ~(valueTypeAffinity(ValueType::Bitmask) | valueTypeAffinity(ValueType::Null))))
        throw std::invalid_argument("Bit width requires a concrete bitmask domain");
    row.flags = (row.flags & ~0xff00u) | (uint32_t(width) << 8);
}

auto SchemaPool::finalized() const -> bool
{
    return schema_->packed;
}
auto SchemaPool::domainCount() const -> size_t
{
    if (schema_->packing && !schema_->packed)
        throw std::logic_error("Schema publication failed");
    return (schema_->packed ? schema_->domains.size() : schema_->drafts.size()) - 1;
}
auto SchemaPool::domain(SchemaId id) const -> Schema const*
{
    return schema_->packed && schema_->valid(id) ? schema_->views[id].get() : nullptr;
}
auto SchemaPool::schemaStatistics() const -> Statistics
{
    return schema_->statistics;
}
auto SchemaPool::fieldNames(SchemaId id, bool recursive) const -> std::span<std::string_view const>
{
    return domain(id) ? std::span<std::string_view const>(
                            schema_->sets[schema_->views[id]->names[recursive ? 1 : 0]].names)
                      : std::span<std::string_view const>{};
}
auto SchemaPool::enumNames(SchemaId id, bool recursive) const -> std::span<std::string_view const>
{
    return domain(id) ? std::span<std::string_view const>(
                            schema_->sets[schema_->views[id]->names[recursive ? 3 : 2]].names)
                      : std::span<std::string_view const>{};
}
auto SchemaPool::binding(std::string identity) const -> std::shared_ptr<SchemaBinding const>
{
    if (!finalized())
        throw std::logic_error("Bindings require an immutable schema graph");
    return std::make_shared<SchemaBinding>(
        std::move(identity), shared_from_this(), [this](SchemaId id) { return domain(id); },
        strings());
}
auto SchemaPool::domainNode(SchemaId id) const -> ModelNode::Ptr
{
    if (!domain(id))
        throw std::invalid_argument("Descriptor requested for an unknown domain");
    auto owner = std::static_pointer_cast<SchemaPool const>(shared_from_this());
    auto model = std::make_shared<SchemaModel>(strings(),
                                               [owner](SchemaId id) { return owner->domain(id); });
    return model->root(id);
}
auto SchemaPool::baseSchema(SchemaId id) const -> SchemaId
{
    return schema_->valid(id) ? SchemaId(schema_->domains[id].base) : NoSchemaId;
}
auto SchemaPool::fieldMultimap(SchemaId id, StringId field) const -> bool
{
    auto view = domain(id);
    if (!view)
        return false;
    field = view->canonicalField(field);
    auto const& row = schema_->domains[id];
    for (auto const& item : row.fields.in(schema_->fields))
        if (item.name == field)
            return item.flags & 1;
    return row.base && fieldMultimap(SchemaId(row.base), field);
}
void SchemaPool::forEachFieldAlias(SchemaId id,
                                   std::function<void(StringId, StringId)> const& fn) const
{
    if (domain(id))
        for (auto const& alias : schema_->domains[id].aliases.in(schema_->aliases))
            fn(alias.alias, alias.canonical);
}

void SchemaPool::Impl::pack()
{
    std::vector<NumericData> packedNumerics;
    std::vector<NamedData> packedNumbers;
    domains.resize(drafts.size());
    for (size_t id = 1; id < drafts.size(); ++id)
    {
        auto const& source = drafts[id];
        auto row = source.data;
        row.fields.begin = fields.size();
        for (auto const& field : source.fields)
        {
            auto packed = field.data;
            packed.children = append(edges, std::span<SchemaId const>(field.children));
            fields.push_back(packed);
        }
        row.fields.count = fields.size() - row.fields.begin;
        row.elements = append(edges, std::span<SchemaId const>(source.elements));
        row.alternatives = append(edges, std::span<SchemaId const>(source.alternatives));
        row.symbols = append(symbols, std::span<StringId const>(source.symbols));
        row.literals = append(literals, std::span<ModelNodeAddress const>(source.literals));
        row.named = append(named, std::span<NamedData const>(source.named));
        row.aliases = append(aliases, std::span<AliasData const>(source.aliases));
        // Numeric setters may replace earlier declarations. Retain only one
        // canonical row and one disjoint interval slice for each final owner.
        if (row.numeric)
        {
            auto numeric = numerics.at(row.numeric - 1);
            numeric.ranges = append(packedNumbers, numeric.ranges.in(numbers));
            packedNumerics.push_back(numeric);
            row.numeric = packedNumerics.size();
        }
        domains[id] = row;
    }
    numerics = std::move(packedNumerics);
    numbers = std::move(packedNumbers);
    // Resolve logical affinity by a descending fixed point. A recursive union
    // starts conservative; each bit can disappear at most once per domain.
    std::vector<std::vector<SchemaId>> parents(domains.size());
    std::vector<uint16_t> bits(domains.size(), Schema::AllAffinities);
    std::vector<bool> queued(domains.size(), true);
    std::vector<SchemaId> pending;
    for (size_t id = 1; id < domains.size(); ++id)
    {
        auto const& row = domains[id];
        if (!(row.flags & 0x30u))
            bits[id] = Schema::affinities(Schema::Kind(row.kind));
        for (auto child : row.alternatives.in(edges))
        {
            if (child >= domains.size())
                throw std::invalid_argument("Dangling alternative schema");
            parents[child].push_back(SchemaId(id));
        }
        pending.push_back(SchemaId(id));
    }
    while (!pending.empty())
    {
        auto id = pending.back();
        pending.pop_back();
        queued[id] = false;
        auto& row = domains[id];
        auto mode = Schema::Composition((row.flags >> 4) & 3);
        auto value = Schema::affinities(Schema::Kind(row.kind));
        if (mode != Schema::Composition::None)
        {
            value = mode == Schema::Composition::AllOf ? Schema::AllAffinities : 0;
            for (auto child : row.alternatives.in(edges))
                value =
                    mode == Schema::Composition::AllOf ? value & bits[child] : value | bits[child];
        }
        if (row.flags & 2)
            value = row.flags & 4 ? value | valueTypeAffinity(ValueType::Null)
                                  : value & ~valueTypeAffinity(ValueType::Null);
        if (value == bits[id])
            continue;
        bits[id] = value;
        for (auto parent : parents[id])
            if (!queued[parent])
            {
                queued[parent] = true;
                pending.push_back(parent);
            }
    }
    for (size_t id = 1; id < domains.size(); ++id)
        domains[id].kind = uint32_t(
            Schema::makeKind(Schema::kindNameId(Schema::Kind(domains[id].kind)), bits[id]));
    std::vector<Draft>{}.swap(drafts);
    textIndex.clear();
    textIndex.rehash(0);
}

void SchemaPool::Impl::validate(size_t literalBudget) const
{
    if (domains.empty() || domains.size() > size_t(MaxSchemaId) + 1)
        throw std::length_error("SchemaPool domain column exceeds its namespace");
    auto name = [&](StringId id)
    {
        if (!pool.strings()->resolve(id))
            throw std::invalid_argument("Unknown schema dictionary identity");
    };
    auto reference = [&](SchemaId id)
    {
        if (id >= domains.size())
            throw std::invalid_argument("Dangling schema domain reference");
    };
    auto consume = [](Range range, size_t& end, size_t capacity)
    {
        if (range.begin > capacity || range.count > capacity - range.begin ||
            (range.count && range.begin != end))
            throw std::invalid_argument("Schema slices must be disjoint and canonical");
        end += range.count;
    };
    for (auto child : edges)
        reference(child);
    size_t fieldEnd = 0, edgeEnd = 0, symbolEnd = 0, literalEnd = 0, namedEnd = 0, aliasEnd = 0;
    size_t numericEnd = 0, numberEnd = 0;
    std::unordered_map<uint32_t, size_t> byteSizes;
    for (size_t id = 1; id < domains.size(); ++id)
    {
        auto const& row = domains[id];
        name(Schema::kindNameId(Schema::Kind(row.kind)));
        if (!Schema::kindNameId(Schema::Kind(row.kind)) ||
            Schema::kindNameId(Schema::Kind(row.kind)) >= StringPool::FirstDynamicId ||
            (row.kind & 0xffffu & ~Schema::AllAffinities) || (row.flags & ~0xff37u))
            throw std::invalid_argument("Invalid schema kind or flags");
        text(row.typeName);
        consume(row.fields, fieldEnd, fields.size());
        std::set<StringId> fieldNames;
        for (auto const& field : row.fields.in(fields))
        {
            name(field.name);
            if (!fieldNames.insert(field.name).second || field.required > 2 || field.flags > 3)
                throw std::invalid_argument("Invalid or repeated schema field");
            consume(field.children, edgeEnd, edges.size());
        }
        consume(row.elements, edgeEnd, edges.size());
        consume(row.alternatives, edgeEnd, edges.size());
        consume(row.symbols, symbolEnd, symbols.size());
        consume(row.literals, literalEnd, literals.size());
        consume(row.named, namedEnd, named.size());
        consume(row.aliases, aliasEnd, aliases.size());
        for (auto id : row.symbols.in(symbols))
            name(id);
        for (auto address : row.literals.in(literals))
        {
            // Repeated addresses may reference one large byte value. Preflight
            // the retained copies before index() materializes any of them.
            if (address.column() == ModelPool::ByteArray)
            {
                auto [entry, inserted] = byteSizes.try_emplace(address.value_);
                if (inserted)
                    entry->second = std::get<simfil::ByteArray>(primitive(address)).bytes.size();
                if (entry->second > literalBudget)
                    throw std::length_error(
                        "Schema literal materialization exceeds its byte budget");
                literalBudget -= entry->second;
                continue;
            }
            auto value = primitive(address);
            if (std::holds_alternative<std::string_view>(value))
                throw std::invalid_argument("String choices must use the schema symbol dictionary");
        }
        for (auto const& value : row.named.in(named))
        {
            name(value.name);
            if (!value.name || value.type > 4 || (value.type == 0 && value.bits > 1) ||
                (value.flags & 3) > 2 || ((value.flags >> 2) & 7) > 4 || (value.flags & 128))
                throw std::invalid_argument("Invalid named schema value");
            if (value.type == 4)
                text(value.text);
            else if (value.type != 0)
                number(value);
        }
        std::set<StringId> seenAliases;
        for (auto const& alias : row.aliases.in(aliases))
        {
            name(alias.alias);
            name(alias.canonical);
            if (alias.alias == alias.canonical || !seenAliases.insert(alias.alias).second)
                throw std::invalid_argument("Invalid or duplicate schema alias");
        }
        auto affinity = Schema::affinities(Schema::Kind(row.kind));
        auto mask = valueTypeAffinity(ValueType::Bitmask);
        auto isMask = !(row.flags & 0x30u) && (affinity & mask) &&
                      !(affinity & ~(mask | valueTypeAffinity(ValueType::Null)));
        auto width = uint8_t(row.flags >> 8);
        if (isMask ? !width || width > 64 : width != 0)
            throw std::invalid_argument("Invalid schema bitmask width");
        for (auto const& value : row.named.in(named))
            if ((value.flags & 3) == unsigned(Schema::NamedValue::Role::Flag))
            {
                if ((value.type != 1 && value.type != 2) ||
                    (value.type == 1 && std::bit_cast<int64_t>(value.bits) < 0))
                    throw std::invalid_argument("Flag masks must be nonnegative integers");
                if (width < 64 && (value.bits >> width))
                    throw std::invalid_argument("Flag declaration exceeds its encoding width");
            }
        if (row.numeric > numerics.size())
            throw std::invalid_argument("Unknown numeric domain");
        if (row.numeric && row.numeric != ++numericEnd)
            throw std::invalid_argument("Numeric rows must have one canonical domain owner");
        if (row.base >= domains.size())
            throw std::invalid_argument("Unknown inherited schema");
        if (row.base && (!(affinity & valueTypeAffinity(ValueType::Object)) ||
                         !(Schema::affinities(Schema::Kind(domains[row.base].kind)) &
                           valueTypeAffinity(ValueType::Object))))
            throw std::invalid_argument("Only object schemas can inherit member edges");
        size_t depth = 0;
        for (auto base = row.base; base; base = domains[base].base)
            if (base >= domains.size() || base == id || ++depth > 256)
                throw std::invalid_argument("Cyclic or excessive schema inheritance");
    }
    if (fieldEnd != fields.size() || edgeEnd != edges.size() || symbolEnd != symbols.size() ||
        literalEnd != literals.size() || namedEnd != named.size() || aliasEnd != aliases.size() ||
        numericEnd != numerics.size())
        throw std::invalid_argument("Unreferenced schema column storage");
    for (auto const& row : numerics)
    {
        consume(row.ranges, numberEnd, numbers.size());
        text(row.encoding);
        text(row.unit);
        if (row.flags > 7 || row.reserved || !std::isfinite(row.scale) ||
            !std::isfinite(row.offset) || row.scale == 0)
            throw std::invalid_argument("Invalid numeric interpretation");
        if (row.flags & 1)
            number(row.minimum);
        if (row.flags & 2)
            number(row.maximum);
        auto range = row.ranges.in(numbers);
        if (range.size() % 2)
            throw std::invalid_argument("Incomplete numeric interval");
        for (auto const& value : range)
            number(value);
    }
    if (numberEnd != numbers.size())
        throw std::invalid_argument("Unreferenced numeric interval storage");
}

auto SchemaPool::Impl::nameSet(std::vector<StringId> names, bool sort) -> size_t
{
    if (sort)
    {
        std::ranges::sort(names);
        names.erase(std::unique(names.begin(), names.end()), names.end());
    }
    auto key = std::pair{sort, names};
    if (auto found = setIndex.find(key); found != setIndex.end())
        return found->second;
    auto id = sets.size();
    setIndex.emplace(std::move(key), id);
    NameSet set;
    for (auto name : names)
        set.names.push_back(pool.strings()->resolve(name).value());
    // ID lookup remains numeric; borrowed recursive names are lexical. A direct
    // declaration with the same IDs must retain its independent source order.
    if (sort)
        std::ranges::sort(set.names);
    set.ids = std::move(names);
    sets.push_back(std::move(set));
    return id;
}

void SchemaPool::Impl::index()
{
    auto& work = indexWork;
    views.resize(domains.size());
    nameSet({}, false);
    for (size_t id = 1; id < domains.size(); ++id)
        views[id] = std::make_unique<Domain>(*this, SchemaId(id));
    // Lookup-only inheritance keeps canonical edges shared. A sorted shadow
    // index avoids scanning every local field for every inherited declaration.
    for (size_t id = 1; id < domains.size(); ++id)
        if (domains[id].base && domains[id].fields.count)
        {
            std::vector<StringId> own;
            for (auto const& field : domains[id].fields.in(fields))
            {
                chargeIndexWork();
                own.push_back(field.name);
            }
            views[id]->names[4] = nameSet(std::move(own), true);
        }
    std::vector<std::vector<SchemaId>> children(domains.size()), parents(domains.size());
    std::vector<bool> localComplete(domains.size(), true);
    for (size_t id = 1; id < domains.size(); ++id)
    {
        auto& view = *views[id];
        auto const& row = domains[id];
        std::vector<StringId> direct;
        view.forEachDirectField([&](StringId field, auto) { direct.push_back(field); });
        view.names[0] = nameSet(std::move(direct), false);
        auto directSymbols = row.symbols.in(symbols);
        view.names[2] = nameSet({directSymbols.begin(), directSymbols.end()}, false);
        for (auto const& field : row.fields.in(fields))
            view.contextFields |= bool(field.flags & 2);
        auto addChild = [&](SchemaId child)
        {
            // Inherited edges can multiply a small wire graph. Charge before
            // expanding them, not only after building the SCC adjacency lists.
            chargeIndexWork();
            if (child)
                children[id].push_back(child);
            else
                localComplete[id] = false;
        };
        view.forEachEnumerableField(
            [&](StringId, auto children)
            {
                if (children.empty())
                    localComplete[id] = false;
                for (auto child : children)
                    addChild(child);
            });
        view.forEachElementSchema(addChild);
        for (auto child : view.alternatives())
            addChild(child);
        if (view.open() ||
            Schema::kindNameId(view.kind()) == Schema::kindNameId(Schema::Kind::Unknown) ||
            Schema::kindNameId(view.kind()) == Schema::kindNameId(Schema::Kind::Any) ||
            (view.hasAffinity(ValueType::Array) &&
             view.composition() == Schema::Composition::None && !row.elements.count) ||
            (view.composition() == Schema::Composition::AllOf && !row.alternatives.count))
            localComplete[id] = false;
        auto& next = children[id];
        std::ranges::sort(next);
        next.erase(std::unique(next.begin(), next.end()), next.end());
        for (auto child : next)
            parents[child].push_back(SchemaId(id));
        for (auto address : row.literals.in(literals))
            view.values.push_back(primitive(address));
        for (auto const& alias : row.aliases.in(aliases))
            aliasTargets[alias.alias].push_back(alias.canonical);
        if (row.numeric)
        {
            auto const& n = numerics[row.numeric - 1];
            Schema::NumericDomain value;
            value.encoding = text(n.encoding);
            value.unit = text(n.unit);
            if (n.flags & 1)
                value.minimum = number(n.minimum);
            if (n.flags & 2)
                value.maximum = number(n.maximum);
            auto range = n.ranges.in(numbers);
            for (size_t i = 0; i < range.size(); i += 2)
                view.ranges.push_back({number(range[i]), number(range[i + 1])});
            value.ordinaryRanges = view.ranges;
            value.scale = n.scale;
            value.offset = n.offset;
            value.complete = n.flags & 4;
            view.numeric = value;
        }
    }
    // Iterative SCC condensation computes cycle-correct reachability once and
    // never spends runtime lookup work traversing descendant graphs.
    std::vector<bool> seen(domains.size());
    std::vector<SchemaId> order;
    for (size_t root = 1; root < domains.size(); ++root)
    {
        if (seen[root])
            continue;
        std::vector<std::pair<SchemaId, size_t>> stack{{SchemaId(root), 0}};
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
    std::vector<uint32_t> component(domains.size(), UINT32_MAX);
    std::vector<std::vector<SchemaId>> members;
    for (auto it = order.rbegin(); it != order.rend(); ++it)
    {
        if (component[*it] != UINT32_MAX)
            continue;
        auto c = uint32_t(members.size());
        members.emplace_back();
        std::vector<SchemaId> pending{*it};
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
    for (size_t c = members.size(); c-- > 0;)
    {
        std::vector<StringId> allFields, allSymbols;
        bool complete = true;
        std::set<uint32_t> downstream;
        for (auto id : members[c])
        {
            auto const& view = *views[id];
            auto fields = view.directFields(), symbols = view.directEnumSymbols();
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
            auto const& view = *views[members[child].front()];
            auto fields = view.nestedFields(), symbols = view.nestedEnumSymbols();
            work += fields.size() + symbols.size();
            if (work > 64 * 1024 * 1024)
                throw std::length_error("Schema reachability work budget exceeded");
            allFields.insert(allFields.end(), fields.begin(), fields.end());
            allSymbols.insert(allSymbols.end(), symbols.begin(), symbols.end());
            complete = complete && view.complete;
        }
        auto fields = nameSet(std::move(allFields), true),
             symbols = nameSet(std::move(allSymbols), true);
        for (auto id : members[c])
        {
            views[id]->names[1] = fields;
            views[id]->names[3] = symbols;
            views[id]->complete = complete;
        }
    }
    for (size_t id = 1; id < domains.size(); ++id)
        if (views[id]->composition() != Schema::Composition::None ||
            !views[id]->alternatives().empty())
            views[id]->finalize([&](SchemaId id) -> Schema*
                                { return valid(id) ? views[id].get() : nullptr; });
    // While collecting the combined indexes, roles describe each declaration's
    // own edges; only published lookups use the conservative combined result.
    memberIndexesReady = true;
    setIndex.clear();
    statistics.domains = domains.size() - 1;
    statistics.fields = fields.size();
    statistics.namedValues = named.size();
    statistics.numericDomains = numerics.size();
    for (auto const& field : fields)
    {
        statistics.fieldEdges += field.children.count;
        statistics.contextFields += bool(field.flags & 2);
    }
    for (auto const& row : domains)
    {
        statistics.elementEdges += row.elements.count;
        statistics.alternativeEdges += row.alternatives.count;
    }
}

void SchemaPool::finalizeSchema()
{
    if (schema_->packed)
        return;
    requireMutable();
    if (schema_->packing)
        throw std::logic_error("Cannot reuse a failed schema publication");
    schema_->packing = true;
    schema_->pack();
    schema_->validate();
    schema_->index();
    schema_->packed = true;
}
void SchemaPool::finalize()
{
    finalizeSchema();
    ModelPool::freeze();
}

auto SchemaPool::importGraph(SchemaPool const& source, std::span<SchemaId const> reuse)
    -> std::vector<SchemaId>
{
    requireMutable();
    if (schema_->packed || !source.finalized() || &source == this)
        throw std::invalid_argument(
            "Graph import requires a distinct frozen source and mutable destination");
    auto count = source.schema_->domains.size();
    if (!reuse.empty() && reuse.size() != count)
        throw std::invalid_argument("Schema reuse table has the wrong namespace size");
    std::vector<SchemaId> remap(count);
    size_t additional = count - 1;
    for (size_t id = 1; id < count; ++id)
    {
        auto name = Schema::kindNameId(source.domain(SchemaId(id))->kind());
        if (strings()->resolve(name) != source.strings()->resolve(name))
            throw std::invalid_argument("Schema kind names require the same static vocabulary");
        if (!reuse.empty() && reuse[id])
        {
            if (reuse[id] >= schema_->drafts.size())
                throw std::invalid_argument("Reused domain is absent from the destination graph");
            remap[id] = reuse[id];
            --additional;
        }
    }
    if (additional > size_t(MaxSchemaId) + 1 - schema_->drafts.size())
        throw std::length_error("Imported graph exhausts the uint16 domain namespace");
    // Allocate every identity first so cycles and sibling sharing remain exact.
    for (size_t id = 1; id < count; ++id)
        if (!remap[id])
            remap[id] = addSchema(source.domain(SchemaId(id))->kind());
    for (size_t id = 1; id < count; ++id)
    {
        if (!reuse.empty() && reuse[id])
            continue;
        auto target = remap[id];
        auto const& row = source.schema_->domains[id];
        auto view = source.domain(SchemaId(id));
        setTypeName(target, view->typeName());
        setOpen(target, view->open());
        setNullable(target, view->nullable());
        setComposition(target, view->composition());
        setBaseSchema(target, remap[row.base]);
        for (auto const& field : row.fields.in(source.schema_->fields))
        {
            auto name = source.strings()->resolve(field.name).value();
            auto children = field.children.in(source.schema_->edges);
            if (children.empty())
                addFieldSchema(target, name, NoSchemaId, field.flags & 1);
            else
                for (auto child : children)
                    addFieldSchema(target, name, remap[child], field.flags & 1);
            setFieldRequired(target, name,
                             field.required ? std::optional<bool>(field.required == 2)
                                            : std::nullopt);
            setFieldRole(target, name,
                         field.flags & 2 ? Schema::FieldRole::Context : Schema::FieldRole::Data);
        }
        for (auto child : row.elements.in(source.schema_->edges))
            addElementSchema(target, remap[child]);
        for (auto child : row.alternatives.in(source.schema_->edges))
            addAlternative(target, remap[child]);
        for (auto name : row.symbols.in(source.schema_->symbols))
            addEnumSymbol(target, source.strings()->resolve(name).value());
        for (auto const& value : view->enumValues())
            addEnumValue(target, value);
        if (auto numeric = view->numericDomain())
            setNumericDomain(target, *numeric);
        view->forEachNamedValue([&](auto const& value) { addNamedValue(target, value); });
        if (view->isBitmask())
            setBitWidth(target, view->bitWidth());
        for (auto const& alias : row.aliases.in(source.schema_->aliases))
            addFieldAlias(target, source.strings()->resolve(alias.alias).value(),
                          source.strings()->resolve(alias.canonical).value());
    }
    return remap;
}

template <class S, class T, class F>
void SchemaPool::Impl::container(S& archive, std::vector<T>& values, size_t limit, size_t& memory,
                                 F&& item)
{
    if constexpr (detail::bitsery_input_archive<S>)
    {
        auto position = archive.adapter().currentReadPos();
        size_t count = 0;
        bitsery::details::readSize(archive.adapter(), count, limit, std::true_type{});
        if (count > memory / sizeof(T) ||
            !detail::check_bitsery_payload_available(archive.adapter(), count))
            throw std::length_error("Schema container exceeds its input/allocation budget");
        memory -= count * sizeof(T);
        archive.adapter().currentReadPos(position);
    }
    archive.container(values, limit, std::forward<F>(item));
}
template <class S>
void SchemaPool::Impl::string(S& archive, std::string& value, size_t limit, size_t& memory)
{
    if constexpr (detail::bitsery_input_archive<S>)
    {
        auto position = archive.adapter().currentReadPos();
        size_t count = 0;
        bitsery::details::readSize(archive.adapter(), count, limit, std::true_type{});
        if (count > memory || !detail::check_bitsery_payload_available(archive.adapter(), count))
            throw std::length_error("Schema text exceeds its input/allocation budget");
        memory -= count;
        archive.adapter().currentReadPos(position);
    }
    archive.text1b(value, limit);
}
template <class S> void SchemaPool::Impl::readWrite(S& archive, size_t budget)
{
    size_t memory = budget > SIZE_MAX / 4 ? SIZE_MAX : budget * 4;
    uint32_t version = CodecVersion;
    archive.value4b(version);
    if (version != CodecVersion)
        throw std::invalid_argument("Unsupported generic schema payload version");
    if constexpr (detail::bitsery_input_archive<S>)
    {
        uint64_t length = 0;
        archive.value8b(length);
        if (length > (size_t(MaxSchemaId) + 1) * sizeof(DomainData) || length > budget ||
            length > memory || length % sizeof(DomainData))
            throw std::length_error("Schema domain column exceeds its namespace/byte budget");
        memory -= length;
        if (!domains.read_payload_from_bitsery(archive.adapter(), size_t(length)))
            throw std::invalid_argument("Truncated schema domain column");
    }
    else
        archive.object(domains);
    auto vector = [&]<class T>(std::vector<T>& values)
    {
        container(archive, values, std::min(MaxEdges, budget / sizeof(T)), memory,
                  [](auto& archive, T& value)
                  {
                      if constexpr (std::is_same_v<T, SchemaId>)
                          archive.value2b(value);
                      else if constexpr (std::is_same_v<T, ModelNodeAddress>)
                          archive.value4b(value.value_);
                      else
                          archive.object(value);
                  });
    };
    vector(fields);
    vector(edges);
    vector(symbols);
    vector(literals);
    vector(named);
    vector(numbers);
    vector(numerics);
    vector(aliases);
}

void SchemaPool::writeSchemaPayload(std::ostream& output) const
{
    if (!finalized())
        throw std::logic_error("Cannot serialize an unfinished schema graph");
    bitsery::Serializer<bitsery::OutputStreamAdapter> archive(output);
    const_cast<Impl&>(*schema_).readWrite(archive, UINT32_MAX);
    std::ostringstream dictionary(std::ios::binary), model(std::ios::binary);
    // An application dictionary may override its transport envelope. This payload
    // owns the generic dictionary framing, not the enclosing application's stream.
    if (auto result = strings()->StringPool::write(dictionary); !result)
        throw std::runtime_error(result.error().message);
    if (auto result = const_cast<SchemaPool*>(this)->ModelPool::write(model); !result)
        throw std::runtime_error(result.error().message);
    auto names = dictionary.str(), values = model.str();
    archive.text1b(names, UINT32_MAX);
    archive.text1b(values, UINT32_MAX);
    archive.adapter().flush();
    if (!output)
        throw std::runtime_error("Failed to write schema graph payload");
}

void SchemaPool::readSchemaPayload(std::span<uint8_t const> bytes, size_t maxBytes)
{
    requireMutable();
    if (schema_->packed || schema_->drafts.size() != 1 || bytes.size() > maxBytes)
        throw std::invalid_argument("Schema payload requires an empty owner and bounded input");
    std::vector<uint8_t> buffer(bytes.begin(), bytes.end());
    using Adapter = bitsery::InputBufferAdapter<std::vector<uint8_t>>;
    bitsery::Deserializer<Adapter> archive(Adapter(buffer.begin(), buffer.end()));
    schema_->readWrite(archive, maxBytes);
    size_t memory = maxBytes;
    std::string dictionary, model;
    Impl::string(archive, dictionary, maxBytes, memory);
    Impl::string(archive, model, maxBytes, memory);
    if (archive.adapter().error() != bitsery::ReaderError::NoError ||
        !archive.adapter().isCompletedSuccessfully())
        throw std::invalid_argument("Malformed, truncated or trailing schema payload");
    if (auto result =
            strings()->StringPool::read(std::vector<uint8_t>(dictionary.begin(), dictionary.end()));
        !result)
        throw std::invalid_argument(result.error().message);
    if (auto result = ModelPool::read(std::vector<uint8_t>(model.begin(), model.end())); !result)
        throw std::invalid_argument(result.error().message);
    schema_->validate(maxBytes);
    schema_->index();
    std::vector<Impl::Draft>{}.swap(schema_->drafts);
    schema_->packed = true;
}

auto SchemaPool::write(std::ostream& output) -> tl::expected<void, Error>
{
    return std::as_const(*this).write(output);
}
auto SchemaPool::write(std::ostream& output) const -> tl::expected<void, Error>
{
    try
    {
        writeSchemaPayload(output);
        return {};
    }
    catch (std::exception const& error)
    {
        return tl::unexpected(Error{Error::EncodeDecodeError, error.what()});
    }
}
auto SchemaPool::read(std::span<uint8_t const> input, size_t maxBytes) -> ConstPtr
{
    auto result = std::make_shared<SchemaPool>();
    result->readSchemaPayload(input, maxBytes);
    result->ModelPool::freeze();
    return result;
}

auto SchemaPool::schemaMemoryUsage() const -> MemoryUsage
{
    MemoryUsage result{sizeof(Impl), sizeof(Impl)};
    auto vector = [&]<class T>(std::vector<T> const& values) {
        result += MemoryUsage{values.size() * sizeof(T), values.capacity() * sizeof(T)};
    };
    result += schema_->domains.memory_usage();
    vector(schema_->fields);
    vector(schema_->edges);
    vector(schema_->symbols);
    vector(schema_->literals);
    vector(schema_->named);
    vector(schema_->numbers);
    vector(schema_->numerics);
    vector(schema_->aliases);
    vector(schema_->views);
    vector(schema_->sets);
    for (auto const& set : schema_->sets)
    {
        vector(set.ids);
        vector(set.names);
    }
    for (auto const& view : schema_->views)
        if (view)
        {
            result += MemoryUsage{sizeof(Impl::Domain), sizeof(Impl::Domain)};
            result += view->indexMemory();
            vector(view->values);
            vector(view->ranges);
            for (auto const& value : view->values)
                if (auto bytes = std::get_if<simfil::ByteArray>(&value))
                {
                    result.logicalBytes += bytes->bytes.size();
                    auto object = reinterpret_cast<uintptr_t>(&bytes->bytes);
                    auto data = reinterpret_cast<uintptr_t>(bytes->bytes.data());
                    // Small-string storage is already charged inside the variant.
                    if (data < object || data >= object + sizeof(bytes->bytes))
                        result.allocatedBytes += bytes->bytes.capacity() + 1;
                }
        }
    for (auto const& [_, targets] : schema_->aliasTargets)
    {
        result +=
            MemoryUsage{sizeof(decltype(schema_->aliasTargets)::value_type),
                        sizeof(decltype(schema_->aliasTargets)::value_type) + 3 * sizeof(void*)};
        vector(targets);
    }
    return result;
}

} // namespace simfil
