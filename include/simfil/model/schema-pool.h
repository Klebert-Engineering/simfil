#pragma once

#include "model.h"

namespace simfil
{

/** Own a compact immutable domain graph and its private dictionary.
 * Construction state is discarded at finalization. Schema views, descriptors
 * and bindings borrow the same canonical columns; ordinary data leaves need
 * no domain tags. Applications may derive an owner for additional metadata. */
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

    /** Allocate a nonzero 16-bit identity, failing explicitly before exhaustion. */
    auto addSchema(Schema::Kind kind) -> SchemaId;
    /** Declare a member edge; repeated additions append possible target domains. */
    void addFieldSchema(SchemaId parent, std::string_view field, SchemaId child = NoSchemaId,
                        bool multimap = false);
    /** Record optional/required presence independently of nullability. */
    void setFieldRequired(SchemaId parent, std::string_view field, std::optional<bool> required);
    /** Set enumeration semantics on a declared edge, never on the target domain. */
    void setFieldRole(SchemaId parent, std::string_view field, Schema::FieldRole role);
    /** Add a lookup-only spelling without duplicating the enumerated data edge. */
    void addFieldAlias(SchemaId parent, std::string_view alias, std::string_view canonical);
    /** Inherit an object's member edges, with locally declared members taking precedence. */
    void setBaseSchema(SchemaId id, SchemaId base);
    /** Append possible array-element domains; unknown elements remain conservative. */
    void addElementSchema(SchemaId parent, SchemaId child);
    /** Set the logical operator independently of structural affinity. */
    void setComposition(SchemaId id, Schema::Composition composition);
    /** Add a logical alternative at the same value position. */
    void addAlternative(SchemaId id, SchemaId alternative);
    /** Retain a producer type name without changing its kind. */
    void setTypeName(SchemaId id, std::string_view name);
    /** Record support for undeclared object members. */
    void setOpen(SchemaId id, bool open);
    /** Record known nullability, including explicit unknown metadata. */
    void setNullable(SchemaId id, std::optional<bool> nullable);
    /** Intern a string enum symbol in the schema dictionary. */
    void addEnumSymbol(SchemaId id, std::string_view symbol);
    /** Retain a non-string literal without interpreting it as a named value. */
    void addEnumValue(SchemaId id, ScalarValueType const& value);
    /** Copy numeric interpretation, bounds and intervals from borrowed metadata. */
    void setNumericDomain(SchemaId id, Schema::NumericDomain const& domain);
    /** Copy one named constant, special value or flag and its semantic metadata. */
    void addNamedValue(SchemaId id, Schema::NamedValue const& value);
    /** Validate and set a concrete bitmask's physical encoding width. */
    void setBitWidth(SchemaId id, uint8_t width);

    /** Pack, validate and freeze the graph and dictionary before publication. */
    void finalize();
    /** Whether canonical domain views are available. */
    auto finalized() const -> bool;
    /** Count definitions, excluding the unknown sentinel. */
    auto domainCount() const -> size_t;
    /** Storage counts for owner-level diagnostics, excluding borrowed model payloads. */
    struct Statistics
    {
        size_t domains = 0, fields = 0, fieldEdges = 0, elementEdges = 0, alternativeEdges = 0;
        size_t contextFields = 0, namedValues = 0, numericDomains = 0;
    };
    /** Read a once-prepared summary without walking the graph. */
    auto schemaStatistics() const -> Statistics;
    /** Borrow a definition from this owner; zero and absent identities return null. */
    auto domain(SchemaId id) const -> Schema const*;
    /** Keep this exact immutable graph and its dictionary alive through a binding. */
    auto binding(std::string identity) const -> std::shared_ptr<const SchemaBinding>;
    /** Construct a bounded lazy descriptor view, not a second schema graph. */
    auto domainNode(SchemaId id) const -> ModelNode::Ptr;
    /** Borrow declaration/reachability names from the same shared dictionary index. */
    auto fieldNames(SchemaId id, bool recursive = false) const -> std::span<std::string_view const>;
    auto enumNames(SchemaId id, bool recursive = false) const -> std::span<std::string_view const>;
    /** Return repeated-member semantics for a declared edge. */
    auto fieldMultimap(SchemaId id, StringId field) const -> bool;
    /** Return an inherited base identity without traversing its members. */
    auto baseSchema(SchemaId id) const -> SchemaId;
    /** Visit lookup aliases in their schema-owned namespace. */
    void forEachFieldAlias(SchemaId id, std::function<void(StringId, StringId)> const& fn) const;
    /** Copy a whole graph, preserving sharing/cycles; supplied nonzero mappings reuse existing
     * domains. The returned construction-only table is indexed by source identity. */
    auto importGraph(SchemaPool const& source, std::span<SchemaId const> reuse = {})
        -> std::vector<SchemaId>;
    /** Retained-capacity lower bound for graph-owned columns, indexes and views. */
    auto schemaMemoryUsage() const -> MemoryUsage;

    /** Bounded generic native graph codec, including its independent dictionary. */
    auto write(std::ostream& output) -> tl::expected<void, Error> override;
    auto write(std::ostream& output) const -> tl::expected<void, Error>;
    /** Validate all references and budgets before returning a frozen owner. */
    static auto read(std::span<uint8_t const> input, size_t maxBytes = 64 * 1024 * 1024)
        -> ConstPtr;

protected:
    /** Prepare canonical graph views while a derived owner finishes its own metadata. */
    void finalizeSchema();
    /** Embed/read the canonical generic payload in a derived artifact envelope. */
    void writeSchemaPayload(std::ostream& output) const;
    void readSchemaPayload(std::span<uint8_t const> input, size_t maxBytes);

private:
    struct Impl;
    std::unique_ptr<Impl> schema_;
};

} // namespace simfil
