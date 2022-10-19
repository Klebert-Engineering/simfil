#include "simfil/model.h"

#include "simfil/value.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "stx/format.h"

namespace simfil
{

/** String Pool implementation */

Strings::Strings() {
    addStaticKey(Empty, "");
    addStaticKey(Lon, "lon");
    addStaticKey(Lat, "lat");
    addStaticKey(OverlaySum, "$sum");
    addStaticKey(OverlayValue, "$val");
    addStaticKey(OverlayIndex, "$idx");
    addStaticKey(Geometry, "geometry");
    addStaticKey(Type, "type");
    addStaticKey(Coordinates, "coordinates");
}

StringId Strings::emplace(std::string const& str)
{
    auto lowerCaseStr = str;
    std::transform(
        lowerCaseStr.begin(),
        lowerCaseStr.end(),
        lowerCaseStr.begin(),
        [](auto ch) { return std::tolower(ch); });

    {
        std::shared_lock stringStoreReadAccess_(stringStoreMutex_);
        auto it = idForString_.find(lowerCaseStr);
        if (it != idForString_.end()) {
            ++cacheHits_;
            return it->second;
        }
    }
    {
        std::unique_lock stringStoreWriteAccess_(stringStoreMutex_);
        auto [it, insertionTookPlace] = idForString_.emplace(str, nextId_);
        if (insertionTookPlace) {
            ++cacheMisses_;
            byteSize_ += (int64_t)str.size();
            stringForId_[nextId_] = str;
            ++nextId_;
        }
        return it->second;
    }
}

StringId Strings::get(std::string const& str)
{
    auto lowerCaseStr = str;
    std::transform(
        lowerCaseStr.begin(),
        lowerCaseStr.end(),
        lowerCaseStr.begin(),
        [](auto ch) { return std::tolower(ch); });

    std::shared_lock stringStoreReadAccess_(stringStoreMutex_);
    auto it = idForString_.find(str);
    if (it != idForString_.end())
        return it->second;

    return Strings::Empty;
}

std::optional<std::string_view> Strings::resolve(StringId const& id)
{
    std::shared_lock stringStoreReadAccess_(stringStoreMutex_);
    auto it = stringForId_.find(id);
    if (it != stringForId_.end())
        return it->second;

    return std::nullopt;
}

size_t Strings::size()
{
    std::shared_lock stringStoreReadAccess_(stringStoreMutex_);
    return idForString_.size();
}

size_t Strings::bytes()
{
    return byteSize_;
}

size_t Strings::hits()
{
    return cacheHits_;
}

size_t Strings::misses()
{
    return cacheMisses_;
}

void Strings::addStaticKey(StringId k, std::string const& v) {
    idForString_[v] = k;
    stringForId_[k] = v;
}

/** Model Pool implementation */

ModelPool::ModelPool() : strings(std::make_shared<Strings>()) {}

ModelPool::ModelPool(std::shared_ptr<Strings> stringStore) : strings(std::move(stringStore)){};

std::vector<std::string> ModelPool::checkForErrors() const
{
    std::vector<std::string> errors;

    auto validateModelIndex = [&](ModelNodeIndex const& node)
    {
        try {
            resolve(node);
        }
        catch (std::exception& e) {
            errors.emplace_back(e.what());
        }
    };

    auto validateMemberRange = [&](MemberRange const& range)
    {
        if (range.second <= 0)
            return;
        if (range.first >= columns_.members_.size() ||
            range.second + range.first > columns_.members_.size())
            errors.push_back(stx::format(
                "Bad array/object member range: [{}-{})",
                range.first,
                range.second + range.first));
    };

    auto validateString = [&, this](StringId const& str)
    {
        if (!strings)
            return;
        if (!strings->resolve(str))
            errors.push_back(stx::format("Bad string ID: {}", str));
    };

    // Check object members
    for (auto const& member : columns_.members_) {
        validateModelIndex(member.nodeIndex_);
        validateString(member.name_);
    }
    for (auto const& memberRange : columns_.object_) {
        validateMemberRange(memberRange);
    }

    // Check array members
    for (auto const& memberRange : columns_.array_) {
        validateMemberRange(memberRange);
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
    columns_.root_.clear();
    columns_.root_.shrink_to_fit();
    columns_.object_.clear();
    columns_.object_.shrink_to_fit();
    columns_.array_.clear();
    columns_.array_.shrink_to_fit();
    columns_.i64_.shrink_to_fit();
    columns_.vertex_.clear();
    columns_.vertex_.shrink_to_fit();
    columns_.members_.clear();
    columns_.members_.shrink_to_fit();
}

ModelNodePtr ModelPool::resolve(ModelNodeIndex const& i) const {
    auto get = [&i](auto const& vec) -> auto& {
        auto idx = i.index();
        if (idx >= vec.size())
            throw std::runtime_error(stx::format("Bad node reference: col={}, i={}", (uint16_t)i.column(), idx));
        return vec[idx];
    };

    switch (i.column()) {
    case Objects: {
        auto& memberRange = get(columns_.object_);
        return std::make_shared<ObjectModelNode>(memberRange, *this);
    }
    case Arrays: {
        auto& memberRange = get(columns_.array_);
        return std::make_shared<ArrayModelNode>(memberRange, *this);
    }
    case Vertices: {
        auto& vert = get(columns_.vertex_);
        return std::make_shared<VertexModelNode>(vert, *this);
    }
    case UInt16: {
        return std::make_shared<ScalarModelNode>((int64_t)i.uint16());
    }
    case Int16: {
        return std::make_shared<ScalarModelNode>((int64_t)i.int16());
    }
    case Int64: {
        auto& val = get(columns_.i64_);
        return std::make_shared<ScalarModelNode>(val);
    }
    case Double: {
        auto& val = get(columns_.double_);
        return std::make_shared<ScalarModelNode>(val);
    }
    case String: {
        return std::make_shared<StringModelNode>(i.uint16(), strings);
    }
    case Null: {
        return std::make_shared<ModelNodeBase>();
    }
    default:
        break;
    }

    throw std::runtime_error(stx::format("Bad column reference: col={}", (uint16_t)i.column()));
}

void ModelPool::visitMembers(MemberRange const& range, std::function<bool(Member const&)> const& f) const {
    if (!f)
        return;
    for (auto i = 0; i < range.second; ++i) {
        auto idx = range.first + i;
        if (idx >= columns_.members_.size())
            break;
        if (!f(columns_.members_[range.first + i]))
            break;
    }
}

size_t ModelPool::numRoots() const {
    return columns_.root_.size();
}

ModelNodePtr ModelPool::root(size_t const& i) const {
    if (i < 0 || i > columns_.root_.size())
        throw std::runtime_error("root index does not exist.");
    return resolve(columns_.root_[i]);
}

void ModelPool::addRoot(ModelNodeIndex const& rootIndex) {
    columns_.root_.emplace_back(rootIndex);
}

ModelPool::ModelNodeIndex ModelPool::addObject(std::vector<Member> const& members) {
    columns_.members_.reserve(columns_.members_.size() + members.size());
    auto memberOffset = columns_.members_.size();
    for (auto const& m : members)
        columns_.members_.emplace_back(m);
    auto idx = columns_.object_.size();
    columns_.object_.emplace_back(MemberRange{memberOffset, members.size()});
    return {Objects, idx};
}

ModelPool::ModelNodeIndex ModelPool::addArray(std::vector<Member> const& members) {
    columns_.members_.reserve(columns_.members_.size() + members.size());
    auto memberOffset = columns_.members_.size();
    for (auto const& m : members)
        columns_.members_.emplace_back(m);
    auto idx = columns_.array_.size();
    columns_.array_.emplace_back(MemberRange{memberOffset, members.size()});
    return {Arrays, idx};
}

ModelPool::ModelNodeIndex ModelPool::addNull() {
    return {Null, 0};
}

ModelPool::ModelNodeIndex ModelPool::addValue(bool const& value) {
    return {Bool, value};
}

ModelPool::ModelNodeIndex ModelPool::addValue(int64_t const& value) {
    if (value < std::numeric_limits<uint16_t>::max() && value >= 0)
        return {UInt16, (uint16_t)value};
    if (value > std::numeric_limits<uint16_t>::min() && value < 0)  // NOLINT
        return {Int16, (uint16_t)value};
    auto idx = columns_.i64_.size();
    columns_.i64_.emplace_back(value);
    return {Int64, idx};
}

ModelPool::ModelNodeIndex ModelPool::addValue(double const& value) {
    auto idx = columns_.double_.size();
    columns_.double_.emplace_back(value);
    return {Double, idx};
}

ModelPool::ModelNodeIndex ModelPool::addValue(std::string const& value) {
    auto strId = strings->emplace(value);
    return {String, strId};
}

ModelPool::ModelNodeIndex ModelPool::addVertex(double const& lon, double const& lat) {
    auto idx = columns_.vertex_.size();
    columns_.vertex_.emplace_back(lon, lat);
    return {Vertices, idx};
}

/** Model Node impls for a scalar value. */

ScalarModelNode::ScalarModelNode(int64_t const& i) : value_(Value::make(i)) {}

ScalarModelNode::ScalarModelNode(double const& d) : value_(Value::make(d)) {}

ScalarModelNode::ScalarModelNode(bool const& b) : value_(Value::make(b)) {}

Value ScalarModelNode::value() const
{
    return value_;
}

/** Model Node impls for a string value. */

StringModelNode::StringModelNode(StringId strId, std::shared_ptr<Strings> stringPool)
    : strId_(strId), stringPool_(std::move(stringPool))
{
}

Value StringModelNode::value() const
{
    if (auto s = stringPool_->resolve(strId_))
        return Value::make(*s);
    throw std::runtime_error("Failed to resolve string id.");
}

/** Model Node impls for an array. */

ArrayModelNode::ArrayModelNode(ModelPool::MemberRange members, ModelPool const& modelPool)
    : members_(std::move(members)), modelPool_(modelPool)
{
}

ModelNode::Type ArrayModelNode::type() const
{
    return ModelNode::Array;
}

ModelNodePtr ArrayModelNode::at(int64_t i) const
{
    ModelNodePtr result;
    if (i >= 0 && i < members_.second)
    {
        modelPool_.visitMembers(
            {members_.first + i, 1},
            [&](ModelPool::Member const& m)
            {
                result = modelPool_.resolve(m.nodeIndex_);
                return true;
            });
    }
    return result;
}

std::vector<ModelNodePtr> ArrayModelNode::children() const
{
    std::vector<ModelNodePtr> result;
    result.reserve(members_.second);
    modelPool_.visitMembers(
        members_,
        [&](ModelPool::Member const& m)
        {
            result.emplace_back(modelPool_.resolve(m.nodeIndex_)); return true;
        });
    return result;
}

uint32_t ArrayModelNode::size() const
{
    return members_.second;
}


/** Model Node impls for an object. */

ObjectModelNode::ObjectModelNode(ModelPool::MemberRange members, ModelPool const& modelPool)
    : ArrayModelNode(std::move(members), modelPool)
{
}

ModelNode::Type ObjectModelNode::type() const
{
    return ModelNode::Object;
}

ModelNodePtr ObjectModelNode::get(const StringId & field) const
{
    ModelNodePtr result;
    modelPool_.visitMembers(
        members_,
        [&](ModelPool::Member const& m)
        {
            if (m.name_ == field) {
                result = modelPool_.resolve(m.nodeIndex_);
                return false;
            }
            return true;
        });
    return result;
}

std::vector<std::string> ObjectModelNode::keys() const
{
    std::vector<std::string> result;
    result.reserve(members_.second);
    modelPool_.visitMembers(
        members_,
        [&](ModelPool::Member const& m)
        {
            if (auto s = modelPool_.strings->resolve(m.name_))
                result.emplace_back(*s);
            return true;
        });
    return result;
}


/** Model Node impls for an object with extra procedural fields. */

ProceduralObjectModelNode::ProceduralObjectModelNode(
    ModelPool::MemberRange members,
    ModelPool const& modelPool)
    : ObjectModelNode(std::move(members), modelPool)
{
}

ModelNodePtr ProceduralObjectModelNode::get(const StringId & key) const {
    for (auto const& [k, vf] : fields_) {
        if (key == k)
            return vf();
    }
    return ObjectModelNode::get(key);
}

ModelNodePtr ProceduralObjectModelNode::at(int64_t i) const {
    if (i < fields_.size() && i > 0) {
        return fields_[i].second();
    }
    return ObjectModelNode::at(i - (int64_t)fields_.size());
}

std::vector<ModelNodePtr> ProceduralObjectModelNode::children() const {
    auto result = ObjectModelNode::children();
    for (auto const& [_, vf] : fields_) {
        result.emplace_back(vf());
    }
    return result;
}

std::vector<std::string> ProceduralObjectModelNode::keys() const {
    auto result = ObjectModelNode::keys();
    for (auto const& [k, _]: fields_) {
        if (auto s = modelPool_.strings->resolve(k))
            result.emplace_back();
        else
            throw std::runtime_error("Could not resolve string key.");
    }
    return result;
}

uint32_t ProceduralObjectModelNode::size() const {
    return ObjectModelNode::size() + fields_.size();
}

/** Model Node impls for a vertex. */

VertexModelNode::VertexModelNode(std::pair<double, double> const& coords, ModelPool const& modelPool)
    : ProceduralObjectModelNode({}, modelPool), coords_(coords)
{
    fields_.emplace_back(
        Strings::Lon,
        [this]() { return std::make_shared<ScalarModelNode>(coords_.first); });

    fields_.emplace_back(
        Strings::Lat,
        [this]() { return std::make_shared<ScalarModelNode>(coords_.second); });
}

ModelNode::Type VertexModelNode::type() const
{
    return ModelNode::Array;
}

}  // namespace simfil
