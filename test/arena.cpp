#include <catch2/catch_test_macros.hpp>
#include <thread>
#include <bitsery/bitsery.h>
#include <bitsery/adapter/buffer.h>
#include <bitsery/traits/vector.h>

#include "simfil/model/arena.h"
#include "simfil/model/bitsery-traits.h"
#include "simfil/model/nodes.h"

using namespace simfil;

TEST_CASE("Model size checks are non-throwing predicates", "[ModelColumn][limits]")
{
    STATIC_REQUIRE(noexcept(ModelNodeAddress::checkIndex(0)));
    STATIC_REQUIRE(noexcept(ModelColumn<uint64_t>::check_size(0)));
    REQUIRE(ModelNodeAddress::checkIndex(0xffffff));
    REQUIRE_FALSE(ModelNodeAddress::checkIndex(0x1000000));
    REQUIRE_FALSE(ModelNodeAddress::checkIndex(std::numeric_limits<size_t>::max()));
    REQUIRE(ModelNodeAddress(1, 0xffffff).index() == 0xffffff);

    auto const maximum = bitsery_max_column_payload_bytes / sizeof(uint64_t);
    REQUIRE(ModelColumn<uint64_t>::check_size(maximum));
    REQUIRE_FALSE(ModelColumn<uint64_t>::check_size(maximum + 1));
    REQUIRE(check_model_column_growth(maximum - 1, 1, sizeof(uint64_t)));
    REQUIRE_FALSE(check_model_column_growth(maximum, 1, sizeof(uint64_t)));
    REQUIRE_FALSE(check_model_column_growth(1, std::numeric_limits<size_t>::max(), 8));
    REQUIRE_FALSE(check_model_column_growth(std::numeric_limits<size_t>::max(), 1, 8));
    REQUIRE_FALSE(check_model_column_growth(0, 1, 0));
    using Split = ModelColumn<TwoPart<uint32_t, uint64_t>>;
    REQUIRE(Split::check_size(bitsery_max_column_payload_bytes / 12));
    REQUIRE_FALSE(Split::check_size(bitsery_max_column_payload_bytes / 12 + 1));
}

TEST_CASE("Column readers reject invalid payloads before allocation", "[ModelColumn][limits]")
{
    auto checkReader = []<typename Column>() {
        std::vector<uint8_t> input(Column::record_size, 0);
        for (auto const declaredSize : {size_t{100000008}, size_t{1},
                 bitsery_max_column_payload_bytes + size_t{1}, std::numeric_limits<size_t>::max()}) {
            Column column;
            column.resize(1);
            auto const allocatedBefore = column.memory_usage().allocatedBytes;
            bitsery::InputBufferAdapter<std::vector<uint8_t>> adapter(input.begin(), input.size());
            REQUIRE_FALSE(column.read_payload_from_bitsery(adapter, declaredSize));
            REQUIRE(adapter.error() != bitsery::ReaderError::NoError);
            REQUIRE(column.empty());
            REQUIRE(column.memory_usage().allocatedBytes <= allocatedBefore);

            // A rejected read does not poison the column for the next input.
            bitsery::InputBufferAdapter<std::vector<uint8_t>> valid(input.begin(), input.size());
            REQUIRE(column.read_payload_from_bitsery(valid, input.size()));
            REQUIRE(column.size() == 1);
            REQUIRE(valid.error() == bitsery::ReaderError::NoError);
        }
        Column column;
        column.resize(1);
        auto const invalid = column.assign_bytes(std::span<const uint8_t>(input).first(1));
        REQUIRE_FALSE(invalid);
        REQUIRE(invalid.error() == model_column_io_error::payload_size_mismatch);
        REQUIRE(column.size() == 1);
    };
    checkReader.template operator()<ModelColumn<uint64_t>>();
    checkReader.template operator()<ModelColumn<uint64_t, 256, vector_column_storage>>();
    checkReader.template operator()<ModelColumn<TwoPart<uint32_t, uint64_t>>>();
}

TEST_CASE("Array arena capacity preflight does not allocate or mutate storage", "[ArrayArena][limits]")
{
    ArrayArena<uint32_t, 8, 8> arena;
    auto first = arena.new_array(200);
    arena.emplace_back(first, 7);
    auto const heads = arena.size();
    auto const allocatedBefore = arena.memory_usage().allocatedBytes;
    REQUIRE_FALSE(arena.check_capacity(size_t{0xffffffffU}));
    REQUIRE_FALSE(arena.check_capacity(std::numeric_limits<size_t>::max()));
    REQUIRE(arena.size() == heads);
    REQUIRE(arena.memory_usage().allocatedBytes == allocatedBefore);
    REQUIRE(arena.size(first) == 1);
    REQUIRE(arena.at(first, 0).value() == 7);
    REQUIRE(arena.check_capacity(55));
    auto second = arena.new_array(55);
    arena.emplace_back(second, 9);
    REQUIRE(arena.at(second, 0).value() == 9);

    using Arena = ArrayArena<uint32_t, 8, 8>;
    STATIC_REQUIRE(noexcept(Arena::check_data_growth(0, 0)));
    auto const maximum = bitsery_max_column_payload_bytes / sizeof(uint32_t);
    REQUIRE(Arena::check_data_growth(maximum - 55, 55));
    REQUIRE_FALSE(Arena::check_data_growth(maximum - 55, 56));
    REQUIRE_FALSE(Arena::check_data_growth(std::numeric_limits<uint32_t>::max(), 1));
    REQUIRE_FALSE(Arena::check_data_growth(std::numeric_limits<size_t>::max(), 1));
}

TEST_CASE("ArrayArena basic functionality", "[ArrayArena]") {
    ArrayArena<int> arena;

    SECTION("new_array") {
        ArrayIndex array1 = arena.new_array(2);
        ArrayIndex array2 = arena.new_array(4);

        REQUIRE(array1 == FirstRegularArrayIndex);
        REQUIRE(array2 == FirstRegularArrayIndex + 1);
    }

    SECTION("size") {
        ArrayIndex array1 = arena.new_array(2);
        REQUIRE(arena.size(array1) == 0);
    }

    SECTION("push_back and at") {
        ArrayIndex array1 = arena.new_array(2);
        arena.push_back(array1, 42);
        arena.push_back(array1, 43);
        REQUIRE(arena.size(array1) == 2);
        REQUIRE(arena.at(array1, 0) == 42);
        REQUIRE(arena.at(array1, 1) == 43);
    }

    SECTION("emplace_back") {
        ArrayIndex array1 = arena.new_array(2);
        arena.emplace_back(array1, 42);
        arena.emplace_back(array1, 43);
        REQUIRE(arena.size(array1) == 2);
        REQUIRE(arena.at(array1, 0) == 42);
        REQUIRE(arena.at(array1, 1) == 43);
    }

    SECTION("array capacity growth") {
        ArrayIndex array1 = arena.new_array(2);
        arena.push_back(array1, 42);
        arena.push_back(array1, 43);
        arena.push_back(array1, 44);
        REQUIRE(arena.size(array1) == 3);
        REQUIRE(arena.at(array1, 0) == 42);
        REQUIRE(arena.at(array1, 1) == 43);
        REQUIRE(arena.at(array1, 2) == 44);
    }

    SECTION("array with empty initial capacity") {
        ArrayIndex array1 = arena.new_array(0);
        arena.push_back(array1, 42);
        arena.push_back(array1, 43);
        REQUIRE(arena.size(array1) == 2);
        REQUIRE(arena.at(array1, 0) == 42);
        REQUIRE(arena.at(array1, 1) == 43);
    }
}

TEST_CASE("ArrayArena clear and shrink_to_fit", "[ArrayArena]") {
    ArrayArena<int> arena;
    ArrayIndex array1 = arena.new_array(2);
    arena.push_back(array1, 42);
    arena.push_back(array1, 43);
    arena.push_back(array1, 44);

    SECTION("clear") {
        arena.clear();
        ArrayIndex array2 = arena.new_array(2);
        REQUIRE(array2 == FirstRegularArrayIndex);
        REQUIRE(!arena.at(array1, 0));
        REQUIRE(arena.at(array1, 0).error().type == Error::IndexOutOfRange);
    }

    SECTION("shrink_to_fit") {
        arena.shrink_to_fit();
        REQUIRE(arena.size(array1) == 3);
        REQUIRE(arena.at(array1, 0) == 42);
        REQUIRE(arena.at(array1, 1) == 43);
        REQUIRE(arena.at(array1, 2) == 44);
    }
}

TEST_CASE("ModelColumn and ArrayArena report retained capacity", "[memory][ModelColumn][ArrayArena]")
{
    ModelColumn<uint32_t, 8> column;
    column.reserve(17);
    column.push_back(7);
    auto const columnUsage = column.memory_usage();
    REQUIRE(columnUsage.logicalBytes == sizeof(uint32_t));
    REQUIRE(columnUsage.allocatedBytes >= 17 * sizeof(uint32_t));

    ArrayArena<uint32_t, 8> arena;
    auto const values = arena.new_array(16);
    arena.push_back(values, 1);
    auto const arenaUsage = arena.memory_usage();
    REQUIRE(arenaUsage.logicalBytes >= sizeof(uint32_t));
    REQUIRE(arenaUsage.allocatedBytes >= 16 * sizeof(uint32_t));
    REQUIRE(arenaUsage.allocatedBytes >= arenaUsage.logicalBytes);
}

TEST_CASE("ArrayArena multiple arrays", "[ArrayArena]") {
    ArrayArena<int> arena;
    std::vector<std::vector<int>> expected = {
        {10, 11, 12, 13, 14, 15, 16, 17, 18, 19},
        {20, 21, 22, 23, 24, 25, 26, 27, 28, 29},
        {30, 31, 32, 33, 34, 35, 36, 37, 38, 39},
        {40, 41, 42, 43, 44, 45, 46, 47, 48, 49}
    };

    // Interleave pushing array elements for maximum fragmentation
    std::vector<ArrayIndex> arrayIndices(expected.size(), InvalidArrayIndex);
    for (auto j = 0; j < expected[0].size(); j+=2) {
        for (auto i = 0; i < expected.size(); ++i) {
            if (j == 0)
                arrayIndices[i] = arena.new_array(2);
            arena.push_back(arrayIndices[i], expected[i][j]);
            arena.push_back(arrayIndices[i], expected[i][j+1]);
        }
    }

    SECTION("accessing elements") {
        REQUIRE(arena.at(arrayIndices[0], 0) == 10);
        REQUIRE(arena.at(arrayIndices[0], 1) == 11);
        REQUIRE(arena.at(arrayIndices[0], 2) == 12);
        REQUIRE(arena.at(arrayIndices[1], 0) == 20);
        REQUIRE(arena.at(arrayIndices[1], 1) == 21);
        REQUIRE(arena.at(arrayIndices[1], 2) == 22);
        REQUIRE(arena.at(arrayIndices[1], 3) == 23);
    }

    SECTION("range-based for loop for multiple arrays") {
        std::vector<std::vector<int>> result = {{}, {}};
        for (auto value : arena.range(arrayIndices[0])) {
            result[0].push_back(value);
        }
        for (auto value : arena.range(arrayIndices[1])) {
            result[1].push_back(value);
        }
        REQUIRE(result[0] == expected[0]);
        REQUIRE(result[1] == expected[1]);
    }

    SECTION("iterating over all arrays") {
        auto i = 0;
        for (auto const& arr: arena) {
            auto j = 0;
            for (auto const& el : arr) {
                REQUIRE(el == expected[i][j]);
                ++j;
            }
            ++i;
        }
    }
}

TEST_CASE("ArrayArena::iterate") {
    ArrayArena<int> arena;
    ArrayIndex a = arena.new_array(2);
    for (size_t i = 0; i < 10; ++i) {
        arena.push_back(a, static_cast<int>(i*2));
    }

    SECTION("lambda with no return type") {
        int sum = 0;
        arena.iterate(a, [&sum](int value) {
            sum += value;
        });
        REQUIRE(sum == 90);  // sum of 0 to 18
    }

    SECTION("lambda with bool return type") {
        int lastValue = -1;
        arena.iterate(a, [&lastValue](int value) -> bool {
            lastValue = value;
            if (value == 10)
                return false;
            return true;
        });
        REQUIRE(lastValue == 10);
    }

    SECTION("lambda with index argument") {
        arena.iterate(a, [](int value, size_t index) {
            REQUIRE(value/2 == index);
        });
    }
}

#ifdef ARRAY_ARENA_THREAD_SAFE
TEST_CASE("ArrayArena Concurrency", "[ArrayArena]") {
    ArrayArena<int> arena;

    // Constants for test configuration
    const size_t num_threads = 100;
    const size_t num_iterations = 10000;

    // Function to be executed by each thread
    auto thread_func = [&]() {
        // Random delay to increase the chances of concurrency issues
        std::this_thread::sleep_for(std::chrono::nanoseconds(rand() % 100));  // NOLINT (rand() is safe here)
        auto array_index = arena.new_array(2);  // Minimal regular-array capacity for maximal fragmentation
        for (size_t i = 0; i < num_iterations; ++i) {
            arena.push_back(array_index, static_cast<int>(i));
            std::this_thread::sleep_for(std::chrono::nanoseconds(rand() % 100));  // NOLINT
        }
    };

    // Spawn threads
    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    for (size_t i = 0; i < num_threads; ++i)
        threads.emplace_back(thread_func);

    // Join threads
    for (auto& thread : threads)
        thread.join();

    // Check the results
    for (auto const& arr : arena) {
        REQUIRE(arr.size() == num_iterations);
        for (size_t i = 0; i < num_iterations; ++i) {
            REQUIRE(arr[i] == static_cast<int>(i));
        }
    }
}
#endif

TEST_CASE("ArrayArena serialization and deserialization") {
    ArrayArena<int> arena;

    // Create arrays and fill them with data
    auto array1 = arena.new_array(10);
    for (int i = 0; i < 10; ++i) {
        arena.push_back(array1, i);
    }

    auto array2 = arena.new_array(5);
    for (int i = 10; i < 15; ++i) {
        arena.push_back(array2, i);
    }

    // Serialize the ArrayArena
    using Buffer = std::vector<uint8_t>;
    using OutputAdapter = bitsery::OutputBufferAdapter<Buffer>;
    using InputAdapter = bitsery::InputBufferAdapter<Buffer>;
    Buffer buffer;
    auto writtenSize = bitsery::quickSerialization<OutputAdapter>(buffer, arena);

    // Deserialize the ArrayArena into a new instance
    ArrayArena<int> deserializedArena;
    auto state = bitsery::quickDeserialization<InputAdapter>(
        {buffer.begin(), writtenSize}, deserializedArena
    );

    // Check the deserialization state and the content of the deserialized arena
    REQUIRE(state.first == bitsery::ReaderError::NoError);
    REQUIRE(state.second);
    REQUIRE(arena.size(array1) == deserializedArena.size(array1));
    REQUIRE(arena.size(array2) == deserializedArena.size(array2));

    for (size_t i = 0; i < arena.size(array1); ++i) {
        REQUIRE(arena.at(array1, i) == deserializedArena.at(array1, i));
    }

    for (size_t i = 0; i < arena.size(array2); ++i) {
        REQUIRE(arena.at(array2, i) == deserializedArena.at(array2, i));
    }
}

TEST_CASE("ArrayArena singleton-handle storage", "[ArrayArena]")
{
    ArrayArena<int> arena;

    auto regularSingleCapacity = arena.new_array(1);
    REQUIRE(!ArrayArena<int>::is_singleton_handle(regularSingleCapacity));
    arena.push_back(regularSingleCapacity, 7);
    REQUIRE(arena.size(regularSingleCapacity) == 1);
    REQUIRE(arena.at(regularSingleCapacity, 0) == 7);
    arena.push_back(regularSingleCapacity, 8);
    REQUIRE(arena.size(regularSingleCapacity) == 2);
    REQUIRE(arena.at(regularSingleCapacity, 1) == 8);

    auto emptySingleton = arena.new_array(1, true);
    REQUIRE(ArrayArena<int>::is_singleton_handle(emptySingleton));
    REQUIRE(arena.size(emptySingleton) == 0);

    arena.push_back(emptySingleton, 42);
    REQUIRE(arena.size(emptySingleton) == 1);
    REQUIRE(arena.at(emptySingleton, 0) == 42);
    REQUIRE(!arena.at(emptySingleton, 1));

    REQUIRE_THROWS(arena.push_back(emptySingleton, 43));

    auto regular = arena.new_array(4);
    REQUIRE(!ArrayArena<int>::is_singleton_handle(regular));
    arena.push_back(regular, 1);
    arena.push_back(regular, 2);
    REQUIRE(arena.size(regular) == 2);
    REQUIRE(arena.at(regular, 1) == 2);

    SECTION("iterating over all arrays includes singleton handles")
    {
        std::vector<int> valuesByArray;
        for (auto const& arr : arena) {
            int value = -1;
            if (arr.size() > 0) {
                value = arr[0].value();
            }
            valuesByArray.push_back(value);
        }

        REQUIRE(valuesByArray.size() == 3);
        REQUIRE(valuesByArray[0] == 7);
        REQUIRE(valuesByArray[1] == 1);
        REQUIRE(valuesByArray[2] == 42);
    }
}

TEST_CASE("ModelColumn supports split TwoPart storage", "[ModelColumn][TwoPart]")
{
    using Pair = TwoPart<uint16_t, uint32_t>;
    ModelColumn<Pair, 8> column;

    column.emplace_back(10u, 100u);
    column.emplace_back(11u, 200u);
    column.push_back(Pair{12u, 300u});

    REQUIRE(column.size() == 3);
    REQUIRE(column.byte_size() == 3 * (sizeof(uint16_t) + sizeof(uint32_t)));
    REQUIRE(column.at(0).first() == 10u);
    REQUIRE(column.at(0).second() == 100u);
    REQUIRE(column.at(2).first() == 12u);
    REQUIRE(column.at(2).second() == 300u);

    using Buffer = std::vector<uint8_t>;
    using OutputAdapter = bitsery::OutputBufferAdapter<Buffer>;
    using InputAdapter = bitsery::InputBufferAdapter<Buffer>;
    Buffer buffer;
    auto writtenSize = bitsery::quickSerialization<OutputAdapter>(buffer, column);

    ModelColumn<Pair, 8> restored;
    auto state = bitsery::quickDeserialization<InputAdapter>(
        {buffer.begin(), writtenSize},
        restored);

    REQUIRE(state.first == bitsery::ReaderError::NoError);
    REQUIRE(state.second);
    REQUIRE(restored.size() == column.size());
    REQUIRE(restored.at(0).first() == 10u);
    REQUIRE(restored.at(0).second() == 100u);
    REQUIRE(restored.at(1).first() == 11u);
    REQUIRE(restored.at(1).second() == 200u);
    REQUIRE(restored.at(2).first() == 12u);
    REQUIRE(restored.at(2).second() == 300u);
}

TEST_CASE("ArrayArena supports TwoPart element storage", "[ArrayArena][TwoPart]")
{
    using Pair = TwoPart<uint16_t, uint32_t>;
    ArrayArena<Pair> arena;

    auto regular = arena.new_array(2);
    arena.emplace_back(regular, 1u, 10u);
    arena.emplace_back(regular, 2u, 20u);
    arena.emplace_back(regular, 3u, 30u);

    REQUIRE(arena.size(regular) == 3);
    auto first = arena.at(regular, 0);
    REQUIRE(first);
    REQUIRE(first->first() == 1u);
    REQUIRE(first->second() == 10u);

    auto third = arena.at(regular, 2);
    REQUIRE(third);
    REQUIRE(third->first() == 3u);
    REQUIRE(third->second() == 30u);

    auto singleton = arena.new_array(1, true);
    arena.push_back(singleton, Pair{7u, 70u});
    auto singletonValue = arena.at(singleton, 0);
    REQUIRE(singletonValue);
    REQUIRE(singletonValue->first() == 7u);
    REQUIRE(singletonValue->second() == 70u);

    size_t visited = 0;
    arena.iterate(regular, [&visited](auto value, size_t index) {
        REQUIRE(value.first() == static_cast<uint16_t>(index + 1));
        REQUIRE(value.second() == static_cast<uint32_t>((index + 1) * 10));
        ++visited;
        return true;
    });
    REQUIRE(visited == 3);
}
