#include <catch2/catch_test_macros.hpp>
#include <array>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>
#include "simfil/model/schema-pool.h"

using namespace simfil;

namespace
{
/** Exercise derived owners' atomic append and metadata publication boundaries. */
class ExtensibleSchemaPool : public SchemaPool
{
public:
    using SchemaPool::appendExtension;
    using SchemaPool::appendFieldWithExtension;
    using SchemaPool::applyScalarAnnotations;
    using SchemaPool::importGraphWithExtension;
    using SchemaPool::addFieldSchema;
    using SchemaPool::setOpen;
};

/** Keep domain policy on the canonical owner, including writable generic views. */
class GuardedSchemaPool : public SchemaPool
{
public:
    bool failClone = false;

    /** Publish a factory's policy only after its initial declarations exist. */
    void protect(SchemaType type)
    {
        REQUIRE(type.pool() == this);
        protectedTypes_.push_back(type.id());
    }

protected:
    /** Even a same-value write to a reserved occurrence is forbidden. */
    void validateFieldMutation(TypeId id, std::string_view name) const override
    {
        if (protectedType(id) && name == "reserved")
            throw std::invalid_argument("Reserved domain member");
    }

    /** Envelope edges cannot be changed through an alternate structural view. */
    void validateStructureMutation(TypeId id) const override
    {
        if (protectedType(id))
            throw std::invalid_argument("Protected domain structure");
    }

    /** Carry private policy to a clone, publishing it only after possible failures. */
    void cloneTypeExtension(TypeId source, TypeId target) override
    {
        if (failClone) {
            // Exercise the generic rollback before publishing derived policy.
            strings()->emplace("failed-clone-name").value();
            newScalarType(SchemaType::Kind::Int);
            throw std::runtime_error("Injected domain clone failure");
        }
        if (protectedType(source))
            protectedTypes_.push_back(target);
    }

private:
    /** Inspect construction policy without interning names or allocating. */
    auto protectedType(TypeId id) const -> bool
    {
        return std::ranges::find(protectedTypes_, id) != protectedTypes_.end();
    }
    std::vector<TypeId> protectedTypes_;
};
}

TEST_CASE("Canonical mutation hooks protect generic views before dictionary changes", "[model.schema-type]")
{
    auto pool = std::make_shared<GuardedSchemaPool>();
    auto root = pool->newObjectType();
    auto scalar = pool->newScalarType(SchemaType::Kind::Int);
    auto base = pool->newObjectType();
    auto array = pool->newArrayType();
    auto choice = pool->newCompositionType(SchemaType::Composition::AnyOf);
    auto field = root.addField("reserved", scalar);
    field.setRequired(true);
    array.addElement(scalar);
    choice.addAlternative(scalar);
    pool->protect(root);
    pool->protect(array);
    pool->protect(choice);
    auto strings = pool->strings()->size();

    REQUIRE_THROWS_AS(root.addField("reserved", scalar), std::invalid_argument);
    REQUIRE_THROWS_AS(root.declareField("reserved"), std::invalid_argument);
    REQUIRE_THROWS_AS(field.addType(scalar), std::invalid_argument);
    REQUIRE_THROWS_AS(field.setRequired(true), std::invalid_argument);
    REQUIRE_THROWS_AS(field.setRole(SchemaType::FieldRole::Context), std::invalid_argument);
    REQUIRE_THROWS_AS(field.setMultimap(true), std::invalid_argument);
    REQUIRE_THROWS_AS(root.addAlias("incoming", "reserved"), std::invalid_argument);
    REQUIRE_THROWS_AS(root.addAlias("reserved", "new-target"), std::invalid_argument);
    REQUIRE_THROWS_AS(root.setKindName(StringPool::SchemaObject), std::invalid_argument);
    REQUIRE_THROWS_AS(root.setBase(base), std::invalid_argument);
    REQUIRE_THROWS_AS(root.setComposition(SchemaType::Composition::None), std::invalid_argument);
    REQUIRE_THROWS_AS(array.addElement(scalar), std::invalid_argument);
    REQUIRE_THROWS_AS(choice.addAlternative(scalar), std::invalid_argument);
    REQUIRE(pool->strings()->size() == strings);
    REQUIRE(field.required() == true);
    REQUIRE(field.role() == SchemaType::FieldRole::Data);
    REQUIRE_FALSE(field.multimap());
    REQUIRE(std::ranges::equal(field.types(), std::array{scalar.id()}));

    // Broadening unknown members and annotating the type cannot replace reserved edges.
    root.setOpen(true);
    root.setNullable(false);
    root.setTypeName("DomainRecord");
    root.addField("extension", scalar);
    pool->finalize();
    REQUIRE(root.open());
    REQUIRE(root.nullable() == false);
    REQUIRE(root.typeName() == "DomainRecord");
    REQUIRE(root.field("extension").type() == scalar);
}

TEST_CASE("Direct cloning carries domain guards and rolls back extension failures", "[model.schema-type]")
{
    auto pool = std::make_shared<GuardedSchemaPool>();
    auto root = pool->newObjectType();
    root.addField("reserved", pool->newScalarType(SchemaType::Kind::Int));
    pool->protect(root);
    root.seal();
    auto types = pool->typeCount();
    auto strings = pool->strings()->size();
    pool->failClone = true;
    REQUIRE_THROWS_AS(pool->cloneType(root), std::runtime_error);
    REQUIRE(pool->typeCount() == types);
    REQUIRE(pool->strings()->size() == strings);
    REQUIRE(pool->dictionary().get("failed-clone-name") == StringPool::Empty);
    pool->failClone = false;
    auto clone = pool->cloneType(root).asObject();
    REQUIRE(clone.id() == types + 1);
    REQUIRE_FALSE(clone.sealed());
    REQUIRE(clone.field("reserved").type() == root.field("reserved").type());
    REQUIRE_THROWS_AS(clone.field("reserved").setRequired(false), std::invalid_argument);
    REQUIRE_THROWS_AS(clone.setKindName(StringPool::SchemaArray), std::invalid_argument);
    clone.addField("extension");
    REQUIRE_FALSE(root.field("extension"));
}

TEST_CASE("Extension-only appends roll back generic types values and dictionary IDs", "[model.schema-type]")
{
    auto pool = std::make_shared<ExtensibleSchemaPool>();
    auto original = pool->newObjectType();
    auto dictionaries = pool->strings()->emplace("before").value();
    REQUIRE_THROWS_AS(pool->appendExtension([&] {
        pool->strings()->emplace("temporary").value();
        pool->newValue(std::string_view("temporary metadata"));
        pool->newScalarType(SchemaType::Kind::Int);
        original.addField("forbidden");
    }), std::logic_error);
    REQUIRE(pool->typeCount() == 1);
    REQUIRE(pool->dictionary().get("temporary") == StringPool::Empty);
    REQUIRE(pool->strings()->emplace("after").value() == dictionaries + 1);
    pool->appendExtension([&] {
        pool->newObjectType().addField("appended");
    });
    REQUIRE(pool->typeCount() == 2);
    pool->finalize();
    REQUIRE_THROWS_AS(pool->appendExtension([] {}), std::logic_error);
}

TEST_CASE("Atomic field publication changes one parent after append-only extension succeeds", "[model.schema-type]")
{
    auto pool = std::make_shared<ExtensibleSchemaPool>();
    auto root = pool->newObjectType();
    auto integer = pool->newScalarType(SchemaType::Kind::Int);
    for (size_t i = 0; i < 300; ++i)
        root.addField("existing" + std::to_string(i), integer);
    root.addAlias("alias", "existing0");
    auto types = pool->typeCount();
    auto strings = pool->strings()->size();
    auto memory = pool->schemaMemoryUsage();

    REQUIRE_THROWS_AS(pool->appendFieldWithExtension(root.id(), "failed", [&]() -> SchemaType {
        pool->newObjectType().addField("new-child", integer);
        pool->newValue(std::string_view("unpublished annotation"));
        throw std::runtime_error("Injected extension failure");
    }), std::runtime_error);
    REQUIRE(pool->typeCount() == types);
    REQUIRE(pool->strings()->size() == strings);
    REQUIRE(pool->schemaMemoryUsage().logicalBytes == memory.logicalBytes);
    REQUIRE_FALSE(root.field("failed"));

    auto foreign = std::make_shared<SchemaPool>();
    auto foreignType = foreign->newObjectType();
    REQUIRE_THROWS_AS(pool->appendFieldWithExtension(root.id(), "foreign", [&] {
        pool->newScalarType(SchemaType::Kind::String);
        return foreignType;
    }), std::invalid_argument);
    REQUIRE(pool->typeCount() == types);
    REQUIRE(pool->strings()->size() == strings);
    REQUIRE_THROWS_AS(pool->appendExtension([&] {
        pool->appendFieldWithExtension(root.id(), "nested", [&] { return integer; });
    }), std::logic_error);
    REQUIRE_THROWS_AS(pool->appendFieldWithExtension(root.id(), "mutation", [&] {
        root.field("existing0").setRequired(true);
        return integer;
    }), std::logic_error);
    REQUIRE_FALSE(root.field("existing0").required().has_value());

    auto field = pool->appendFieldWithExtension(root.id(), "attached", [&] {
        auto child = pool->newObjectType();
        child.addField("payload", integer);
        return child;
    }, true, false);
    REQUIRE(field.type().field("payload").type() == integer);
    REQUIRE(field.multimap());
    REQUIRE(field.required() == false);
    REQUIRE(field.role() == SchemaType::FieldRole::Data);
    size_t called = 0;
    for (auto name : {"attached", "existing0", "alias"}) {
        REQUIRE_THROWS_AS(pool->appendFieldWithExtension(root.id(), name, [&] {
            ++called;
            return integer;
        }), std::invalid_argument);
    }
    REQUIRE(called == 0);
    pool->finalize();
    REQUIRE(root.directFields().size() == 301);
    REQUIRE(root.field("alias").type() == integer);
    for (size_t i = 0; i < 300; ++i)
        REQUIRE(root.field("existing" + std::to_string(i)).type() == integer);
    std::ostringstream encoded;
    REQUIRE(pool->write(encoded));
    auto bytes = encoded.str();
    auto restored = SchemaPool::read(std::span(reinterpret_cast<uint8_t const*>(bytes.data()), bytes.size()));
    auto restoredField = restored->schemaType(root.id()).field("attached");
    REQUIRE(restoredField.type().field("payload").type().id() == integer.id());
    REQUIRE(restoredField.multimap());
    REQUIRE(restoredField.required() == false);
}

TEST_CASE("Typed schema views construct and read canonical unpublished records", "[model.schema-type]")
{
    auto pool = std::make_shared<SchemaPool>();
    auto root = pool->newObjectType();
    auto scalar = pool->newScalarType(SchemaType::Kind::Int);
    auto array = pool->newArrayType();
    auto value = root.addField("value", scalar);
    value.setRequired(false);
    value.setMultimap(true);
    value.setRole(SchemaType::FieldRole::Context);
    scalar.setNullable(true);
    scalar.setTypeName("Measurement");
    array.addElement(scalar);
    array.addField("context", root).setRole(SchemaType::FieldRole::Context);
    root.addField("items", array);
    root.addField("", scalar);
    root.addAlias("valueAlias", "value");

    auto verify = [&](SchemaPool const& graph) {
        auto type = graph.schemaType(root.id());
        REQUIRE(type.kind() == SchemaType::Kind::Object);
        REQUIRE(type.field("valueAlias").name() == "value");
        REQUIRE(type.field("value").type().id() == scalar.id());
        REQUIRE(type.field("value").required() == false);
        REQUIRE(type.field("value").multimap());
        REQUIRE(type.field("value").role() == SchemaType::FieldRole::Context);
        REQUIRE(type.field("value").type().nullable() == true);
        REQUIRE(type.field("value").type().typeName() == "Measurement");
        REQUIRE(type.field(""));
        REQUIRE_FALSE(type.field("missing"));
        REQUIRE(type.fieldSchema(graph.strings()->get("value")) == scalar.id());
        REQUIRE(graph.schemaType(array.id()).itemSchema() == scalar.id());
        REQUIRE(std::ranges::equal(graph.schemaType(array.id()).elements(), std::array{scalar.id()}));
        std::vector<std::string_view> fields;
        type.forEachField([&](auto const& field) { fields.push_back(field.name()); });
        REQUIRE(fields == std::vector<std::string_view>{"value", "items", ""});
    };

    REQUIRE_FALSE(root.finalized());
    verify(*pool);
    pool->finalize();
    verify(*pool);
    std::ostringstream encoded;
    REQUIRE(pool->write(encoded));
    auto bytes = encoded.str();
    auto restored = SchemaPool::read(std::span(reinterpret_cast<uint8_t const*>(bytes.data()), bytes.size()));
    verify(*restored);
}

TEST_CASE("Typed field declarations preserve empty targets separately from unknown targets", "[model.schema-type]")
{
    auto pool = std::make_shared<SchemaPool>();
    auto object = pool->newObjectType();
    auto array = pool->newArrayType();
    auto integer = pool->newScalarType(SchemaType::Kind::Int);
    auto empty = object.declareField("empty");
    empty.setRequired(true);
    empty.setMultimap(true);
    empty.setRole(SchemaType::FieldRole::Context);
    object.addField("unknown");
    object.addField("known", integer);
    array.declareField("named").setRequired(false);
    array.addElement(integer);
    object.declareField("empty");
    object.declareField("known");
    REQUIRE(empty.types().empty());
    REQUIRE(empty.required() == true);
    REQUIRE(empty.multimap());
    REQUIRE(empty.role() == SchemaType::FieldRole::Context);
    REQUIRE(std::ranges::equal(object.field("unknown").types(), std::array{NoTypeId}));
    REQUIRE(std::ranges::equal(object.field("known").types(), std::array{integer.id()}));
    pool->finalize();
    std::ostringstream encoded;
    REQUIRE(pool->write(encoded));
    auto bytes = encoded.str();
    auto restored = SchemaPool::read(std::span(reinterpret_cast<uint8_t const*>(bytes.data()), bytes.size()));
    REQUIRE(restored->schemaType(object.id()).field("empty").types().empty());
    REQUIRE(restored->schemaType(object.id()).field("empty").required() == true);
    REQUIRE(restored->schemaType(array.id()).field("named").types().empty());
    REQUIRE(restored->schemaType(array.id()).field("named").required() == false);
    REQUIRE(std::ranges::equal(restored->schemaType(object.id()).field("unknown").types(), std::array{NoTypeId}));
}

TEST_CASE("Typed schema borrowing and retention have explicit lifetimes", "[model.schema-type]")
{
    auto pool = std::make_shared<SchemaPool>();
    auto root = pool->newObjectType();
    auto scalar = pool->newScalarType(SchemaType::Kind::Int);
    root.addField("value", scalar);
    std::weak_ptr<SchemaPool> weak = pool;
    REQUIRE(pool.use_count() == 1);
    auto borrowed = root;
    REQUIRE_FALSE(borrowed.ownsPool());
    REQUIRE(borrowed.field("value").type() == scalar);
    REQUIRE(pool.use_count() == 1);

    SchemaType retained;
    {
        auto owner = root.retain();
        REQUIRE(owner.ownsPool());
        REQUIRE(owner.asObject().ownsPool());
        REQUIRE(owner == borrowed);
        REQUIRE_FALSE(owner.borrow().ownsPool());
        retained = owner.field("value").type();
        REQUIRE(retained.ownsPool());
    }
    pool.reset();
    REQUIRE_FALSE(weak.expired());
    REQUIRE(retained.kind() == SchemaType::Kind::Int);
    retained = {};
    REQUIRE(weak.expired());

    SchemaPool stack;
    auto local = stack.newObjectType();
    REQUIRE_THROWS_AS(local.retain(), std::bad_weak_ptr);
    REQUIRE_FALSE(SchemaType{}.retain());
}

TEST_CASE("Typed schema mutation respects constness freeze and exact namespaces", "[model.schema-type]")
{
    auto pool = std::make_shared<SchemaPool>();
    auto root = pool->newObjectType();
    auto scalar = pool->newScalarType(SchemaType::Kind::Int);
    auto field = root.addField("value", scalar);
    auto readonly = std::as_const(*pool).schemaType(root.id()).asObject();
    auto count = pool->strings()->size();
    REQUIRE_THROWS_AS(readonly.addField("readonly", scalar), std::logic_error);
    REQUIRE(pool->strings()->size() == count);
    auto other = std::make_shared<SchemaPool>();
    auto foreign = other->newObjectType();
    REQUIRE(foreign.id() == root.id());
    REQUIRE_FALSE(foreign == root);
    REQUIRE_THROWS_AS(root.addField("foreign", foreign), std::invalid_argument);
    REQUIRE(pool->strings()->size() == count);
    REQUIRE_FALSE(root.field("foreign"));

    auto before = pool->typeCount();
    REQUIRE_THROWS_AS(pool->newObjectType(SchemaType::Kind::Int), std::invalid_argument);
    REQUIRE_THROWS_AS(pool->newArrayType(SchemaType::Kind::Object), std::invalid_argument);
    REQUIRE_THROWS_AS(pool->newBitmaskType(0), std::invalid_argument);
    REQUIRE_THROWS_AS(pool->newScalarType(SchemaType::Kind::Object), std::invalid_argument);
    REQUIRE_THROWS_AS(pool->newCompositionType(SchemaType::Composition::None), std::invalid_argument);
    REQUIRE(pool->typeCount() == before);
    REQUIRE_THROWS_AS(root.asArray(), std::invalid_argument);
    REQUIRE_THROWS_AS(scalar.asBitmask(), std::invalid_argument);

    pool->finalize();
    REQUIRE_THROWS_AS(root.addField("late", scalar), std::logic_error);
    REQUIRE_THROWS_AS(field.setRequired(true), std::logic_error);
    REQUIRE_THROWS_AS(field.setRole(SchemaType::FieldRole::Data), std::logic_error);
    REQUIRE_THROWS_AS(field.setMultimap(false), std::logic_error);
    REQUIRE_THROWS_AS(scalar.setNullable(false), std::logic_error);
    REQUIRE_THROWS_AS(pool->schemaType(root.id()).setTypeName("late"), std::logic_error);
    REQUIRE(pool->strings()->size() == count);
}

TEST_CASE("Typed enum bitmask and composition views keep independent semantics", "[model.schema-type]")
{
    auto pool = std::make_shared<SchemaPool>();
    auto enumeration = pool->newEnumType(SchemaType::Kind::Int);
    enumeration.addValue(int64_t{7});
    auto text = pool->newEnumType();
    text.addSymbol("READY");
    auto flags = pool->newBitmaskType(64);
    flags.addFlag("NONE", 0);
    flags.addFlag("HIGH", uint64_t{1} << 63);
    flags.addFlag("HIGH_ALIAS", uint64_t{1} << 63);
    auto small = pool->newBitmaskType(3);
    REQUIRE_THROWS_AS(small.addFlag("OUTSIDE", 8), std::invalid_argument);
    REQUIRE_FALSE(small.namedValue("OUTSIDE"));
    auto choice = pool->newCompositionType(SchemaType::Composition::OneOf);
    choice.addAlternative(enumeration);
    choice.addAlternative(text);
    choice.addAlternative(text);
    choice.asEnum().addSymbol("CHOICE");
    // The unfinished combiner remains structurally conservative, but owns its declarations.
    REQUIRE_THROWS_AS(choice.asScalar(), std::invalid_argument);
    choice.addNamedValue({.name = "CHOICE_VALUE", .value = std::string_view("READY")});
    pool->finalize();

    REQUIRE(std::ranges::equal(choice.alternatives(), std::array{enumeration.id(), text.id(), text.id()}));
    REQUIRE(choice.composition() == SchemaType::Composition::OneOf);
    REQUIRE(choice.hasAffinity(ValueType::Int));
    REQUIRE(choice.hasAffinity(ValueType::String));
    REQUIRE_FALSE(choice.hasAffinity(ValueType::Object));
    REQUIRE(std::get<std::string_view>(choice.namedValue("CHOICE_VALUE")->value) == "READY");
    REQUIRE_FALSE(text.namedValue("CHOICE_VALUE"));
    REQUIRE(flags.bitWidth() == 64);
    REQUIRE(std::get<uint64_t>(flags.namedValue("HIGH")->value) == uint64_t{1} << 63);
    REQUIRE(flags.namedValue("NONE")->role == SchemaType::NamedValue::Role::Flag);
    std::vector<SchemaType::EnumValue> literals;
    enumeration.forEachEnumValue([&](auto const& value) { literals.push_back(value); });
    REQUIRE(literals == std::vector<SchemaType::EnumValue>{int64_t{7}});
    REQUIRE(enumeration.enumValueCount() == 1);
    REQUIRE(enumeration.enumValueAt(0) == SchemaType::EnumValue{int64_t{7}});
    REQUIRE_THROWS_AS(enumeration.enumValueAt(1), std::out_of_range);
    REQUIRE(SchemaType{}.enumValueCount() == 0);
    REQUIRE_THROWS_AS(SchemaType{}.enumValueAt(0), std::out_of_range);
    std::vector<StringId> symbols;
    text.forEachEnumSymbol([&](auto name) { symbols.push_back(name); });
    REQUIRE(symbols == std::vector<StringId>{pool->strings()->get("READY")});
}

TEST_CASE("Typed numeric domains decode arena intervals before publication and replace safely", "[model.schema-type]")
{
    auto pool = std::make_shared<SchemaPool>();
    auto number = pool->newScalarType(SchemaType::Kind::Int);
    std::array<SchemaType::NumericRange, 3> ranges{{{int64_t{1}, int64_t{3}},
                                                 {uint64_t{8}, uint64_t{20}},
                                                 {0.5, 2.5}}};
    number.setNumericDomain({.encoding = "u64", .unit = "m",
        .ordinaryRanges = std::span<SchemaType::NumericRange const>(ranges), .scale = 0.5});
    auto verify = [&] {
        auto domain = number.numericDomain();
        REQUIRE(domain);
        REQUIRE(domain->encoding == "u64");
        REQUIRE(domain->unit == "m");
        REQUIRE(domain->scale == 0.5);
        REQUIRE(domain->ordinaryRanges.size() == 3);
        for (size_t i = 0; i < ranges.size(); ++i) {
            REQUIRE(domain->ordinaryRanges[i].min == ranges[i].min);
            REQUIRE(domain->ordinaryRanges[i].max == ranges[i].max);
        }
    };
    verify();
    number.setNumericDomain(*number.numericDomain());
    verify();
    pool->finalize();
    verify();
}

TEST_CASE("Typed inherited field views identify the declaration instead of changing the target", "[model.schema-type]")
{
    auto pool = std::make_shared<SchemaPool>();
    auto base = pool->newObjectType();
    auto object = pool->newObjectType();
    auto child = pool->newScalarType(SchemaType::Kind::Int);
    base.addField("inherited", child).setRequired(true);
    base.addField("shadow", child).setRole(SchemaType::FieldRole::Context);
    object.setBase(base);
    object.addField("shadow");
    REQUIRE(object.field("inherited").parentType() == base);
    REQUIRE(object.field("shadow").parentType() == object);
    REQUIRE(object.field("shadow").role() == SchemaType::FieldRole::Data);
    REQUIRE_FALSE(object.field("shadow").type());
    REQUIRE(object.field("inherited").type() == child);
    REQUIRE_FALSE(child.nullable().has_value());
    std::vector<std::string_view> fields;
    object.forEachField([&](auto const& field) { fields.push_back(field.name()); });
    REQUIRE(fields == std::vector<std::string_view>{"inherited", "shadow"});
}

TEST_CASE("Sealed type records reject every mutation path while clones remain writable", "[model.schema-type]")
{
    auto pool = std::make_shared<ExtensibleSchemaPool>();
    auto object = pool->newObjectType();
    auto scalar = pool->newScalarType(SchemaType::Kind::Int);
    auto field = object.addField("value", scalar);
    auto earlier = object;
    object.seal();
    object.seal();
    REQUIRE(object.sealed());
    REQUIRE_FALSE(object.finalized());
    REQUIRE_THROWS_AS(earlier.addField("late", scalar), std::logic_error);
    REQUIRE_THROWS_AS(field.setRequired(false), std::logic_error);
    REQUIRE_THROWS_AS(pool->addFieldSchema(object.id(), "bypass", scalar.id()), std::logic_error);
    REQUIRE_THROWS_AS(pool->setOpen(object.id(), true), std::logic_error);
    REQUIRE_THROWS_AS(object.setKindName(StringPool::SchemaArray), std::logic_error);
    REQUIRE_THROWS_AS(pool->applyScalarAnnotations(object.id(), {}, {}, {}, {}), std::logic_error);
    REQUIRE_THROWS_AS(std::as_const(*pool).schemaType(scalar.id()).seal(), std::logic_error);
    auto copy = pool->cloneType(object).asObject();
    REQUIRE_FALSE(copy.sealed());
    copy.addField("additional", scalar);
    REQUIRE_FALSE(object.field("additional"));
    copy.setKindName(StringPool::SchemaValue);
    REQUIRE(copy.hasAffinity(ValueType::Object));
    REQUIRE(copy.kind() == SchemaType::makeKind(StringPool::SchemaValue, valueTypeAffinity(ValueType::Object)));
    REQUIRE_THROWS_AS(copy.setKindName(StringPool::FirstDynamicId), std::invalid_argument);
    pool->finalize();
    REQUIRE(scalar.sealed());
    REQUIRE(copy.sealed());
}

TEST_CASE("Same-pool type cloning owns direct collections but shares child identities", "[model.schema-type]")
{
    auto pool = std::make_shared<SchemaPool>();
    auto root = pool->newObjectType();
    auto child = pool->newScalarType(SchemaType::Kind::Int);
    root.addField("recursive", root);
    root.addField("unknown");
    root.declareField("empty").setRequired(false);
    for (size_t i = 0; i < 4200; ++i)
        root.addField("field" + std::to_string(i), child);
    root.addAlias("alias", "field0");
    root.field("field0").setRole(SchemaType::FieldRole::Context);
    root.field("field0").setMultimap(true);
    auto copy = pool->cloneType(root).asObject();
    REQUIRE(copy.id() != root.id());
    REQUIRE(copy.field("recursive").type() == root);
    REQUIRE(copy.field("alias").type() == child);
    REQUIRE(copy.field("alias").role() == SchemaType::FieldRole::Context);
    REQUIRE(copy.field("alias").multimap());
    REQUIRE(copy.field("empty").types().empty());
    REQUIRE(std::ranges::equal(copy.field("unknown").types(), std::array{NoTypeId}));
    for (size_t i = 0; i < 4200; ++i)
        REQUIRE(copy.field("field" + std::to_string(i)).type() == child);
    copy.field("field0").addType(copy);
    copy.setNullable(true);
    REQUIRE(copy.field("field0").types().size() == 2);
    REQUIRE(root.field("field0").types().size() == 1);
    REQUIRE_FALSE(root.nullable());
    REQUIRE(copy.nullable() == true);
    pool->finalize();
}

TEST_CASE("Failed schema imports restore type dictionary arena and primitive tails", "[model.schema-type]")
{
    auto source = std::make_shared<SchemaPool>();
    auto imported = source->newObjectType();
    auto literal = source->newEnumType();
    literal.addSymbol("IMPORTED");
    imported.setTypeName("ImportedType");
    imported.addField("self", imported);
    imported.addField("choice", literal);
    source->finalize();

    auto target = std::make_shared<ExtensibleSchemaPool>();
    auto original = target->newObjectType();
    original.setTypeName("OriginalType");
    auto highest = target->strings()->highest();
    auto count = target->typeCount();
    std::ostringstream before;
    REQUIRE(target->ModelPool::write(before));
    auto failure = [&](std::span<TypeId const> remap) {
        REQUIRE(remap[imported.id()] > original.id());
        REQUIRE(target->schemaType(remap[imported.id()]).field("self").type().id() == remap[imported.id()]);
        target->newObjectType().setTypeName("CallbackType");
        target->addRoot(target->newValue(std::string_view("callback payload")));
        REQUIRE(target->strings()->emplace("callback-field"));
        throw std::runtime_error("extension rejected");
    };
    REQUIRE_THROWS_AS(target->importGraphWithExtension(*source, {}, failure), std::runtime_error);
    REQUIRE(target->typeCount() == count);
    REQUIRE(target->strings()->highest() == highest);
    REQUIRE(target->strings()->get("IMPORTED") == StringPool::Empty);
    REQUIRE(original.typeName() == "OriginalType");
    std::ostringstream after;
    REQUIRE(target->ModelPool::write(after));
    REQUIRE(before.str() == after.str());

    auto control = std::make_shared<ExtensibleSchemaPool>();
    control->newObjectType().setTypeName("OriginalType");
    REQUIRE(target->importGraph(*source) == control->importGraph(*source));
    target->finalize();
    control->finalize();
    std::ostringstream retried, clean;
    REQUIRE(target->write(retried));
    REQUIRE(control->write(clean));
    REQUIRE(retried.str() == clean.str());
}

TEST_CASE("Schema import callbacks cannot mutate or publish pre-existing records", "[model.schema-type]")
{
    auto source = std::make_shared<SchemaPool>();
    source->newObjectType();
    source->finalize();
    auto target = std::make_shared<ExtensibleSchemaPool>();
    auto original = target->newObjectType();
    auto highest = target->strings()->highest();
    SECTION("Existing records are immutable inside the append boundary") {
        REQUIRE_THROWS_AS(target->importGraphWithExtension(*source, {}, [&](auto) {
            original.addField("forbidden");
        }), std::logic_error);
    }
    SECTION("Publication cannot escape a still-active append") {
        REQUIRE_THROWS_AS(target->importGraphWithExtension(*source, {}, [&](auto) {
            target->finalize();
        }), std::logic_error);
    }
    REQUIRE(target->typeCount() == 1);
    REQUIRE(target->strings()->highest() == highest);
    REQUIRE_FALSE(target->finalized());
    REQUIRE_FALSE(original.field("forbidden"));
    original.addField("after");
    target->finalize();
}

TEST_CASE("Scalar annotations publish atomically alongside appended extension types", "[model.schema-type]")
{
    auto pool = std::make_shared<ExtensibleSchemaPool>();
    auto number = pool->newScalarType(SchemaType::Kind::Int);
    std::array<SchemaType::NumericRange, 1> ranges{{{int64_t{1}, int64_t{10}}}};
    number.setNumericDomain({.unit = "m", .ordinaryRanges = std::span<SchemaType::NumericRange const>(ranges)});
    number.addNamedValue({.name = "ORIGINAL", .value = int64_t{7}});
    std::array<SchemaType::NamedValue, 1> added{{{.name = "ADDED", .value = uint64_t{UINT64_MAX},
                                              .rawValue = uint64_t{UINT64_MAX}}}};
    SchemaType::NumericDomain next{.unit = "cm", .minimum = int64_t{2}, .scale = 0.01};
    auto highest = pool->strings()->highest();
    REQUIRE_THROWS_AS(pool->applyScalarAnnotations(number.id(), next, {}, added, [&] {
        REQUIRE(number.numericDomain()->unit == "m");
        REQUIRE_FALSE(number.namedValue("ADDED"));
        pool->newObjectType().addField("nested");
        throw std::runtime_error("extension rejected");
    }), std::runtime_error);
    REQUIRE(pool->typeCount() == 1);
    REQUIRE(pool->strings()->highest() == highest);
    REQUIRE(number.numericDomain()->unit == "m");
    REQUIRE(number.numericDomain()->ordinaryRanges.size() == 1);
    REQUIRE_FALSE(number.namedValue("ADDED"));
    pool->applyScalarAnnotations(number.id(), next, {}, added, [&] {
        pool->newObjectType().addField("nested");
    });
    REQUIRE(pool->typeCount() == 2);
    REQUIRE(number.numericDomain()->unit == "cm");
    REQUIRE(number.numericDomain()->ordinaryRanges.empty());
    REQUIRE(std::get<uint64_t>(*number.namedValue("ADDED")->rawValue) == UINT64_MAX);
    REQUIRE_THROWS_AS(pool->applyScalarAnnotations(number.id(), {}, {}, added, {}), std::invalid_argument);
    pool->finalize();
    REQUIRE(pool->schemaStatistics().numericDomains == 1);
    REQUIRE(pool->schemaStatistics().namedValues == 2);
    std::ostringstream output;
    REQUIRE(pool->write(output));
    auto bytes = output.str();
    auto restored = SchemaPool::read(std::span(reinterpret_cast<uint8_t const*>(bytes.data()), bytes.size()));
    REQUIRE(restored->schemaType(number.id()).numericDomain()->unit == "cm");
    REQUIRE(restored->schemaType(number.id()).namedValue("ORIGINAL"));
    REQUIRE(std::get<uint64_t>(*restored->schemaType(number.id()).namedValue("ADDED")->rawValue) == UINT64_MAX);
}
