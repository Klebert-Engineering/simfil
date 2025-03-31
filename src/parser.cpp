#include "simfil/parser.h"

#include <cassert>
#include <stdexcept>
#include "fmt/core.h"
#include "simfil/exception-handler.h"

namespace simfil
{

ParserError::ParserError(const std::string& msg)
    : std::runtime_error(msg)
    , range_(0, 0)
{}

ParserError::ParserError(const std::string& msg, const Token& token)
    : std::runtime_error(msg)
    , range_(token.begin, token.end)
{}

ParserError::ParserError(const std::string& msg, RangeType range)
    : std::runtime_error(msg)
    , range_(std::move(range))
{}

auto ParserError::range() const noexcept -> ParserError::RangeType
{
    return range_;
}

Parser::Parser(Environment* env, std::vector<Token> tokens)
    : env(env)
    , tokens_(std::move(tokens))
    , pos_(0)
{
    assert(env);
}

Parser::Parser(Environment* env, std::string_view expr)
    : env(env)
    , tokens_(tokenize(expr))
    , pos_(0)
{
    assert(env);
}

auto Parser::eof() const -> bool
{
    return pos_ >= tokens_.size();
}

auto Parser::lookahead(std::size_t offset) const -> const Token*
{
    offset += pos_;
    if (offset < tokens_.size())
        return &tokens_[offset];
    return nullptr;
}

auto Parser::match(Token::Type t) const -> bool
{
    if (!eof())
        if (auto head = lookahead())
            return head->type == t;
    return false;
}

auto Parser::consume() -> const Token&
{
    if (!eof())
        return tokens_[pos_++];
    raise<ParserError>("Parser end of input (consume)");
}

auto Parser::current() const -> const Token&
{
    if (!eof())
        return tokens_[pos_];
    raise<ParserError>("Parser end of input");
}

auto Parser::precedence(Token token) const -> int
{
    if (auto parser = findInfixParser(token))
        return parser->precedence();
    return 0;
}

auto Parser::parseInfix(ExprPtr left, int prec) -> ExprPtr
{
    while (prec < precedence(current())) {
        auto token = consume();
        auto parser = findInfixParser(token);
        if (!parser) {
            auto msg = fmt::format("Could not parse right side of '{}'",
                                   token.toString());
            raise<ParserError>(msg, token);
        }

        left = parser->parse(*this, std::move(left), token);
    }
    return left;
}

auto Parser::parsePrecedence(int precedence, bool optional) -> ExprPtr
{
    auto token = current();
    auto parser = findPrefixParser(token);
    if (!parser) {
        if (!optional) {
            auto msg = fmt::format("Unexpected input '{}'",
                                   token.toString());

            raise<ParserError>(msg, token);
        }
        return nullptr;
    }
    consume();

    auto left = parser->parse(*this, token);
    return parseInfix(std::move(left), precedence);
}

auto Parser::parse() -> ExprPtr
{
    return parsePrecedence(0);
}

auto Parser::parseTo(Token::Type type) -> ExprPtr
{
    auto expr = parse();
    if (!expr) {
        auto msg = fmt::format("Expected expression after '{}'",
                               current().toString());
        raise<ParserError>(msg, current());
    }

    if (!match(type)) {
        auto msg = fmt::format("Expected '{}', got '{}'",
                               Token::toString(type),
                               current().toString());
        raise<ParserError>(msg, current());
    }
    consume();

    return expr;
}

auto Parser::parseList(Token::Type stop) -> std::vector<ExprPtr>
{
    std::vector<ExprPtr> exprs;
    const auto begin = current().begin;

    if (match(stop)) {
        consume();
        return exprs;
    }

    for (;;) {
        exprs.emplace_back(parse());

        if (!match(stop)) {
            if (match(Token::COMMA)) {
                consume();
            } else {
              auto msg = fmt::format("Expected '{}' but got '{}'",
                                     Token::toString(stop),
                                     current().toString());
              raise<ParserError>(ParserError(msg, {begin, current().end}));
            }
        } else {
            consume();
            break;
        }
    }

    return exprs;
}

auto Parser::findPrefixParser(const Token& t) const -> const PrefixParselet*
{
    if (auto iter = prefixParsers.find(t.type); iter != prefixParsers.end())
        return iter->second.get();
    return nullptr;
}

auto Parser::findInfixParser(const Token& t) const -> const InfixParselet*
{
    if (auto iter = infixParsers.find(t.type); iter != infixParsers.end())
        return iter->second.get();
    return nullptr;
}
}
