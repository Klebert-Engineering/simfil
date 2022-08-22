// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#include <string>
#include <string_view>

namespace simfil
{

using namespace std::string_literals;

class Value;
struct Object;

struct MetaType
{
    /* NOTE: If you want to implement fields for objects:
     *       - Remove the checks from the parser (Path, Subscript & Subexpr)
     *       - Make Value::node optionally owning its ptr
     *       - Set ModelNode instance for objects you create */

    MetaType(std::string ident)
        : ident(std::move(ident))
    {}

    const std::string ident;

    /* Memory management */
    virtual auto init() const -> void* = 0;
    virtual auto copy(void*) const -> void* = 0;
    virtual auto deinit(void*) const -> void = 0;

    /* Operators */
    virtual auto unaryOp(std::string_view op, const Object&) const -> Value = 0;
    virtual auto binaryOp(std::string_view op, const Object&, const Value&) const -> Value = 0;
    virtual auto binaryOp(std::string_view op, const Value&, const Object&) const -> Value = 0;

    /* Unpack */
    virtual auto unpack(const Object&, std::function<bool(Value)>) const -> void = 0; /* cb return false to stop! */
};

struct Object
{
    const MetaType* meta = nullptr;
    void* data = nullptr;

    Object(const Object& other)
        : meta(other.meta), data(meta->copy(other.data))
    {}

    auto operator=(const Object& other)
    {
        meta->deinit(data);
        meta = other.meta;
        data = meta->copy(other.data);
        return *this;
    }

    Object(Object&& other)
        : meta(std::move(other.meta))
        , data(std::move(other.data))
    {
        other.meta = nullptr;
        other.data = nullptr;
    }

    auto operator=(Object&& other)
    {
        meta->deinit(data);
        meta = std::move(other.meta);
        data = std::move(other.data);
        other.meta = nullptr;
        other.data = nullptr;
        return *this;
    }

    explicit Object(const MetaType* meta)
        : meta(meta), data(meta->init())
    {}

    ~Object()
    {
        if (meta && data) {
            meta->deinit(data);
            data = nullptr;
            meta = nullptr;
        }
    }
};

}
