#include "simfil/model.h"

#include "simfil/value.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "stx/format.h"

#include <sfl/segmented_vector.hpp>

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
    addStaticKey(Elevation, "elevation");
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
        auto [it, insertionTookPlace] = idForString_.emplace(lowerCaseStr, nextId_);
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
    auto it = idForString_.find(lowerCaseStr);
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
    auto lowerCaseStr = v;
    std::transform(
        lowerCaseStr.begin(),
        lowerCaseStr.end(),
        lowerCaseStr.begin(),
        [](auto ch) { return std::tolower(ch); });

    idForString_[lowerCaseStr] = k;
    stringForId_[k] = v;
}

/** Model Pool implementation */
struct ModelPool::Impl
{
    struct {
        sfl::segmented_vector<ModelNodeIndex, BigChunkSize> root_;
        sfl::segmented_vector<MemberRange, BigChunkSize> object_;
        sfl::segmented_vector<MemberRange, BigChunkSize> array_;
        sfl::segmented_vector<int64_t, BigChunkSize> i64_;
        sfl::segmented_vector<double, BigChunkSize> double_;
        sfl::segmented_vector<Member, BigChunkSize*2> members_;
        sfl::segmented_vector<std::pair<double, double>, BigChunkSize> vertex_;
        sfl::segmented_vector<simfil::Vertex3d, BigChunkSize> vertex3d_;
    } columns_;
};

ModelPool::ModelPool()
    : strings(std::make_shared<Strings>())
    , impl_(std::make_unique<ModelPool::Impl>())
{}

ModelPool::ModelPool(std::shared_ptr<Strings> stringStore)
    : strings(std::move(stringStore))
    , impl_(std::make_unique<ModelPool::Impl>())
{}

ModelPool::~ModelPool()
{}

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
        if (range.first >= impl_->columns_.members_.size() ||
            range.second + range.first > impl_->columns_.members_.size())
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
    for (auto const& member : impl_->columns_.members_) {
        validateModelIndex(member.nodeIndex_);
        validateString(member.name_);
    }
    for (auto const& memberRange : impl_->columns_.object_) {
        validateMemberRange(memberRange);
    }

    // Check array members
    for (auto const& memberRange : impl_->columns_.array_) {
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
    auto& columns = impl_->columns_;
    columns.root_.clear();
    columns.root_.shrink_to_fit();
    columns.object_.clear();
    columns.object_.shrink_to_fit();
    columns.array_.clear();
    columns.array_.shrink_to_fit();
    columns.i64_.clear();
    columns.i64_.shrink_to_fit();
    columns.vertex_.clear();
    columns.vertex_.shrink_to_fit();
    columns.vertex3d_.clear();
    columns.vertex3d_.shrink_to_fit();
    columns.members_.clear();
    columns.members_.shrink_to_fit();
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
        auto& memberRange = get(impl_->columns_.object_);
        return std::make_shared<ObjectModelNode>(memberRange, *this);
    }
    case Arrays: {
        auto& memberRange = get(impl_->columns_.array_);
        return std::make_shared<ArrayModelNode>(memberRange, *this);
    }
    case Vertex: {
        auto& vert = get(impl_->columns_.vertex_);
        return std::make_shared<VertexModelNode>(vert, *this);
    }
    case Vertex3d: {
        auto& vert = get(impl_->columns_.vertex3d_);
        return std::make_shared<Vertex3dModelNode>(vert, *this);
    }
    case UInt16: {
        return std::make_shared<ScalarModelNode>((int64_t)i.uint16());
    }
    case Int16: {
        return std::make_shared<ScalarModelNode>((int64_t)i.int16());
    }
    case Int64: {
        auto& val = get(impl_->columns_.i64_);
        return std::make_shared<ScalarModelNode>(val);
    }
    case Double: {
        auto& val = get(impl_->columns_.double_);
        return std::make_shared<ScalarModelNode>(val);
    }
    case String: {
        return std::make_shared<StringModelNode>(i.uint16(), strings);
    }
    case Null: {
        return std::make_shared<ModelNodeBase>();
    }
    case Bool: {
        return std::make_shared<ScalarModelNode>(i.uint16() != 0);
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
        if (idx >= impl_->columns_.members_.size())
            break;
        if (!f(impl_->columns_.members_[range.first + i]))
            break;
    }
}

size_t ModelPool::numRoots() const {
    return impl_->columns_.root_.size();
}

ModelNodePtr ModelPool::root(size_t const& i) const {
    if (i < 0 || i > impl_->columns_.root_.size())
        throw std::runtime_error("root index does not exist.");
    return resolve(impl_->columns_.root_[i]);
}

void ModelPool::addRoot(ModelNodeIndex const& rootIndex) {
    impl_->columns_.root_.emplace_back(rootIndex);
}

ModelPool::ModelNodeIndex ModelPool::addObject(std::vector<Member> const& members) {
    auto idx = impl_->columns_.object_.size();
    impl_->columns_.object_.emplace_back(addMembers(members));
    return {Objects, idx};
}

ModelPool::ModelNodeIndex ModelPool::addArray(std::vector<Member> const& members) {
    auto idx = impl_->columns_.array_.size();
    impl_->columns_.array_.emplace_back(addMembers(members));
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
    auto idx = impl_->columns_.i64_.size();
    impl_->columns_.i64_.emplace_back(value);
    return {Int64, idx};
}

ModelPool::ModelNodeIndex ModelPool::addValue(double const& value) {
    auto idx = impl_->columns_.double_.size();
    impl_->columns_.double_.emplace_back(value);
    return {Double, idx};
}

ModelPool::ModelNodeIndex ModelPool::addValue(std::string const& value) {
    auto strId = strings->emplace(value);
    return {String, strId};
}

ModelPool::ModelNodeIndex ModelPool::addVertex(double const& lon, double const& lat) {
    auto idx = impl_->columns_.vertex_.size();
    impl_->columns_.vertex_.emplace_back(lon, lat);
    return {Vertex, idx};
}

ModelPool::ModelNodeIndex ModelPool::addVertex3d(double const& lon, double const& lat, float const &elevation) {
    auto idx = impl_->columns_.vertex3d_.size();
    impl_->columns_.vertex3d_.emplace_back(lon, lat, elevation);
    return {Vertex3d, idx};
}

ModelPool::MemberRange ModelPool::addMembers(std::vector<ModelPool::Member> const& members)
{
    impl_->columns_.members_.reserve(impl_->columns_.members_.size() + members.size());
    auto memberOffset = impl_->columns_.members_.size();
    impl_->columns_.members_.insert(impl_->columns_.members_.end(), members.begin(), members.end());
    return MemberRange{memberOffset, members.size()};
}

/** Model Node impls for a scalar value. */

ScalarModelNode::ScalarModelNode(int64_t const& i) : value_(Value::make(i)) {}

ScalarModelNode::ScalarModelNode(double const& d) : value_(Value::make(d)) {}

ScalarModelNode::ScalarModelNode(bool const& b) : value_(Value::make(b)) {}

ScalarModelNode::ScalarModelNode(std::string const& s) : value_(Value::make(s)) {}

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
    if (i < fields_.size() && i >= 0) {
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

/** Model Node impls for a 3d vertex. */

Vertex3dModelNode::Vertex3dModelNode(Vertex3d const& coords, ModelPool const& modelPool)
    : ProceduralObjectModelNode({}, modelPool), coords_(coords)
{
    fields_.emplace_back(
        Strings::Lon,
        [this]() { return std::make_shared<ScalarModelNode>(std::get<0>(coords_)); });

    fields_.emplace_back(
        Strings::Lat,
        [this]() { return std::make_shared<ScalarModelNode>(std::get<1>(coords_)); });

    fields_.emplace_back(
        Strings::Elevation,
        [this]() { return std::make_shared<ScalarModelNode>(std::get<2>(coords_)); });
}

ModelNode::Type Vertex3dModelNode::type() const
{
    return ModelNode::Array;
}


}  // namespace simfil
