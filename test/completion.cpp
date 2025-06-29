#include <string_view>
#include <optional>

#include "common.hpp"

auto FindCompletion(std::string_view query, std::optional<size_t> point, std::string_view what) {
    auto comp = CompleteQuery(query, point.value_or(query.size()));
    REQUIRE(comp.size() > 0);

    for (const auto& item : comp) {
        INFO(item.text);
        if (item.text == what)
            return true;
    }
    return false;
}

#define EXPECT_COMPLETION(query, point, what) \
    REQUIRE(FindCompletion(query, point, what) == true);

TEST_CASE("CompleteField", "[completion.field.incompleteQuery]") {
    EXPECT_COMPLETION("((num", {}, "number");
}

TEST_CASE("CompleteField", "[completion.field]") {
    EXPECT_COMPLETION("num", {}, "number");
}

TEST_CASE("CompleteField", "[completion.field.midQuery]") {
    EXPECT_COMPLETION("num > 123", 3, "number");
    //                    ^- cursor
}

TEST_CASE("CompleteField", "[completion.sub-field]") {
    EXPECT_COMPLETION("sub.", {}, "sub");
}
