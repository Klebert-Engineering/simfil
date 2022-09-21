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
#include <fstream>
#include <sstream>

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
    using Member = std::pair<std::string_view, ModelNode*>;

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
        if (!storage_)
            return {};
        for (int i = 0; i < size_; ++i)
            if(storage_->at(firstMemberIndex_ + i).first == key)
                return storage_->at(firstMemberIndex_ + i).second;
        return nullptr;
    }

    auto get(int64_t) const -> const ModelNode*
    {
        return nullptr;
    }

    auto children() const -> std::vector<const ModelNode*>
    {
        if (!storage_)
            return {};
        std::vector<const ModelNode*> nodes;
        nodes.reserve(size_);
        for (int i = 0; i < size_; ++i)
            nodes.push_back(storage_->at(firstMemberIndex_ + i).second);
        return nodes;
    }

    auto keys() const -> std::vector<std::string_view>
    {
        if (!storage_)
            return {};
        std::vector<std::string_view> names;
        names.reserve(size_);
        for (int i = 0; i < size_; ++i)
            names.push_back(storage_->at(firstMemberIndex_ + i).first);
        return names;
    }

    auto size() const -> int64_t
    {
        return size_;
    }

    std::deque<ObjectNode::Member>* storage_ = nullptr;
    size_t firstMemberIndex_ = 0;
    size_t size_ = 0;
};

class ArrayNode : public ModelNode
{
public:
    /// Array member nodes are referenced by index
    using Member = ModelNode*;

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

    auto get(int64_t i) const -> const ModelNode*
    {
        if (!storage_)
            return {};
        if (0 <= i && i <= size_)
            return storage_->at(firstMemberIndex_ + i);
        return nullptr;
    }

    auto children() const -> std::vector<const ModelNode*>
    {
        if (!storage_)
            return {};
        std::vector<const ModelNode*> result;
        result.reserve(size_);
        for (auto i = 0; i < size_; ++i)
            result.push_back(storage_->at(firstMemberIndex_ + i));
        return result;
    }

    auto keys() const -> std::vector<std::string_view>
    {
        return {};
    }

    auto size() const -> int64_t
    {
        return size_;
    }

    std::deque<ArrayNode::Member>* storage_ = nullptr;
    size_t firstMemberIndex_ = 0;
    size_t size_ = 0;
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

class FeatureIdNode : public ModelNode
{
public:
    FeatureIdNode() = default;
    FeatureIdNode(
        std::string_view const& prefix,
        std::vector<std::pair<char const*, int64_t>> idPathElements
    ) : prefix_(prefix), idPathElements_(std::move(idPathElements))
    {}

    auto type() const -> ModelNode::Type override {return ModelNode::Type::Scalar; }

    auto value() const -> Value override {
        std::stringstream result;
        result << prefix_;
        for (auto& [type, id] : idPathElements_) {
            result <<  "." << type << "." << std::to_string(id);
        }
        return Value::make(result.str());
    }

    auto get(const std::string_view & key) const -> const ModelNode* override
    {
        return nullptr;
    }

    auto get(int64_t idx) const -> const ModelNode* override
    {
        return nullptr;
    }

    auto children() const -> std::vector<const ModelNode*> override { return {}; }

    auto keys() const -> std::vector<std::string_view> override { return {}; }

    auto size() const -> int64_t override { return 2; }

    std::string_view prefix_;
    std::vector<std::pair<char const*, int64_t>> idPathElements_;
};

/**
 * Efficient storage of SIMFIL model nodes. This way, a whole
 * ModelNode tree can be cleaned up in constant time. The nodes
 * reference each other via deque indices. Because deques are used,
 * the pointers stay valid as the containers grow.
 */
struct Model
{
    /** Fast and efficient string storage -
     * referenced by object keys and string values.
     */
    struct Strings
    {
    private:
        std::unordered_set<std::string> strings_;
        std::atomic_int64_t byteSize_;
        std::shared_mutex stringStoreMutex_;
        std::atomic_int64_t cacheHits_;
        std::atomic_int64_t cacheMisses_;

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
                    ++cacheHits_;
                    return *it;
                }
            }
            {
                std::unique_lock stringStoreWriteAccess_(stringStoreMutex_);
                auto [it, insertionTookPlace] = strings_.emplace(str);
                if (insertionTookPlace) {
                    ++cacheMisses_;
                    byteSize_ += str.size();
                }
                return *it;
            }
        }

        /// Get stats
        size_t size() {
            std::shared_lock stringStoreReadAccess_(stringStoreMutex_);
            return strings_.size();
        }
        size_t bytes() {
            return byteSize_;
        }
        size_t hits() {
            return cacheHits_;
        }
        size_t misses() {
            return cacheMisses_;
        }
    };

    /// No copies allowed...
    Model(Model const&) = delete;

    /// Default ctor with own string storage
    Model() : strings(std::make_shared<Strings>()) {}

    /// Ctor with shared string storage
    explicit Model(std::shared_ptr<Strings> stringStore) : strings(std::move(stringStore)) {};

    /// Root nodes
    std::vector<simfil::ModelNode*> roots;

    /// Objects
    std::deque<simfil::ObjectNode> objects;

    /// Arrays
    std::deque<simfil::ArrayNode> arrays;

    /// Scalars
    std::deque<simfil::ScalarNode> scalars;

    /// Vertices
    std::deque<simfil::VertexNode> vertices;

    /// Strings
    std::shared_ptr<Strings> strings;

    /// Feature Ids
    std::deque<FeatureIdNode> featureIds;

    /// Array member references - all member references
    /// for a single array appear consecutively.
    std::deque<ArrayNode::Member> arrayMembers;

    /// Object member references - all member references
    /// for a single array appear consecutively.
    std::deque<ObjectNode::Member> objectMembers;
};

using ModelPtr = std::shared_ptr<Model>;

}
