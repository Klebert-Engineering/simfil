#pragma once

#include <variant>
#include <string>
#include <cstdint>
#include <cassert>
#include <bitset>

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

    auto operator()([[maybe_unused]] const ModelNode& node) const
    {
#if defined(SIMFIL_WITH_MODEL_JSON)
        return nlohmann::to_string(node.toJson());
#else
        return "<model>"s;
#endif
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
    }
    assert(0 && "unreachable"); return "unknown"; // GCOVR_EXCL_LINE
}


/**
 * Bitset of ValueTypes
 */
struct TypeFlags
{
    std::bitset<9> flags;

    auto test(ValueType type) const
    {
        return flags.test(static_cast<std::underlying_type_t<ValueType>>(type));
    }

    auto test(TypeFlags other) const
    {
        return flags & other.flags;
    }

    auto set(ValueType type, bool value = true)
    {
        flags.set(static_cast<std::underlying_type_t<ValueType>>(type), value);
    }

    auto set(TypeFlags other)
    {
        flags |= other.flags;
    }

    auto types() const -> std::vector<ValueType>;
    auto typeNames() const -> std::vector<std::string_view>;
};

class Value;

template <class>
struct ValueType4CType;

template <>
struct ValueType4CType<std::monostate> {
    static constexpr ValueType Type = ValueType::Null;
};

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

template <>
struct ValueType4CType<ModelNode::Ptr> {
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

template <ValueType ArgType>
struct ValueAs
{
    template <class VariantType>
    static inline auto get(const VariantType& v) noexcept -> decltype(auto)
    {
        return std::get<typename ValueTypeInfo<ArgType>::Type>(v);
    }
};

template <>
struct ValueAs<ValueType::String>
{
    template <class VariantType>
    static auto get(const VariantType& v) -> std::string
    {
        if (auto str = std::get_if<std::string>(&v))
            return *str;
        if (auto str = std::get_if<std::string_view>(&v))
            return std::string(*str);
        return ""s;
    }
};

template <>
struct ValueAs<ValueType::Object>
{
    template <class VariantType>
    static auto get(const VariantType& v) -> ModelNode::Ptr
    {
        if (auto nodePtr = std::get_if<ModelNode::Ptr>(&v))
            return *nodePtr;
        return {};
    }
};

template <>
struct ValueAs<ValueType::Array>
{
    template <class VariantType>
    static auto get(const VariantType& v) -> ModelNode::Ptr
    {
        if (auto nodePtr = std::get_if<ModelNode::Ptr>(&v))
            return *nodePtr;
        return {};
    }
};

class Value
{
public:
    static auto t() -> Value
    {
        return {ValueType::Bool, true};
    }

    static auto f() -> Value
    {
        return {ValueType::Bool, false};
    }

    static auto null() -> Value
    {
        return {ValueType::Null};
    }

    static auto undef() -> Value
    {
        return {ValueType::Undef};
    }

    static auto model() -> Value
    {
        return {ValueType::Object};
    }

    static auto strref(std::string_view sv) -> Value
    {
        return Value::make(sv);
    }

    template <class CType>
    static auto make(CType&& value) -> Value
    {
        return {ValueType4CType<std::decay_t<CType>>::Type, std::forward<CType>(value)};
    }

    static auto field(const ModelNode& node) -> Value
    {
        const auto type = node.type();
        if (type == ValueType::Object || type == ValueType::Array) {
            return Value{type, model_ptr<ModelNode>(node)};
        } else if (type == ValueType::String) {
            auto value = node.value();
            if (auto view = std::get_if<std::string_view>(&value))
                return Value::make(std::string(*view));
            return Value{std::move(value)};
        } else {
            return Value{node.value()};
        }
    }

    static auto field(ModelNode&& node) -> Value
    {
        const auto type = node.type();
        if (type == ValueType::Object || type == ValueType::Array) {
            return {type, model_ptr<ModelNode>(std::move(node))};
        } else if (type == ValueType::String) {
            auto value = node.value();
            if (auto view = std::get_if<std::string_view>(&value))
                return Value::make(std::string(*view));
            return Value{std::move(value)};
        } else {
            return Value{node.value()};
        }
    }

    template <class ModelNodeT>
    static auto field(const model_ptr<ModelNodeT>& node) -> Value
    {
        const auto type = node->type();
        if (type == ValueType::Object || type == ValueType::Array) {
            return {type, model_ptr<ModelNode>(node)};
        } else {
            return Value{node->value()};
        }
    }

    Value(ValueType type)  // NOLINT
        : type(type)
    {}

    template <class ArgType>
    Value(ValueType type, ArgType&& value)
        : type(type)
          , value(std::forward<ArgType>(value))
    {}


    Value(ScalarValueType&& value_)  // NOLINT
    {
        std::visit([this](auto&& v){
            type = ValueType4CType<std::decay_t<decltype(v)>>::Type;
            value = std::forward<decltype(v)>(v);
        }, value_);
    }

    Value(const Value&) = default;
    Value(Value&&) = default;
    Value& operator=(const Value&) = default;
    Value& operator=(Value&&) = default;

    [[nodiscard]] auto isa(ValueType test) const noexcept
    {
        return type == test;
    }
    
    [[nodiscard]] bool asBool() const noexcept {
        return std::get<bool>(value);
    }
    
    template <ValueType ArgType>
    [[nodiscard]] auto as() const -> decltype(auto)
    {
        return ValueAs<ArgType>::get(value);
    }

    [[nodiscard]] auto isBool(bool v) const noexcept
    {
        return type == ValueType::Bool && asBool() == v;
    }

    template <class Visitor>
    [[nodiscard]] auto visit(Visitor fn) const
    {
        switch (type) {
        case ValueType::Undef:
            return fn(UndefinedType{});
        case ValueType::Null:
            return fn(NullType{});
        case ValueType::Bool:
            return fn(asBool());
        case ValueType::Int:
            return fn(this->template as<ValueType::Int>());
        case ValueType::Float:
            return fn(this->template as<ValueType::Float>());
        case ValueType::String:
            return fn(this->template as<ValueType::String>());
        case ValueType::TransientObject:
            return fn(this->template as<ValueType::TransientObject>());
        case ValueType::Object:
        case ValueType::Array:
            if (auto nodePtr = std::get_if<ModelNode::Ptr>(&value); nodePtr && *nodePtr) [[likely]]
                return fn(**nodePtr);
            else
                return fn(NullType{});
        }
        return fn(UndefinedType{});
    }

    [[nodiscard]] auto toString() const
    {
        if (isa(ValueType::TransientObject)) {
            const auto& obj = std::get<TransientObject>(value);
            if (obj.meta) {
                if (auto vv = obj.meta->unaryOp("string", obj); vv && vv->isa(ValueType::String))
                    return vv->template as<ValueType::String>();
                return "<"s + obj.meta->ident + ">"s;
            }
        }
        return visit(impl::ValueToString());
    }

    [[nodiscard]] auto getScalar() noexcept
    {
        struct {
            void operator() (std::monostate const& v) {result = v;}
            void operator() (bool const& v) {result = v;}
            void operator() (int64_t const& v) {result = v;}
            void operator() (double const& v) {result = v;}
            void operator() (std::string const& v) {result = v;}
            void operator() (std::string_view const& v) {result = v;}
            void operator() (TransientObject const&) {}
            void operator() (ModelNode::Ptr const&) {}
            ScalarValueType result;
        } scalarVisitor;
        std::visit(scalarVisitor, value);
        return scalarVisitor.result;
    }

    std::string_view const* stringViewValue() noexcept
    {
        return std::get_if<std::string_view>(&value);
    }

    ModelNode::Ptr const* nodePtr() const noexcept
    {
        return std::get_if<ModelNode::Ptr>(&value);
    }

    ModelNode const* node() const noexcept
    {
        if (auto ptr = nodePtr(); ptr)
            return &**ptr;
        return nullptr;
    }

    ValueType type;
    std::variant<
        std::monostate,
        bool,
        int64_t,
        double,
        std::string,
        std::string_view,
        TransientObject,
        ModelNode::Ptr> value;
};

template <typename ResultT, typename ValueT>
auto getNumeric(const ValueT& v) -> std::pair<bool, ResultT>
{
    if constexpr (std::is_same_v<std::decay_t<ValueT>, Value>) {
        if (v.isa(ValueType::Int))
            return {true, (ResultT)v.template as<ValueType::Int>()};
        if (v.isa(ValueType::Float))
            return {true, (ResultT)v.template as<ValueType::Float>()};
    }
    else {
        if (std::holds_alternative<double>(v)) {
            return {true, (ResultT)std::get<double>(v)};
        }
        else if (std::holds_alternative<int64_t>(v)) {
            return {true, (ResultT)std::get<int64_t>(v)};
        }
    }
    return {false, {}};
}

}
