#include "simfil/parser.h"

#include <cassert>
#include <stdexcept>

namespace simfil
{

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
    raise<std::runtime_error>("Parser EOF (consume)");
}

auto Parser::current() const -> const Token&
{
    if (!eof())
        return tokens_[pos_];
    raise<std::runtime_error>("Parser EOF (current)");
}

auto Parser::precedence(const Token& token) const -> int
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
        if (!parser)
            raise<std::runtime_error>("Error parsing infix expression");

        left = parser->parse(*this, std::move(left), token);
    }
    return left;
}

auto Parser::parsePrecedence(int precedence, bool optional) -> ExprPtr
{
    auto token = current();
    auto parser = findPrefixParser(token);
    if (!parser) {
        if (!optional)
            raise<std::runtime_error>("Error parsing left expression");
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
    if (!expr)
        raise<std::runtime_error>("Expected expression"s);

    if (!match(type)) {
        auto msg = "Expected "s + Token::toString(type) +
            " got "s + current().toString();
        raise<std::runtime_error>(msg);
    }
    consume();

    return expr;
}

auto Parser::parseList(Token::Type stop) -> std::vector<ExprPtr>
{
    std::vector<ExprPtr> exprs;

    if (match(stop)) {
        consume();
        return exprs;
    }

    for (;;) {
        exprs.emplace_back(parse());

        if (!match(stop)) {
            if (match(Token::COMMA))
                consume();
            else
                raise<std::runtime_error>("Expected "s + Token::toString(stop) +
                                         " got "s + current().toString());
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
