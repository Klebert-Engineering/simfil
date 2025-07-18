#include "simfil/token.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

using namespace simfil;

static auto getFirstType(std::string_view input)
{
    auto tokens = tokenize(input).value();
    REQUIRE(tokens.size() >= 2);

    return tokens.at(0).type;
}

template <class Type>
static auto getFirst(std::string_view input, Token::Type t) -> Type
{
    auto tokens = tokenize(input).value();
    INFO(input);
    REQUIRE(tokens.size() >= 2);
    REQUIRE(tokens.at(0).type == t);

    return std::get<Type>(tokens.at(0).value);
}

static auto asInt(const std::string_view input)   {return getFirst<int64_t>(input, Token::Type::INT);}
static auto asFloat(const std::string_view input) {return getFirst<double>(input, Token::Type::FLOAT);}
static auto asStr(const std::string_view input)   {return getFirst<std::string>(input, Token::Type::STRING);}
static auto asRegexp(const std::string_view input)   {return getFirst<std::string>(input, Token::Type::REGEXP);}
static auto asWord(const std::string_view input)  {return getFirst<std::string>(input, Token::Type::WORD);}
static auto asError(const std::string_view input) {
    auto tokens = tokenize(input);
    INFO("Input: " << input);
    REQUIRE(!tokens.has_value());
    return tokens.error().message;
}

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
    REQUIRE(asError("0b12") == "Invalid digit for base at 4");
    REQUIRE(asError("0abc") == "Invalid input at 1 (abc)");
    REQUIRE(asError("0xGZ") == "Invalid input at 2 (GZ)");
    REQUIRE(asError("0x.2") == "Invalid input at 2 (.2)");
    REQUIRE(asError("0x0.2") == "Invalid input at 4 (2)");
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

    /* r"..." */
    REQUIRE(asStr("r\"\"") == "");
    REQUIRE(asStr("R\"\"") == "");
    REQUIRE(asStr("r\"abc\"") == "abc");
    REQUIRE(asStr("r\"\\\"abc\\\"\"") == "\"abc\"");
    REQUIRE(asStr("r\"\\\\\"\"") == "\\\"");
    REQUIRE(asStr("r\"\\a\"") == "\\a");

    /* r'...' */
    REQUIRE(asStr("r''") == "");
    REQUIRE(asStr("R''") == "");
    REQUIRE(asStr("r'\"'") == "\"");

    /* re'...' */
    REQUIRE(asRegexp("re''") == "");
    REQUIRE(asRegexp("RE''") == "");
    REQUIRE(asRegexp("re'\"'") == "\"");

    /* Quote mismatch */
    REQUIRE(asError("'abc") == "Quote mismatch at 4");
    REQUIRE(asError("abc'") == "Quote mismatch at 4");
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
    REQUIRE(getFirstType("==")     == Token::Type::OP_EQ);
    REQUIRE(getFirstType("=")      == Token::Type::OP_EQ);
    REQUIRE(getFirstType("<")      == Token::Type::OP_LT);
    REQUIRE(getFirstType("< <")    == Token::Type::OP_LT);
    REQUIRE(getFirstType("<<")     == Token::Type::OP_LSHIFT);
    REQUIRE(getFirstType("typeof") == Token::Type::OP_TYPEOF);
}

TEST_CASE("Tokenize symbols", "[token.icase]") {
    /* Test operator (and) for case insensitivity */
    REQUIRE(getFirstType("and") == Token::Type::OP_AND);
    REQUIRE(getFirstType("AND") == Token::Type::OP_AND);
    REQUIRE(getFirstType("And") == Token::Type::OP_AND);
    REQUIRE(getFirstType("AnD") == Token::Type::OP_AND);
}

TEST_CASE("Tokenize mixed", "[token.mixed]") {
    auto tokens = tokenize("1+.0 and true or 'test'").value();
    REQUIRE(tokens.size() == 7 + 1 /*EOF*/);
}

TEST_CASE("Token location", "[token.location]") {
    auto tokens = tokenize("1+.0 and true").value();
    REQUIRE(tokens.size() == 5 + 1 /*EOF*/);

    auto pos = 0;
    REQUIRE(tokens[0].begin == pos); REQUIRE(tokens[0].end == pos+1); pos+=1; /* 1    */
    REQUIRE(tokens[1].begin == pos); REQUIRE(tokens[1].end == pos+1); pos+=1; /* +    */
    REQUIRE(tokens[2].begin == pos); REQUIRE(tokens[2].end == pos+2); pos+=3; /* .0   */
    REQUIRE(tokens[3].begin == pos); REQUIRE(tokens[3].end == pos+3); pos+=4; /* and  */
    REQUIRE(tokens[4].begin == pos); REQUIRE(tokens[4].end == pos+4); pos+=4; /* true */
}
