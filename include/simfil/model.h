// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#include "value.h"
#include "segmented-vector.h"

#include <algorithm>
#include <memory>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <atomic>

namespace simfil
{

constexpr auto SegmentSize = 2048;
constexpr auto MemberSegmentSize = SegmentSize*4;

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

/** Scalar Node */
class ScalarNode : public ModelNode
{
public:
    ScalarNode();
    ScalarNode(Value&& v);
    auto value() const -> Value override;
    auto type() const -> Type override;
    auto get(const std::string_view &) const ->  const ModelNode* override;
    auto get(int64_t) const ->  const ModelNode* override;
    auto children() const -> std::vector<const ModelNode*> override;
    auto keys() const -> std::vector<std::string_view> override;
    auto size() const -> int64_t override;

    Value scalar;
};

/** Object Node */
class ObjectNode : public ModelNode
{
public:
    using Member = std::pair<std::string_view, ModelNode*>;

    auto value() const -> Value override;
    auto type() const -> Type override;
    auto get(const std::string_view & key) const -> const ModelNode* override;
    auto get(int64_t) const -> const ModelNode* override;
    auto children() const -> std::vector<const ModelNode*> override;
    auto keys() const -> std::vector<std::string_view> override;
    auto size() const -> int64_t override;

    sfl::segmented_vector<ObjectNode::Member, MemberSegmentSize>* storage_ = nullptr;
    size_t firstMemberIndex_ = 0;
    size_t size_ = 0;
};

/** Array Node */
class ArrayNode : public ModelNode
{
public:
    /// Array member nodes are referenced by index
    using Member = ModelNode*;

    auto value() const -> Value override;
    auto type() const -> Type override;
    auto get(const std::string_view &) const -> const ModelNode* override;
    auto get(int64_t i) const -> const ModelNode* override;
    auto children() const -> std::vector<const ModelNode*> override;
    auto keys() const -> std::vector<std::string_view> override;
    auto size() const -> int64_t override;

    sfl::segmented_vector<ArrayNode::Member, MemberSegmentSize>* storage_ = nullptr;
    size_t firstMemberIndex_ = 0;
    size_t size_ = 0;
};

/** Vertex Node */
class VertexNode : public ModelNode
{
public:
    VertexNode() = default;
    VertexNode(double lon, double lat);
    auto type() const -> ModelNode::Type override;
    auto value() const -> Value override;
    auto get(const std::string_view & key) const -> const ModelNode* override;
    auto get(int64_t idx) const -> const ModelNode* override;
    auto children() const -> std::vector<const ModelNode*> override;
    auto keys() const -> std::vector<std::string_view> override;
    auto size() const -> int64_t override;

    ScalarNode lon;
    ScalarNode lat;
};

/** FeatureId Node */
class FeatureIdNode : public ModelNode
{
public:
    FeatureIdNode() = default;
    FeatureIdNode(
        std::string_view const& prefix,
        std::vector<std::pair<char const*, int64_t>> idPathElements
    );

    auto type() const -> ModelNode::Type override;
    auto value() const -> Value override;
    auto get(const std::string_view & key) const -> const ModelNode* override;
    auto get(int64_t idx) const -> const ModelNode* override;
    auto children() const -> std::vector<const ModelNode*> override;
    auto keys() const -> std::vector<std::string_view> override;
    auto size() const -> int64_t override;

    std::string_view prefix_;
    std::vector<std::pair<char const*, int64_t>> idPathElements_;
};

/**
 * Efficient storage of SIMFIL model nodes. This way, a whole
 * ModelNode tree can be cleaned up in constant time. The nodes
 * reference each other via deque indices. Because deques are used,
 * the pointers stay valid as the containers grow.
 */
struct ModelPool {

    /** Fast and efficient string storage -
     * referenced by object keys and string values.
     */
    struct Strings
    {
        friend struct ModelPool;

    private:
        std::shared_mutex stringStoreMutex_;
        std::unordered_set<std::string> strings_;
        std::atomic_int64_t byteSize_;
        std::atomic_int64_t cacheHits_;
        std::atomic_int64_t cacheMisses_;

    public:
        /// Use this function to lookup a stored string, or insert it
        /// if it doesn't exist yet. Unfortunately, we can't use string_view
        /// as lookup type until C++ 20 is used:
        ///   http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2018/p0919r2.html
        std::string_view getOrInsert(std::string const& str);

        /// Get stats
        size_t size();
        size_t bytes();
        size_t hits();
        size_t misses();
    };

    /// No copies allowed...
    ModelPool(ModelPool const&) = delete;

    /// Default ctor with own string storage
    ModelPool();

    /// Ctor with shared string storage
    explicit ModelPool(std::shared_ptr<Strings> stringStore);

    /// Root nodes
    std::vector<simfil::ModelNode*> roots;

    /// Objects
    sfl::segmented_vector<simfil::ObjectNode, SegmentSize> objects;

    /// Arrays
    sfl::segmented_vector<simfil::ArrayNode, SegmentSize> arrays;

    /// Scalars
    sfl::segmented_vector<simfil::ScalarNode, SegmentSize> scalars;

    /// Vertices
    sfl::segmented_vector<simfil::VertexNode, SegmentSize> vertices;

    /// Feature Ids
    sfl::segmented_vector<FeatureIdNode, SegmentSize> featureIds;

    /// Strings
    std::shared_ptr<Strings> strings;

    /// Array member references - all member references
    /// for a single array appear consecutively.
    sfl::segmented_vector<ArrayNode::Member, MemberSegmentSize> arrayMembers;

    /// Object member references - all member references
    /// for a single array appear consecutively.
    sfl::segmented_vector<ObjectNode::Member, MemberSegmentSize> objectMembers;

    /// Validate that all internal string/node references are valid
    /// Returns a list of found errors.
    std::vector<std::string> checkForErrors() const;

    /// Check for errors, throw if there are any
    void validate() const;

    /// Clear all member containers (except Strings).
    void clear();
};

using ModelPoolPtr = std::shared_ptr<ModelPool>;

}
