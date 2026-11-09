#pragma once

#include "model.h"
#include "schema-type.h"

namespace simfil
{

class SchemaTypeModelNode;

/**
 * Own a type graph and the dictionary used by its field and symbol names.
 * Factories return views that read and update the pool's records directly. Build
 * the graph, then finalize it before sharing it with query workers. For example:
 *
 *     auto pool = std::make_shared<SchemaPool>();
 *     auto road = pool->newObjectType();
 *     road.addField("speed", pool->newScalarType(SchemaType::Kind::Int));
 *     pool->finalize();
 *     auto queryRoot = road.retain();
 *
 * queryRoot keeps the pool alive. Ordinary factory/lookup views borrow it and
 * require the caller to retain the pool for their lifetime. Finalization validates
 * the graph, prepares lookup indexes and freezes the records and dictionary.
 */
class SchemaPool : public ModelPool
{
public:
    using Ptr = std::shared_ptr<SchemaPool>;
    using ConstPtr = std::shared_ptr<const SchemaPool>;

    /** A supplied dictionary may define application-specific static kind names. */
    explicit SchemaPool(std::shared_ptr<StringPool> strings = std::make_shared<StringPool>());
    ~SchemaPool() override;
    SchemaPool(SchemaPool const&) = delete;
    SchemaPool& operator=(SchemaPool const&) = delete;

    /** Borrow the immutable dictionary namespace without an ownership-counter update. */
    auto dictionary() const noexcept -> StringPool const&;

    /** A schema dictionary cannot be rebound: its IDs are embedded throughout the graph. */
    auto setStrings(std::shared_ptr<StringPool> const& strings) -> tl::expected<void, Error> override;

    /** Read the published artifact identity, independent of graph-local type IDs. */
    auto identity() const noexcept -> std::string_view;

    /** Borrow a readonly type directly from canonical storage, including during construction. */
    auto schemaType(TypeId id) const -> SchemaType;

    /** Borrow a writable type; every mutation still checks the owner's current freeze state. */
    auto schemaType(TypeId id) -> SchemaType;

    /** Allocate a type view for an open application-specific kind. */
    auto newType(SchemaType::Kind kind) -> SchemaType;

    /** Allocate an object type, rejecting kinds without object affinity. */
    auto newObjectType(SchemaType::Kind kind = SchemaType::Kind::Object) -> SchemaObjectType;

    /** Allocate an array type, rejecting kinds without array affinity. */
    auto newArrayType(SchemaType::Kind kind = SchemaType::Kind::Array) -> SchemaArrayType;

    /** Allocate a scalar type without inventing a new semantic kind. */
    auto newScalarType(SchemaType::Kind kind) -> SchemaScalarType;

    /** Allocate a scalar enumeration; its literals need not be strings. */
    auto newEnumType(SchemaType::Kind kind = SchemaType::Kind::String) -> SchemaEnumType;

    /** Allocate a concrete mask and validate its physical width before publication. */
    auto newBitmaskType(uint8_t width = 64, SchemaType::Kind kind = SchemaType::Kind::Bitmask)
        -> SchemaBitmaskType;

    /** Allocate a logical combiner independently of its eventual structural affinity. */
    auto newCompositionType(SchemaType::Composition composition) -> SchemaCompositionType;

    /** Copy one same-pool type's direct metadata and collections; child identities stay shared. */
    auto cloneType(SchemaType source) -> SchemaType;

protected:
    // Derived owners build domain factories here; callers use pool-qualified typed views.
    /** Allocate a nonzero 16-bit identity, failing explicitly before exhaustion. */
    auto addSchema(SchemaType::Kind kind) -> TypeId;

    /** Declare a member edge; repeated additions append possible target types. */
    void addFieldSchema(TypeId parent, std::string_view field, TypeId child = NoTypeId,
                        bool multimap = false);

    /** Record optional/required presence independently of nullability. */
    void setFieldRequired(TypeId parent, std::string_view field, std::optional<bool> required);

    /** Set enumeration semantics on a declared edge, never on the target type. */
    void setFieldRole(TypeId parent, std::string_view field, SchemaType::FieldRole role);

    /** Change repeated-member semantics without adding an unknown child declaration. */
    void setFieldMultimap(TypeId parent, std::string_view field, bool multimap);

    /** Add a lookup-only spelling without duplicating the enumerated data edge. */
    void addFieldAlias(TypeId parent, std::string_view alias, std::string_view canonical);

    /** Inherit an object's member edges, with locally declared members taking precedence. */
    void setBaseSchema(TypeId id, TypeId base);

    /** Append possible array-element types; unknown elements remain conservative. */
    void addElementSchema(TypeId parent, TypeId child);

    /** Set the logical operator independently of structural affinity. */
    void setComposition(TypeId id, SchemaType::Composition composition);

    /** Add a logical alternative at the same value position. */
    void addAlternative(TypeId id, TypeId alternative);

    /** Retain a producer type name without changing its kind. */
    void setTypeName(TypeId id, std::string_view name);

    /** Record support for undeclared object members. */
    void setOpen(TypeId id, bool open);

    /** Record known nullability, including explicit unknown metadata. */
    void setNullable(TypeId id, std::optional<bool> nullable);

    /** Intern a string enum symbol in the schema dictionary. */
    void addEnumSymbol(TypeId id, std::string_view symbol);

    /** Retain a non-string literal without interpreting it as a named value. */
    void addEnumValue(TypeId id, SchemaType::EnumValue const& value);

    /** Copy numeric interpretation, bounds and intervals from borrowed metadata. */
    void setNumericDomain(TypeId id, SchemaType::NumericDomain const& domain);

    /** Copy one named constant, special value or flag and its semantic metadata. */
    void addNamedValue(TypeId id, SchemaType::NamedValue const& value);

    /** Validate and set a concrete bitmask's physical encoding width. */
    void setBitWidth(TypeId id, uint8_t width);

public:
    /**
     * Validate, prepare lookup indexes and freeze the graph and dictionary.
     * If validation/indexing throws, discard this pool: publication has begun and
     * its partially prepared indexes cannot be repaired by further builder calls.
     */
    void finalize();

    /** Publish a caller-supplied transport identity, rejecting invalid names before freezing. */
    void finalize(std::string identity);

    /** Report whether graph validation and index preparation have completed. */
    auto finalized() const -> bool;

    /** Count definitions, excluding the unknown sentinel. */
    auto typeCount() const -> size_t;

    /** Storage counts for owner-level diagnostics, excluding borrowed model payloads. */
    struct Statistics
    {
        size_t types = 0, fields = 0, fieldEdges = 0, elementEdges = 0, alternativeEdges = 0;
        size_t contextFields = 0, namedValues = 0, numericDomains = 0;
    };

    /** Read a once-prepared summary without walking the graph. */
    auto schemaStatistics() const -> Statistics;

    /** Retain a direct descriptor over this graph; navigation creates no adapter graph. */
    auto typeNode(TypeId id) const -> ModelNode::Ptr;
    using ModelPool::resolve;

    /** Resolve schema descriptor addresses directly, falling back to ordinary model primitives. */
    auto resolve(ModelNode const& node, ResolveFn const& callback) const -> tl::expected<void, Error> override;

    /** Descriptor edges form a graph; recursive queries suppress only active-path back-edges. */
    auto supportsRecursiveCycles() const noexcept -> bool override { return true; }

    /** Field occurrences of one represented type share its traversal identity, not its descriptor address. */
    auto recursiveTraversalKey(ModelNode const& node) const noexcept -> ModelNodeAddress override;

    /**
     * Copy a distinct finalized source into this mutable pool, preserving sharing
     * and cycles. The returned table maps source TypeIds to destination TypeIds,
     * including zero -> zero. An optional reuse table has source.typeCount() + 1
     * entries; nonzero entries skip copying that source type. The caller must
     * establish that reused types have compatible semantics. Static kind-name
     * vocabularies must match; dynamic names are interned in the destination.
     */
    auto importGraph(SchemaPool const& source, std::span<TypeId const> reuse = {})
        -> std::vector<TypeId>;

    /** Retained-capacity lower bound for graph-owned columns, indexes and views. */
    auto schemaMemoryUsage() const -> MemoryUsage;

    /** Bounded generic native graph codec, including its independent dictionary. */
    auto write(std::ostream& output) -> tl::expected<void, Error> override;
    auto write(std::ostream& output) const -> tl::expected<void, Error>;

    /** Validate all references and budgets before returning a frozen owner. */
    static auto read(std::span<uint8_t const> input, size_t maxBytes = 64 * 1024 * 1024)
        -> ConstPtr;

    /** Decode an artifact whose transport identity was supplied and verified by its envelope. */
    static auto read(std::span<uint8_t const> input, std::string identity,
                     size_t maxBytes = 64 * 1024 * 1024) -> ConstPtr;

protected:
    /** Reject changes to domain-owned field occurrences before interning or allocation.
     * Alias declarations validate both spellings. Implementations only inspect policy. */
    virtual void validateFieldMutation(TypeId, std::string_view) const {}

    /** Protect a native envelope's kind, inheritance and structural edges.
     * Open/nullable/name annotations still follow ordinary per-record sealing. */
    virtual void validateStructureMutation(TypeId) const {}

    /** Copy a direct clone's domain declarations and private mutation policy before commit.
     * Generic append rollback is active. Derived owners restore their own append
     * tails on failure and publish last; runtime use sites are not cloned here. */
    virtual void cloneTypeExtension(TypeId source, TypeId target) {}

    /** Domain-specific standalone symbol predicates; paths use this pool's dictionary. */
    virtual auto symbolEqualityPaths(TypeId, StringId) const -> std::vector<SchemaPath> { return {}; }

    /** Domain-specific operand shorthand; no additional schema graph or namespace facade. */
    virtual auto scalarFieldPathsForSymbol(TypeId, StringId) const -> std::vector<SchemaPath> { return {}; }

    /** Atomically append a derived owner's metadata, including dictionary/primitive storage.
     * The callback owns its extension rollback, may only append generic records,
     * and must not finalize/freeze. Publish derived state last, without throwing. */
    void appendExtension(std::function<void()> const& extension);

    /** Atomically attach one new field and its append-only domain declarations.
     * The callback returns a local child and may only append records. The
     * caller keeps derived rollback active across this entire operation, since
     * generic staging may fail after the callback. Existing fields are rejected;
     * this boundary cannot nest inside another generic append transaction. */
    auto appendFieldWithExtension(TypeId parent, std::string_view name,
                                  std::function<SchemaType()> const& extension,
                                  bool multimap = false, std::optional<bool> required = {},
                                  SchemaType::FieldRole role = SchemaType::FieldRole::Data) -> SchemaField;

    /** Publish a derived artifact's identity after generic finalization and before freezing.
     * The identity is not included in its own hashed schema payload. */
    void setIdentity(std::string identity);

    /** Count additional descriptor members without materializing their values. */
    virtual auto descriptorExtensionSize(TypeId) const -> uint32_t { return 0; }

    /** Enumerate unique extension keys, distinct from generic metadata keys. */
    virtual auto descriptorExtensionKeyAt(TypeId, uint32_t) const -> StringId { return StringPool::Empty; }

    /** Resolve an extension value lazily over this same owner's canonical records. */
    virtual auto descriptorExtensionGet(TypeId, StringId) const -> ModelNode::Ptr { return {}; }

    /** Append a graph atomically, including a derived owner's append-only extension callback.
     * The callback owns its extension rollback and must not finalize, freeze or
     * mutate existing generic records. It receives the complete source-ID remap. */
    auto importGraphWithExtension(SchemaPool const& source, std::span<TypeId const> reuse,
                                 std::function<void(std::span<TypeId const>)> const& extension)
        -> std::vector<TypeId>;

    /** Atomically apply explicit scalar annotations and an append-only extension callback.
     * Missing options leave native metadata unchanged. Named values append and
     * must have unique nonempty names; the callback must not mutate existing
     * generic records or publish/freeze the graph. Retired arrays become empty. */
    void applyScalarAnnotations(TypeId id, std::optional<SchemaType::NumericDomain> const& numeric,
                                std::optional<uint8_t> width,
                                std::span<SchemaType::NamedValue const> named,
                                std::function<void()> const& extension);

    /** Prepare canonical graph views while a derived owner finishes its own metadata. */
    void finalizeSchema();

    /** Embed/read the canonical generic payload in a derived artifact envelope. */
    void writeSchemaPayload(std::ostream& output) const;
    void readSchemaPayload(std::span<uint8_t const> input, size_t maxBytes);

private:
    friend class SchemaTypeModelNode;
    friend class SchemaType;
    friend class SchemaField;
    friend class SchemaObjectType;
    friend class SchemaArrayType;
    friend class SchemaScalarType;
    friend class SchemaEnumType;
    friend class SchemaBitmaskType;
    friend class SchemaCompositionType;

    /** Declare an empty occurrence through the same policy boundary as other field writes. */
    void declareField(TypeId id, std::string_view name);

    /** Copy borrowed arena/producer intervals directly into canonical numeric storage. */
    void setTypeNumericDomain(TypeId id, SchemaType::NumericDomain const& domain);
    struct Impl;
    std::unique_ptr<Impl> schema_;
};

} // namespace simfil
