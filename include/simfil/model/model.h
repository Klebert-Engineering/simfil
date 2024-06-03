// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#include "simfil/value.h"

#include <algorithm>
#include <memory>
#include <vector>
#include <istream>
#include <ostream>
#include <optional>

#include "nodes.h"

namespace simfil
{

/**
 * Basic node model which only resolves trivial node types.
 */
class Model : public std::enable_shared_from_this<Model>
{
public:
    enum TrivialColumnId : uint8_t {
        Null = 0,
        UInt16,
        Int16,
        Bool,
        Scalar,

        FirstNontrivialColumnId,
    };

    struct ResolveFn
    {
        virtual ~ResolveFn() = default;
        virtual void operator() (ModelNode const& resolved) const = 0;
    };

    template<typename T>
    struct Lambda : public ResolveFn
    {
        Lambda(T const& fn) : fn_(fn) {}  // NOLINT
        void operator() (ModelNode const& resolved) const override {fn_(resolved);};
        T fn_;
    };

    /// Virtual destructor to allow polymorphism
    virtual ~Model() = default;

    /// Get a callback with the actual class of the given node.
    /// This facilitates the Virtual Function Table role of the Model.
    virtual void resolve(ModelNode const& n, ResolveFn const& cb) const;

    /// Add a small scalar value and get its model node view
    ModelNode::Ptr newSmallValue(bool value);
    ModelNode::Ptr newSmallValue(int16_t value);
    ModelNode::Ptr newSmallValue(uint16_t value);
};

/**
 * Efficient, extensible pool of SIMFIL model nodes based on
 * column stores for different node types.
 */
class ModelPool : public Model
{
    friend struct Object;
    friend struct Array;
    friend struct Geometry;
    friend struct VertexBufferNode;
    friend struct VertexNode;
    friend struct GeometryCollection;

public:
    /**
     * The pool consists of multiple ModelNode columns,
     *  each for a different data type. Each column
     *  is identified by a static column ID.
     */
    enum ColumnId : uint8_t {
        Objects = FirstNontrivialColumnId,
        Arrays,
        Int64,
        Double,
        String,

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
    void resolve(ModelNode const& n, ResolveFn const& cb) const override;

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
    shared_model_ptr<Object> newObject(size_t initialFieldCapacity = 2);

    /// Adopt members from the given vector and obtain a new array
    ///  model index which has these members.
    shared_model_ptr<Array> newArray(size_t initialFieldCapacity = 2);

    /// Add a scalar value and get its new model node index.
    ModelNode::Ptr newValue(int64_t const& value);
    ModelNode::Ptr newValue(double const& value);
    ModelNode::Ptr newValue(std::string_view const& value);

    /// Node-type-specific resolve-functions
    shared_model_ptr<Object> resolveObject(ModelNode::Ptr const& n) const;
    shared_model_ptr<Array> resolveArray(ModelNode::Ptr const& n) const;

    /// Access the field name storage
    std::shared_ptr<Fields> fieldNames() const;

    /// Change the fields dict of this model to a different one.
    /// Note: This will potentially create new field entries in the newDict,
    /// for field names which were not there before.
    virtual void setFieldNames(std::shared_ptr<simfil::Fields> const& newDict);

    /// Serialization
    virtual void write(std::ostream& outputStream);
    virtual void read(std::istream& inputStream);

protected:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    /// Protected object/array member storage access,
    /// so derived ModelPools can create Object/Array-derived nodes.
    Object::Storage& objectMemberStorage();
    Array::Storage& arrayMemberStorage();
};

}
