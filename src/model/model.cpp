#include "simfil/model/model.h"
#include "simfil/model/arena.h"
#include "simfil/value.h"
#include "simfil/overlay.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <unordered_map>
#include <vector>
#include "stx/format.h"

#include <sfl/segmented_vector.hpp>

namespace simfil
{

void ModelPoolBase::resolve(
    const ModelNode& n,
    const std::function<void(ModelNode&&)>& cb) const
{
    if (!cb)
        return;
    switch (n.addr_.column()) {
        case Null:
            cb(ModelNodeBase(shared_from_this()));
            break;
        case UInt16:
            cb(SmallScalarNode<uint16_t>(shared_from_this(), n.addr_));
            break;
        case Int16:
            cb(SmallScalarNode<int16_t>(shared_from_this(), n.addr_));
            break;
        case Bool:
            cb(SmallScalarNode<bool>(shared_from_this(), n.addr_));
            break;
        case VirtualValue:
            cb(ValueNode(n));
            break;
        default:
            throw std::runtime_error(stx::format("Bad column reference: col={}", (uint16_t)n.addr_.column()));
    }
}

struct ModelPool::Impl
{
    explicit Impl(std::shared_ptr<Fields> fieldNames) :
        fieldNames_(std::move(fieldNames))
    {
        columns_.stringData_.reserve(detail::ColumnPageSize*4);
    }

    /// This model pool's field name store
    std::shared_ptr<Fields> fieldNames_;

    struct {
        sfl::segmented_vector<ModelNodeAddress, detail::ColumnPageSize> roots_;
        sfl::segmented_vector<ArrayIndex, detail::ColumnPageSize> objects_;
        sfl::segmented_vector<ArrayIndex, detail::ColumnPageSize> arrays_;
        sfl::segmented_vector<int64_t, detail::ColumnPageSize> i64_;
        sfl::segmented_vector<double, detail::ColumnPageSize> double_;

        std::string stringData_;
        sfl::segmented_vector<StringNode::Data, detail::ColumnPageSize> strings_;

        ArrayArena<Object::Field, detail::ColumnPageSize*2> objectMemberArrays_;
        ArrayArena<ModelNodeAddress, detail::ColumnPageSize*2> arrayMemberArrays_;

        sfl::segmented_vector<geo::Point<double>, detail::ColumnPageSize*2> vertices_;
        // sfl::segmented_vector<ArrayIndex, detail::ColumnPageSize> geom_;
        // ArrayArena<geo::Point<float>, detail::ColumnPageSize*2> vertexArrays_;
    } columns_;
};

ModelPool::ModelPool()
    : impl_(std::make_unique<ModelPool::Impl>(std::make_shared<Fields>()))
{}

ModelPool::ModelPool(std::shared_ptr<Fields> stringStore)
    : impl_(std::make_unique<ModelPool::Impl>(std::move(stringStore)))
{}

ModelPool::~ModelPool()  // NOLINT
{}

std::vector<std::string> ModelPool::checkForErrors() const
{
    std::vector<std::string> errors;

    auto validateModelIndex = [&](ModelNodeAddress const& node) {
        try {
            resolve(ModelNode(shared_from_this(), node), [](auto&&){});
        }
        catch (std::exception& e) {
            errors.emplace_back(e.what());
        }
    };

    auto validateFieldName = [&, this](FieldId const& str)
    {
        if (!impl_->fieldNames_)
            return;
        if (!impl_->fieldNames_->resolve(str))
            errors.push_back(stx::format("Bad string ID: {}", str));
    };

    // Check object members
    for (auto const& memberArray : impl_->columns_.objectMemberArrays_) {
        for (auto const& member : memberArray) {
            validateModelIndex(member.node_);
            validateFieldName(member.name_);
        }
    }

    // Check array members
    for (auto const& memberArray : impl_->columns_.arrayMemberArrays_) {
        for (auto const& member : memberArray) {
            validateModelIndex(member);
        }
    }

    return errors;
}

void ModelPool::validate() const
{
    auto errors = checkForErrors();
    if (!errors.empty()) {
        throw std::runtime_error(
            stx::format("Model Error(s): {}", stx::join(errors.begin(), errors.end(), ", ")));
    }
}

void ModelPool::clear()
{
    auto& columns = impl_->columns_;
    auto clear_and_shrink = [](auto&& what){
        what.clear();
        what.shrink_to_fit();
    };

    clear_and_shrink(columns.roots_);
    clear_and_shrink(columns.objects_);
    clear_and_shrink(columns.arrays_);
    clear_and_shrink(columns.i64_);
    clear_and_shrink(columns.double_);
    clear_and_shrink(columns.strings_);
    clear_and_shrink(columns.stringData_);
    clear_and_shrink(columns.objectMemberArrays_);
    clear_and_shrink(columns.arrayMemberArrays_);
    clear_and_shrink(columns.vertices_);
}

void ModelPool::resolve(
    ModelNode const& n,
    std::function<void(ModelNode&&)> const& cb
) const {
    auto get = [&n](auto const& vec) -> auto& {
        auto idx = n.addr_.index();
        if (idx >= vec.size())
            throw std::runtime_error(
                stx::format(
                    "Bad node reference: col={}, i={}",
                    (uint16_t)n.addr_.column(), idx
                ));
        return vec[idx];
    };

    switch (n.addr_.column()) {
    case Objects: {
        auto& val = get(impl_->columns_.objects_);
        cb(Object(val, shared_from_this(), n.addr_));
        break;
    }
    case Arrays: {
        auto& val = get(impl_->columns_.arrays_);
        cb(Array(val, shared_from_this(), n.addr_));
        break;
    }
    case Int64: {
        auto& val = get(impl_->columns_.i64_);
        cb(ScalarNode<int64_t>(val, shared_from_this(), n.addr_));
        break;
    }
    case Double: {
        auto& val = get(impl_->columns_.double_);
        cb(ScalarNode<double>(val, shared_from_this(), n.addr_));
        break;
    }
    case Vertex: {
        auto& val = get(impl_->columns_.vertices_);
        cb(VertexNode(val, shared_from_this(), n.addr_));
        break;
    }
    case String: {
        auto& val = get(impl_->columns_.strings_);
        cb(StringNode(
            std::string_view(impl_->columns_.stringData_).substr(val.offset_, val.size_),
            shared_from_this(),
            n.addr_));
        break;
    }
    default:
        ModelPoolBase::resolve(n, cb);
    }
}

size_t ModelPool::numRoots() const {
    return impl_->columns_.roots_.size();
}

ModelNode::Ptr ModelPool::root(size_t const& i) const {
    if (i < 0 || i > impl_->columns_.roots_.size())
        throw std::runtime_error("Root index does not exist.");
    return ModelNode(shared_from_this(), impl_->columns_.roots_[i]);
}

void ModelPool::addRoot(ModelNode::Ptr const& rootNode) {
    impl_->columns_.roots_.emplace_back(rootNode->addr_);
}

shared_model_ptr<Object> ModelPool::newObject(size_t initialFieldCapacity)
{
    auto memberArrId = impl_->columns_.objectMemberArrays_.new_array(initialFieldCapacity);
    impl_->columns_.objects_.emplace_back(memberArrId);
    return Object(
        memberArrId,
        shared_from_this(),
        {Objects, (uint32_t)impl_->columns_.objects_.size()-1});
}

shared_model_ptr<Array> ModelPool::newArray(size_t initialFieldCapacity)
{
    auto memberArrId = impl_->columns_.arrayMemberArrays_.new_array(initialFieldCapacity);
    impl_->columns_.arrays_.emplace_back(memberArrId);
    return Array(
        memberArrId,
        shared_from_this(),
        {Arrays, (uint32_t)impl_->columns_.arrays_.size()-1});
}

ModelNode::Ptr ModelPoolBase::newSmallValue(bool value)
{
    return ModelNode(shared_from_this(), {Bool, (uint32_t)value});
}

ModelNode::Ptr ModelPoolBase::newSmallValue(int16_t value)
{
    return ModelNode(shared_from_this(), {Int16, (uint32_t)value});
}

ModelNode::Ptr ModelPoolBase::newSmallValue(uint16_t value)
{
    return ModelNode(shared_from_this(), {UInt16, (uint32_t)value});
}

ModelNode::Ptr ModelPool::newValue(int64_t const& value)
{
    if (value < 0 && value >= std::numeric_limits<int16_t>::min())
        return newSmallValue((int16_t)value);
    else if (value >= 0 && value <= std::numeric_limits<uint16_t>::max())
        return newSmallValue((uint16_t)value);
    impl_->columns_.i64_.emplace_back(value);
    return ModelNode(shared_from_this(), {Int64, (uint32_t)impl_->columns_.i64_.size()-1});
}

ModelNode::Ptr ModelPool::newValue(double const& value)
{
    impl_->columns_.double_.emplace_back(value);
    return ModelNode(shared_from_this(), {Double, (uint32_t)impl_->columns_.double_.size()-1});
}

ModelNode::Ptr ModelPool::newValue(std::string_view const& value)
{
    impl_->columns_.strings_.emplace_back(StringNode::Data{
        impl_->columns_.stringData_.size(),
        value.size()
    });
    impl_->columns_.stringData_ += value;
    return ModelNode(shared_from_this(), {String, (uint32_t)impl_->columns_.strings_.size()-1});
}

ModelNode::Ptr ModelPool::newVertex(double const& x, double const& y, double const& z) {
    impl_->columns_.vertices_.emplace_back(geo::Point<double>{x, y, z});
    return ModelNode(shared_from_this(), {Vertex, (uint32_t)impl_->columns_.vertices_.size()-1});
}

template<>
shared_model_ptr<Object> ModelPool::resolve(ModelNode::Ptr const& n) const {
    if (n->addr_.column() != Objects)
        throw std::runtime_error("Cannot cast this node to an object.");
    auto& memberArrayIdx = impl_->columns_.objects_[n->addr_.index()];
    return Object(memberArrayIdx, shared_from_this(), n->addr_);
}

template<>
shared_model_ptr<Array> ModelPool::resolve(ModelNode::Ptr const& n) const
{
    if (n->addr_.column() != Arrays)
        throw std::runtime_error("Cannot cast this node to an array.");
    auto& memberArrayIdx = impl_->columns_.arrays_[n->addr_.index()];
    return Array(memberArrayIdx, shared_from_this(), n->addr_);
}

std::shared_ptr<Fields> ModelPool::fieldNames() const
{
    return impl_->fieldNames_;
}

Object::Storage& ModelPool::objectMemberStorage() {
    return impl_->columns_.objectMemberArrays_;
}

Array::Storage& ModelPool::arrayMemberStorage() {
    return impl_->columns_.arrayMemberArrays_;
}

}  // namespace simfil