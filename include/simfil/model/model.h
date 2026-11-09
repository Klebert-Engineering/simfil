// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.
#pragma once

#include "simfil/model/string-pool.h"
#include "simfil/model/memory.h"
#include "simfil/byte-array.h"
#include "tl/expected.hpp"
#if defined(SIMFIL_WITH_MODEL_JSON)
#  include "nlohmann/json.hpp"
#endif

#include <memory>
#include <functional>
#include <string_view>
#include <vector>
#include <type_traits>
#include <utility>
#include <cassert>
#include <istream>
#include <ostream>

#include "nodes.h"
#include "string-pool.h"

namespace simfil
{

namespace res
{
// Tag type for ADL-based resolve hooks implemented by model libraries.
template<typename Target>
struct tag {};
}

namespace detail
{
template<class T>
concept HasModelType = requires { typename T::ModelType; };

template<HasModelType T>
using ModelTypeOf = typename T::ModelType;
}

/**
 * ADL customization point for typed node resolution.
 * Libraries define resolveInternal(tag, model, node) in their namespace.
 */
template<typename Target, typename ModelType>
model_ptr<Target> resolveInternal(res::tag<Target>, ModelType const&, ModelNode const&) = delete;

class ModelPool;
class SchemaPool;

// Built-in resolve hooks for core node types. Declared here so ADL sees them
// across translation units without relying on friend injection.
template<>
model_ptr<Object> resolveInternal(res::tag<Object>, ModelPool const&, ModelNode const&);
template<>
model_ptr<Array> resolveInternal(res::tag<Array>, ModelPool const&, ModelNode const&);

/**
 * Basic node model which only resolves trivial node types.
 */
class Model : public std::enable_shared_from_this<Model>
{
public:
    enum TrivialColumnId : uint8_t {
        Null = 0,
        UInt16,
        Int16,
        Bool,
        Scalar,
        Undefined,

        FirstNontrivialColumnId,
    };

    struct ResolveFn
    {
        virtual ~ResolveFn() = default;
        virtual void operator() (ModelNode const& resolved) const = 0;
    };

    template<typename T>
    struct Lambda : public ResolveFn
    {
        Lambda(T const& fn) : fn_(fn) {}  // NOLINT
        void operator() (ModelNode const& resolved) const override {fn_(resolved);};
        T fn_;
    };

    /** Virtual destructor to allow polymorphism */
    virtual ~Model() = default;

    /**
     * Decode the node's column/address and invoke cb synchronously with its
     * concrete wrapper. The wrapper reference expires when cb returns; copy a
     * model_ptr if the result must escape. Return an error for unsupported columns.
     */
    virtual tl::expected<void, Error> resolve(ModelNode const& n, ResolveFn const& cb) const;

    /**
     * Resolve a node to a specific ModelNode subtype using ADL hooks.
     * This provides a clean cast API without exposing model internals.
     */
    template<typename Target = ModelNode>
    model_ptr<Target> resolve(ModelNodeAddress const& address) const
    {
        if constexpr (std::is_same_v<Target, ModelNode>) {
            return ModelNode::Ptr::make(shared_from_this(), address);
        }
        return resolve<Target>(*ModelNode::Ptr::make(shared_from_this(), address));
    }

    template<typename Target = ModelNode>
    model_ptr<Target> resolve(ModelNodeAddress const& address, ScalarValueType data) const
    {
        if constexpr (std::is_same_v<Target, ModelNode>) {
            return ModelNode::Ptr::make(shared_from_this(), address, std::move(data));
        }
        return resolve<Target>(*ModelNode::Ptr::make(shared_from_this(), address, std::move(data)));
    }

    template<typename Target>
    model_ptr<Target> resolve(ModelNode::Ptr const& node) const
    {
        return resolve<Target>(*node);
    }

    template<typename Target>
    model_ptr<Target> resolve(ModelNode const& node) const
    {
        if constexpr (std::is_same_v<Target, ModelNode>) {
            return model_ptr<ModelNode>(node);
        }
        else {
            if constexpr (!detail::HasModelType<Target>) {
                static_assert(detail::HasModelType<Target>, "Target must provide a ModelType alias.");
                return {};
            }
            else {
                using ModelType = detail::ModelTypeOf<Target>;

                // Merged views may expose nodes owned by another model, e.g.
                // overlay feature references returned through a base tile. The
                // node address must be interpreted by the model that created it.
                // The node pins its owner; dispatch needs no additional shared reference.
                if (auto* owner = node.model_.get(); owner && owner != this) {
                    if (auto typedOwner = dynamic_cast<ModelType const*>(owner)) {
                        return resolveInternal(res::tag<Target>{}, *typedOwner, node);
                    }
                }

#if !defined(NDEBUG)
                // In debug builds, validate the model type to catch misuse early.
                auto typedModel = dynamic_cast<ModelType const*>(this);
                assert(typedModel && "resolve<T> called on incompatible model type.");
                return resolveInternal(res::tag<Target>{}, *typedModel, node);
#else
                // In release builds, avoid RTTI overhead on this hot path.
                return resolveInternal(res::tag<Target>{}, *static_cast<ModelType const*>(this), node);
#endif
            }
        }
    }

    /** Add a small scalar value and get its model node view */
    ModelNode::Ptr newSmallValue(bool value);
    ModelNode::Ptr newSmallValue(int16_t value);
    ModelNode::Ptr newSmallValue(uint16_t value);

    /** Create a native undefined value, distinct from null in model and binary form. */
    ModelNode::Ptr newUndefined();

    /**
     * Lookup a field name for a field-id.
     *
     * This is in the base Model class to allow JSON
     * serialization, without having to downcast to ModelPool;
     * which contains the `strings()` function. The base
     * implementation returns an unset optional.
     */
    virtual std::optional<std::string_view> lookupStringId(StringId id) const;

    /**
     * Enable cycle checks when recursive wildcards traverse this model.
     * Graph models such as SchemaPool can lead back to an ancestor. Traversal
     * stops that branch while still visiting a shared child through each sibling.
     */
    virtual auto supportsRecursiveCycles() const noexcept -> bool { return false; }

    /**
     * Identify a node for the active-ancestor cycle check.
     * The default uses its address. SchemaPool maps different field descriptors
     * for the same represented type to one key so recursive types terminate.
     */
    virtual auto recursiveTraversalKey(ModelNode const& node) const noexcept -> ModelNodeAddress
    {
        return node.addr();
    }

    /**
     * Read the runtime value type using this model's storage layout.
     * ModelNode delegates here because the owner knows how to decode its columns.
     * ModelPool reads core column tags directly; custom models can use resolve().
     */
    virtual auto type(ModelNode const& node) const -> ValueType;

    /** Read a node's stored type ID through its owner's storage/resolution protocol. */
    virtual auto schema(ModelNode const& node) const -> TypeId;

    /**
     * Resolve this model's numeric type ID and retain the corresponding schema.
     * For example, two copied models can both use ID 7 for different types. The
     * model's binding identifies which SchemaPool owns that ID. Unknown or
     * unbound IDs return an empty SchemaType.
     */
    virtual auto resolveSchemaType(TypeId id) const -> SchemaType;

    /**
     * Resolve the schema for a node, forwarding to its owning model when needed.
     * Overlay models use this when they expose a node from a base model whose
     * numeric type IDs have meaning only in that base model.
     */
    virtual auto resolveSchemaType(ModelNode const& node) const -> SchemaType;

protected:
    // Shared passkey for direct node construction in Model/ModelPool implementations.
    detail::mp_key mpKey_{detail::mp_key{0}};
};

/**
 * Efficient, extensible pool of SIMFIL model nodes based on
 * column stores for different node types.
 */
class ModelPool : public Model
{
    template<typename, typename> friend struct BaseObject;
    template<typename, typename> friend struct BaseArray;

public:
    // Keep Model::resolve<T> overloads visible alongside the virtual resolve override.
    using Model::resolve;
    using Model::resolveSchemaType;

    /**
     * The pool consists of multiple ModelNode columns,
     * each for a different data type. Each column
     * is identified by a static column ID.
     */
    enum ColumnId : uint8_t {
        Objects = FirstNontrivialColumnId,
        Arrays,
        Int64,
        Double,
        String,
        PooledString,
        ByteArray,
        TypedScalar,
        Bitmask,

        FirstCustomColumnId = 128,
    };

    /** Default ctor with own string storage */
    ModelPool();
    ~ModelPool();

    /** Ctor with shared string storage */
    explicit ModelPool(std::shared_ptr<StringPool> stringStore);

    /**
     * Seal model storage and its shared dictionary before immutable publication.
     * No concurrent mutation or derived low-level mutable borrow may survive this boundary.
     * Derived owners must also seal their own columns; there is no unfreeze operation.
     */
    void freeze();

    /** Whether supported pool and retained-node mutation operations are disabled. */
    auto frozen() const -> bool;

    /**
     * Validate that all internal string/node references are valid
     * Returns a list of found errors.
     */
    virtual std::vector<std::string> checkForErrors() const;

    /**
     * Get a callback with the actual class of the given node.
     * This facilitates the Virtual Function Table role of the ModelPool.
     */
    tl::expected<void, Error> resolve(ModelNode const& n, ResolveFn const& cb) const override;

    /** Clear columns, roots and schema bindings, invalidating existing addresses; keep the dictionary. */
    virtual void clear();

    /** Check for errors and returns them */
    tl::expected<void, Error> validate() const;

    /** Get number of root nodes */
    [[nodiscard]] size_t numRoots() const;

    /** Get specific root node */
    [[nodiscard]] tl::expected<ModelNode::Ptr, Error> root(size_t const& i) const;

    /** Append a root address from this pool; this does not copy or import a foreign node. */
    void addRoot(ModelNode::Ptr const& rootNode);

    /**
     * Allocate an empty object with the requested initial member capacity.
     * fixedSize selects compact singleton storage when capacity is one; larger
     * arrays still grow in chunks. The returned handle retains this pool.
     */
    model_ptr<Object> newObject(size_t initialFieldCapacity = 2, bool fixedSize = false);

    /**
     * Allocate an empty array with the requested initial element capacity.
     * fixedSize has the same singleton-storage meaning as for newObject().
     */
    model_ptr<Array> newArray(size_t initialFieldCapacity = 2, bool fixedSize = false);

    /** Add a scalar value and get its new model node index. */
    ModelNode::Ptr newValue(int64_t const& value);
    ModelNode::Ptr newValue(double const& value);
    ModelNode::Ptr newValue(std::string_view const& value);
    ModelNode::Ptr newValue(simfil::ByteArray const& value);
    ModelNode::Ptr newValue(StringId handle);

    /** Persist an existing scalar type; validate storage bounds and throw on invalid input. */
    auto newTypedValue(ScalarValueType value, SchemaType const& typeRef) -> ModelNode::Ptr;

    /** Persist full-width bits and their intrinsic type; reject invalid width, binding or storage size. */
    auto newBitmask(std::uint64_t bits, SchemaType const& typeRef) -> ModelNode::Ptr;

    /**
     * Application lookup from a serialized schema identity to its immutable pool.
     * For example, a tile reader looks up an ID in its already downloaded schema
     * cache. Return an empty pointer for an unavailable identity; model decoding
     * fails if a required schema cannot be resolved. The returned pool must be
     * finalized, frozen and carry the requested identity.
     */
    using SchemaResolver = std::function<std::shared_ptr<SchemaPool const>(std::string_view)>;

    /** Bind a single graph directly, preserving its numeric ids; incompatible rebinding is rejected. */
    void setSchemaPool(std::shared_ptr<const SchemaPool> pool);

    /** Rebind core schema columns transactionally to one exact graph without copying payloads.
     * Unknown leaves stay untyped. Reject unresolved/incompatible mappings before mutation;
     * derived pools must separately prevalidate and update their custom schema columns. */
    void remapSchemas(std::shared_ptr<const SchemaPool> target,
                      std::function<SchemaType(SchemaType const&)> const& remap);

    /**
     * Install the schema lookup used by schemaPool() and binary read().
     * Set it before reading a model that refers to externally stored schemas.
     * read() resolves every referenced identity before exposing typed values.
     */
    void setSchemaResolver(SchemaResolver resolver);

    /**
     * Find a retained schema by identity, then try the installed resolver on a miss.
     * Return an empty pointer for an empty identity, conflicting local owners,
     * an unavailable schema, or a result whose identity/finalization does not match.
     */
    auto schemaPool(std::string_view identity) const -> std::shared_ptr<const SchemaPool>;

    /** Check a native single-graph payload binding without invoking its external resolver. */
    auto hasDirectSchemaPool(std::string_view identity) const noexcept -> bool;

    /** Resolve a single-graph id or a result pool's common remapped type id. */
    auto resolveSchemaType(TypeId id) const -> SchemaType override;

    /** Read core schema columns directly, retaining custom-node and foreign-owner dispatch. */
    auto schema(ModelNode const& node) const -> TypeId override;

    /** Classify core columns without materializing wrappers; custom columns still resolve normally. */
    auto type(ModelNode const& node) const -> ValueType override;

    /** Avoid resolving node schemas when this model has no semantic graph binding. */
    auto resolveSchemaType(ModelNode const& node) const -> SchemaType override;

    /** Import an exact type into this pool's common binding namespace, retaining its graph once. */
    auto importSchema(SchemaType const& typeRef) -> tl::expected<TypeId, Error>;

    /** Checked creation for untrusted inputs; size errors and frozen pools return errors without mutation. */
    [[nodiscard]] auto tryNewObject(size_t initialFieldCapacity = 2, bool fixedSize = false)
        -> tl::expected<model_ptr<Object>, Error>;
    [[nodiscard]] auto tryNewArray(size_t initialFieldCapacity = 2, bool fixedSize = false)
        -> tl::expected<model_ptr<Array>, Error>;
    [[nodiscard]] auto tryNewValue(int64_t const& value) -> tl::expected<ModelNode::Ptr, Error>;
    [[nodiscard]] auto tryNewValue(double const& value) -> tl::expected<ModelNode::Ptr, Error>;
    [[nodiscard]] auto tryNewValue(std::string_view const& value) -> tl::expected<ModelNode::Ptr, Error>;
    [[nodiscard]] auto tryNewValue(simfil::ByteArray const& value) -> tl::expected<ModelNode::Ptr, Error>;
    [[nodiscard]] auto tryNewValue(StringId handle) -> tl::expected<ModelNode::Ptr, Error>;

    /**
     * Bounds for one native copy, independent of evaluator work limits. Depth is
     * zero at a subtree root; copySequence's enclosing array uses depth zero and
     * its values start at one. Bytes charge copied payload/metadata, excluding
     * allocator slack and work or allocations inside custom node accessors.
     */
    struct CopyOptions {
        size_t maxDepth = 64;
        size_t maxNodes = 100000;
        size_t maxBytes = 16 * 1024 * 1024;

        /** Rebind known types during copying, for example into a composed graph.
         * An absent callback preserves source bindings. A callback must return a
         * resolved type; removing provenance or coercing the payload is not allowed.
         * Ordinary untyped scalar leaves do not invoke it. */
        std::function<SchemaType(SchemaType const&)> schemaRemap;
    };

    /**
     * Copy query-visible nodes, preserving/remapping bound schema IDs without
     * changing its dictionary. Field names must already exist in this pool.
     * Errors can leave unreachable, budget-bounded allocations in this append-only pool.
     */
    auto copyNode(ModelNode const& source, CopyOptions const& options) -> tl::expected<ModelNode::Ptr, Error>;

    /** Copy a subtree using the default materialization limits. */
    auto copyNode(ModelNode const& source) -> tl::expected<ModelNode::Ptr, Error>;

    /** Materialize a complete result sequence under one shared node/depth/byte budget. */
    auto copySequence(std::span<Value const> values, CopyOptions const& options) -> tl::expected<model_ptr<Array>, Error>;

    /** Materialize a result sequence with default limits, retaining its outer array even when empty. */
    auto copySequence(std::span<Value const> values) -> tl::expected<model_ptr<Array>, Error>;

    /** Access the field name storage */
    [[nodiscard]]
    std::shared_ptr<StringPool> strings() const;

    /**
     * Rebind the dictionary and translate core object field names, inserting
     * missing names into the destination. Other dictionary references, including
     * pooled scalar strings and custom columns, need their own migration.
     * A failure can leave the dictionary and some fields changed; use this during
     * exclusive construction, before publishing node handles to readers.
     */
    virtual auto setStrings(std::shared_ptr<simfil::StringPool> const& strings) -> tl::expected<void, Error>;

    std::optional<std::string_view> lookupStringId(StringId id) const override;

    /** Write core columns, roots and schema identities; the StringPool is a separate stream. */
    virtual tl::expected<void, Error> write(std::ostream& outputStream);

    /**
     * Decode core storage starting at offset, resolving named schemas through the
     * installed resolver. Restore the matching dictionary separately before
     * validate(). Decode mutates this pool in place: discard its contents on an
     * error and do not use handles into its previous storage.
     */
    virtual tl::expected<void, Error> read(const std::vector<uint8_t>& input, size_t offset = 0);

    struct SerializationSizeStats {
        size_t rootsBytes = 0;
        size_t int64Bytes = 0;
        size_t doubleBytes = 0;
        size_t stringDataBytes = 0;
        size_t stringRangeBytes = 0;
        size_t objectMemberBytes = 0;
        size_t objectSchemaBytes = 0;
        size_t arrayMemberBytes = 0;
        size_t arraySchemaBytes = 0;
        size_t typedScalarBytes = 0;
        size_t bitmaskBytes = 0;
        size_t schemaBindingBytes = 0;

        [[nodiscard]] size_t totalBytes() const
        {
            return rootsBytes
                + int64Bytes
                + doubleBytes
                + stringDataBytes
                + stringRangeBytes
                + objectMemberBytes
                + objectSchemaBytes
                + arrayMemberBytes
                + arraySchemaBytes + typedScalarBytes + bitmaskBytes + schemaBindingBytes;
        }
    };

    [[nodiscard]] SerializationSizeStats serializationSizeStats() const;

    /** Capacity-oriented memory breakdown of the generic model-pool columns. */
    struct MemoryUsageStats
    {
        MemoryUsage implementation;
        MemoryUsage roots;
        MemoryUsage int64Values;
        MemoryUsage doubleValues;
        MemoryUsage stringData;
        MemoryUsage stringRanges;
        MemoryUsage byteArrayRanges;
        MemoryUsage objectMembers;
        MemoryUsage objectSchemas;
        MemoryUsage arrayMembers;
        MemoryUsage arraySchemas;
        MemoryUsage typedScalars;
        MemoryUsage bitmasks;
        MemoryUsage schemaOwners;

        /** Sum all independently owned model-pool storage. */
        [[nodiscard]] MemoryUsage total() const
        {
            return implementation + roots + int64Values + doubleValues + stringData +
                stringRanges + byteArrayRanges + objectMembers + objectSchemas +
                arrayMembers + arraySchemas + typedScalars + bitmasks + schemaOwners;
        }
    };

    /** Return live payload and retained capacity for generic model-pool storage. */
    [[nodiscard]] MemoryUsageStats memoryUsageStats() const;

#if defined(SIMFIL_WITH_MODEL_JSON)

    /** JSON Serialization */
    virtual nlohmann::json toJson() const;
#endif

protected:
    /** Logical tails of plain scalar appends, including roots and shared byte storage. */
    struct ScalarAppendCheckpoint
    {
        ScalarAppendCheckpoint(ScalarAppendCheckpoint const&) = default;
        ScalarAppendCheckpoint& operator=(ScalarAppendCheckpoint const&) = default;

        size_t roots = 0;
        size_t int64Values = 0;
        size_t doubleValues = 0;
        size_t stringBytes = 0;
        size_t stringRows = 0;
        size_t byteArrayRows = 0;

    private:
        friend class ModelPool;
        ScalarAppendCheckpoint() = default;
        ModelPool const* owner_ = nullptr;
    };

    /**
     * Exclusive construction only: append plain scalars and roots without
     * modifying existing values, containers, bindings, or tagged scalar columns.
     * Dictionary appends need their owner's separate checkpoint. No freeze,
     * clear, read, or pool replacement is permitted while the checkpoint is active.
     */
    [[nodiscard]] ScalarAppendCheckpoint scalarAppendCheckpoint() const;

    /** Restore logical scalar tails without allocating or shrinking capacity. */
    void rollbackScalarAppends(ScalarAppendCheckpoint const& checkpoint) noexcept;

    /** Fail before mutation through factories or retained nodes of a published immutable pool. */
    void requireMutable() const;
    struct Impl;
    std::unique_ptr<Impl> impl_;

    [[nodiscard]] TypeId objectSchemaId(ArrayIndex members) const;
    auto setObjectSchemaId(ArrayIndex members, TypeId schemaId) -> tl::expected<void, Error>;
    [[nodiscard]] TypeId arraySchemaId(ArrayIndex members) const;
    auto setArraySchemaId(ArrayIndex members, TypeId schemaId) -> tl::expected<void, Error>;

    /**
     * Protected object/array member storage access,
     * so derived ModelPools can create Object/Array-derived nodes.
     */
    Object::Storage& objectMemberStorage();
    [[nodiscard]] Object::Storage const& objectMemberStorage() const;

    Array::Storage& arrayMemberStorage();
    [[nodiscard]] Array::Storage const& arrayMemberStorage() const;

private:
    class NodeCopy;
    class ScalarNode;
};

}

#include "nodes.impl.h"
