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
    auto definition = type.definition();
    if (!definition || !definition->isBitmask() || !definition->bitWidth() || definition->bitWidth() > 64 ||
        (definition->bitWidth() < 64 && (bits >> definition->bitWidth())))
        throw std::invalid_argument("Cannot serialize a bitmask without its type binding");
    return {{"_bitmask", true}, {"bits", fmt::format("0x{:016x}", bits)},
            {"schema", type.binding->identity()}, {"typeId", type.id}};
}
#endif

auto Value::schemaReference() const -> SchemaReference
{
    if (auto scalar = std::get_if<QualifiedScalar>(&value))
        return scalar->type;
    if (auto mask = std::get_if<Bitmask>(&value))
        return mask->type;
    if (auto source = node())
        return source->schemaReference();
    return {};
}

auto Value::withSchema(SchemaReference typeRef) const -> Value
{
    // Flags are intrinsically typed, so annotation cannot change their interpretation.
    if (type == ValueType::Bitmask) {
        if (typeRef && typeRef != as<ValueType::Bitmask>().type)
            throw std::invalid_argument("Cannot rebind a bitmask to a different type");
        return *this;
    }
    if (!typeRef || type == ValueType::Null || type == ValueType::Undef ||
        type == ValueType::Object || type == ValueType::Array || type == ValueType::TransientObject)
        return *this;
    return fromScalar(getScalar(), std::move(typeRef));
}

auto Value::namedValue(std::string_view name) const -> tl::expected<Value, Error>
{
    auto typeRef = schemaReference();
    auto definition = typeRef.definition();
    if (!definition)
        return tl::unexpected(Error{Error::InvalidOperands, "Symbol requires an exact scalar type: " + std::string(name)});
    auto named = definition->namedValue(name);
    if (!named)
        return tl::unexpected(Error{Error::InvalidOperands, "Unknown symbol in scalar type: " + std::string(name)});
    return std::visit([&](auto raw) -> tl::expected<Value, Error> {
        using T = decltype(raw);
        if constexpr (std::is_same_v<T, std::uint64_t> || std::is_same_v<T, std::int64_t>) {
            if (named->role == Schema::NamedValue::Role::Flag) {
                if constexpr (std::is_same_v<T, std::int64_t>)
                    if (raw < 0)
                        return tl::unexpected(Error{Error::InvalidOperands, "Negative flag declaration"});
                if (!definition->isBitmask() || !definition->bitWidth() || definition->bitWidth() > 64 ||
                    (definition->bitWidth() < 64 && (std::uint64_t(raw) >> definition->bitWidth())))
                    return tl::unexpected(Error{Error::InvalidOperands, "Flag exceeds its type encoding"});
                return Value::make(Bitmask{std::uint64_t(raw), typeRef});
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

auto Value::fieldTypeReference(ModelNode const& parent, StringId field) -> SchemaReference
{
    auto parentType = parent.schemaReference();
    if (parentType) {
        auto owner = parent.owningModel();
        auto name = owner ? owner->lookupStringId(field) : std::nullopt;
        auto schema = parentType.definition();
        if (name && schema) {
            auto id = parentType.binding->strings()->get(*name);
            // Missing names must not alias a declared empty field.
            if (id != StringPool::Empty || name->empty())
                return {std::move(parentType.binding), schema->fieldSchema(id)};
        }
    }
    return {};
}

auto Value::at(ModelNode const& parent, ModelNode const& child, std::int64_t index) -> Value
{
    // Object indexes have actual keys; avoid scalarizing the child twice.
    if (parent.type() == ValueType::Object)
        return field(parent, child, parent.keyAt(index));
    return field(child, [&]() -> SchemaReference {
        auto parentType = parent.schemaReference();
        auto schema = parentType.definition();
        return schema ? SchemaReference{std::move(parentType.binding), schema->itemSchema()} : SchemaReference{};
    });
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
