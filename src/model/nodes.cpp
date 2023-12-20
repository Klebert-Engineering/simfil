#include "simfil/model/model.h"
#include "simfil/value.h"
#include "simfil/model/nodes.h"

namespace simfil
{

static std::string_view GeometryCollectionStr("GeometryCollection");
static std::string_view MultiPointStr("MultiPoint");
static std::string_view LineStringStr("LineString");
static std::string_view PolygonStr("Polygon");
static std::string_view MultiPolygonStr("MultiPolygon");

/// Create a ModelNode from a model pool which serves as its
/// VFT, and a TreeNodeAddress.
ModelNode::ModelNode(ModelConstPtr pool, ModelNodeAddress addr, ScalarValueType data)
    : model_(std::move(pool)), addr_(addr), data_(std::move(data))
{}

/// Get the node's scalar value if it has one
ScalarValueType ModelNode::value() const {
    ScalarValueType result;
    if (model_)
        model_->resolve(*this, Model::Lambda([&](auto&& resolved) { result = resolved.value(); }));
    return result;
}

/// Get the node's abstract model type
ValueType ModelNode::type() const {
    ValueType result = ValueType::Null;
    if (model_)
        model_->resolve(*this, Model::Lambda([&](auto&& resolved) { result = resolved.type(); }));
    return result;
}

/// Get a child by name
ModelNode::Ptr ModelNode::get(const FieldId& field) const {
    ModelNode::Ptr result;
    if (model_)
        model_
            ->resolve(*this, Model::Lambda([&](auto&& resolved) { result = resolved.get(field); }));
    return result;
}

/// Get a child by index
ModelNode::Ptr ModelNode::at(int64_t index) const {
    ModelNode::Ptr result;
    if (model_)
        model_
            ->resolve(*this, Model::Lambda([&](auto&& resolved) { result = resolved.at(index); }));
    return result;
}

/// Get an Object model's field names
FieldId ModelNode::keyAt(int64_t i) const {
    FieldId result = 0;
    if (model_)
        model_->resolve(*this, Model::Lambda([&](auto&& resolved) { result = resolved.keyAt(i); }));
    return result;
}

/// Get the number of children
uint32_t ModelNode::size() const {
    uint32_t result = 0;
    if (model_)
        model_->resolve(*this, Model::Lambda([&](auto&& resolved) { result = resolved.size(); }));
    return result;
}

/// Fast iteration
bool ModelNode::iterate(const IterCallback& cb) const {
    bool result = true;
    if (model_)
        model_->resolve(*this, Model::Lambda([&](auto&& resolved) { result = resolved.iterate(cb); }));
    return result;
}

/** Model Node Base Impl. */

ModelNodeBase::ModelNodeBase(ModelConstPtr pool, ModelNodeAddress addr, ScalarValueType data)
    : ModelNode(std::move(pool), addr, std::move(data))
{
}

ModelNodeBase::ModelNodeBase(const ModelNode& n)
    : ModelNode(n)
{
}

ScalarValueType ModelNodeBase::value() const
{
    return data_;
}

ValueType ModelNodeBase::type() const
{
    return ValueType::Null;
}

ModelNode::Ptr ModelNodeBase::get(const FieldId&) const
{
    return nullptr;
}

ModelNode::Ptr ModelNodeBase::at(int64_t) const
{
    return nullptr;
}

FieldId ModelNodeBase::keyAt(int64_t) const
{
    return 0;
}

uint32_t ModelNodeBase::size() const
{
    return 0;
}

/** Model Node impls. for arbitrary self-contained value storage. */

ValueNode::ValueNode(ScalarValueType const& value)
    : ModelNodeBase(std::make_shared<Model>(), Model::Scalar, value)
{}

ValueNode::ValueNode(const ScalarValueType& value, const ModelConstPtr& p)
    : ModelNodeBase(p, Model::Scalar, value)
{}

ValueNode::ValueNode(ModelNode const& n) : ModelNodeBase(n) {}

ValueType ValueNode::type() const {
    ValueType result;
    std::visit([&result](auto&& v){
        result = ValueType4CType<std::decay_t<decltype(v)>>::Type;
    }, data_);
    return result;
}

/** Model Node impls. for SmallValueNode */

template<> ScalarValueType SmallValueNode<int16_t>::value() const {
    return (int64_t)addr_.int16();
}

template<> ValueType SmallValueNode<int16_t>::type() const {
    return ValueType::Int;
}

template<>
SmallValueNode<int16_t>::SmallValueNode(ModelConstPtr p, ModelNodeAddress a)
    : ModelNodeBase(std::move(p), a)
{}

template<> ScalarValueType SmallValueNode<uint16_t>::value() const {
    return (int64_t)addr_.uint16();
}

template<> ValueType SmallValueNode<uint16_t>::type() const {
    return ValueType::Int;
}

template<>
SmallValueNode<uint16_t>::SmallValueNode(ModelConstPtr p, ModelNodeAddress a)
    : ModelNodeBase(std::move(p), a)
{}

template<> ScalarValueType SmallValueNode<bool>::value() const {
    return (bool)addr_.uint16();
}

template<> ValueType SmallValueNode<bool>::type() const {
    return ValueType::Bool;
}

template<>
SmallValueNode<bool>::SmallValueNode(ModelConstPtr p, ModelNodeAddress a)
    : ModelNodeBase(std::move(p), a)
{}

/** Model Node impls for an array. */

Array::Array(ModelConstPtr pool_, ModelNodeAddress a)
    : MandatoryModelPoolNodeBase(std::move(pool_), a), storage_(nullptr), members_((ArrayIndex)a.index())
{
    storage_ = &model().arrayMemberStorage();
}

ValueType Array::type() const
{
    return ValueType::Array;
}

ModelNode::Ptr Array::at(int64_t i) const
{
    if (i < 0 || i >= (int64_t)storage_->size(members_))
        return {};
    return ModelNode::Ptr::make(model_, storage_->at(members_, i));
}

uint32_t Array::size() const
{
    return (uint32_t)storage_->size(members_);
}

Array& Array::append(bool value) {storage_->push_back(members_, model().newSmallValue(value)->addr()); return *this;}
Array& Array::append(uint16_t value) {storage_->push_back(members_, model().newSmallValue(value)->addr()); return *this;}
Array& Array::append(int16_t value) {storage_->push_back(members_, model().newSmallValue(value)->addr()); return *this;}
Array& Array::append(int64_t const& value) {storage_->push_back(members_, model().newValue(value)->addr()); return *this;}
Array& Array::append(double const& value) {storage_->push_back(members_, model().newValue(value)->addr()); return *this;}
Array& Array::append(std::string_view const& value) {storage_->push_back(members_, model().newValue(value)->addr()); return *this;}
Array& Array::append(ModelNode::Ptr const& value) {storage_->push_back(members_, value->addr()); return *this;}

bool Array::iterate(const ModelNode::IterCallback& cb) const
{
    auto cont = true;
    auto resolveAndCb = Model::Lambda([&cb, &cont](auto && node){
        cont = cb(node);
    });
    storage_->iterate(members_, [&, this](auto&& member){
            model_->resolve(*ModelNode::Ptr::make(model_, member), resolveAndCb);
        return cont;
    });
    return cont;
}

Array& Array::extend(shared_model_ptr<Array> const& other) {
    auto otherSize = other->size();
    for (auto i = 0u; i < otherSize; ++i) {
        storage_->push_back(members_, storage_->at(other->members_, i));
    }
    return *this;
}

/** Model Node impls for an object. */

Object::Object(ModelConstPtr pool_, ModelNodeAddress a)
    : MandatoryModelPoolNodeBase(std::move(pool_), a), storage_(nullptr), members_((ArrayIndex)a.index())
{
    storage_ = &model().objectMemberStorage();
}

Object::Object(ArrayIndex members, ModelConstPtr pool_, ModelNodeAddress a)
    : MandatoryModelPoolNodeBase(std::move(pool_), a), storage_(nullptr), members_(members)
{
    storage_ = &model().objectMemberStorage();
}

ValueType Object::type() const
{
    return ValueType::Object;
}

ModelNode::Ptr Object::at(int64_t i) const
{
    if (i < 0 || i >= (int64_t)storage_->size(members_))
        return {};
    return ModelNode::Ptr::make(model_, storage_->at(members_, i).node_);
}

FieldId Object::keyAt(int64_t i) const {
    if (i < 0 || i >= (int64_t)storage_->size(members_))
        return {};
    return storage_->at(members_, i).name_;
}

uint32_t Object::size() const
{
    return (uint32_t)storage_->size(members_);
}

ModelNode::Ptr Object::get(const FieldId & field) const
{
    ModelNode::Ptr result;
    storage_->iterate(members_, [&field, &result, this](auto&& member){
        if (member.name_ == field) {
            result = ModelNode::Ptr::make(model_, member.node_);
            return false;
        }
        return true;
    });
    return result;
}

ModelNode::Ptr Object::get(std::string_view const& fieldName) const {
    auto fieldId = model().fieldNames()->emplace(fieldName);
    return get(fieldId);
}

Object& Object::addBool(std::string_view const& name, bool value) {
    auto fieldId = model().fieldNames()->emplace(name);
    storage_->emplace_back(members_, fieldId, model().newSmallValue(value)->addr());
    return *this;
}

Object& Object::addField(std::string_view const& name, uint16_t value) {
    auto fieldId = model().fieldNames()->emplace(name);
    storage_->emplace_back(members_, fieldId, model().newSmallValue(value)->addr());
    return *this;
}

Object& Object::addField(std::string_view const& name, int16_t value) {
    auto fieldId = model().fieldNames()->emplace(name);
    storage_->emplace_back(members_, fieldId, model().newSmallValue(value)->addr());
    return *this;
}

Object& Object::addField(std::string_view const& name, int64_t const& value) {
    auto fieldId = model().fieldNames()->emplace(name);
    storage_->emplace_back(members_, fieldId, model().newValue(value)->addr());
    return *this;
}

Object& Object::addField(std::string_view const& name, double const& value) {
    auto fieldId = model().fieldNames()->emplace(name);
    storage_->emplace_back(members_, fieldId, model().newValue(value)->addr());
    return *this;
}

Object& Object::addField(std::string_view const& name, std::string_view const& value) {
    auto fieldId = model().fieldNames()->emplace(name);
    storage_->emplace_back(members_, fieldId, model().newValue(value)->addr());
    return *this;
}

Object& Object::addField(std::string_view const& name, ModelNode::Ptr const& value) {
    auto fieldId = model().fieldNames()->emplace(name);
    storage_->emplace_back(members_, fieldId, value->addr());
    return *this;
}

bool Object::iterate(const ModelNode::IterCallback& cb) const
{
    auto cont = true;
    auto resolveAndCb = Model::Lambda([&cb, &cont](auto && node){
        cont = cb(node);
    });
    storage_->iterate(members_, [&, this](auto&& member) {
        model_->resolve(*ModelNode::Ptr::make(model_, member.node_), resolveAndCb);
        return cont;
    });
    return cont;
}

Object& Object::extend(shared_model_ptr<Object> const& other)
{
    auto otherSize = other->size();
    for (auto i = 0u; i < otherSize; ++i) {
        storage_->push_back(members_, storage_->at(other->members_, i));
    }
    return *this;
}

/** Model node impls. for GeometryCollection */

GeometryCollection::GeometryCollection(ModelConstPtr pool_, ModelNodeAddress a)
    : MandatoryModelPoolNodeBase(std::move(pool_), a)
{
}

ValueType GeometryCollection::type() const {
    return ValueType::Object;
}

ModelNode::Ptr GeometryCollection::at(int64_t i) const {
    if (auto singleGeomEntry = singleGeom())
        return (*singleGeomEntry)->at(i);
    if (i == 0) return ValueNode(GeometryCollectionStr, model_);
    if (i == 1) return ModelNode::Ptr::make(model_, ModelNodeAddress{ModelPool::Arrays, addr_.index()});
    throw std::out_of_range("geom collection: Out of range.");
}

uint32_t GeometryCollection::size() const {
    if (auto singleGeomEntry = singleGeom())
        return (*singleGeomEntry)->size();
    return 2;
}

ModelNode::Ptr GeometryCollection::get(const FieldId& f) const {
    if (auto singleGeomEntry = singleGeom())
        return (*singleGeomEntry)->get(f);
    if (f == Fields::Type) return at(0);
    if (f == Fields::Geometries) return at(1);
    return {};
}

FieldId GeometryCollection::keyAt(int64_t i) const {
    if (auto singleGeomEntry = singleGeom())
        return (*singleGeomEntry)->keyAt(i);
    if (i == 0) return Fields::Type;
    if (i == 1) return Fields::Geometries;
    throw std::out_of_range("geom collection: Out of range.");
}

shared_model_ptr<Geometry> GeometryCollection::newGeometry(Geometry::GeomType type, size_t initialCapacity) {
    auto result = model().newGeometry(type, initialCapacity);
    auto arrayPtr = ModelNode::Ptr::make(model_, ModelNodeAddress{ModelPool::Arrays, addr_.index()});
    model().resolveArray(arrayPtr)->append(result);
    return result;
}

bool GeometryCollection::iterate(const IterCallback& cb) const
{
    if (auto singleGeomEntry = singleGeom())
        return (*singleGeomEntry)->iterate(cb);
    if (!cb(*at(0))) return false;
    if (!cb(*at(1))) return false;
    return true;
}

std::optional<ModelNode::Ptr> GeometryCollection::singleGeom() const
{
    if (model().arrayMemberStorage().size((ArrayIndex)addr_.index()) == 1) {
        auto arrayPtr = ModelNode::Ptr::make(model_, ModelNodeAddress{ModelPool::Arrays, addr_.index()});
        return model().resolveArray(arrayPtr)->at(0);
    }
    return {};
}

void GeometryCollection::addGeometry(const shared_model_ptr<Geometry>& geom)
{
    auto arrayPtr = ModelNode::Ptr::make(model_, ModelNodeAddress{ModelPool::Arrays, addr_.index()});
    model().resolveArray(arrayPtr)->append(ModelNode::Ptr(geom));
}

size_t GeometryCollection::numGeometries() const
{
    return model().arrayMemberStorage().size((ArrayIndex)addr().index());
}

/** ModelNode impls. for Geometry */

Geometry::Geometry(Data* data, ModelConstPtr pool_, ModelNodeAddress a)
    : MandatoryModelPoolNodeBase(std::move(pool_), a), geomData_(data)
{
    storage_ = &model().vertexBufferStorage();
}

ValueType Geometry::type() const {
    return ValueType::Object;
}

ModelNode::Ptr Geometry::at(int64_t i) const {
    if (i == 0) return ValueNode(
        geomData_->type_ == GeomType::Points ? MultiPointStr :
        geomData_->type_ == GeomType::Line ? LineStringStr :
        geomData_->type_ == GeomType::Polygon ? PolygonStr :
        geomData_->type_ == GeomType::Mesh ? MultiPolygonStr : "",
            model_);
    if (i == 1) return ModelNode::Ptr::make(
        model_, ModelNodeAddress{ModelPool::PointBuffers, addr_.index()});
    throw std::out_of_range("geom: Out of range.");
}

uint32_t Geometry::size() const {
    return 2;
}

ModelNode::Ptr Geometry::get(const FieldId& f) const {
    if (f == Fields::Type) return at(0);
    if (f == Fields::Coordinates) return at(1);
    return {};
}

FieldId Geometry::keyAt(int64_t i) const {
    if (i == 0) return Fields::Type;
    if (i == 1) return Fields::Coordinates;
    throw std::out_of_range("geom: Out of range.");
}

void Geometry::append(geo::Point<double> const& p)
{
    if (geomData_->isView_)
        throw std::runtime_error("Cannot append to geometry view.");

    auto& geomData = geomData_->detail_.geom_;

    // Before the geometry is assigned with a vertex array,
    // a negative array handle denotes the desired initial
    // capacity, +1, because there is always the additional
    // offset point.
    if (geomData.vertexArray_ < 0) {
        auto initialCapacity = abs(geomData_->detail_.geom_.vertexArray_);
        geomData.vertexArray_ = storage_->new_array(initialCapacity-1);
        geomData.offset_ = p;
        return;
    }
    storage_->emplace_back(
        geomData.vertexArray_,
        geo::Point<float>{
            static_cast<float>(p.x - geomData.offset_.x),
            static_cast<float>(p.y - geomData.offset_.y),
            static_cast<float>(p.z - geomData.offset_.z)});
}

Geometry::GeomType Geometry::geomType() const {
    return geomData_->type_;
}

bool Geometry::iterate(const IterCallback& cb) const
{
    if (!cb(*at(0))) return false;
    if (!cb(*at(1))) return false;
    return true;
}

size_t Geometry::numPoints() const
{
    VertexBufferNode vertexBufferNode{geomData_, model_, {ModelPool::PointBuffers, addr_.index()}};
    return vertexBufferNode.size();
}

geo::Point<double> Geometry::pointAt(size_t index) const
{
    VertexBufferNode vertexBufferNode{geomData_, model_, {ModelPool::PointBuffers, addr_.index()}};
    VertexNode vertex{*vertexBufferNode.at((int64_t)index), vertexBufferNode.baseGeomData_};
    return vertex.point_;
}

/** ModelNode impls. for VertexBufferNode */

VertexBufferNode::VertexBufferNode(Geometry::Data const* geomData, ModelConstPtr pool_, ModelNodeAddress const& a)
    : MandatoryModelPoolNodeBase(std::move(pool_), a), baseGeomData_(geomData), baseGeomAddress_(a)
{
    storage_ = &model().vertexBufferStorage();

    // Resolve geometry view to actual geometry, process
    // actual offset and length.
    if (baseGeomData_->isView_) {
        size_ = baseGeomData_->detail_.view_.size_;

        while (baseGeomData_->isView_) {
            offset_ += baseGeomData_->detail_.view_.offset_;
            baseGeomAddress_ = baseGeomData_->detail_.view_.baseGeometry_;
            baseGeomData_ = model().resolveGeometry(
                ModelNode::Ptr::make(model_, baseGeomData_->detail_.view_.baseGeometry_))->geomData_;
        }

        auto maxSize = 1 + storage_->size(baseGeomData_->detail_.geom_.vertexArray_);
        if (offset_ + size_ > maxSize)
            throw std::runtime_error("Geometry view is out of bounds.");
    }
    else {
        // Just get the correct length.
        if (baseGeomData_->detail_.geom_.vertexArray_ >= 0)
            size_ = 1 + storage_->size(baseGeomData_->detail_.geom_.vertexArray_);
    }
}

ValueType VertexBufferNode::type() const {
    return ValueType::Array;
}

ModelNode::Ptr VertexBufferNode::at(int64_t i) const {
    if (i < 0 || i >= size())
        throw std::out_of_range("vertex-buffer: Out of range.");
    i += offset_;
    return ModelNode::Ptr::make(model_, ModelNodeAddress{ModelPool::Points, baseGeomAddress_.index()}, i);
}

uint32_t VertexBufferNode::size() const {
    return size_;
}

ModelNode::Ptr VertexBufferNode::get(const FieldId &) const {
    return {};
}

FieldId VertexBufferNode::keyAt(int64_t) const {
    return {};
}

bool VertexBufferNode::iterate(const IterCallback& cb) const
{
    auto cont = true;
    auto resolveAndCb = Model::Lambda([&cb, &cont](auto && node){
        cont = cb(node);
    });
    for (auto i = 0u; i < size_; ++i) {
        resolveAndCb(*ModelNode::Ptr::make(
            model_, ModelNodeAddress{ModelPool::Points, baseGeomAddress_.index()}, (int64_t)i+offset_));
        if (!cont)
            break;
    }
    return cont;
}

/** Model node impls for vertex. */

VertexNode::VertexNode(ModelNode const& baseNode, Geometry::Data const* geomData)
    : MandatoryModelPoolNodeBase(baseNode)
{
    if (geomData->isView_)
        throw std::runtime_error("Point must be constructed through VertexBuffer which resolves view to geometry.");
    auto i = std::get<int64_t>(data_);
    point_ = geomData->detail_.geom_.offset_;
    if (i > 0)
        point_ += model().vertexBufferStorage().at(geomData->detail_.geom_.vertexArray_, i - 1);
}

ValueType VertexNode::type() const {
    return ValueType::Array;
}

ModelNode::Ptr VertexNode::at(int64_t i) const {
    if (i == 0) return shared_model_ptr<ValueNode>::make(point_.x, model_);
    if (i == 1) return shared_model_ptr<ValueNode>::make(point_.y, model_);
    if (i == 2) return shared_model_ptr<ValueNode>::make(point_.z, model_);
    throw std::out_of_range("vertex: Out of range.");
}

uint32_t VertexNode::size() const {
    return 3;
}

ModelNode::Ptr VertexNode::get(const FieldId & field) const {
    if (field == Fields::Lon) return at(0);
    if (field == Fields::Lat) return at(1);
    if (field == Fields::Elevation) return at(2);
    else return {};
}

FieldId VertexNode::keyAt(int64_t i) const {
    if (i == 0) return Fields::Lon;
    if (i == 1) return Fields::Lat;
    if (i == 2) return Fields::Elevation;
    throw std::out_of_range("vertex: Out of range.");
}

bool VertexNode::iterate(const IterCallback& cb) const
{
    if (!cb(ValueNode(point_.x, model_))) return false;
    if (!cb(ValueNode(point_.y, model_))) return false;
    if (!cb(ValueNode(point_.z, model_))) return false;
    return true;
}

}
