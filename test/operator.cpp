#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <cstdint>
#include <type_traits>

#include "simfil/operator.h"
#include "simfil/model/model.h"
#include "simfil/value.h"

using namespace simfil;
using namespace std::string_literals;

#define REQUIRE_INVALID_OPERANDS(v) \
    static_assert(std::is_same_v<decltype((v)), InvalidOperands>);

TEST_CASE("Unary operators", "[operator.unary]") {
    SECTION("OperatorNegate") {
        OperatorNegate op;
        REQUIRE(op.name() == std::string("-"));
        
        REQUIRE(op(int64_t(5)) == int64_t(-5));
        REQUIRE(op(int64_t(-5)) == int64_t(5));
        REQUIRE(op(0.0) == Catch::Approx(-0.0));
        REQUIRE(op(3.14) == Catch::Approx(-3.14));
        REQUIRE(op(-3.14) == Catch::Approx(3.14));

        REQUIRE_INVALID_OPERANDS(op("string"s));
        REQUIRE_INVALID_OPERANDS(op(true));
    }

    SECTION("OperatorBool") {
        OperatorBool op;
        REQUIRE(op.name() == std::string("?"));
        
        REQUIRE(op(NullType{}) == false);
        REQUIRE(op(false) == false);
        REQUIRE(op(true) == true);
        REQUIRE(op(int64_t(0)) == true);
        REQUIRE(op(int64_t(1)) == true);
        REQUIRE(op(0.0) == true);
        REQUIRE(op(1.0) == true);
        REQUIRE(op("") == true);
        REQUIRE(op("hello") == true);
    }

    SECTION("OperatorNot") {
        OperatorNot op;
        REQUIRE(op.name() == std::string("not"));
        
        REQUIRE(op(NullType{}) == true);
        REQUIRE(op(false) == true);
        REQUIRE(op(true) == false);
        REQUIRE(op(int64_t(0)) == false);
        REQUIRE(op(int64_t(1)) == false);
        REQUIRE(op(0.0) == false);
        REQUIRE(op(1.0) == false);
        REQUIRE(op("") == false);
        REQUIRE(op("hello") == false);
    }
    
    SECTION("OperatorBitInv") {
        OperatorBitInv op;
        REQUIRE(op.name() == std::string("~"));
        
        REQUIRE(op(uint64_t(0)) == ~uint64_t(0));
        REQUIRE(op(uint64_t(0xFF)) == ~uint64_t(0xFF));
        REQUIRE(op(int64_t(0)) == int64_t(~uint64_t(0)));
        REQUIRE(op(int64_t(-1)) == int64_t(0));

        REQUIRE_INVALID_OPERANDS(op("string"s));
        REQUIRE_INVALID_OPERANDS(op(3.14));
        REQUIRE_INVALID_OPERANDS(op(true));
    }

    SECTION("OperatorLen") {
        OperatorLen op;
        REQUIRE(op.name() == std::string("#"));
        
        REQUIRE(op(""s) == int64_t(0));
        REQUIRE(op("hello"s) == int64_t(5));
        REQUIRE(op("hello world"s) == int64_t(11));

        REQUIRE_INVALID_OPERANDS(op(int64_t(123)));
        REQUIRE_INVALID_OPERANDS(op(3.14));
        REQUIRE_INVALID_OPERANDS(op(true));
    }

    SECTION("OperatorTypeof") {
        OperatorTypeof op;
        REQUIRE(op.name() == std::string("typeof"));
        
        REQUIRE(op(NullType{}) == "null");
        REQUIRE(op(true) == "bool");
        REQUIRE(op(false) == "bool");
        REQUIRE(op(int64_t(42)) == "int");
        REQUIRE(op(3.14) == "float");
        REQUIRE(op("hello"s) == "string");
    }
}

TEST_CASE("Type conversion operators", "[operator.conversion]") {
    SECTION("OperatorAsInt") {
        OperatorAsInt op;
        REQUIRE(op.name() == std::string("int"));
        
        REQUIRE(op(true) == int64_t(1));
        REQUIRE(op(false) == int64_t(0));
        REQUIRE(op(int64_t(42)) == int64_t(42));
        REQUIRE(op(3.14) == int64_t(3));
        REQUIRE(op(-3.99) == int64_t(-3));
        REQUIRE(op("123"s) == int64_t(123));
        REQUIRE(op("-456"s) == int64_t(-456));
        REQUIRE(op("not a number"s) == int64_t(0));
        REQUIRE(op(""s) == int64_t(0));
        REQUIRE(op(NullType{}) == int64_t(0));
    }

    SECTION("OperatorAsFloat") {
        OperatorAsFloat op;
        REQUIRE(op.name() == std::string("float"));
        
        REQUIRE(op(true) == 1.0);
        REQUIRE(op(false) == 0.0);
        REQUIRE(op(int64_t(42)) == 42.0);
        REQUIRE(op(3.14) == 3.14);
        REQUIRE(op("123.45"s) == 123.45);
        REQUIRE(op("-67.89"s) == -67.89);
        REQUIRE(op("not a number"s) == 0.0);
        REQUIRE(op(""s) == 0.0);
        REQUIRE(op(NullType{}) == 0.0);
    }
}

TEST_CASE("Binary arithmetic operators", "[operator.binary.arithmetic]") {
    SECTION("OperatorAdd") {
        OperatorAdd op;
        REQUIRE(op.name() == std::string("+"));
        
        REQUIRE(op(int64_t(5), int64_t(3)) == int64_t(8));
        REQUIRE(op(int64_t(-5), int64_t(3)) == int64_t(-2));
        REQUIRE(op(3.14, 2.86) == Catch::Approx(6.0));
        REQUIRE(op(-1.5, 2.5) == Catch::Approx(1.0));
        REQUIRE(op(int64_t(5), 3.5) == Catch::Approx(8.5));
        REQUIRE(op(3.5, int64_t(5)) == Catch::Approx(8.5));
        REQUIRE(op("hello"s, " world"s) == "hello world");

        REQUIRE_INVALID_OPERANDS(op(true, int64_t(123)));
    }

    SECTION("OperatorSub") {
        OperatorSub op;
        REQUIRE(op.name() == std::string("-"));
        
        REQUIRE(op(int64_t(5), int64_t(3)) == int64_t(2));
        REQUIRE(op(int64_t(3), int64_t(5)) == int64_t(-2));
        REQUIRE(op(5.5, 2.5) == Catch::Approx(3.0));
        REQUIRE(op(2.5, 5.5) == Catch::Approx(-3.0));
        REQUIRE(op(int64_t(10), 3.5) == Catch::Approx(6.5));
        REQUIRE(op(3.5, int64_t(10)) == Catch::Approx(-6.5));

        REQUIRE_INVALID_OPERANDS(op("test"s, "test"s));
    }

    SECTION("OperatorMul") {
        OperatorMul op;
        REQUIRE(op.name() == std::string("*"));
        
        REQUIRE(op(int64_t(5), int64_t(3)) == int64_t(15));
        REQUIRE(op(int64_t(-5), int64_t(3)) == int64_t(-15));
        REQUIRE(op(2.5, 4.0) == Catch::Approx(10.0));
        REQUIRE(op(-2.5, 4.0) == Catch::Approx(-10.0));
        REQUIRE(op(int64_t(5), 2.5) == Catch::Approx(12.5));
        REQUIRE(op(2.5, int64_t(5)) == Catch::Approx(12.5));

        REQUIRE_INVALID_OPERANDS(op("test"s, "test"s));
        REQUIRE_INVALID_OPERANDS(op(true, 123));
    }
    
    SECTION("OperatorDiv") {
        OperatorDiv op;
        REQUIRE(op.name() == std::string("/"));
        
        REQUIRE(op(int64_t(15), int64_t(3)) == int64_t(5));
        REQUIRE(op(int64_t(16), int64_t(3)) == int64_t(5));
        REQUIRE(op(10.0, 4.0) == Catch::Approx(2.5));
        REQUIRE(op(-10.0, 4.0) == Catch::Approx(-2.5));
        REQUIRE(op(int64_t(10), 4.0) == Catch::Approx(2.5));
        REQUIRE(op(10.0, int64_t(4)) == Catch::Approx(2.5));
    }
    
    SECTION("OperatorMod") {
        OperatorMod op;
        REQUIRE(op.name() == std::string("%"));
        
        REQUIRE(op(int64_t(17), int64_t(5)) == int64_t(2));
        REQUIRE(op(int64_t(-17), int64_t(5)) == int64_t(-2));
        REQUIRE(op(int64_t(17), int64_t(-5)) == int64_t(2));

        REQUIRE_INVALID_OPERANDS(op(3.14, 2.0));
        REQUIRE_INVALID_OPERANDS(op("test"s, "test"s));
    }
}

TEST_CASE("Binary comparison operators", "[operator.binary.comparison]") {
    SECTION("OperatorEq") {
        OperatorEq op;
        REQUIRE(op.name() == std::string("=="));
        
        REQUIRE(op(int64_t(5), int64_t(5)) == true);
        REQUIRE(op(int64_t(5), int64_t(6)) == false);
        REQUIRE(op(3.14, 3.14) == true);
        REQUIRE(op(3.14, 3.15) == false);
        REQUIRE(op("hello"s, "hello"s) == true);
        REQUIRE(op("hello"s, "world"s) == false);
        REQUIRE(op(true, true) == true);
        REQUIRE(op(false, false) == true);
        REQUIRE(op(true, false) == false);
        REQUIRE(op(NullType{}, NullType{}) == true);
        REQUIRE(op(NullType{}, int64_t(0)) == false);
        REQUIRE(op(int64_t(5), 5.0) == true);
        REQUIRE(op(5.0, int64_t(5)) == true);
        REQUIRE(op(int64_t(5), 5.1) == false);
    }
}
