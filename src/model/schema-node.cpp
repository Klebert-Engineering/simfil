#include "simfil/model/schema-node.h"
#include "schema-pool-impl.h"

#include <array>
#include <fmt/format.h>

namespace simfil
{

auto SchemaTypeModelNode::pool() const -> SchemaPool const&
{
    return static_cast<SchemaPool const&>(*model_);
}

auto SchemaTypeModelNode::describedType() const -> SchemaType
{
    auto const& graph = *pool().schema_;
    if (addr_.column() == Type)
        return pool().schemaType(TypeId(addr_.index()));
    if (addr_.column() == Field)
    {
        auto children = graph.edges.range(graph.fieldRows[addr_.index()].children);
        if (children.size() == 1)
            return pool().schemaType(children.front());
    }
    return {};
}

auto SchemaTypeModelNode::kind() const -> SchemaType::Kind
{
    auto const& graph = *pool().schema_;
    if (addr_.column() == Field && graph.edges.size(graph.fieldRows[addr_.index()].children) > 1)
        return SchemaType::Kind::Union;
    return describedType().kind();
}

auto SchemaTypeModelNode::type() const -> ValueType
{
    switch (addr_.column())
    {
    case Type:
    case Field:
    case Fields:
    case NamedValue:
    case NumericDomain:
    case Interval:
        return ValueType::Object;
    default:
        return ValueType::Array;
    }
}

auto SchemaTypeModelNode::builtinKeys(std::span<StringId> output) const -> uint32_t
{
    // Count every visible key even when the caller only needs the count. The
    // same order drives keyAt() and at(), including optional metadata fields.
    auto const& graph = *pool().schema_;
    uint32_t count = 0;
    auto key = [&](StringId name) {
        if (count < output.size())
            output[count] = name;
        ++count;
    };
    switch (addr_.column())
    {
    case NamedValue:
        for (auto name : {StringPool::SchemaName, StringPool::SchemaValue, StringPool::SchemaRole,
                          StringPool::SchemaMeaning, StringPool::SchemaDeprecated, StringPool::SchemaRemoved})
            key(name);
        if (graph.namedRows[addr_.index()].rawType)
            key(StringPool::SchemaRawValue);
        return count;
    case Interval:
        key(StringPool::SchemaMin);
        key(StringPool::SchemaMax);
        return count;
    case NumericDomain:
    {
        auto const& row = graph.numerics[addr_.index()];
        key(StringPool::SchemaEncoding);
        key(StringPool::SchemaUnit);
        if (row.flags & 1)
            key(StringPool::SchemaMinimum);
        if (row.flags & 2)
            key(StringPool::SchemaMaximum);
        for (auto name : {StringPool::SchemaOrdinaryRanges, StringPool::SchemaScale,
                          StringPool::SchemaOffset, StringPool::SchemaComplete})
            key(name);
        return count;
    }
    case Type:
    case Field:
        break;
    default:
        return 0;
    }

    key(StringPool::SchemaKind);
    auto represented = describedType();
    bool alternatives = false;
    if (addr_.column() == Field)
    {
        auto const& field = graph.fieldRows[addr_.index()];
        auto children = graph.edges.range(field.children);
        if (children.size() > 1 || (children.size() == 1 && children.front()))
            key(StringPool::SchemaRef);
        if (field.required)
            key(StringPool::SchemaRequired);
        if (field.flags & 2)
            key(StringPool::SchemaRole);
        if (field.flags & 1)
            key(StringPool::SchemaMultimap);
        alternatives = children.size() > 1;
    }
    else if (represented)
        key(StringPool::SchemaRef);
    if (!represented)
    {
        if (alternatives)
            key(StringPool::SchemaAlternatives);
        return count;
    }

    auto const& row = graph.types[represented.id()];
    if (row.typeName)
        key(StringPool::SchemaTypeName);
    if (row.flags & 2)
        key(StringPool::SchemaNullable);
    if (represented.composition() != SchemaType::Composition::None)
        key(StringPool::SchemaAlternatives);
    auto name = SchemaType::kindNameId(represented.kind());
    bool concrete = name != StringPool::SchemaUnknown && name != StringPool::SchemaAny;
    // Declared named array terminals and combiner context remain visible;
    // they do not turn array elements or alternatives into extra data levels.
    if (concrete && (represented.hasAffinity(ValueType::Object) ||
                     graph.effectiveFields.size(graph.effectiveFieldOrders[represented.id()])))
    {
        if (represented.composition() == SchemaType::Composition::None || row.fields || row.base)
        {
            key(StringPool::SchemaFields);
            key(StringPool::SchemaOpen);
        }
    }
    if (concrete && represented.hasAffinity(ValueType::Array) &&
        represented.composition() == SchemaType::Composition::None)
        key(StringPool::SchemaElements);
    if (graph.symbols.size(row.symbols) || graph.literals.size(row.literals))
        key(StringPool::SchemaEnum);
    if (graph.named.size(row.named))
        key(StringPool::SchemaNamedValues);
    if (row.numeric)
        key(StringPool::SchemaNumericDomain);
    if (represented.isBitmask())
        key(StringPool::SchemaBitWidth);
    return count;
}

auto SchemaTypeModelNode::size() const -> uint32_t
{
    auto const& graph = *pool().schema_;
    auto index = addr_.index();
    switch (addr_.column())
    {
    case Fields: return graph.effectiveFields.size(graph.effectiveFieldOrders[index]);
    case Elements: return graph.edges.size(graph.types[index].elements);
    case Alternatives: return graph.edges.size(graph.types[index].alternatives);
    case FieldAlternatives:
    case FieldReferences: return graph.edges.size(graph.fieldRows[index].children);
    case Enum:
        return graph.symbols.size(graph.types[index].symbols) + graph.literals.size(graph.types[index].literals);
    case NamedValues: return graph.named.size(graph.types[index].named);
    case Intervals: return graph.numbers.size(graph.numerics[index].ranges);
    default:
        auto represented = describedType();
        return builtinKeys() + (represented ? pool().descriptorExtensionSize(represented.id()) : 0);
    }
}

auto SchemaTypeModelNode::keyAt(int64_t position) const -> StringId
{
    if (position < 0 || type() != ValueType::Object || uint64_t(position) >= size())
        return StringPool::Empty;
    auto const& graph = *pool().schema_;
    if (addr_.column() == Fields)
    {
        auto records = graph.effectiveFields.range(graph.effectiveFieldOrders[addr_.index()]);
        return graph.fieldRows[records[position]].name;
    }
    std::array<StringId, 20> keys{};
    auto count = builtinKeys(keys);
    if (uint64_t(position) < count)
        return keys[position];
    auto name = pool().descriptorExtensionKeyAt(describedType().id(), uint32_t(position) - count);
    if (!pool().dictionary().resolve(name) ||
        std::find(keys.begin(), keys.begin() + count, name) != keys.begin() + count)
        throw std::logic_error("Schema descriptor extensions require distinct declared keys");
    return name;
}

auto SchemaTypeModelNode::get(StringId const& field) const -> ModelNode::Ptr
{
    if (type() != ValueType::Object)
        return {};
    for (uint32_t i = 0, count = size(); i < count; ++i)
        if (keyAt(i) == field)
            return at(i);
    return {};
}

auto SchemaTypeModelNode::scalar(ScalarValueType value) const -> ModelNode::Ptr
{
    return pool().Model::resolve(ModelNodeAddress(Model::Scalar), std::move(value));
}

template <class T>
auto SchemaTypeModelNode::metadataScalar(T value, bool flag) const -> ModelNode::Ptr
{
    // Runtime integers are signed: preserve large unsigned metadata as decimal
    // text, and flag values as fixed-width hex independent of their magnitude.
    if constexpr (std::is_same_v<T, uint64_t> || std::is_same_v<T, int64_t>)
    {
        if (flag)
            return scalar(fmt::format("0x{:016x}", uint64_t(value)));
        if constexpr (std::is_same_v<T, uint64_t>)
            if (value > uint64_t(INT64_MAX))
                return scalar(std::to_string(value));
        return scalar(int64_t(value));
    }
    else
        return scalar(value);
}

auto SchemaTypeModelNode::at(int64_t position) const -> ModelNode::Ptr
{
    if (position < 0 || uint64_t(position) >= size())
        return {};
    auto const& graph = *pool().schema_;
    auto index = addr_.index();
    auto child = [&](Column column, uint32_t id) {
        return pool().Model::resolve(ModelNodeAddress(column, id));
    };
    switch (addr_.column())
    {
    case Fields:
        return child(Field, graph.effectiveFields.range(graph.effectiveFieldOrders[index])[position]);
    case Elements:
        return child(Type, graph.edges.range(graph.types[index].elements)[position]);
    case Alternatives:
        return child(Type, graph.edges.range(graph.types[index].alternatives)[position]);
    case FieldAlternatives:
        return child(Type, graph.edges.range(graph.fieldRows[index].children)[position]);
    case FieldReferences:
        return scalar(int64_t(graph.edges.range(graph.fieldRows[index].children)[position]));
    case Enum:
    {
        auto const& row = graph.types[index];
        auto symbols = graph.symbols.range(row.symbols);
        if (uint64_t(position) < symbols.size())
            return scalar(pool().dictionary().resolve(symbols[position]).value());
        // Null has a zero core address, so wrap it as an ordinary scalar node.
        return scalar(graph.primitive(graph.literals.range(row.literals)[position - symbols.size()]));
    }
    case NamedValues:
        return child(NamedValue, graph.named.range(graph.types[index].named)[position]);
    case Intervals:
        return child(Interval, graph.numbers.range(graph.numerics[index].ranges)[position]);
    default:
        break;
    }

    auto key = keyAt(position);
    if (addr_.column() == NamedValue)
    {
        auto named = graph.decodeNamed(graph.namedRows[index]);
        switch (key)
        {
        case StringPool::SchemaName: return scalar(named.name);
        case StringPool::SchemaValue:
            return std::visit([&](auto value) {
                return metadataScalar(value, named.role == SchemaType::NamedValue::Role::Flag);
            }, named.value);
        case StringPool::SchemaRawValue:
            return std::visit([&](auto value) { return metadataScalar(value); }, *named.rawValue);
        case StringPool::SchemaRole:
            return scalar(std::string_view(named.role == SchemaType::NamedValue::Role::Flag ? "flag" :
                named.role == SchemaType::NamedValue::Role::Special ? "special" : "constant"));
        case StringPool::SchemaMeaning:
        {
            constexpr std::array meanings{"ordinary", "unknown", "variable", "unbounded", "saturated"};
            return scalar(std::string_view(meanings[unsigned(named.meaning)]));
        }
        case StringPool::SchemaDeprecated: return scalar(named.deprecated);
        case StringPool::SchemaRemoved: return scalar(named.removed);
        default: return {};
        }
    }
    if (addr_.column() == Interval)
    {
        auto const& range = graph.intervalRows[index];
        return std::visit([&](auto value) { return metadataScalar(value); },
                          graph.number(key == StringPool::SchemaMin ? range.minimum : range.maximum));
    }
    if (addr_.column() == NumericDomain)
    {
        auto const& row = graph.numerics[index];
        switch (key)
        {
        case StringPool::SchemaEncoding: return scalar(graph.text(row.encoding));
        case StringPool::SchemaUnit: return scalar(graph.text(row.unit));
        case StringPool::SchemaMinimum:
            return std::visit([&](auto value) { return metadataScalar(value); }, graph.number(row.minimum));
        case StringPool::SchemaMaximum:
            return std::visit([&](auto value) { return metadataScalar(value); }, graph.number(row.maximum));
        case StringPool::SchemaOrdinaryRanges: return child(Intervals, index);
        case StringPool::SchemaScale: return scalar(row.scale);
        case StringPool::SchemaOffset: return scalar(row.offset);
        case StringPool::SchemaComplete: return scalar(bool(row.flags & 4));
        default: return {};
        }
    }

    auto represented = describedType();
    if (uint64_t(position) >= builtinKeys())
        return pool().descriptorExtensionGet(represented.id(), key);
    switch (key)
    {
    case StringPool::SchemaKind:
        return scalar(pool().dictionary().resolve(SchemaType::kindNameId(kind())).value());
    case StringPool::SchemaRef:
        return represented ? scalar(int64_t(represented.id())) : child(FieldReferences, index);
    case StringPool::SchemaRequired: return scalar(graph.fieldRows[index].required == 2);
    case StringPool::SchemaRole: return scalar(std::string_view("context"));
    case StringPool::SchemaMultimap: return scalar(true);
    case StringPool::SchemaTypeName: return scalar(represented.typeName());
    case StringPool::SchemaNullable: return scalar(*represented.nullable());
    case StringPool::SchemaOpen: return scalar(represented.open());
    case StringPool::SchemaBitWidth: return scalar(int64_t(represented.bitWidth()));
    case StringPool::SchemaFields: return child(Fields, represented.id());
    case StringPool::SchemaElements: return child(Elements, represented.id());
    case StringPool::SchemaEnum: return child(Enum, represented.id());
    case StringPool::SchemaNamedValues: return child(NamedValues, represented.id());
    case StringPool::SchemaNumericDomain: return child(NumericDomain, graph.types[represented.id()].numeric - 1);
    case StringPool::SchemaAlternatives:
        return represented ? child(Alternatives, represented.id()) : child(FieldAlternatives, index);
    default:
        return {};
    }
}

bool SchemaTypeModelNode::iterate(IterCallback const& callback) const
{
    for (uint32_t i = 0, count = size(); i < count; ++i)
        if (auto node = at(i); node && !callback(*node))
            return false;
    return true;
}

auto SchemaPool::resolve(ModelNode const& node, ResolveFn const& callback) const -> tl::expected<void, Error>
{
    using Node = SchemaTypeModelNode;
    auto address = node.addr();
    auto index = address.index();
    if (address.column() < Node::Type || address.column() >= Node::End)
        return ModelPool::resolve(node, callback);
    bool valid = false;
    switch (address.column())
    {
    case Node::Type:
    case Node::Fields:
    case Node::Elements:
    case Node::Alternatives:
    case Node::Enum:
    case Node::NamedValues: valid = index < schema_->types.size(); break;
    case Node::Field:
    case Node::FieldAlternatives:
    case Node::FieldReferences: valid = index < schema_->fieldRows.size(); break;
    case Node::NamedValue: valid = index < schema_->namedRows.size(); break;
    case Node::NumericDomain:
    case Node::Intervals: valid = index < schema_->numerics.size(); break;
    case Node::Interval:
        valid = index < schema_->intervalRows.size() && schema_->intervalRows[index].minimum.type != 0;
        break;
    default: break;
    }
    if (!finalized() || !valid)
        return tl::unexpected(Error{Error::IndexOutOfRange, "Invalid or unpublished schema descriptor address"});
    callback(Node(shared_from_this(), address, mpKey_));
    return {};
}

auto SchemaPool::recursiveTraversalKey(ModelNode const& node) const noexcept -> ModelNodeAddress
{
    // A field occurrence and its sole represented type are the same recursive
    // target, even though descriptor navigation exposes different addresses.
    auto address = node.addr();
    if (address.column() == SchemaTypeModelNode::Field && address.index() < schema_->fieldRows.size())
    {
        auto children = schema_->edges.range(schema_->fieldRows[address.index()].children);
        if (children.size() == 1 && children.front())
            return ModelNodeAddress(SchemaTypeModelNode::Type, children.front());
    }
    return address;
}

template <>
auto resolveInternal(res::tag<SchemaTypeModelNode>, SchemaPool const& model, ModelNode const& node)
    -> model_ptr<SchemaTypeModelNode>
{
    if (node.addr().column() != SchemaTypeModelNode::Type || !model.finalized() ||
        !model.schemaType(TypeId(node.addr().index())) || node.addr().index() > MaxTypeId)
        return {};
    return model_ptr<SchemaTypeModelNode>::make(model.shared_from_this(), node.addr());
}

auto SchemaType::modelNode() const -> model_ptr<SchemaTypeModelNode>
{
    if (!*this || !finalized())
        throw std::logic_error("A descriptor requires a published schema type");
    return pool()->resolve<SchemaTypeModelNode>(*pool()->typeNode(id()));
}

#if defined(SIMFIL_WITH_MODEL_JSON)
/** One bounded serialization traversal; navigation never stores its ancestry or counters. */
class SchemaTypeModelNode::JsonExport
{
public:
    /** Keep the source graph alive through the caller's node, not through another model owner. */
    JsonExport(SchemaTypeModelNode const& root, JsonOptions limits) : root_(root), limits_(limits)
    {
        if (limits.maxDepth > 256)
            throw std::invalid_argument("Schema JSON depth cannot exceed 256");
    }

    /** Build a complete result or throw before exposing a partially serialized descriptor. */
    auto run() -> nlohmann::json { return write(root_, 0); }

private:
    /** Charge output conservatively; allocator overhead and arbitrary extension work are separate. */
    void charge(size_t bytes)
    {
        if (bytes > limits_.maxBytes - bytes_)
            throw std::length_error("Schema JSON byte budget exceeded");
        if (limits_.reserve && bytes)
            limits_.reserve(0, bytes);
        bytes_ += bytes;
    }

    /** Bound the number of produced values, including cycle-reference marker values. */
    void nodes(size_t count = 1)
    {
        if (count > limits_.maxNodes - nodes_)
            throw std::length_error("Schema JSON node budget exceeded");
        if (limits_.reserve && count)
            limits_.reserve(count, 0);
        nodes_ += count;
    }

    /** Account for quoting and worst-case control-character escaping before allocating JSON text. */
    void string(std::string_view text)
    {
        size_t total = 2;
        for (unsigned char c : text)
        {
            auto bytes = c < 0x20 ? 6u : c == '"' || c == '\\' ? 2u : 1u;
            if (total > limits_.maxBytes - bytes_ || bytes > limits_.maxBytes - bytes_ - total)
                throw std::length_error("Schema JSON string exceeds its byte budget");
            total += bytes;
        }
        charge(total);
    }

    /** Identify represented types separately from field-occurrence descriptor addresses. */
    auto represented(ModelNode const& node) const -> SchemaType
    {
        if (node.owningModel().get() != &root_.pool() ||
            (node.addr().column() != Type && node.addr().column() != Field))
            return {};
        auto descriptor = model_ptr<SchemaTypeModelNode>::make(node.owningModel(), node.addr());
        return descriptor->describedType();
    }

    /** Emit a terminal reference only for an ancestor, never for merely shared sibling types. */
    auto cycle(SchemaType type) -> nlohmann::json
    {
        nodes(type ? 3 : 1);
        charge(64);
        auto result = nlohmann::json{{"truncated", "cycle"}};
        if (type)
        {
            auto name = root_.pool().dictionary().resolve(SchemaType::kindNameId(type.kind())).value();
            string(name);
            result["kind"] = name;
            result["$ref"] = type.id();
        }
        return result;
    }

    /** Serialize through the ordinary protocol so application extensions share the same limits. */
    auto write(ModelNode const& node, size_t depth) -> nlohmann::json
    {
        if (depth > limits_.maxDepth)
            throw std::length_error("Schema JSON depth budget exceeded");
        nodes();
        auto category = node.type();
        if (category != ValueType::Object && category != ValueType::Array)
        {
            auto before = bytes_;
            auto value = node.value();
            std::visit([&](auto const& scalar) {
                using T = std::decay_t<decltype(scalar)>;
                if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, std::string_view>)
                    string(scalar);
                else if constexpr (std::is_same_v<T, ByteArray>)
                {
                    charge(128);
                    if (scalar.bytes.size() > (limits_.maxBytes - bytes_) / 2)
                        throw std::length_error("Schema JSON byte literal exceeds its budget");
                    charge(scalar.bytes.size() * 2);
                }
                else
                    charge(64);
            }, value);
            // Primitive export also preserves the ordinary undefined/bytes/mask tags.
            auto result = limits_.scalar ? limits_.scalar(node) : node.toJson();
            if (limits_.scalar || category == ValueType::Bitmask)
            {
                auto encoded = result.dump().size();
                if (encoded > bytes_ - before)
                    charge(encoded - (bytes_ - before));
            }
            return result;
        }

        auto type = represented(node);
        auto address = std::pair{node.owningModel().get(), node.addr()};
        if ((type && std::ranges::find(types_, type.id()) != types_.end()) ||
            std::ranges::find(addresses_, address) != addresses_.end())
            return cycle(type);
        addresses_.push_back(address);
        if (type)
            types_.push_back(type.id());
        charge(2);
        auto result = category == ValueType::Object ? nlohmann::json::object() : nlohmann::json::array();
        for (uint32_t i = 0, count = node.size(); i < count; ++i)
        {
            charge(i ? 1 : 0);
            auto child = node.at(i);
            if (!child)
                throw std::logic_error("Schema descriptor exposes an absent child");
            if (category == ValueType::Object)
            {
                auto key = node.owningModel()->lookupStringId(node.keyAt(i));
                if (!key || result.contains(*key))
                    throw std::logic_error("Schema descriptor requires unique declared field keys");
                string(*key);
                charge(1);
                result[*key] = write(*child, depth + 1);
            }
            else
                result.push_back(write(*child, depth + 1));
        }
        if (type)
            types_.pop_back();
        addresses_.pop_back();
        return result;
    }

    SchemaTypeModelNode const& root_;
    JsonOptions limits_;
    size_t bytes_ = 0, nodes_ = 0;
    std::vector<TypeId> types_;
    std::vector<std::pair<Model const*, ModelNodeAddress>> addresses_;
};

auto SchemaTypeModelNode::toJson() const -> nlohmann::json
{
    return toJson(JsonOptions{});
}

auto SchemaTypeModelNode::toJson(JsonOptions const& limits) const -> nlohmann::json
{
    return JsonExport(*this, limits).run();
}
#endif

} // namespace simfil
