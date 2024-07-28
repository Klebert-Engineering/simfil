#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <functional>

#include <bitsery/bitsery.h>
#include <bitsery/adapter/buffer.h>
#include <bitsery/traits/vector.h>

#include "simfil/model/fixed-arena.h"
#include "simfil/model/bitsery-traits.h"

using namespace simfil;

TEST_CASE("FixedArrayArena basic functionality", "[FixedArrayArena]") {
    FixedArrayArena<int, 24, 8> arena;

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

TEST_CASE("FixedArrayArena at", "[FixedArrayArena]") {
    FixedArrayArena<int, 24, 8> arena;
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


TEST_CASE("FixedArrayArena iterator", "[FixedArrayArena]") {
    FixedArrayArena<int, 24, 8> arena;
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

TEST_CASE("FixedArrayArena many arrays", "[FixedArrayArena]") {
    FixedArrayArena<int, 24, 8> arena;

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

TEST_CASE("FixedArrayArena index OOR", "[FixedArrayArena]") {
    FixedArrayArena<int, 1, 31> arena;
    arena.newArray(1); // index = 0
    arena.newArray(1); // index = 1

    CHECK_THROWS(arena.newArray(1)); // index > MaxIndex
}


TEST_CASE("FixedArrayArena size OOR", "[FixedArrayArena]") {
    FixedArrayArena<int, 31, 1> arena;
    arena.newArray(0); // size 0
    arena.newArray(1); // size 1

    CHECK_THROWS(arena.newArray(2)); // index > MaxSize
}

TEST_CASE("FixedArrayArena handle size", "[FixedArrayArena]") {
    REQUIRE(sizeof(FixedArrayArena<int,  4,  4>::Handle) == 1);
    REQUIRE(sizeof(FixedArrayArena<int,  8,  8>::Handle) == 2);
    REQUIRE(sizeof(FixedArrayArena<int, 31,  1>::Handle) == 4);
    REQUIRE(sizeof(FixedArrayArena<int, 31, 33>::Handle) == 8);
}

TEST_CASE("FixedArrayArena bitsery", "[FixedArrayArena]") {
    FixedArrayArena<int, 16, 16> arena;
    auto h0 = arena.newArray(12, 123);
    auto h1 = arena.newArray(2, 7);
    auto h2 = arena.newArray(0);

    using Buffer = std::vector<uint8_t>;
    using OutputAdapter = bitsery::OutputBufferAdapter<Buffer>;
    using InputAdapter = bitsery::InputBufferAdapter<Buffer>;
    Buffer buffer;
    auto writtenSize = bitsery::quickSerialization<OutputAdapter>(buffer, arena);

    // Deserialize the ArrayArena into a new instance
    decltype(arena) deserializedArena;
    auto state = bitsery::quickDeserialization<InputAdapter>(
        {buffer.begin(), writtenSize}, deserializedArena
    );

    // Check the deserialization state and the content of the deserialized arena
    REQUIRE(state.first == bitsery::ReaderError::NoError);
    REQUIRE(state.second);
    //REQUIRE(arena.size(array1) == deserializedArena.size(array1));
    //REQUIRE(arena.size(array2) == deserializedArena.size(array2));
}
