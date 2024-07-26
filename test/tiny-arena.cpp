#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <functional>
#include "simfil/model/tiny-arena.h"

using namespace simfil;

TEST_CASE("TinyArrayArena basic functionality", "[TinyArrayArena]") {
    TinyArrayArena<int, 24, 8> arena;

    SECTION("maxima") {
        REQUIRE(arena.MaxIndex == 0xffffff);
        REQUIRE(arena.MaxSize == 0xff);
    }

    SECTION("newArray") {
        auto handle = arena.newArray(5);
        REQUIRE(handle.index == 0);
        REQUIRE(handle.size == 5);
    }
}

TEST_CASE("TinyArrayArena at", "[TinyArrayArena]") {
    TinyArrayArena<int, 24, 8> arena;
    auto handle = arena.newArray({1, 2, 3});

    SECTION("read") {
        for (auto i = 0; i < handle.size; ++i)
            REQUIRE(arena.at(handle, i) == i + 1);
    }

    SECTION("mutate") {
        for (auto i = 0; i < handle.size; ++i)
            arena.at(handle, i) += 1;
        for (auto i = 0; i < handle.size; ++i)
            REQUIRE(arena.at(handle, i) == i + 2);
    }
}


TEST_CASE("TinyArrayArena iterator", "[TinyArrayArena]") {
    TinyArrayArena<int, 24, 8> arena;
    auto handle = arena.newArray({1, 2, 3});

    SECTION("read non-const") {
        auto i = 1;
        for (auto v : arena.array(handle))
            REQUIRE(v == i++);
    }

    SECTION("read const") {
        auto i = 1;
        auto const& constArena = arena;
        for (auto v : constArena.array(handle))
            REQUIRE(v == i++);
    }

    SECTION("mutate") {
        for (auto& v : arena.array(handle))
            v += 1;
        auto i = 2;
        for (auto v : arena.array(handle))
            REQUIRE(v == i++);
    }
}

TEST_CASE("TinyArrayArena many arrays", "[TinyArrayArena]") {
    TinyArrayArena<int, 24, 8> arena;

    std::vector<decltype(arena)::Handle> handles;

    // Add some arrays
    const auto count = 1000;
    const auto size = 5;
    for (auto i = 1; i <= count + 1; ++i)
        handles.emplace_back(arena.newArray(size, i));

    // Read them back & compare
    auto i = 1;
    for (auto h : handles) {
        REQUIRE(std::all_of(
                    arena.array(h).begin(),
                    arena.array(h).end(), [&i](auto v) { return v == i; }));
        ++i;
    }

    // Test last handles index
    REQUIRE(handles.back().index == (count) * size);
}

TEST_CASE("TinyArrayArena index OOR", "[TinyArrayArena]") {
    TinyArrayArena<int, 1, 31> arena;
    arena.newArray(1); // index = 0
    arena.newArray(1); // index = 1

    CHECK_THROWS(arena.newArray(1)); // index > MaxIndex
}


TEST_CASE("TinyArrayArena size OOR", "[TinyArrayArena]") {
    TinyArrayArena<int, 31, 1> arena;
    arena.newArray(0); // size 0
    arena.newArray(1); // size 1

    CHECK_THROWS(arena.newArray(2)); // index > MaxSize
}

TEST_CASE("TinyArrayArena handle size", "[TinyArrayArena]") {
    REQUIRE(sizeof(TinyArrayArena<int,  4,  4>::Handle) == 1);
    REQUIRE(sizeof(TinyArrayArena<int,  8,  8>::Handle) == 2);
    REQUIRE(sizeof(TinyArrayArena<int, 31,  1>::Handle) == 4);
    REQUIRE(sizeof(TinyArrayArena<int, 31, 33>::Handle) == 8);
}
