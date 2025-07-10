// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#include "simfil/token.h"
#include "simfil/expression.h"
#include "simfil/error.h"

#include <tl/expected.hpp>
#include <unordered_map>
#include <memory>
#include <vector>

namespace simfil
{

struct Environment;
class Parser;

/**
 * Parselet for parsing prefix expressions.
 * Examples are: Numbers, identifiers or prefix operators.
 */
class PrefixParselet
{
public:
    virtual ~PrefixParselet() = default;

    /**
     * Parse
     *
     * @param p  Calling parser instance.
     * @param t  Current token for which this parser got called.
     * @return  Parsed expression object.
     */
    virtual auto parse(Parser& p, Token t) const -> tl::expected<ExprPtr, Error> = 0;
};

/**
 * Parselet for parsing infix or postfix expressions.
 * Examples are infix or postfix operators.
 */
class InfixParselet
{
public:
    virtual ~InfixParselet() = default;

    virtual auto parse(Parser&, std::unique_ptr<Expr> left, Token) const -> tl::expected<ExprPtr, Error> = 0;
    virtual int precedence() const = 0;
};


class Parser
{
public:
    enum class Mode {
        Strict,  // Panic on errors
        Relaxed, // Try to recover from errors, if possible
    };

    struct Context {
        bool inPath = false;
    };

    Parser(Environment*, std::vector<Token> tokens, Mode mode);

    auto eof() const -> bool;

    /**
     * Parsing entry point(s).
     */
    auto parse() -> tl::expected<ExprPtr, Error>;
    auto parseInfix(tl::expected<ExprPtr, Error> left, int prec) -> tl::expected<ExprPtr, Error>;
    auto parsePrecedence(int precedence, bool optional = false) -> tl::expected<ExprPtr, Error>;

    /**
     * Parse expression and match next token against given token type.
     */
    auto parseTo(Token::Type type) -> tl::expected<ExprPtr, Error>;

    /**
     * Helper for parsing comma separated expressions.
     */
    auto parseList(Token::Type stop) -> tl::expected<std::vector<ExprPtr>, Error>;

    /**
     * Returns token at offset relative to current position.
     */
    auto lookahead(std::size_t offset = 0) const -> const Token*;
    auto match(Token::Type t) const -> bool;

    auto consume() -> const Token&;
    auto current() const -> const Token&;

    /**
     * Returns the precedence of the parselet of the given token or 0.
     */
    auto precedence(const Token& token) const -> int;

    /**
     * Get the current parsing mode.
     */
    auto mode() const -> Mode;
    auto relaxed() const -> bool;

    Context ctx;
    Environment* const env;
    std::unordered_map<Token::Type, std::unique_ptr<PrefixParselet>> prefixParsers;
    std::unordered_map<Token::Type, std::unique_ptr<InfixParselet>> infixParsers;

private:
    auto findPrefixParser(const Token& t) const -> const PrefixParselet*;
    auto findInfixParser(const Token& t) const -> const InfixParselet*;

    Mode mode_ = Mode::Strict;
    std::vector<Token> tokens_;
    std::size_t pos_;
};

}
