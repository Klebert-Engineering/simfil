#include "simfil/model/model.h"
#include "simfil/model/schema-pool.h"
#include "simfil/model/arena.h"
#include "simfil/model/bitsery-traits.h"
#include "simfil/model/nodes.h"
#include "simfil/byte-array.h"
#include "simfil/value.h"

#include <memory>
#include <new>
#include <type_traits>
#include <variant>
#include <vector>
#include <deque>
#include <streambuf>
#include <unordered_set>

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

namespace simfil
{

tl::expected<void, Error> Model::resolve(const ModelNode& n, const ResolveFn& cb) const
{
    // Reuse the node's strong owner instead of locking enable_shared_from_this
    // for every temporary wrapper. Returned wrappers still own the model.
    auto retainOwner = [this, &n] {
        return n.model_.get() == this ? n.model_ : shared_from_this();
    };

    switch (n.addr_.column()) {
        case Null:
        {
            cb(ModelNodeBase(retainOwner(), ModelNodeAddress{}, mpKey_));
            break;
        }
        case UInt16:
        {
            cb(SmallValueNode<uint16_t>(retainOwner(), n.addr_, mpKey_));
            break;
        }
        case Int16:
        {
            cb(SmallValueNode<int16_t>(retainOwner(), n.addr_, mpKey_));
            break;
        }
        case Bool:
        {
            cb(SmallValueNode<bool>(retainOwner(), n.addr_, mpKey_));
            break;
        }
        case Scalar:
        {
            cb(ValueNode(n, mpKey_));
            break;
        }
        case Undefined:
        {
            cb(UndefinedNode(retainOwner(), n.addr_, mpKey_));
            break;
        }
        default:
            return tl::unexpected<Error>(Error::RuntimeError,
                                         fmt::format("Bad column reference: col={}", (uint16_t)n.addr_.column()));
    }
    return {};
}

/** Core value columns plus one model-local mapping from stored TypeIds to exact schema owners. */
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

    // Keep both the pool handle and its control block away from other models' hot counters.
    // Libraries without the interference-size constant use a 64-byte cache line.
#if defined(__cpp_lib_hardware_interference_size)
    static constexpr auto SchemaOwnerAlignment = std::hardware_destructive_interference_size;
#else
    static constexpr auto SchemaOwnerAlignment = size_t{64};
#endif

    /** Retain the same pool through a private per-model counter, never another schema facade. */
    struct alignas(SchemaOwnerAlignment) SchemaOwner {
        std::shared_ptr<const SchemaPool> owner;

        /** Release the graph with the last strong reference, even if weak observers remain. */
        void operator()(SchemaPool const*) noexcept { owner.reset(); }
    };

    /** Isolate hot Value ownership from other models sharing the same immutable graph. */
    static auto localSchemaPool(std::shared_ptr<const SchemaPool> const& pool)
        -> std::shared_ptr<const SchemaPool>
    {
        // Imports/readers may receive another model's group. Retain its original
        // owner directly, not a chain of per-model counters. Preserve alias pointers.
        auto* previous = std::get_deleter<SchemaOwner>(pool);
        return {pool.get(), SchemaOwner{previous ? previous->owner : pool}};
    }

    /// This model pool's field name store
    std::shared_ptr<StringPool> strings_;
    bool frozen_ = false;

    // Runtime graph slots are pointer-qualified; identities are only transport keys.
    bool directSchema_ = false;
    std::vector<std::string> schemaIdentities_;
    std::vector<SchemaOwner> schemaPools_;
    ModelColumn<TwoPart<std::uint16_t, TypeId>> importedSchemas_;
    SchemaResolver schemaResolver_;

    struct {
        ModelColumn<ModelNodeAddress, detail::ColumnPageSize> roots_;
        ModelColumn<int64_t, detail::ColumnPageSize> i64_;
        ModelColumn<double, detail::ColumnPageSize> double_;

        std::string stringData_;
        ModelColumn<StringRange, detail::ColumnPageSize> strings_;
        ModelColumn<StringRange, detail::ColumnPageSize> byteArrays_;
        ModelColumn<TwoPart<ModelNodeAddress, TypeId>> typedScalars_;
        ModelColumn<TwoPart<std::uint64_t, TypeId>> bitmasks_;

        ModelColumn<TypeId, detail::ColumnPageSize> objectSchemas_;
        ModelColumn<TypeId, detail::ColumnPageSize> objectSingletonSchemas_;
        ModelColumn<TypeId, detail::ColumnPageSize> arraySchemas_;
        ModelColumn<TypeId, detail::ColumnPageSize> arraySingletonSchemas_;
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

    /** Shared core wire order; schema graphs and the field dictionary travel separately. */
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
        s.object(columns_.typedScalars_);
        s.object(columns_.bitmasks_);
        s.boolValue(directSchema_);
        s.container(schemaIdentities_, available(s, size_t(MaxTypeId)), [](auto& serializer, auto& identity) {
            serializer.text1b(identity, available(serializer, size_t{65535}));
        });
        s.object(importedSchemas_);
    }
};

/** A column-backed scalar view: typed scalars delegate primitive access, masks retain all raw bits. */
class ModelPool::ScalarNode final : public ModelNodeBase
{
public:
    /** Retain the same persistent address/owner without allocating a separate scalar object. */
    ScalarNode(ModelPool const& owner, ModelNode const& node)
        : ModelNodeBase(node, owner.mpKey_), owner_(owner) {}

    /** Both scalar columns carry pool-local schema ids interpreted by their common bindings. */
    auto schema() const -> TypeId override
    {
        return addr().column() == Bitmask ? owner_.impl_->columns_.bitmasks_[addr().index()].second()
            : owner_.impl_->columns_.typedScalars_[addr().index()].second();
    }

    /** Owning mask values can outlive the model; ordinary scalar payloads retain their primitive form. */
    auto value() const -> ScalarValueType override
    {
        if (addr().column() == Bitmask)
            return simfil::Bitmask{owner_.impl_->columns_.bitmasks_[addr().index()].first(),
                                  owner_.resolveSchemaType(schema())};
        return owner_.resolve(owner_.impl_->columns_.typedScalars_[addr().index()].first())->value();
    }

    /** Schema annotations preserve the primitive type and its arithmetic behavior. */
    auto type() const -> ValueType override
    {
        return addr().column() == Bitmask ? ValueType::Bitmask
            : owner_.resolve(owner_.impl_->columns_.typedScalars_[addr().index()].first())->type();
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

auto ModelPool::scalarAppendCheckpoint() const -> ScalarAppendCheckpoint
{
    requireMutable();
    auto const& columns = impl_->columns_;
    ScalarAppendCheckpoint checkpoint;
    checkpoint.owner_ = this;
    checkpoint.roots = columns.roots_.size();
    checkpoint.int64Values = columns.i64_.size();
    checkpoint.doubleValues = columns.double_.size();
    checkpoint.stringBytes = columns.stringData_.size();
    checkpoint.stringRows = columns.strings_.size();
    checkpoint.byteArrayRows = columns.byteArrays_.size();
    return checkpoint;
}

void ModelPool::rollbackScalarAppends(ScalarAppendCheckpoint const& checkpoint) noexcept
{
    assert(checkpoint.owner_ == this && !frozen());
    auto& columns = impl_->columns_;
    assert(checkpoint.roots <= columns.roots_.size());
    assert(checkpoint.int64Values <= columns.i64_.size());
    assert(checkpoint.doubleValues <= columns.double_.size());
    assert(checkpoint.stringBytes <= columns.stringData_.size());
    assert(checkpoint.stringRows <= columns.strings_.size());
    assert(checkpoint.byteArrayRows <= columns.byteArrays_.size());

    columns.roots_.resize(checkpoint.roots);
    columns.i64_.resize(checkpoint.int64Values);
    columns.double_.resize(checkpoint.doubleValues);
    // Drop row references before trimming shared string/byte-array payload.
    columns.strings_.resize(checkpoint.stringRows);
    columns.byteArrays_.resize(checkpoint.byteArrayRows);
    columns.stringData_.resize(checkpoint.stringBytes);
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

    std::function<void(ModelNode::Ptr, bool)> validateModelNode = [&](ModelNode::Ptr node, bool requireCoreSchema)
    {
        if (node->type() == ValueType::Object) {
            // Custom object columns own their schema storage; their indices are not ours.
            if (node->addr().column() == Objects) {
                if (!validateArrayIndex(node->addr().index(), "object", impl_->columns_.objectMemberArrays_))
                    return;
                if (requireCoreSchema && !objectHasSchema(node->addr().index())) {
                    errors.emplace_back(fmt::format("Missing object schema index {}.", node->addr().index()));
                    return;
                }
            }
            for (auto const& [fieldName, fieldValue] : node->fields()) {
                validatePooledString(fieldName);
                validateModelNode(fieldValue, true);
            }
        }
        else if (node->type() == ValueType::Array) {
            // Custom arrays still have their children checked, but need no core arena/schema entry.
            if (node->addr().column() == Arrays) {
                if (!validateArrayIndex(node->addr().index(), "arrays", impl_->columns_.arrayMemberArrays_))
                    return;
                if (requireCoreSchema && !arrayHasSchema(node->addr().index())) {
                    errors.emplace_back(fmt::format("Missing array schema index {}.", node->addr().index()));
                    return;
                }
            }
            for (auto const& member : *node)
                validateModelNode(member, true);
        }
        else if (node->addr().column() == PooledString) {
            validatePooledString(static_cast<StringId>(node->addr().index()));
        }

        if (auto res = resolve(*node, Lambda([](auto&&) {})); !res)
            errors.emplace_back(res.error().message);
    };

    // Shared arenas also back custom columns without core schema sidecars.
    // Synthetic sweep nodes validate storage/members, not their invented core identity;
    // actual core child/root references still require metadata before following children.
    for (auto i = FirstRegularArrayIndex; i < impl_->columns_.objectMemberArrays_.size(); ++i)
        validateModelNode(ModelNode::Ptr::make(
            shared_from_this(),
            ModelNodeAddress{Objects, (uint32_t)i}), false);
    for (auto i = 0u; i < impl_->columns_.objectMemberArrays_.singleton_handle_count(); ++i)
        validateModelNode(ModelNode::Ptr::make(
            shared_from_this(),
            ModelNodeAddress{
                Objects,
                SingletonArrayHandleMask | static_cast<uint32_t>(i)}), false);

    // Validate arrays
    for (auto i = FirstRegularArrayIndex; i < impl_->columns_.arrayMemberArrays_.size(); ++i)
        validateModelNode(ModelNode::Ptr::make(
            shared_from_this(),
            ModelNodeAddress{Arrays, (uint32_t)i}), false);
    for (auto i = 0u; i < impl_->columns_.arrayMemberArrays_.singleton_handle_count(); ++i)
        validateModelNode(ModelNode::Ptr::make(
            shared_from_this(),
            ModelNodeAddress{
                Arrays,
                SingletonArrayHandleMask | static_cast<uint32_t>(i)}), false);

    // Validate roots
    for (auto i = 0; i < numRoots(); ++i)
        if (auto node = root(i))
            validateModelNode(*node, true);

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
    clear_and_shrink(columns.typedScalars_);
    clear_and_shrink(columns.bitmasks_);
    clear_and_shrink(impl_->importedSchemas_);
    impl_->schemaPools_.clear();
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
    // Reuse the node's strong owner instead of locking enable_shared_from_this
    // for every temporary wrapper. Returned wrappers still own the model.
    auto retainOwner = [this, &n] {
        return n.model_.get() == this ? n.model_ : shared_from_this();
    };

    // Merged/container views can surface child nodes from another model. Always
    // let the owning model interpret its own column/index address.
    // The input node already retains this owner throughout resolution.
    if (auto* owner = n.model_.get(); owner && owner != this) {
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
        cb(Object(retainOwner(), n.addr_, mpKey_));
        break;
    }
    case Arrays: {
        cb(Array(retainOwner(), n.addr_, mpKey_));
        break;
    }
    case Int64: {
        if (auto err = checkBounds(impl_->columns_.i64_))
            return tl::unexpected<Error>(*err);
        auto idx = n.addr().index();
        auto& val = impl_->columns_.i64_[idx];
        cb(ValueNode(val, retainOwner(), mpKey_));
        break;
    }
    case Double: {
        if (auto err = checkBounds(impl_->columns_.double_))
            return tl::unexpected<Error>(*err);
        auto idx = n.addr().index();
        auto& val = impl_->columns_.double_[idx];
        cb(ValueNode(val, retainOwner(), mpKey_));
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
            retainOwner(),
            mpKey_));
        break;
    }
    case ByteArray: {
        auto idx = n.addr().index();
        if (auto err = checkBounds(impl_->columns_.byteArrays_))
            return tl::unexpected<Error>(*err);
        auto& val = impl_->columns_.byteArrays_[idx];
        auto view = std::string_view(impl_->columns_.stringData_).substr(val.offset_, val.length_);
        cb(ValueNode(simfil::ByteArray{view}, retainOwner(), mpKey_));
        break;
    }
    case PooledString: {
        auto str = lookupStringId(static_cast<StringId>(n.addr().index()));
        cb(ValueNode(str.value_or(std::string_view{}), retainOwner(), mpKey_));
        break;
    }
    case TypedScalar: {
        if (auto error = checkBounds(impl_->columns_.typedScalars_))
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
        impl_->columns_.objectSingletonSchemas_[singletonIndex] = TypeId{};
    } else {
        if (impl_->columns_.objectSchemas_.size() <= memberArrId)
            impl_->columns_.objectSchemas_.resize(memberArrId + 1);
        impl_->columns_.objectSchemas_[memberArrId] = TypeId{};
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
        impl_->columns_.arraySingletonSchemas_[singletonIndex] = TypeId{};
    } else {
        if (impl_->columns_.arraySchemas_.size() <= memberArrId)
            impl_->columns_.arraySchemas_.resize(memberArrId + 1);
        impl_->columns_.arraySchemas_[memberArrId] = TypeId{};
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

auto Model::resolveSchemaType(TypeId) const -> SchemaType
{
    return {};
}

auto Model::schema(ModelNode const& node) const -> TypeId
{
    TypeId result = NoTypeId;
    resolve(node, Lambda([&](auto const& resolved) { result = resolved.schema(); }));
    return result;
}

auto Model::type(ModelNode const& node) const -> ValueType
{
    auto result = ValueType::Null;
    resolve(node, Lambda([&](auto const& resolved) { result = resolved.type(); }));
    return result;
}

auto Model::resolveSchemaType(ModelNode const& node) const -> SchemaType
{
    if (auto* owner = node.model_.get(); owner && owner != this)
        return owner->resolveSchemaType(node);
    return resolveSchemaType(node.schema());
}

void ModelPool::setSchemaPool(std::shared_ptr<const SchemaPool> pool)
{
    requireMutable();
    if (!pool || !pool->finalized() || !pool->frozen())
        throw std::invalid_argument("A direct schema pool must be finalized and frozen");
    if (!impl_->schemaPools_.empty()) {
        if (impl_->directSchema_ && impl_->schemaPools_.front().owner.get() == pool.get())
            return;
        throw std::invalid_argument("Cannot replace a populated model schema namespace");
    }
    // Prepare both tails before publication; allocation failure cannot leave a half-installed graph.
    std::vector<std::string> identities{std::string(pool->identity())};
    std::vector<Impl::SchemaOwner> pools{{Impl::localSchemaPool(pool)}};
    impl_->schemaIdentities_.swap(identities);
    impl_->schemaPools_.swap(pools);
    impl_->directSchema_ = true;
}

void ModelPool::setSchemaResolver(SchemaResolver resolver)
{
    requireMutable();
    impl_->schemaResolver_ = std::move(resolver);
}

void ModelPool::remapSchemas(std::shared_ptr<const SchemaPool> target,
                            std::function<SchemaType(SchemaType const&)> const& remap)
{
    requireMutable();
    if (!target || !target->finalized() || !target->frozen() || !remap)
        throw std::invalid_argument("Schema remapping requires an immutable target pool and callback");

    // The table is bounded by the 16-bit local namespace, not the number of values.
    std::vector<TypeId> mapped(size_t(MaxTypeId) + 1, NoTypeId);
    auto validate = [&](TypeId id, ValueType type) -> SchemaType {
        if (id == NoTypeId)
            return {};
        if (mapped[id] == NoTypeId) {
            auto source = resolveSchemaType(id);
            if (!source)
                throw std::invalid_argument("Cannot remap an unresolved source type");
            auto replacement = remap(source);
            if (!replacement || replacement.pool() != target.get() ||
                !target->schemaType(replacement.id()))
                throw std::invalid_argument("Schema remapping must resolve into the target graph");
            mapped[id] = replacement.id();
        }
        auto definition = target->schemaType(mapped[id]);
        if (!definition.hasAffinity(type))
            throw std::invalid_argument("Remapped schema is incompatible with the stored value");
        return definition;
    };
    auto& columns = impl_->columns_;
    for (auto* column : {&columns.objectSchemas_, &columns.objectSingletonSchemas_,
                         &columns.arraySchemas_, &columns.arraySingletonSchemas_})
        for (size_t i = 0; i < column->size(); ++i)
            validate((*column)[i], column == &columns.objectSchemas_ ||
                column == &columns.objectSingletonSchemas_ ? ValueType::Object : ValueType::Array);
    for (size_t i = 0; i < columns.typedScalars_.size(); ++i) {
        auto value = columns.typedScalars_[i];
        if (!validate(value.second(), type(ModelNode(shared_from_this(), value.first(), mpKey_))))
            throw std::invalid_argument("A typed scalar must retain its type");
    }
    for (size_t i = 0; i < columns.bitmasks_.size(); ++i) {
        auto value = columns.bitmasks_[i];
        auto definition = validate(value.second(), ValueType::Bitmask);
        if (!definition || !definition.isBitmask() || !definition.bitWidth() ||
            definition.bitWidth() > 64 ||
            (definition.bitWidth() < 64 && (value.first() >> definition.bitWidth())))
            throw std::invalid_argument("Remapped bitmask type does not preserve its width");
    }

    // Allocate the new owners before changing any column. The commit below cannot throw.
    std::vector<std::string> identities{std::string(target->identity())};
    std::vector<Impl::SchemaOwner> bindings{{Impl::localSchemaPool(target)}};
    for (auto* column : {&columns.objectSchemas_, &columns.objectSingletonSchemas_,
                         &columns.arraySchemas_, &columns.arraySingletonSchemas_})
        for (size_t i = 0; i < column->size(); ++i)
            (*column)[i] = mapped[(*column)[i]];
    for (size_t i = 0; i < columns.typedScalars_.size(); ++i)
        columns.typedScalars_[i].second() = mapped[columns.typedScalars_[i].second()];
    for (size_t i = 0; i < columns.bitmasks_.size(); ++i)
        columns.bitmasks_[i].second() = mapped[columns.bitmasks_[i].second()];
    impl_->schemaIdentities_.swap(identities);
    impl_->schemaPools_.swap(bindings);
    impl_->importedSchemas_.clear();
    impl_->directSchema_ = true;
}

auto ModelPool::schemaPool(std::string_view identity) const -> std::shared_ptr<const SchemaPool>
{
    if (identity.empty())
        return {};
    std::shared_ptr<SchemaPool const> result;
    for (auto const& entry : impl_->schemaPools_) {
        if (!entry.owner || entry.owner->identity() != identity)
            continue;
        if (result && result.get() != entry.owner.get())
            return {};
        result = entry.owner;
    }
    if (!result && impl_->schemaResolver_)
        result = impl_->schemaResolver_(identity);
    return result && result->identity() == identity && result->finalized() && result->frozen()
        ? result : nullptr;
}

auto ModelPool::resolveSchemaType(TypeId id) const -> SchemaType
{
    if (id == NoTypeId || impl_->schemaPools_.empty())
        return {};
    if (impl_->directSchema_) {
        auto const& owner = impl_->schemaPools_.front().owner;
        return owner && owner->schemaType(id) ? SchemaType(owner, id) : SchemaType{};
    }
    if (size_t(id) > impl_->importedSchemas_.size())
        return {};
    auto entry = impl_->importedSchemas_[size_t(id) - 1];
    if (entry.first() >= impl_->schemaPools_.size())
        return {};
    auto const& owner = impl_->schemaPools_[entry.first()].owner;
    return owner && owner->schemaType(entry.second()) ? SchemaType(owner, entry.second()) : SchemaType{};
}

auto ModelPool::hasDirectSchemaPool(std::string_view identity) const noexcept -> bool
{
    return !identity.empty() && impl_->directSchema_ && impl_->schemaIdentities_.size() == 1 && impl_->schemaPools_.size() == 1 &&
        impl_->schemaIdentities_.front() == identity && impl_->schemaPools_.front().owner &&
        impl_->schemaPools_.front().owner->identity() == identity && impl_->importedSchemas_.empty();
}

auto ModelPool::resolveSchemaType(ModelNode const& node) const -> SchemaType
{
    if (auto* owner = node.model_.get(); owner && owner != this)
        return owner->resolveSchemaType(node);
    if (impl_->schemaPools_.empty())
        return {};
    return resolveSchemaType(node.schema());
}

auto ModelPool::schema(ModelNode const& node) const -> TypeId
{
    if (auto* owner = node.model_.get(); owner && owner != this)
        return owner->schema(node);
    // Core column ids already identify where the schema is stored. Resolving a
    // temporary node wrapper here would retain the model just to read this id.
    auto address = node.addr();
    switch (address.column()) {
    case Objects:
        return objectSchemaId(address.index());
    case Arrays:
        return arraySchemaId(address.index());
    case TypedScalar:
        return address.index() < impl_->columns_.typedScalars_.size()
            ? TypeId(impl_->columns_.typedScalars_[address.index()].second()) : NoTypeId;
    case Bitmask:
        return address.index() < impl_->columns_.bitmasks_.size()
            ? TypeId(impl_->columns_.bitmasks_[address.index()].second()) : NoTypeId;
    default:
        // Custom columns keep their model-specific schema protocol. Plain core
        // primitives have no occurrence-specific type outside their parent.
        return address.column() >= FirstCustomColumnId ? Model::schema(node) : NoTypeId;
    }
}

auto ModelPool::type(ModelNode const& node) const -> ValueType
{
    if (auto* owner = node.model_.get(); owner && owner != this)
        return owner->type(node);
    auto address = node.addr();
    auto index = address.index();
    switch (address.column()) {
    case Null: return ValueType::Null;
    case Undefined: return ValueType::Undef;
    case Bool: return ValueType::Bool;
    case UInt16:
    case Int16: return ValueType::Int;
    case Objects: return ValueType::Object;
    case Arrays: return ValueType::Array;
    // Match resolve()'s invalid-reference fallback without reading the payload.
    case Int64: return index < impl_->columns_.i64_.size() ? ValueType::Int : ValueType::Null;
    case Double: return index < impl_->columns_.double_.size() ? ValueType::Float : ValueType::Null;
    case String: return index < impl_->columns_.strings_.size() ? ValueType::String : ValueType::Null;
    case PooledString: return ValueType::String;
    case ByteArray: return index < impl_->columns_.byteArrays_.size() ? ValueType::Bytes : ValueType::Null;
    case Bitmask: return index < impl_->columns_.bitmasks_.size() ? ValueType::Bitmask : ValueType::Null;
    default:
        // Typed scalars and custom columns can define their own primitive
        // representation; their node protocol remains the authority.
        return Model::type(node);
    }
}

auto ModelPool::importSchema(SchemaType const& typeRef) -> tl::expected<TypeId, Error>
{
    if (frozen())
        return tl::unexpected(Error{Error::RuntimeError, "Cannot import a schema into a frozen ModelPool"});
    if (!typeRef || !typeRef.pool()->finalized() || !typeRef.pool()->frozen())
        return tl::unexpected(Error{Error::RuntimeError, "Cannot import a type without an immutable schema pool"});
    if (impl_->directSchema_) {
        if (typeRef.pool() != impl_->schemaPools_.front().owner.get())
            return tl::unexpected(Error{Error::RuntimeError, "Foreign type in a single-graph model"});
        return typeRef.id();
    }
    auto owner = std::ranges::find_if(impl_->schemaPools_, [&](auto const& entry) {
        return entry.owner.get() == typeRef.pool();
    });
    auto poolIndex = size_t(owner - impl_->schemaPools_.begin());
    for (size_t i = 0; i < impl_->importedSchemas_.size(); ++i) {
        auto entry = impl_->importedSchemas_[i];
        if (entry.first() == poolIndex && entry.second() == typeRef.id())
            return static_cast<TypeId>(i + 1);
    }
    if (impl_->importedSchemas_.size() >= MaxTypeId || poolIndex >= MaxTypeId)
        return tl::unexpected(Error{Error::RuntimeError, "Model schema namespace exhausted"});
    auto newPool = owner == impl_->schemaPools_.end();
    if (newPool) {
        // ModelPool is a private SchemaType friend; unwrap a source model's local
        // counter while retaining the identical pool, not its producer model.
        auto local = Impl::localSchemaPool(typeRef.retain().owner_);
        std::string identity(typeRef.pool()->identity());
        impl_->schemaIdentities_.reserve(poolIndex + 1);
        impl_->schemaPools_.reserve(poolIndex + 1);
        impl_->schemaIdentities_.push_back(std::move(identity));
        impl_->schemaPools_.push_back({std::move(local)});
    }
    try {
        impl_->importedSchemas_.emplace_back(static_cast<std::uint16_t>(poolIndex), typeRef.id());
    }
    catch (...) {
        if (newPool) {
            impl_->schemaPools_.pop_back();
            impl_->schemaIdentities_.pop_back();
        }
        throw;
    }
    return static_cast<TypeId>(impl_->importedSchemas_.size());
}

auto ModelPool::newBitmask(std::uint64_t bits, SchemaType const& typeRef) -> ModelNode::Ptr
{
    requireMutable();
    auto const& definition = typeRef;
    if (!definition || !definition.isBitmask() ||
        definition.bitWidth() == 0 || definition.bitWidth() > 64)
        throw std::invalid_argument("Bitmask requires a compatible bound type and width");
    if (definition.bitWidth() < 64 && (bits >> definition.bitWidth()) != 0)
        throw std::invalid_argument("Bitmask exceeds its declared encoding width");
    auto index = impl_->columns_.bitmasks_.size();
    // These typed factories are input boundaries, unlike the trusted primitive builders.
    if (!ModelNodeAddress::checkIndex(index))
        throw std::length_error("Bitmask column exceeds its 24-bit index representation");
    auto imported = importSchema(typeRef);
    if (!imported)
        throw std::invalid_argument(imported.error().message);
    impl_->columns_.bitmasks_.emplace_back(bits, *imported);
    return resolve(ModelNodeAddress{Bitmask, index});
}

auto ModelPool::newTypedValue(ScalarValueType value, SchemaType const& typeRef) -> ModelNode::Ptr
{
    requireMutable();
    if (std::holds_alternative<std::monostate>(value))
        throw std::invalid_argument("Null is not a schema-typed primitive");
    if (auto mask = std::get_if<simfil::Bitmask>(&value)) {
        if (mask->type != typeRef)
            throw std::invalid_argument("Cannot rebind a bitmask to a different type");
        return newBitmask(mask->bits, typeRef);
    }
    auto type = std::visit([](auto const& scalar) {
        return ValueType4CType<std::decay_t<decltype(scalar)>>::Type;
    }, value);
    if (!typeRef || !typeRef.hasAffinity(type))
        throw std::invalid_argument("Typed scalar requires a compatible type");
    auto index = impl_->columns_.typedScalars_.size();
    if (!ModelNodeAddress::checkIndex(index))
        throw std::length_error("Typed scalar column exceeds its 24-bit index representation");
    auto imported = importSchema(typeRef);
    if (!imported)
        throw std::invalid_argument(imported.error().message);
    auto raw = std::visit([&](auto const& scalar) -> tl::expected<ModelNode::Ptr, Error> {
        using T = std::decay_t<decltype(scalar)>;
        if constexpr (std::is_same_v<T, std::monostate>)
            return resolve(ModelNodeAddress{Null, 1});
        else if constexpr (std::is_same_v<T, bool>)
            return newSmallValue(scalar);
        else if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, std::string_view>)
            return tryNewValue(std::string_view(scalar));
        else if constexpr (std::is_same_v<T, simfil::Bitmask>)
            return newBitmask(scalar.bits, scalar.type);
        else
            return tryNewValue(scalar);
    }, value);
    if (!raw)
        throw std::length_error(raw.error().message);
    impl_->columns_.typedScalars_.emplace_back((*raw)->addr(), *imported);
    return resolve(ModelNodeAddress{TypedScalar, index});
}

ModelNode::Ptr ModelPool::newValue(int64_t const& value)
{
    requireMutable();
    if (value < 0 && value >= std::numeric_limits<int16_t>::min())
        return newSmallValue((int16_t)value);
    else if (value >= 0 && value <= std::numeric_limits<uint16_t>::max())
        return newSmallValue((uint16_t)value);
    assert(ModelNodeAddress::checkIndex(impl_->columns_.i64_.size()));
    impl_->columns_.i64_.emplace_back(value);
    return ModelNode::Ptr::make(
        shared_from_this(),
        ModelNodeAddress{Int64, (uint32_t)impl_->columns_.i64_.size()-1});
}

ModelNode::Ptr ModelPool::newValue(double const& value)
{
    requireMutable();
    assert(ModelNodeAddress::checkIndex(impl_->columns_.double_.size()));
    impl_->columns_.double_.emplace_back(value);
    return ModelNode::Ptr::make(
        shared_from_this(),
        ModelNodeAddress{Double, (uint32_t)impl_->columns_.double_.size()-1});
}

ModelNode::Ptr ModelPool::newValue(std::string_view const& value)
{
    requireMutable();
    assert(ModelNodeAddress::checkIndex(impl_->columns_.strings_.size()));
    assert(check_model_column_growth(impl_->columns_.stringData_.size(), value.size(), 1));
    auto const offset = impl_->columns_.stringData_.size();
    // Append preserves aliased input and geometric growth. Exact-size reserve
    // here invalidates source views and causes repeated copying with libc++.
    // Publish the range only after its bytes exist.
    impl_->columns_.stringData_ += value;
    impl_->columns_.strings_.emplace_back(Impl::StringRange{
        (uint32_t)offset,
        (uint32_t)value.size()
    });
    return ModelNode::Ptr::make(
        shared_from_this(),
        ModelNodeAddress{String, (uint32_t)impl_->columns_.strings_.size()-1});
}

ModelNode::Ptr ModelPool::newValue(simfil::ByteArray const& value)
{
    requireMutable();
    assert(ModelNodeAddress::checkIndex(impl_->columns_.byteArrays_.size()));
    assert(check_model_column_growth(impl_->columns_.stringData_.size(), value.bytes.size(), 1));
    auto const offset = impl_->columns_.stringData_.size();
    impl_->columns_.stringData_.append(value.bytes.data(), value.bytes.size());
    impl_->columns_.byteArrays_.emplace_back(Impl::StringRange{
        (uint32_t)offset,
        (uint32_t)value.bytes.size()
    });
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

auto ModelPool::tryNewObject(size_t initialFieldCapacity, bool fixedSize)
    -> tl::expected<model_ptr<Object>, Error>
{
    if (frozen())
        return tl::unexpected<Error>(Error::RuntimeError, "Cannot mutate a frozen ModelPool");
    if (!objectMemberStorage().check_capacity(initialFieldCapacity, fixedSize))
        return tl::unexpected<Error>(Error::IndexOutOfRange, "Object capacity exceeds model storage representation");
    return newObject(initialFieldCapacity, fixedSize);
}

auto ModelPool::tryNewArray(size_t initialFieldCapacity, bool fixedSize)
    -> tl::expected<model_ptr<Array>, Error>
{
    if (frozen())
        return tl::unexpected<Error>(Error::RuntimeError, "Cannot mutate a frozen ModelPool");
    if (!arrayMemberStorage().check_capacity(initialFieldCapacity, fixedSize))
        return tl::unexpected<Error>(Error::IndexOutOfRange, "Array capacity exceeds model storage representation");
    return newArray(initialFieldCapacity, fixedSize);
}

auto ModelPool::tryNewValue(int64_t const& value) -> tl::expected<ModelNode::Ptr, Error>
{
    if (frozen())
        return tl::unexpected<Error>(Error::RuntimeError, "Cannot mutate a frozen ModelPool");
    bool const inlineValue = value >= std::numeric_limits<int16_t>::min() &&
        value <= std::numeric_limits<uint16_t>::max();
    if (!inlineValue && !ModelNodeAddress::checkIndex(impl_->columns_.i64_.size()))
        return tl::unexpected<Error>(Error::IndexOutOfRange, "Integer column exceeds its 24-bit index representation");
    return newValue(value);
}

auto ModelPool::tryNewValue(double const& value) -> tl::expected<ModelNode::Ptr, Error>
{
    if (frozen())
        return tl::unexpected<Error>(Error::RuntimeError, "Cannot mutate a frozen ModelPool");
    if (!ModelNodeAddress::checkIndex(impl_->columns_.double_.size()))
        return tl::unexpected<Error>(Error::IndexOutOfRange, "Double column exceeds its 24-bit index representation");
    return newValue(value);
}

auto ModelPool::tryNewValue(std::string_view const& value) -> tl::expected<ModelNode::Ptr, Error>
{
    if (frozen())
        return tl::unexpected<Error>(Error::RuntimeError, "Cannot mutate a frozen ModelPool");
    if (!ModelNodeAddress::checkIndex(impl_->columns_.strings_.size()) ||
        !check_model_column_growth(impl_->columns_.stringData_.size(), value.size(), 1))
        return tl::unexpected<Error>(Error::IndexOutOfRange, "String value exceeds model storage representation");
    return newValue(value);
}

auto ModelPool::tryNewValue(simfil::ByteArray const& value) -> tl::expected<ModelNode::Ptr, Error>
{
    if (frozen())
        return tl::unexpected<Error>(Error::RuntimeError, "Cannot mutate a frozen ModelPool");
    if (!ModelNodeAddress::checkIndex(impl_->columns_.byteArrays_.size()) ||
        !check_model_column_growth(impl_->columns_.stringData_.size(), value.bytes.size(), 1))
        return tl::unexpected<Error>(Error::IndexOutOfRange, "Byte array exceeds model storage representation");
    return newValue(value);
}

auto ModelPool::tryNewValue(StringId handle) -> tl::expected<ModelNode::Ptr, Error>
{
    if (frozen())
        return tl::unexpected<Error>(Error::RuntimeError, "Cannot mutate a frozen ModelPool");
    if (!ModelNodeAddress::checkIndex(handle))
        return tl::unexpected<Error>(Error::IndexOutOfRange, "String ID exceeds its 24-bit index representation");
    return newValue(handle);
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
    stats.typedScalarBytes = impl_->columns_.typedScalars_.byte_size();
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
    stats.typedScalars = impl_->columns_.typedScalars_.memory_usage();
    stats.bitmasks = impl_->columns_.bitmasks_.memory_usage();
    stats.schemaOwners = impl_->importedSchemas_.memory_usage();
    // Include padded handles/deleters, but not implementation-specific control-block bookkeeping.
    stats.schemaOwners.allocatedBytes += impl_->schemaPools_.capacity() * sizeof(Impl::SchemaOwner)
        + impl_->schemaIdentities_.capacity() * sizeof(std::string)
        + impl_->schemaPools_.size() * sizeof(Impl::SchemaOwner);
    for (auto const& identity : impl_->schemaIdentities_) {
        stats.schemaOwners.logicalBytes += identity.size();
        stats.schemaOwners.allocatedBytes += identity.capacity();
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

TypeId ModelPool::objectSchemaId(ArrayIndex members) const
{
    if (Object::Storage::is_singleton_handle(members)) {
        auto singletonIndex = Object::Storage::singleton_payload(members);
        if (singletonIndex >= impl_->columns_.objectSingletonSchemas_.size())
            return {};
        return TypeId{impl_->columns_.objectSingletonSchemas_[singletonIndex]};
    }
    if (members >= impl_->columns_.objectSchemas_.size())
        return {};
    return TypeId{impl_->columns_.objectSchemas_[members]};
}

auto ModelPool::setObjectSchemaId(ArrayIndex members, TypeId schemaId) -> tl::expected<void, Error>
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

TypeId ModelPool::arraySchemaId(ArrayIndex members) const
{
    if (Array::Storage::is_singleton_handle(members)) {
        auto singletonIndex = Array::Storage::singleton_payload(members);
        if (singletonIndex >= impl_->columns_.arraySingletonSchemas_.size())
            return {};
        return TypeId{impl_->columns_.arraySingletonSchemas_[singletonIndex]};
    }
    if (members >= impl_->columns_.arraySchemas_.size())
        return {};
    return TypeId{impl_->columns_.arraySchemas_[members]};
}

auto ModelPool::setArraySchemaId(ArrayIndex members, TypeId schemaId) -> tl::expected<void, Error>
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
    if (impl_->schemaPools_.size() != impl_->schemaIdentities_.size())
        return tl::unexpected(Error{Error::EncodeDecodeError, "Invalid model schema-owner table"});
    // Anonymous or ambiguous graphs are valid in-memory owners, but a string-keyed
    // resolver cannot reproduce them. Reject before writing any payload bytes.
    std::unordered_set<std::string_view> identities;
    for (size_t i = 0; i < impl_->schemaPools_.size(); ++i) {
        auto const& owner = impl_->schemaPools_[i].owner;
        auto const& identity = impl_->schemaIdentities_[i];
        if (!owner || identity.empty() || identity.size() > std::numeric_limits<uint16_t>::max() ||
            owner->identity() != identity || !owner->finalized() || !owner->frozen())
            return tl::unexpected(Error{Error::EncodeDecodeError, "Model schema transport requires an immutable named pool"});
        if (!identities.insert(identity).second)
            return tl::unexpected(Error{Error::EncodeDecodeError, "Ambiguous model schema artifact identity"});
    }
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

    // Resolve all serialized graph identities before accepting stored type IDs.
    // A failed read leaves this construction-time pool unusable by its caller.
    if ((impl_->directSchema_ && (impl_->schemaIdentities_.size() != 1 || !impl_->importedSchemas_.empty())) ||
        impl_->importedSchemas_.size() > MaxTypeId)
        return tl::unexpected(Error{Error::EncodeDecodeError, "Invalid model schema namespace"});
    std::vector<Impl::SchemaOwner> pools;
    std::unordered_set<std::string_view> identities;
    for (auto const& identity : impl_->schemaIdentities_) {
        auto pool = schemaPool(identity);
        if (identity.empty() || !pool)
            return tl::unexpected(Error{Error::EncodeDecodeError, "Missing or mismatched schema pool: " + identity});
        if (!identities.insert(identity).second)
            return tl::unexpected(Error{Error::EncodeDecodeError, "Duplicate model schema identity"});
        pools.push_back({Impl::localSchemaPool(pool)});
    }
    impl_->schemaPools_ = std::move(pools);
    for (size_t i = 0; i < impl_->importedSchemas_.size(); ++i) {
        auto typeRef = resolveSchemaType(static_cast<TypeId>(i + 1));
        if (!typeRef)
            return tl::unexpected(Error{Error::EncodeDecodeError, "Invalid imported schema type"});
    }

    // A valid numeric ID must also describe the runtime column's value kind.
    // This catches bindings that resolve successfully to an incompatible graph.
    auto validType = [&](TypeId id, ValueType type) {
        auto definition = resolveSchemaType(id);
        return id == NoTypeId || (definition && definition.hasAffinity(type));
    };
    if (!impl_->schemaPools_.empty()) {
        for (auto const* column : {&impl_->columns_.objectSchemas_, &impl_->columns_.objectSingletonSchemas_,
                                  &impl_->columns_.arraySchemas_, &impl_->columns_.arraySingletonSchemas_})
            for (size_t i = 0; i < column->size(); ++i)
                if (!validType((*column)[i], column == &impl_->columns_.objectSchemas_ ||
                    column == &impl_->columns_.objectSingletonSchemas_ ? ValueType::Object : ValueType::Array))
                    return tl::unexpected(Error{Error::EncodeDecodeError, "Invalid container schema type"});
    }
    for (auto const* column : {&impl_->columns_.strings_, &impl_->columns_.byteArrays_})
        for (size_t i = 0; i < column->size(); ++i) {
            auto const& range = (*column)[i];
            if (range.offset_ > impl_->columns_.stringData_.size() ||
                range.length_ > impl_->columns_.stringData_.size() - range.offset_)
                return tl::unexpected(Error{Error::EncodeDecodeError, "Scalar string/byte range exceeds model storage"});
        }
    for (size_t i = 0; i < impl_->columns_.typedScalars_.size(); ++i) {
        auto entry = impl_->columns_.typedScalars_[i];
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
        auto definition = resolveSchemaType(entry.second());
        if (!inBounds || !definition)
            return tl::unexpected(Error{Error::EncodeDecodeError, "Invalid typed scalar reference"});
        if (!definition.hasAffinity(type))
            return tl::unexpected(Error{Error::EncodeDecodeError, "Typed scalar has an incompatible type"});
    }
    for (size_t i = 0; i < impl_->columns_.bitmasks_.size(); ++i) {
        auto entry = impl_->columns_.bitmasks_[i];
        auto definition = resolveSchemaType(entry.second());
        if (!definition || !definition.isBitmask() ||
            definition.bitWidth() == 0 || definition.bitWidth() > 64 ||
            (definition.bitWidth() < 64 && (entry.first() >> definition.bitWidth()) != 0))
            return tl::unexpected(Error{Error::EncodeDecodeError, "Invalid bitmask type or width"});
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
