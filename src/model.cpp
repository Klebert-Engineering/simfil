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

namespace
{

template <typename T>
ModelNode numberToModel(T val, ModelPool const& pool)
{
    return {
        [val]() { return Value::make(val); },              /* value */
        []() { return ModelNode::Scalar; },              /* type */
        [](StringId const&) { return std::nullopt; }, /* getForName */
        [](int64_t const&) { return std::nullopt; },     /* getForIndex */
        []() { return std::vector<ModelNode>(); },       /* children */
        []() { return std::vector<std::string>(); },     /* keys */
        0                                                /* size */
    };
}

ModelNode stringToModel(StringId val, ModelPool const& pool)
{
    return {
        [val, &pool]()
        {
            if (auto str = pool.strings->resolve(val))
                return Value::make(*str);
            return Value::null();
        },                                               /* value */
        []() { return ModelNode::Scalar; },              /* type */
        [](StringId const&) { return std::nullopt; }, /* getForName */
        [](int64_t const&) { return std::nullopt; },     /* getForIndex */
        []() { return std::vector<ModelNode>(); },       /* children */
        []() { return std::vector<std::string>(); },     /* keys */
        0                                                /* size */
    };
}

ModelNode nullToModel()
{
    return {
        []() { return Value::null(); },              /* value */
        []() { return ModelNode::Scalar; },           /* type */
        [](StringId const&) { return std::nullopt; }, /* getForName */
        [](int64_t const&) { return std::nullopt; },  /* getForIndex */
        []() { return std::vector<ModelNode>(); },    /* children */
        []() { return std::vector<std::string>(); },  /* keys */
        0                                             /* size */
    };
}

ModelNode objectToModel(ModelPool::MemberRange const& members, ModelPool const& pool)
{
    return {
        []() { return Value::null(); },     /* value */
        []() { return ModelNode::Object; }, /* type */
        [&](StringId const& field) -> std::optional<ModelNode>
        {
            std::optional<ModelNode> result;
            pool.visitMembers(
                members,
                [&](ModelPool::Member const& m)
                {
                    if (m.name_ == field) {
                        result = pool.resolve(m.nodeIndex_);
                        return false;
                    }
                    return true;
                });
            return result;
        },                                           /* getForName */
        [](int64_t const&) { return std::nullopt; }, /* getForIndex */
        [&]()
        {
            std::vector<ModelNode> result;
            result.reserve(members.second);
            pool.visitMembers(
                members,
                [&](ModelPool::Member const& m)
                { result.emplace_back(pool.resolve(m.nodeIndex_)); return true; });
            return result;
        }, /* children */
        [&]()
        {
            std::vector<std::string> result;
            result.reserve(members.second);
            pool.visitMembers(
                members,
                [&](ModelPool::Member const& m)
                {
                    if (auto s = pool.strings->resolve(m.name_))
                        result.emplace_back(*s);
                    return true;
                });
            return result;
        },             /* keys */
        members.second /* size */
    };
}

ModelNode arrayToModel(ModelPool::MemberRange const& members, ModelPool const& pool)
{
    return {
        []() { return Value::null(); },                        /* value */
        []() { return ModelNode::Array; },                     /* type */
        [](StringId const& field) { return std::nullopt; }, /* getForName */
        [&](int64_t const& i)
        {
            std::optional<ModelNode> result;
            if (i >= 0 && i < members.second)
            {
                pool.visitMembers(
                    {members.first + i, 1},
                    [&](ModelPool::Member const& m)
                    {
                        result = pool.resolve(m.nodeIndex_);
                        return true;
                    });
            }
            return result;
        }, /* getForIndex */
        [&]()
        {
            std::vector<ModelNode> result;
            result.reserve(members.second);
            pool.visitMembers(
                members,
                [&](ModelPool::Member const& m)
                {
                    result.emplace_back(pool.resolve(m.nodeIndex_));
                    return true;
                });
            return result;
        },                                            /* children */
        [&]() { return std::vector<std::string>{}; }, /* keys */
        members.second                                /* size */
    };
}

ModelNode vertexToModel(std::pair<double, double> const& coords, ModelPool const& pool)
{
    return {
        []() { return Value::null(); },    /* value */
        []() { return ModelNode::Array; }, /* type */
        [&](StringId const& field) -> std::optional<ModelNode>
        {
            switch (field) {
            case Strings::Lon: return numberToModel(coords.first, pool);
            case Strings::Lat: return numberToModel(coords.second, pool);
            default: return std::nullopt;
            }
        }, /* getForName */
        [&](int64_t const& i) -> std::optional<ModelNode>
        {
            if (i == 0)
                return numberToModel(coords.first, pool);
            if (i == 1)
                return numberToModel(coords.second, pool);
            return std::nullopt;
        }, /* getForIndex */
        [&]() -> std::vector<ModelNode> {
            return {numberToModel(coords.first, pool), numberToModel(coords.second, pool)};
        }, /* children */
        [&]() -> std::vector<std::string> {
            return {"lon", "lat"};
        }, /* keys */
        2  /* size */
    };
}

}  // namespace

/** String Pool implementation */

Strings::Strings() {
    auto addStaticKey = [this](auto const& k, auto const& v) {
        idForString_[v] = k;
        stringForId_[k] = v;
    };
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

ModelNode ModelPool::resolve(ModelNodeIndex const& i) const {
    auto get = [&i](auto const& vec) -> auto& {
        auto idx = i.index();
        if (idx >= vec.size())
            throw std::runtime_error(stx::format("Bad node reference: col={}, i={}", (uint16_t)i.column(), idx));
        return vec[idx];
    };

    switch (i.column()) {
    case Objects: {
        auto& memberRange = get(columns_.object_);
        return objectToModel(memberRange, *this);
    }
    case Arrays: {
        auto& memberRange = get(columns_.array_);
        return arrayToModel(memberRange, *this);
    }
    case Vertices: {
        auto& vert = get(columns_.vertex_);
        return vertexToModel(vert, *this);
    }
    case UInt16: {
        return numberToModel((int64_t)i.uint16(), *this);
    }
    case Int16: {
        return numberToModel((int64_t)i.int16(), *this);
    }
    case Int64: {
        auto& val = get(columns_.i64_);
        return numberToModel(val, *this);
    }
    case Double: {
        auto& val = get(columns_.double_);
        return numberToModel(val, *this);
    }
    case String: {
        return stringToModel(i.uint16(), *this);
    }
    case Null: {
        return nullToModel();
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

ModelNode ModelPool::root(size_t const& i) const {
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

}  // namespace simfil
