#pragma once

#include "simfil/model/model.h"
#include "simfil/model/schema-type.h"

namespace simfil
{

class SchemaPool;
class SchemaTypeModelNode;

/**
 * Resolve canonical type-root addresses as typed descriptor handles. Declare
 * this specialization before its friendship below, so that declaration cannot
 * instantiate the deleted primary resolution template.
 */
template <>
auto resolveInternal(res::tag<SchemaTypeModelNode>, SchemaPool const& model, ModelNode const& node)
    -> model_ptr<SchemaTypeModelNode>;

/** Owning metadata-node view over canonical schema records, never a copied descriptor tree.
 * Type descriptors are Objects regardless of their represented kind. Child
 * addresses identify fields, declarations and containers in the same pool. */
class SchemaTypeModelNode : public ModelNodeBase
{
public:
    using ModelType = SchemaPool;

    /** Empty construction is reserved for model_ptr's absent-value representation. */
    explicit SchemaTypeModelNode(detail::mp_key key) : ModelNodeBase(key) {}

    /** Bind an already validated canonical descriptor address to its retaining owner. */
    SchemaTypeModelNode(ModelConstPtr owner, ModelNodeAddress address, detail::mp_key key)
        : ModelNodeBase(std::move(owner), address, key) {}

    /** Borrow the represented type; metadata containers and ambiguous fields have no unique type. */
    auto describedType() const -> SchemaType;

    /** Describe represented semantics without confusing them with the metadata container type. */
    auto kind() const -> SchemaType::Kind;

    /** Report Object for descriptors and Array for ordered metadata collections. */
    auto type() const -> ValueType override;

    /** Metadata is not runtime feature data and has no attached feature type. */
    auto schema() const -> TypeId override { return NoTypeId; }

    /** Count immediate members without creating child nodes. */
    auto size() const -> uint32_t override;

    /** Enumerate object keys in descriptor order without allocating a key collection. */
    auto keyAt(int64_t position) const -> StringId override;

    /** Look up metadata, not fields of the represented runtime value. */
    auto get(StringId const& field) const -> ModelNode::Ptr override;

    /** Resolve one canonical child without changing future navigation or cycle behavior. */
    auto at(int64_t position) const -> ModelNode::Ptr override;

    /** Visit immediate metadata, preserving source order and callback short-circuiting. */
    bool iterate(IterCallback const& callback) const override;

    // Descriptor navigation and native transport also work without JSON. Keep the
    // export API conditional with ModelNode::toJson and the nlohmann dependency.
#if defined(SIMFIL_WITH_MODEL_JSON)

    /** Bound this export and optionally apply the embedding application's scalar policy. */
    struct JsonOptions
    {
        size_t maxDepth = 128;
        size_t maxNodes = 10000;
        size_t maxBytes = 4 * 1024 * 1024;

        /** Charge a caller's shared budget before work/output; throwing aborts this export.
         * Accessor internals and allocator overhead are not a memory/CPU sandbox. */
        std::function<void(size_t nodes, size_t chargedBytes)> reserve;

        /** Override primitive JSON only; the complete descriptor traversal stays generic.
         * Base charges precede this call. Extra encoded bytes are charged after
         * it returns; internal callback allocations/work remain caller-owned. */
        std::function<nlohmann::json(ModelNode const&)> scalar;
    };

    /** Export cycle references with default limits; exhaustion throws, never publishes partial JSON. */
    auto toJson() const -> nlohmann::json override;

    /** Serialize with request-local cycle detection and conservative charged output-byte limits. */
    auto toJson(JsonOptions const& limits) const -> nlohmann::json;
#endif

private:
    friend class SchemaPool;
    friend auto resolveInternal<>(res::tag<SchemaTypeModelNode>, SchemaPool const&, ModelNode const&)
        -> model_ptr<SchemaTypeModelNode>;

    /** Descriptor columns are reserved below the application custom-column boundary. */
    enum Column : uint8_t
    {
        Type = ModelPool::Bitmask + 1,
        Field,
        Fields,
        Elements,
        Alternatives,
        FieldAlternatives,
        FieldReferences,
        Enum,
        NamedValues,
        NamedValue,
        NumericDomain,
        Intervals,
        Interval,
        End
    };
    static_assert(unsigned(End) <= unsigned(ModelPool::FirstCustomColumnId));

    /** Borrow the pool already retained by the ordinary ModelNode owner. */
    auto pool() const -> SchemaPool const&;

    /** Count generic members, writing their ordered keys when output space is supplied. */
    auto builtinKeys(std::span<StringId> output = {}) const -> uint32_t;

    /** Keep inline scalar payloads alive through the same canonical owner. */
    auto scalar(ScalarValueType value) const -> ModelNode::Ptr;

    /** Preserve exact unsigned metadata and represent masks losslessly. */
    template <class T> auto metadataScalar(T value, bool flag = false) const -> ModelNode::Ptr;

    /** Own the limits and active ancestry for one JSON export, never for navigation. */
    class JsonExport;
};

} // namespace simfil
