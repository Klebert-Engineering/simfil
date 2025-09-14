#include "simfil/parser.h"

#include <cassert>
#include <memory>
#include "fmt/core.h"
#include "simfil/expression.h"
#include "simfil/result.h"
#include "simfil/token.h"
#include "src/expressions.h"
#include "expected.h"


namespace simfil
{

namespace
{

class NOOPExpr : public Expr
{
public:
    auto type() const -> Type override
    {
        return Type::FIELD;
    }

    auto ieval(Context ctx, const Value& val, const ResultFn& ores) -> tl::expected<Result, Error> override
    {
        return Result::Stop;
    }

    auto clone() const -> ExprPtr override
    {
        return std::make_unique<NOOPExpr>();
    }

    void accept(ExprVisitor& v) override
    {
        v.visit(*this);
    }

    auto toString() const -> std::string override
    {
        return "noop";
    }
};

}

Parser::Parser(Environment* env, std::vector<Token> tokens, Mode mode)
    : env(env)
    , mode_(mode)
    , tokens_(std::move(tokens))
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
    if (!eof()) {
        if (auto head = lookahead())
            return head->type == t;
    }

    return mode() == Mode::Relaxed;
}

auto Parser::consume() -> const Token&
{
    static Token eofToken(Token::NIL, 0, 0);

    if (!eof())
        return tokens_[pos_++];

    return eofToken;
}

auto Parser::current() const -> const Token&
{
    static Token eofToken(Token::NIL, 0, 0);

    if (!eof())
        return tokens_[pos_];

    return eofToken;
}

auto Parser::precedence(const Token& token) const -> int
{
    if (auto parser = findInfixParser(token))
        return parser->precedence();
    return 0;
}

auto Parser::mode() const -> Mode
{
    return mode_;
}

auto Parser::relaxed() const -> bool
{
    return mode_ == Mode::Relaxed;
}

auto Parser::parseInfix(expected<ExprPtr, Error> left, int prec) -> expected<ExprPtr, Error>
{
    TRY_EXPECTED(left);

    while (prec < precedence(current())) {
        auto token = consume();
        auto parser = findInfixParser(token);
        if (!parser) {
            if (relaxed())
                return std::make_unique<NOOPExpr>();

            return unexpected<Error>(
                Error::ParserError,
                fmt::format("Could not parse right side of '{}'",
                            token.toString()),
                token);
        }

        auto next = parser->parse(*this, std::move(*left), token);
        TRY_EXPECTED(next);

        left = std::move(*next);
    }
    return left;
}

auto Parser::parsePrecedence(int precedence, bool optional) -> expected<ExprPtr, Error>
{
    auto token = current();
    auto parser = findPrefixParser(token);
    if (!parser) {
        if (!optional)
          return unexpected<Error>(
              Error::ParserError,
              fmt::format("Unexpected input '{}'", token.toString()), token);

        return nullptr;
    }
    consume();

    auto left = parser->parse(*this, token);
    TRY_EXPECTED(left);

    return parseInfix(std::move(*left), precedence);
}

auto Parser::parse() -> expected<ExprPtr, Error>
{
    return parsePrecedence(0);
}

auto Parser::parseTo(Token::Type type) -> expected<ExprPtr, Error>
{
    auto expr = parse();
    TRY_EXPECTED(expr);

    if (!*expr) {
        if (relaxed())
            return std::make_unique<NOOPExpr>();

        return unexpected<Error>(
            Error::ParserError,
            fmt::format("Expected expression after '{}'", current().toString()),
            current());
    }

    if (!match(type)) {
        if (!relaxed()) {
            return unexpected<Error>(
                Error::ParserError,
                fmt::format("Expected '{}', got '{}'", Token::toString(type), current().toString()),
                current());
        }
    } else {
        consume();
    }

    return expr;
}

auto Parser::parseList(Token::Type stop) -> expected<std::vector<ExprPtr>, Error>
{
    std::vector<ExprPtr> exprs;
    const auto begin = current().begin;

    if (match(stop)) {
        consume();
        return exprs;
    }

    for (;;) {
        auto res = parse();
        TRY_EXPECTED(res);

        exprs.emplace_back(std::move(res.value()));
        if (!match(stop)) {
            if (match(Token::COMMA)) {
                consume();
            } else if (mode() == Mode::Relaxed && match(Token::NIL)) {
                // We are at eof, so we might just ignore the missing token and
                // break.
                break;
            } else {
              return unexpected<Error>(Error::ParserError,
                                       fmt::format("Expected '{}' but got '{}'",
                                                   Token::toString(stop),
                                                   current().toString()),
                                       current());
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
