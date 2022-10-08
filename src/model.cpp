#include "simfil/model.h"

#include "simfil/value.h"

#include <algorithm>
#include <memory>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <atomic>
#include <sstream>
#include <map>
#include "stx/format.h"

namespace simfil
{

/** Scalar Node implementation */

ScalarNode::ScalarNode()
    : scalar(Value::null())
{}

ScalarNode::ScalarNode(Value&& v)
    : scalar(std::move(v))
{}

auto ScalarNode::value() const -> Value
{
    return scalar;
}

auto ScalarNode::type() const -> Type
{
    return Type::Scalar;
}

auto ScalarNode::get(const std::string_view &) const -> const ModelNode*
{
    return nullptr;
}

auto ScalarNode::get(int64_t) const -> const ModelNode*
{
    return nullptr;
}

auto ScalarNode::children() const -> std::vector<const ModelNode*>
{
    return {};
}

auto ScalarNode::keys() const -> std::vector<std::string_view>
{
    return {};
}

auto ScalarNode::size() const -> int64_t
{
    return 0;
}

/** Object Node implementation */

auto ObjectNode::value() const -> Value
{
    return Value::null();
}

auto ObjectNode::type() const -> Type
{
    return ModelNode::Type::Object;
}

auto ObjectNode::get(const std::string_view & key) const -> const ModelNode*
{
    if (!storage_)
        return {};
    for (int i = 0; i < size_; ++i)
        if(storage_->at(firstMemberIndex_ + i).first == key)
            return storage_->at(firstMemberIndex_ + i).second;
    return nullptr;
}

auto ObjectNode::get(int64_t) const -> const ModelNode*
{
    return nullptr;
}

auto ObjectNode::children() const -> std::vector<const ModelNode*>
{
    if (!storage_)
        return {};
    std::vector<const ModelNode*> nodes;
    nodes.reserve(size_);
    for (int i = 0; i < size_; ++i)
        nodes.push_back(storage_->at(firstMemberIndex_ + i).second);
    return nodes;
}

auto ObjectNode::keys() const -> std::vector<std::string_view>
{
    if (!storage_)
        return {};
    std::vector<std::string_view> names;
    names.reserve(size_);
    for (int i = 0; i < size_; ++i)
        names.push_back(storage_->at(firstMemberIndex_ + i).first);
    return names;
}

auto ObjectNode::size() const -> int64_t
{
    return (int64_t)size_;
}

/** Array Node implementation */

auto ArrayNode::value() const -> Value
{
    return Value::null();
}

auto ArrayNode::type() const -> Type
{
    return ModelNode::Type::Array;
}

auto ArrayNode::get(const std::string_view &) const -> const ModelNode*
{
    return nullptr;
}

auto ArrayNode::get(int64_t i) const -> const ModelNode*
{
    if (!storage_)
        return {};
    if (0 <= i && i <= size_)
        return storage_->at(firstMemberIndex_ + i);
    return nullptr;
}

auto ArrayNode::children() const -> std::vector<const ModelNode*>
{
    if (!storage_)
        return {};
    std::vector<const ModelNode*> result;
    result.reserve(size_);
    for (auto i = 0; i < size_; ++i)
        result.push_back(storage_->at(firstMemberIndex_ + i));
    return result;
}

auto ArrayNode::keys() const -> std::vector<std::string_view>
{
    return {};
}

auto ArrayNode::size() const -> int64_t
{
    return (int64_t)size_;
}

/** Vertex Node implementation */

VertexNode::VertexNode(double lon, double lat) : lon(Value::make(lon)), lat(Value::make(lat)) {}

auto VertexNode::type() const -> ModelNode::Type {return ModelNode::Type::Array; }

auto VertexNode::value() const -> Value { return Value::null(); }

auto VertexNode::get(const std::string_view & key) const -> const ModelNode*
{
    if (key == "lon")
        return &lon;
    if (key == "lat")
        return &lat;
    return nullptr;
}

auto VertexNode::get(int64_t idx) const -> const ModelNode*
{
    switch (idx) {
    case 0: return &lon;
    case 1: return &lat;
    default: return nullptr;
    }
}

auto VertexNode::children() const -> std::vector<const ModelNode*> { return {{&lon, &lat}}; }

auto VertexNode::keys() const -> std::vector<std::string_view> { return {{"lon"s, "lat"s}}; }

auto VertexNode::size() const -> int64_t { return 2; }

/** Feature-ID Node implementation */

FeatureIdNode::FeatureIdNode(
    std::string_view const& prefix,
    std::vector<std::pair<char const*, int64_t>> idPathElements
) : prefix_(prefix), idPathElements_(std::move(idPathElements))
{}

auto FeatureIdNode::type() const -> ModelNode::Type {return ModelNode::Type::Scalar; }

auto FeatureIdNode::value() const -> Value {
    std::stringstream result;
    result << prefix_;
    for (auto& [type, id] : idPathElements_) {
        result <<  "." << type << "." << std::to_string(id);
    }
    return Value::make(result.str());
}

auto FeatureIdNode::get(const std::string_view & key) const -> const ModelNode*
{
    return nullptr;
}

auto FeatureIdNode::get(int64_t idx) const -> const ModelNode*
{
    return nullptr;
}

auto FeatureIdNode::children() const -> std::vector<const ModelNode*> { return {}; }

auto FeatureIdNode::keys() const -> std::vector<std::string_view> { return {}; }

auto FeatureIdNode::size() const -> int64_t { return 2; }

/** String Pool implementation */

std::string_view ModelPool::Strings::getOrInsert(std::string const& str)
{
    {
        std::shared_lock stringStoreReadAccess_(stringStoreMutex_);
        auto it = strings_.find(str);
        if (it != strings_.end()) {
            ++cacheHits_;
            return *it;
        }
    }
    {
        std::unique_lock stringStoreWriteAccess_(stringStoreMutex_);
        auto [it, insertionTookPlace] = strings_.emplace(str);
        if (insertionTookPlace) {
            ++cacheMisses_;
            byteSize_ += (int64_t)str.size();
        }
        return *it;
    }
}

/// Get stats
size_t ModelPool::Strings::size() {
    std::shared_lock stringStoreReadAccess_(stringStoreMutex_);
    return strings_.size();
}
size_t ModelPool::Strings::bytes() {
    return byteSize_;
}
size_t ModelPool::Strings::hits() {
    return cacheHits_;
}
size_t ModelPool::Strings::misses() {
    return cacheMisses_;
}

/** Model Pool implementation */

ModelPool::ModelPool() : strings(std::make_shared<Strings>()) {}

ModelPool::ModelPool(std::shared_ptr<Strings> stringStore) : strings(std::move(stringStore)) {};

std::vector<std::string> ModelPool::checkForErrors() const {
    std::vector<std::string> errors;

    // Gather nodes
    std::unordered_set<ModelNode const*> modelNodes;
    for (auto const& o : objects)
        modelNodes.insert(&o);
    for (auto const& a : arrays)
        modelNodes.insert(&a);
    for (auto const& s : scalars)
        modelNodes.insert(&s);
    for (auto const& v : vertices)
        modelNodes.insert(&v);
    for (auto const& f : featureIds)
        modelNodes.insert(&f);
    auto validateModelNode = [&](ModelNode const* const& node){
        if (modelNodes.find(node) == modelNodes.end())
            errors.push_back(stx::format("Bad node reference: 0x{0:0x}", (uint64_t)node));
    };
    auto validateRange = [&](auto const& first, auto const& size, auto const& container) {
        if (size <= 0)
            return;
        if (first >= container.size() || first+size > container.size())
            errors.push_back(stx::format("Bad array/object member range: [{}-{})", first, first+size));
    };

    // Gather strings
    std::map<char const*, char const*> stringRanges;
    if (strings) {
        std::unique_lock stringStoreReadAccess_(strings->stringStoreMutex_);
        for (auto const& str : strings->strings_) {
            stringRanges[str.data()] = str.data() + str.size();
        }
    }
    auto validateString = [&, this](std::string_view const& str){
        auto it = stringRanges.upper_bound(str.data());
        if (it == stringRanges.begin() || str.data()+str.size() > (--it)->second)
            errors.push_back(stx::format("Bad string view: 0x{0:0x}", (uint64_t)str.data()));
    };

    // Check object members
    for (auto const& [s, n] : objectMembers) {
        validateModelNode(n);
        validateString(s);
    }
    for (auto const& o : objects) {
        validateRange(o.firstMemberIndex_, o.size_, objectMembers);
    }

    // Check array members
    for (auto const& n : arrayMembers) {
        validateModelNode(n);
    }
    for (auto const& a : arrays) {
        validateRange(a.firstMemberIndex_, a.size_, arrayMembers);
    }

    // Check values
    for (auto const& scalar : scalars) {
        auto const* v = scalar.value().stringViewValue();
        if (v)
            validateString(*v);
    }

    return errors;
}

void ModelPool::validate() const {
    auto errors = checkForErrors();
    if (!errors.empty()) {
        throw std::runtime_error(stx::format("Model Error(s): {}", stx::join(errors.begin(), errors.end(), ", ")));
    }
}

void ModelPool::clear()
{
    roots.clear();
    roots.shrink_to_fit();
    objects.clear();
    objects.shrink_to_fit();
    arrays.clear();
    arrays.shrink_to_fit();
    scalars.clear();
    scalars.shrink_to_fit();
    vertices.clear();
    vertices.shrink_to_fit();
    featureIds.clear();
    featureIds.shrink_to_fit();
    objectMembers.clear();
    objectMembers.shrink_to_fit();
    arrayMembers.clear();
    arrayMembers.shrink_to_fit();
}

}

