// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#include "value.h"

#include <algorithm>
#include <memory>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <shared_mutex>

namespace simfil
{

/** Simfil search model interface */
class ModelNode
{
public:
    enum Type
    {
        Scalar,
        Array,
        Object,
    };

    virtual ~ModelNode() = default;

    /** Node information */
    virtual auto value() const -> Value = 0;
    virtual auto type() const -> Type = 0;

    /** Child access */
    virtual auto get(const std::string_view &) const -> const ModelNode* = 0;
    virtual auto get(int64_t) const -> const ModelNode* = 0;
    virtual auto children() const -> std::vector<const ModelNode*> = 0;
    virtual auto keys() const -> std::vector<std::string_view> = 0;
    virtual auto size() const -> int64_t = 0;

    ModelNode() = default;
    ModelNode(const ModelNode&) = delete;
    ModelNode& operator=(const ModelNode&) = delete;
    ModelNode(ModelNode&&) = default;
    ModelNode& operator=(ModelNode&&) = default;
};

/** Scalar implementation */
class ScalarNode : public ModelNode
{
public:
    ScalarNode()
        : scalar(Value::null())
    {}

    ScalarNode(Value&& v)
        : scalar(std::move(v))
    {}

    auto value() const -> Value override
    {
        return scalar;
    }

    auto type() const -> Type override
    {
        return Type::Scalar;
    }

    auto get(const std::string_view &) const ->  const ModelNode* override
    {
        return nullptr;
    }

    auto get(int64_t) const ->  const ModelNode* override
    {
        return nullptr;
    }

    auto children() const -> std::vector<const ModelNode*> override
    {
        return {};
    }

    auto keys() const -> std::vector<std::string_view> override
    {
        return {};
    }

    auto size() const -> int64_t override
    {
        return 0;
    }

    Value scalar;
};

class ObjectNode : public ModelNode
{
public:
    auto value() const -> Value
    {
        return Value::null();
    }

    auto type() const -> Type
    {
        return ModelNode::Type::Object;
    }

    auto get(const std::string_view & key) const -> const ModelNode*
    {
        if (auto i = nodes_.find(key); i != nodes_.end())
            return i->second;
        return nullptr;
    }

    auto get(int64_t) const -> const ModelNode*
    {
        return nullptr;
    }

    auto children() const -> std::vector<const ModelNode*>
    {
        std::vector<const ModelNode*> nodes(nodes_.size(), nullptr);
        std::transform(nodes_.begin(), nodes_.end(), nodes.begin(), [](const auto& pair) {
            return pair.second;
        });

        return nodes;
    }

    auto keys() const -> std::vector<std::string_view>
    {
        std::vector<std::string_view> names(nodes_.size(), std::string{});
        std::transform(nodes_.begin(), nodes_.end(), names.begin(), [](const auto& pair) {
            return pair.first;
        });

        return names;
    }

    auto size() const -> int64_t
    {
        return nodes_.size();
    }

    std::unordered_map<std::string_view, ModelNode*> nodes_;
};

class ArrayNode : public ModelNode
{
public:
    auto value() const -> Value
    {
        return Value::null();
    }

    auto type() const -> Type
    {
        return ModelNode::Type::Array;
    }

    auto get(const std::string_view &) const -> const ModelNode*
    {
        return nullptr;
    }

    auto get(int64_t idx) const -> const ModelNode*
    {
        if (0 <= idx && (size_t)idx <= nodes_.size())
            return nodes_[idx];
        return nullptr;
    }

    auto children() const -> std::vector<const ModelNode*>
    {
       return {nodes_.begin(), nodes_.end()};
    }

    auto keys() const -> std::vector<std::string_view>
    {
        return {};
    }

    auto size() const -> int64_t
    {
        return nodes_.size();
    }

    std::vector<ModelNode*> nodes_;
};

class VertexNode : public ModelNode
{
public:
    VertexNode() = default;
    VertexNode(double lon, double lat) : lon(Value::make(lon)), lat(Value::make(lat)) {}

    auto type() const -> ModelNode::Type override {return ModelNode::Type::Array; }

    auto value() const -> Value override { return Value::null(); }

    auto get(const std::string_view & key) const -> const ModelNode* override
    {
        if (key == "lon")
            return &lon;
        if (key == "lat")
            return &lat;
        return nullptr;
    }

    auto get(int64_t idx) const -> const ModelNode* override
    {
        switch (idx) {
        case 0: return &lon;
        case 1: return &lat;
        default: return nullptr;
        }
    }

    auto children() const -> std::vector<const ModelNode*> override { return {{&lon, &lat}}; }

    auto keys() const -> std::vector<std::string_view> override { return {{"lon"s, "lat"s}}; }

    auto size() const -> int64_t override { return 2; }

    ScalarNode lon;
    ScalarNode lat;
};

/// Efficient storage of SIMFIL model nodes. This way, a whole
/// ModelNode tree can be cleaned up in constant time. The nodes
/// reference each other via raw pointers. Because decks are used,
/// the pointers stay valid as the containers grow.
struct Model
{
    /// Fast and efficient string storage -
    /// referenced by object keys and string values.
    struct Strings
    {
    private:
        std::unordered_set<std::string> strings_;
        std::shared_mutex stringStoreMutex_;

    public:
        /// Use this function to lookup a stored string, or insert it
        /// if it doesn't exist yet. Unfortunately, we can't use string_view
        /// as lookup type until C++ 20 is used:
        ///   http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2018/p0919r2.html
        std::string_view getOrInsert(std::string const& str)
        {
            {
                std::shared_lock stringStoreReadAccess_(stringStoreMutex_);
                auto it = strings_.find(str);
                if (it != strings_.end()) {
                    return *it;
                }
            }
            {
                std::unique_lock stringStoreWriteAccess_(stringStoreMutex_);
                auto [it, _] = strings_.emplace(str);
                return *it;
            }
        }

        /// Get the count of strings currently stored
        size_t size() {
            std::shared_lock stringStoreReadAccess_(stringStoreMutex_);
            return strings_.size();
        }
    };

    /// No copies allowed...
    Model(Model const&) = delete;

    /// Default ctor with own string storage
    Model() : strings(std::make_shared<Strings>()) {}

    /// Ctor with shared string storage
    explicit Model(std::shared_ptr<Strings> stringStore) : strings(std::move(stringStore)) {};

    /// Objects referenced as ModelNode* -
    ///  the first object is the root node.
    std::deque<simfil::ObjectNode> objects;

    /// Arrays referenced as ModelNode*
    std::deque<simfil::ArrayNode> arrays;

    /// Scalars referenced as ModelNode*
    std::deque<simfil::ScalarNode> scalars;

    /// Vertices referenced as ModelNode*
    std::deque<simfil::VertexNode> vertices;

    /// Strings referenced as string_view
    std::shared_ptr<Strings> strings;

    inline ModelNode const* root() const {
        if (objects.empty())
            return nullptr;
        return &objects[0];
    }
};

}
