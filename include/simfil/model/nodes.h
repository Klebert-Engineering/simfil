#pragma once

#include <memory>
#include <variant>

#include "arena.h"
#include "point.h"
#include "fields.h"

#include "sfl/small_vector.hpp"

namespace bitsery {
    // Pre-declare bitsery protected member accessor.
    class Access;
}

namespace simfil
{

class Value;
class ModelPool;
class Model;
struct ModelNode;

using ModelConstPtr = std::shared_ptr<const Model>;
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
 * a better sense of the object they are dealing with. Only shared_model_ptr
 * is allowed to copy ModelNodes - so e.g. `auto node = *nodePtr` is not possible.
 */
template<typename T>
struct shared_model_ptr
{
    template<typename> friend struct shared_model_ptr;

    shared_model_ptr(::nullptr_t) {}  // NOLINT
    shared_model_ptr(T&& modelNode) : data_(std::move(modelNode)) {}  // NOLINT
    explicit shared_model_ptr(T const& modelNode) : data_(modelNode) {}  // NOLINT

    shared_model_ptr() = default;
    shared_model_ptr(const shared_model_ptr&) = default;
    shared_model_ptr(shared_model_ptr&&) = default;

    shared_model_ptr& operator=(shared_model_ptr const&) = default;
    shared_model_ptr& operator=(shared_model_ptr&&) = default;

    template<typename OtherT>
    shared_model_ptr(shared_model_ptr<OtherT> const& other) : data_(other.data_) {}  // NOLINT

    template<typename OtherT>
    shared_model_ptr(shared_model_ptr<OtherT>&& other) : data_(std::move(other.data_)) {}  // NOLINT

    template<typename OtherT>
    shared_model_ptr& operator= (shared_model_ptr<OtherT> const& other) {data_ = other.data_; return *this;}

    template<typename OtherT>
    shared_model_ptr& operator= (shared_model_ptr<OtherT>&& other) {data_ = std::move(other.data_); return *this;}

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

    ModelNodeAddress(uint8_t const& col, uint32_t const& idx = 0) {  // NOLINT
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

    template<typename S>
    void serialize(S& s) {
        s.value4b(value_);
    }
};

/** Semantic view onto a particular node in a ModelPool. */
struct ModelNode
{
    using Ptr = shared_model_ptr<ModelNode>;

    template<typename> friend struct shared_model_ptr;
    friend class ModelPool;
    friend class Model;
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

    /// Virtual destructor to allow polymorphism
    virtual ~ModelNode() = default;

    /// Fast iterator support
    struct IterCallback
    {
        virtual ~IterCallback() = default;
        virtual bool operator() (ModelNode const& resolved) const = 0;
    };
    template<typename T>
    struct IterLambda final : public IterCallback
    {
        IterLambda(T const& fn) : fn_(fn) {}  // NOLINT
        bool operator() (ModelNode const& resolved) const override {return fn_(resolved);};
        T fn_;
    };
    virtual bool iterate(IterCallback const& cb) const; // NOLINT (allow discard)

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
    ModelNode(ModelConstPtr, ModelNodeAddress, ScalarValueType data={});

    /// Extra data for the node
    ScalarValueType data_;

    /// Reference to the model which controls this node
    ModelConstPtr model_;

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
    friend class Model;

    [[nodiscard]] ScalarValueType value() const override;
    [[nodiscard]] ValueType type() const override;
    [[nodiscard]] ModelNode::Ptr get(const FieldId&) const override;
    [[nodiscard]] ModelNode::Ptr at(int64_t) const override;
    [[nodiscard]] FieldId keyAt(int64_t) const override;
    [[nodiscard]] uint32_t size() const override;
    bool iterate(IterCallback const&) const override {return true;}  // NOLINT (allow discard)

protected:
    ModelNodeBase(ModelConstPtr, ModelNodeAddress={}, ScalarValueType data={});  // NOLINT
    ModelNodeBase(ModelNode const&);  // NOLINT
    ModelNodeBase() = default;
};

/**
 * Through the data_ member, a ModelNode can store an arbitrary value.
 */
struct ValueNode final : public ModelNodeBase
{
    explicit ValueNode(ScalarValueType const& value);
    explicit ValueNode(ScalarValueType const& value, ModelConstPtr const& p);
    explicit ValueNode(ModelNode const&);
    [[nodiscard]] ValueType type() const override;
};

/**
 * ModelNode base class which provides a pool() method that returns
 * a reference to a Model-derived pool type.
 * @tparam ModelType Model-derived type.
 */
template<class ModelType>
struct MandatoryDerivedModelNodeBase : public ModelNodeBase
{
    inline ModelType& model() const {return *modelPtr<ModelType>();}  // NOLINT

protected:
    template<class ModelType_ = ModelType>
    inline ModelType_* modelPtr() const {
        static_assert(std::is_base_of<ModelType, ModelType_>::value);
        return reinterpret_cast<ModelType_*>(const_cast<Model*>(model_.get()));
    }  // NOLINT

    MandatoryDerivedModelNodeBase() = default;
    MandatoryDerivedModelNodeBase(ModelConstPtr p, ModelNodeAddress a={}, ScalarValueType data={})  // NOLINT
        : ModelNodeBase(p, a, std::move(data)) {}
    MandatoryDerivedModelNodeBase(ModelNode const& n) : ModelNodeBase(n) {}  // NOLINT
};

using MandatoryModelPoolNodeBase = MandatoryDerivedModelNodeBase<ModelPool>;

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
struct SmallValueNode final : public ModelNodeBase
{
    template<typename> friend struct shared_model_ptr;
    friend class Model;
    [[nodiscard]] ScalarValueType value() const override;
    [[nodiscard]] ValueType type() const override;
protected:
    SmallValueNode() = default;
    SmallValueNode(ModelConstPtr, ModelNodeAddress);
};

template<> [[nodiscard]] ScalarValueType SmallValueNode<int16_t>::value() const;
template<> [[nodiscard]] ValueType SmallValueNode<int16_t>::type() const;
template<>
SmallValueNode<int16_t>::SmallValueNode(ModelConstPtr, ModelNodeAddress);
template<> [[nodiscard]] ScalarValueType SmallValueNode<uint16_t>::value() const;
template<> [[nodiscard]] ValueType SmallValueNode<uint16_t>::type() const;
template<>
SmallValueNode<uint16_t>::SmallValueNode(ModelConstPtr, ModelNodeAddress);
template<> [[nodiscard]] ScalarValueType SmallValueNode<bool>::value() const;
template<> [[nodiscard]] ValueType SmallValueNode<bool>::type() const;
template<>
SmallValueNode<bool>::SmallValueNode(ModelConstPtr, ModelNodeAddress);

/** Model Node for an array. */

struct Array final : public MandatoryModelPoolNodeBase
{
    template<typename> friend struct shared_model_ptr;
    friend class ModelPool;
    friend struct GeometryCollection;

    [[nodiscard]] ValueType type() const override;
    [[nodiscard]] ModelNode::Ptr at(int64_t) const override;
    [[nodiscard]] uint32_t size() const override;
    bool iterate(IterCallback const& cb) const override;  // NOLINT (allow discard)

    template<class ModelNodeType>
    Array& append(shared_model_ptr<ModelNodeType> const& value) {
        return append(static_cast<ModelNode::Ptr>(value));
    }

    Array& append(bool value);
    Array& append(uint16_t value);
    Array& append(int16_t value);
    Array& append(int64_t const& value);
    Array& append(double const& value);
    Array& append(std::string_view const& value);
    Array& append(ModelNode::Ptr const& value={});

    /**
     * Append all elements from `other` to this array.
     */
    Array& extend(shared_model_ptr<Array> const& other);

protected:
    using Storage = ArrayArena<ModelNodeAddress, detail::ColumnPageSize*2>;

    Array() = default;
    Array(ModelConstPtr pool, ModelNodeAddress);

    Storage* storage_ = nullptr;
    ArrayIndex members_ = 0;
};

/** Model Node for an object. */

struct Object : public MandatoryModelPoolNodeBase
{
    template<typename> friend struct shared_model_ptr;
    friend class ModelPool;
    friend class bitsery::Access;

    [[nodiscard]] ValueType type() const override;
    [[nodiscard]] ModelNode::Ptr at(int64_t) const override;
    [[nodiscard]] uint32_t size() const override;
    [[nodiscard]] ModelNode::Ptr get(const FieldId &) const override;
    [[nodiscard]] FieldId keyAt(int64_t) const override;
    bool iterate(IterCallback const& cb) const override;  // NOLINT (allow discard)

    template<class ModelNodeType>
    Object& addField(std::string_view const& name, shared_model_ptr<ModelNodeType> const& value) {
        return addField(name, static_cast<ModelNode::Ptr>(value));
    }

    Object& addBool(std::string_view const& name, bool value);
    Object& addField(std::string_view const& name, uint16_t value);
    Object& addField(std::string_view const& name, int16_t value);
    Object& addField(std::string_view const& name, int64_t const& value);
    Object& addField(std::string_view const& name, double const& value);
    Object& addField(std::string_view const& name, std::string_view const& value);
    Object& addField(std::string_view const& name, ModelNode::Ptr const& value={});

    [[nodiscard]] ModelNode::Ptr get(std::string_view const& fieldName) const;

    /**
     * Adopt all fields from the `other` object into this one.
     */
    Object& extend(shared_model_ptr<Object> const& other);

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

        template<typename S>
        void serialize(S& s) {
            s.value2b(name_);
            s.object(node_);
        }
    };

    using Storage = ArrayArena<Field, detail::ColumnPageSize*2>;

    Object() = default;
    Object(ModelConstPtr pool, ModelNodeAddress);
    Object(ArrayIndex members, ModelConstPtr pool, ModelNodeAddress);

    Storage* storage_ = nullptr;
    ArrayIndex members_ = 0;
};

/** Object with extra procedural fields */

template<uint16_t MaxProceduralFields, class LambdaThisType=Object>
class ProceduralObject : public Object
{
public:
    [[nodiscard]] ModelNode::Ptr at(int64_t i) const override {
        if (i < fields_.size())
            return fields_[i].second(reinterpret_cast<LambdaThisType const&>(*this));
        return Object::at(i - fields_.size());
    }

    [[nodiscard]] uint32_t size() const override {
        return fields_.size() + Object::size();
    }

    [[nodiscard]] ModelNode::Ptr get(const FieldId & field) const override {
        for (auto const& [k, v] : fields_)
            if (k == field)
                return v(reinterpret_cast<LambdaThisType const&>(*this));
        return Object::get(field);
    }

    [[nodiscard]] FieldId keyAt(int64_t i) const override {
        if (i < fields_.size())
            return fields_[i].first;
        return Object::keyAt(i - fields_.size());
    }

    bool iterate(IterCallback const& cb) const override {  // NOLINT (allow discard)
        for (auto const& [k, v] : fields_) {
            auto vv = v(reinterpret_cast<LambdaThisType const&>(*this));
            if (!cb(*vv))
                return false;
        }
        return Object::iterate(cb);
    }

protected:
    ProceduralObject() = default;
    ProceduralObject(ArrayIndex i, ModelConstPtr pool, ModelNodeAddress a)
        : Object(i, pool, a) {}

    sfl::small_vector<
        std::pair<FieldId, std::function<ModelNode::Ptr(LambdaThisType const&)>>,
        MaxProceduralFields> fields_;
};

/**
 * Geometry object, which stores a point collection, a line-string,
 * or a triangle mesh.
 */

struct Geometry final : public MandatoryModelPoolNodeBase
{
    template<typename> friend struct shared_model_ptr;
    friend class ModelPool;
    friend struct VertexNode;
    friend struct VertexBufferNode;

    enum class GeomType: uint8_t {
        Points,   // Point-cloud
        Line,     // Line-string
        Polygon,  // Auto-closed polygon
        Mesh      // Collection of triangles
    };

    [[nodiscard]] ValueType type() const override;
    [[nodiscard]] ModelNode::Ptr at(int64_t) const override;
    [[nodiscard]] uint32_t size() const override;
    [[nodiscard]] ModelNode::Ptr get(const FieldId&) const override;
    [[nodiscard]] FieldId keyAt(int64_t) const override;
    bool iterate(IterCallback const& cb) const override;  // NOLINT (allow discard)

    /** Add a point to the Geometry. */
    void append(geo::Point<double> const& p);

    /** Get the type of the geometry. */
    [[nodiscard]] GeomType geomType() const;

    /** Get the number of points in the geometry buffer. */
    [[nodiscard]] size_t numPoints() const;

    /** Get a point at an index. */
    [[nodiscard]] geo::Point<double> pointAt(size_t index) const;

    /** Iterate over all Points in the geometry.
     * @param callback Function which is called for each contained point.
     *  Must return true to continue iteration, false to abort iteration.
     * @return True if all points were visited, false if the callback ever returned false.
     * @example
     *   collection->forEachPoint([](simfil::geo::Point<double>&& point){
     *      std::cout << point.x() << "," << point.y() << "," << point.z() << std::endl;
     *      return true;
     *   })
     * @note The ModelType must also be templated here, because in this header
     *  the class only exists in a predeclared form.
     */
    template <typename LambdaType, class ModelType=ModelPool>
    bool forEachPoint(LambdaType const& callback) const;

protected:
    struct Data {
        GeomType type = GeomType::Points;

        // Vertex array index, or negative requested initial
        // capacity, if no point is added yet.
        ArrayIndex vertexArray_ = -1;

        // Offset is set when vertexArray is allocated,
        // which happens when the first point is added.
        geo::Point<double> offset_;

        template<typename S>
        void serialize(S& s) {
            s.value1b(type);
            s.value4b(vertexArray_);
            s.object(offset_);
        }
    };

    using Storage = ArrayArena<geo::Point<float>, detail::ColumnPageSize*2>;

    Data* geomData_ = nullptr;
    Storage* storage_ = nullptr;

    Geometry() = default;
    Geometry(Data* data, ModelConstPtr pool, ModelNodeAddress a);
};

/** GeometryCollection node has `type` and `geometries` fields. */

struct GeometryCollection : public MandatoryModelPoolNodeBase
{
    template<typename> friend struct shared_model_ptr;
    friend class ModelPool;
    friend struct GeometryList;

    [[nodiscard]] ValueType type() const override;
    [[nodiscard]] ModelNode::Ptr at(int64_t) const override;
    [[nodiscard]] uint32_t size() const override;
    [[nodiscard]] ModelNode::Ptr get(const FieldId&) const override;
    [[nodiscard]] FieldId keyAt(int64_t) const override;
    bool iterate(IterCallback const& cb) const override;  // NOLINT (allow discard)

    /** Adds a new Geometry to the collection and returns a reference. */
    shared_model_ptr<Geometry> newGeometry(Geometry::GeomType type, size_t initialCapacity=4);

    /** Append an existing Geometry to the collection. */
    void addGeometry(shared_model_ptr<Geometry> const& geom);

    /** Get the number of contained geometries. */
    [[nodiscard]] size_t numGeometries() const;

    /** Iterate over all Geometries in the collection.
     * @param callback Function which is called for each contained geometry.
     *  Must return true to continue iteration, false to abort iteration.
     * @return True if all geometries were visited, false if the callback ever returned false.
     * @example
     *   collection->forEachGeometry([](simfil::shared_model_ptr<Geometry> const& geom){
     *      std::cout << geom->type() << std::endl;
     *      return true;
     *   })
     * @note The ModelType must also be templated here, because in this header
     *  the class only exists in a predeclared form.
     */
    template <typename LambdaType, class ModelType=ModelPool>
    bool forEachGeometry(LambdaType const& callback) const {
        auto geomArray = modelPtr<ModelType>()->arrayMemberStorage().range((ArrayIndex)addr().index());
        return std::all_of(geomArray.begin(), geomArray.end(), [this, &callback](auto&& geomNodeAddress){
            return callback(modelPtr<ModelType>()->resolveGeometry(ModelNode::Ptr::make(model_, geomNodeAddress)));
        });
    }

protected:
    std::optional<ModelNode::Ptr> singleGeom() const;

    using Storage = Array::Storage;

    GeometryCollection() = default;
    GeometryCollection(ModelConstPtr pool, ModelNodeAddress);
};

/** VertexBuffer Node */

struct VertexBufferNode final : public MandatoryModelPoolNodeBase
{
    template<typename> friend struct shared_model_ptr;
    friend class ModelPool;
    friend struct Geometry;

    [[nodiscard]] ValueType type() const override;
    [[nodiscard]] ModelNode::Ptr at(int64_t) const override;
    [[nodiscard]] uint32_t size() const override;
    [[nodiscard]] ModelNode::Ptr get(const FieldId &) const override;
    [[nodiscard]] FieldId keyAt(int64_t) const override;
    bool iterate(IterCallback const& cb) const override;  // NOLINT (allow discard)

protected:
    VertexBufferNode() = default;
    VertexBufferNode(Geometry::Data const* geomData, ModelConstPtr pool, ModelNodeAddress const& a);

    Geometry::Data const* geomData_ = nullptr;
    Geometry::Storage* storage_ = nullptr;
};

/** Vertex Node */

struct VertexNode final : public MandatoryModelPoolNodeBase
{
    template<typename> friend struct shared_model_ptr;
    friend class ModelPool;
    friend struct Geometry;

    [[nodiscard]] ValueType type() const override;
    [[nodiscard]] ModelNode::Ptr at(int64_t) const override;
    [[nodiscard]] uint32_t size() const override;
    [[nodiscard]] ModelNode::Ptr get(const FieldId &) const override;
    [[nodiscard]] FieldId keyAt(int64_t) const override;
    bool iterate(IterCallback const& cb) const override;  // NOLINT (allow discard)

protected:
    VertexNode() = default;
    VertexNode(ModelNode const& baseNode, Geometry::Data const& geomData);

    geo::Point<double> point_;
};

template <typename LambdaType, class ModelType>
bool Geometry::forEachPoint(LambdaType const& callback) const {
    VertexBufferNode vertexBufferNode{geomData_, model_, {ModelType::PointBuffers, addr_.index()}};
    for (auto i = 0; i < vertexBufferNode.size(); ++i) {
        VertexNode vertex{*vertexBufferNode.at(i), *geomData_};
        if (!callback(vertex.point_))
            return false;
    }
    return true;
}

}
