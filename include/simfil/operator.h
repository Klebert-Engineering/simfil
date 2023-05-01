// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#include "simfil/value.h"

#include <cstdint>
#include <string_view>
#include <string>
#include <regex>
#include <type_traits>

namespace simfil
{

using namespace std::string_literals;

/**
 * Special return type for Operator type mismatch.
 */
struct InvalidOperands {
    auto operator!() const -> InvalidOperands
    {
        return {};
    }
};

/**
 * Exception for invalid operand types.
 */
struct InvalidOperandsError : std::exception
{
    std::string operatorName;

    explicit InvalidOperandsError(std::string_view opName)
        : operatorName(opName)
    {}
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
    template <class _Type>
    auto operator()(const _Type&) const
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

    auto operator()(NullType) const
    {
        return "null"s;
    }

    auto operator()(bool) const
    {
        return "bool"s;
    }

    auto operator()(int64_t) const
    {
        return "int"s;
    }

    auto operator()(double) const
    {
        return "float"s;
    }

    auto operator()(const std::string&) const
    {
        return "string"s;
    }

    auto operator()(const ModelNode& v) const
    {
        return "model"s;
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
        long long out = 0;
        if (std::sscanf(v.c_str(), "%lld", &out) == 1)
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

    auto operator()(bool v) const
    {
        return static_cast<double>(v ? 1.0 : 0.0);
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
        double out = 0;
        if (std::sscanf(v.c_str(), "%lf", &out) == 1)
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

    auto operator()(bool v) const
    {
        return v ? "true"s : "false"s;
    }

    auto operator()(const std::string& v) const
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

    NULL_AS(""s);
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
    DECL_OPERATION(int64_t,     int64_t,     +)
    DECL_OPERATION(int64_t,     double,      +)
    DECL_OPERATION(double,      int64_t,     +)
    DECL_OPERATION(double,      double,      +)
    DECL_OPERATION(std::string, std::string, +)

    auto operator()(const std::string& l, NullType) const -> Value
    {
        return Value::null();
    }

    auto operator()(NullType, const std::string& r) const -> Value
    {
        return Value::null();
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

    auto operator()(int64_t l, int64_t r) const -> int64_t
    {
        if (r == 0)
            throw std::runtime_error("Division by zero");
        return static_cast<int64_t>(l / r);
    }

    auto operator()(int64_t l, double r) const -> double
    {
        if (r == 0)
            throw std::runtime_error("Division by zero");
        return static_cast<double>(l / r);
    }

    auto operator()(double l, int64_t r) const -> double
    {
        if (r == 0)
            throw std::runtime_error("Division by zero");
        return static_cast<double>(l / r);
    }

    auto operator()(double l, double r) const -> double
    {
        if (r == 0)
            throw std::runtime_error("Division by zero");
        return static_cast<double>(l / r);
    }
};

struct OperatorMod
{
    NAME("%")
    DENY_OTHER()
    NULL_AS_NULL()

    auto operator()(int64_t l, int64_t r) const -> int64_t
    {
        if (r == 0)
            throw std::runtime_error("Division by zero");
        return static_cast<int64_t>(l % r);
    }
};

struct OperatorEq
{
    NAME("==")
    DENY_OTHER()
    DECL_OPERATION(bool,        bool,        ==)
    DECL_OPERATION(int64_t,     int64_t,     ==)
    DECL_OPERATION(int64_t,     double,      ==)
    DECL_OPERATION(double,      int64_t,     ==)
    DECL_OPERATION(double,      double,      ==)
    DECL_OPERATION(std::string, std::string, ==)

    template <class Right>
    auto operator()(NullType, const Right&) const
    {
        return false;
    }

    template <class Left>
    auto operator()(const Left&, NullType) const
    {
        return false;
    }

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
    DENY_OTHER()
    DECL_OPERATION(int64_t,     int64_t,     <)
    DECL_OPERATION(int64_t,     double,      <)
    DECL_OPERATION(double,      int64_t,     <)
    DECL_OPERATION(double,      double,      <)
    DECL_OPERATION(std::string, std::string, <)

    template <class Right>
    auto operator()(NullType, const Right&) const
    {
        return false;
    }

    template <class Left>
    auto operator()(const Left&, NullType) const
    {
        return false;
    }

    auto operator()(NullType, NullType) const
    {
        return false;
    }
};

struct OperatorLtEq
{
    NAME("<=")
    DENY_OTHER()
    DECL_OPERATION(int64_t,     int64_t,     <=)
    DECL_OPERATION(int64_t,     double,      <=)
    DECL_OPERATION(double,      int64_t,     <=)
    DECL_OPERATION(double,      double,      <=)
    DECL_OPERATION(std::string, std::string, <=)

    template <class Right>
    auto operator()(NullType, const Right&) const
    {
        return false;
    }

    template <class Left>
    auto operator()(const Left&, NullType) const
    {
        return false;
    }

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

struct OperatorMatch
{
    NAME("=~")
    DENY_OTHER()

    auto operator()(const std::string& s, const std::string& pattern) const -> Value
    {
        std::regex re(pattern);
        return std::regex_match(s, re) ? Value::make(s) : Value::f();
    }

    NULL_AS_NULL()
};

struct OperatorNotMatch
{
    NAME("!~")
    DENY_OTHER()

    auto operator()(const std::string& s, const std::string& pattern) const -> Value
    {
        std::regex re(pattern);
        return std::regex_match(s, re) ? Value::f() : Value::make(s);
    }

    NULL_AS_NULL()
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
Value makeOperatorResult(_CType&& value)
{
    return Value::make(std::forward<_CType>(value));
}

template <class _Operator>
inline Value makeOperatorResult(Value value)
{
    return value;
}

template <class _Operator>
inline Value makeOperatorResult(InvalidOperands)
{
    throw InvalidOperandsError(_Operator::name());
}

}

template <class _Operator>
struct UnaryOperatorDispatcher
{
    static auto dispatch(const Value& value) -> Value
    {
        try {
            if (value.isa(ValueType::TransientObject)) {
                const auto& obj = value.as<ValueType::TransientObject>();
                return obj.meta->unaryOp(_Operator::name(), obj);
            }

            return value.visit(UnaryOperatorDispatcher());
        } catch (const InvalidOperandsError& err) {
            std::string ltype;
            try {
                ltype = UnaryOperatorDispatcher<OperatorTypeof>::dispatch(value).toString();
            } catch (...) {
                ltype = valueType2String(value.type);
            }
            throw std::runtime_error("Invalid operand "s + ltype +
                                     " for operator "s + std::string(err.operatorName));
        }
    }

    auto operator()(UndefinedType) -> Value
    {
        return Value::undef();
    }

    template <class _Value>
    auto operator()(const _Value& rhs) -> Value
    {
        return impl::makeOperatorResult<_Operator>(_Operator()(rhs));
    }
};

template <class _Left, class _Operator>
struct BinaryOperatorDispatcherRHS
{
    const _Left& lhs;
    BinaryOperatorDispatcherRHS(const _Left& lhs)
        : lhs(lhs)
    {}

    auto operator()(UndefinedType) -> Value
    {
        return Value::undef();
    }

    template <class _Right>
    auto operator()(const _Right& rhs) -> Value
    {
        return impl::makeOperatorResult<_Operator>(_Operator()(lhs, rhs));
    }
};

template <class _Operator>
struct BinaryOperatorDispatcher
{
    const Value& value;

    static auto dispatch(const Value& lhs, const Value& rhs) -> Value
    {
        try {
            if (lhs.isa(ValueType::TransientObject)) {
                if (rhs.isa(ValueType::Undef))
                    return Value::undef();
                const auto& obj = lhs.as<ValueType::TransientObject>();
                return obj.meta->binaryOp(_Operator::name(), obj, rhs);
            }

            if (rhs.isa(ValueType::TransientObject)) {
                if (lhs.isa(ValueType::Undef))
                    return Value::undef();
                const auto& obj = rhs.as<ValueType::TransientObject>();
                return obj.meta->binaryOp(_Operator::name(), lhs, obj);
            }

            return lhs.visit(BinaryOperatorDispatcher<_Operator>(rhs));
        } catch (const InvalidOperandsError& err) {
            std::string ltype, rtype;
            try {
                ltype = UnaryOperatorDispatcher<OperatorTypeof>::dispatch(lhs).toString();
                rtype = UnaryOperatorDispatcher<OperatorTypeof>::dispatch(rhs).toString();
            } catch (...) {
                ltype = valueType2String(lhs.type);
                rtype = valueType2String(rhs.type);
            }
            throw std::runtime_error("Invalid operands "s + ltype +
                                     " and "s + rtype +
                                     " for operator "s + std::string(err.operatorName));
        }
    }

    BinaryOperatorDispatcher(const Value& value)
        : value(value)
    {}

    auto operator()(UndefinedType) -> Value
    {
        return Value::undef();
    }

    template <class _Left>
    auto operator()(const _Left& lhs) -> Value
    {
        return value.visit(BinaryOperatorDispatcherRHS<_Left, _Operator>(lhs));
    }
};

}
