#include "simfil/model/string-pool.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <memory>
#include <sstream>

using namespace simfil;

TEST_CASE("StringPool exhaustion does not mutate or reuse reserved ids", "[model.string-pool]")
{
    auto owner = std::make_unique<StringPool>();
    auto& pool = *owner;
    for (std::uint32_t id = StringPool::FirstDynamicId; id <= std::numeric_limits<StringId>::max(); ++id) {
        auto added = pool.emplace("field-" + std::to_string(id));
        REQUIRE(added);
        REQUIRE(*added == id);
    }
    const auto size = pool.size();
    const auto bytes = pool.bytes();
    const auto stored = pool.strings().size();
    for (int attempt = 0; attempt < 3; ++attempt) {
        auto failure = pool.emplace("overflow-" + std::to_string(attempt));
        REQUIRE_FALSE(failure);
        CHECK(failure.error().type == Error::StringPoolOverflow);
        CHECK(pool.size() == size);
        CHECK(pool.bytes() == bytes);
        CHECK(pool.strings().size() == stored);
        CHECK(pool.highest() == std::numeric_limits<StringId>::max());
        CHECK(pool.resolve(StringPool::Empty) == "");
    }
    CHECK(pool.emplace("field-65535").value() == 65535);

    // An inclusive uint16 loop used to wrap at this boundary and never finish.
    std::ostringstream output;
    REQUIRE(pool.write(output));
    auto data = output.str();
    auto restoredOwner = std::make_unique<StringPool>();
    auto& restored = *restoredOwner;
    REQUIRE(restored.read(std::vector<std::uint8_t>(data.begin(), data.end())));
    CHECK(restored == pool);
    CHECK(restored.highest() == 65535);
    CHECK_FALSE(restored.emplace("still-full"));

    std::ostringstream tail;
    REQUIRE(pool.write(tail, 65535));
    CHECK_FALSE(tail.str().empty());
}
