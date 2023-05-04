// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#include "simfil/value.h"

#include <algorithm>
#include <memory>
#include <vector>

#include <optional>

#include "nodes.h"

namespace simfil
{

/**
 * Basic model pool which only serves as a VFT for trivial node types.
 */
class ModelPoolBase : public std::enable_shared_from_this<ModelPoolBase>
{
public:
    enum TrivialColumnId: uint8_t {
        Null = 0,
        UInt16,
        Int16,
        Bool,
        VirtualOverlay,
        VirtualIntValue,
        VirtualDoubleValue,
        VirtualStringValue,
        VirtualValue,

        FirstNontrivialColumnId,
    };

    /// Get a callback with the actual class of the given node.
    /// This facilitates the Virtual Function Table role of the ModelPool.
    virtual void resolve(
        ModelNode const& n,
        std::function<void(ModelNode&&)> const& cb) const;

    /// Add a small scalar value and get its model node view
    ModelNode::Ptr newSmallValue(bool value);
    ModelNode::Ptr newSmallValue(int16_t value);
    ModelNode::Ptr newSmallValue(uint16_t value);
};

/**
 * Efficient, extensible pool of SIMFIL model nodes based on
 * column stores for different node types.
 */
class ModelPool : public ModelPoolBase
{
    friend class Object;
    friend class Array;

public:
    /**
     * The pool consists of multiple ModelNode columns,
     *  each for a different data type. Each column
     *  is identified by a static column ID.
     */
    enum ColumnId: uint8_t {
        Objects = FirstNontrivialColumnId,
        Arrays,
        Point,
        Geom,
        GeomCollection,
        Int64,
        Double,
        String,
        Vertex,

        FirstCustomColumnId = 128,
    };

    /// Default ctor with own string storage
    ModelPool();
    ~ModelPool();

    /// Ctor with shared string storage
    explicit ModelPool(std::shared_ptr<Fields> stringStore);

    /// Validate that all internal string/node references are valid
    /// Returns a list of found errors.
    virtual std::vector<std::string> checkForErrors() const;

    /// Get a callback with the actual class of the given node.
    /// This facilitates the Virtual Function Table role of the ModelPool.
    void resolve(
        ModelNode const& n,
        std::function<void(ModelNode&&)> const& cb) const override;

    /// Clear all columns and roots
    virtual void clear();

    /// Check for errors, throw if there are any
    void validate() const;

    /// Get number of root nodes
    [[nodiscard]] size_t numRoots() const;

    /// Get specific root node
    [[nodiscard]] ModelNode::Ptr root(size_t const& i) const;

    /// Designate a model node index as a root
    void addRoot(ModelNode::Ptr const& rootNode);

    /// Adopt members from the given vector and obtain a new object
    ///  model index which has these members.
    shared_model_ptr<Object> newObject(size_t initialFieldCapacity=2);

    /// Adopt members from the given vector and obtain a new array
    ///  model index which has these members.
    shared_model_ptr<Array> newArray(size_t initialFieldCapacity=2);

    /// Add a scalar value and get its new model node index.
    ModelNode::Ptr newValue(int64_t const& value);
    ModelNode::Ptr newValue(double const& value);
    ModelNode::Ptr newValue(std::string_view const& value);

    /// Add a vertex and get its new model node index.
    ModelNode::Ptr newVertex(double const& x, double const& y, double const& z);

    /// Node-type-specific resolve-functions
    template<class NodeType>
    shared_model_ptr<NodeType> resolve(ModelNode::Ptr const& n) const;

    /// Access the field name storage
    std::shared_ptr<Fields> fieldNames() const;

protected:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    /// Protected object/array member storage access,
    /// so derived ModelPools can create Object/Array-derived nodes.
    Object::Storage& objectMemberStorage();
    Array::Storage& arrayMemberStorage();
};

/// Node-type-specific resolve-functions
template<>
shared_model_ptr<Object> ModelPool::resolve(ModelNode::Ptr const& n) const;
template<>
shared_model_ptr<Array> ModelPool::resolve(ModelNode::Ptr const& n) const;

}
