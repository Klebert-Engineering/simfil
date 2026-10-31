#include "simfil/value.h"
#include "simfil/model/model.h"
#include <fmt/format.h>

namespace simfil
{

auto impl::ValueToString::operator()(const Bitmask& value) const -> std::string
{
    return fmt::format("0x{:016x}", value.bits);
}

#if defined(SIMFIL_WITH_MODEL_JSON)
auto Bitmask::toJson() const -> nlohmann::json
{
    auto definition = domain.definition();
    if (!definition || !definition->isBitmask() || !definition->bitWidth() || definition->bitWidth() > 64 ||
        (definition->bitWidth() < 64 && (bits >> definition->bitWidth())))
        throw std::invalid_argument("Cannot serialize a bitmask without its domain binding");
    return {{"_bitmask", true}, {"bits", fmt::format("0x{:016x}", bits)},
            {"schema", domain.binding->identity()}, {"domain", domain.id}};
}
#endif

auto Value::schemaReference() const -> SchemaReference
{
    if (auto scalar = std::get_if<QualifiedScalar>(&value))
        return scalar->domain;
    if (auto mask = std::get_if<Bitmask>(&value))
        return mask->domain;
    if (auto source = node())
        return source->schemaReference();
    return {};
}

auto Value::withSchema(SchemaReference domain) const -> Value
{
    // Flags are intrinsically typed, so annotation cannot change their interpretation.
    if (type == ValueType::Bitmask) {
        if (domain && domain != as<ValueType::Bitmask>().domain)
            throw std::invalid_argument("Cannot rebind a bitmask to a different domain");
        return *this;
    }
    if (!domain || type == ValueType::Null || type == ValueType::Undef ||
        type == ValueType::Object || type == ValueType::Array || type == ValueType::TransientObject)
        return *this;
    auto scalar = getScalar();
    // Returned values must not borrow bytes from the source model's string column.
    if (auto view = std::get_if<std::string_view>(&scalar))
        scalar = std::string(*view);
    return {type, QualifiedScalar{std::move(scalar), std::move(domain)}};
}

auto Value::namedValue(std::string_view name) const -> tl::expected<Value, Error>
{
    auto domain = schemaReference();
    auto definition = domain.definition();
    if (!definition)
        return tl::unexpected(Error{Error::InvalidOperands, "Symbol requires an exact scalar domain: " + std::string(name)});
    auto named = definition->namedValue(name);
    if (!named)
        return tl::unexpected(Error{Error::InvalidOperands, "Unknown symbol in scalar domain: " + std::string(name)});
    return std::visit([&](auto raw) -> tl::expected<Value, Error> {
        using T = decltype(raw);
        if constexpr (std::is_same_v<T, std::uint64_t> || std::is_same_v<T, std::int64_t>) {
            if (named->role == Schema::NamedValue::Role::Flag) {
                if constexpr (std::is_same_v<T, std::int64_t>)
                    if (raw < 0)
                        return tl::unexpected(Error{Error::InvalidOperands, "Negative flag declaration"});
                if (!definition->isBitmask() || !definition->bitWidth() || definition->bitWidth() > 64 ||
                    (definition->bitWidth() < 64 && (std::uint64_t(raw) >> definition->bitWidth())))
                    return tl::unexpected(Error{Error::InvalidOperands, "Flag exceeds its domain encoding"});
                return Value::make(Bitmask{std::uint64_t(raw), domain});
            }
            if constexpr (std::is_same_v<T, std::uint64_t>)
                if (raw > std::uint64_t(std::numeric_limits<std::int64_t>::max()))
                    return tl::unexpected(Error{Error::InvalidOperands, "Unsigned symbol exceeds signed scalar representation"});
            return Value::make(std::int64_t(raw));
        }
        else if constexpr (std::is_same_v<T, std::string_view>)
            return Value::make(std::string(raw));
        else
            return Value::make(raw);
    }, named->value);
}

auto Value::field(ModelNode const& parent, ModelNode const& child, StringId field) -> Value
{
    auto result = Value::field(child);
    if (result.type == ValueType::Object || result.type == ValueType::Array ||
        result.type == ValueType::Null || result.type == ValueType::Undef || result.schemaReference())
        return result;
    auto parentDomain = parent.schemaReference();
    if (!parentDomain || !parent.owningModel())
        return result;
    auto name = parent.owningModel()->lookupStringId(field);
    auto schema = parentDomain.definition();
    if (!name || !schema)
        return result;
    auto const& strings = parentDomain.binding->strings();
    auto id = strings->get(*name);
    if (id == StringPool::Empty && !name->empty())
        return result;
    auto fields = Schema::fieldSchemas(parentDomain.id,
        [&](SchemaId childId) { return parentDomain.binding->lookup(childId); }, id);
    // A concrete parent must identify one effective domain; never guess among alternatives.
    if (fields.size() == 1 && fields.front() != NoSchemaId)
        return result.withSchema({parentDomain.binding, fields.front()});
    return result;
}

auto Value::at(ModelNode const& parent, ModelNode const& child, std::int64_t index) -> Value
{
    auto result = Value::field(child);
    if (result.type == ValueType::Object || result.type == ValueType::Array ||
        result.type == ValueType::Null || result.type == ValueType::Undef || result.schemaReference())
        return result;
    auto parentDomain = parent.schemaReference();
    auto schema = parentDomain.definition();
    if (!schema)
        return result;
    if (parent.type() == ValueType::Object)
        return field(parent, child, parent.keyAt(index));
    auto elements = Schema::itemSchemas(parentDomain.id,
        [&](SchemaId childId) { return parentDomain.binding->lookup(childId); });
    if (elements.size() == 1 && elements.front() != NoSchemaId)
        return result.withSchema({parentDomain.binding, elements.front()});
    return result;
}

auto TypeFlags::types() const -> std::vector<ValueType>
{
    std::vector<ValueType> result;
    for (uint32_t bit = 0; bit < flags.size(); ++bit)
        if (flags.test(bit))
            result.push_back(static_cast<ValueType>(bit));

    return result;
}

auto TypeFlags::typeNames() const -> std::vector<std::string_view>
{
    auto values = types();

    std::vector<std::string_view> names;
    names.resize(values.size());
    std::transform(values.begin(), values.end(), names.begin(), [](auto value) {
        return valueType2String(value);
    });

    return names;
}

}
