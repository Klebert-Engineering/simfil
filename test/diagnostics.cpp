#include <string_view>

#include "common.hpp"

auto FindMessage(std::string_view query, std::string_view needle) {
    const auto messages = GetDiagnosticMessages(query);
    REQUIRE(messages.size() > 0);

    for (const auto& msg : messages) {
        auto pos = msg.message.find(needle);
        if (pos != std::string::npos)
            return true;
    }

    return false;
}

#define EXPECT_DIAGNOSTIC_MESSAGE_CONTAINING(query, needle) \
    REQUIRE(FindMessage((query), (needle)) == true)

#define EXPECT_N_DIAGNOSTIC_MESSAGES(query, n) \
    REQUIRE(GetDiagnosticMessages((query)).size() == (n))

TEST_CASE("UnknownField", "[diag.unknown-field]") {
    EXPECT_DIAGNOSTIC_MESSAGE_CONTAINING("**.not_a_field > 0", "No matches for field");
}

TEST_CASE("ComparatorTypeMissmatch", "[diag.comparator-type-missmatch]") {
    EXPECT_DIAGNOSTIC_MESSAGE_CONTAINING("*['string'] > 123", "All values compared to");
}

TEST_CASE("AnyUnknownField", "[diag.any-suppress-if-any]") {
    EXPECT_N_DIAGNOSTIC_MESSAGES("any(**.not_a_field > 0 or **.number = 123)", 0);
}

TEST_CASE("OrShortCircuit", "[diag.suppress-short-circuitted-or]") {
    EXPECT_N_DIAGNOSTIC_MESSAGES("**.number = 123 or **.not_a_field > 0", 0);
}
