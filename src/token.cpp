// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#include "simfil/token.h"
#include "simfil/error.h"
#include "src/expected.h"

#include <cctype>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <optional>
#include <cstring>
#include <unordered_map>
#include <algorithm>
#include <ostream>

namespace
{

std::string downcase(std::string s)
{
    std::ranges::transform(s.begin(), s.end(), s.begin(), [](auto c) {
        return tolower(c);
    });
    return s;
}

}

namespace simfil
{

using namespace std::string_literals;

auto Token::toString() const -> std::string
{
    switch (type) {
    case Token::INT:
        return std::to_string(std::get<int64_t>(value));
    case Token::FLOAT:
        return std::to_string(std::get<double>(value));
    case Token::STRING:
        return "'"s + std::get<std::string>(value) + "'"s;
    case Token::REGEXP:
        return "re'"s + std::get<std::string>(value) + "'"s;
    case Token::WORD:
        return std::get<std::string>(value);
    default:
        return Token::toString(type);
    }
}

auto Token::toString(Type t) -> std::string
{
    switch (t) {
    case Token::NIL:          return "EOF";
    case Token::LPAREN:       return "(";
    case Token::RPAREN:       return ")";
    case Token::LBRACK:       return "[";
    case Token::RBRACK:       return "]";
    case Token::LBRACE:       return "{";
    case Token::RBRACE:       return "}";
    case Token::COMMA:        return ",";
    case Token::COLON:        return ":";
    case Token::DOT:          return ".";
    case Token::WILDCARD:     return "**";
    case Token::SELF:         return "_";
    case Token::C_NULL:       return "null";
    case Token::C_TRUE:       return "true";
    case Token::C_FALSE:      return "false";
    case Token::OP_ADD:       return "+";
    case Token::OP_SUB:       return "-";
    case Token::OP_TIMES:     return "*";
    case Token::OP_DIV:       return "/";
    case Token::OP_MOD:       return "%";
    case Token::OP_LSHIFT:    return "<<";
    case Token::OP_RSHIFT:    return ">>";
    case Token::OP_BITAND:    return "&";
    case Token::OP_BITOR:     return "|";
    case Token::OP_BITXOR:    return "^";
    case Token::OP_BITINV:    return "~";
    case Token::OP_NOT:       return "not";
    case Token::OP_AND:       return "and";
    case Token::OP_OR:        return "or";
    case Token::OP_EQ:        return "==";
    case Token::OP_NOT_EQ:    return "!=";
    case Token::OP_LT:        return "<";
    case Token::OP_LTEQ:      return "<=";
    case Token::OP_GT:        return ">";
    case Token::OP_GTEQ:      return ">=";
    case Token::OP_BOOL:      return "?";
    case Token::OP_LEN:       return "#";
    case Token::OP_TYPEOF:    return "typeof";
    case Token::OP_CAST:      return "as";
    case Token::OP_UNPACK:    return "...";
    /* Values */
    case Token::INT:          return "<int>";
    case Token::FLOAT:        return "<float>";
    case Token::STRING:       return "<string>";
    case Token::REGEXP:       return "<regexp>";
    case Token::WORD:         return "<word>";
    };
    return "";
}

auto Token::containsPoint(size_t point) const -> bool
{
    return (begin == end && begin == point) || (begin <= point && point <= end);
}

auto operator<<(std::ostream& o, const Token& t) -> std::ostream&
{
    return o << t.toString();
}

struct Scanner
{
    static constexpr struct Skip_ {} Skip {};

    const std::string_view orig_;
    std::string_view sv_;
    std::size_t pos_;
    mutable std::optional<Error> error_;

    explicit Scanner(std::string_view sv)
        : orig_(sv)
        , sv_(sv)
        , pos_(0)
    {}

    explicit operator bool() const
    {
        return !sv_.empty();
    }

    auto skip(std::size_t n = 1)
    {
        pos_ += n;
        sv_.remove_prefix(n);
    }

    auto at(std::size_t offset) const
    {
        return sv_.size() > offset ? sv_[offset] : '\0';
    }

    auto pop()
    {
        auto c = at(0);
        skip();
        return c;
    }

    auto match(const char* str) const
    {
        return sv_.compare(0, std::strlen(str), str) == 0;
    }

    auto match(const char* str, const Skip_&)
    {
        if (match(str)) {
            skip(std::strlen(str));
            return true;
        }
        return false;
    }

    auto fail(std::string msg) const -> Error&
    {
        msg += " at "s + std::to_string(pos_);
        if (pos_ < orig_.size())
            msg += " ("s + std::string(orig_.substr(pos_)) + ")"s;

        error_.emplace(Error::ParserError, std::move(msg));
        return *error_;
    }

    auto pos() const
    {
        return pos_;
    }

    auto hasError() const
    {
        return error_.has_value();
    }

    auto error() const -> Error&
    {
        return *error_;
    }
};

void skipWhitespace(Scanner& s)
{
    while (isspace(s.at(0)))
        s.skip();
}

auto scanWord(Scanner& s) -> std::optional<Token>
{
    if (s.hasError())
        return {};

    static const std::unordered_map<std::string, Token::Type> keyword_tab = {
        /* lowercase-op-name | token-type */
        {"_",                  Token::SELF},
        {"and",                Token::OP_AND},
        {"or",                 Token::OP_OR},
        {"not",                Token::OP_NOT},
        {"typeof",             Token::OP_TYPEOF},
        {"true",               Token::C_TRUE},
        {"false",              Token::C_FALSE},
        {"null",               Token::C_NULL},
        {"as",                 Token::OP_CAST},
    };

    const auto isWordStart = [](auto c) {
        return isalpha(c) || c == '_' || c == '\\' || c == '$';
    };

    if (isWordStart(s.at(0))) {
        auto begin = s.pos();
        std::string text;
        do {
            if (s.match("\\", Scanner::Skip)) {
                if (!s) {
                    s.fail("Unfinished escape sequence");
                    return {};
                }
            }

            text.push_back(s.pop());
        } while (s && (isalnum(s.at(0)) || s.at(0) == '_' || s.at(0) == '\\'));

        if (auto iter = keyword_tab.find(downcase(text)); iter != keyword_tab.end())
            return Token(iter->second, begin, s.pos());

        return Token(Token::WORD, text, begin, s.pos());
    }

    return {};
}

std::optional<Token> scanStringLiteral(Scanner& s)
{
    if (s.hasError())
        return {};

    // Test for raw strings
    const auto raw =
        s.match("r'") || s.match("R'") ||
        s.match("r\"") || s.match("R\"");

    // Test for regexp
    const auto regexp =
        s.match("re'") || s.match("RE'") ||
        s.match("re\"") || s.match("RE\"");

    if (raw)
        s.skip(1);
    else if (regexp)
        s.skip(2);

    const auto quote = s.at(0);
    if (quote == '"' || quote == '\'') {
        auto begin = s.pos();
        s.skip();

        std::string text;
        while (s) {
            if (s.at(0) == quote)
                break;

            if (s.match("\\", Scanner::Skip)) {
                if (!s) {
                    s.fail("Unfinished escape sequence");
                    return {};
                }

                if (raw || regexp) {
                    if (s.at(0) == quote)
                        text.push_back(s.pop());
                    else
                        text.push_back('\\');
                } else {
                    switch (s.at(0)) {
                        case 'n': text.push_back('\n'); break;
                        case 'r': text.push_back('\r'); break;
                        case 't': text.push_back('\t'); break;
                        default:  text.push_back(s.at(0)); break;
                    }

                    s.skip();
                }
                continue;
            }

            if (!s)
                break;

            text.push_back(s.pop());
        }

        if (!s || s.pop() != quote) {
            s.fail("Quote mismatch");
            return {};
        }

        if (regexp)
            return Token(Token::REGEXP, text, begin, s.pos());
        return Token(Token::STRING, text, begin, s.pos());
    }

    return {};
}

std::optional<Token> scanNumber(Scanner& s)
{
    if (s.hasError())
        return {};

    const auto chr2digit = [](const Scanner& s, int base, auto c)
    {
        if (c >= '0' && c <= '1')
            return c - '0';
        else if (base >= 10) {
            if (c <= '9')
                return c - '0';
            if (base >= 16) {
                if (c >= 'a' && c <= 'f')
                    return 10 + c - 'a';
                if (c >= 'A' && c <= 'F')
                    return 10 + c - 'A';
            }
        }

        s.fail("Invalid digit for base");
        return 0;
    };

    const auto parseSciSuffix = [](Scanner& s)
    {
        if (tolower(s.at(0)) == 'e') {
            s.pop();

            const auto neg = s.match("-", Scanner::Skip);
            double exp = 0;
            while (isdigit(s.at(0))) {
                exp *= 10.;
                exp += s.pop() - '0';
            }

            if (neg)
                return 1.0 / std::pow(10.0, exp);
            return std::pow(10.0, exp);
        }

        return 1.0;
    };

    const auto begin = s.pos();
    auto base = 10;
    if (s.match("0x", Scanner::Skip))
        base = 16;
    else if (s.match("0b", Scanner::Skip))
        base = 2;

    if (base != 10 && !isxdigit(s.at(0))) {
        s.fail("Expected at least one digit after base specifier");
        return {};
    }

    if (isdigit(s.at(0)) || (s.at(0) == '.' && isdigit(s.at(1))) || base > 10) {
        int64_t n = 0;
        while (isdigit(s.at(0)) || (base > 10 && isxdigit(s.at(0)))) {
            n *= base;
            n += chr2digit(s, base, s.pop());
        }

        if (s.match(".", Scanner::Skip)) {
            if (base != 10) {
                s.fail("Decimal point is allowed for base 10 numbers only");
                return {};
            }

            auto f = static_cast<double>(n);
            for (auto i = 1; isdigit(s.at(0)); ++i) {
                f += (s.pop() - '0') / std::pow(10, i);
            }

            f *= parseSciSuffix(s);
            if (isalpha(s.at(0)) || s.at(0) == '.') {
                s.fail("Unexpected non-digit character");
                return {};
            }

            return Token(Token::FLOAT, f, begin, s.pos());
        }

        /* SCI notation results in float if exponent != 0 (=1) */
        if (auto sci = parseSciSuffix(s); sci != 1)
            return Token(Token::FLOAT, static_cast<double>(n) * sci, begin, s.pos());
        if (isalpha(s.at(0)) || s.at(0) == '.') {
            s.fail("Unexpected non-digit character");
            return {};
        }

        return Token(Token::INT, n, begin, s.pos());
    }

    return {};
}

std::optional<Token> scanSyntax(Scanner& s)
{
    if (s.hasError())
        return {};

    static const std::pair<const char*, Token::Type> tab[] = {
        {"(", Token::LPAREN},
        {")", Token::RPAREN},
        {"{", Token::LBRACE},
        {"}", Token::RBRACE},
        {"[", Token::LBRACK},
        {"]", Token::RBRACK},
        {",", Token::COMMA},
        {":", Token::COLON},
        {"...",Token::OP_UNPACK},
        {"**",Token::WILDCARD},
        {".", Token::DOT},
        {"+", Token::OP_ADD},
        {"-", Token::OP_SUB},
        {"*", Token::OP_TIMES},
        {"/", Token::OP_DIV},
        {"%", Token::OP_MOD},
        {"<<",Token::OP_LSHIFT},
        {">>",Token::OP_RSHIFT},
        {"~", Token::OP_BITINV},
        {"|", Token::OP_BITOR},
        {"&", Token::OP_BITAND},
        {"#", Token::OP_LEN},
        {"^", Token::OP_BITXOR},
        {"?", Token::OP_BOOL},
        {"<=",Token::OP_LTEQ},
        {"<", Token::OP_LT},
        {">=",Token::OP_GTEQ},
        {">", Token::OP_GT},
        {"==",Token::OP_EQ},
        {"!=",Token::OP_NOT_EQ},
        {"=", Token::OP_EQ}, /* Alias for == */
    };

    auto begin = s.pos();
    for (const auto& op : tab)
        if (s.match(op.first, Scanner::Skip))
            return Token(op.second, begin, s.pos());

    return {};
}

auto tokenize(std::string_view expr) -> expected<std::vector<Token>, Error>
{
    std::vector<Token> tokens;

    Scanner s(expr);
    while (s) {
        skipWhitespace(s);
        if (auto t = scanNumber(s))
            tokens.push_back(std::move(*t));
        else if (auto t = scanStringLiteral(s))
            tokens.push_back(std::move(*t));
        else if (auto t = scanWord(s))
            tokens.push_back(std::move(*t));
        else if (auto t = scanSyntax(s))
            tokens.push_back(std::move(*t));
        else {
            if (s.at(0) != '\0')
                return unexpected<Error>(s.fail("Invalid input"));
        }

        if (s.hasError())
            return unexpected<Error>(std::move(s.error()));
    }
    tokens.emplace_back(Token::NIL, expr.size(), expr.size());

    return tokens;
}

}
