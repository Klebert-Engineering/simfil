#include "simfil/model/model.h"
#include "simfil/model/arena.h"
#include "simfil/model/bitsery-traits.h"
#include "simfil/model/nodes.h"
#include "simfil/byte-array.h"
#include "simfil/value.h"

#include <memory>
#include <type_traits>
#include <variant>
#include <vector>
#include <deque>
#include <streambuf>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <bitsery/bitsery.h>
#include <bitsery/adapter/buffer.h>
#include <bitsery/adapter/stream.h>
#include <bitsery/traits/deque.h>
#include <bitsery/traits/string.h>
#include <bitsery/traits/vector.h>
#include <tl/expected.hpp>

#include "../expected.h"
#include "simfil/model/schema.h"

namespace simfil
{

tl::expected<void, Error> Model::resolve(const ModelNode& n, const ResolveFn& cb) const
{
    switch (n.addr_.column()) {
        case Null:
        {
            cb(ModelNodeBase(shared_from_this(), ModelNodeAddress{}, mpKey_));
            break;
        }
        case UInt16:
        {
            cb(SmallValueNode<uint16_t>(shared_from_this(), n.addr_, mpKey_));
            break;
        }
        case Int16:
        {
            cb(SmallValueNode<int16_t>(shared_from_this(), n.addr_, mpKey_));
            break;
        }
        case Bool:
        {
            cb(SmallValueNode<bool>(shared_from_this(), n.addr_, mpKey_));
            break;
        }
        case Scalar:
        {
            cb(ValueNode(n, mpKey_));
            break;
        }
        case Undefined:
        {
            cb(UndefinedNode(shared_from_this(), n.addr_, mpKey_));
            break;
        }
        default:
            return tl::unexpected<Error>(Error::RuntimeError,
                                         fmt::format("Bad column reference: col={}", (uint16_t)n.addr_.column()));
    }
    return {};
}

struct ModelPool::Impl
{
    explicit Impl(std::shared_ptr<StringPool> strings) :
        strings_(std::move(strings))
    {
        columns_.stringData_.reserve(detail::ColumnPageSize*4);
    }

    struct StringRange {
        MODEL_COLUMN_TYPE(8);

        uint32_t offset_;
        uint32_t length_;
    };

    /// This model pool's field name store
    std::shared_ptr<StringPool> strings_;
    bool frozen_ = false;

    // Common model bindings retain graphs once, not one owner per stored result.
    bool directSchema_ = false;
    std::vector<std::string> schemaIdentities_;
    std::vector<std::shared_ptr<const SchemaBinding>> schemaBindings_;
    ModelColumn<TwoPart<std::uint16_t, SchemaId>> importedSchemas_;
    SchemaBinding::Resolver schemaResolver_;

    struct {
        ModelColumn<ModelNodeAddress, detail::ColumnPageSize> roots_;
        ModelColumn<int64_t, detail::ColumnPageSize> i64_;
        ModelColumn<double, detail::ColumnPageSize> double_;

        std::string stringData_;
        ModelColumn<StringRange, detail::ColumnPageSize> strings_;
        ModelColumn<StringRange, detail::ColumnPageSize> byteArrays_;
        ModelColumn<TwoPart<ModelNodeAddress, SchemaId>> qualifiedScalars_;
        ModelColumn<TwoPart<std::uint64_t, SchemaId>> bitmasks_;

        ModelColumn<SchemaId, detail::ColumnPageSize> objectSchemas_;
        ModelColumn<SchemaId, detail::ColumnPageSize> objectSingletonSchemas_;
        ModelColumn<SchemaId, detail::ColumnPageSize> arraySchemas_;
        ModelColumn<SchemaId, detail::ColumnPageSize> arraySingletonSchemas_;
        Object::Storage objectMemberArrays_;
        Array::Storage arrayMemberArrays_;
    } columns_;

    /** Bound variable-size primitive allocations by bytes remaining in the actual input buffer. */
    template<class S>
    static auto available(S& serializer, std::size_t maximum) -> std::size_t
    {
        if constexpr (requires { serializer.adapter().currentReadEndPos(); }) {
            auto& adapter = serializer.adapter();
            if (adapter.error() != bitsery::ReaderError::NoError)
                return 0;
            auto start = adapter.currentReadPos(), end = adapter.currentReadEndPos();
            return std::min(maximum, end >= start ? end - start : 0);
        }
        return maximum;
    }

    template<typename S>
    void readWrite(S& s) {
        constexpr size_t maxColumnSize = std::numeric_limits<uint32_t>::max();
        s.object(columns_.roots_);
        s.object(columns_.i64_);
        s.object(columns_.double_);
        s.text1b(columns_.stringData_, available(s, maxColumnSize));
        s.object(columns_.strings_);
        s.object(columns_.byteArrays_);
        s.object(columns_.objectSchemas_);
        s.object(columns_.objectSingletonSchemas_);
        s.object(columns_.arraySchemas_);
        s.object(columns_.arraySingletonSchemas_);

        s.ext(columns_.objectMemberArrays_, bitsery::ext::ArrayArenaExt{});
        s.ext(columns_.arrayMemberArrays_, bitsery::ext::ArrayArenaExt{});
        s.object(columns_.qualifiedScalars_);
        s.object(columns_.bitmasks_);
        s.boolValue(directSchema_);
        s.container(schemaIdentities_, available(s, size_t(MaxSchemaId)), [](auto& serializer, auto& identity) {
            serializer.text1b(identity, available(serializer, size_t{65535}));
        });
        s.object(importedSchemas_);
    }
};

/** A column-backed scalar view: qualifiers delegate primitive access, masks retain all raw bits. */
class ModelPool::ScalarNode final : public ModelNodeBase
{
public:
    /** Retain the same persistent address/owner without allocating a separate scalar object. */
    ScalarNode(ModelPool const& owner, ModelNode const& node)
        : ModelNodeBase(node, owner.mpKey_), owner_(owner) {}

    /** Both scalar columns carry pool-local schema ids interpreted by their common bindings. */
    auto schema() const -> SchemaId override
    {
        return addr().column() == Bitmask ? owner_.impl_->columns_.bitmasks_[addr().index()].second()
            : owner_.impl_->columns_.qualifiedScalars_[addr().index()].second();
    }

    /** Owning mask values can outlive the model; ordinary scalar payloads retain their primitive form. */
    auto value() const -> ScalarValueType override
    {
        if (addr().column() == Bitmask)
            return simfil::Bitmask{owner_.impl_->columns_.bitmasks_[addr().index()].first(),
                                  owner_.schemaReference(schema())};
        return owner_.resolve(owner_.impl_->columns_.qualifiedScalars_[addr().index()].first())->value();
    }

    /** Qualification never invents an Enum type or changes numeric arithmetic. */
    auto type() const -> ValueType override
    {
        return addr().column() == Bitmask ? ValueType::Bitmask
            : owner_.resolve(owner_.impl_->columns_.qualifiedScalars_[addr().index()].first())->type();
    }

private:
    ModelPool const& owner_;
};

ModelPool::ModelPool()
    : impl_(std::make_unique<ModelPool::Impl>(std::make_shared<StringPool>()))
{}

ModelPool::ModelPool(std::shared_ptr<StringPool> stringStore)
    : impl_(std::make_unique<ModelPool::Impl>(std::move(stringStore)))
{}

ModelPool::~ModelPool()  // NOLINT
{}

void ModelPool::freeze()
{
    impl_->strings_->freeze();
    impl_->frozen_ = true;
}

auto ModelPool::frozen() const -> bool { return impl_->frozen_; }

void ModelPool::requireMutable() const
{
    if (frozen())
        throw std::logic_error("Cannot mutate a frozen ModelPool");
}

std::vector<std::string> ModelPool::checkForErrors() const
{
    std::vector<std::string> errors;
    auto objectHasSchema = [&](ArrayIndex members) {
        if (Object::Storage::is_singleton_handle(members)) {
            return Object::Storage::singleton_payload(members)
                < impl_->columns_.objectSingletonSchemas_.size();
        }
        return members < impl_->columns_.objectSchemas_.size();
    };
    auto arrayHasSchema = [&](ArrayIndex members) {
        if (Array::Storage::is_singleton_handle(members)) {
            return Array::Storage::singleton_payload(members)
                < impl_->columns_.arraySingletonSchemas_.size();
        }
        return members < impl_->columns_.arraySchemas_.size();
    };

    auto validateArrayIndex = [&](auto i, auto arrType, auto const& arena) {
        if (!arena.valid(static_cast<ArrayIndex>(i))) {
            errors.emplace_back(fmt::format("Bad {} array index {}.", arrType, i));
            return false;
        }
        return true;
    };

    auto validatePooledString = [&, this](StringId const& str)
    {
        if (!impl_->strings_)
            return;
        if (!impl_->strings_->resolve(str))
            errors.push_back(fmt::format("Bad string ID: {}", str));
    };

    std::function<void(ModelNode::Ptr)> validateModelNode = [&](ModelNode::Ptr node)
    {
        if (node->type() == ValueType::Object) {
            // Custom object columns own their schema storage; their indices are not ours.
            if (node->addr().column() == Objects) {
                if (!validateArrayIndex(node->addr().index(), "object", impl_->columns_.objectMemberArrays_))
                    return;
                if (!objectHasSchema(node->addr().index())) {
                    errors.emplace_back(fmt::format("Missing object schema index {}.", node->addr().index()));
                    return;
                }
            }
            for (auto const& [fieldName, fieldValue] : node->fields()) {
                validatePooledString(fieldName);
                validateModelNode(fieldValue);
            }
        }
        else if (node->type() == ValueType::Array) {
            // Custom arrays still have their children checked, but need no core arena/schema entry.
            if (node->addr().column() == Arrays) {
                if (!validateArrayIndex(node->addr().index(), "arrays", impl_->columns_.arrayMemberArrays_))
                    return;
                if (!arrayHasSchema(node->addr().index())) {
                    errors.emplace_back(fmt::format("Missing array schema index {}.", node->addr().index()));
                    return;
                }
            }
            for (auto const& member : *node)
                validateModelNode(member);
        }
        else if (node->addr().column() == PooledString) {
            validatePooledString(static_cast<StringId>(node->addr().index()));
        }

        if (auto res = resolve(*node, Lambda([](auto&&) {})); !res)
            errors.emplace_back(res.error().message);
    };

    // Validate objects
    for (auto i = FirstRegularArrayIndex; i < impl_->columns_.objectMemberArrays_.size(); ++i)
        validateModelNode(ModelNode::Ptr::make(
            shared_from_this(),
            ModelNodeAddress{Objects, (uint32_t)i}));
    for (auto i = 0u; i < impl_->columns_.objectMemberArrays_.singleton_handle_count(); ++i)
        validateModelNode(ModelNode::Ptr::make(
            shared_from_this(),
            ModelNodeAddress{
                Objects,
                SingletonArrayHandleMask | static_cast<uint32_t>(i)}));

    // Validate arrays
    for (auto i = FirstRegularArrayIndex; i < impl_->columns_.arrayMemberArrays_.size(); ++i)
        validateModelNode(ModelNode::Ptr::make(
            shared_from_this(),
            ModelNodeAddress{Arrays, (uint32_t)i}));
    for (auto i = 0u; i < impl_->columns_.arrayMemberArrays_.singleton_handle_count(); ++i)
        validateModelNode(ModelNode::Ptr::make(
            shared_from_this(),
            ModelNodeAddress{
                Arrays,
                SingletonArrayHandleMask | static_cast<uint32_t>(i)}));

    // Validate roots
    for (auto i = 0; i < numRoots(); ++i)
        if (auto node = root(i))
            validateModelNode(*node);

    return errors;
}

auto ModelPool::validate() const -> tl::expected<void, Error>
{
    auto errors = checkForErrors();
    if (!errors.empty()) {
        return tl::unexpected<Error>(Error::RuntimeError, fmt::format("Model Error(s): {}", fmt::join(errors, ", ")));
    }
    return {};
}

void ModelPool::clear()
{
    requireMutable();
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
    clear_and_shrink(columns.byteArrays_);
    clear_and_shrink(columns.qualifiedScalars_);
    clear_and_shrink(columns.bitmasks_);
    clear_and_shrink(impl_->importedSchemas_);
    impl_->schemaBindings_.clear();
    impl_->schemaIdentities_.clear();
    impl_->directSchema_ = false;
    clear_and_shrink(columns.objectSchemas_);
    clear_and_shrink(columns.objectSingletonSchemas_);
    clear_and_shrink(columns.arraySchemas_);
    clear_and_shrink(columns.arraySingletonSchemas_);
    clear_and_shrink(columns.objectMemberArrays_);
    clear_and_shrink(columns.arrayMemberArrays_);
}

tl::expected<void, Error> ModelPool::resolve(ModelNode const& n, ResolveFn const& cb) const
{
    // Merged/container views can surface child nodes from another model. Always
    // let the owning model interpret its own column/index address.
    if (auto owner = n.owningModel(); owner && owner.get() != this) {
        return owner->resolve(n, cb);
    }

    auto checkBounds = [&n](auto const& vec) -> std::optional<Error> {
        auto idx = n.addr_.index();
        if (idx >= vec.size())
            return Error(Error::RuntimeError, fmt::format("bad node reference: col={}, i={}",
                                                          (uint16_t)n.addr_.column(), idx));
        return {};
    };

    switch (n.addr_.column()) {
    case Objects: {
        cb(Object(shared_from_this(), n.addr_, mpKey_));
        break;
    }
    case Arrays: {
        cb(Array(shared_from_this(), n.addr_, mpKey_));
        break;
    }
    case Int64: {
        if (auto err = checkBounds(impl_->columns_.i64_))
            return tl::unexpected<Error>(*err);
        auto idx = n.addr().index();
        auto& val = impl_->columns_.i64_[idx];
        cb(ValueNode(val, shared_from_this(), mpKey_));
        break;
    }
    case Double: {
        if (auto err = checkBounds(impl_->columns_.double_))
            return tl::unexpected<Error>(*err);
        auto idx = n.addr().index();
        auto& val = impl_->columns_.double_[idx];
        cb(ValueNode(val, shared_from_this(), mpKey_));
        break;
    }
    case String: {
        auto idx = n.addr().index();
        if (auto err = checkBounds(impl_->columns_.strings_))
            return tl::unexpected<Error>(*err);
        auto& val = impl_->columns_.strings_[idx];
        cb(ValueNode(
            // TODO: Make sure that the string view is not turned into a string here.
            std::string_view(impl_->columns_.stringData_).substr(val.offset_, val.length_),
            shared_from_this(),
            mpKey_));
        break;
    }
    case ByteArray: {
        auto idx = n.addr().index();
        if (auto err = checkBounds(impl_->columns_.byteArrays_))
            return tl::unexpected<Error>(*err);
        auto& val = impl_->columns_.byteArrays_[idx];
        auto view = std::string_view(impl_->columns_.stringData_).substr(val.offset_, val.length_);
        cb(ValueNode(simfil::ByteArray{view}, shared_from_this(), mpKey_));
        break;
    }
    case PooledString: {
        auto str = lookupStringId(static_cast<StringId>(n.addr().index()));
        cb(ValueNode(str.value_or(std::string_view{}), shared_from_this(), mpKey_));
        break;
    }
    case QualifiedScalar: {
        if (auto error = checkBounds(impl_->columns_.qualifiedScalars_))
            return tl::unexpected(*error);
        cb(ScalarNode(*this, n));
        break;
    }
    case Bitmask: {
        if (auto error = checkBounds(impl_->columns_.bitmasks_))
            return tl::unexpected(*error);
        cb(ScalarNode(*this, n));
        break;
    }
    default:
        return Model::resolve(n, cb);
    }
    return {};
}

size_t ModelPool::numRoots() const {
    return impl_->columns_.roots_.size();
}

tl::expected<ModelNode::Ptr, Error> ModelPool::root(size_t const& i) const {
    if ((i < 0) || (i >= impl_->columns_.roots_.size()))
        return tl::unexpected<Error>(Error::RuntimeError, "Root index does not exist.");
    return ModelNode::Ptr::make(shared_from_this(), impl_->columns_.roots_.at(i));
}

void ModelPool::addRoot(ModelNode::Ptr const& rootNode) {
    requireMutable();
    impl_->columns_.roots_.emplace_back(rootNode->addr_);
}

model_ptr<Object> ModelPool::newObject(size_t initialFieldCapacity, bool fixedSize)
{
    requireMutable();
    auto memberArrId = impl_->columns_.objectMemberArrays_.new_array(initialFieldCapacity, fixedSize);
    if (Object::Storage::is_singleton_handle(memberArrId)) {
        auto singletonIndex = Object::Storage::singleton_payload(memberArrId);
        if (impl_->columns_.objectSingletonSchemas_.size() <= singletonIndex)
            impl_->columns_.objectSingletonSchemas_.resize(singletonIndex + 1);
        impl_->columns_.objectSingletonSchemas_[singletonIndex] = SchemaId{};
    } else {
        if (impl_->columns_.objectSchemas_.size() <= memberArrId)
            impl_->columns_.objectSchemas_.resize(memberArrId + 1);
        impl_->columns_.objectSchemas_[memberArrId] = SchemaId{};
    }
    return model_ptr<Object>::make(shared_from_this(), ModelNodeAddress{Objects, (uint32_t)memberArrId});
}

model_ptr<Array> ModelPool::newArray(size_t initialFieldCapacity, bool fixedSize)
{
    requireMutable();
    auto memberArrId = impl_->columns_.arrayMemberArrays_.new_array(initialFieldCapacity, fixedSize);
    if (Array::Storage::is_singleton_handle(memberArrId)) {
        auto singletonIndex = Array::Storage::singleton_payload(memberArrId);
        if (impl_->columns_.arraySingletonSchemas_.size() <= singletonIndex)
            impl_->columns_.arraySingletonSchemas_.resize(singletonIndex + 1);
        impl_->columns_.arraySingletonSchemas_[singletonIndex] = SchemaId{};
    } else {
        if (impl_->columns_.arraySchemas_.size() <= memberArrId)
            impl_->columns_.arraySchemas_.resize(memberArrId + 1);
        impl_->columns_.arraySchemas_[memberArrId] = SchemaId{};
    }
    return model_ptr<Array>::make(shared_from_this(), ModelNodeAddress{Arrays, (uint32_t)memberArrId});
}

ModelNode::Ptr Model::newUndefined()
{
    return resolve(ModelNodeAddress{Undefined, 1});
}

ModelNode::Ptr Model::newSmallValue(bool value)
{
    return ModelNode::Ptr::make(
        shared_from_this(),
        ModelNodeAddress{Bool, (uint32_t)value});
}

ModelNode::Ptr Model::newSmallValue(int16_t value)
{
    return ModelNode::Ptr::make(
        shared_from_this(),
        ModelNodeAddress{Int16, static_cast<uint16_t>(value)});
}

ModelNode::Ptr Model::newSmallValue(uint16_t value)
{
    return ModelNode::Ptr::make(
        shared_from_this(),
        ModelNodeAddress{UInt16, (uint32_t)value});
}

std::optional<std::string_view> Model::lookupStringId(const simfil::StringId) const
{
    return {};
}

auto Model::schemaReference(SchemaId) const -> SchemaReference
{
    return {};
}

auto Model::schemaReference(ModelNode const& node) const -> SchemaReference
{
    return schemaReference(node.schema());
}

void ModelPool::setSchemaBinding(std::shared_ptr<const SchemaBinding> binding)
{
    requireMutable();
    if (!binding)
        throw std::invalid_argument("A direct schema binding must be resolved");
    if (!impl_->schemaBindings_.empty()) {
        if (impl_->directSchema_ && impl_->schemaIdentities_.front() == binding->identity())
            return;
        throw std::invalid_argument("Cannot replace a populated model schema namespace");
    }
    impl_->directSchema_ = true;
    impl_->schemaIdentities_.emplace_back(binding->identity());
    impl_->schemaBindings_.push_back(std::move(binding));
}

void ModelPool::setSchemaResolver(SchemaBinding::Resolver resolver)
{
    requireMutable();
    impl_->schemaResolver_ = std::move(resolver);
}

auto ModelPool::schemaBinding(std::string_view identity) const -> std::shared_ptr<const SchemaBinding>
{
    for (auto const& binding : impl_->schemaBindings_)
        if (binding && binding->identity() == identity)
            return binding;
    auto binding = impl_->schemaResolver_ ? impl_->schemaResolver_(identity) : nullptr;
    return binding && binding->identity() == identity ? binding : nullptr;
}

auto ModelPool::schemaReference(SchemaId id) const -> SchemaReference
{
    if (id == NoSchemaId || impl_->schemaBindings_.empty())
        return {};
    if (impl_->directSchema_)
        return {impl_->schemaBindings_.front(), id};
    if (size_t(id) > impl_->importedSchemas_.size())
        return {};
    auto entry = impl_->importedSchemas_[size_t(id) - 1];
    if (entry.first() >= impl_->schemaBindings_.size())
        return {};
    return {impl_->schemaBindings_[entry.first()], entry.second()};
}

auto ModelPool::hasDirectSchemaBinding(std::string_view identity) const noexcept -> bool
{
    return impl_->directSchema_ && impl_->schemaIdentities_.size() == 1 && impl_->schemaBindings_.size() == 1 &&
        impl_->schemaIdentities_.front() == identity && impl_->schemaBindings_.front() &&
        impl_->schemaBindings_.front()->identity() == identity && impl_->importedSchemas_.empty();
}

auto ModelPool::schemaReference(ModelNode const& node) const -> SchemaReference
{
    if (impl_->schemaBindings_.empty())
        return {};
    return schemaReference(node.schema());
}

auto ModelPool::importSchema(SchemaReference const& domain) -> tl::expected<SchemaId, Error>
{
    if (frozen())
        return tl::unexpected(Error{Error::RuntimeError, "Cannot import a schema into a frozen ModelPool"});
    if (!domain || !domain.definition())
        return tl::unexpected(Error{Error::RuntimeError, "Cannot import an unresolved schema domain"});
    if (impl_->directSchema_) {
        if (domain.binding->identity() != impl_->schemaIdentities_.front())
            return tl::unexpected(Error{Error::RuntimeError, "Foreign domain in a single-graph model"});
        return domain.id;
    }
    auto identity = std::ranges::find(impl_->schemaIdentities_, domain.binding->identity());
    auto bindingIndex = size_t(identity - impl_->schemaIdentities_.begin());
    for (size_t i = 0; i < impl_->importedSchemas_.size(); ++i) {
        auto entry = impl_->importedSchemas_[i];
        if (entry.first() == bindingIndex && entry.second() == domain.id)
            return static_cast<SchemaId>(i + 1);
    }
    if (impl_->importedSchemas_.size() >= MaxSchemaId || bindingIndex >= MaxSchemaId)
        return tl::unexpected(Error{Error::RuntimeError, "Model schema namespace exhausted"});
    if (identity == impl_->schemaIdentities_.end()) {
        impl_->schemaIdentities_.emplace_back(domain.binding->identity());
        impl_->schemaBindings_.push_back(domain.binding);
    }
    impl_->importedSchemas_.emplace_back(static_cast<std::uint16_t>(bindingIndex), domain.id);
    return static_cast<SchemaId>(impl_->importedSchemas_.size());
}

auto ModelPool::newBitmask(std::uint64_t bits, SchemaReference const& domain) -> ModelNode::Ptr
{
    requireMutable();
    auto definition = domain.definition();
    if (!definition || !definition->isBitmask() ||
        definition->bitWidth() == 0 || definition->bitWidth() > 64)
        throw std::invalid_argument("Bitmask requires a compatible bound domain and width");
    if (definition->bitWidth() < 64 && (bits >> definition->bitWidth()) != 0)
        throw std::invalid_argument("Bitmask exceeds its declared encoding width");
    auto imported = importSchema(domain);
    if (!imported)
        throw std::invalid_argument(imported.error().message);
    auto index = impl_->columns_.bitmasks_.size();
    ModelNodeAddress::checkIndex(index);
    impl_->columns_.bitmasks_.emplace_back(bits, *imported);
    return resolve(ModelNodeAddress{Bitmask, index});
}

auto ModelPool::newQualifiedValue(ScalarValueType value, SchemaReference const& domain) -> ModelNode::Ptr
{
    requireMutable();
    if (std::holds_alternative<std::monostate>(value))
        throw std::invalid_argument("Null is not a schema-qualified primitive");
    if (auto mask = std::get_if<simfil::Bitmask>(&value)) {
        if (mask->domain != domain)
            throw std::invalid_argument("Cannot rebind a bitmask to a different domain");
        return newBitmask(mask->bits, domain);
    }
    if (!domain.definition() || !domain.definition()->hasAffinity(Value{ScalarValueType(value)}.type))
        throw std::invalid_argument("Qualified scalar requires a compatible domain");
    auto imported = importSchema(domain);
    if (!imported)
        throw std::invalid_argument(imported.error().message);
    auto raw = std::visit([&](auto const& scalar) -> ModelNode::Ptr {
        using T = std::decay_t<decltype(scalar)>;
        if constexpr (std::is_same_v<T, std::monostate>)
            return resolve(ModelNodeAddress{Null, 1});
        else if constexpr (std::is_same_v<T, bool>)
            return newSmallValue(scalar);
        else if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, std::string_view>)
            return newValue(std::string_view(scalar));
        else if constexpr (std::is_same_v<T, simfil::Bitmask>)
            return newBitmask(scalar.bits, scalar.domain);
        else
            return newValue(scalar);
    }, value);
    auto index = impl_->columns_.qualifiedScalars_.size();
    ModelNodeAddress::checkIndex(index);
    impl_->columns_.qualifiedScalars_.emplace_back(raw->addr(), *imported);
    return resolve(ModelNodeAddress{QualifiedScalar, index});
}

ModelNode::Ptr ModelPool::newValue(int64_t const& value)
{
    requireMutable();
    if (value < 0 && value >= std::numeric_limits<int16_t>::min())
        return newSmallValue((int16_t)value);
    else if (value >= 0 && value <= std::numeric_limits<uint16_t>::max())
        return newSmallValue((uint16_t)value);
    ModelNodeAddress::checkIndex(impl_->columns_.i64_.size());
    impl_->columns_.i64_.emplace_back(value);
    return ModelNode::Ptr::make(
        shared_from_this(),
        ModelNodeAddress{Int64, (uint32_t)impl_->columns_.i64_.size()-1});
}

ModelNode::Ptr ModelPool::newValue(double const& value)
{
    requireMutable();
    ModelNodeAddress::checkIndex(impl_->columns_.double_.size());
    impl_->columns_.double_.emplace_back(value);
    return ModelNode::Ptr::make(
        shared_from_this(),
        ModelNodeAddress{Double, (uint32_t)impl_->columns_.double_.size()-1});
}

ModelNode::Ptr ModelPool::newValue(std::string_view const& value)
{
    requireMutable();
    ModelNodeAddress::checkIndex(impl_->columns_.strings_.size());
    check_model_column_growth(impl_->columns_.stringData_.size(), value.size(), 1);
    impl_->columns_.stringData_.reserve(impl_->columns_.stringData_.size() + value.size());
    impl_->columns_.strings_.emplace_back(Impl::StringRange{
        (uint32_t)impl_->columns_.stringData_.size(),
        (uint32_t)value.size()
    });
    impl_->columns_.stringData_ += value;
    return ModelNode::Ptr::make(
        shared_from_this(),
        ModelNodeAddress{String, (uint32_t)impl_->columns_.strings_.size()-1});
}

ModelNode::Ptr ModelPool::newValue(simfil::ByteArray const& value)
{
    requireMutable();
    ModelNodeAddress::checkIndex(impl_->columns_.byteArrays_.size());
    check_model_column_growth(impl_->columns_.stringData_.size(), value.bytes.size(), 1);
    impl_->columns_.stringData_.reserve(impl_->columns_.stringData_.size() + value.bytes.size());
    impl_->columns_.byteArrays_.emplace_back(Impl::StringRange{
        (uint32_t)impl_->columns_.stringData_.size(),
        (uint32_t)value.bytes.size()
    });
    impl_->columns_.stringData_.append(value.bytes.data(), value.bytes.size());
    return ModelNode::Ptr::make(
        shared_from_this(),
        ModelNodeAddress{ByteArray, (uint32_t)impl_->columns_.byteArrays_.size()-1});
}

ModelNode::Ptr ModelPool::newValue(StringId handle) {
    requireMutable();
    return ModelNode::Ptr::make(
        shared_from_this(),
        ModelNodeAddress{PooledString, static_cast<uint32_t>(handle)});
}

// Core ADL resolve hooks for base Object/Array nodes.
template<>
model_ptr<Object> resolveInternal(res::tag<Object>, ModelPool const& model, ModelNode const& node)
{
    if (node.addr().column() != ModelPool::Objects)
        raise<std::runtime_error>("Cannot cast this node to an object.");
    return model_ptr<Object>::make(model.shared_from_this(), node.addr());
}

template<>
model_ptr<Array> resolveInternal(res::tag<Array>, ModelPool const& model, ModelNode const& node)
{
    if (node.addr().column() != ModelPool::Arrays)
        raise<std::runtime_error>("Cannot cast this node to an array.");
    return model_ptr<Array>::make(model.shared_from_this(), node.addr());
}

std::shared_ptr<StringPool> ModelPool::strings() const
{
    return impl_->strings_;
}

auto ModelPool::setStrings(std::shared_ptr<StringPool> const& strings) -> tl::expected<void, Error>
{
    if (frozen())
        return tl::unexpected(Error{Error::RuntimeError, "Cannot rebind a frozen ModelPool dictionary"});
    if (!strings)
        return tl::unexpected<Error>(Error::RuntimeError, "Attempt to call ModelPool::setStrings(nullptr)!");

    auto oldStrings = impl_->strings_;
    impl_->strings_ = strings;
    if (!oldStrings || *strings == *oldStrings)
        return {};

    // Translate object field IDs to the new dictionary.
    for (auto memberArray : impl_->columns_.objectMemberArrays_) {
        for (auto member : memberArray) {
            if (auto resolvedName = oldStrings->resolve(detail::objectFieldName(member))) {
                auto stringId = strings->emplace(*resolvedName);
                TRY_EXPECTED(stringId);
                detail::objectFieldName(member) = *stringId;
            }
        }
    }

    return {};
}

ModelPool::SerializationSizeStats ModelPool::serializationSizeStats() const
{
    SerializationSizeStats stats;
    stats.rootsBytes = impl_->columns_.roots_.byte_size();
    stats.int64Bytes = impl_->columns_.i64_.byte_size();
    stats.doubleBytes = impl_->columns_.double_.byte_size();
    stats.stringDataBytes = impl_->columns_.stringData_.size();
    stats.stringRangeBytes = impl_->columns_.strings_.byte_size();
    stats.stringRangeBytes += impl_->columns_.byteArrays_.byte_size();
    stats.objectMemberBytes = impl_->columns_.objectMemberArrays_.byte_size();
    stats.objectSchemaBytes = impl_->columns_.objectSchemas_.byte_size()
        + impl_->columns_.objectSingletonSchemas_.byte_size();
    stats.arrayMemberBytes = impl_->columns_.arrayMemberArrays_.byte_size();
    stats.arraySchemaBytes = impl_->columns_.arraySchemas_.byte_size()
        + impl_->columns_.arraySingletonSchemas_.byte_size();
    stats.qualifiedScalarBytes = impl_->columns_.qualifiedScalars_.byte_size();
    stats.bitmaskBytes = impl_->columns_.bitmasks_.byte_size();
    stats.schemaBindingBytes = impl_->importedSchemas_.byte_size();
    for (auto const& identity : impl_->schemaIdentities_)
        stats.schemaBindingBytes += identity.size();
    return stats;
}

ModelPool::MemoryUsageStats ModelPool::memoryUsageStats() const
{
    MemoryUsageStats stats;
    stats.implementation = {sizeof(Impl), sizeof(Impl)};
    stats.roots = impl_->columns_.roots_.memory_usage();
    stats.int64Values = impl_->columns_.i64_.memory_usage();
    stats.doubleValues = impl_->columns_.double_.memory_usage();
    stats.stringData = {
        impl_->columns_.stringData_.size(),
        impl_->columns_.stringData_.capacity(),
    };
    stats.stringRanges = impl_->columns_.strings_.memory_usage();
    stats.byteArrayRanges = impl_->columns_.byteArrays_.memory_usage();
    stats.objectMembers = impl_->columns_.objectMemberArrays_.memory_usage();
    stats.objectSchemas =
        impl_->columns_.objectSchemas_.memory_usage() +
        impl_->columns_.objectSingletonSchemas_.memory_usage();
    stats.arrayMembers = impl_->columns_.arrayMemberArrays_.memory_usage();
    stats.arraySchemas =
        impl_->columns_.arraySchemas_.memory_usage() +
        impl_->columns_.arraySingletonSchemas_.memory_usage();
    stats.qualifiedScalars = impl_->columns_.qualifiedScalars_.memory_usage();
    stats.bitmasks = impl_->columns_.bitmasks_.memory_usage();
    stats.schemaBindings = impl_->importedSchemas_.memory_usage();
    stats.schemaBindings.allocatedBytes += impl_->schemaBindings_.capacity() * sizeof(std::shared_ptr<const SchemaBinding>)
        + impl_->schemaIdentities_.capacity() * sizeof(std::string);
    for (auto const& identity : impl_->schemaIdentities_) {
        stats.schemaBindings.logicalBytes += identity.size();
        stats.schemaBindings.allocatedBytes += identity.capacity();
    }
    return stats;
}

std::optional<std::string_view> ModelPool::lookupStringId(const simfil::StringId id) const
{
    return impl_->strings_->resolve(id);
}

Object::Storage& ModelPool::objectMemberStorage() {
    return impl_->columns_.objectMemberArrays_;
}

SchemaId ModelPool::objectSchemaId(ArrayIndex members) const
{
    if (Object::Storage::is_singleton_handle(members)) {
        auto singletonIndex = Object::Storage::singleton_payload(members);
        if (singletonIndex >= impl_->columns_.objectSingletonSchemas_.size())
            return {};
        return SchemaId{impl_->columns_.objectSingletonSchemas_[singletonIndex]};
    }
    if (members >= impl_->columns_.objectSchemas_.size())
        return {};
    return SchemaId{impl_->columns_.objectSchemas_[members]};
}

auto ModelPool::setObjectSchemaId(ArrayIndex members, SchemaId schemaId) -> tl::expected<void, Error>
{
    if (frozen())
        return tl::unexpected(Error{Error::RuntimeError, "Cannot change a frozen object's schema"});
    if (!impl_->columns_.objectMemberArrays_.valid(members))
        return tl::unexpected<Error>(Error::RuntimeError, "Object schema index out of range.");

    if (Object::Storage::is_singleton_handle(members)) {
        auto singletonIndex = Object::Storage::singleton_payload(members);
        if (impl_->columns_.objectSingletonSchemas_.size() <= singletonIndex)
            impl_->columns_.objectSingletonSchemas_.resize(singletonIndex + 1);
        impl_->columns_.objectSingletonSchemas_[singletonIndex] = schemaId;
        return {};
    }

    if (impl_->columns_.objectSchemas_.size() <= members)
        impl_->columns_.objectSchemas_.resize(members + 1);
    impl_->columns_.objectSchemas_[members] = schemaId;
    return {};
}

SchemaId ModelPool::arraySchemaId(ArrayIndex members) const
{
    if (Array::Storage::is_singleton_handle(members)) {
        auto singletonIndex = Array::Storage::singleton_payload(members);
        if (singletonIndex >= impl_->columns_.arraySingletonSchemas_.size())
            return {};
        return SchemaId{impl_->columns_.arraySingletonSchemas_[singletonIndex]};
    }
    if (members >= impl_->columns_.arraySchemas_.size())
        return {};
    return SchemaId{impl_->columns_.arraySchemas_[members]};
}

auto ModelPool::setArraySchemaId(ArrayIndex members, SchemaId schemaId) -> tl::expected<void, Error>
{
    if (frozen())
        return tl::unexpected(Error{Error::RuntimeError, "Cannot change a frozen array's schema"});
    if (!impl_->columns_.arrayMemberArrays_.valid(members))
        return tl::unexpected<Error>(Error::RuntimeError, "Array schema index out of range.");

    if (Array::Storage::is_singleton_handle(members)) {
        auto singletonIndex = Array::Storage::singleton_payload(members);
        if (impl_->columns_.arraySingletonSchemas_.size() <= singletonIndex)
            impl_->columns_.arraySingletonSchemas_.resize(singletonIndex + 1);
        impl_->columns_.arraySingletonSchemas_[singletonIndex] = schemaId;
        return {};
    }

    if (impl_->columns_.arraySchemas_.size() <= members)
        impl_->columns_.arraySchemas_.resize(members + 1);
    impl_->columns_.arraySchemas_[members] = schemaId;
    return {};
}

Object::Storage const& ModelPool::objectMemberStorage() const
{
    return impl_->columns_.objectMemberArrays_;
}

Array::Storage& ModelPool::arrayMemberStorage() {
    return impl_->columns_.arrayMemberArrays_;
}

Array::Storage const& ModelPool::arrayMemberStorage() const
{
    return impl_->columns_.arrayMemberArrays_;
}

tl::expected<void, Error> ModelPool::write(std::ostream& outputStream) {
    bitsery::Serializer<bitsery::OutputStreamAdapter> s(outputStream);
    impl_->readWrite(s);
    return {};
}

tl::expected<void, Error> ModelPool::read(const std::vector<uint8_t>& input, const size_t offset) {
    if (frozen())
        return tl::unexpected(Error{Error::RuntimeError, "Cannot read into a frozen ModelPool"});
    if (offset > input.size()) {
        return tl::unexpected<Error>(Error::EncodeDecodeError, "Failed to read ModelPool: invalid input offset.");
    }

    using Adapter = bitsery::InputBufferAdapter<std::vector<uint8_t>>;
    bitsery::Deserializer<Adapter> s(Adapter(input.begin() + static_cast<std::ptrdiff_t>(offset), input.end()));
    // Bitsery otherwise reports zero as an unbounded-end sentinel, not the buffer's physical size.
    s.adapter().currentReadEndPos(input.size() - offset);
    impl_->readWrite(s);
    if (s.adapter().error() != bitsery::ReaderError::NoError) {
        return tl::unexpected<Error>(Error::EncodeDecodeError, fmt::format(
            "Failed to read ModelPool: Error {}",
            static_cast<std::underlying_type_t<bitsery::ReaderError>>(s.adapter().error())));
    }

    // Hydrate all identities before making any schema-bearing value available to callers.
    if ((impl_->directSchema_ && (impl_->schemaIdentities_.size() != 1 || !impl_->importedSchemas_.empty())) ||
        impl_->importedSchemas_.size() > MaxSchemaId)
        return tl::unexpected(Error{Error::EncodeDecodeError, "Invalid model schema namespace"});
    std::vector<std::shared_ptr<const SchemaBinding>> bindings;
    for (auto const& identity : impl_->schemaIdentities_) {
        auto binding = schemaBinding(identity);
        if (identity.empty() || !binding)
            return tl::unexpected(Error{Error::EncodeDecodeError, "Missing or mismatched schema binding: " + identity});
        if (std::ranges::any_of(bindings, [&](auto const& previous) { return previous->identity() == identity; }))
            return tl::unexpected(Error{Error::EncodeDecodeError, "Duplicate model schema identity"});
        bindings.push_back(std::move(binding));
    }
    impl_->schemaBindings_ = std::move(bindings);
    for (size_t i = 0; i < impl_->importedSchemas_.size(); ++i) {
        auto domain = schemaReference(static_cast<SchemaId>(i + 1));
        if (!domain.definition())
            return tl::unexpected(Error{Error::EncodeDecodeError, "Invalid imported schema domain"});
    }

    auto validDomain = [&](SchemaId id, ValueType type) {
        auto definition = schemaReference(id).definition();
        return id == NoSchemaId || (definition && definition->hasAffinity(type));
    };
    if (!impl_->schemaBindings_.empty()) {
        for (auto const* column : {&impl_->columns_.objectSchemas_, &impl_->columns_.objectSingletonSchemas_,
                                  &impl_->columns_.arraySchemas_, &impl_->columns_.arraySingletonSchemas_})
            for (size_t i = 0; i < column->size(); ++i)
                if (!validDomain((*column)[i], column == &impl_->columns_.objectSchemas_ ||
                    column == &impl_->columns_.objectSingletonSchemas_ ? ValueType::Object : ValueType::Array))
                    return tl::unexpected(Error{Error::EncodeDecodeError, "Invalid container schema domain"});
    }
    for (auto const* column : {&impl_->columns_.strings_, &impl_->columns_.byteArrays_})
        for (size_t i = 0; i < column->size(); ++i) {
            auto const& range = (*column)[i];
            if (range.offset_ > impl_->columns_.stringData_.size() ||
                range.length_ > impl_->columns_.stringData_.size() - range.offset_)
                return tl::unexpected(Error{Error::EncodeDecodeError, "Scalar string/byte range exceeds model storage"});
        }
    for (size_t i = 0; i < impl_->columns_.qualifiedScalars_.size(); ++i) {
        auto entry = impl_->columns_.qualifiedScalars_[i];
        auto raw = entry.first();
        auto type = ValueType::Undef;
        bool inBounds = false;
        // Reads also run inside derived pool constructors, before shared_from_this is available.
        // Inspect primitive storage directly; resolving an owning node here would throw bad_weak_ptr.
        switch (raw.column()) {
        case UInt16:
        case Int16:
            type = ValueType::Int;
            inBounds = raw.index() <= std::numeric_limits<uint16_t>::max();
            break;
        case Bool:
            type = ValueType::Bool;
            inBounds = raw.index() <= 1;
            break;
        case Int64:
            type = ValueType::Int;
            inBounds = raw.index() < impl_->columns_.i64_.size();
            break;
        case Double:
            type = ValueType::Float;
            inBounds = raw.index() < impl_->columns_.double_.size();
            break;
        case String:
            type = ValueType::String;
            inBounds = raw.index() < impl_->columns_.strings_.size();
            break;
        case PooledString:
            type = ValueType::String;
            // The separately transported dictionary need not have been restored yet.
            inBounds = raw.index() <= std::numeric_limits<StringId>::max();
            break;
        case ByteArray:
            type = ValueType::Bytes;
            inBounds = raw.index() < impl_->columns_.byteArrays_.size();
            break;
        default:
            break;
        }
        auto definition = schemaReference(entry.second()).definition();
        if (!inBounds || !definition)
            return tl::unexpected(Error{Error::EncodeDecodeError, "Invalid qualified scalar reference"});
        if (!definition->hasAffinity(type))
            return tl::unexpected(Error{Error::EncodeDecodeError, "Qualified scalar has an incompatible domain"});
    }
    for (size_t i = 0; i < impl_->columns_.bitmasks_.size(); ++i) {
        auto entry = impl_->columns_.bitmasks_[i];
        auto definition = schemaReference(entry.second()).definition();
        if (!definition || !definition->isBitmask() ||
            definition->bitWidth() == 0 || definition->bitWidth() > 64 ||
            (definition->bitWidth() < 64 && (entry.first() >> definition->bitWidth()) != 0))
            return tl::unexpected(Error{Error::EncodeDecodeError, "Invalid bitmask domain or width"});
    }

    return {};
}

#if defined(SIMFIL_WITH_MODEL_JSON)
nlohmann::json ModelPool::toJson() const
{
    auto roots = nlohmann::json::array();
    const auto n = numRoots();
    for (auto i = 0u; i < n; ++i) {
        if (auto node = root(i))
            roots.push_back(node.value()->toJson());
    }

    return roots;
}
#endif

}  // namespace simfil
