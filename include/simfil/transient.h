// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#include <string>
#include <string_view>
#include <tl/expected.hpp>

#include "error.h"

namespace simfil
{

using namespace std::string_literals;

class Value;
struct TransientObject;

struct MetaType
{
    /* NOTE: If you want to implement fields for objects:
     *       - Remove the checks from the parser (Path, Subscript & Subexpr)
     *       - Make Value::node optionally owning its ptr
     *       - Set ModelNode instance for objects you create */

    explicit MetaType(std::string ident)
        : ident(std::move(ident))
    {}

    const std::string ident;

    /* Memory management */
    virtual auto init() const -> void* = 0;
    virtual auto copy(void*) const -> void* = 0;
    virtual auto deinit(void*) const -> void = 0;

    /* Operators */
    virtual auto unaryOp(std::string_view op, const TransientObject&) const -> tl::expected<Value, Error> = 0;
    virtual auto binaryOp(std::string_view op, const TransientObject&, const Value&) const -> tl::expected<Value, Error> = 0;
    virtual auto binaryOp(std::string_view op, const Value&, const TransientObject&) const -> tl::expected<Value, Error> = 0;

    /* Unpack */
    virtual auto unpack(const TransientObject&, std::function<bool(Value)>) const -> tl::expected<void, Error> = 0; /* cb return false to stop! */
};

/**
 * Transient object, which may be created during the execution
 * of a simfil statement, for example to represent geometry.
 */
struct TransientObject
{
    const MetaType* meta = nullptr;
    void* data = nullptr;

    TransientObject(const TransientObject& other)
        : meta(other.meta), data(meta->copy(other.data))
    {}

    auto operator=(const TransientObject& other)
    {
        meta->deinit(data);
        meta = other.meta;
        data = meta->copy(other.data);
        return *this;
    }

    TransientObject(TransientObject&& other) noexcept
        : meta(other.meta)
        , data(other.data)
    {
        other.meta = nullptr;
        other.data = nullptr;
    }

    TransientObject& operator=(TransientObject&& other) noexcept
    {
        meta->deinit(data);
        meta = other.meta;
        data = other.data;
        other.meta = nullptr;
        other.data = nullptr;
        return *this;
    }

    explicit TransientObject(const MetaType* meta)
        : meta(meta), data(meta->init())
    {}

    ~TransientObject()
    {
        if (meta && data) {
            meta->deinit(data);
            data = nullptr;
            meta = nullptr;
        }
    }
};

}
