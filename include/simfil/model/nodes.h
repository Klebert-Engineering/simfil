#pragma once

#include <memory>
#include <any>

#include "arena.h"
#include "point.h"
#include "fields.h"

#include "sfl/small_vector.hpp"

namespace simfil
{

class Value;
class ModelPool;
class ModelPoolBase;
struct ModelNode;

using ModelPoolConstBasePtr = std::shared_ptr<const ModelPoolBase>;
using ModelPoolConstPtr = std::shared_ptr<const ModelPool>;
using ModelPoolPtr = std::shared_ptr<ModelPool>;

/**
 * Simfil value types
 */
enum class ValueType
{
    Undef,
    Null,
    Bool,
    Int,
    Float,
    String,
    TransientObject,
    Object,
    Array
};

using ScalarValueType = std::variant<
    std::monostate,
    bool,
    int64_t,
    double,
    std::string,
    std::string_view>;

/**
 * Why is shared_model_ptr's value on the stack?
 *
 * All ModelNode types are actually pointers into a ModelPool, via their
 * nested ModelNodeAddress. They keep the ModelPool which they reference
 * alive. The shared_model_ptr wrapper ensures that the user has
 * a better sense of the object they are dealing with. Only mapget::shared_model_ptr
 * is allowed to copy ModelNodes - so e.g. `auto node = *nodePtr` is not possible.
 */
template<typename T>
struct shared_model_ptr
{
    template<typename> friend struct shared_model_ptr;

    shared_model_ptr(::nullptr_t) {}  // NOLINT
    shared_model_ptr(T&& modelNode) : data_(modelNode) {}  // NOLINT
    shared_model_ptr(T const& modelNode) : data_(modelNode) {}  // NOLINT
    shared_model_ptr() = default;

    template<typename OtherT>
    shared_model_ptr(shared_model_ptr<OtherT> const& other) : data_(other.data_) {};  // NOLINT

    template<typename OtherT>
    shared_model_ptr(shared_model_ptr<OtherT>&& other) : data_(other.data_) {};  // NOLINT

    template<typename OtherT>
    shared_model_ptr& operator= (shared_model_ptr<OtherT> const& other) {data_ = other.data_; return *this;};

    template<typename... Args>
    explicit shared_model_ptr(std::in_place_t, Args&&... args) : data_(std::forward<Args>(args)...) {}

    static_assert(std::is_base_of<ModelNode, T>::value, "T must inherit from ModelNode.");

    template<typename... Args>
    static shared_model_ptr<T> make(Args&&... args) {
        return shared_model_ptr(std::in_place, std::forward<Args>(args)...);
    }

    inline T& operator* () {return data_;}
    inline T* operator-> () {return &data_;}
    inline T const& operator* () const {return data_;}
    inline T const* operator-> () const {return &data_;}
    inline operator bool () const {return data_.addr_.value_ != 0;}  // NOLINT

private:
    T data_;
};

/**
 * Unique address of a ModelNode within a Pool.
 * Consists of an 8b column id and 24b index within a column.
 * No column may grow beyond 2^25-1 (33.5M) entries.
 *
 * Some special cases apply for small scalar types:
 * - String: The index is interpreted directly as a string ID.
 * - UInt16|Int16|Bool: The index is interpreted as the integer value.
 * - Null: The index value is ignored.
 */
struct ModelNodeAddress
{
    ModelNodeAddress() = default;

    uint32_t value_ = 0;

    ModelNodeAddress(uint8_t const& col, uint32_t const& idx) {
        value_ = (idx << 8) | col;
    }

    [[nodiscard]] uint8_t column() const {
        return static_cast<uint8_t>(value_ & 0xff);
    }

    void setColumn(uint8_t const& col) {
        value_ = (value_ & 0xffffff00) | col;
    }

    [[nodiscard]] uint32_t index() const {
        return value_ >> 8;
    }

    [[nodiscard]] uint16_t uint16() const {
        return (value_ >> 8) & 0xffff;
    }

    [[nodiscard]] int16_t int16() const {
        return static_cast<int16_t>((value_ >> 8) & 0xffff);
    }
};

/** Semantic view onto a particular node in a ModelPool. */
struct ModelNode
{
    using Ptr = shared_model_ptr<ModelNode>;

    template<typename> friend struct shared_model_ptr;
    friend class ModelPool;
    friend class ModelPoolBase;
    friend class OverlayNode;

    /// Get the node's scalar value if it has one
    [[nodiscard]] virtual ScalarValueType value() const;

    /// Get the node's abstract model type
    [[nodiscard]] virtual ValueType type() const;

    /// Get a child by name
    [[nodiscard]] virtual Ptr get(FieldId const& f) const;

    /// Get a child by index
    [[nodiscard]] virtual Ptr at(int64_t i) const;

    /// Get an Object model's field names
    [[nodiscard]] virtual FieldId keyAt(int64_t i) const;

    /// Get the number of children
    [[nodiscard]] virtual uint32_t size() const;

    /// Get the node's address
    [[nodiscard]] inline ModelNodeAddress addr() const {return addr_;}

    /// Iterator Support
    /// * `fieldNames()`: Returns range object which supports `for(FieldId const& f: node.fieldNames())`
    /// * `fields()`: Returns range object which supports `for(auto const& [fieldId, value] : node.fields())`
    /// * Normal `begin()`/`end()`: Supports `for(ModelNode::Ptr child : node)`

    // Implement fieldNames() by creating an iterator for FieldId
    class FieldIdIterator
    {
        int64_t index;
        ModelNode const* parent;
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = FieldId;
        using difference_type = std::ptrdiff_t;
        using pointer = FieldId*;
        using reference = FieldId&;
        FieldIdIterator(int64_t idx, ModelNode const* p) : index(idx), parent(p) {}
        FieldIdIterator& operator++() { ++index; return *this; }
        bool operator==(const FieldIdIterator& other) const { return index == other.index; }
        bool operator!=(const FieldIdIterator& other) const { return index != other.index; }
        FieldId operator*() const { return parent->keyAt(index); }
    };
    class FieldIdRange
    {
        ModelNode const* node;
    public:
        explicit FieldIdRange(ModelNode const* n) : node(n) {}
        [[nodiscard]] FieldIdIterator begin() const { return node->fieldNamesBegin(); }
        [[nodiscard]] FieldIdIterator end() const { return node->fieldNamesEnd(); }
    };
    [[nodiscard]] FieldIdIterator fieldNamesBegin() const { return {0, this}; }
    [[nodiscard]] FieldIdIterator fieldNamesEnd() const { return {size(), this}; }
    [[nodiscard]] auto fieldNames() const { return FieldIdRange{this}; }

    // Implement fields() by creating an iterator for field-value pairs
    class FieldIterator
    {
        int64_t index;
        ModelNode const* parent;
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = std::pair<FieldId, Ptr>;
        using difference_type = std::ptrdiff_t;
        using pointer = value_type*;
        using reference = value_type&;
        FieldIterator(int64_t idx, ModelNode const* p) : index(idx), parent(p) {}
        FieldIterator& operator++() { ++index; return *this; }
        bool operator==(const FieldIterator& other) const { return index == other.index; }
        bool operator!=(const FieldIterator& other) const { return index != other.index; }
        std::pair<FieldId, Ptr> operator*() const { return std::make_pair(parent->keyAt(index), parent->get(parent->keyAt(index))); }
    };
    class FieldRange
    {
        ModelNode const* node;
    public:
        explicit FieldRange(ModelNode const* n) : node(n) {}
        [[nodiscard]] FieldIterator begin() const { return node->fieldsBegin(); }
        [[nodiscard]] FieldIterator end() const { return node->fieldsEnd(); }
    };
    [[nodiscard]] FieldIterator fieldsBegin() const { return {0, this}; }
    [[nodiscard]] FieldIterator fieldsEnd() const { return {size(), this}; }
    [[nodiscard]] auto fields() const { return FieldRange(this); }

    // Implement the normal begin()/end() for ModelNode::Ptr
    class ChildIterator
    {
        int64_t index;
        ModelNode const* parent;
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = Ptr;
        using difference_type = std::ptrdiff_t;
        using pointer = value_type*;
        using reference = value_type&;
        ChildIterator(int64_t idx, ModelNode const* p) : index(idx), parent(p) {}
        ChildIterator& operator++() { ++index; return *this; }
        bool operator==(const ChildIterator& other) const { return index == other.index; }
        bool operator!=(const ChildIterator& other) const { return index != other.index; }
        Ptr operator*() const { return parent->at(index); }
    };
    [[nodiscard]] ChildIterator begin() const { return {0, this}; }
    [[nodiscard]] ChildIterator end() const { return {size(), this}; }

protected:
    ModelNode() = default;
    ModelNode(ModelNode const&) = default;
    ModelNode(ModelNode&&) = default;
    ModelNode& operator= (ModelNode const&) = default;
    ModelNode(ModelPoolConstBasePtr, ModelNodeAddress);

    /// Extra data for the node
    ScalarValueType data_;

    /// Reference to the model pool which owns this node
    ModelPoolConstBasePtr pool_;

    /// Address of the model pool node which this object references
    ModelNodeAddress addr_;
};

/**
 * ModelNode with default interface implementations.
 * All other ModelNode types should derive from this
 * interface. This is to ensure, that there is never
 * an infinite call-chain due to the ModelNode's pool::resolve
 * calls, when the derived class does not overload one
 * of the methods.
 */
struct ModelNodeBase : public ModelNode
{
    friend class ModelPoolBase;

    [[nodiscard]] ScalarValueType value() const override;
    [[nodiscard]] ValueType type() const override;
    [[nodiscard]] ModelNode::Ptr get(const FieldId&) const override;
    [[nodiscard]] ModelNode::Ptr at(int64_t) const override;
    [[nodiscard]] FieldId keyAt(int64_t) const override;
    [[nodiscard]] uint32_t size() const override;

protected:
    ModelNodeBase(ModelPoolConstBasePtr, ModelNodeAddress={});  // NOLINT
    ModelNodeBase(ModelNode const&);  // NOLINT
};

/**
 * Through the data_ member, a ModelNode can store an arbitrary value.
 */
struct ValueNode : public ModelNodeBase
{
    explicit ValueNode(ScalarValueType const& value, ModelPoolConstBasePtr const& pool = std::make_shared<ModelPoolBase>());
    explicit ValueNode(ModelNode const&);
    [[nodiscard]] ValueType type() const override;
};

/**
 * ModelNode base class which provides a pool() method that returns
 * a reference to a ModelPoolBase-derived pool type.
 * @tparam PoolType ModelPoolBase-derived type.
 */
template<class PoolType>
struct MandatoryDerivedModelPoolNodeBase : public ModelNodeBase
{
protected:
    inline PoolType& pool() const {return *reinterpret_cast<PoolType*>(const_cast<ModelPoolBase*>(pool_.get()));}  // NOLINT

    MandatoryDerivedModelPoolNodeBase(ModelPoolConstBasePtr p, ModelNodeAddress a={}) : ModelNodeBase(p, a) {}  // NOLINT
    MandatoryDerivedModelPoolNodeBase(ModelNode const& n) : ModelNodeBase(n) {}  // NOLINT
};

using MandatoryModelPoolNodeBase = MandatoryDerivedModelPoolNodeBase<ModelPool>;

namespace detail
{
    // Columns need to use some page size in their
    // underlying segmented vectors for efficient allocation.
    // We define this here, because some nodes need to keep
    // a reference to their array storages. The storage type
    // has the page size as a template parameter.
    constexpr auto ColumnPageSize = 8192;
}

/**
 * Model Node for a UInt16/Int16/Bool.
 * The value is stored in the TreeNodeAddress.
 */
template<typename T>
struct SmallScalarNode : public ModelNodeBase
{
    friend class ModelPoolBase;
    [[nodiscard]] ScalarValueType value() const override;
    [[nodiscard]] ValueType type() const override;
protected:
    SmallScalarNode(ModelPoolConstBasePtr, ModelNodeAddress);
};

template<> [[nodiscard]] ScalarValueType SmallScalarNode<int16_t>::value() const;
template<> [[nodiscard]] ValueType SmallScalarNode<int16_t>::type() const;
template<> SmallScalarNode<int16_t>::SmallScalarNode(ModelPoolConstBasePtr, ModelNodeAddress);
template<> [[nodiscard]] ScalarValueType SmallScalarNode<uint16_t>::value() const;
template<> [[nodiscard]] ValueType SmallScalarNode<uint16_t>::type() const;
template<> SmallScalarNode<uint16_t>::SmallScalarNode(ModelPoolConstBasePtr, ModelNodeAddress);
template<> [[nodiscard]] ScalarValueType SmallScalarNode<bool>::value() const;
template<> [[nodiscard]] ValueType SmallScalarNode<bool>::type() const;
template<> SmallScalarNode<bool>::SmallScalarNode(ModelPoolConstBasePtr, ModelNodeAddress);


/** Model Node for a string value. */

struct StringNode : public MandatoryModelPoolNodeBase
{
    friend class ModelPool;
    [[nodiscard]] ScalarValueType value() const override;
    [[nodiscard]] ValueType type() const override;
protected:
    StringNode(std::string_view s, ModelPoolConstBasePtr storage, ModelNodeAddress);

    /**
     * String Data stored in ModelPool column.
     */
    struct Data {
        size_t offset_ = 0;
        size_t size_ = 0;
    };

    std::string_view str_;
};

/** Model Node for a scalar reference value. */

template<typename T>
struct ScalarNode : public MandatoryModelPoolNodeBase
{
    friend class ModelPool;
    template<typename> friend struct shared_model_ptr;

    [[nodiscard]] ValueType type() const override;

protected:
    ScalarNode(T const& value, ModelPoolConstBasePtr storage, ModelNodeAddress);
    ScalarNode(ModelNode const&);
};

template<> [[nodiscard]] ValueType ScalarNode<int64_t>::type() const;
template<> ScalarNode<int64_t>::ScalarNode(int64_t const& value, ModelPoolConstBasePtr storage, ModelNodeAddress);
template<> ScalarNode<int64_t>::ScalarNode(ModelNode const&);
template<> [[nodiscard]] ValueType ScalarNode<double>::type() const;
template<> ScalarNode<double>::ScalarNode(double const& value, ModelPoolConstBasePtr storage, ModelNodeAddress);
template<> ScalarNode<double>::ScalarNode(ModelNode const&);

/** Model Node for an array. */

struct Array : public MandatoryModelPoolNodeBase
{
    friend class ModelPool;

    [[nodiscard]] ValueType type() const override;
    [[nodiscard]] ModelNode::Ptr at(int64_t) const override;
    [[nodiscard]] uint32_t size() const override;

    Array& append(bool value);
    Array& append(uint16_t value);
    Array& append(int16_t value);
    Array& append(int64_t const& value);
    Array& append(double const& value);
    Array& append(std::string_view const& value);
    Array& append(ModelNode::Ptr const& value={});

protected:
    using Storage = ArrayArena<ModelNodeAddress, detail::ColumnPageSize*2>;

    Array(ArrayIndex i, ModelPoolConstBasePtr pool, ModelNodeAddress);

    Storage* storage_;
    ArrayIndex members_;
};

/** Model Node for an object. */

struct Object : public MandatoryModelPoolNodeBase
{
    friend class ModelPool;

    [[nodiscard]] ValueType type() const override;
    [[nodiscard]] ModelNode::Ptr at(int64_t) const override;
    [[nodiscard]] uint32_t size() const override;
    [[nodiscard]] ModelNode::Ptr get(const FieldId &) const override;
    [[nodiscard]] FieldId keyAt(int64_t) const override;

    Object& addField(std::string_view const& name, bool value);
    Object& addField(std::string_view const& name, uint16_t value);
    Object& addField(std::string_view const& name, int16_t value);
    Object& addField(std::string_view const& name, int64_t const& value);
    Object& addField(std::string_view const& name, double const& value);
    Object& addField(std::string_view const& name, std::string_view const& value);
    Object& addField(std::string_view const& name, ModelNode::Ptr const& value={});

    [[nodiscard]] ModelNode::Ptr get(std::string_view const& fieldName) const;

protected:
    /**
     * Object field - a name and a tree node address.
     * These are stored in the ModelPools Field array arena.
     */
    struct Field
    {
        Field() = default;
        Field(FieldId name, ModelNodeAddress a) : name_(name), node_(a) {}
        FieldId name_ = Fields::Empty;
        ModelNodeAddress node_;
    };

    using Storage = ArrayArena<Field, detail::ColumnPageSize*2>;

    Object(ArrayIndex i, ModelPoolConstBasePtr pool, ModelNodeAddress);

    Storage* storage_;
    ArrayIndex members_;
};

/** Object with extra procedural fields */

template<uint16_t MaxProceduralFields>
class ProceduralObject : public Object
{
    [[nodiscard]] ModelNode::Ptr at(int64_t i) const override {
        if (i < fields_.size())
            return fields_[i].second();
        return Object::at(i - fields_.size());
    }

    [[nodiscard]] uint32_t size() const override {
        return fields_.size() + Object::size();
    }

    [[nodiscard]] ModelNode::Ptr get(const FieldId & field) const override {
        for (auto const& [k, v] : fields_)
            if (k == field)
                return v();
        return Object::get(field);
    }

    [[nodiscard]] FieldId keyAt(int64_t i) const override {
        if (i < fields_.size())
            return fields_[i].first;
        return Object::keyAt(i - fields_.size());
    }

protected:
    ProceduralObject(ArrayIndex i, ModelPoolConstBasePtr pool, ModelNodeAddress a)
        : Object(i, pool, a) {}

    sfl::small_vector<
        std::pair<FieldId, std::function<ModelNode::Ptr()>>,
        MaxProceduralFields> fields_;
};

/** Vertex Node */

struct VertexNode : public MandatoryModelPoolNodeBase
{
    friend class ModelPool;

    [[nodiscard]] ValueType type() const override;
    [[nodiscard]] ModelNode::Ptr at(int64_t) const override;
    [[nodiscard]] uint32_t size() const override;
    [[nodiscard]] ModelNode::Ptr get(const FieldId &) const override;
    [[nodiscard]] FieldId keyAt(int64_t) const override;

protected:
    VertexNode(geo::Point<double> const& p, ModelPoolConstBasePtr pool, ModelNodeAddress a);

    geo::Point<double> point_;
};

//     /**
// * Geometry object, which stores a point collection, a line-string,
// * or a triangle mesh.
//     */
//    struct GeometryData {
//        enum Type: uint8_t {
//            Points,
//            PolyLine,
//            Triangles
//        };
//
//        ArrayIndex vertexArray_ = -1;
//
//        // Offset is set when vertexArray is allocated,
//        // which happens when the first point is added.
//        geo::Point<double> offset_;
//    };

}