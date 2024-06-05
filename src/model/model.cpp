#include "simfil/model/model.h"
#include "simfil/model/arena.h"
#include "simfil/value.h"
#include "simfil/overlay.h"
#include "simfil/model/bitsery-traits.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <bitsery/bitsery.h>
#include <bitsery/adapter/stream.h>
#include <bitsery/traits/string.h>
#include <sfl/segmented_vector.hpp>

namespace simfil
{

void Model::resolve(const ModelNode& n, const ResolveFn& cb) const
{
    switch (n.addr_.column()) {
        case Null:
            cb(ModelNodeBase(shared_from_this()));
            break;
        case UInt16:
            cb(SmallValueNode<uint16_t>(shared_from_this(), n.addr_));
            break;
        case Int16:
            cb(SmallValueNode<int16_t>(shared_from_this(), n.addr_));
            break;
        case Bool:
            cb(SmallValueNode<bool>(shared_from_this(), n.addr_));
            break;
        case Scalar:
            cb(ValueNode(n));
            break;
        default:
            raise<std::runtime_error>(fmt::format("Bad column reference: col={}", (uint16_t)n.addr_.column()));
    }
}

struct ModelPool::Impl
{
    explicit Impl(std::shared_ptr<Fields> fieldNames) :
        fieldNames_(std::move(fieldNames))
    {
        columns_.stringData_.reserve(detail::ColumnPageSize*4);
    }

    struct StringRange {
        uint32_t offset_;
        uint32_t length_;

        template<typename S>
        void serialize(S& s) {
            s.value4b(offset_);
            s.value4b(length_);
        }
    };

    /// This model pool's field name store
    std::shared_ptr<Fields> fieldNames_;

    struct {
        sfl::segmented_vector<ModelNodeAddress, detail::ColumnPageSize> roots_;
        sfl::segmented_vector<int64_t, detail::ColumnPageSize> i64_;
        sfl::segmented_vector<double, detail::ColumnPageSize> double_;

        std::string stringData_;
        sfl::segmented_vector<StringRange, detail::ColumnPageSize> strings_;

        Object::Storage objectMemberArrays_;
        Array::Storage arrayMemberArrays_;
    } columns_;

    template<typename S>
    void readWrite(S& s) {
        constexpr size_t maxColumnSize = std::numeric_limits<uint32_t>::max();

        s.container(columns_.roots_, maxColumnSize);

        s.container(columns_.i64_, maxColumnSize);
        s.container(columns_.double_, maxColumnSize);
        s.text1b(columns_.stringData_, maxColumnSize);
        s.container(columns_.strings_, maxColumnSize);

        s.ext(columns_.objectMemberArrays_, bitsery::ext::ArrayArenaExt{});
        s.ext(columns_.arrayMemberArrays_, bitsery::ext::ArrayArenaExt{});
    }
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

    auto validateArrayIndex = [&](auto i, auto arrType, auto const& arena) {
        if ((i < 0) || (i >= arena.size())) {
            errors.emplace_back(fmt::format("Bad {} array index {}.", arrType, i));
            return false;
        }
        return true;
    };

    auto validateFieldName = [&, this](FieldId const& str)
    {
        if (!impl_->fieldNames_)
            return;
        if (!impl_->fieldNames_->resolve(str))
            errors.push_back(fmt::format("Bad string ID: {}", str));
    };

    std::function<void(ModelNode::Ptr)> validateModelNode = [&](ModelNode::Ptr node)
    {
        try {
            if (node->type() == ValueType::Object) {
                if (node->addr().column() == Objects)
                    if (!validateArrayIndex(node->addr().index(), "object", impl_->columns_.objectMemberArrays_))
                        return;
                for (auto const& [fieldName, fieldValue] : node->fields()) {
                    validateFieldName(fieldName);
                    validateModelNode(fieldValue);
                }
            }
            else if (node->type() == ValueType::Array) {
                if (node->addr().column() == Arrays)
                    if (!validateArrayIndex(node->addr().index(), "arrays", impl_->columns_.arrayMemberArrays_))
                        return;
                for (auto const& member : *node)
                    validateModelNode(member);
            }
            resolve(*node, Lambda([](auto&&) {}));
        }
        catch (std::exception& e) {
            errors.emplace_back(e.what());
        }
    };

    // Validate objects
    for (auto i = 0; i < impl_->columns_.objectMemberArrays_.size(); ++i)
        validateModelNode(ModelNode::Ptr::make(shared_from_this(), ModelNodeAddress{Objects, (uint32_t)i}));

    // Validate arrays
    for (auto i = 0; i < impl_->columns_.arrayMemberArrays_.size(); ++i)
        validateModelNode(ModelNode::Ptr::make(shared_from_this(), ModelNodeAddress{Arrays, (uint32_t)i}));

    // Validate roots
    for (auto i = 0; i < numRoots(); ++i)
        validateModelNode(root(i));

    return errors;
}

void ModelPool::validate() const
{
    auto errors = checkForErrors();
    if (!errors.empty()) {
        raise<std::runtime_error>(
            fmt::format("Model Error(s): {}", fmt::join(errors, ", ")));
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
    clear_and_shrink(columns.i64_);
    clear_and_shrink(columns.double_);
    clear_and_shrink(columns.strings_);
    clear_and_shrink(columns.stringData_);
    clear_and_shrink(columns.objectMemberArrays_);
    clear_and_shrink(columns.arrayMemberArrays_);
}

void ModelPool::resolve(ModelNode const& n, ResolveFn const& cb) const
{
    auto get = [&n](auto const& vec) -> auto& {
        auto idx = n.addr_.index();
        if (idx >= vec.size())
            raise<std::runtime_error>(
                fmt::format(
                    "Bad node reference: col={}, i={}",
                    (uint16_t)n.addr_.column(), idx
                ));
        return vec[idx];
    };

    switch (n.addr_.column()) {
    case Objects: {
        cb(Object(shared_from_this(), n.addr_));
        break;
    }
    case Arrays: {
        cb(Array(shared_from_this(), n.addr_));
        break;
    }
    case Int64: {
        auto& val = get(impl_->columns_.i64_);
        cb(ValueNode(val, shared_from_this()));
        break;
    }
    case Double: {
        auto& val = get(impl_->columns_.double_);
        cb(ValueNode(val, shared_from_this()));
        break;
    }
    case String: {
        auto& val = get(impl_->columns_.strings_);
        cb(ValueNode(
            // TODO: Make sure that the string view is not turned into a string here.
            std::string_view(impl_->columns_.stringData_).substr(val.offset_, val.length_),
            shared_from_this()));
        break;
    }
    default: Model::resolve(n, cb);
    }
}

size_t ModelPool::numRoots() const {
    return impl_->columns_.roots_.size();
}

ModelNode::Ptr ModelPool::root(size_t const& i) const {
    if ((i < 0) || (i >= impl_->columns_.roots_.size()))
        raise<std::runtime_error>("Root index does not exist.");
    return ModelNode(shared_from_this(), impl_->columns_.roots_.at(i));
}

void ModelPool::addRoot(ModelNode::Ptr const& rootNode) {
    impl_->columns_.roots_.emplace_back(rootNode->addr_);
}

shared_model_ptr<Object> ModelPool::newObject(size_t initialFieldCapacity)
{
    auto memberArrId = impl_->columns_.objectMemberArrays_.new_array(initialFieldCapacity);
    return Object(
        shared_from_this(),
        {Objects, (uint32_t)memberArrId});
}

shared_model_ptr<Array> ModelPool::newArray(size_t initialFieldCapacity)
{
    auto memberArrId = impl_->columns_.arrayMemberArrays_.new_array(initialFieldCapacity);
    return Array(
        shared_from_this(),
        {Arrays, (uint32_t)memberArrId});
}

ModelNode::Ptr Model::newSmallValue(bool value)
{
    return ModelNode(shared_from_this(), {Bool, (uint32_t)value});
}

ModelNode::Ptr Model::newSmallValue(int16_t value)
{
    return ModelNode(shared_from_this(), {Int16, (uint32_t)value});
}

ModelNode::Ptr Model::newSmallValue(uint16_t value)
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
    impl_->columns_.strings_.emplace_back(Impl::StringRange{
        (uint32_t)impl_->columns_.stringData_.size(),
        (uint32_t)value.size()
    });
    impl_->columns_.stringData_ += value;
    return ModelNode(shared_from_this(), {String, (uint32_t)impl_->columns_.strings_.size()-1});
}

shared_model_ptr<Object> ModelPool::resolveObject(const ModelNode::Ptr& n) const {
    if (n->addr_.column() != Objects)
        raise<std::runtime_error>("Cannot cast this node to an object.");
    return Object(shared_from_this(), n->addr_);
}

shared_model_ptr<Array> ModelPool::resolveArray(ModelNode::Ptr const& n) const
{
    if (n->addr_.column() != Arrays)
        raise<std::runtime_error>("Cannot cast this node to an array.");
    return Array(shared_from_this(), n->addr_);
}

std::shared_ptr<Fields> ModelPool::fieldNames() const
{
    return impl_->fieldNames_;
}

void ModelPool::setFieldNames(std::shared_ptr<Fields> const& newDict)
{
    // Translate object field IDs to the new dictionary.
    for (auto memberArray : impl_->columns_.objectMemberArrays_) {
        for (auto& member : memberArray) {
            if (auto resolvedName = impl_->fieldNames_->resolve(member.name_))
                member.name_ = newDict->emplace(*resolvedName);
        }
    }
    impl_->fieldNames_ = newDict;
}

Object::Storage& ModelPool::objectMemberStorage() {
    return impl_->columns_.objectMemberArrays_;
}

Array::Storage& ModelPool::arrayMemberStorage() {
    return impl_->columns_.arrayMemberArrays_;
}

void ModelPool::write(std::ostream& outputStream) {
    bitsery::Serializer<bitsery::OutputStreamAdapter> s(outputStream);
    impl_->readWrite(s);
}

void ModelPool::read(std::istream& inputStream) {
    bitsery::Deserializer<bitsery::InputStreamAdapter> s(inputStream);
    impl_->readWrite(s);
    if (s.adapter().error() != bitsery::ReaderError::NoError) {
        raise<std::runtime_error>(fmt::format(
            "Failed to read ModelPool: Error {}",
            static_cast<std::underlying_type_t<bitsery::ReaderError>>(s.adapter().error())));
    }
}

}  // namespace simfil
