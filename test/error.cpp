#include <catch2/catch_test_macros.hpp>

#include "simfil/error.h"
#include "simfil/token.h"


using namespace simfil;

TEST_CASE("Error constructors", "[error]") {
    SECTION("Constructor with type only") {
        Error error(Error::Type::ParserError);
        REQUIRE(error.type == Error::Type::ParserError);
        REQUIRE(error.message.empty());
        REQUIRE(error.location.offset == 0);
        REQUIRE(error.location.size == 0);
    }

    SECTION("Constructor with type and message") {
        Error error(Error::Type::RuntimeError, "Test error message");
        REQUIRE(error.type == Error::Type::RuntimeError);
        REQUIRE(error.message == "Test error message");
        REQUIRE(error.location.offset == 0);
        REQUIRE(error.location.size == 0);
    }

    SECTION("Constructor with type, message and source location") {
        SourceLocation loc(10, 5);
        Error error(Error::Type::InvalidExpression, "Invalid expr", loc);
        REQUIRE(error.type == Error::Type::InvalidExpression);
        REQUIRE(error.message == "Invalid expr");
        REQUIRE(error.location.offset == 10);
        REQUIRE(error.location.size == 5);
    }

    SECTION("Constructor with type, message and token") {
        Token token(Token::Type::WORD, 20, 25);
        Error error(Error::Type::UnknownFunction, "MyFunction", token);
        REQUIRE(error.type == Error::Type::UnknownFunction);
        REQUIRE(error.message == "MyFunction");
        REQUIRE(error.location.offset == 20);
        REQUIRE(error.location.size == 5);
    }
}

TEST_CASE("Error equality operator", "[error]") {
    SECTION("Equal errors") {
        Error a(Error::Type::DivisionByZero, "Division by zero");
        Error b(Error::Type::DivisionByZero, "Division by zero");
        REQUIRE(a == b);
    }

    SECTION("Different types") {
        Error a(Error::Type::DivisionByZero, "Error");
        Error b(Error::Type::RuntimeError, "Error");
        REQUIRE(!(a == b));
    }
}
