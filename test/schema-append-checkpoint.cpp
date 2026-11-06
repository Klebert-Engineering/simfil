// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#include <catch2/catch_test_macros.hpp>
#include <bitsery/adapter/buffer.h>
#include <bitsery/bitsery.h>
#include <bitsery/traits/string.h>
#include <bitsery/traits/vector.h>
#include <array>
#include <concepts>
#include <limits>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "simfil/model/arena.h"
#include "simfil/model/bitsery-traits.h"
#include "simfil/model/schema-pool.h"

using namespace simfil;

namespace
{
/** Exercise the protected plain-scalar contract without adding a production test hook. */
class ScalarCheckpointPool final : public ModelPool
{
public:
    using ModelPool::ScalarAppendCheckpoint;
    using ModelPool::scalarAppendCheckpoint;
    using ModelPool::rollbackScalarAppends;
};

/** Drive private dictionary rollback through the real generic/extension import boundary. */
class ExtensionCheckpointPool final : public SchemaPool
{
public:
    using SchemaPool::SchemaPool;
    using SchemaPool::importGraphWithExtension;
};
}

TEST_CASE("Arena append checkpoints restore only newly allocated array tails", "[schema.append-checkpoint][ArrayArena]")
{
    auto check = []<typename Value>() {
        using Arena = ArrayArena<Value, 4, 2>;
        STATIC_REQUIRE(std::copyable<typename Arena::AppendCheckpoint>);
        STATIC_REQUIRE_FALSE(std::default_initializable<typename Arena::AppendCheckpoint>);
        STATIC_REQUIRE(noexcept(std::declval<Arena&>().rollbackAppends(
            std::declval<typename Arena::AppendCheckpoint const&>())));
        auto value = [](uint32_t i) {
            if constexpr (std::same_as<Value, uint32_t>) return i;
            else return Value{i, uint64_t(i) * 10};
        };
        auto asValue = [](auto const& item) { return static_cast<Value>(item); };

        Arena arena;
        auto const oldArray = arena.new_array(1);
        auto const oldSingleton = arena.new_array(1, true);
        auto const oldEmpty = arena.new_array(1, true);
        for (uint32_t i = 0; i < 31; ++i) arena.push_back(oldArray, value(i));
        arena.push_back(oldSingleton, value(99));
        auto const before = arena.memory_usage();
        auto const headCount = arena.size();
        auto const singletonCount = arena.singleton_handle_count();

        using Buffer = std::vector<uint8_t>;
        using Output = bitsery::OutputBufferAdapter<Buffer>;
        Buffer originalBytes;
        bitsery::quickSerialization<Output>(originalBytes, arena);
        auto checkpoint = arena.checkpointAppends();
        auto copiedCheckpoint = checkpoint;
        copiedCheckpoint = checkpoint;

        auto const first = arena.new_array(0);
        auto const second = arena.new_array(1);
        for (uint32_t i = 0; i < 137; ++i) {
            arena.push_back(first, value(1000 + i));
            arena.push_back(second, value(2000 + i));
        }
        std::array<ArrayIndex, 11> singletons;
        for (size_t i = 0; i < singletons.size(); ++i) {
            singletons[i] = arena.new_array(1, true);
            if (i % 2) arena.push_back(singletons[i], value(static_cast<uint32_t>(i)));
        }
        auto const grown = arena.memory_usage();
        REQUIRE(grown.allocatedBytes > before.allocatedBytes);
        arena.rollbackAppends(copiedCheckpoint);
        REQUIRE(arena.size() == headCount);
        REQUIRE(arena.singleton_handle_count() == singletonCount);
        REQUIRE(arena.singleton_occupied_count() == 1);
        REQUIRE(arena.memory_usage().logicalBytes == before.logicalBytes);
        REQUIRE(arena.memory_usage().allocatedBytes == grown.allocatedBytes);
        REQUIRE_FALSE(arena.valid(first));
        REQUIRE_FALSE(arena.valid(second));
        for (auto handle : singletons) REQUIRE_FALSE(arena.valid(handle));
        REQUIRE(arena.range(oldEmpty).empty());
        REQUIRE(asValue(arena.range(oldSingleton).front()) == value(99));
        for (uint32_t i = 0; i < 31; ++i) REQUIRE(asValue(arena.range(oldArray)[i]) == value(i));

        Buffer restoredBytes;
        bitsery::quickSerialization<Output>(restoredBytes, arena);
        REQUIRE(restoredBytes == originalBytes);
        arena.rollbackAppends(checkpoint);
        auto const replacement = arena.new_array(1);
        REQUIRE(replacement == first);
        REQUIRE(arena.range(replacement).empty());
        for (uint32_t i = 0; i < 73; ++i) arena.push_back(replacement, value(3000 + i));
        REQUIRE(asValue(arena.range(replacement).back()) == value(3072));
        auto const replacementSingleton = arena.new_array(1, true);
        REQUIRE(replacementSingleton == singletons.front());
        REQUIRE(arena.range(replacementSingleton).empty());
        arena.push_back(replacementSingleton, value(123));
        REQUIRE(asValue(arena.range(replacementSingleton).front()) == value(123));
        arena.rollbackAppends(checkpoint);
        REQUIRE(arena.memory_usage().logicalBytes == before.logicalBytes);
    };
    check.template operator()<uint32_t>();
    check.template operator()<TwoPart<uint32_t, uint64_t>>();
}

TEST_CASE("Arena append checkpoints support empty and nested append scopes", "[schema.append-checkpoint][ArrayArena]")
{
    ArrayArena<uint32_t, 4, 2> arena;
    auto const empty = arena.checkpointAppends();
    auto const first = arena.new_array(0);
    auto const singleton = arena.new_array(1, true);
    arena.push_back(first, 1);
    auto const inner = arena.checkpointAppends();
    auto const second = arena.new_array(100);
    arena.push_back(second, 2);
    arena.new_array(1, true);
    arena.rollbackAppends(inner);
    REQUIRE(arena.range(first).front() == 1);
    REQUIRE(arena.range(singleton).empty());
    REQUIRE_FALSE(arena.valid(second));
    arena.rollbackAppends(empty);
    REQUIRE(arena.size() == FirstRegularArrayIndex);
    REQUIRE(arena.singleton_handle_count() == 0);
    REQUIRE(arena.begin() == arena.end());
    REQUIRE(arena.new_array(0) == first);
    REQUIRE(arena.new_array(1, true) == singleton);
    REQUIRE(arena.range(singleton).empty());
}

TEST_CASE("Compact arenas reject append checkpoints without materialization", "[schema.append-checkpoint][ArrayArena]")
{
    using Arena = ArrayArena<uint32_t, 4, 2>;
    Arena source;
    auto const values = source.new_array(1);
    for (uint32_t i = 0; i < 17; ++i) source.push_back(values, i);
    using Buffer = std::vector<uint8_t>;
    using Output = bitsery::OutputBufferAdapter<Buffer>;
    using Input = bitsery::InputBufferAdapter<Buffer>;
    Buffer bytes;
    auto const written = bitsery::quickSerialization<Output>(bytes, source);
    Arena compact;
    auto const state = bitsery::quickDeserialization<Input>({bytes.begin(), written}, compact);
    REQUIRE(state.first == bitsery::ReaderError::NoError);
    REQUIRE(state.second);
    REQUIRE(compact.is_compact());
    auto const before = compact.memory_usage();
    REQUIRE_THROWS_AS(compact.checkpointAppends(), std::logic_error);
    REQUIRE(compact.is_compact());
    REQUIRE(compact.memory_usage().logicalBytes == before.logicalBytes);
    REQUIRE(compact.memory_usage().allocatedBytes == before.allocatedBytes);
    REQUIRE(std::ranges::equal(compact.range(values), source.range(values)));
    Buffer after;
    bitsery::quickSerialization<Output>(after, compact);
    REQUIRE(after == bytes);
}

TEST_CASE("Scalar append checkpoints restore rows bytes roots and reusable addresses", "[schema.append-checkpoint][model.scalar]")
{
    auto pool = std::make_shared<ScalarCheckpointPool>();
    auto const oldInteger = pool->newValue(int64_t{1} << 40)->addr();
    auto const oldDouble = pool->newValue(1.25)->addr();
    auto const oldString = pool->newValue(std::string_view("existing text"))->addr();
    auto const oldBytes = pool->newValue(ByteArray{std::string("\0\xff", 2)})->addr();
    for (auto address : {oldInteger, oldDouble, oldString, oldBytes}) pool->addRoot(pool->resolve(address));
    auto const before = pool->memoryUsageStats();
    auto checkpoint = pool->scalarAppendCheckpoint();
    auto copy = checkpoint;
    copy = checkpoint;
    STATIC_REQUIRE(noexcept(pool->rollbackScalarAppends(checkpoint)));
    REQUIRE(checkpoint.roots == 4);
    REQUIRE(checkpoint.int64Values == 1);
    REQUIRE(checkpoint.doubleValues == 1);
    REQUIRE(checkpoint.stringRows == 1);
    REQUIRE(checkpoint.byteArrayRows == 1);
    REQUIRE(checkpoint.stringBytes == 15);

    // Cross every scalar column's page boundary, and force shared byte-buffer growth.
    for (size_t i = 0; i <= detail::ColumnPageSize; ++i) {
        pool->addRoot(pool->newValue((int64_t{1} << 41) + static_cast<int64_t>(i)));
        pool->addRoot(pool->newValue(2.5 + static_cast<double>(i)));
        pool->addRoot(pool->newValue(std::string_view("appended string")));
        pool->addRoot(pool->newValue(ByteArray{std::string("\0new", 4)}));
    }
    auto const grown = pool->memoryUsageStats();
    REQUIRE(grown.total().allocatedBytes > before.total().allocatedBytes);
    pool->rollbackScalarAppends(copy);
    auto const restored = pool->scalarAppendCheckpoint();
    REQUIRE(restored.roots == checkpoint.roots);
    REQUIRE(restored.int64Values == checkpoint.int64Values);
    REQUIRE(restored.doubleValues == checkpoint.doubleValues);
    REQUIRE(restored.stringBytes == checkpoint.stringBytes);
    REQUIRE(restored.stringRows == checkpoint.stringRows);
    REQUIRE(restored.byteArrayRows == checkpoint.byteArrayRows);
    REQUIRE(pool->memoryUsageStats().total().logicalBytes == before.total().logicalBytes);
    REQUIRE(pool->memoryUsageStats().total().allocatedBytes == grown.total().allocatedBytes);
    REQUIRE(std::get<int64_t>(pool->resolve(oldInteger)->value()) == (int64_t{1} << 40));
    REQUIRE(std::get<double>(pool->resolve(oldDouble)->value()) == 1.25);
    REQUIRE(std::get<std::string_view>(pool->resolve(oldString)->value()) == "existing text");
    REQUIRE(std::get<ByteArray>(pool->resolve(oldBytes)->value()).bytes == std::string("\0\xff", 2));
    REQUIRE(pool->root(3).value()->addr() == oldBytes);
    REQUIRE(pool->newValue(int64_t{1} << 42)->addr().index() == checkpoint.int64Values);
    REQUIRE(pool->newValue(3.5)->addr().index() == checkpoint.doubleValues);
    REQUIRE(pool->newValue(std::string_view("replacement"))->addr().index() == checkpoint.stringRows);
    REQUIRE(pool->newValue(ByteArray{std::string("bytes")})->addr().index() == checkpoint.byteArrayRows);
    pool->rollbackScalarAppends(checkpoint);
    REQUIRE(pool->numRoots() == 4);

    auto empty = std::make_shared<ScalarCheckpointPool>();
    auto const emptyCheckpoint = empty->scalarAppendCheckpoint();
    empty->addRoot(empty->newValue(int64_t{7}));
    empty->rollbackScalarAppends(emptyCheckpoint);
    REQUIRE(empty->numRoots() == 0);
    empty->freeze();
    REQUIRE_THROWS_AS(empty->scalarAppendCheckpoint(), std::logic_error);
}

TEST_CASE("Failed extension imports restore private dictionary tails and scalar text indexes", "[schema.append-checkpoint][model.schema-pool]")
{
    auto source = std::make_shared<SchemaPool>();
    auto importedObject = source->newObjectType();
    auto importedScalar = source->newScalarType(SchemaType::Kind::Int);
    importedScalar.setTypeName("imported integer");
    importedScalar.addNamedValue({"IMPORTED_CONSTANT", int64_t{1} << 40});
    importedObject.addField("imported-field", importedScalar);
    source->finalize();

    auto target = std::make_shared<ExtensionCheckpointPool>();
    auto existingObject = target->newObjectType();
    auto existingScalar = target->newScalarType(SchemaType::Kind::Int);
    existingScalar.setTypeName("existing integer");
    existingObject.addField("existing-field", existingScalar);
    auto const dictionary = target->strings();
    auto const highest = dictionary->highest();
    auto const dictionarySize = dictionary->size();
    auto const storedStrings = dictionary->strings().size();
    auto const bytes = dictionary->bytes();
    auto const oldTypes = target->typeCount();
    auto const scalarBytes = target->serializationSizeStats().totalBytes();
    auto const schemaBytes = target->schemaMemoryUsage().logicalBytes;
    ArrayArena<uint32_t, 4, 2> extension;
    auto const existingExtension = extension.new_array(1);
    extension.push_back(existingExtension, 42);

    for (int attempt = 0; attempt < 3; ++attempt) {
        bool called = false;
        StringId importedName = 0, extensionName = 0;
        auto import = [&] {
            auto const checkpoint = extension.checkpointAppends();
            try {
                return target->importGraphWithExtension(*source, {}, [&](std::span<TypeId const> remap) {
                    called = true;
                    REQUIRE(remap.size() == source->typeCount() + 1);
                    REQUIRE(remap[importedObject.id()] != NoTypeId);
                    importedName = dictionary->get("imported-field");
                    extensionName = dictionary->emplace("extension-only-key").value();
                    auto const added = extension.new_array(1);
                    for (uint32_t i = 0; i < 37; ++i) extension.push_back(added, i);
                    throw std::runtime_error("extension rejected after appending");
                });
            } catch (...) {
                extension.rollbackAppends(checkpoint);
                throw;
            }
        };
        REQUIRE_THROWS_AS(import(), std::runtime_error);
        REQUIRE(called);
        REQUIRE(importedName > highest);
        REQUIRE(extensionName > highest);
        REQUIRE(target->typeCount() == oldTypes);
        REQUIRE(target->serializationSizeStats().totalBytes() == scalarBytes);
        REQUIRE(target->schemaMemoryUsage().logicalBytes == schemaBytes);
        REQUIRE(dictionary->highest() == highest);
        REQUIRE(dictionary->size() == dictionarySize);
        REQUIRE(dictionary->strings().size() == storedStrings);
        REQUIRE(dictionary->bytes() == bytes);
        REQUIRE(dictionary->get("imported-field") == StringPool::Empty);
        REQUIRE(dictionary->get("extension-only-key") == StringPool::Empty);
        REQUIRE_FALSE(dictionary->resolve(importedName));
        REQUIRE_FALSE(dictionary->resolve(extensionName));
        REQUIRE(dictionary->resolve(StringPool::SchemaObject) == "object");
        REQUIRE(existingObject.field("existing-field").type() == existingScalar);
        REQUIRE(existingScalar.typeName() == "existing integer");
        REQUIRE(extension.size() == FirstRegularArrayIndex + 1);
        REQUIRE(extension.range(existingExtension).front() == 42);
        StringPool dictionaryCopy(*dictionary);
        REQUIRE(dictionaryCopy == *dictionary);
    }

    REQUIRE(dictionary->emplace("replacement-key").value() == highest + 1);
    auto replacement = target->newScalarType(SchemaType::Kind::String);
    replacement.setTypeName("replacement in reclaimed text row");
    auto const remap = target->importGraphWithExtension(*source, {}, [&](std::span<TypeId const>) {
        auto added = extension.new_array(1);
        extension.push_back(added, 43);
    });
    REQUIRE(target->schemaType(remap[importedScalar.id()]).typeName() == "imported integer");
    REQUIRE(replacement.typeName() == "replacement in reclaimed text row");
    REQUIRE(target->schemaType(remap[importedObject.id()]).field("imported-field").type().id() == remap[importedScalar.id()]);
    REQUIRE(extension.size() == FirstRegularArrayIndex + 2);
}

TEST_CASE("Dictionary rollback returns the last uint16 ID for reuse", "[schema.append-checkpoint][model.string-pool]")
{
    auto dictionary = std::make_shared<StringPool>();
    using Buffer = std::vector<uint8_t>;
    Buffer bytes;
    bitsery::Serializer<bitsery::OutputBufferAdapter<Buffer>> writer(bytes);
    uint16_t count = 1;
    auto id = static_cast<StringId>(std::numeric_limits<StringId>::max() - 1);
    std::string text = "near-id-limit";
    writer.value2b(count);
    writer.value2b(id);
    writer.text1b(text, std::numeric_limits<uint16_t>::max());
    writer.adapter().flush();
    REQUIRE(dictionary->read(bytes));
    REQUIRE(dictionary->highest() == id);
    auto target = std::make_shared<ExtensionCheckpointPool>(dictionary);
    auto source = std::make_shared<SchemaPool>();
    source->finalize();
    auto const stored = dictionary->strings().size();
    auto const dictionarySize = dictionary->size();
    for (int attempt = 0; attempt < 3; ++attempt) {
        auto import = [&] {
            target->importGraphWithExtension(*source, {}, [&](std::span<TypeId const>) {
                REQUIRE(dictionary->emplace("last-id-rejected").value() == std::numeric_limits<StringId>::max());
                throw std::runtime_error("reject the last ID");
            });
        };
        REQUIRE_THROWS_AS(import(), std::runtime_error);
        REQUIRE(dictionary->highest() == id);
        REQUIRE(dictionary->strings().size() == stored);
        REQUIRE(dictionary->size() == dictionarySize);
        REQUIRE(dictionary->get("last-id-rejected") == StringPool::Empty);
        REQUIRE_FALSE(dictionary->resolve(std::numeric_limits<StringId>::max()));
    }
    target->importGraphWithExtension(*source, {}, [&](std::span<TypeId const>) {
        REQUIRE(dictionary->emplace("last-id-accepted").value() == std::numeric_limits<StringId>::max());
    });
    REQUIRE(dictionary->resolve(std::numeric_limits<StringId>::max()) == "last-id-accepted");
    REQUIRE_FALSE(dictionary->emplace("overflow"));
    REQUIRE(dictionary->highest() == std::numeric_limits<StringId>::max());
}
