// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#include "value.h"
#include "operator.h"
#include "exception-handler.h"

namespace simfil
{

/**
 * Helper class for implementing meta-types
 *
 * See `IRangeT` for an example.
 */
template <class Type>
struct TypedMetaType : MetaType
{
    TypedMetaType(std::string ident)
        : MetaType(std::move(ident))
    {}

    auto get(TransientObject& o) const -> Type*
    {
        if (o.meta == this)
            return (Type*)o.data;
        return nullptr;
    }

    auto get(const TransientObject& o) const -> const Type*
    {
        if (o.meta == this)
            return (const Type*)o.data;
        return nullptr;
    }

    auto init() const -> void*
    {
        return new Type();
    }

    auto copy(void* ptr) const -> void*
    {
        return new Type(*((Type*)ptr));
    }

    auto deinit(void* ptr) const -> void
    {
        delete (Type*)ptr;
    }

    auto unaryOp(std::string_view op, const TransientObject& obj) const -> Value
    {
        return unaryOp(op, *(const Type*)obj.data);
    }

    auto binaryOp(std::string_view op, const TransientObject& obj, const Value& v) const -> Value
    {
        return binaryOp(op, *(const Type*)obj.data, v);
    }

    auto binaryOp(std::string_view op, const Value& v, const TransientObject& obj) const -> Value
    {
        return binaryOp(op, v, *(const Type*)obj.data);
    }

    auto unpack(const TransientObject& obj, std::function<bool(Value)> fn) const -> void
    {
        return unpack(*(const Type*)obj.data, fn);
    }

    virtual auto unaryOp(std::string_view op, const Type&) const -> Value = 0;
    virtual auto binaryOp(std::string_view op, const Type&, const Value&) const -> Value = 0;
    virtual auto binaryOp(std::string_view op, const Value&, const Type&) const -> Value = 0;

    virtual auto unpack(const Type&, std::function<bool(Value)> fn) const -> void
    {
        raise<InvalidOperandsError>("...");
    }
};

template <class Type, class Meta>
auto getObject(const TransientObject& obj, const Meta* meta) -> const Type*
{
    if (obj.meta == meta)
        return (const Type*)obj.data;
    return nullptr;
}

template <class Type, class Meta>
auto getObject(const Value& val, const Meta* meta) -> const Type*
{
    if (val.isa(ValueType::TransientObject))
        return getObject<Type>(val.as<ValueType::TransientObject>(), meta);
    return nullptr;
}

}
