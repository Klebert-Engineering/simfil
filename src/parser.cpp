#include "simfil/parser.h"

#include <cassert>
#include <memory>
#include <stdexcept>
#include "fmt/core.h"
#include "simfil/exception-handler.h"
#include "simfil/expression.h"
#include "simfil/result.h"
#include "src/expressions.h"


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

    auto ieval(Context ctx, const Value& val, const ResultFn& ores) -> Result override
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

template <class ErrorType, class ...Args>
[[nodiscard]]
auto raiseOrNOOP(const Parser& p, Args... args)
{
    if (p.mode() == Parser::Mode::Relaxed)
        return std::make_unique<NOOPExpr>();

    raise<ErrorType>(std::forward<Args>(args)...);
    assert(0);
}

}

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

Parser::Parser(Environment* env, std::string_view expr, Parser::Mode mode)
    : env(env)
    , mode_(mode)
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
    if (!eof()) {
        if (auto head = lookahead())
            return head->type == t;
    }

    return mode() == Mode::Relaxed;
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

auto Parser::parseInfix(ExprPtr left, int prec) -> ExprPtr
{
    while (prec < precedence(current())) {
        auto token = consume();
        auto parser = findInfixParser(token);
        if (!parser) {
            auto msg = fmt::format("Could not parse right side of '{}'",
                                   token.toString());
            return raiseOrNOOP<ParserError>(*this, msg, token);
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

            return raiseOrNOOP<ParserError>(*this, msg, token);
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
        return raiseOrNOOP<ParserError>(*this, msg, current());
    }

    if (!match(type)) {
        if (mode() != Mode::Relaxed) {
            auto msg = fmt::format("Expected '{}', got '{}'",
                                Token::toString(type),
                                current().toString());
            raise<ParserError>(msg, current());
        }
    } else {
        consume();
    }

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
            } else if (mode() == Mode::Relaxed && match(Token::NIL)) {
                // We are at eof, so we might just ignore the missing token and
                // break.
                break;
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
