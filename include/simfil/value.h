#pragma once

#include <variant>
#include <string>
#include <cstdint>
#include <cassert>
#include <functional>
#include <optional>
#include <memory>
#include <vector>

#include "object.h"

namespace simfil
{

using namespace std::string_literals;

class Value;
using StringId = uint16_t;

struct ModelNode;
using ModelNodePtr = std::shared_ptr<ModelNode>;

/** Simfil search model interface */
struct ModelNode
{
    enum Type
    {
        Scalar,
        Array,
        Object,
    };

    /// Get the node's scalar value if it has one
    virtual Value value() const = 0;

    /// Get the node's abstract model type
    virtual Type type() const = 0;

    /// Get a child by name
    virtual ModelNodePtr get(const StringId &) const = 0;

    /// Get a child by index
    virtual ModelNodePtr at(int64_t) const = 0;

    /// Get an Object/Array model's children
    virtual std::vector<ModelNodePtr> children() const = 0;

    /// Get an Object model's field names
    virtual std::vector<std::string> keys() const = 0;

    /// Get the number of children
    virtual uint32_t size() const = 0;
};

/**
 * Special type to represent undefined values.
 */
struct UndefinedType {};

/**
 * Special type to represent null values.
 */
struct NullType {};

/**
 * Simfil value types
 */
enum class ValueType
{
    Undef,
    Null,
    Bool,
    Int,
    Float,
    String,
    Object,
    Model,
};

namespace impl
{
struct ValueToString
{
    auto operator()(UndefinedType) const
    {
        return "undef"s;
    }

    auto operator()(NullType) const
    {
        return "null"s;
    }

    auto operator()(bool v) const
    {
        return v ? "true"s : "false"s;
    }

    auto operator()(int64_t v) const
    {
        return std::to_string(v);
    }

    auto operator()(double v) const
    {
        return std::to_string(v);
    }

    auto operator()(const std::string& v) const
    {
        return v;
    }

    auto operator()(const Object&) const
    {
        return "<object>"s;
    }

    auto operator()(const ModelNode&) const
    {
        return "model"s;
    }
};
}


inline auto valueType2String(ValueType t) -> const char*
{
    switch (t) {
    case ValueType::Undef:  return "undef";
    case ValueType::Null:   return "null";
    case ValueType::Bool:   return "bool";
    case ValueType::Int:    return "int";
    case ValueType::Float:  return "float";
    case ValueType::String: return "string";
    case ValueType::Object: return "object";
    case ValueType::Model:  return "model";
    default:
        assert(0 && "unreachable");
        return "unknown";
    }
}

class Value;

template <class>
struct ValueType4CType;

template <>
struct ValueType4CType<bool> {
    static constexpr ValueType Type = ValueType::Bool;
};

template <>
struct ValueType4CType<int64_t> {
    static constexpr ValueType Type = ValueType::Int;
};

template <>
struct ValueType4CType<double> {
    static constexpr ValueType Type = ValueType::Float;
};

template <>
struct ValueType4CType<std::string> {
    static constexpr ValueType Type = ValueType::String;
};

template <>
struct ValueType4CType<std::string_view> {
    static constexpr ValueType Type = ValueType::String;
};

template <>
struct ValueType4CType<Object> {
    static constexpr ValueType Type = ValueType::Object;
};

template <>
struct ValueType4CType<ModelNode> {
    static constexpr ValueType Type = ValueType::Model;
};

template <ValueType>
struct ValueTypeInfo;

template <>
struct ValueTypeInfo<ValueType::Null> {
    using Type = void;
};

template <>
struct ValueTypeInfo<ValueType::Bool> {
    using Type = bool;
};

template <>
struct ValueTypeInfo<ValueType::Int> {
    using Type = int64_t;
};

template <>
struct ValueTypeInfo<ValueType::Float> {
    using Type = double;
};

template <>
struct ValueTypeInfo<ValueType::String> {
    using Type = std::string;
};

template <>
struct ValueTypeInfo<ValueType::Object> {
    using Type = Object;
};

template <>
struct ValueTypeInfo<ValueType::Model> {
    using Type = ModelNode;
};

template <ValueType _Type>
struct VauleAs {};

template <ValueType _Type>
struct ValueAs
{
    template <class _Variant>
    static auto get(const _Variant& v) -> decltype(auto)
    {
        return std::get<typename ValueTypeInfo<_Type>::Type>(v);
    }
};

template <>
struct ValueAs<ValueType::String>
{
    template <class _Variant>
    static auto get(const _Variant& v) -> std::string
    {
        if (auto str = std::get_if<std::string>(&v))
            return *str;
        if (auto str = std::get_if<std::string_view>(&v))
            return std::string(*str);
        return ""s;
    }
};


class Value
{
public:
    static auto t() -> Value
    {
        return Value(ValueType::Bool, true);
    }

    static auto f() -> Value
    {
        return Value(ValueType::Bool, false);
    }

    static auto null() -> Value
    {
        return Value(ValueType::Null);
    }

    static auto undef() -> Value
    {
        return Value(ValueType::Undef);
    }

    static auto model() -> Value
    {
        return Value(ValueType::Model);
    }

    static auto strref(std::string_view sv) -> Value
    {
        return Value::make(sv);
    }

    template <class _CType>
    static auto make(_CType&& value) -> Value
    {
        return Value(ValueType4CType<std::decay_t<_CType>>::Type, std::forward<_CType>(value));
    }

    static auto field(Value&& v, const ModelNodePtr& node)
    {
        return Value(v.type, std::move(v.value), node);
    }

    Value(ValueType type)
        : type(type)
    {}

    template <class _Type>
    Value(ValueType type, _Type&& value)
        : type(type)
        , value(std::forward<_Type>(value))
    {}

    template <class _Type>
    Value(ValueType type, _Type&& value, const ModelNodePtr& node)
        : type(type)
        , value(std::forward<_Type>(value))
        , node(node)
    {}

    Value(const Value&) = default;
    Value(Value&&) = default;
    Value& operator=(const Value&) = default;
    Value& operator=(Value&&) = default;

    auto isa(ValueType test) const
    {
        return type == test;
    }

    template <ValueType _Type>
    auto as() const -> decltype(auto)
    {
        return ValueAs<_Type>::get(value);
    }

    auto isBool(bool v) const
    {
        return isa(ValueType::Bool) && this->as<ValueType::Bool>() == v;
    }

    template <class _Visitor>
    auto visit(_Visitor fn) const
    {
        if (type == ValueType::Undef)
            return fn(UndefinedType{});
        if (type == ValueType::Null)
            return fn(NullType{});
        if (type == ValueType::Bool)
            return fn(this->template as<ValueType::Bool>());
        if (type == ValueType::Int)
            return fn(this->template as<ValueType::Int>());
        if (type == ValueType::Float)
            return fn(this->template as<ValueType::Float>());
        if (type == ValueType::String)
            return fn(this->template as<ValueType::String>());
        if (type == ValueType::Object)
            return fn(this->template as<ValueType::Object>());
        if (type == ValueType::Model) {
            if (node) return fn(*node);
            else return fn(NullType{});
        }
        return fn(UndefinedType{});
    }

    auto toString() const
    {
        if (isa(ValueType::Object)) {
            const auto& obj = std::get<Object>(value);
            if (obj.meta) {
                if (Value vv = obj.meta->unaryOp("string", obj); vv.isa(ValueType::String))
                    return vv.as<ValueType::String>();
                return "<"s + obj.meta->ident + ">"s;
            }
        }
        return visit(impl::ValueToString());
    }

    /// Get the string_view of this Value if it has one.
    std::string_view const* stringViewValue() {
        return std::get_if<std::string_view>(&value);
    }

    /// Convert the Value to a generic ModelNode
    ModelNodePtr toModelNode();

    ValueType type;
    std::variant<
        std::monostate,
        bool,
        int64_t,
        double,
        std::string,
        std::string_view,
        Object> value;

    ModelNodePtr node;
};

/** Model node with default interface implementations.
 (scalar, no children, null value). */
struct ModelNodeBase : public ModelNode
{
    Value value() const override;
    Type type() const override;
    ModelNodePtr get(const StringId &) const override;
    ModelNodePtr at(int64_t) const override;
    std::vector<ModelNodePtr> children() const override;
    std::vector<std::string> keys() const override;
    uint32_t size() const override;
};

template <class Type>
auto getNumeric(const Value& v) -> std::pair<bool, Type>
{
    if (v.isa(ValueType::Int))
        return std::make_pair(true, (Type)v.as<ValueType::Int>());
    if (v.isa(ValueType::Float))
        return std::make_pair(true, (Type)v.as<ValueType::Float>());
    return std::make_pair(false, Type{});
}

}
