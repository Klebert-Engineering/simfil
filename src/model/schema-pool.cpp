#include "simfil/model/schema-pool.h"
#include "simfil/model/schema-node.h"
#include "schema-pool-impl.h"

#include <bit>
#include <cmath>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

#include <bitsery/adapter/buffer.h>
#include <bitsery/adapter/stream.h>
#include <bitsery/traits/string.h>
#include "simfil/model/bitsery-traits.h"

namespace simfil
{

SchemaPool::SchemaPool(std::shared_ptr<StringPool> strings)
    : ModelPool(strings ? std::move(strings) : throw std::invalid_argument("A schema requires a dictionary")),
      schema_(std::make_unique<Impl>(*this))
{
}

SchemaPool::~SchemaPool() = default;


auto SchemaPool::dictionary() const noexcept -> StringPool const& { return schema_->dictionary; }
auto SchemaPool::identity() const noexcept -> std::string_view { return schema_->identity; }

auto SchemaPool::setStrings(std::shared_ptr<StringPool> const& strings) -> tl::expected<void, Error>
{
    if (strings.get() != &schema_->dictionary)
        return tl::unexpected(Error{Error::RuntimeError, "A SchemaPool dictionary cannot be rebound"});
    return {};
}

void SchemaPool::setIdentity(std::string identity)
{
    requireMutable();
    if (identity.size() > 65535)
        throw std::length_error("Schema identity exceeds its transport limit");
    if (!finalized() || identity.empty() || !schema_->identity.empty())
        throw std::logic_error("Artifact identity requires a prepared graph and one nonempty publication");
    schema_->identity = std::move(identity);
}

void SchemaPool::appendExtension(std::function<void()> const& extension)
{
    requireMutable();
    if (schema_->packed || schema_->packing)
        throw std::logic_error("Cannot append metadata after schema publication");
    Impl::AppendRollback rollback(*schema_);
    extension();
    rollback.committed = true;
}

auto SchemaPool::appendFieldWithExtension(
    TypeId parent, std::string_view name, std::function<SchemaType()> const& extension,
    bool multimap, std::optional<bool> required, SchemaType::FieldRole role) -> SchemaField
{
    auto original = schema_->mutableType(parent);
    // A surrounding append rollback cannot restore a published pre-existing row.
    if (schema_->activeAppends)
        throw std::logic_error("Atomic field publication cannot nest in an append transaction");
    auto type = schemaType(parent);
    if (!type.hasAffinity(ValueType::Object) && !type.hasAffinity(ValueType::Array))
        throw std::invalid_argument("A field requires container affinity");
    if (unsigned(role) > unsigned(SchemaType::FieldRole::Context))
        throw std::invalid_argument("Invalid schema field role");
    validateFieldMutation(parent, name);
    if (type.field(name))
        throw std::invalid_argument("Atomic field publication requires a new field");

    Impl::AppendRollback rollback(*schema_);
    auto key = schema_->intern(name);
    auto child = extension();
    if (child && (child.pool() != this || !schema_->valid(child.id())))
        throw std::invalid_argument("A field target must belong to the same schema pool");

    Impl::FieldData field{.name = key, .owner = parent,
        .required = uint8_t(required ? (*required ? 2 : 1) : 0),
        .flags = uint8_t(uint8_t(multimap) | (role == SchemaType::FieldRole::Context ? 2 : 0))};
    schema_->append(schema_->edges, field.children, child.id());
    ArrayIndex members = 0;
    // Reading by index avoids retaining an iterator while growing the same arena.
    for (size_t i = 0, count = schema_->fields.size(original.fields); i < count; ++i)
    {
        auto record = schema_->fields.at(original.fields, i).value().get();
        schema_->append(schema_->fields, members, record);
    }
    schema_->appendRecord(schema_->fieldRows, schema_->fields, members, field);
    SchemaField result(type, key);

    // Publish only after callbacks and all allocations succeed. The old member
    // records remain shared by the replacement membership, not copied or retired.
    schema_->types[parent].fields = members;
    if (original.fields)
        schema_->fields.truncate(original.fields, 0);
    rollback.committed = true;
    return result;
}

auto SchemaPool::addSchema(SchemaType::Kind kind) -> TypeId
{
    requireMutable();
    if (schema_->packed || schema_->packing)
        throw std::logic_error("SchemaPool is immutable after finalization");
    if (schema_->types.size() > MaxTypeId)
        throw std::length_error("SchemaPool exhausted its uint16 type namespace");
    Impl::TypeData row;
    row.kind = uint32_t(kind);
    auto affinity = SchemaType::affinities(kind);
    auto mask = valueTypeAffinity(ValueType::Bitmask);
    if ((affinity & mask) && !(affinity & ~(mask | valueTypeAffinity(ValueType::Null))))
        row.flags |= 64u << 8;
    auto id = TypeId(schema_->types.size());
    schema_->types.push_back(row);
    return id;
}

void SchemaPool::addFieldSchema(TypeId parent, std::string_view name, TypeId child, bool multimap)
{
    schema_->mutableType(parent);
    validateFieldMutation(parent, name);
    auto& field = schema_->field(parent, name);
    schema_->append(schema_->edges, field.children, child);
    field.flags |= uint8_t(multimap);
}

void SchemaPool::setFieldRequired(TypeId id, std::string_view name, std::optional<bool> required)
{
    schema_->mutableType(id);
    validateFieldMutation(id, name);
    schema_->field(id, name).required = required ? (*required ? 2 : 1) : 0;
}

void SchemaPool::setFieldRole(TypeId id, std::string_view name, SchemaType::FieldRole role)
{
    schema_->mutableType(id);
    if (unsigned(role) > unsigned(SchemaType::FieldRole::Context))
        throw std::invalid_argument("Invalid schema field role");
    validateFieldMutation(id, name);
    auto& field = schema_->field(id, name);
    field.flags = (field.flags & ~uint8_t{2}) | (role == SchemaType::FieldRole::Context ? 2 : 0);
}

void SchemaPool::addFieldAlias(TypeId id, std::string_view alias, std::string_view canonical)
{
    auto& row = schema_->mutableType(id);
    validateFieldMutation(id, alias);
    validateFieldMutation(id, canonical);
    schema_->append(schema_->aliases, row.aliases,
                    Impl::AliasData{schema_->intern(alias), schema_->intern(canonical)});
}

void SchemaPool::setBaseSchema(TypeId id, TypeId base)
{
    auto& row = schema_->mutableType(id);
    validateStructureMutation(id);
    row.base = base;
}

void SchemaPool::addElementSchema(TypeId id, TypeId child)
{
    auto& row = schema_->mutableType(id);
    validateStructureMutation(id);
    schema_->append(schema_->edges, row.elements, child);
}

void SchemaPool::addAlternative(TypeId id, TypeId child)
{
    auto& row = schema_->mutableType(id);
    validateStructureMutation(id);
    schema_->append(schema_->edges, row.alternatives, child);
}

void SchemaPool::setComposition(TypeId id, SchemaType::Composition composition)
{
    auto& row = schema_->mutableType(id);
    validateStructureMutation(id);
    if (unsigned(composition) > unsigned(SchemaType::Composition::AllOf))
        throw std::invalid_argument("Invalid schema composition");
    row.flags = (row.flags & ~0x30u) | (uint32_t(composition) << 4);
}

void SchemaPool::setTypeName(TypeId id, std::string_view name)
{
    schema_->mutableType(id).typeName = schema_->text(name);
}

void SchemaPool::setOpen(TypeId id, bool open)
{
    auto& row = schema_->mutableType(id);
    row.flags = (row.flags & ~1u) | uint32_t(open);
}

void SchemaPool::setNullable(TypeId id, std::optional<bool> nullable)
{
    auto& row = schema_->mutableType(id);
    row.flags = (row.flags & ~6u) | (nullable ? 2u | (uint32_t(*nullable) << 2) : 0u);
}

void SchemaPool::addEnumSymbol(TypeId id, std::string_view symbol)
{
    auto& row = schema_->mutableType(id);
    schema_->append(schema_->symbols, row.symbols, schema_->intern(symbol));
}

void SchemaPool::addEnumValue(TypeId id, SchemaType::EnumValue const& value)
{
    auto& row = schema_->mutableType(id);
    schema_->append(schema_->literals, row.literals, schema_->literal(value));
}

void SchemaPool::setNumericDomain(TypeId id, SchemaType::NumericDomain const& value)
{
    setTypeNumericDomain(id, value);
}

void SchemaPool::setTypeNumericDomain(TypeId id, SchemaType::NumericDomain const& value)
{
    applyScalarAnnotations(id, value, std::nullopt, {}, {});
}

void SchemaPool::addNamedValue(TypeId id, SchemaType::NamedValue const& value)
{
    auto& type = schema_->mutableType(id);
    schema_->appendRecord(schema_->namedRows, schema_->named, type.named, schema_->encodeNamed(value));
}


void SchemaPool::setBitWidth(TypeId id, uint8_t width)
{
    auto& row = schema_->mutableType(id);
    auto bits = SchemaType::affinities(SchemaType::Kind(row.kind));
    if (!width || width > 64 || !(bits & valueTypeAffinity(ValueType::Bitmask)) ||
        (bits & ~(valueTypeAffinity(ValueType::Bitmask) | valueTypeAffinity(ValueType::Null))))
        throw std::invalid_argument("Bit width requires a concrete bitmask type");
    row.flags = (row.flags & ~0xff00u) | (uint32_t(width) << 8);
}

auto SchemaPool::finalized() const -> bool
{
    return schema_->packed;
}

auto SchemaPool::typeCount() const -> size_t
{
    if (schema_->packing && !schema_->packed)
        throw std::logic_error("Schema publication failed");
    return schema_->types.size() - 1;
}

auto SchemaPool::schemaStatistics() const -> Statistics
{
    return schema_->statistics;
}

auto SchemaPool::typeNode(TypeId id) const -> ModelNode::Ptr
{
    if (!finalized() || !schema_->valid(id))
        throw std::invalid_argument("Descriptor requested for an unknown type");
    return Model::resolve(ModelNodeAddress(SchemaTypeModelNode::Type, id));
}

auto SchemaPool::Impl::decodeNamed(NamedData const& row) const -> SchemaType::NamedValue
{
    SchemaType::NamedValue value;
    value.name = dictionary.resolve(row.name).value();
    value.role = SchemaType::NamedValue::Role(row.flags & 3);
    value.meaning = SchemaType::NamedValue::Meaning((row.flags >> 2) & 7);
    value.deprecated = row.flags & 32;
    value.removed = row.flags & 64;
    if (row.rawType == 1)
        value.rawValue = std::bit_cast<int64_t>(row.rawBits);
    else if (row.rawType == 2)
        value.rawValue = row.rawBits;
    switch (row.type)
    {
    case 0: value.value = bool(row.bits); break;
    case 1: value.value = std::bit_cast<int64_t>(row.bits); break;
    case 2: value.value = row.bits; break;
    case 3: value.value = std::bit_cast<double>(row.bits); break;
    case 4: value.value = text(row.text); break;
    default: throw std::invalid_argument("Invalid schema named value type");
    }
    return value;
}

auto SchemaPool::Impl::encodeNamed(SchemaType::NamedValue const& value) -> NamedData
{
    auto primitive = std::visit([&](auto v) { return scalar(v); }, value.value);
    NamedData row{.bits = primitive.bits, .text = primitive.text, .type = primitive.type};
    row.name = intern(value.name);
    row.flags = uint8_t(value.role) | (uint8_t(value.meaning) << 2) |
                (value.deprecated ? 32u : 0u) | (value.removed ? 64u : 0u);
    if (value.rawValue)
        std::visit([&](auto raw) {
            row.rawType = std::is_same_v<decltype(raw), int64_t> ? 1 : 2;
            row.rawBits = std::bit_cast<uint64_t>(raw);
        }, *value.rawValue);
    return row;
}

void SchemaPool::Impl::copyDirectType(TypeId target, SchemaPool const& source,
                                     TypeId sourceId, std::span<TypeId const> remap)
{
    auto const& origin = *source.schema_;
    // Same-pool copies may grow every backing column. Handles and record copies
    // survive that growth; borrowed range iterators and record references do not.
    auto row = origin.types[sourceId];
    auto view = source.schemaType(sourceId);
    auto mapped = [&](TypeId id) { return remap.empty() ? id : remap[id]; };
    pool.setTypeName(target, view.typeName());
    pool.setOpen(target, view.open());
    pool.setNullable(target, view.nullable());
    pool.setComposition(target, view.composition());
    pool.setBaseSchema(target, mapped(TypeId(row.base)));
    for (size_t i = 0, count = origin.fields.size(row.fields); i < count; ++i)
    {
        auto field = origin.fieldRows[origin.fields.at(row.fields, i).value().get()];
        auto name = source.dictionary().resolve(field.name).value();
        for (size_t j = 0, children = origin.edges.size(field.children); j < children; ++j)
        {
            auto child = origin.edges.at(field.children, j).value().get();
            pool.addFieldSchema(target, name, mapped(child), field.flags & 1);
        }
        pool.setFieldRequired(target, name,
            field.required ? std::optional<bool>(field.required == 2) : std::nullopt);
        pool.setFieldMultimap(target, name, field.flags & 1);
        pool.setFieldRole(target, name,
            field.flags & 2 ? SchemaType::FieldRole::Context : SchemaType::FieldRole::Data);
    }
    for (size_t i = 0, count = origin.edges.size(row.elements); i < count; ++i)
        pool.addElementSchema(target, mapped(origin.edges.at(row.elements, i).value().get()));
    for (size_t i = 0, count = origin.edges.size(row.alternatives); i < count; ++i)
        pool.addAlternative(target, mapped(origin.edges.at(row.alternatives, i).value().get()));
    for (size_t i = 0, count = origin.symbols.size(row.symbols); i < count; ++i)
        pool.addEnumSymbol(target, source.dictionary().resolve(origin.symbols.at(row.symbols, i).value().get()).value());
    for (size_t i = 0, count = origin.literals.size(row.literals); i < count; ++i)
        pool.addEnumValue(target, origin.enumValue(origin.literals.at(row.literals, i).value().get()));
    if (auto numeric = view.numericDomain())
        pool.setTypeNumericDomain(target, *numeric);
    for (size_t i = 0, count = origin.named.size(row.named); i < count; ++i)
    {
        auto value = origin.namedRows[origin.named.at(row.named, i).value().get()];
        pool.addNamedValue(target, origin.decodeNamed(value));
    }
    if (view.isBitmask())
        pool.setBitWidth(target, view.bitWidth());
    for (size_t i = 0, count = origin.aliases.size(row.aliases); i < count; ++i)
    {
        auto alias = origin.aliases.at(row.aliases, i).value().get();
        pool.addFieldAlias(target, source.dictionary().resolve(alias.alias).value(),
                           source.dictionary().resolve(alias.canonical).value());
    }
}

void SchemaPool::Impl::prepareAffinities()
{
    // Resolve logical affinity by a descending fixed point. A recursive union
    // starts conservative; each bit can disappear at most once per type.
    std::vector<std::vector<TypeId>> parents(types.size());
    std::vector<uint16_t> bits(types.size(), SchemaType::AllAffinities);
    std::vector<bool> queued(types.size(), true);
    std::vector<TypeId> pending;
    for (size_t id = 1; id < types.size(); ++id)
    {
        auto const& row = types[id];
        if (!(row.flags & 0x30u))
            bits[id] = SchemaType::affinities(SchemaType::Kind(row.kind));
        for (auto child : edges.range(row.alternatives))
        {
            if (child >= types.size())
                throw std::invalid_argument("Dangling alternative schema");
            parents[child].push_back(TypeId(id));
        }
        pending.push_back(TypeId(id));
    }
    while (!pending.empty())
    {
        auto id = pending.back();
        pending.pop_back();
        queued[id] = false;
        auto& row = types[id];
        auto mode = SchemaType::Composition((row.flags >> 4) & 3);
        auto value = SchemaType::affinities(SchemaType::Kind(row.kind));
        if (mode != SchemaType::Composition::None)
        {
            value = mode == SchemaType::Composition::AllOf ? SchemaType::AllAffinities : 0;
            for (auto child : edges.range(row.alternatives))
                value =
                    mode == SchemaType::Composition::AllOf ? value & bits[child] : value | bits[child];
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
    for (size_t id = 1; id < types.size(); ++id)
        types[id].kind = uint32_t(
            SchemaType::makeKind(SchemaType::kindNameId(SchemaType::Kind(types[id].kind)), bits[id]));
    textIndex.clear();
    textIndex.rehash(0);
}

void SchemaPool::Impl::validate(size_t literalBudget) const
{
    if (types.empty() || types.size() > size_t(MaxTypeId) + 1)
        throw std::length_error("SchemaPool type column exceeds its namespace");
    auto name = [&](StringId id)
    {
        if (!dictionary.resolve(id))
            throw std::invalid_argument("Unknown schema dictionary identity");
    };
    auto reference = [&](TypeId id)
    {
        if (id >= types.size())
            throw std::invalid_argument("Dangling schema type reference");
    };
    std::map<void const*, std::set<ArrayIndex>> consumed;
    size_t totalEdges = 0;
    auto consume = [&]<class T>(ArrayIndex handle, ArrayArena<T> const& arena)
    {
        if (!arena.valid(handle) || !arena.valid(0) || arena.size(0))
            throw std::invalid_argument("Invalid schema array handle");
        if (handle && !consumed[&arena].insert(handle).second)
            throw std::invalid_argument("Schema arrays must have one canonical owner");
        auto count = arena.size(handle);
        if (count > MaxEdges - totalEdges)
            throw std::length_error("Schema edge budget exceeded");
        totalEdges += count;
    };
    auto childArray = [&](ArrayIndex handle)
    {
        consume(handle, edges);
        for (auto child : edges.range(handle))
            reference(child);
    };
    std::set<uint32_t> numericOwners;
    std::vector<bool> usedFields(fieldRows.size()), usedNamed(namedRows.size()), usedIntervals(intervalRows.size());
    std::unordered_map<uint32_t, size_t> byteSizes;

    // Validate each declaration and its owned collections before following any
    // inherited or derived lookup. Shared targets are allowed; shared metadata
    // rows/array handles would make mutation and byte accounting ambiguous.
    for (size_t id = 1; id < types.size(); ++id)
    {
        auto const& row = types[id];
        name(SchemaType::kindNameId(SchemaType::Kind(row.kind)));
        if (!SchemaType::kindNameId(SchemaType::Kind(row.kind)) ||
            SchemaType::kindNameId(SchemaType::Kind(row.kind)) >= StringPool::FirstDynamicId ||
            (row.kind & 0xffffu & ~SchemaType::AllAffinities) || (row.flags & ~0xff3fu))
            throw std::invalid_argument("Invalid schema kind or flags");
        text(row.typeName);
        consume(row.fields, fields);
        for (auto index : fields.range(row.fields))
        {
            if (index >= fieldRows.size() || usedFields[index])
                throw std::invalid_argument("Invalid or shared schema field record");
            usedFields[index] = true;
        }
        std::set<StringId> fieldNames;
        for (auto const& field : fieldRecords(row.fields))
        {
            name(field.name);
            if (!fieldNames.insert(field.name).second || field.required > 2 || field.flags > 3 ||
                field.owner != id || field.reserved)
                throw std::invalid_argument("Invalid or repeated schema field");
            childArray(field.children);
        }
        childArray(row.elements);
        childArray(row.alternatives);
        consume(row.symbols, symbols);
        consume(row.literals, literals);
        consume(row.named, named);
        for (auto index : named.range(row.named))
        {
            if (index >= namedRows.size() || usedNamed[index])
                throw std::invalid_argument("Invalid or shared named schema record");
            usedNamed[index] = true;
        }
        consume(row.aliases, aliases);
        for (auto id : symbols.range(row.symbols))
            name(id);
        for (auto address : literals.range(row.literals))
        {
            // Bound untrusted repeated byte literals before resolving their values.
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
        for (auto const& value : namedRecords(row.named))
        {
            name(value.name);
            if (!value.name || value.type > 4 || (value.type == 0 && value.bits > 1) ||
                (value.flags & 3) > 2 || ((value.flags >> 2) & 7) > 4 || (value.flags & 128))
                throw std::invalid_argument("Invalid named schema value");
            // A missing ordinal must have zero storage; zero itself is a present tagged value.
            if (value.rawType > 2 || (!value.rawType && value.rawBits) ||
                std::ranges::any_of(value.reserved, [](uint8_t byte) { return byte != 0; }))
                throw std::invalid_argument("Invalid named schema raw value");
            if (value.type == 4)
                text(value.text);
            else if (value.type != 0)
                number(ScalarData{.bits=value.bits, .text=value.text, .type=value.type});
        }
        std::set<StringId> seenAliases;
        for (auto const& alias : aliases.range(row.aliases))
        {
            name(alias.alias);
            name(alias.canonical);
            if (alias.alias == alias.canonical || !seenAliases.insert(alias.alias).second)
                throw std::invalid_argument("Invalid or duplicate schema alias");
        }

        // The packed width is meaningful only for a concrete bitmask. A union
        // that can contain a mask does not itself define one physical encoding.
        auto affinity = SchemaType::affinities(SchemaType::Kind(row.kind));
        auto mask = valueTypeAffinity(ValueType::Bitmask);
        auto isMask = !(row.flags & 0x30u) && (affinity & mask) &&
                      !(affinity & ~(mask | valueTypeAffinity(ValueType::Null)));
        auto width = uint8_t(row.flags >> 8);
        if (isMask ? !width || width > 64 : width != 0)
            throw std::invalid_argument("Invalid schema bitmask width");
        for (auto const& value : namedRecords(row.named))
            if ((value.flags & 3) == unsigned(SchemaType::NamedValue::Role::Flag))
            {
                if ((value.type != 1 && value.type != 2) ||
                    (value.type == 1 && std::bit_cast<int64_t>(value.bits) < 0))
                    throw std::invalid_argument("Flag masks must be nonnegative integers");
                if (width < 64 && (value.bits >> width))
                    throw std::invalid_argument("Flag declaration exceeds its encoding width");
            }
        if (row.numeric > numerics.size())
            throw std::invalid_argument("Unknown numeric domain");
        if (row.numeric && !numericOwners.insert(row.numeric).second)
            throw std::invalid_argument("Numeric rows must have one canonical type owner");
        if (row.base >= types.size())
            throw std::invalid_argument("Unknown inherited schema");
        if (row.base && (!(affinity & valueTypeAffinity(ValueType::Object)) ||
                         !(SchemaType::affinities(SchemaType::Kind(types[row.base].kind)) &
                           valueTypeAffinity(ValueType::Object))))
            throw std::invalid_argument("Only object schemas can inherit member edges");
        size_t depth = 0;
        for (auto base = row.base; base; base = types[base].base)
            if (base >= types.size() || base == id || ++depth > 256)
                throw std::invalid_argument("Cyclic or excessive schema inheritance");
    }
    // Every numeric row has exactly one type owner. Check its interval payloads
    // separately, since these live in stable descriptor-addressable columns.
    if (numericOwners.size() != numerics.size())
        throw std::invalid_argument("Unreferenced numeric domain storage");
    for (auto const& row : numerics)
    {
        consume(row.ranges, numbers);
        text(row.encoding);
        text(row.unit);
        if (row.flags > 7 || !std::isfinite(row.scale) ||
            !std::isfinite(row.offset) || row.scale == 0)
            throw std::invalid_argument("Invalid numeric interpretation");
        if (row.flags & 1)
            number(row.minimum);
        if (row.flags & 2)
            number(row.maximum);
        for (auto index : numbers.range(row.ranges))
        {
            if (index >= intervalRows.size() || usedIntervals[index])
                throw std::invalid_argument("Invalid or shared schema interval record");
            usedIntervals[index] = true;
            number(intervalRows[index].minimum);
            number(intervalRows[index].maximum);
        }
    }
    // Replaced arrays may retain empty capacity, but unreachable live metadata
    // must not survive publication or hide outside the validated graph.
    if (std::ranges::find(usedFields, false) != usedFields.end() ||
        std::ranges::find(usedNamed, false) != usedNamed.end())
        throw std::invalid_argument("Unreferenced schema record storage");
    for (size_t i = 0; i < intervalRows.size(); ++i)
        if (!usedIntervals[i])
        {
            auto empty = [](ScalarData const& value) {
                return !value.bits && !value.text && !value.name && !value.type && !value.flags;
            };
            auto const& row = intervalRows[i];
            if (!empty(row.minimum) || !empty(row.maximum))
                throw std::invalid_argument("Retired schema intervals must have no payload");
        }
    auto allReferenced = [&]<class T>(ArrayArena<T> const& arena)
    {
        if (!arena.valid(0) || arena.size(0))
            throw std::invalid_argument("Schema arena lacks its empty reserved array");
        auto const& handles = consumed[&arena];
        // Metadata replacement retires its former arrays without relocating
        // other handles. Empty retained slots are capacity, not hidden graph data.
        for (ArrayIndex id = 1; id < arena.size(); ++id)
            if (arena.size(id) && !handles.contains(id))
                throw std::invalid_argument("Unreferenced nonempty schema array storage");
        for (ArrayIndex i = 0; i < arena.singleton_handle_count(); ++i)
        {
            auto handle = SingletonArrayHandleMask | i;
            if (arena.size(handle) && !handles.contains(handle))
                throw std::invalid_argument("Unreferenced nonempty singleton schema storage");
        }
    };
    allReferenced(fields);
    allReferenced(edges);
    allReferenced(symbols);
    allReferenced(literals);
    allReferenced(named);
    allReferenced(numbers);
    allReferenced(aliases);
}

void SchemaPool::Impl::indexFieldOrder()
{
    effectiveFieldOrders.resize(types.size());
    std::vector<bool> ready(types.size());
    auto prepare = [&](auto&& self, TypeId id) -> void
    {
        if (!id || ready[id])
            return;
        auto const& row = types[id];
        self(self, TypeId(row.base));
        if (row.base && !fields.size(row.fields))
        {
            // An unchanged inherited member order is already immutable and
            // need not be stored again for every nominal specialization.
            effectiveFieldOrders[id] = effectiveFieldOrders[row.base];
            ready[id] = true;
            return;
        }
        std::set<StringId> own;
        for (auto const& field : fieldRecords(row.fields))
            own.insert(field.name);
        std::vector<uint32_t> order;
        for (auto record : effectiveFields.range(effectiveFieldOrders[row.base]))
        {
            chargeIndexWork();
            if (!own.contains(fieldRows[record].name))
                order.push_back(record);
        }
        for (auto record : fields.range(row.fields))
        {
            chargeIndexWork();
            order.push_back(record);
        }
        if (!order.empty())
        {
            auto handle = effectiveFields.new_array(order.size());
            for (auto record : order)
                effectiveFields.push_back(handle, record);
            effectiveFieldOrders[id] = handle;
        }
        ready[id] = true;
    };
    for (size_t id = 1; id < types.size(); ++id)
        prepare(prepare, TypeId(id));
}

void SchemaPool::finalizeSchema()
{
    if (schema_->packed)
        return;
    requireMutable();
    if (schema_->packing)
        throw std::logic_error("Cannot reuse a failed schema publication");
    if (schema_->activeAppends)
        throw std::logic_error("Cannot finalize while a schema append is active");

    // Mark publication before mutating derived state. A failed preparation leaves
    // packing set, preventing later callers from using a partially indexed pool.
    schema_->packing = true;
    schema_->prepareAffinities();
    schema_->validate();
    schema_->index();
    for (size_t id = 1; id < schema_->types.size(); ++id)
        schema_->types[id].flags |= 8;
    schema_->packed = true;
}

void SchemaPool::finalize()
{
    finalizeSchema();
    ModelPool::freeze();
}

void SchemaPool::finalize(std::string identity)
{
    if (identity.empty() || identity.size() > 65535)
        throw std::invalid_argument("A named schema requires a nonempty bounded identity");
    requireMutable();
    if (!schema_->identity.empty())
        throw std::logic_error("A schema identity cannot be replaced");
    finalizeSchema();
    setIdentity(std::move(identity));
    ModelPool::freeze();
}

auto SchemaPool::importGraph(SchemaPool const& source, std::span<TypeId const> reuse)
    -> std::vector<TypeId>
{
    return importGraphWithExtension(source, reuse, {});
}

auto SchemaPool::importGraphWithExtension(
    SchemaPool const& source, std::span<TypeId const> reuse,
    std::function<void(std::span<TypeId const>)> const& extension) -> std::vector<TypeId>
{
    requireMutable();
    if (schema_->packed || schema_->packing || !source.finalized() || &source == this)
        throw std::invalid_argument("Graph import requires a distinct frozen source and mutable destination");
    auto count = source.schema_->types.size();
    if (!reuse.empty() && (reuse.size() != count || reuse.front() != NoTypeId))
        throw std::invalid_argument("Schema reuse table has the wrong namespace or unknown mapping");
    std::vector<TypeId> remap(count);
    size_t additional = count - 1;
    for (size_t id = 1; id < count; ++id)
    {
        auto name = SchemaType::kindNameId(source.schemaType(TypeId(id)).kind());
        if (strings()->resolve(name) != source.dictionary().resolve(name))
            throw std::invalid_argument("Schema kind names require the same static vocabulary");
        if (!reuse.empty() && reuse[id])
        {
            if (!schema_->valid(reuse[id]))
                throw std::invalid_argument("Reused type is absent from the destination graph");
            remap[id] = reuse[id];
            --additional;
        }
    }
    if (additional > size_t(MaxTypeId) + 1 - schema_->types.size())
        throw std::length_error("Imported graph exhausts the uint16 type namespace");
    Impl::AppendRollback rollback(*schema_);
    // Reserve identities before edges, preserving cycles, sharing and explicit zero targets.
    for (size_t id = 1; id < count; ++id)
        if (!remap[id])
            remap[id] = addSchema(source.schemaType(TypeId(id)).kind());
    for (size_t id = 1; id < count; ++id)
        if (reuse.empty() || !reuse[id])
            schema_->copyDirectType(remap[id], source, TypeId(id), remap);
    if (extension)
        extension(remap);
    rollback.committed = true;
    return remap;
}

auto SchemaPool::cloneType(SchemaType source) -> SchemaType
{
    if (!source || source.pool() != this)
        throw std::invalid_argument("Direct type cloning requires a type in the same pool");
    requireMutable();
    if (schema_->packed || schema_->packing)
        throw std::logic_error("Cannot clone into a finalized schema graph");
    Impl::AppendRollback rollback(*schema_);
    auto target = newType(source.kind());
    schema_->copyDirectType(target.id(), *this, source.id());
    cloneTypeExtension(source.id(), target.id());
    rollback.committed = true;
    return target;
}

void SchemaPool::applyScalarAnnotations(
    TypeId id, std::optional<SchemaType::NumericDomain> const& numeric,
    std::optional<uint8_t> width, std::span<SchemaType::NamedValue const> values,
    std::function<void()> const& extension)
{
    auto original = schema_->mutableType(id);
    auto staged = original;
    auto type = schemaType(id);
    auto bitWidth = width.value_or(type.bitWidth());
    if (width && (!type.isBitmask() || !*width || *width > 64))
        throw std::invalid_argument("A bit width annotation requires a concrete bitmask type");
    if (numeric && (!std::isfinite(numeric->scale) || numeric->scale == 0 ||
                    !std::isfinite(numeric->offset) || numeric->ordinaryRanges.size() > Impl::MaxEdges / 2))
        throw std::invalid_argument("Invalid numeric domain annotation");

    auto checkFlag = [&](SchemaType::NamedValue const& value)
    {
        if (value.role != SchemaType::NamedValue::Role::Flag)
            return;
        if (!type.isBitmask())
            throw std::invalid_argument("Flag annotations require a concrete bitmask type");
        uint64_t bits = 0;
        if (auto unsignedValue = std::get_if<uint64_t>(&value.value))
            bits = *unsignedValue;
        else if (auto signedValue = std::get_if<int64_t>(&value.value); signedValue && *signedValue >= 0)
            bits = uint64_t(*signedValue);
        else
            throw std::invalid_argument("Flag annotations require nonnegative integer masks");
        if (bitWidth < 64 && (bits >> bitWidth))
            throw std::invalid_argument("Flag annotation exceeds the physical encoding width");
    };
    std::set<std::string_view> names;
    type.forEachNamedValue([&](auto const& value) {
        names.insert(value.name);
        if (width)
            checkFlag(value);
    });
    for (auto const& value : values)
    {
        if (value.name.empty() || !names.insert(value.name).second ||
            unsigned(value.role) > unsigned(SchemaType::NamedValue::Role::Flag) ||
            unsigned(value.meaning) > unsigned(SchemaType::NamedValue::Meaning::Saturated))
            throw std::invalid_argument("Invalid or duplicate named value annotation");
        checkFlag(value);
    }

    Impl::AppendRollback rollback(*schema_);
    std::optional<Impl::NumericData> replacement;
    ArrayIndex retiredNumbers = 0;
    if (numeric)
    {
        Impl::NumericData row;
        row.encoding = schema_->text(numeric->encoding);
        row.unit = schema_->text(numeric->unit);
        row.scale = numeric->scale;
        row.offset = numeric->offset;
        row.flags = (numeric->minimum ? 1u : 0u) | (numeric->maximum ? 2u : 0u) |
                    (numeric->complete ? 4u : 0u);
        if (numeric->minimum)
            row.minimum = std::visit([&](auto value) { return schema_->scalar(value); }, *numeric->minimum);
        if (numeric->maximum)
            row.maximum = std::visit([&](auto value) { return schema_->scalar(value); }, *numeric->maximum);
        for (auto const& range : numeric->ordinaryRanges)
        {
            Impl::IntervalData interval{
                std::visit([&](auto value) { return schema_->scalar(value); }, range.min),
                std::visit([&](auto value) { return schema_->scalar(value); }, range.max)};
            schema_->appendRecord(schema_->intervalRows, schema_->numbers, row.ranges, interval);
        }
        if (original.numeric)
        {
            retiredNumbers = schema_->numerics[original.numeric - 1].ranges;
            replacement = row;
        }
        else
        {
            schema_->numerics.push_back(row);
            staged.numeric = schema_->numerics.size();
        }
    }
    if (!values.empty())
    {
        staged.named = 0;
        for (size_t i = 0, count = schema_->named.size(original.named); i < count; ++i)
        {
            auto value = schema_->named.at(original.named, i).value().get();
            schema_->append(schema_->named, staged.named, value);
        }
        for (auto const& value : values)
            schema_->appendRecord(schema_->namedRows, schema_->named, staged.named, schema_->encodeNamed(value));
    }
    if (width)
        staged.flags = (staged.flags & ~0xff00u) | (uint32_t(*width) << 8);

    // Everything that can allocate or fail precedes publication. Existing
    // semantic rows stay unchanged while the extension appends its own records.
    if (extension)
        extension();
    if (replacement)
        schema_->numerics[original.numeric - 1] = *replacement;
    schema_->types[id] = staged;
    if (retiredNumbers)
        schema_->retireIntervals(retiredNumbers);
    if (!values.empty() && original.named)
        schema_->named.truncate(original.named, 0);
    rollback.committed = true;
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
    uint32_t version = CodecVersion;
    archive.value4b(version);
    if (version != CodecVersion)
        throw std::invalid_argument("Unsupported generic schema payload version");
    if constexpr (detail::bitsery_input_archive<S>)
    {
        // Every existing column codec begins with its exact byte length. A
        // read-only pass bounds their aggregate before any arena is allocated.
        auto start = archive.adapter().currentReadPos();
        size_t remaining = budget;
        auto column = [&](size_t recordSize, size_t maxRecords)
        {
            uint64_t bytes = 0;
            archive.value8b(bytes);
            if (bytes > remaining || bytes > bitsery_max_column_payload_bytes ||
                bytes % recordSize || bytes / recordSize > maxRecords ||
                !detail::check_bitsery_payload_available(archive.adapter(), size_t(bytes)))
                throw std::length_error("Schema column exceeds its input/allocation budget");
            remaining -= size_t(bytes);
            archive.adapter().currentReadPos(archive.adapter().currentReadPos() + size_t(bytes));
        };
        auto arena = [&](size_t recordSize)
        {
            column(sizeof(ArrayArena<TypeId>::CompactArrayChunk), MaxEdges + 1);
            column(recordSize, MaxEdges);
            column(recordSize, MaxEdges);
            column(1, MaxEdges);
        };
        column(sizeof(TypeData), size_t(MaxTypeId) + 1);
        column(sizeof(FieldData), MaxEdges);
        arena(sizeof(uint32_t));
        arena(sizeof(TypeId));
        arena(sizeof(StringId));
        arena(sizeof(ModelNodeAddress));
        column(sizeof(NamedData), MaxEdges);
        arena(sizeof(uint32_t));
        column(sizeof(IntervalData), MaxEdges);
        arena(sizeof(uint32_t));
        column(sizeof(NumericData), MaxTypeId);
        arena(sizeof(AliasData));
        archive.adapter().currentReadPos(start);
    }
    archive.object(types);
    archive.object(fieldRows);
    archive.ext(fields, bitsery::ext::ArrayArenaExt{});
    archive.ext(edges, bitsery::ext::ArrayArenaExt{});
    archive.ext(symbols, bitsery::ext::ArrayArenaExt{});
    archive.ext(literals, bitsery::ext::ArrayArenaExt{});
    archive.object(namedRows);
    archive.ext(named, bitsery::ext::ArrayArenaExt{});
    archive.object(intervalRows);
    archive.ext(numbers, bitsery::ext::ArrayArenaExt{});
    archive.object(numerics);
    archive.ext(aliases, bitsery::ext::ArrayArenaExt{});
    if constexpr (detail::bitsery_input_archive<S>)
        if (archive.adapter().error() != bitsery::ReaderError::NoError)
            throw std::invalid_argument("Malformed schema column or arena storage");
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
    if (schema_->packed || schema_->types.size() != 1 || bytes.size() > maxBytes)
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

    // Rebuild indexes from validated canonical records; indexes are not trusted
    // wire input and no descriptor can escape this unfinished owner yet.
    schema_->index();
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

auto SchemaPool::read(std::span<uint8_t const> input, std::string identity, size_t maxBytes) -> ConstPtr
{
    if (identity.empty() || identity.size() > 65535)
        throw std::invalid_argument("A named schema requires a nonempty bounded identity");
    auto result = std::make_shared<SchemaPool>();
    result->readSchemaPayload(input, maxBytes);
    result->setIdentity(std::move(identity));
    result->ModelPool::freeze();
    return result;
}

auto SchemaPool::schemaMemoryUsage() const -> MemoryUsage
{
    MemoryUsage result{sizeof(Impl), sizeof(Impl)};
    result.logicalBytes += schema_->identity.size();
    auto identityObject = reinterpret_cast<uintptr_t>(&schema_->identity);
    auto identityData = reinterpret_cast<uintptr_t>(schema_->identity.data());
    if (identityData < identityObject || identityData >= identityObject + sizeof(schema_->identity))
        result.allocatedBytes += schema_->identity.capacity() + 1;
    result += schema_->types.memory_usage();
    result += schema_->fieldRows.memory_usage();
    result += schema_->fields.memory_usage();
    result += schema_->edges.memory_usage();
    result += schema_->symbols.memory_usage();
    result += schema_->literals.memory_usage();
    result += schema_->namedRows.memory_usage();
    result += schema_->intervalRows.memory_usage();
    result += schema_->named.memory_usage();
    result += schema_->numbers.memory_usage();
    result += schema_->numerics.memory_usage();
    result += schema_->aliases.memory_usage();
    result += schema_->effectiveFieldOrders.memory_usage();
    result += schema_->effectiveFields.memory_usage();
    result += schema_->indexes.memory_usage();
    result += schema_->queryNames.memory_usage();
    result += schema_->fieldLookup.memory_usage();
    result += schema_->queryTypes.memory_usage();
    result += schema_->queryFields.memory_usage();
    result += schema_->queryAliases.memory_usage();
    return result;
}

SchemaType::SchemaType(SchemaPool const* pool, TypeId id, bool writable) noexcept
    : pool_(pool), id_(id), writable_(writable)
{
}

SchemaType::SchemaType(std::shared_ptr<SchemaPool const> owner, TypeId id) noexcept
    : pool_(owner.get()), owner_(std::move(owner)), id_(id)
{
}

auto SchemaType::retain() const -> SchemaType
{
    auto result = *this;
    if (result && !result.owner_)
        result.owner_ = std::static_pointer_cast<SchemaPool const>(pool_->shared_from_this());
    return result;
}

auto SchemaType::borrow() const noexcept -> SchemaType
{
    return SchemaType(pool_, id_, writable_);
}

auto SchemaType::operator==(SchemaType const& other) const noexcept -> bool
{
    return pool_ == other.pool_ && id_ == other.id_;
}

auto SchemaType::mutablePool() const -> SchemaPool&
{
    if (!*this || !writable_)
        throw std::logic_error("Cannot mutate a readonly schema type view");
    pool_->schema_->mutableType(id_);
    return *const_cast<SchemaPool*>(pool_);
}

auto SchemaType::localId(SchemaType const& child) const -> TypeId
{
    if (child && child.pool_ != pool_)
        throw std::invalid_argument("Schema edge targets another pool's type namespace");
    return child.id_;
}

auto SchemaType::kind() const -> Kind
{
    if (!*this)
        return Kind::Unknown;
    auto const& row = pool_->schema_->types[id_];
    auto value = Kind(row.kind);
    if (row.flags & 2)
        value = SchemaType::makeKind(SchemaType::kindNameId(value), row.flags & 4
            ? SchemaType::affinities(value) | valueTypeAffinity(ValueType::Null)
            : SchemaType::affinities(value) & ~valueTypeAffinity(ValueType::Null));
    return value;
}

auto SchemaType::hasAffinity(ValueType value) const -> bool
{
    return SchemaType::affinities(kind()) & valueTypeAffinity(value);
}

auto SchemaType::isBitmask() const -> bool
{
    auto bits = SchemaType::affinities(kind());
    auto mask = valueTypeAffinity(ValueType::Bitmask);
    return composition() == Composition::None && (bits & mask) &&
        !(bits & ~(mask | valueTypeAffinity(ValueType::Null)));
}

auto SchemaType::typeName() const -> std::string_view
{
    return *this ? pool_->schema_->text(pool_->schema_->types[id_].typeName) : std::string_view{};
}

auto SchemaType::composition() const -> Composition
{
    return *this ? Composition((pool_->schema_->types[id_].flags >> 4) & 3) : Composition::None;
}

auto SchemaType::open() const -> bool
{
    return *this && (pool_->schema_->types[id_].flags & 1);
}

auto SchemaType::nullable() const -> std::optional<bool>
{
    if (!*this)
        return {};
    auto flags = pool_->schema_->types[id_].flags;
    return flags & 2 ? std::optional<bool>(flags & 4) : std::nullopt;
}

auto SchemaType::alternatives() const -> TypeIds
{
    return *this ? pool_->schema_->edges.range(pool_->schema_->types[id_].alternatives) : TypeIds{};
}

auto SchemaType::elements() const -> TypeIds
{
    return *this ? pool_->schema_->edges.range(pool_->schema_->types[id_].elements) : TypeIds{};
}

auto SchemaType::baseType() const -> SchemaType
{
    if (!*this)
        return {};
    auto base = TypeId(pool_->schema_->types[id_].base);
    if (!pool_->schema_->valid(base))
        return {};
    auto result = *this;
    result.id_ = base;
    return result;
}

auto SchemaType::fieldTypes(StringId name) const -> TypeIds
{
    if (!*this)
        return {};
    auto const& graph = *pool_->schema_;
    if (composition() != Composition::None || !alternatives().empty())
    {
        if (!graph.memberIndexesReady)
            return {};
        if (auto field = graph.combinedField(id_, name))
            return graph.queryTypes.range(field->children);
        return graph.queryTypes.range(graph.indexes[id_].flags & 2 ? graph.unknownTypes : 0);
    }
    auto parent = id_;
    if (auto field = graph.declaredField(parent, name))
        return field->children ? graph.edges.range(field->children)
            : graph.queryTypes.range(graph.unknownTypes);
    auto kindName = kindNameId(kind());
    if (open() || kindName == StringPool::SchemaUnknown || kindName == StringPool::SchemaAny)
        return graph.queryTypes.range(graph.unknownTypes);
    return {};
}

auto SchemaType::itemTypes() const -> TypeIds
{
    if (!*this)
        return {};
    auto const& graph = *pool_->schema_;
    if (composition() != Composition::None || !alternatives().empty())
        return graph.memberIndexesReady ? graph.queryTypes.range(graph.indexes[id_].combinedItems) : TypeIds{};
    auto handle = graph.types[id_].elements;
    return handle ? graph.edges.range(handle)
        : graph.queryTypes.range(hasAffinity(ValueType::Array) ? graph.unknownTypes : 0);
}

auto SchemaType::fieldSchema(StringId name) const -> TypeId
{
    auto types = fieldTypes(name);
    if (types.empty())
        return NoTypeId;
    auto id = types.front();
    for (auto other : types)
        if (id != other)
            return NoTypeId;
    return id;
}

auto SchemaType::itemSchema() const -> TypeId
{
    auto types = itemTypes();
    if (types.empty())
        return NoTypeId;
    auto id = types.front();
    for (auto other : types)
        if (id != other)
            return NoTypeId;
    return id;
}

auto SchemaType::fieldType(StringId name) const -> SchemaType
{
    auto id = fieldSchema(name);
    if (!id)
        return {};
    auto result = *this;
    result.id_ = id;
    return result;
}

auto SchemaType::itemType() const -> SchemaType
{
    auto id = itemSchema();
    if (!id)
        return {};
    auto result = *this;
    result.id_ = id;
    return result;
}

auto SchemaType::field(StringId name) const -> SchemaField
{
    if (!*this)
        return {};
    auto parent = *this;
    if (!pool_->schema_->declaredField(parent.id_, name))
        return {};
    return SchemaField(std::move(parent), name);
}

auto SchemaType::field(std::string_view name) const -> SchemaField
{
    if (!*this)
        return {};
    auto id = pool_->dictionary().get(name);
    // Empty is a valid declared spelling, but also the dictionary's miss sentinel.
    return id || name.empty() ? field(id) : SchemaField{};
}

void SchemaType::forEachField(std::function<void(SchemaField const&)> const& fn) const
{
    if (!*this)
        return;
    auto const& graph = *pool_->schema_;
    if (graph.memberIndexesReady)
    {
        for (auto index : graph.effectiveFields.range(graph.effectiveFieldOrders[id_]))
        {
            auto const& row = graph.fieldRows[index];
            auto parent = *this;
            parent.id_ = row.owner;
            fn(SchemaField(std::move(parent), row.name));
        }
        return;
    }
    auto visit = [&](auto&& self, TypeId id, size_t depth) -> void
    {
        if (!pool_->schema_->valid(id))
            return;
        if (depth > 256)
            throw std::invalid_argument("Cyclic or excessive schema inheritance");
        auto const& row = pool_->schema_->types[id];
        if (row.base)
            self(self, TypeId(row.base), depth + 1);
        for (auto const& item : pool_->schema_->fieldRecords(row.fields))
        {
            auto occurrence = field(item.name);
            // Local declarations shadow inherited members without changing source order.
            if (occurrence && occurrence.parent_.id_ == id)
                fn(occurrence);
        }
    };
    visit(visit, id_, 0);
}

void SchemaType::forEachNamedValue(std::function<void(NamedValue const&)> const& fn) const
{
    if (!*this)
        return;
    auto const& graph = *pool_->schema_;
    for (auto const& row : graph.namedRecords(graph.types[id_].named))
        fn(graph.decodeNamed(row));
}

void SchemaType::forEachDirectField(std::function<void(SchemaField const&)> const& fn) const
{
    if (*this)
        for (auto const& field : pool_->schema_->fieldRecords(pool_->schema_->types[id_].fields))
            fn(SchemaField(*this, field.name));
}

void SchemaType::forEachFieldAlias(std::function<void(StringId, StringId)> const& fn) const
{
    if (*this)
        for (auto const& alias : pool_->schema_->aliases.range(pool_->schema_->types[id_].aliases))
            fn(alias.alias, alias.canonical);
}

auto SchemaType::namedValue(std::string_view name) const -> std::optional<NamedValue>
{
    std::optional<NamedValue> result;
    forEachNamedValue([&](auto const& value) { if (value.name == name) result = value; });
    return result;
}

auto SchemaType::NumericRanges::Read::operator()(size_t position) const -> NumericRange
{
    if (!pool)
        return data[position];
    auto const& graph = *pool->schema_;
    auto const& row = graph.intervalRows[graph.numbers.range(handle)[position]];
    return {graph.number(row.minimum), graph.number(row.maximum)};
}

auto SchemaType::numericDomain() const -> std::optional<NumericDomain>
{
    if (!*this)
        return {};
    auto const& graph = *pool_->schema_;
    auto index = graph.types[id_].numeric;
    if (!index)
        return {};
    auto const& row = graph.numerics[index - 1];
    return NumericDomain{
        .encoding = graph.text(row.encoding), .unit = graph.text(row.unit),
        .minimum = row.flags & 1 ? std::optional(graph.number(row.minimum)) : std::nullopt,
        .maximum = row.flags & 2 ? std::optional(graph.number(row.maximum)) : std::nullopt,
        .ordinaryRanges = NumericRanges(pool_, row.ranges, graph.numbers.size(row.ranges)),
        .scale = row.scale, .offset = row.offset, .complete = bool(row.flags & 4)};
}

void SchemaType::forEachEnumSymbol(std::function<void(StringId)> const& fn) const
{
    if (*this)
        for (auto symbol : pool_->schema_->symbols.range(pool_->schema_->types[id_].symbols))
            fn(symbol);
}

void SchemaType::forEachEnumValue(std::function<void(EnumValue const&)> const& fn) const
{
    if (*this)
        for (auto address : pool_->schema_->literals.range(pool_->schema_->types[id_].literals))
            fn(pool_->schema_->enumValue(address));
}

auto SchemaType::enumValueCount() const -> size_t
{
    return *this ? pool_->schema_->literals.size(pool_->schema_->types[id_].literals) : 0;
}

auto SchemaType::enumValueAt(size_t position) const -> EnumValue
{
    if (position >= enumValueCount())
        throw std::out_of_range("Schema enum literal index out of range");
    auto const& graph = *pool_->schema_;
    return graph.enumValue(graph.literals.range(graph.types[id_].literals)[position]);
}

auto SchemaType::bitWidth() const -> uint8_t
{
    return *this ? uint8_t(pool_->schema_->types[id_].flags >> 8) : 0;
}

auto SchemaType::finalized() const -> bool { return *this && pool_->finalized(); }
auto SchemaType::sealed() const -> bool
{
    return *this && ((pool_->schema_->types[id_].flags & 8) || pool_->finalized() || pool_->frozen());
}

void SchemaType::seal()
{
    if (!*this || !writable_)
        throw std::logic_error("Cannot seal through a readonly schema type view");
    auto& graph = *pool_->schema_;
    if (graph.packed || graph.packing || pool_->frozen())
        throw std::logic_error("Cannot mutate a finalized schema graph");
    if (!(graph.types[id_].flags & 8))
        graph.mutableType(id_).flags |= 8;
}

void SchemaType::setTypeName(std::string_view name) { mutablePool().setTypeName(id_, name); }
void SchemaType::addNamedValue(NamedValue const& value) { mutablePool().addNamedValue(id_, value); }
void SchemaType::setComposition(Composition composition) { mutablePool().setComposition(id_, composition); }
void SchemaType::setKindName(StringId name)
{
    auto& pool = mutablePool();
    if (!name || name >= StringPool::FirstDynamicId || !pool.dictionary().resolve(name))
        throw std::invalid_argument("Semantic kind names require a declared static string id");
    pool.validateStructureMutation(id_);
    auto& row = pool.schema_->types[id_];
    row.kind = (uint32_t(name) << 16) | (row.kind & 0xffffu);
}

void SchemaType::setNullable(std::optional<bool> nullable) { mutablePool().setNullable(id_, nullable); }
auto SchemaType::asObject() const -> SchemaObjectType { return SchemaObjectType(*this); }
auto SchemaType::asArray() const -> SchemaArrayType { return SchemaArrayType(*this); }
auto SchemaType::asScalar() const -> SchemaScalarType { return SchemaScalarType(*this); }
auto SchemaType::asEnum() const -> SchemaEnumType { return SchemaEnumType(*this); }
auto SchemaType::asBitmask() const -> SchemaBitmaskType { return SchemaBitmaskType(*this); }
auto SchemaType::asComposition() const -> SchemaCompositionType { return SchemaCompositionType(*this); }

auto SchemaField::name() const -> std::string_view
{
    return *this ? parent_.pool_->dictionary().resolve(name_).value() : std::string_view{};
}

auto SchemaField::types() const -> SchemaType::TypeIds
{
    if (!*this)
        return {};
    auto id = parent_.id_;
    auto name = name_;
    auto field = parent_.pool_->schema_->declaredField(id, name);
    return field ? parent_.pool_->schema_->edges.range(field->children) : SchemaType::TypeIds{};
}

auto SchemaField::type() const -> SchemaType
{
    if (!*this)
        return {};
    auto id = parent_.fieldSchema(name_);
    if (!id)
        return {};
    auto result = parent_;
    result.id_ = id;
    return result;
}

auto SchemaField::required() const -> std::optional<bool>
{
    if (!*this)
        return {};
    auto id = parent_.id_;
    auto name = name_;
    auto field = parent_.pool_->schema_->declaredField(id, name);
    return field && field->required ? std::optional<bool>(field->required == 2) : std::nullopt;
}

auto SchemaField::multimap() const -> bool
{
    if (!*this)
        return false;
    auto id = parent_.id_;
    auto name = name_;
    auto field = parent_.pool_->schema_->declaredField(id, name);
    return field && (field->flags & 1);
}

auto SchemaField::role() const -> SchemaType::FieldRole
{
    if (!*this)
        return SchemaType::FieldRole::Data;
    auto id = parent_.id_;
    auto name = name_;
    auto field = parent_.pool_->schema_->declaredField(id, name);
    return field && (field->flags & 2) ? SchemaType::FieldRole::Context : SchemaType::FieldRole::Data;
}

auto SchemaField::retain() const -> SchemaField { return SchemaField(parent_.retain(), name_); }
void SchemaField::setRequired(std::optional<bool> required)
{
    parent_.mutablePool().setFieldRequired(parent_.id_, name(), required);
}

void SchemaField::setRole(SchemaType::FieldRole role)
{
    parent_.mutablePool().setFieldRole(parent_.id_, name(), role);
}

void SchemaField::setMultimap(bool multimap)
{
    parent_.mutablePool().setFieldMultimap(parent_.id_, name(), multimap);
}

void SchemaField::addType(SchemaType child)
{
    parent_.mutablePool().addFieldSchema(parent_.id_, name(), parent_.localId(child));
}

SchemaObjectType::SchemaObjectType(SchemaType type) : SchemaType(std::move(type))
{
    if (!*this || !hasAffinity(ValueType::Object))
        throw std::invalid_argument("SchemaObjectType requires object affinity");
}

auto SchemaObjectType::declareField(std::string_view name) -> SchemaField
{
    mutablePool().declareField(id(), name);
    return field(name);
}

auto SchemaObjectType::addField(std::string_view name, SchemaType child) -> SchemaField
{
    mutablePool().addFieldSchema(id(), name, localId(child));
    return field(name);
}

void SchemaObjectType::addAlias(std::string_view alias, std::string_view canonical)
{
    mutablePool().addFieldAlias(id(), alias, canonical);
}

void SchemaObjectType::setBase(SchemaObjectType base) { mutablePool().setBaseSchema(id(), localId(base)); }
void SchemaObjectType::setOpen(bool open) { mutablePool().setOpen(id(), open); }

SchemaArrayType::SchemaArrayType(SchemaType type) : SchemaType(std::move(type))
{
    if (!*this || !hasAffinity(ValueType::Array))
        throw std::invalid_argument("SchemaArrayType requires array affinity");
}

void SchemaArrayType::addElement(SchemaType child) { mutablePool().addElementSchema(id(), localId(child)); }
auto SchemaArrayType::declareField(std::string_view name) -> SchemaField
{
    mutablePool().declareField(id(), name);
    return field(name);
}

auto SchemaArrayType::addField(std::string_view name, SchemaType child) -> SchemaField
{
    mutablePool().addFieldSchema(id(), name, localId(child));
    return field(name);
}

void SchemaArrayType::addAlias(std::string_view alias, std::string_view canonical)
{
    mutablePool().addFieldAlias(id(), alias, canonical);
}

SchemaScalarType::SchemaScalarType(SchemaType type) : SchemaType(std::move(type))
{
    if (!*this || (SchemaType::affinities(kind()) & ~SchemaType::ScalarAffinities))
        throw std::invalid_argument("SchemaScalarType requires scalar affinity");
}

void SchemaScalarType::setNumericDomain(NumericDomain const& domain)
{
    mutablePool().setTypeNumericDomain(id(), domain);
}

SchemaEnumType::SchemaEnumType(SchemaType type) : SchemaType(std::move(type))
{
    if (!*this || !(SchemaType::affinities(kind()) & SchemaType::ScalarAffinities))
        throw std::invalid_argument("SchemaEnumType requires a possible scalar affinity");
}

void SchemaEnumType::addSymbol(std::string_view symbol) { mutablePool().addEnumSymbol(id(), symbol); }
void SchemaEnumType::addValue(EnumValue const& value) { mutablePool().addEnumValue(id(), value); }

SchemaBitmaskType::SchemaBitmaskType(SchemaType type) : SchemaScalarType(std::move(type))
{
    if (!isBitmask())
        throw std::invalid_argument("SchemaBitmaskType requires a concrete mask type");
}

void SchemaBitmaskType::setBitWidth(uint8_t width) { mutablePool().setBitWidth(id(), width); }
void SchemaBitmaskType::addFlag(std::string_view name, uint64_t bits)
{
    if (bitWidth() < 64 && (bits >> bitWidth()))
        throw std::invalid_argument("Flag declaration exceeds its encoding width");
    addNamedValue({.name = name, .value = bits, .role = NamedValue::Role::Flag});
}

SchemaCompositionType::SchemaCompositionType(SchemaType type) : SchemaType(std::move(type))
{
    if (!*this || composition() == Composition::None)
        throw std::invalid_argument("SchemaCompositionType requires a logical combiner");
}

void SchemaCompositionType::addAlternative(SchemaType alternative)
{
    mutablePool().addAlternative(id(), localId(alternative));
}

auto SchemaPool::schemaType(TypeId id) const -> SchemaType
{
    return schema_->valid(id) ? SchemaType(this, id, false) : SchemaType{};
}

auto SchemaPool::schemaType(TypeId id) -> SchemaType
{
    return schema_->valid(id) ? SchemaType(this, id, true) : SchemaType{};
}

auto SchemaPool::newType(SchemaType::Kind kind) -> SchemaType { return schemaType(addSchema(kind)); }
auto SchemaPool::newObjectType(SchemaType::Kind kind) -> SchemaObjectType
{
    if (!(SchemaType::affinities(kind) & valueTypeAffinity(ValueType::Object)))
        throw std::invalid_argument("Object factory requires object affinity");
    return SchemaObjectType(newType(kind));
}

auto SchemaPool::newArrayType(SchemaType::Kind kind) -> SchemaArrayType
{
    if (!(SchemaType::affinities(kind) & valueTypeAffinity(ValueType::Array)))
        throw std::invalid_argument("Array factory requires array affinity");
    return SchemaArrayType(newType(kind));
}

auto SchemaPool::newScalarType(SchemaType::Kind kind) -> SchemaScalarType
{
    if (SchemaType::affinities(kind) & ~SchemaType::ScalarAffinities)
        throw std::invalid_argument("Scalar factory requires scalar affinity");
    return SchemaScalarType(newType(kind));
}

auto SchemaPool::newEnumType(SchemaType::Kind kind) -> SchemaEnumType
{
    if (!(SchemaType::affinities(kind) & SchemaType::ScalarAffinities))
        throw std::invalid_argument("Enum factory requires possible scalar affinity");
    return SchemaEnumType(newType(kind));
}

auto SchemaPool::newBitmaskType(uint8_t width, SchemaType::Kind kind) -> SchemaBitmaskType
{
    auto bits = SchemaType::affinities(kind);
    auto mask = valueTypeAffinity(ValueType::Bitmask);
    if (!width || width > 64 || !(bits & mask) ||
        (bits & ~(mask | valueTypeAffinity(ValueType::Null))))
        throw std::invalid_argument("Bitmask factory requires a concrete width and mask affinity");
    auto result = SchemaBitmaskType(newType(kind));
    result.setBitWidth(width);
    return result;
}

auto SchemaPool::newCompositionType(SchemaType::Composition composition) -> SchemaCompositionType
{
    auto kind = SchemaType::Kind::Union;
    switch (composition)
    {
    case SchemaType::Composition::AnyOf: break;
    case SchemaType::Composition::OneOf: kind = SchemaType::Kind::OneOf; break;
    case SchemaType::Composition::AllOf: kind = SchemaType::Kind::Intersection; break;
    default: throw std::invalid_argument("Composition factory requires a logical operator");
    }
    auto result = newType(kind);
    setComposition(result.id(), composition);
    return SchemaCompositionType(result);
}

void SchemaPool::setFieldMultimap(TypeId id, std::string_view name, bool multimap)
{
    schema_->mutableType(id);
    validateFieldMutation(id, name);
    auto& field = schema_->field(id, name);
    field.flags = (field.flags & ~uint8_t{1}) | uint8_t(multimap);
}

void SchemaPool::declareField(TypeId id, std::string_view name)
{
    schema_->mutableType(id);
    validateFieldMutation(id, name);
    schema_->field(id, name);
}

} // namespace simfil
