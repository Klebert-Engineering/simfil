#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <array>
#include <concepts>
#include <iterator>
#include <numeric>
#include <ranges>
#include <thread>
#include <type_traits>
#include <utility>
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
                value = arr[0];
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

TEST_CASE("ArrayArena ranges satisfy forward range contracts", "[ArrayArena][ranges]")
{
    using Arena = ArrayArena<int32_t, 4, 2>;
    using Range = Arena::BasicArrayRange<false>;
    using ConstRange = Arena::BasicArrayRange<true>;

    STATIC_REQUIRE(std::forward_iterator<Arena::iterator>);
    STATIC_REQUIRE(std::forward_iterator<Arena::const_iterator>);
    STATIC_REQUIRE(std::semiregular<Arena::iterator>);
    STATIC_REQUIRE(std::semiregular<Arena::const_iterator>);
    STATIC_REQUIRE(std::semiregular<Range>);
    STATIC_REQUIRE(std::semiregular<ConstRange>);
    STATIC_REQUIRE(std::ranges::view<Range>);
    STATIC_REQUIRE(std::ranges::view<ConstRange>);
    STATIC_REQUIRE(std::ranges::viewable_range<Range>);
    STATIC_REQUIRE(std::ranges::viewable_range<ConstRange>);
    STATIC_REQUIRE(std::ranges::forward_range<Range>);
    STATIC_REQUIRE(std::ranges::forward_range<ConstRange>);
    STATIC_REQUIRE(std::ranges::forward_range<Range const>);
    STATIC_REQUIRE(std::ranges::sized_range<Range>);
    STATIC_REQUIRE(std::ranges::sized_range<ConstRange>);
    STATIC_REQUIRE(std::ranges::common_range<Range>);
    STATIC_REQUIRE_FALSE(std::ranges::random_access_range<Range>);
    STATIC_REQUIRE_FALSE(std::ranges::contiguous_range<Range>);
    STATIC_REQUIRE(std::same_as<std::ranges::range_reference_t<Range>, int32_t&>);
    STATIC_REQUIRE(std::same_as<std::ranges::range_reference_t<Range const>, int32_t&>);
    STATIC_REQUIRE(std::same_as<std::ranges::range_reference_t<ConstRange>, int32_t const&>);
    STATIC_REQUIRE(std::same_as<decltype(*std::declval<Arena::iterator const&>()), int32_t&>);
    STATIC_REQUIRE(std::convertible_to<Arena::iterator, Arena::const_iterator>);
    STATIC_REQUIRE_FALSE(std::convertible_to<Arena::const_iterator, Arena::iterator>);
    STATIC_REQUIRE(std::convertible_to<Range, ConstRange>);
    STATIC_REQUIRE_FALSE(std::convertible_to<ConstRange, Range>);

    Range empty;
    ConstRange constEmpty = empty;
    REQUIRE(empty.empty());
    REQUIRE(empty.size() == 0);
    REQUIRE(empty.begin() == empty.end());
    REQUIRE(std::ranges::distance(empty) == 0);
    REQUIRE(std::ranges::equal(empty, constEmpty));
    REQUIRE(std::ranges::find(empty, 1) == empty.end());
    REQUIRE_FALSE(empty.at(0));
    REQUIRE(empty.at(0).error().type == Error::IndexOutOfRange);
    REQUIRE_FALSE(constEmpty.at(std::numeric_limits<size_t>::max()));
    auto emptyView = Range{} | std::views::transform([](int32_t value) { return value + 1; });
    REQUIRE(std::ranges::empty(emptyView));

    Arena::iterator defaultIterator;
    Arena::iterator assigned;
    assigned = defaultIterator;
    Arena::const_iterator constAssigned;
    constAssigned = assigned;
    REQUIRE(assigned == defaultIterator);
    REQUIRE(assigned == constAssigned);
    REQUIRE(assigned == empty.end());
    empty = Range(assigned, assigned);
    REQUIRE(empty.empty());

    Arena arena;
    auto const emptyZeroCapacity = arena.new_array(0);
    auto const emptyReserved = arena.new_array(9);
    auto const emptySingleton = arena.new_array(1, true);
    for (auto const handle : {ArrayIndex{0}, emptyZeroCapacity, emptyReserved, emptySingleton}) {
        auto const range = arena.range(handle);
        auto const readOnly = std::as_const(arena).range(handle);
        REQUIRE(range.empty());
        REQUIRE(range.begin() == range.end());
        REQUIRE(arena.begin(handle) == arena.end(handle));
        REQUIRE(std::ranges::equal(range, readOnly));
        REQUIRE_FALSE(range.at(0));
    }
    for (auto const handle : {InvalidArrayIndex, std::numeric_limits<ArrayIndex>::max(),
             static_cast<ArrayIndex>(arena.size()),
             SingletonArrayHandleMask | static_cast<ArrayIndex>(arena.singleton_handle_count())}) {
        REQUIRE_FALSE(arena.valid(handle));
        REQUIRE_THROWS_AS(arena.range(handle), std::out_of_range);
        REQUIRE_THROWS_AS(std::as_const(arena).range(handle), std::out_of_range);
        REQUIRE_THROWS_AS(arena.begin(handle), std::out_of_range);
        REQUIRE_THROWS_AS(arena.end(handle), std::out_of_range);
        REQUIRE_FALSE(arena.at(handle, 0));
    }

    arena.push_back(emptySingleton, 42);
    auto const singleton = arena.range(emptySingleton);
    REQUIRE(singleton.size() == 1);
    REQUIRE_FALSE(singleton.empty());
    REQUIRE(&singleton.front() == &singleton.back());
    REQUIRE(&singleton[0] == &singleton.at(0)->get());
    singleton.front() = 43;
    auto cursor = singleton.begin();
    REQUIRE(*cursor++ == 43);
    REQUIRE(cursor == singleton.end());
    REQUIRE_FALSE(singleton.at(1));
    auto singleView = std::as_const(arena).range(emptySingleton) |
        std::views::transform([](int32_t value) { return value * 2; });
    REQUIRE(std::ranges::equal(singleView, std::array{86}));
    constEmpty = singleton;
    REQUIRE(constEmpty.front() == 43);

    std::array<ArrayIndex, 9> singletons;
    for (size_t i = 0; i < singletons.size(); ++i) {
        singletons[i] = arena.new_array(1, true);
        if (i % 3 != 0) {
            arena.push_back(singletons[i], static_cast<int32_t>(100 + i));
        }
    }
    for (size_t i = 0; i < singletons.size(); ++i) {
        auto const range = arena.range(singletons[i]);
        REQUIRE(range.empty() == (i % 3 == 0));
        REQUIRE(std::ranges::equal(range, std::as_const(arena).range(singletons[i])));
        if (!range.empty()) {
            REQUIRE(range.front() == 100 + i);
            REQUIRE(std::ranges::next(range.begin()) == range.end());
        }
    }
}

TEST_CASE("ArrayArena forward cursors cross fragmented chunks and pages", "[ArrayArena][ranges]")
{
    using Arena = ArrayArena<int32_t, 4, 2>;
    constexpr size_t count = 97;
    Arena arena;
    std::array handles{arena.new_array(0), arena.new_array(1), arena.new_array(2),
        arena.new_array(7), arena.new_array(19)};

    // Interleaving separates consecutive chunks physically. Tiny payload and
    // chunk pages force both cached cursors and continuation indices across pages.
    for (size_t i = 0; i < count; ++i) {
        for (size_t j = 0; j < handles.size(); ++j) {
            arena.push_back(handles[j], static_cast<int32_t>(j * 1000 + i));
        }
    }
    auto const allocated = arena.memory_usage().allocatedBytes;
    for (size_t j = 0; j < handles.size(); ++j) {
        auto const range = arena.range(handles[j]);
        Arena::BasicArrayRange<false> copiedRange;
        copiedRange = range;
        Arena::BasicArrayRange<true> readOnly = copiedRange;
        REQUIRE(std::ranges::size(range) == count);
        REQUIRE(std::ranges::distance(range) == count);
        REQUIRE(std::ranges::equal(range, readOnly));

        std::array<int32_t, count> expected;
        std::iota(expected.begin(), expected.end(), static_cast<int32_t>(j * 1000));
        std::array<int32_t, count> copied{};
        auto const result = std::ranges::copy(readOnly, copied.begin());
        REQUIRE(result.in == readOnly.end());
        REQUIRE(result.out == copied.end());
        REQUIRE(copied == expected);
        REQUIRE(*std::ranges::lower_bound(range, expected[57]) == expected[57]);
        REQUIRE(std::ranges::find(range, -1) == range.end());

        // Each pipeline owns the temporary view, while the arena owns the data.
        // Keep using the pipeline after that temporary has left its full expression.
        auto mutableView = arena.range(handles[j]) |
            std::views::transform([](int32_t& value) -> int32_t& { return value; });
        auto readOnlyView = std::as_const(arena).range(handles[j]) |
            std::views::drop(3) | std::views::take(70) |
            std::views::transform([](int32_t value) { return value * 2; });
        REQUIRE(std::ranges::equal(mutableView, expected));
        REQUIRE(std::ranges::equal(readOnlyView,
            expected | std::views::drop(3) | std::views::take(70) |
                std::views::transform([](int32_t value) { return value * 2; })));

        auto current = range.begin();
        for (size_t i = 0; i < count; ++i) {
            auto const stationary = current;
            Arena::iterator assigned;
            assigned = current;
            Arena::const_iterator converted = current;
            REQUIRE(&*stationary == &range[i]);
            REQUIRE(stationary.operator->() == &range.at(i)->get());
            REQUIRE(&*converted == &readOnly[i]);
            REQUIRE(*assigned++ == expected[i]);
            REQUIRE(*stationary == expected[i]);
            ++current;
            REQUIRE(assigned == current);
            ++converted;
            REQUIRE(converted == current);
        }
        REQUIRE(current == range.end());
        REQUIRE_FALSE(range.at(count));
        REQUIRE_FALSE(range.at(std::numeric_limits<size_t>::max()));

        Arena::BasicArrayRange<false> slice{
            std::ranges::next(range.begin(), 3), std::ranges::next(range.begin(), 73)};
        REQUIRE(slice.size() == 70);
        REQUIRE(slice.front() == expected[3]);
        REQUIRE(slice.back() == expected[72]);
        REQUIRE(slice.at(69)->get() == expected[72]);
        REQUIRE_FALSE(slice.at(70));
        REQUIRE(std::ranges::equal(slice, expected | std::views::drop(3) | std::views::take(70)));
        Arena::BasicArrayRange<false> emptySlice{slice.end(), slice.end()};
        REQUIRE(emptySlice.empty());
        REQUIRE(emptySlice.begin() == emptySlice.end());
        REQUIRE_FALSE(emptySlice.at(0));

        std::ranges::for_each(range, [](int32_t& value) { ++value; });
        REQUIRE(readOnly.front() == expected.front() + 1);
        REQUIRE(readOnly.back() == expected.back() + 1);
        REQUIRE(*mutableView.begin() == expected.front() + 1);
        REQUIRE(*readOnlyView.begin() == (expected[3] + 1) * 2);
    }
    REQUIRE(arena.memory_usage().allocatedBytes == allocated);
}

TEST_CASE("ArrayArena split proxies are const-correct forward references", "[ArrayArena][ranges][TwoPart]")
{
    using Pair = TwoPart<uint16_t, uint32_t>;
    using Arena = ArrayArena<Pair, 4, 2>;
    using Range = Arena::BasicArrayRange<false>;
    using ConstRange = Arena::BasicArrayRange<true>;
    STATIC_REQUIRE(std::ranges::view<Range>);
    STATIC_REQUIRE(std::ranges::view<ConstRange>);
    STATIC_REQUIRE(std::forward_iterator<Arena::iterator>);
    STATIC_REQUIRE(std::forward_iterator<Arena::const_iterator>);
    STATIC_REQUIRE(std::ranges::forward_range<Range>);
    STATIC_REQUIRE(std::ranges::forward_range<ConstRange>);
    STATIC_REQUIRE(std::ranges::sized_range<Range>);
    STATIC_REQUIRE(std::ranges::sized_range<ConstRange>);
    STATIC_REQUIRE_FALSE(std::ranges::random_access_range<Range>);
    STATIC_REQUIRE(std::same_as<std::ranges::range_value_t<Range>, Pair>);
    STATIC_REQUIRE(std::same_as<std::ranges::range_reference_t<Range>, Arena::DataWriteRef>);
    STATIC_REQUIRE(std::same_as<std::ranges::range_reference_t<ConstRange>, Arena::DataReadRef>);
    STATIC_REQUIRE(std::same_as<decltype(std::declval<ConstRange>()[0].first()), uint16_t const&>);
    STATIC_REQUIRE(std::same_as<decltype(std::declval<ConstRange>().back().second()), uint32_t const&>);
    STATIC_REQUIRE_FALSE(std::assignable_from<decltype(std::declval<ConstRange>()[0].first()), uint16_t>);

    constexpr size_t count = 41;
    Arena arena;
    auto const values = arena.new_array(1);
    auto const interleaved = arena.new_array(1);
    auto const singleton = arena.new_array(1, true);
    std::array<Pair, count> expected;
    for (size_t i = 0; i < count; ++i) {
        expected[i] = Pair{static_cast<uint16_t>(i), static_cast<uint32_t>(i * 10)};
        arena.push_back(values, expected[i]);
        arena.push_back(interleaved, Pair{999u, 999u});
    }
    arena.emplace_back(singleton, 7u, 70u);
    auto const range = arena.range(values);
    ConstRange readOnly;
    readOnly = range;
    std::array<Pair, count> copied{};
    REQUIRE(std::ranges::copy(readOnly, copied.begin()).out == copied.end());
    REQUIRE(copied == expected);
    auto secondValues = std::as_const(arena).range(values) |
        std::views::transform([](auto pair) { return pair.second(); });
    REQUIRE(std::ranges::equal(secondValues,
        expected | std::views::transform([](auto const& pair) { return pair.second(); })));
    // Forward iteration does not require the column proxies themselves to be
    // equality_comparable; compare their logical values through a projection.
    auto pairValue = [](auto const& pair) { return Pair{pair.first(), pair.second()}; };
    REQUIRE(std::ranges::equal(range, expected, {}, pairValue));
    auto const lower = std::ranges::lower_bound(readOnly, uint16_t{27}, {},
        [](auto const& pair) { return pair.first(); });
    REQUIRE(lower != readOnly.end());
    REQUIRE((*lower).first() == 27);

    auto current = range.begin();
    for (size_t i = 0; i < count; ++i) {
        Arena::iterator assigned;
        assigned = current;
        Arena::const_iterator converted;
        converted = current;
        auto const stationary = current;
        REQUIRE((*assigned++).to_value() == expected[i]);
        REQUIRE((*stationary).to_value() == expected[i]);
        REQUIRE(&(*converted).first() == &range[i].first());
        ++current;
        ++converted;
        REQUIRE(assigned == current);
        REQUIRE(converted == current);
    }
    REQUIRE(current == range.end());
    range.front().first() = 101;
    range.back() = Pair{102u, 1020u};
    range.at(3)->second() = 333;
    REQUIRE(readOnly.front().first() == 101);
    REQUIRE(readOnly.back().second() == 1020);
    REQUIRE(readOnly[3].second() == 333);
    REQUIRE_FALSE(readOnly.at(count));
    auto const found = std::ranges::find_if(readOnly, [](auto const& pair) { return pair.second() == 333; });
    REQUIRE(found != readOnly.end());
    REQUIRE((*found).first() == 3);

    auto const singleRange = arena.range(singleton);
    singleRange.front().second() = 71;
    REQUIRE(std::as_const(arena).range(singleton).back().second() == 71);
    REQUIRE(std::ranges::next(singleRange.begin()) == singleRange.end());
}

TEST_CASE("ArrayArena ranges preserve compact serialization", "[ArrayArena][ranges][serialization]")
{
    // Exercise the same cursor/codec contract with physical references and split proxies.
    auto check = []<typename Value>() {
        using Arena = ArrayArena<Value, 4, 2>;
        auto value = [](uint32_t i) {
            if constexpr (std::same_as<Value, uint32_t>) return i;
            else return Value{static_cast<uint16_t>(i), i * 10};
        };
        auto asValue = [](auto const& item) { return static_cast<Value>(item); };
        Arena arena;
        auto const empty = arena.new_array(0);
        auto const values = arena.new_array(1);
        auto const interleaved = arena.new_array(3);
        auto const singleton = arena.new_array(1, true);
        auto const emptySingleton = arena.new_array(1, true);
        for (uint32_t i = 0; i < 43; ++i) {
            arena.push_back(values, value(i));
            arena.push_back(interleaved, value(100 + i));
        }
        arena.push_back(singleton, value(99));

        using Buffer = std::vector<uint8_t>;
        using Output = bitsery::OutputBufferAdapter<Buffer>;
        using Input = bitsery::InputBufferAdapter<Buffer>;
        Buffer bytes;
        auto const written = bitsery::quickSerialization<Output>(bytes, arena);
        Arena restored;
        auto const state = bitsery::quickDeserialization<Input>({bytes.begin(), written}, restored);
        REQUIRE(state.first == bitsery::ReaderError::NoError);
        REQUIRE(state.second);
        REQUIRE(restored.is_compact());
        auto const allocated = restored.memory_usage().allocatedBytes;
        for (auto const handle : {empty, values, interleaved, singleton, emptySingleton}) {
            auto const range = restored.range(handle);
            auto const readOnly = std::as_const(restored).range(handle);
            REQUIRE(std::ranges::equal(range, arena.range(handle), {}, asValue, asValue));
            REQUIRE(std::ranges::equal(range, readOnly, {}, asValue, asValue));
            REQUIRE(std::ranges::distance(range) == arena.size(handle));
            REQUIRE_FALSE(range.at(range.size()));
            if (!range.empty()) {
                REQUIRE(range.front() == value(handle == singleton ? 99 : handle == values ? 0 : 100));
                REQUIRE(asValue(range.back()) == asValue(arena.range(handle).back()));
                auto copy = range.begin();
                typename Arena::const_iterator converted;
                converted = copy;
                REQUIRE(asValue(*copy++) == asValue(*converted));
                ++converted;
                REQUIRE(copy == converted);
            }
        }
        REQUIRE(restored.memory_usage().allocatedBytes == allocated);
        REQUIRE(restored.is_compact());
        REQUIRE_THROWS_AS(restored.range(InvalidArrayIndex), std::out_of_range);

        Buffer rewritten;
        REQUIRE(bitsery::quickSerialization<Output>(rewritten, restored) == written);
        REQUIRE(rewritten == bytes);
    };
    check.template operator()<uint32_t>();
    check.template operator()<TwoPart<uint16_t, uint32_t>>();
}

TEST_CASE("ArrayArena truncation preserves handles and retained chunk storage", "[ArrayArena][truncate]")
{
    using Arena = ArrayArena<uint32_t, 4, 2>;
    constexpr size_t count = 97;
    for (size_t const keep : {0, 1, 2, 3, 6, 7, 14, 15, 30, 31, 62, 63, 96, 97}) {
        CAPTURE(keep);
        Arena arena;
        auto const values = arena.new_array(2);
        auto const other = arena.new_array(1);
        for (size_t i = 0; i < count; ++i) {
            arena.push_back(values, static_cast<uint32_t>(i));
            arena.push_back(other, static_cast<uint32_t>(1000 + i));
        }
        auto const before = arena.memory_usage();
        auto const handles = arena.size();
        arena.truncate(values, keep);
        REQUIRE(arena.valid(values));
        REQUIRE(arena.size() == handles);
        REQUIRE(arena.size(values) == keep);
        REQUIRE(arena.memory_usage().allocatedBytes == before.allocatedBytes);
        REQUIRE(arena.memory_usage().logicalBytes == before.logicalBytes - (count - keep) * sizeof(uint32_t));
        REQUIRE(std::ranges::equal(arena.range(values), std::views::iota(uint32_t{0}, static_cast<uint32_t>(keep))));
        REQUIRE_FALSE(arena.at(values, keep));
        size_t visited = 0;
        arena.iterate(values, [&](uint32_t value) { REQUIRE(value == visited++); });
        REQUIRE(visited == keep);

        // Invalid truncations must not detach links or change the surviving tail.
        REQUIRE_THROWS_AS(arena.truncate(values, keep + 1), std::out_of_range);
        REQUIRE_THROWS_AS(arena.truncate(values, std::numeric_limits<size_t>::max()), std::out_of_range);
        REQUIRE(arena.size(values) == keep);
        REQUIRE(arena.memory_usage().allocatedBytes == before.allocatedBytes);
        for (size_t i = keep; i < count + 13; ++i) {
            arena.push_back(values, static_cast<uint32_t>(i));
        }
        REQUIRE(std::ranges::equal(arena.range(values), std::views::iota(uint32_t{0}, uint32_t{count + 13})));
        REQUIRE(std::ranges::equal(arena.range(other), std::views::iota(uint32_t{1000}, uint32_t{1000 + count})));
        arena.truncate(values, 0);
        arena.emplace_back(values, 123u);
        REQUIRE(arena.range(values).front() == 123);
        REQUIRE(arena.range(values).size() == 1);
    }
}

TEST_CASE("ArrayArena truncation handles empty singleton and invalid arrays", "[ArrayArena][truncate]")
{
    ArrayArena<uint32_t, 4, 2> arena;
    auto const empty = arena.new_array(0);
    auto const reserved = arena.new_array(9);
    auto const singleton = arena.new_array(1, true);
    auto const allocated = arena.memory_usage().allocatedBytes;
    for (auto const handle : {ArrayIndex{0}, empty, reserved, singleton}) {
        arena.truncate(handle, 0);
        REQUIRE(arena.range(handle).empty());
        REQUIRE_THROWS_AS(arena.truncate(handle, 1), std::out_of_range);
    }
    REQUIRE(arena.memory_usage().allocatedBytes == allocated);
    for (auto const handle : {InvalidArrayIndex, static_cast<ArrayIndex>(arena.size()),
             SingletonArrayHandleMask | static_cast<ArrayIndex>(arena.singleton_handle_count())}) {
        REQUIRE_THROWS_AS(arena.truncate(handle, 0), std::out_of_range);
    }
    arena.push_back(singleton, 17);
    arena.truncate(singleton, 1);
    REQUIRE(arena.range(singleton).front() == 17);
    REQUIRE_THROWS_AS(arena.truncate(singleton, 2), std::out_of_range);
    arena.truncate(singleton, 0);
    REQUIRE(arena.singleton_occupied_count() == 0);
    REQUIRE_FALSE(arena.at(singleton, 0));
    arena.emplace_back(singleton, 18);
    REQUIRE(arena.range(singleton).front() == 18);
    REQUIRE(arena.singleton_occupied_count() == 1);
    REQUIRE(arena.memory_usage().allocatedBytes == allocated);
    arena.push_back(empty, 19);
    arena.push_back(reserved, 20);
    REQUIRE(arena.range(empty).back() == 19);
    REQUIRE(arena.range(reserved).back() == 20);
}

TEST_CASE("ArrayArena compact truncation roundtrips and permits appending", "[ArrayArena][truncate][serialization]")
{
    auto check = []<typename Value>() {
        using Arena = ArrayArena<Value, 4, 2>;
        auto value = [](uint32_t i) {
            if constexpr (std::same_as<Value, uint32_t>) return i;
            else return Value{static_cast<uint16_t>(i), i * 10};
        };
        auto asValue = [](auto const& item) { return static_cast<Value>(item); };
        Arena source;
        auto const prefix = source.new_array(2);
        auto const values = source.new_array(1);
        auto const other = source.new_array(2);
        auto const tail = source.new_array(1);
        auto const empty = source.new_array(0);
        auto const singleton = source.new_array(1, true);
        auto const emptySingleton = source.new_array(1, true);
        for (uint32_t i = 0; i < 47; ++i) {
            source.push_back(values, value(i));
            source.push_back(other, value(100 + i));
        }
        for (uint32_t i = 0; i < 3; ++i) {
            source.push_back(prefix, value(800 + i));
        }
        for (uint32_t i = 0; i < 7; ++i) {
            source.push_back(tail, value(900 + i));
        }
        source.push_back(singleton, value(17));

        using Buffer = std::vector<uint8_t>;
        using Output = bitsery::OutputBufferAdapter<Buffer>;
        using Input = bitsery::InputBufferAdapter<Buffer>;
        Buffer bytes;
        auto const written = bitsery::quickSerialization<Output>(bytes, source);
        Arena arena;
        auto state = bitsery::quickDeserialization<Input>({bytes.begin(), written}, arena);
        REQUIRE(state.first == bitsery::ReaderError::NoError);
        REQUIRE(state.second);
        REQUIRE(arena.is_compact());
        auto const before = arena.memory_usage();

        // Trim a middle array while preserving both neighbors, and remove a
        // populated tail to exercise both internal holes and trailing payload.
        arena.truncate(values, 11);
        arena.truncate(tail, 0);
        arena.truncate(empty, 0);
        arena.truncate(singleton, 0);
        arena.truncate(emptySingleton, 0);
        REQUIRE(arena.is_compact());
        REQUIRE(arena.size(values) == 11);
        REQUIRE(arena.range(tail).empty());
        REQUIRE(arena.range(prefix).front() == value(800));
        REQUIRE(arena.range(prefix).back() == value(802));
        REQUIRE(arena.range(other).front() == value(100));
        REQUIRE(arena.range(other).back() == value(146));
        REQUIRE(arena.range(singleton).empty());
        REQUIRE(arena.memory_usage().allocatedBytes == before.allocatedBytes);
        REQUIRE(arena.memory_usage().logicalBytes == before.logicalBytes - 43 * Arena::DataStorage::record_size);
        REQUIRE_THROWS_AS(arena.truncate(values, 12), std::out_of_range);
        REQUIRE_THROWS_AS(arena.truncate(InvalidArrayIndex, 0), std::out_of_range);
        REQUIRE_THROWS_AS(arena.truncate(static_cast<ArrayIndex>(arena.size()), 0), std::out_of_range);

        // Singleton writes must not discard the still-authoritative compact heads.
        arena.push_back(singleton, value(18));
        arena.emplace_back(emptySingleton, value(19));
        REQUIRE(arena.is_compact());
        REQUIRE(arena.range(values).back() == value(10));
        REQUIRE(arena.range(singleton).front() == value(18));
        REQUIRE(arena.range(emptySingleton).front() == value(19));
        REQUIRE(arena.memory_usage().allocatedBytes == before.allocatedBytes);

        // The wire codec must repack holes left by compact truncation into its
        // canonical contiguous layout before the validating reader accepts it.
        Buffer truncated;
        auto const truncatedSize = bitsery::quickSerialization<Output>(truncated, arena);
        Arena restored;
        state = bitsery::quickDeserialization<Input>({truncated.begin(), truncatedSize}, restored);
        REQUIRE(state.first == bitsery::ReaderError::NoError);
        REQUIRE(state.second);
        for (auto const handle : {prefix, values, other, tail, empty, singleton, emptySingleton}) {
            REQUIRE(std::ranges::equal(arena.range(handle), restored.range(handle), {}, asValue, asValue));
        }

        for (uint32_t i = 11; i < 61; ++i) {
            arena.push_back(values, value(i));
        }
        arena.push_back(other, value(200));
        arena.push_back(empty, value(201));
        arena.push_back(tail, value(202));
        REQUIRE_FALSE(arena.is_compact());
        auto const range = arena.range(values);
        REQUIRE(range.size() == 61);
        for (uint32_t i = 0; i < 61; ++i) {
            REQUIRE(range[i] == value(i));
        }
        REQUIRE(arena.range(prefix).back() == value(802));
        REQUIRE(arena.range(other).front() == value(100));
        REQUIRE(arena.range(other).back() == value(200));
        REQUIRE(arena.range(empty).front() == value(201));
        REQUIRE(arena.range(tail).front() == value(202));
    };
    check.template operator()<uint32_t>();
    check.template operator()<TwoPart<uint16_t, uint32_t>>();
}
