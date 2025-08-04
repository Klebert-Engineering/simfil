#include <algorithm>
#include <string_view>
#include <optional>

#include "common.hpp"

const auto model = R"json(
{
  "f": 1,
  "fi": 2,
  "fie": 3,
  "fiel": 4,
  "field": "CONSTANT_1",
  "other": "CONSTANT_2",
  "constant": 5,
  "__long__name__": "long text value",
  "this needs escaping": 6,
  "sub": {
    "child": 7
  }
}
)json";

using Type = simfil::CompletionCandidate::Type;

auto GetCompletion(std::string_view query, std::optional<size_t> point)
{
    return CompleteQuery(query, point.value_or(query.size()), model);
}

auto FindCompletion(std::string_view query, std::optional<size_t> point, std::string_view what, std::optional<Type> type, size_t count)
{
    auto comp = GetCompletion(query, point);
    if (count != 0)
        REQUIRE(comp.size() == count);
    else
        REQUIRE(comp.size() > 0);

    for (const auto& item : comp) {
        INFO(item.text);
        if (item.text == what && (!type || item.type == *type))
            return true;
    }
    return false;
}

auto EXPECT_COMPLETION(std::string_view query, std::optional<size_t> point, std::string_view what, std::optional<Type> type = {}, size_t count = 0)
{
    REQUIRE(FindCompletion(query, point, what, type, count) == true);
}

TEST_CASE("CompleteField", "[completion.field.incompleteQuery]") {
    EXPECT_COMPLETION("((oth", {}, "other");
}

TEST_CASE("CompleteField", "[completion.field]") {
    EXPECT_COMPLETION("oth", {}, "other");
    EXPECT_COMPLETION("__", {}, "__long__name__");
}

TEST_CASE("CompleteField", "[completion.field.escape]") {
    EXPECT_COMPLETION("this", {}, "[\"this needs escaping\"]");
}

TEST_CASE("CompleteField", "[completion.field.midQuery]") {
    EXPECT_COMPLETION("oth > 123", 3, "other");
    //                    ^- cursor
    EXPECT_COMPLETION("oth>123", 3, "other");
    //                    ^- cursor
}

TEST_CASE("CompleteField", "[completion.sub-field]") {
    EXPECT_COMPLETION("sub.", {}, "child");
}

TEST_CASE("CompleteString", "[completion.string-const]") {
    EXPECT_COMPLETION("1 > C", {}, "CONSTANT_1");
}

TEST_CASE("CompleteFieldOrString") {
    // Complete both the field and the constants
    EXPECT_COMPLETION("cons", {}, "constant", Type::FIELD);
    EXPECT_COMPLETION("cons", {}, "CONSTANT_1", Type::WORD);

    // Do not complete the field
    EXPECT_COMPLETION("CONS", {}, "CONSTANT_1", Type::WORD, 2);
    EXPECT_COMPLETION("CONS", {}, "CONSTANT_2", Type::WORD, 2);
}

TEST_CASE("CompleteSorted") {
    auto comp = GetCompletion("f", {});
    REQUIRE(std::is_sorted(comp.begin(), comp.end(), [](const auto& l, const auto& r) {
        return l.text < r.text;
    }));
}
