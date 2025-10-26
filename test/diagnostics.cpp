#include <string_view>
#include <sstream>

#include "catch2/catch_test_macros.hpp"
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
    EXPECT_DIAGNOSTIC_MESSAGE_CONTAINING("['string'] > 123", "All values compared to true. Left hand types are string");
}

TEST_CASE("AnyUnknownField", "[diag.any-suppress-if-any]") {
    EXPECT_N_DIAGNOSTIC_MESSAGES("any(**.not_a_field > 0 or **.number = 123)", 0);
}

TEST_CASE("OrShortCircuit", "[diag.suppress-short-circuitted-or]") {
    EXPECT_N_DIAGNOSTIC_MESSAGES("**.number = 123 or **.not_a_field > 0", 0);
}

TEST_CASE("DiagnosticsSerialization", "[diag.serialization]") {
    auto model = simfil::json::parse(TestModel);
    REQUIRE(model);
    Environment env(model.value()->strings());

    // Create two diagnostic messages.
    Diagnostics originalDiag;
    auto ast = compile(env, "**.number == \"string\" or **.number == \"string\"");
    REQUIRE(ast.has_value());
    auto root = model.value()->root(0);
    REQUIRE(root);
    eval(env, **ast, **root, &originalDiag);

    std::stringstream stream;
    auto writeResult = originalDiag.write(stream);
    REQUIRE(writeResult.has_value());
    
    stream.seekg(0);
    Diagnostics deserializedDiag;
    auto readResult = deserializedDiag.read(stream);
    REQUIRE(readResult.has_value());
    
    // Both diagnostics objects must generate the exact same messages.
    auto originalMessages = diagnostics(env, **ast, originalDiag);
    auto deserializedMessages = diagnostics(env, **ast, deserializedDiag);
    
    REQUIRE(originalMessages.has_value());
    REQUIRE(deserializedMessages.has_value());
    REQUIRE(originalMessages->size() == deserializedMessages->size());
    
    if (!originalMessages->empty()) {
        REQUIRE(originalMessages->front().message == deserializedMessages->front().message);
    }
}
