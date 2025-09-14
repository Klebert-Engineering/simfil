// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#include "simfil/error.h"
#include "simfil/value.h"
#include "exception-handler.h"
#include "fmt/format.h"

#include <tl/expected.hpp>
#include <cstdint>
#include <string_view>
#include <string>
#include <iostream>

namespace simfil
{

using namespace std::string_literals;

/**
 * Special return type for operator type mismatch.
 */
struct InvalidOperands {
    auto operator!() const -> InvalidOperands
    {
        return {};
    }
};

#define NAME(str)                               \
    static const char * name() {                \
        return str;                             \
    }

/* Unary Operators */

#define DENY_OTHER()                                        \
    template <class Type>                                   \
    auto operator()(const Type&) const -> InvalidOperands   \
    {                                                       \
        return {};                                          \
    }

#define DECL_OPERATION(type_a, op)                      \
    auto operator()(type_a a) const -> decltype(op a)   \
    {                                                   \
        return op a;                                    \
    }

#define NULL_AS(x)                                       \
    auto operator()(NullType) const                      \
    {                                                    \
        return (*this)(x);                               \
    }                                                    \

#define NULL_AS_NULL()                                   \
    auto operator()(NullType) const                      \
    {                                                    \
        return Value::null();                            \
    }                                                    \

#define INT_AS_UINT()                           \
    auto operator()(int64_t v) const -> int64_t \
    {                                           \
        uint64_t uv = *(uint64_t*)&v;           \
        uint64_t res = (*this)(uv);             \
        return *(int64_t*)&res;                 \
    }

struct OperatorNegate
{
    NAME("-")
    DENY_OTHER()
    DECL_OPERATION(int64_t, -)
    DECL_OPERATION(double,  -)
};

struct OperatorBool
{
    NAME("?")

    /* Everything but `false` and `null` are `true`. */
    template <class Type>
    auto operator()(const Type&) const
    {
        return true;
    }

    auto operator()(NullType) const
    {
        return false;
    }

    auto operator()(bool v) const
    {
        return v;
    }
};

struct OperatorNot
{
    NAME("not")

    template <class _Type>
    auto operator()(const _Type& value) const
    {
        return !OperatorBool()(value);
    }
};

struct OperatorBitInv
{
    NAME("~")
    DENY_OTHER()
    INT_AS_UINT()
    DECL_OPERATION(uint64_t, ~)
    NULL_AS_NULL()
};

struct OperatorLen
{
    NAME("#")
    DENY_OTHER()

    auto operator()(const std::string& s) const
    {
        return static_cast<int64_t>(s.size());
    }

    auto operator()(const ModelNode& n) const
    {
        return static_cast<int64_t>(n.size());
    }

    NULL_AS_NULL()
};

struct OperatorTypeof
{
    NAME("typeof")

    auto operator()(NullType) const -> const std::string&
    {
        static auto n = "null"s;
        return n;
    }

    auto operator()(bool) const -> const std::string&
    {
        static auto n = "bool"s;
        return n;
    }

    auto operator()(int64_t) const -> const std::string&
    {
        static auto n = "int"s;
        return n;
    }

    auto operator()(double) const -> const std::string&
    {
        static auto n = "float"s;
        return n;
    }

    auto operator()(const std::string&) const -> const std::string&
    {
        static auto n = "string"s;
        return n;
    }

    auto operator()(const ModelNode& v) const -> const std::string&
    {
        static auto n = "model"s;
        return n;
    }

    auto operator()(const TransientObject& v) const
    {
        // Handled by MetaType::unaryOp
        return ""s;
    }
};

struct OperatorAsInt
{
    NAME("int")
    DENY_OTHER()

    auto operator()(bool v) const
    {
        return static_cast<int64_t>(v ? 1 : 0);
    }

    auto operator()(int64_t v) const
    {
        return v;
    }

    auto operator()(double v) const
    {
        return static_cast<int64_t>(v);
    }

    auto operator()(const std::string& v) const
    {
        if (long long out = 0; std::sscanf(v.c_str(), "%lld", &out) == 1)
            return (int64_t)out;
        return (int64_t)0;
    }

    auto operator()(const ModelNode&) const
    {
        return (int64_t)0;
    }

    NULL_AS((int64_t)0)
};

struct OperatorAsFloat
{
    NAME("float")
    DENY_OTHER()

    auto operator()(bool v) const -> double
    {
        return v ? 1.0 : 0.0;
    }

    auto operator()(int64_t v) const
    {
        return static_cast<double>(v);
    }

    auto operator()(double v) const
    {
        return v;
    }

    auto operator()(const std::string& v) const
    {
        if (double out = 0; std::sscanf(v.c_str(), "%lf", &out) == 1)
            return out;
        return 0.0;
    }

    auto operator()(const ModelNode&) const
    {
        return (int64_t)0.0;
    }

    NULL_AS((double)0)
};

struct OperatorAsString
{
    NAME("string")

    auto operator()(bool v) const -> const std::string&
    {
        static auto tn = "true"s;
        static auto fn = "false"s;
        return v ? tn : fn;
    }

    auto operator()(const std::string& v) const -> const std::string&
    {
        return v;
    }

    auto operator()(const ModelNode& v) const
    {
        return ""s;
    }

    auto operator()(const TransientObject& v) const
    {
        // Handled by MetaType::unaryOp
        return ""s;
    }

    template <class Type>
    auto operator()(Type v) const
    {
        return std::to_string(v);
    }

    NULL_AS("null"s);
};

#undef DENY_OTHER
#undef NULL_AS
#undef NULL_AS_NULL
#undef DECL_OPERATION
#undef INT_AS_UINT

/* Binary Operators */

#define DENY_OTHER()                                                    \
    template <class Left, class Right>                                  \
    auto operator()(const Left&, const Right&) const -> InvalidOperands \
    {                                                                   \
        return {};                                                      \
    }

#define IGNORE_INCOMPATIBLE()                                           \
    template <class Left, class Right>                                  \
    auto operator()(const Left&, const Right&) const                    \
    {                                                                   \
        return false;                                                   \
    }

#define NULL_AS_NULL()                              \
    template <class Right>                          \
    auto operator()(NullType, const Right& b) const \
    {                                               \
        return Value::null();                       \
    }                                               \
    template <class Left>                           \
    auto operator()(const Left& a, NullType) const  \
    {                                               \
        return Value::null();                       \
    }                                               \
    auto operator()(NullType, NullType) const       \
    {                                               \
        return Value::null();                       \
    }

#define INT_AS_UINT()                                       \
    auto operator()(int64_t l, int64_t r) const -> int64_t  \
    {                                                       \
        uint64_t ul = *(uint64_t*)&l;                       \
        uint64_t ur = *(uint64_t*)&r;                       \
        uint64_t res = (*this)(ul, ur);                     \
        return *(int64_t*)&res;                             \
    }

#define DECL_OPERATION(type_a, type_b, op)                          \
    auto operator()(type_a a, type_b b) const -> decltype(a op b)   \
    {                                                               \
        return a op b;                                              \
    }


struct OperatorAdd
{
    NAME("+")
    DENY_OTHER()
    NULL_AS_NULL()
    DECL_OPERATION(int64_t,            int64_t,            +)
    DECL_OPERATION(int64_t,            double,             +)
    DECL_OPERATION(double,             int64_t,            +)
    DECL_OPERATION(double,             double,             +)
    DECL_OPERATION(const std::string&, const std::string&, +)

    auto operator()(const std::string& l, NullType) const -> Value
    {
        return Value::make(l + "null"s);
    }

    auto operator()(NullType, const std::string& r) const -> Value
    {
        return Value::make("null"s + r);
    }

    auto operator()(const std::string& l, const ModelNode&) const -> Value
    {
        return Value::null();
    }

    auto operator()(const ModelNode& l, const ModelNode&) const -> Value
    {
        return Value::null();
    }

    auto operator()(const ModelNode&, const std::string& r) const -> Value
    {
        return Value::null();
    }

    template <class Right>
    auto operator()(const std::string& l, const Right& r) const -> std::string
    {
        return l + OperatorAsString()(r);
    }

    template <class Left>
    auto operator()(const Left& l, const std::string& r) const -> std::string
    {
        return OperatorAsString()(l) + r;
    }
};

struct OperatorSub
{
    NAME("-")
    DENY_OTHER()
    NULL_AS_NULL()
    DECL_OPERATION(int64_t,     int64_t,     -)
    DECL_OPERATION(int64_t,     double,      -)
    DECL_OPERATION(double,      int64_t,     -)
    DECL_OPERATION(double,      double,      -)
};

struct OperatorMul
{
    NAME("*")
    DENY_OTHER()
    NULL_AS_NULL()
    DECL_OPERATION(int64_t,     int64_t,     *)
    DECL_OPERATION(int64_t,     double,      *)
    DECL_OPERATION(double,      int64_t,     *)
    DECL_OPERATION(double,      double,      *)
};

struct OperatorDiv
{
    NAME("/")
    DENY_OTHER()
    NULL_AS_NULL()

    auto operator()(int64_t l, int64_t r) const -> tl::expected<int64_t, Error>
    {
        if (r == 0)
            return tl::unexpected<Error>(Error::DivisionByZero, "Division by zero");
        return static_cast<int64_t>(l / r);
    }

    auto operator()(int64_t l, double r) const -> tl::expected<double, Error>
    {
        if (r == 0)
            return tl::unexpected<Error>(Error::DivisionByZero, "Division by zero");
        return static_cast<double>(l / r);
    }

    auto operator()(double l, int64_t r) const -> tl::expected<double, Error>
    {
        if (r == 0)
            return tl::unexpected<Error>(Error::DivisionByZero, "Division by zero");
        return static_cast<double>(l / r);
    }

    auto operator()(double l, double r) const -> tl::expected<double, Error>
    {
        if (r == 0)
            return tl::unexpected<Error>(Error::DivisionByZero, "Division by zero");
        return static_cast<double>(l / r);
    }
};

struct OperatorMod
{
    NAME("%")
    DENY_OTHER()
    NULL_AS_NULL()

    auto operator()(int64_t l, int64_t r) const -> tl::expected<int64_t, Error>
    {
        if (r == 0)
            return tl::unexpected<Error>(Error::DivisionByZero, "Division by zero");
        return static_cast<int64_t>(l % r);
    }
};

struct OperatorEq
{
    NAME("==")
    IGNORE_INCOMPATIBLE()
    DECL_OPERATION(bool,               bool,               ==)
    DECL_OPERATION(int64_t,            int64_t,            ==)
    DECL_OPERATION(int64_t,            double,             ==)
    DECL_OPERATION(double,             int64_t,            ==)
    DECL_OPERATION(double,             double,             ==)
    DECL_OPERATION(const std::string&, const std::string&, ==)

    auto operator()(NullType, NullType) const
    {
        return true;
    }
};

struct OperatorNeq
{
    NAME("!=")

    template <class Left, class Right>
    auto operator()(const Left& l, const Right& r) const
    {
        return !OperatorEq()(l, r);
    }
};

struct OperatorLt
{
    NAME("<")
    IGNORE_INCOMPATIBLE()
    DECL_OPERATION(int64_t,            int64_t,            <)
    DECL_OPERATION(int64_t,            double,             <)
    DECL_OPERATION(double,             int64_t,            <)
    DECL_OPERATION(double,             double,             <)
    DECL_OPERATION(const std::string&, const std::string&, <)

    auto operator()(NullType, NullType) const
    {
        return false;
    }
};

struct OperatorLtEq
{
    NAME("<=")
    IGNORE_INCOMPATIBLE()
    DECL_OPERATION(int64_t,            int64_t,            <=)
    DECL_OPERATION(int64_t,            double,             <=)
    DECL_OPERATION(double,             int64_t,            <=)
    DECL_OPERATION(double,             double,             <=)
    DECL_OPERATION(const std::string&, const std::string&, <=)

    auto operator()(NullType, NullType) const
    {
        return true;
    }
};

struct OperatorGt
{
    NAME(">")

    template<class Left, class Right>
    auto operator()(const Left& l, const Right& r) const
    {
        return OperatorLt()(r, l);
    }
};

struct OperatorGtEq
{
    NAME(">=")

    template<class Left, class Right>
    auto operator()(const Left& l, const Right& r) const
    {
        return OperatorLtEq()(r, l);
    }
};

struct OperatorSubscript
{
    NAME("[]")
    DENY_OTHER();

    auto operator()(const std::string& s, int64_t idx) const -> Value
    {
        if ((size_t)idx < s.size() && idx >= 0)
            return Value::make(s.substr(idx, 1));
        return Value::null();
    }

    NULL_AS_NULL()
};

/* NOTE: And and Or are implemented as expressions, because of lazy evaluation. */

struct OperatorBitAnd
{
    NAME("&")
    NULL_AS_NULL()
    INT_AS_UINT()
    DENY_OTHER()
    DECL_OPERATION(uint64_t,     uint64_t,     &)
};

struct OperatorBitOr
{
    NAME("|")
    NULL_AS_NULL()
    INT_AS_UINT()
    DENY_OTHER()
    DECL_OPERATION(uint64_t,     uint64_t,     |)
};

struct OperatorBitXor
{
    NAME("^")
    NULL_AS_NULL()
    INT_AS_UINT()
    DENY_OTHER()
    DECL_OPERATION(uint64_t,     uint64_t,     ^)
};

struct OperatorShl
{
    NAME("<<")
    NULL_AS_NULL()
    INT_AS_UINT()
    DENY_OTHER()
    DECL_OPERATION(uint64_t,     uint64_t,     <<)
};

struct OperatorShr
{
    NAME(">>")
    NULL_AS_NULL()
    INT_AS_UINT()
    DENY_OTHER()
    DECL_OPERATION(uint64_t,     uint64_t,     >>)
};

#undef DENY_OTHER
#undef DECL_OPERATION
#undef NULL_AS
#undef NULL_AS_NULL
#undef NAME
#undef INT_AS_UINT

namespace impl
{

template <class _Operator, class _CType>
inline auto makeOperatorResult(_CType&& value) -> tl::expected<Value, Error>
{
    return Value::make(std::forward<_CType>(value));
}

template <class _Operator>
inline auto makeOperatorResult(Value value) -> tl::expected<Value, Error>
{
    return value;
}

template <class _Operator, class _CType>
inline auto makeOperatorResult(tl::expected<_CType, Error> value) -> tl::expected<Value, Error>
{
    if (!value)
        return tl::unexpected<Error>(std::move(value.error()));
    return Value::make(std::forward<_CType>(std::move(value.value())));
}

template <class _Operator>
inline auto makeOperatorResult(InvalidOperands) -> tl::expected<Value, Error>
{
    return tl::unexpected<Error>(Error::InvalidOperands, fmt::format("Invalid operands for operator '{}'", _Operator::name()));
}

}

template <class _Operator>
struct UnaryOperatorDispatcher
{
    static auto dispatch(const Value& value) -> tl::expected<Value, Error>
    {
        if (value.isa(ValueType::TransientObject)) {
            const auto& obj = value.as<ValueType::TransientObject>();
            return obj.meta->unaryOp(_Operator::name(), obj);
        }

        return value.visit(UnaryOperatorDispatcher());
    }

    auto operator()(UndefinedType) -> tl::expected<Value, Error>
    {
        return Value::undef();
    }

    template <class _Value>
    auto operator()(const _Value& rhs) -> tl::expected<Value, Error>
    {
        return impl::makeOperatorResult<_Operator>(_Operator()(rhs));
    }
};

template <class Left, class Operator>
struct BinaryOperatorDispatcherRHS
{
    const Left& lhs;
    explicit BinaryOperatorDispatcherRHS(const Left& lhs)
        : lhs(lhs)
    {}

    auto operator()(UndefinedType) -> tl::expected<Value, Error>
    {
        return Value::undef();
    }

    template <class Right>
    auto operator()(const Right& rhs) -> tl::expected<Value, Error>
    {
        return impl::makeOperatorResult<Operator>(Operator()(lhs, rhs));
    }
};

template <class Operator>
struct BinaryOperatorDispatcher
{
    const Value& value;

    static auto dispatch(const Value& lhs, const Value& rhs) -> tl::expected<Value, Error>
    {
        auto result = ([&]() -> tl::expected<Value, Error> {
            if (lhs.isa(ValueType::TransientObject)) {
                if (rhs.isa(ValueType::Undef)) {
                    return Value::undef();
                } else {
                    const auto& obj = lhs.as<ValueType::TransientObject>();
                    return obj.meta->binaryOp(Operator::name(), obj, rhs);
                }
            }
            else if (rhs.isa(ValueType::TransientObject)) {
                if (lhs.isa(ValueType::Undef)) {
                    return Value::undef();
                } else {
                    const auto& obj = rhs.as<ValueType::TransientObject>();
                    return obj.meta->binaryOp(Operator::name(), lhs, obj);
                }
            }
            else {
                return lhs.visit(BinaryOperatorDispatcher<Operator>(rhs));
            }
        })();

        if (!result) {
            // Try to find the operand types
            auto& error = result.error();
            if (error.type == Error::InvalidOperands) {
                auto ltype = UnaryOperatorDispatcher<OperatorTypeof>::dispatch(lhs).value_or(Value::strref("unknown")).toString();
                auto rtype = UnaryOperatorDispatcher<OperatorTypeof>::dispatch(rhs).value_or(Value::strref("unknown")).toString();
                return tl::unexpected<Error>(Error::InvalidOperands,
                                             fmt::format("Invalid operands {} and {} for operator {}", ltype, rtype, Operator::name()));
            }
        }

        return result;
    }

    explicit BinaryOperatorDispatcher(const Value& value)
        : value(value)
    {}

    auto operator()(UndefinedType) -> tl::expected<Value, Error>
    {
        return Value::undef();
    }

    template <class Left>
    auto operator()(const Left& lhs) -> tl::expected<Value, Error>
    {
        return value.visit(BinaryOperatorDispatcherRHS<Left, Operator>(lhs));
    }
};

}
