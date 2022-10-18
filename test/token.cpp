#include "simfil/token.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

using namespace simfil;

static auto getFirstType(std::string_view input)
{
    auto tokens = tokenize(input);
    REQUIRE(tokens.size() >= 2);

    return tokens.at(0).type;
}

template <class Type>
static auto getFirst(std::string_view input, Token::Type t) -> Type
{
    auto tokens = tokenize(input);
    REQUIRE(tokens.size() >= 2);
    REQUIRE(tokens.at(0).type == t);

    return std::get<Type>(tokens.at(0).value);
}

static auto asInt(std::string_view input)   {return getFirst<int64_t>(input, Token::Type::INT);}
static auto asFloat(std::string_view input) {return getFirst<double>(input, Token::Type::FLOAT);}
static auto asStr(std::string_view input)   {return getFirst<std::string>(input, Token::Type::STRING);}
static auto asWord(std::string_view input)  {return getFirst<std::string>(input, Token::Type::WORD);}

TEST_CASE("Tokenize integers", "[token.integer]") {
    /* Decimal */
    REQUIRE(asInt("0") == 0);
    REQUIRE(asInt("1") == 1);
    REQUIRE(asInt("123456789") == 123456789);

    /* Hex */
    REQUIRE(asInt("0xf123A") == 0xf123a);

    /* Binary */
    REQUIRE(asInt("0b1001011") == 0b1001011);

    /* Invalid */
    CHECK_THROWS(asInt("0b12"));  // Invalid characters for base  2
    CHECK_THROWS(asInt("0abc"));  // Invalid characters for base 10
    CHECK_THROWS(asInt("0xGZ"));  // Invalid characters for base 16
    CHECK_THROWS(asInt("0x.2"));  // Invalid decimal point
    CHECK_THROWS(asInt("0x0.2")); // Invalid decimal point
}

TEST_CASE("Tokenize floats", "[token.float]") {
    /* Default Format */
    REQUIRE(asFloat("0.") == 0.);
    REQUIRE(asFloat("1.") == 1.);
    REQUIRE(asFloat(".1") == .1);
    REQUIRE(asFloat("123.") == 123.);
    REQUIRE(asFloat(".123") == Catch::Approx(.123));

    /* SCI */
    REQUIRE(asFloat("0.e0") == 0.);
    REQUIRE(asFloat("1.e0") == 1.);
    REQUIRE(asFloat(".1e0") == .1);
    REQUIRE(asInt("1e0")    == 1);
    REQUIRE(asFloat("1e1")  == 10.);
    REQUIRE(asFloat("1e-1") == .1);
    REQUIRE(asFloat("1e5")  == 100000.);
    REQUIRE(asFloat("1e-5") == 0.00001);
}

TEST_CASE("Tokenize strings", "[token.string]") {
    /* '...' */
    REQUIRE(asStr("''") == "");
    REQUIRE(asStr("'abc'") == "abc");
    REQUIRE(asStr("'\\'abc\\''") == "'abc'");

    /* "..." */
    REQUIRE(asStr("\"\"") == "");
    REQUIRE(asStr("\"abc\"") == "abc");
    REQUIRE(asStr("\"\\\"abc\\\"\"") == "\"abc\"");

    /* Quote mismatch */
    CHECK_THROWS(asStr("'abc"));
    CHECK_THROWS(asStr("abc'"));
}

TEST_CASE("Tokenize words", "[token.word]") {
    /* Legal characters */
    REQUIRE(asWord("abc")  == "abc");
    REQUIRE(asWord("_abc") == "_abc");
    REQUIRE(asWord("abc0") == "abc0");
    REQUIRE(asWord("$abc") == "$abc");

    /* Escaped characters */
    REQUIRE(asWord("\\0abc") == "0abc");
    REQUIRE(asWord("\\ abc") == " abc");
}

TEST_CASE("Tokenize symbols", "[token.symbol]") {
    /* Some random tokens */
    REQUIRE(getFirstType("_")      == Token::Type::SELF);
    REQUIRE(getFirstType("+")      == Token::Type::OP_ADD);
    REQUIRE(getFirstType("=~")     == Token::Type::OP_MATCH);
    REQUIRE(getFirstType("==")     == Token::Type::OP_EQ);
    REQUIRE(getFirstType("=")      == Token::Type::OP_EQ);
    REQUIRE(getFirstType("<")      == Token::Type::OP_LT);
    REQUIRE(getFirstType("< <")    == Token::Type::OP_LT);
    REQUIRE(getFirstType("<<")     == Token::Type::OP_LSHIFT);
    REQUIRE(getFirstType("typeof") == Token::Type::OP_TYPEOF);
}

TEST_CASE("Tokenize symbols", "[token.icase]") {
    /* Test operator (and) for case insensitivity */
    REQUIRE(getFirstType("and")     == Token::Type::OP_AND);
    REQUIRE(getFirstType("AND")     == Token::Type::OP_AND);
    REQUIRE(getFirstType("And")     == Token::Type::OP_AND);
    REQUIRE(getFirstType("AnD")     == Token::Type::OP_AND);
}

TEST_CASE("Tokenize mixed", "[token.mixed]") {
    auto tokens = tokenize("1+.0 and true or 'test'");
    REQUIRE(tokens.size() == 7 + 1 /*EOF*/);
}

TEST_CASE("Token location", "[token.location]") {
    auto tokens = tokenize("1+.0 and true");
    REQUIRE(tokens.size() == 5 + 1 /*EOF*/);

    auto pos = 0;
    REQUIRE(tokens[0].begin == pos); REQUIRE(tokens[0].end == pos+1); pos+=1; /* 1    */
    REQUIRE(tokens[1].begin == pos); REQUIRE(tokens[1].end == pos+1); pos+=1; /* +    */
    REQUIRE(tokens[2].begin == pos); REQUIRE(tokens[2].end == pos+2); pos+=3; /* .0   */
    REQUIRE(tokens[3].begin == pos); REQUIRE(tokens[3].end == pos+3); pos+=4; /* and  */
    REQUIRE(tokens[4].begin == pos); REQUIRE(tokens[4].end == pos+4); pos+=4; /* true */
}
