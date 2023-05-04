#pragma once

#include <variant>
#include <string>
#include <cstdint>
#include <cassert>
#include <functional>
#include <optional>
#include <memory>
#include <vector>

#include "model/nodes.h"
#include "transient.h"

namespace simfil
{

using namespace std::string_literals;

/**
 * Special type to represent undefined values.
 */
struct UndefinedType {};

/**
 * Special type to represent null values.
 */
struct NullType {};

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

    auto operator()(const TransientObject&) const
    {
        return "<transient>"s;
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
    case ValueType::TransientObject: return "transient";
    case ValueType::Object: return "object";
    case ValueType::Array:  return "array";
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
struct ValueType4CType<TransientObject> {
    static constexpr ValueType Type = ValueType::TransientObject;
};

template <>
struct ValueType4CType<ModelNode> {
    static constexpr ValueType Type = ValueType::Object;
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
struct ValueTypeInfo<ValueType::TransientObject> {
    using Type = TransientObject;
};

template <>
struct ValueTypeInfo<ValueType::Object> {
    using Type = ModelNode::Ptr;
};

template <>
struct ValueTypeInfo<ValueType::Array> {
    using Type = ModelNode::Ptr;
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
        return Value(ValueType::Object);
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

    static auto field(Value&& v, const ModelNode::Ptr& node)
    {
        return Value(node->type(), std::move(v.value), node);
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
    Value(ValueType type, _Type&& value, const ModelNode::Ptr& node)
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
        if (type == ValueType::TransientObject)
            return fn(this->template as<ValueType::TransientObject>());
        if (type == ValueType::Object || type == ValueType::Array) {
            if (node) return fn(*node);
            else return fn(NullType{});
        }
        return fn(UndefinedType{});
    }

    auto toString() const
    {
        if (isa(ValueType::TransientObject)) {
            const auto& obj = std::get<TransientObject>(value);
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

    ValueType type;
    std::variant<
        std::monostate,
        bool,
        int64_t,
        double,
        std::string,
        std::string_view,
        TransientObject> value;

    ModelNode::Ptr node;
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
