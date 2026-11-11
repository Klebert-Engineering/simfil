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

    /** Take ownership of the left operand and consume any operands following the supplied operator. */
    virtual auto parse(Parser&, std::unique_ptr<Expr> left, Token) const -> tl::expected<ExprPtr, Error> = 0;
    virtual int precedence() const = 0;
};


/**
 * Pratt parser with caller-registered prefix/infix handlers. The parser owns its
 * token sequence and borrows its environment and parselets for the parse lifetime.
 * Relaxed mode is used for incomplete completion queries; it can still return errors.
 */
class Parser
{
public:
    enum class Mode {
        Strict,  // Report syntax errors rather than recover missing input.
        Relaxed, // Try to recover from errors, if possible
    };

    struct Context {
        bool inPath = false;
    };

    Parser(Environment*, std::vector<Token> tokens, Mode mode);

    auto eof() const -> bool;

    /**
     * Parse one expression from the current token. The caller checks eof() when
     * the entire query must be consumed, rather than an enclosing subexpression.
     */
    auto parse() -> tl::expected<ExprPtr, Error>;

    /** Extend left while the next operator binds more tightly than prec. */
    auto parseInfix(tl::expected<ExprPtr, Error> left, int prec) -> tl::expected<ExprPtr, Error>;

    /** Parse above this binding power; optional returns a null ExprPtr when no prefix matches. */
    auto parsePrecedence(int precedence, bool optional = false) -> tl::expected<ExprPtr, Error>;

    /**
     * Parse an expression and consume the requested delimiter. Relaxed mode may
     * accept a missing delimiter so completion can inspect an unfinished query.
     */
    auto parseTo(Token::Type type) -> tl::expected<ExprPtr, Error>;

    /**
     * Parse comma-separated expressions through the closing token, consuming it.
     * An immediate closing token produces an empty list.
     */
    auto parseList(Token::Type stop) -> tl::expected<std::vector<ExprPtr>, Error>;

    /**
     * Returns token at offset relative to current position.
     */
    auto lookahead(std::size_t offset = 0) const -> const Token*;

    /** Test the next token without consuming it; relaxed EOF also counts as a match. */
    auto match(Token::Type t) const -> bool;

    /** Advance when a token is available; otherwise return a shared NIL sentinel. */
    auto consume() -> const Token&;

    /** Borrow the next token without advancing, or a shared NIL sentinel at EOF. */
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

    /** Schema completion parses structure without executing constant/custom functions. */
    bool simplify = true;
    Environment* const env;
    std::unordered_map<Token::Type, const PrefixParselet*> prefixParsers;
    std::unordered_map<Token::Type, const InfixParselet*> infixParsers;

private:
    auto findPrefixParser(const Token& t) const -> const PrefixParselet*;
    auto findInfixParser(const Token& t) const -> const InfixParselet*;

    Mode mode_ = Mode::Strict;
    std::vector<Token> tokens_;
    std::size_t pos_;
};

}
