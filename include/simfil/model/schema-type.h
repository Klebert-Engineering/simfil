#pragma once

#include "simfil/model/arena.h"
#include "simfil/model/string-pool.h"
#include "simfil/model/value-type.h"
#include "simfil/byte-array.h"

#include <cassert>
#include <compare>
#include <functional>
#include <optional>
#include <ranges>
#include <span>
#include <variant>

namespace simfil
{

class SchemaPool;
class ModelPool;

/**
 * One segment in a schema-derived query path.
 *
 * Field segments address object members. Array-element segments represent the
 * non-recursive `*` operator needed to traverse array elements precisely.
 */
struct SchemaPathSegment
{
    enum class Kind {
        Field,
        ArrayElement,
    };

    Kind kind = Kind::Field;
    StringId field = 0;

    auto operator<=>(const SchemaPathSegment&) const = default;
};

/** Sequence of schema path segments from a root schema to a reachable value. */
using SchemaPath = std::vector<SchemaPathSegment>;


class SchemaField;
class SchemaObjectType;
class SchemaArrayType;
class SchemaScalarType;
class SchemaEnumType;
class SchemaBitmaskType;
class SchemaCompositionType;
class SchemaTypeModelNode;
template <class T> struct model_ptr;

/** A pool-qualified type view, borrowed by default and explicitly retainable.
 * Borrowed copies do not acquire ownership. Collection ranges borrow the pool,
 * even when obtained from an owning view; retain the view during traversal. */
class SchemaType
{
public:
    /** Exact scalar metadata; unsigned encodings must not pass through a floating-point value. */
    using NumericValue = std::variant<std::int64_t, std::uint64_t, double>;

    /** Inclusive ordinary-measurement interval, distinct from named special values. */
    struct NumericRange {
        NumericValue min = std::int64_t{0};
        NumericValue max = std::int64_t{0};
    };

    /** A named literal or mask, not an implicit exhaustive enum or a globally scoped constant. */
    struct NamedValue {
        enum class Role { Constant, Special, Flag };
        enum class Meaning { Ordinary, Unknown, Variable, Unbounded, Saturated };
        std::string_view name;
        std::variant<bool, std::int64_t, std::uint64_t, double, std::string_view> value = std::int64_t{0};
        Role role = Role::Constant;
        Meaning meaning = Meaning::Ordinary;
        bool deprecated = false;
        bool removed = false;
        /** Original integer encoding, independent of the runtime label; absent is not zero. */
        std::optional<std::variant<std::int64_t, std::uint64_t>> rawValue;
    };

    /** Possible present values; absence is field metadata, not a null value. */
    static constexpr std::uint16_t AllAffinities = (1u << unsigned(ValueType::LAST_)) - 2;
    static constexpr std::uint16_t ScalarAffinities = (1u << unsigned(ValueType::TransientObject)) - 2;

    /** Open packed kind: static name StringId above structural-affinity bits. */
    enum class Kind : std::uint32_t {
        Unknown = (StringPool::SchemaUnknown << 16) | AllAffinities,
        Any = (StringPool::SchemaAny << 16) | AllAffinities,
        Never = StringPool::SchemaNever << 16,
        Value = (StringPool::SchemaValue << 16) | ScalarAffinities,
        Null = (StringPool::SchemaNull << 16) | valueTypeAffinity(ValueType::Null),
        Bool = (StringPool::SchemaBool << 16) | valueTypeAffinity(ValueType::Bool),
        Int = (StringPool::SchemaInt << 16) | valueTypeAffinity(ValueType::Int),
        Float = (StringPool::SchemaFloat << 16) | valueTypeAffinity(ValueType::Float),
        String = (StringPool::SchemaString << 16) | valueTypeAffinity(ValueType::String),
        Bytes = (StringPool::SchemaBytes << 16) | valueTypeAffinity(ValueType::Bytes),
        Bitmask = (StringPool::SchemaBitmask << 16) | valueTypeAffinity(ValueType::Bitmask),
        Object = (StringPool::SchemaObject << 16) | valueTypeAffinity(ValueType::Object),
        Array = (StringPool::SchemaArray << 16) | valueTypeAffinity(ValueType::Array),
        Union = (StringPool::SchemaUnion << 16) | AllAffinities,
        OneOf = (StringPool::SchemaOneOf << 16) | AllAffinities,
        Intersection = (StringPool::SchemaIntersection << 16) | AllAffinities,
    };

    /** Logical alternatives apply at one value position, never an array level. */
    enum class Composition { None, AnyOf, OneOf, AllOf };

    /** Construct an extension kind without registering its semantics in simfil. */
    static constexpr auto makeKind(StringId name, std::uint16_t affinities) -> Kind
    {
        assert(name != StringPool::Empty && name < StringPool::FirstDynamicId);
        return Kind((std::uint32_t(name) << 16) | affinities);
    }

    /** Return the static name id, independent of the represented structure. */
    static constexpr auto kindNameId(Kind kind) -> StringId { return std::uint32_t(kind) >> 16; }

    /** Return the coarse set of possible runtime types, not full constraints. */
    static constexpr auto affinities(Kind kind) -> std::uint16_t { return std::uint32_t(kind) & 0xffff; }

    /** Context edges are addressable metadata, not recursively enumerable runtime children. */
    enum class FieldRole : uint8_t { Data, Context };
    /** Non-string enum literals have no runtime provenance; string symbols use the dictionary. */
    using EnumValue = std::variant<std::monostate, bool, int64_t, double, ByteArray>;
    using TypeIds = ArrayArena<TypeId>::BasicArrayRange<true>;
    using NameIds = ArrayArena<StringId>::BasicArrayRange<true>;

    /** Borrow numeric intervals from producer memory or decode canonical arena rows lazily. */
    class NumericRanges
    {
        /** Decode one interval without copying the containing collection. */
        struct Read
        {
            SchemaPool const* pool = nullptr;
            NumericRange const* data = nullptr;
            ArrayIndex handle = 0;
            auto operator()(size_t position) const -> NumericRange;
        };
        using View = std::ranges::transform_view<std::ranges::iota_view<size_t, size_t>, Read>;

    public:
        /** Construct an empty borrowed range. */
        NumericRanges() : view_(std::views::iota(size_t{0}, size_t{0}), Read{}) {}
        /** Borrow producer intervals for a synchronous construction call. */
        NumericRanges(std::span<NumericRange const> values)
            : view_(std::views::iota(size_t{0}, values.size()), Read{.data = values.data()}) {}
        /** Iterate intervals by value; no expanded numeric metadata is retained. */
        auto begin() const { return view_.begin(); }
        /** Return the sentinel for the same borrowed collection. */
        auto end() const { return view_.end(); }
        /** Count intervals, not the individual encoded bounds. */
        auto size() const -> size_t { return view_.size(); }
        /** Test whether any ordinary-measurement interval is declared. */
        auto empty() const -> bool { return size() == 0; }
        /** Decode one interval; the position must be in range. */
        auto operator[](size_t position) const -> NumericRange { return view_[position]; }

    private:
        friend class SchemaType;
        /** Bind a canonical interval handle; the caller keeps the pool alive. */
        NumericRanges(SchemaPool const* pool, ArrayIndex handle, size_t size)
            : view_(std::views::iota(size_t{0}, size), Read{.pool = pool, .handle = handle}) {}
        View view_;
    };

    /** Numeric interpretation with a borrowed, allocation-free interval range. */
    struct NumericDomain
    {
        std::string_view encoding;
        std::string_view unit;
        std::optional<NumericValue> minimum;
        std::optional<NumericValue> maximum;
        NumericRanges ordinaryRanges;
        double scale = 1;
        double offset = 0;
        bool complete = false;
    };

    /** An empty view represents unknown information, not a separately owned type. */
    SchemaType() = default;
    /** Identify a declared type independently of whether this view owns its pool. */
    explicit operator bool() const noexcept { return pool_ && id_; }
    /** Return the graph-local identity; it has no meaning without pool(). */
    auto id() const noexcept -> TypeId { return id_; }
    /** Borrow the graph owner without incrementing its reference count. */
    auto pool() const noexcept -> SchemaPool const* { return pool_; }
    /** Distinguish explicit ownership from a caller-managed lifetime. */
    auto ownsPool() const noexcept -> bool { return bool(owner_); }
    /** Retain the same graph; a stack-owned pool cannot produce an owning view. */
    auto retain() const -> SchemaType;
    /** Return a borrowed view of the same type without transferring ownership. */
    auto borrow() const noexcept -> SchemaType;
    /** Retain a direct metadata descriptor over this type's canonical pool. */
    auto modelNode() const -> model_ptr<SchemaTypeModelNode>;
    /** Compare exact owner/type identity, not the address of a temporary view. */
    auto operator==(SchemaType const& other) const noexcept -> bool;

    /** Read structural affinity without converting to a descriptor ModelNode. */
    auto kind() const -> Kind;
    /** Recognize generic structure for application-specific kinds. */
    auto hasAffinity(ValueType type) const -> bool;
    /** Recognize a concrete mask, not a union merely capable of holding one. */
    auto isBitmask() const -> bool;
    /** Read the source type name separately from its semantic kind. */
    auto typeName() const -> std::string_view;
    /** Distinguish logical composition from structural affinity. */
    auto composition() const -> Composition;
    /** Read whether undeclared members are permitted. */
    auto open() const -> bool;
    /** Read value nullability independently of field presence. */
    auto nullable() const -> std::optional<bool>;
    /** Read direct alternatives in declaration order, preserving multiplicity. */
    auto alternatives() const -> TypeIds;
    /** Read direct element declarations, including explicit unknown targets. */
    auto elements() const -> TypeIds;
    /** Read the inherited base without flattening its declarations into this type. */
    auto baseType() const -> SchemaType;
    /** Resolve one effective member type without allocation. */
    auto fieldSchema(StringId name) const -> TypeId;
    /** Resolve one effective element type without allocation. */
    auto itemSchema() const -> TypeId;
    /** Preserve this view's optional owner while resolving one unambiguous member. */
    auto fieldType(StringId name) const -> SchemaType;
    /** Preserve this view's optional owner while resolving one unambiguous element. */
    auto itemType() const -> SchemaType;
    /** Read effective alternatives at this member position without allocation. */
    auto fieldTypes(StringId name) const -> TypeIds;
    /** Read effective element alternatives without allocation. */
    auto itemTypes() const -> TypeIds;
    /** Read a declared field's presence, multiplicity and role through its edge. */
    auto field(StringId name) const -> SchemaField;
    /** Look up a field spelling without interning it. */
    auto field(std::string_view name) const -> SchemaField;
    /** Enumerate effective inherited fields; Context edges remain visible here. */
    void forEachField(std::function<void(SchemaField const&)> const& fn) const;
    /** Enumerate this record's own fields, preserving empty child lists and source order. */
    void forEachDirectField(std::function<void(SchemaField const&)> const& fn) const;
    /** Enumerate lookup-only aliases in the schema dictionary's namespace. */
    void forEachFieldAlias(std::function<void(StringId, StringId)> const& fn) const;
    /** Enumerate borrowed named literals without a second metadata collection. */
    void forEachNamedValue(std::function<void(NamedValue const&)> const& fn) const;
    /** Resolve a declared symbol in this type only. */
    auto namedValue(std::string_view name) const -> std::optional<NamedValue>;
    /** Read numeric metadata before or after publication without expanded range storage. */
    auto numericDomain() const -> std::optional<NumericDomain>;
    /** Visit string enum declarations in source order, without interning query text. */
    void forEachEnumSymbol(std::function<void(StringId)> const& fn) const;
    /** Decode non-string literals on demand rather than retaining another value collection. */
    void forEachEnumValue(std::function<void(EnumValue const&)> const& fn) const;
    /** Count literal declarations without reading or allocating their payloads. */
    auto enumValueCount() const -> size_t;
    /** Decode one checked literal, allowing previews to bound both work and allocations. */
    auto enumValueAt(size_t position) const -> EnumValue;
    /** Read the concrete mask's physical encoding width. */
    auto bitWidth() const -> uint8_t;
    /** Publication prevents mutation through every previously obtained view. */
    auto finalized() const -> bool;
    /** Read whether this record is immutable, independently of whole-graph publication. */
    auto sealed() const -> bool;

    /** Read inherited member names in declaration order, without introducing alias duplicates. */
    auto directFields() const -> NameIds;
    /** Read sorted recursively reachable names; unknown descendants remain conservative. */
    auto nestedFields() const -> NameIds;
    /** Read this type's declared enum symbols in source order. */
    auto directEnumSymbols() const -> NameIds;
    /** Read sorted recursively reachable enum symbols. */
    auto nestedEnumSymbols() const -> NameIds;
    /** Test whether descendant field absence is proven by the immutable index. */
    auto reachabilityComplete() const -> bool;
    /** Conservatively test recursive presence without traversing the graph or allocating. */
    auto canHaveField(StringId name) const -> bool;
    /** Test reachable enum declarations; unpublished views remain conservative. */
    auto canHaveEnumSymbol(StringId name) const -> bool;
    /** Resolve lookup-only aliases without changing enumerable source declarations. */
    auto canonicalField(StringId name) const -> StringId;
    /** Unknown or conflicting roles remain enumerable rather than hiding data. */
    auto fieldRole(StringId name) const -> FieldRole;
    /** Test a declared enum symbol by spelling without interning it. */
    auto hasDirectEnumSymbol(std::string_view name) const -> bool;
    /** Enumerate finite exact field paths; cycles or unknowns mark the result incomplete. */
    auto fieldPaths(StringId name, bool* complete = nullptr) const -> std::vector<SchemaPath>;
    /** Enumerate finite paths declaring an enum symbol, in the canonical dictionary namespace. */
    auto enumSymbolPaths(StringId name, bool* complete = nullptr) const -> std::vector<SchemaPath>;
    /** Enumerate finite paths declaring a named value rather than a global constant. */
    auto namedValuePaths(std::string_view name, bool* complete = nullptr) const -> std::vector<SchemaPath>;
    /** Inventory declaring types once; excluded types are opaque boundaries, not a projected graph. */
    auto symbolTypes(std::string_view name, std::span<TypeId const> excluded = {}) const -> std::vector<TypeId>;
    /** Find the first scalar path in declaration order without assuming a runtime object exists. */
    auto firstScalarFieldPath() const -> std::optional<SchemaPath>;
    /** Ask the domain owner for symbol equality paths without a per-type forwarding adapter. */
    auto symbolEqualityPaths(StringId symbol) const -> std::vector<SchemaPath>;
    /** Ask the domain owner for field-only symbol dispatch paths. */
    auto scalarFieldPathsForSymbol(StringId symbol) const -> std::vector<SchemaPath>;

    /** Assign a source name without changing semantic classification. */
    void setTypeName(std::string_view name);
    /** Declare an exact semantic constant, sentinel or flag, including on logical combiners.
     * Named metadata does not narrow a type's structural affinity. */
    void addNamedValue(NamedValue const& value);
    /** Change only the static semantic kind name, preserving affinity and composition. */
    void setKindName(StringId name);
    /** Change the logical operator without replacing a domain kind or its existing metadata. */
    void setComposition(Composition composition);
    /** Assign known or explicitly unknown value nullability. */
    void setNullable(std::optional<bool> nullable);
    /** Seal a reusable type during construction; cloning creates a writable independent copy. */
    void seal();
    /** Convert to a useful structural view, rejecting an incompatible affinity. */
    auto asObject() const -> SchemaObjectType;
    /** Convert to an array view while preserving optional ownership. */
    auto asArray() const -> SchemaArrayType;
    /** Convert to a scalar view while preserving optional ownership. */
    auto asScalar() const -> SchemaScalarType;
    /** Convert to a scalar enum view without making its values exhaustive implicitly. */
    auto asEnum() const -> SchemaEnumType;
    /** Convert only a concrete bitmask type, preserving optional ownership. */
    auto asBitmask() const -> SchemaBitmaskType;
    /** Convert a logical combiner without flattening its alternatives. */
    auto asComposition() const -> SchemaCompositionType;

protected:
    friend class SchemaPool;
    friend class ModelPool;
    friend class SchemaField;
    /** Construction is owner-controlled; the writable flag never overrides pool freezing. */
    SchemaType(SchemaPool const* pool, TypeId id, bool writable) noexcept;
    /** Preserve a runtime model's local owner group rather than reacquiring the global counter. */
    SchemaType(std::shared_ptr<SchemaPool const> owner, TypeId id) noexcept;
    /** Reject readonly borrows and sealed owners before any mutation or interning. */
    auto mutablePool() const -> SchemaPool&;
    /** Reject edges into another pool's unrelated numeric namespace. */
    auto localId(SchemaType const& child) const -> TypeId;

private:
    /** Bound one path-enumeration request; no traversal state is retained by the pool. */
    class PathSearch;
    SchemaPool const* pool_ = nullptr;
    std::shared_ptr<SchemaPool const> owner_;
    TypeId id_ = NoTypeId;
    bool writable_ = false;
};

/** A field occurrence; optional presence and Context role never modify its target type. */
class SchemaField
{
public:
    /** An empty field is distinct from a declared edge with an unknown target. */
    SchemaField() = default;
    /** Test declaration presence, not whether its target type is known. */
    explicit operator bool() const noexcept { return bool(parent_); }
    /** Return the name in the schema's own dictionary. */
    auto nameId() const noexcept -> StringId { return name_; }
    /** Return the declaring type; inherited fields retain their declaration owner. */
    auto parentType() const -> SchemaType { return parent_; }
    /** Borrow the source spelling without mutating any dictionary. */
    auto name() const -> std::string_view;
    /** Read all direct target declarations without discarding unknown edges. */
    auto types() const -> SchemaType::TypeIds;
    /** Read one unambiguous target, retaining ownership only if the field owns it. */
    auto type() const -> SchemaType;
    /** Read optional presence independently of target nullability. */
    auto required() const -> std::optional<bool>;
    /** Read whether repeated runtime members are permitted. */
    auto multimap() const -> bool;
    /** Read whether implicit enumeration may descend through this occurrence. */
    auto role() const -> SchemaType::FieldRole;
    /** Retain this occurrence and its exact parent graph explicitly. */
    auto retain() const -> SchemaField;
    /** Change presence without changing any shared child type. */
    void setRequired(std::optional<bool> required);
    /** Change implicit enumeration only on this edge. */
    void setRole(SchemaType::FieldRole role);
    /** Change repeated-member semantics only on this edge. */
    void setMultimap(bool multimap);
    /** Append another possible target in the same pool, including unknown. */
    void addType(SchemaType child);

private:
    friend class SchemaType;
    friend class SchemaPool;
    /** Only declared occurrences are constructed by a pool/type lookup. */
    SchemaField(SchemaType parent, StringId name) : parent_(std::move(parent)), name_(name) {}
    SchemaType parent_;
    StringId name_ = 0;
};

/** Object construction and reading over the same canonical type record. */
class SchemaObjectType : public SchemaType
{
public:
    /** Validate object affinity while preserving the source view's ownership mode. */
    explicit SchemaObjectType(SchemaType type);
    /** Ensure a member exists without adding a target or resetting existing edge metadata. */
    auto declareField(std::string_view name) -> SchemaField;
    /** Declare or extend a member; zero/empty target means unknown metadata. */
    auto addField(std::string_view name, SchemaType child = {}) -> SchemaField;
    /** Declare a lookup-only spelling without another enumerable edge. */
    void addAlias(std::string_view alias, std::string_view canonical);
    /** Inherit members with local declarations taking precedence. */
    void setBase(SchemaObjectType base);
    /** Permit undeclared members without discarding known declarations. */
    void setOpen(bool open);
};

/** Array construction preserves direct element declarations and named lookup edges. */
class SchemaArrayType : public SchemaType
{
public:
    /** Validate array affinity while preserving optional ownership. */
    explicit SchemaArrayType(SchemaType type);
    /** Append a possible element type without flattening a declared union. */
    void addElement(SchemaType child);
    /** Ensure a named lookup exists, preserving an explicitly empty target collection. */
    auto declareField(std::string_view name) -> SchemaField;
    /** Declare a named lookup on an array; its role controls implicit descent. */
    auto addField(std::string_view name, SchemaType child = {}) -> SchemaField;
    /** Alias a named lookup without adding it to element iteration or field enumeration. */
    void addAlias(std::string_view alias, std::string_view canonical);
};

/** Scalar construction keeps mathematical interpretation separate from named values. */
class SchemaScalarType : public SchemaType
{
public:
    /** Reject container affinities without imposing a simfil-only semantic kind. */
    explicit SchemaScalarType(SchemaType type);
    /** Copy numeric metadata into canonical storage, replacing the previous declaration. */
    void setNumericDomain(NumericDomain const& domain);
};

/** Scalar enumeration is orthogonal to producer kind and logical composition. */
class SchemaEnumType : public SchemaType
{
public:
    /** Preserve source kind and ownership rather than inventing an Enum kind. */
    explicit SchemaEnumType(SchemaType type);
    /** Add a schema-owned string symbol without mutating datasource dictionaries. */
    void addSymbol(std::string_view symbol);
    /** Add a non-string literal; string choices use addSymbol. */
    void addValue(EnumValue const& value);
};

/** A concrete full-width bitmask, not a closed enumeration of its named flags. */
class SchemaBitmaskType : public SchemaScalarType
{
public:
    /** Reject unions and broad scalar kinds which merely allow bitmasks. */
    explicit SchemaBitmaskType(SchemaType type);
    /** Constrain the raw encoding while preserving unnamed bits within that width. */
    void setBitWidth(uint8_t width);
    /** Add a named bit pattern, preserving zero and aliases as meaningful declarations. */
    void addFlag(std::string_view name, uint64_t bits);
};

/** A logical combination at one value position, independent of array traversal. */
class SchemaCompositionType : public SchemaType
{
public:
    /** Require a declared combiner, preserving AnyOf/OneOf/AllOf semantics. */
    explicit SchemaCompositionType(SchemaType type);
    /** Append an alternative in source order without deduplication. */
    void addAlternative(SchemaType alternative);
};

} // namespace simfil
