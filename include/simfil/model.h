// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#include "value.h"

#include <algorithm>
#include <memory>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <atomic>

#include <optional>

namespace simfil
{

using Vertex3d = std::tuple<double, double, float>;

/**
 * Fast and efficient case-insensitive string storage -
 * referenced by object keys and string values.
 */
struct Strings
{
    using Id = uint16_t;
    enum StaticStringIds: Id {
        Empty = 0,
        Lon = 1,
        Lat = 2,
        OverlaySum = 3,
        OverlayValue = 4,
        OverlayIndex = 5,
        Geometry = 6,
        Type = 7,
        Coordinates = 8,
        Elevation = 9,

        NextStaticId = 10,
        FirstDynamicId = 128
    };

public:
    /// Default constructor initializes strings for static Ids
    Strings();

    /// Use this function to lookup a stored string, or insert it
    /// if it doesn't exist yet. Unfortunately, we can't use string_view
    /// as lookup type until C++ 20 is used:
    ///   http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2018/p0919r2.html
    Id emplace(std::string const& str);

    /// Returns the ID of the given string, or `Invalid` if
    /// no such string was ever inserted.
    Id get(std::string const& str);

    /// Get the actual string for the given ID, or
    ///  nullopt if the given ID is invalid.
    /// Virtual to allow defining an inherited StringPool which understands
    /// additional StaticStringIds.
    virtual std::optional<std::string_view> resolve(Id const& id);

    /// Get stats
    size_t size();
    size_t bytes();
    size_t hits();
    size_t misses();

    /// Add a static key-string mapping - Warning: Not thread-safe.
    void addStaticKey(StringId k, std::string const& v);

private:
    std::shared_mutex stringStoreMutex_;
    std::unordered_map<std::string, Id> idForString_;
    std::unordered_map<Id, std::string> stringForId_;
    Id nextId_ = FirstDynamicId;
    std::atomic_int64_t byteSize_;
    std::atomic_int64_t cacheHits_;
    std::atomic_int64_t cacheMisses_;
};

/**
 * Efficient, extensible pool of SIMFIL model nodes based on
 * column stores for different node types.
 */
struct ModelPool
{
    /**
     * The pool consists of multiple ModelNode columns,
     *  each for a different data type. Each column
     *  is identified by a static column ID.
     */
    enum ColumnId: uint8_t {
        Objects = 0,
        Arrays = 1,
        Vertex = 3,
        Vertex3d = 4,
        UInt16 = 7,
        Int16 = 8,
        Int64 = 10,
        Double = 11,
        String = 12,
        Null = 13,
        Bool = 14,

        FirstCustomColumnId = 128,
    };

    /**
     * Unique address of a ModelNode within the Pool.
     * Consists of an 8b column id and 24b index within a column.
     * No column may grow beyond 2^25-1 (33.5M) entries.
     *
     * Some special cases apply for small scalar types:
     * - String: The index is interpreted directly as a string ID.
     * - UInt16|Int16|Bool: The index is interpreted as the integer value.
     * - Null: The index value is ignored.
     */
    struct ModelNodeIndex
    {
        uint32_t value_;

        ModelNodeIndex(ColumnId const& col, size_t const& idx) {
            value_ = (idx << 8) | col;
        }

        ColumnId column() const {
            return static_cast<ColumnId>(value_ & 0xff);
        }

        uint32_t index() const {
            return value_ >> 8;
        }

        uint16_t uint16() const {
            return (value_ >> 8) & 0xffff;
        }

        int16_t int16() const {
            return static_cast<int16_t>((value_ >> 8) & 0xffff);
        }
    };

    /**
     * Named object/array member.
     */
    struct Member {
        StringId name_ = Strings::Empty;
        ModelNodeIndex nodeIndex_;

        Member(StringId name, ModelNodeIndex nodeIndex) : name_(name), nodeIndex_(nodeIndex) {}
        Member(ModelNodeIndex nodeIndex) : nodeIndex_(nodeIndex) {}
    };
    using MemberRange = std::pair<uint32_t, uint32_t>;

    /// Default ctor with own string storage
    ModelPool();
    ~ModelPool();

    /// Ctor with shared string storage
    explicit ModelPool(std::shared_ptr<Strings> stringStore);

    /// This model pool's string store
    std::shared_ptr<Strings> strings;

    /// Validate that all internal string/node references are valid
    /// Returns a list of found errors.
    virtual std::vector<std::string> checkForErrors() const;

    /// Get a model-node for a specific column address
    virtual ModelNodePtr resolve(ModelNodeIndex const& i) const;

    /// Clear all columns and roots
    virtual void clear();

    /// Check for errors, throw if there are any
    void validate() const;

    /// Visit all members within the given range
    void visitMembers(MemberRange const& range, std::function<bool(Member const&)> const&) const;

    /// Get number of root nodes
    size_t numRoots() const;

    /// Get specific root node
    ModelNodePtr root(size_t const& i) const;

    /// Designate a model node index as a root
    void addRoot(ModelNodeIndex const& rootIndex);

    /// Adopt members from the given vector and obtain a new object
    ///  model index which has these members.
    ModelNodeIndex addObject(std::vector<Member> const& members);

    /// Adopt members from the given vector and obtain a new array
    ///  model index which has these members.
    ModelNodeIndex addArray(std::vector<Member> const& members);

    /// Add a scalar value and get its new model node index.
    ModelNodeIndex addNull();
    ModelNodeIndex addValue(bool const& value);
    ModelNodeIndex addValue(int64_t const& value);
    ModelNodeIndex addValue(double const& value);
    ModelNodeIndex addValue(std::string const& value);

    /// Add a vertex and get its new model node index.
    ModelNodeIndex addVertex(double const& lon, double const& lat);

    /// Add a 3d vertex and get its new model node index.
    ModelNodeIndex addVertex3d(double const& lon, double const& lat, float const &elevation);

    /// Add some members and get their occupied range
    MemberRange addMembers(std::vector<Member> const&);

protected:
    static constexpr auto ChunkSize = 4096;
    static constexpr auto BigChunkSize = 8192;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

using ModelPoolPtr = std::shared_ptr<ModelPool>;

/** Model Node for a scalar value. */
struct ScalarModelNode : public ModelNodeBase
{
    ScalarModelNode(int64_t const& i);
    ScalarModelNode(double const& d);
    ScalarModelNode(bool const& b);
    ScalarModelNode(std::string const& s);

    Value value() const override;

private:
    Value value_;
};

/** Model Node for a symbolic string value. */
struct StringModelNode : public ModelNodeBase
{
    StringModelNode(StringId strId, std::shared_ptr<Strings> stringPool);

    Value value() const override;

private:
    StringId strId_;
    std::shared_ptr<Strings> stringPool_;
};

/** Model Node for an array. */
struct ArrayModelNode : public ModelNodeBase
{
    ArrayModelNode(ModelPool::MemberRange members, ModelPool const& modelPool);

    Type type() const override;
    ModelNodePtr at(int64_t) const override;
    std::vector<ModelNodePtr> children() const override;
    uint32_t size() const override;

protected:
    ModelPool::MemberRange members_;
    ModelPool const& modelPool_;
};

/** Model Node for an object. */
struct ObjectModelNode : public ArrayModelNode
{
    ObjectModelNode(ModelPool::MemberRange members, ModelPool const& modelPool);

    Type type() const override;
    ModelNodePtr get(const StringId &) const override;
    std::vector<std::string> keys() const override;
};

/** Model Node for an object with extra procedural fields. */
struct ProceduralObjectModelNode : public ObjectModelNode
{
    using Field = std::pair<StringId, std::function<ModelNodePtr()>>;

    /// Construct a node with procedural fields
    ProceduralObjectModelNode(
        ModelPool::MemberRange members,
        ModelPool const &modelPool);

    ModelNodePtr get(const StringId &) const override;
    ModelNodePtr at(int64_t) const override;
    std::vector<ModelNodePtr> children() const override;
    std::vector<std::string> keys() const override;
    uint32_t size() const override;

protected:
    std::vector<Field> fields_;
};

/** Model Node for a vertex. */
struct VertexModelNode : public ProceduralObjectModelNode
{
    VertexModelNode(std::pair<double, double> const& coords, ModelPool const& modelPool);

    Type type() const override;

private:
    std::pair<double, double> const& coords_;
};

/** Model Node for a 3d vertex. */
struct Vertex3dModelNode : public ProceduralObjectModelNode
{
    Vertex3dModelNode(Vertex3d const& coords, ModelPool const& modelPool);

    Type type() const override;

private:
    Vertex3d const& coords_;
};

}
