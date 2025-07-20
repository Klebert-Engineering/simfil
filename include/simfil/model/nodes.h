#pragma once

#include <tl/expected.hpp>
#include <memory>
#include <type_traits>
#include <variant>
#include <functional>
#include <bitset>

#include "arena.h"
#include "string-pool.h"
#include "simfil/error.h"

#include <sfl/small_vector.hpp>

#if defined(SIMFIL_WITH_MODEL_JSON)
#  include "nlohmann/json.hpp"
#endif

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
struct Environment;
struct Diagnostics;
class AST;
class Expr;

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
    // If you add types, update TypeFlags::flags bit size!
};

using ScalarValueType = std::variant<
    std::monostate,
    bool,
    int64_t,
    double,
    std::string,
    std::string_view>;

/**
 * Why is model_ptr's value on the stack?
 *
 * All ModelNode types are actually pointers into a ModelPool, via their
 * nested ModelNodeAddress. They keep the ModelPool which they reference
 * alive. The model_ptr wrapper ensures that the user has
 * a better sense of the object they are dealing with. Only model_ptr
 * is allowed to copy ModelNodes - so e.g. `auto node = *nodePtr` is not possible.
 */
template<typename T>
struct model_ptr
{
    template<typename> friend struct model_ptr;

    model_ptr(std::nullptr_t) {}  // NOLINT
    model_ptr(T&& modelNode) : data_(std::move(modelNode)) {}  // NOLINT
    explicit model_ptr(T const& modelNode) : data_(modelNode) {}  // NOLINT

    model_ptr() = default;
    model_ptr(const model_ptr&) = default;
    model_ptr(model_ptr&&) = default;

    model_ptr& operator=(model_ptr const&) = default;
    model_ptr& operator=(model_ptr&&) = default;

    template<typename OtherT>
    model_ptr(model_ptr<OtherT> const& other) : data_(other.data_) {}  // NOLINT

    template<typename OtherT>
    model_ptr(model_ptr<OtherT>&& other) : data_(std::move(other.data_)) {}  // NOLINT

    template<typename OtherT>
    model_ptr& operator= (model_ptr<OtherT> const& other) {data_ = other.data_; return *this;}

    template<typename OtherT>
    model_ptr& operator= (model_ptr<OtherT>&& other) {data_ = std::move(other.data_); return *this;}

    template<typename... Args>
    explicit model_ptr(std::in_place_t, Args&&... args) : data_(std::forward<Args>(args)...) {}

    static_assert(std::is_base_of_v<ModelNode, T>, "T must inherit from ModelNode.");

    template<typename... Args>
    static model_ptr<T> make(Args&&... args) {
        return model_ptr(std::in_place, std::forward<Args>(args)...);
    }

    inline void ensureModelIsNotNull() const {
        if (!data_.model_ || !data_.addr_) {
            raise<std::runtime_error>("Attempt to dereference null model_ptr!");
        }
    }

    inline T& operator* () {
        ensureModelIsNotNull();
        return data_;
    }

    inline T* operator-> () {
        ensureModelIsNotNull();
        return &data_;
    }

    inline T const& operator* () const {
        ensureModelIsNotNull();
        return data_;
    }

    inline T const* operator-> () const {
        ensureModelIsNotNull();
        return &data_;
    }

    inline operator bool () const {return data_.addr_;}  // NOLINT (allow implicit bool cast)

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

    [[nodiscard]] operator bool() const {  // NOLINT (allow implicit bool cast)
        return value_ != 0;
    }

    template<typename S>
    void serialize(S& s) {
        s.value4b(value_);
    }
};

/** Semantic view onto a particular node in a ModelPool. */
struct ModelNode
{
    using Ptr = model_ptr<ModelNode>;

    template<typename> friend struct model_ptr;
    friend class ModelPool;
    friend class Model;
    friend class OverlayNode;
    friend auto eval(Environment& env, const AST& ast, const ModelNode& node, Diagnostics*) -> tl::expected<std::vector<Value>, Error>;

    /// Get the node's scalar value if it has one
    [[nodiscard]] virtual ScalarValueType value() const;

    /// Get the node's abstract model type
    [[nodiscard]] virtual ValueType type() const;

    /// Get a child by name
    [[nodiscard]] virtual Ptr get(StringHandle const& f) const;

    /// Get a child by index
    [[nodiscard]] virtual Ptr at(int64_t i) const;

    /// Get an Object model's field names
    [[nodiscard]] virtual StringHandle keyAt(int64_t i) const;

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
    /// * `fieldNames()`: Returns range object which supports `for(StringId const& f: node.fieldNames())`
    /// * `fields()`: Returns range object which supports `for(auto const& [fieldId, value] : node.fields())`
    /// * Normal `begin()`/`end()`: Supports `for(ModelNode::Ptr child : node)`

    // Implement fieldNames() by creating an iterator for StringId
    class StringHandleIterator
    {
        int64_t index;
        ModelNode const* parent;
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = StringHandle;
        using difference_type = std::ptrdiff_t;
        using pointer = StringHandle*;
        using reference = StringHandle&;
        StringHandleIterator(int64_t idx, ModelNode const* p) : index(idx), parent(p) {}
        StringHandleIterator& operator++() { ++index; return *this; }
        bool operator==(const StringHandleIterator& other) const { return index == other.index; }
        bool operator!=(const StringHandleIterator& other) const { return index != other.index; }
        StringHandle operator*() const { return parent->keyAt(index); }
    };
    class StringIdRange
    {
        ModelNode const* node;
    public:
        explicit StringIdRange(ModelNode const* const n) : node(n) {}
        [[nodiscard]] StringHandleIterator begin() const { return node->fieldNamesBegin(); }
        [[nodiscard]] StringHandleIterator end() const { return node->fieldNamesEnd(); }
    };
    [[nodiscard]] StringHandleIterator fieldNamesBegin() const { return {0, this}; }
    [[nodiscard]] StringHandleIterator fieldNamesEnd() const { return {size(), this}; }
    [[nodiscard]] auto fieldNames() const { return StringIdRange{this}; }

    // Implement fields() by creating an iterator for field-value pairs
    class FieldIterator
    {
        int64_t index;
        ModelNode const* parent;
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = std::pair<StringHandle, Ptr>;
        using difference_type = std::ptrdiff_t;
        using pointer = value_type*;
        using reference = value_type&;
        FieldIterator(int64_t const idx, ModelNode const* const p) : index(idx), parent(p) {}
        FieldIterator& operator++() { ++index; return *this; }
        bool operator==(const FieldIterator& other) const { return index == other.index; }
        bool operator!=(const FieldIterator& other) const { return index != other.index; }
        value_type operator*() const { return std::make_pair(parent->keyAt(index), parent->at(index)); }
    };
    class FieldRange
    {
        ModelNode const* node;
    public:
        explicit FieldRange(ModelNode const* const n) : node(n) {}
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
        ChildIterator(int64_t const idx, ModelNode const* const p) : index(idx), parent(p) {}
        ChildIterator& operator++() { ++index; return *this; }
        bool operator==(const ChildIterator& other) const { return index == other.index; }
        bool operator!=(const ChildIterator& other) const { return index != other.index; }
        Ptr operator*() const { return parent->at(index); }
    };
    [[nodiscard]] ChildIterator begin() const { return {0, this}; }
    [[nodiscard]] ChildIterator end() const { return {size(), this}; }

#if defined(SIMFIL_WITH_MODEL_JSON)
    [[nodiscard]] virtual nlohmann::json toJson() const;
#endif

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
    [[nodiscard]] ModelNode::Ptr get(const StringHandle&) const override;
    [[nodiscard]] ModelNode::Ptr at(int64_t) const override;
    [[nodiscard]] StringHandle keyAt(int64_t) const override;
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
        return static_cast<ModelType_*>(const_cast<Model*>(model_.get()));
    }

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
    template<typename> friend struct model_ptr;
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

/** Model Node for an array from which typed/untyped arrays may be derived. */

template <class ModelType, class ModelNodeType>
struct BaseArray : public MandatoryDerivedModelNodeBase<ModelType>
{
    using Storage = ArrayArena<ModelNodeAddress, detail::ColumnPageSize*2>;

    template<typename> friend struct model_ptr;
    friend class ModelPool;

    template<class OtherModelNodeType>
    requires std::derived_from<OtherModelNodeType, ModelNodeType>
    BaseArray& append(model_ptr<OtherModelNodeType> const& value) {
        return appendInternal(static_cast<ModelNode::Ptr>(value));
    }

    bool forEach(std::function<bool(ModelNodeType const&)> const& callback) const;

    [[nodiscard]] ValueType type() const override;
    [[nodiscard]] ModelNode::Ptr at(int64_t) const override;
    [[nodiscard]] uint32_t size() const override;
    bool iterate(ModelNode::IterCallback const& cb) const override;  // NOLINT (allow discard)

protected:
    BaseArray() = default;
    BaseArray(ModelConstPtr pool, ModelNodeAddress);
    BaseArray& appendInternal(ModelNode::Ptr const& value={});

    using ModelNode::model_;
    using MandatoryDerivedModelNodeBase<ModelType>::model;

    Storage* storage_ = nullptr;
    ArrayIndex members_ = 0;
};

/** Model Node for a mixed-type array. */

struct Array : public BaseArray<ModelPool, ModelNode>
{
    template<typename> friend struct model_ptr;
    friend class ModelPool;

    using BaseArray::append;

    Array& append(bool value);
    Array& append(uint16_t value);
    Array& append(int16_t value);
    Array& append(int64_t const& value);
    Array& append(double const& value);
    Array& append(std::string_view const& value);

    /**
     * Append all elements from `other` to this array.
     */
    Array& extend(model_ptr<Array> const& other);

protected:
    Array() = default;
    using BaseArray::BaseArray;
};

/** Model Node for an object from which typed/untyped objects may be derived. */

template <class ModelType, class ModelNodeType>
struct BaseObject : public MandatoryDerivedModelNodeBase<ModelType>
{
    template<typename> friend struct model_ptr;
    friend class ModelPool;
    friend class bitsery::Access;

    template<class OtherModelNodeType>
    requires std::derived_from<OtherModelNodeType, ModelNodeType>
    BaseObject& addField(std::string_view const& name, model_ptr<OtherModelNodeType> const& value) {
        return addFieldInternal(name, static_cast<ModelNode::Ptr>(value));
    }

    [[nodiscard]] ValueType type() const override;
    [[nodiscard]] ModelNode::Ptr at(int64_t) const override;
    [[nodiscard]] uint32_t size() const override;
    [[nodiscard]] ModelNode::Ptr get(const StringHandle&) const override;
    [[nodiscard]] StringHandle keyAt(int64_t) const override;
    bool iterate(ModelNode::IterCallback const& cb) const override;  // NOLINT (allow discard)

protected:
    /**
     * Object field - a name and a tree node address.
     * These are stored in the ModelPools Field array arena.
     */
    struct Field
    {
        Field() = default;
        Field(StringId name, ModelNodeAddress a) : name_(name), node_(a) {}
        Field(StringHandle name, ModelNodeAddress a) : name_(static_cast<StringId>(name)), node_(a) {}
        StringId name_ = StringPool::Empty;
        ModelNodeAddress node_;

        template<typename S>
        void serialize(S& s) {
            s.value2b(name_);
            s.object(node_);
        }
    };

    using Storage = ArrayArena<Field, detail::ColumnPageSize*2>;
    using ModelNode::model_;
    using MandatoryDerivedModelNodeBase<ModelType>::model;

    BaseObject() = default;
    BaseObject(ModelConstPtr pool, ModelNodeAddress);
    BaseObject(ArrayIndex members, ModelConstPtr pool, ModelNodeAddress);

    BaseObject& addFieldInternal(std::string_view const& name, ModelNode::Ptr const& value={});

    Storage* storage_ = nullptr;
    ArrayIndex members_ = 0;
};

/** Model Node for an object. */

struct Object : public BaseObject<ModelPool, ModelNode>
{
    template<typename> friend struct model_ptr;
    friend class ModelPool;
    friend class bitsery::Access;

    using BaseObject<ModelPool, ModelNode>::get;
    using BaseObject<ModelPool, ModelNode>::addField;

    Object& addBool(std::string_view const& name, bool value);
    Object& addField(std::string_view const& name, uint16_t value);
    Object& addField(std::string_view const& name, int16_t value);
    Object& addField(std::string_view const& name, int64_t const& value);
    Object& addField(std::string_view const& name, double const& value);
    Object& addField(std::string_view const& name, std::string_view const& value);
    Object& addField(std::string_view const& name, StringHandle const& value);

    [[nodiscard]] ModelNode::Ptr get(std::string_view const& fieldName) const;

    /**
     * Adopt all fields from the `other` object into this one.
     */
    Object& extend(model_ptr<Object> const& other);

protected:
    Object() = default;
    using BaseObject<ModelPool, ModelNode>::BaseObject;
};

/** Object with extra procedural fields */

template<uint16_t MaxProceduralFields, class LambdaThisType=Object, class ModelPoolDerivedModel=ModelPool>
class ProceduralObject : public Object
{
public:
    [[nodiscard]] ModelNode::Ptr at(int64_t i) const override {
        if (i < fields_.size())
            return fields_[i].second(static_cast<LambdaThisType const&>(*this));
        if (members_ != InvalidArrayIndex)
            return Object::at(i - fields_.size());
        return {};
    }

    [[nodiscard]] uint32_t size() const override {
        return fields_.size() + (members_ != InvalidArrayIndex ? Object::size() : 0);
    }

    [[nodiscard]] ModelNode::Ptr get(const StringHandle& field) const override {
        for (auto const& [k, v] : fields_)
            if (field == k)
                return v(static_cast<LambdaThisType const&>(*this));
        if (members_ != InvalidArrayIndex)
            return Object::get(field);
        return {};
    }

    [[nodiscard]] StringHandle keyAt(int64_t i) const override {
        if (i < fields_.size())
            return fields_[i].first;
        if (members_ != InvalidArrayIndex)
            return Object::keyAt(i - fields_.size());
        return StringPool::Empty;
    }

    bool iterate(IterCallback const& cb) const override {  // NOLINT (allow discard)
        for (auto const& [k, v] : fields_) {
            auto vv = v(static_cast<LambdaThisType const&>(*this));
            if (!cb(*vv))
                return false;
        }
        if (members_ != InvalidArrayIndex)
            return Object::iterate(cb);
        return true;
    }

    inline ModelPoolDerivedModel& model() const {return *modelPtr<ModelPoolDerivedModel>();}  // NOLINT

protected:
    ProceduralObject() = default;
    ProceduralObject(ArrayIndex i, ModelConstPtr pool, ModelNodeAddress a)
        : Object(i, pool, a) {}
    ProceduralObject(ModelConstPtr pool, ModelNodeAddress a)
        : Object(InvalidArrayIndex, pool, a) {}

    sfl::small_vector<
        std::pair<StringId, std::function<ModelNode::Ptr(LambdaThisType const&)>>,
        MaxProceduralFields> fields_;
};

}
