#include <catch2/catch_test_macros.hpp>
#include <thread>
#include <bitsery/bitsery.h>
#include <bitsery/adapter/buffer.h>
#include <bitsery/traits/vector.h>

#include "simfil/model/arena.h"
#include "simfil/model/bitsery-traits.h"

using namespace simfil;

TEST_CASE("ArrayArena basic functionality", "[ArrayArena]") {
    ArrayArena<int> arena;

    SECTION("new_array") {
        ArrayIndex array1 = arena.new_array(2);
        ArrayIndex array2 = arena.new_array(4);

        REQUIRE(array1 == 0);
        REQUIRE(array2 == 1);
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
        REQUIRE(array2 == 0);
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

TEST_CASE("ArrayArena multiple arrays", "[ArrayArena]") {
    ArrayArena<int> arena;
    std::vector<std::vector<int>> expected = {
        {10, 11, 12, 13, 14, 15, 16, 17, 18, 19},
        {20, 21, 22, 23, 24, 25, 26, 27, 28, 29},
        {30, 31, 32, 33, 34, 35, 36, 37, 38, 39},
        {40, 41, 42, 43, 44, 45, 46, 47, 48, 49}
    };

    // Interleave pushing array elements for maximum fragmentation
    for (auto j = 0; j < expected[0].size(); j+=2) {
        for (auto i = 0; i < expected.size(); ++i) {
            if (j == 0)
                arena.new_array(1);
            arena.push_back(i, expected[i][j]);
            arena.push_back(i, expected[i][j+1]);
        }
    }

    SECTION("accessing elements") {
        REQUIRE(arena.at(0, 0) == 10);
        REQUIRE(arena.at(0, 1) == 11);
        REQUIRE(arena.at(0, 2) == 12);
        REQUIRE(arena.at(1, 0) == 20);
        REQUIRE(arena.at(1, 1) == 21);
        REQUIRE(arena.at(1, 2) == 22);
        REQUIRE(arena.at(1, 3) == 23);
    }

    SECTION("range-based for loop for multiple arrays") {
        std::vector<std::vector<int>> result = {{}, {}};
        for (auto value : arena.range(0)) {
            result[0].push_back(value);
        }
        for (auto value : arena.range(1)) {
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
    ArrayIndex a = arena.new_array(1);
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
        auto array_index = arena.new_array(1);  // Minimal initial capacity for maximal fragmentation
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
