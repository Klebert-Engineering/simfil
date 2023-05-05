#include <catch2/catch_test_macros.hpp>
#include <thread>

#include "simfil/model/arena.h"

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

    SECTION("array growth") {
        ArrayIndex array1 = arena.new_array(2);
        arena.push_back(array1, 42);
        arena.push_back(array1, 43);
        arena.push_back(array1, 44);
        REQUIRE(arena.size(array1) == 3);
        REQUIRE(arena.at(array1, 0) == 42);
        REQUIRE(arena.at(array1, 1) == 43);
        REQUIRE(arena.at(array1, 2) == 44);
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
        REQUIRE_THROWS_AS(arena.at(array1, 0), std::out_of_range);
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
