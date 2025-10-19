#include "simfil/simfil.h"
#include "simfil/model/nodes.h"
#include "simfil/model/string-pool.h"
#include "simfil/sourcelocation.h"
#include "simfil/token.h"
#include "simfil/operator.h"
#include "simfil/value.h"
#include "simfil/function.h"
#include "simfil/expression.h"
#include "simfil/parser.h"
#include "simfil/environment.h"
#include "simfil/model/model.h"
#include "simfil/types.h"
#include "simfil/error.h"
#include "fmt/core.h"

#include "expressions.h"
#include "expression-patterns.h"
#include "completion.h"
#include "expected.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <deque>
#include <cassert>
#include <vector>

namespace simfil
{

using namespace std::string_literals;

namespace strings
{
static constexpr std::string_view TypenameNull("null");
static constexpr std::string_view TypenameBool("bool");
static constexpr std::string_view TypenameInt("int");
static constexpr std::string_view TypenameFloat("float");
static constexpr std::string_view TypenameString("string");
}

/**
 * Parser precedence groups.
 */
enum Precedence {
    PATH        = 12, // a.b
    SUBEXPR     = 11, // a{b}
    SUBSCRIPT   = 10, // a[b]
    POST_UNARY  = 9,  // a?, a exists, a...
    UNARY       = 8,  // not, -, ~
    CAST        = 7,  // a as b
    CUSTOM      = 7,  //
    PRODUCT     = 6,  // *, /, %
    TERM        = 5,  // +, -
    BITWISE     = 4,  // <<, >>, &, |, ^
    COMPARISON  = 3,  // <, <=, >, >=
    EQUALITY    = 2,  // =, ==, !=
    LOGIC       = 1,  // and, or
};

/**
 * Returns if a word should be parsed as a symbol (string).
 * This is true for all UPPER_CASE words.
 */
static auto isSymbolWord(std::string_view sv) -> bool
{
    auto numUpperCaseLetters = 0;
    return std::ranges::all_of(sv.begin(), sv.end(), [&numUpperCaseLetters](auto c) {
       if (std::isupper(c)) {
           ++numUpperCaseLetters;
           return true;
       }
       return c == '_' || std::isdigit(c) != 0;
    }) && numUpperCaseLetters > 0;
}

/**
 * RIIA Helper for calling function at destruction.
 */
template <class Fun>
struct scoped {
    Fun f;

    explicit scoped(Fun f) : f(std::move(f)) {}
    scoped(scoped&& s) noexcept : f(std::move(s.f)) { s.f = nullptr; }
    scoped(const scoped& s) = delete;
    ~scoped() {
        f();
    }
};

/**
 * Temporarily set the parser context to be not in a path expression.
 */
[[nodiscard]]
static auto scopedNotInPath(Parser& p) {
    auto inPath = false;
    std::swap(p.ctx.inPath, inPath);

    return scoped([&p, inPath]() {
        p.ctx.inPath = inPath;
    });
}

/**
 * Tries to evaluate the input expression on a stub context.
 * Returns the evaluated result on success, otherwise the original expression is returned.
 */
static auto simplifyOrForward(Environment* env, expected<ExprPtr, Error> expr) -> expected<ExprPtr, Error>
{
    if (!expr)
        return expr;
    if (!*expr)
        return nullptr;

    std::deque<Value> values;
    auto stub = Context(env, Context::Phase::Compilation);
    auto res = (*expr)->eval(stub, Value::undef(), LambdaResultFn([&, n = 0](Context ctx, Value&& vv) mutable {
        n += 1;
        if ((n <= MultiConstExpr::Limit) && (!vv.isa(ValueType::Undef) || vv.nodePtr())) {
            values.push_back(std::move(vv));
            return Result::Continue;
        }

        values.clear();
        return Result::Stop;
    }));
    TRY_EXPECTED(res);

    /* Warn about constant results */
    if (!values.empty() && std::ranges::all_of(values.begin(), values.end(), [](const Value& v) {
        return v.isa(ValueType::Null);
    }))
        env->warn("Expression is alway null"s, (*expr)->toString());

    if (!values.empty() && values[0].isa(ValueType::Bool) && std::ranges::all_of(values.begin(), values.end(), [&](const Value& v) {
        return v.isBool(values[0].as<ValueType::Bool>());
    }))
        env->warn("Expression is always "s + values[0].toString(), (*expr)->toString());

    if (values.size() == 1)
        return std::make_unique<ConstExpr>(std::move(values[0]));
    if (values.size() > 1)
        return std::make_unique<MultiConstExpr>(std::vector<Value>(std::make_move_iterator(values.begin()),
                                                                   std::make_move_iterator(values.end())));

    return expr;
}

AST::~AST() = default;

/**
 * Parser wrapper for parsing and & or operators.
 *
 * <expr> [and|or] <expr>
 */
class AndOrParser : public InfixParselet
{
public:
    auto parse(Parser& p, ExprPtr left, Token t) const -> expected<ExprPtr, Error> override
    {
        auto right = p.parsePrecedence(precedence());
        if (!right)
            return right;

        if (t.type == Token::OP_AND)
          return simplifyOrForward(p.env, std::make_unique<AndExpr>(std::move(left),
                                                                    std::move(*right)));
        else if (t.type == Token::OP_OR)
          return simplifyOrForward(p.env, std::make_unique<OrExpr>(std::move(left),
                                                                   std::move(*right)));
        assert(0);
        return nullptr;
    }

    int precedence() const override
    {
        return Precedence::LOGIC;
    }
};

class CompletionAndOrParser : public InfixParselet
{
public:
    explicit CompletionAndOrParser(const Completion* comp)
        : comp_(comp)
    {}

    auto parse(Parser& p, ExprPtr left, Token t) const -> expected<ExprPtr, Error> override
    {
        auto right = p.parsePrecedence(precedence());
        if (!right)
            return right;

        if (t.type == Token::OP_AND)
          return simplifyOrForward(p.env, std::make_unique<CompletionAndExpr>(std::move(left),
                                                                              std::move(*right), comp_));
        else if (t.type == Token::OP_OR)
          return simplifyOrForward(p.env, std::make_unique<CompletionOrExpr>(std::move(left),
                                                                             std::move(*right), comp_));
        assert(0);
        return nullptr;
    }

    int precedence() const override
    {
        return Precedence::LOGIC;
    }

    const Completion* comp_;
};

class CastParser : public InfixParselet
{
public:
    auto parse(Parser& p, ExprPtr left, Token t) const -> expected<ExprPtr, Error> override
    {
        auto type = p.consume();
        if (type.type == Token::C_NULL)
            return std::make_unique<ConstExpr>(Value::null());

        if (type.type != Token::Type::WORD)
            return unexpected<Error>(Error::InvalidType, fmt::format("'as' expected typename got {}", type.toString()));

        auto name = std::get<std::string>(type.value);
        return simplifyOrForward(p.env, [&]() -> expected<ExprPtr, Error> {
            if (name == strings::TypenameNull)
                return std::make_unique<ConstExpr>(Value::null());
            if (name == strings::TypenameBool)
                return std::make_unique<UnaryExpr<OperatorBool>>(std::move(left));
            if (name == strings::TypenameInt)
                return std::make_unique<UnaryExpr<OperatorAsInt>>(std::move(left));
            if (name == strings::TypenameFloat)
                return std::make_unique<UnaryExpr<OperatorAsFloat>>(std::move(left));
            if (name == strings::TypenameString)
                return std::make_unique<UnaryExpr<OperatorAsString>>(std::move(left));

            return unexpected<Error>(Error::InvalidType, fmt::format("Invalid type name for cast '{}'", name));
        }());
    }

    int precedence() const override
    {
        return Precedence::CAST;
    }
};

/**
 * Parser wrapper for parsing infix operators.
 *
 * <expr> OP <expr>
 */
template <class Operator,
          int Precedence>
class BinaryOpParser : public InfixParselet
{
public:
    auto parse(Parser& p, ExprPtr left, Token t) const -> expected<ExprPtr, Error> override
    {
        auto right = p.parsePrecedence(precedence());
        if (!right)
            return right;

        return simplifyOrForward(p.env, std::make_unique<BinaryExpr<Operator>>(t,
                                                                               std::move(left),
                                                                               std::move(*right)));
    }

    int precedence() const override
    {
        return Precedence;
    }
};

/**
 * Parser for unary operators.
 *
 * ('-' | '~' | 'not') <expr>
 */
template <class Operator>
class UnaryOpParser : public PrefixParselet
{
    auto parse(Parser& p, Token t) const -> expected<ExprPtr, Error> override
    {
        auto sub = p.parsePrecedence(Precedence::UNARY);
        if (!sub)
            return sub;

        return simplifyOrForward(p.env, std::make_unique<UnaryExpr<Operator>>(std::move(*sub)));
    }
};

/**
 * Parse postfix unary operator.
 */
template <class Operator>
class UnaryPostOpParser : public InfixParselet
{
    auto parse(Parser& p, ExprPtr left, Token t) const -> expected<ExprPtr, Error> override
    {
        return p.parseInfix(simplifyOrForward(p.env, std::make_unique<UnaryExpr<Operator>>(std::move(left))), 0);
    }

    auto precedence() const -> int override
    {
        return Precedence::POST_UNARY;
    }
};

/**
 * Parse unpack (...) operator.
 */
class UnpackOpParser : public InfixParselet
{
    auto parse(Parser& p, ExprPtr left, Token t) const -> expected<ExprPtr, Error> override
    {
        return p.parseInfix(simplifyOrForward(p.env, std::make_unique<UnpackExpr>(std::move(left))), 0);
    }

    auto precedence() const -> int override
    {
        return Precedence::POST_UNARY;
    }
};

/**
 * Parse any word as unary or binary-postfix operator
 */
class WordOpParser : public InfixParselet
{
    auto parse(Parser& p, ExprPtr left, Token t) const -> expected<ExprPtr, Error> override
    {
        /* Try parse as binary operator */
        auto right = p.parsePrecedence(precedence(), true);
        if (!right)
            return right;

        if (*right)
            return simplifyOrForward(p.env, std::make_unique<BinaryWordOpExpr>(std::get<std::string>(t.value),
                                                                               std::move(left),
                                                                               std::move(*right)));

        /* Parse as unary operator */
        return p.parseInfix(simplifyOrForward(p.env, std::make_unique<UnaryWordOpExpr>(std::get<std::string>(t.value),
                                                                                       std::move(left))), 0);
    }

    auto precedence() const -> int override
    {
        return Precedence::CUSTOM;
    }
};

/**
 * Parser for parsing scalars.
 *
 * <token>
 */
template <class Type>
class ScalarParser : public PrefixParselet
{
    auto parse(Parser& p, Token t) const -> expected<ExprPtr, Error> override
    {
        return std::make_unique<ConstExpr>(std::get<Type>(t.value));
    }
};

/**
 * Parser for parsing regular expression literals.
 *
 * <token>
 */
class RegExpParser : public PrefixParselet
{
    auto parse(Parser& p, Token t) const -> expected<ExprPtr, Error> override
    {
        auto value = ReType::Type.make(std::get<std::string>(t.value));
        return std::make_unique<ConstExpr>(std::move(value));
    }
};

/**
 * Parser emitting constant expressions.
 */
class ConstParser : public PrefixParselet
{
public:
    template <class ValueType>
    explicit ConstParser(ValueType value)
        : value_(Value::make(value))
    {}

    explicit ConstParser(Value value)
        : value_(std::move(value))
    {}

    auto parse(Parser& p, Token t) const -> expected<ExprPtr, Error> override
    {
        return std::make_unique<ConstExpr>(value_);
    }

    Value value_;
};

/**
 * Parser for parsing grouping parentheses.
 *
 * '(' <expr> ')'
 */
class ParenParser : public PrefixParselet
{
    auto parse(Parser& p, Token t) const -> expected<ExprPtr, Error> override
    {
        auto _ = scopedNotInPath(p);
        return p.parseTo(Token::RPAREN);
    }
};

/**
 * Parser for parsing subscripts.
 *
 * '[' <expr> ']'
 * <expr> '[' <expr> ']'
 */
class SubscriptParser : public PrefixParselet, public InfixParselet
{
    auto parse(Parser& p, Token t) const -> expected<ExprPtr, Error> override
    {
        auto _ = scopedNotInPath(p);
        auto body = p.parseTo(Token::RBRACK);
        if (!body)
            return body;

        return simplifyOrForward(p.env, std::make_unique<SubscriptExpr>(std::make_unique<FieldExpr>("_"),
                                                                        std::move(*body)));
    }

    auto parse(Parser& p, ExprPtr left, Token t) const -> expected<ExprPtr, Error> override
    {
        auto _ = scopedNotInPath(p);
        auto body = p.parseTo(Token::RBRACK);
        if (!body)
            return body;

        return simplifyOrForward(p.env, std::make_unique<SubscriptExpr>(std::move(left),
                                                                        std::move(*body)));
    }

    auto precedence() const -> int override
    {
        return Precedence::SUBSCRIPT;
    }
};

/**
 * Parser for parsing sub-expressions
 *
 * '{' <expr> '}'
 * <expr> '{' <expr> '}'
 */
class SubSelectParser : public PrefixParselet, public InfixParselet
{
    auto parse(Parser& p, Token t) const -> expected<ExprPtr, Error> override
    {
        auto _ = scopedNotInPath(p);
        /* Prefix sub-selects are transformed to a right side path expression,
         * with the current node on the left. As "standalone" sub-selects are not useful. */
        auto body = p.parseTo(Token::RBRACE);
        if (!body)
            return unexpected<Error>(std::move(body.error()));

        return simplifyOrForward(p.env, std::make_unique<SubExpr>(std::make_unique<FieldExpr>("_"),
                                                                  std::move(*body)));
    }

    auto parse(Parser& p, ExprPtr left, Token t) const -> expected<ExprPtr, Error> override
    {
        auto _ = scopedNotInPath(p);
        auto body = p.parseTo(Token::RBRACE);
        if (!body)
            return unexpected<Error>(std::move(body.error()));

        return simplifyOrForward(p.env, std::make_unique<SubExpr>(std::move(left),
                                                                  std::move(*body)));
    }

    auto precedence() const -> int override
    {
        return Precedence::SUBEXPR;
    }
};

/**
 * Parser for parsing words into either paths or function calls.
 *
 * <word> ['(' [<expr>] {',' <expr>} ')']
 */
class WordParser : public PrefixParselet
{
public:
    auto parse(Parser& p, Token t) const -> expected<ExprPtr, Error> override
    {
        /* Self */
        if (t.type == Token::SELF)
            return std::make_unique<FieldExpr>("_", t);

        /* Any Child */
        if (t.type == Token::OP_TIMES)
            return std::make_unique<AnyChildExpr>();

        /* Wildcard */
        if (t.type == Token::WILDCARD)
            return std::make_unique<WildcardExpr>();

        auto word = std::get<std::string>(t.value);

        /* Function call */
        if (p.match(Token::LPAREN)) {
            auto _ = scopedNotInPath(p);
            p.consume();

            auto arguments = p.parseList(Token::RPAREN);
            TRY_EXPECTED(arguments);

            if (word == "any") {
                return simplifyOrForward(p.env, std::make_unique<AnyExpr>(std::move(*arguments)));
            } else if (word == "each" || word == "all") {
                return simplifyOrForward(p.env, std::make_unique<EachExpr>(std::move(*arguments)));
            } else {
                return simplifyOrForward(p.env, std::make_unique<CallExpression>(word, std::move(*arguments)));
            }
        } else if (!p.ctx.inPath) {
            /* Parse Symbols (words in upper-case) */
            if (isSymbolWord(word)) {
                return std::make_unique<ConstExpr>(Value::make<std::string>(std::move(word)));
            }
            /* Constant */
            else if (auto constant = p.env->findConstant(word)) {
                return std::make_unique<ConstExpr>(*constant);
            }
        }

        /* Single field name */
        return std::make_unique<FieldExpr>(std::move(word), t);
    }
};

/**
 * Parser for word (field or function name) completion.
 */
class CompletionWordParser : public WordParser
{
public:
    explicit CompletionWordParser(Completion* comp)
        : comp_(comp)
    {}

    auto parse(Parser& p, Token t) const -> expected<ExprPtr, Error> override
    {
        /* Self */
        if (t.type == Token::SELF)
            return std::make_unique<FieldExpr>("_");

        /* Any Child */
        if (t.type == Token::OP_TIMES)
            return std::make_unique<AnyChildExpr>();

        /* Wildcard */
        if (t.type == Token::WILDCARD)
            return std::make_unique<WildcardExpr>();

        auto word = std::get<std::string>(t.value);

        /* Function call */
        if (p.match(Token::LPAREN)) {
            p.consume();

            /* Downcase function name */
            std::ranges::transform(word.begin(), word.end(), word.begin(), [](auto c) {
                return tolower(c);
            });

            auto arguments = p.parseList(Token::RPAREN);
            TRY_EXPECTED(arguments);

            return simplifyOrForward(p.env, std::make_unique<CallExpression>(word, std::move(*arguments)));
        } else if (!p.ctx.inPath) {
            /* Parse Symbols (words in upper-case) */
            if (isSymbolWord(word)) {
                if (t.containsPoint(comp_->point)) {
                    return std::make_unique<CompletionWordExpr>(word.substr(0, comp_->point - t.begin), comp_, t);
                }
                return std::make_unique<CompletionConstExpr>(Value::make<std::string>(std::move(word)));
            }
            /* Constant */
            else if (auto constant = p.env->findConstant(word)) {
                return std::make_unique<ConstExpr>(*constant);
            }
        }

        /* Single field name */
        if (t.containsPoint(comp_->point)) {
            return std::make_unique<CompletionFieldOrWordExpr>(word.substr(0, comp_->point - t.begin), comp_, t, p.ctx.inPath);
        }
        return std::make_unique<FieldExpr>(std::move(word));
    }

    Completion* comp_;
};
;

/**
 * Parser for parsing '.' separated paths.
 *
 * <expr> '.' <expr>
 */
class PathParser : public InfixParselet
{
public:
    auto parse(Parser& p, ExprPtr left, Token t) const -> expected<ExprPtr, Error> override
    {
        auto inPath = true;
        std::swap(p.ctx.inPath, inPath);

        scoped _([&p, inPath]() {
            p.ctx.inPath = inPath;
        });

        auto right = p.parsePrecedence(precedence());
        TRY_EXPECTED(right);

        return std::make_unique<PathExpr>(std::move(left), std::move(*right));
    }

    auto precedence() const -> int override
    {
        return Precedence::PATH;
    }
};

class CompletionPathParser : public PathParser
{
public:
    explicit CompletionPathParser(Completion* comp)
        : comp_(comp)
    {}

    auto parse(Parser& p, ExprPtr left, Token t) const -> expected<ExprPtr, Error> override
    {
        auto inPath = true;
        std::swap(p.ctx.inPath, inPath);

        scoped _([&p, inPath]() {
            p.ctx.inPath = inPath;
        });

        auto right = p.parsePrecedence(precedence(), t.containsPoint(comp_->point));
        if (!right)
            return right;

        if (!*right) {
            Token expectedWord(Token::WORD, "", t.end, t.end);
            right = std::make_unique<CompletionFieldOrWordExpr>("", comp_, expectedWord, p.ctx.inPath);
        }

        return std::make_unique<PathExpr>(std::move(left), std::move(*right));
    }

    Completion* comp_;
};

namespace
{
// Static stateles parselets re-used by all parser instances
const ScalarParser<int64_t> intParser;
const ScalarParser<double> floatParser;
const ScalarParser<std::string> stringParser;
const RegExpParser regexpParser;
const UnaryOpParser<OperatorNegate> negateParser;
const UnaryOpParser<OperatorBitInv> bitInvParser;
const UnaryOpParser<OperatorNot> notParser;
const UnaryOpParser<OperatorLen> lenParser;
const UnaryOpParser<OperatorTypeof> typeofParser;
const UnaryPostOpParser<OperatorBool> boolParser;
const BinaryOpParser<OperatorAdd, Precedence::TERM> addParser;
const BinaryOpParser<OperatorSub, Precedence::TERM> subParser;
const BinaryOpParser<OperatorMul, Precedence::PRODUCT> mulParser;
const BinaryOpParser<OperatorDiv, Precedence::PRODUCT> divParser;
const BinaryOpParser<OperatorMod, Precedence::PRODUCT> modParser;
const BinaryOpParser<OperatorBitAnd, Precedence::BITWISE> bitAndParser;
const BinaryOpParser<OperatorBitOr, Precedence::BITWISE> bitOrParser;
const BinaryOpParser<OperatorBitXor, Precedence::BITWISE> bitXorParser;
const BinaryOpParser<OperatorShl, Precedence::BITWISE> shlParser;
const BinaryOpParser<OperatorShr, Precedence::BITWISE> shrParser;
const BinaryOpParser<OperatorEq, Precedence::EQUALITY> eqParser;
const BinaryOpParser<OperatorNeq, Precedence::EQUALITY> neqParser;
const BinaryOpParser<OperatorLt, Precedence::EQUALITY> ltParser;
const BinaryOpParser<OperatorLtEq, Precedence::EQUALITY> lteqParser;
const BinaryOpParser<OperatorGt, Precedence::EQUALITY> gtParser;
const BinaryOpParser<OperatorGtEq, Precedence::EQUALITY> gteqParser;
const AndOrParser andOrParser;
const CastParser castParser;
const ParenParser parenParser;
const SubSelectParser subSelectParser;
const SubscriptParser subscriptParser;
const WordParser wordParser;
const PathParser pathParser;
const UnpackOpParser unpackParser;
const WordOpParser wordOpParser;
const ConstParser trueParser{Value::t()};
const ConstParser falseParser{Value::f()};
const ConstParser nullParser{Value::null()};
}

static auto setupParser(Parser& p)
{
    /* Scalars */
    p.prefixParsers[Token::C_TRUE]  = &trueParser;
    p.prefixParsers[Token::C_FALSE] = &falseParser;
    p.prefixParsers[Token::C_NULL]  = &nullParser;
    p.prefixParsers[Token::INT]     = &intParser;
    p.prefixParsers[Token::FLOAT]   = &floatParser;
    p.prefixParsers[Token::STRING]  = &stringParser;
    p.prefixParsers[Token::REGEXP]  = &regexpParser;

    /* Unary Operators */
    p.prefixParsers[Token::OP_SUB]    = &negateParser;
    p.prefixParsers[Token::OP_BITINV] = &bitInvParser;
    p.prefixParsers[Token::OP_NOT]    = &notParser;
    p.prefixParsers[Token::OP_LEN]    = &lenParser;
    p.infixParsers[Token::OP_BOOL]    = &boolParser;
    p.prefixParsers[Token::OP_TYPEOF] = &typeofParser;
    p.infixParsers[Token::OP_UNPACK]  = &unpackParser;
    p.infixParsers[Token::WORD]       = &wordOpParser;

    /* Binary Operators */
    p.infixParsers[Token::OP_ADD]   = &addParser;
    p.infixParsers[Token::OP_SUB]   = &subParser;
    p.infixParsers[Token::OP_TIMES] = &mulParser;
    p.infixParsers[Token::OP_DIV]   = &divParser;
    p.infixParsers[Token::OP_MOD]   = &modParser;

    /* Bit Operators */
    p.infixParsers[Token::OP_BITAND] = &bitAndParser;
    p.infixParsers[Token::OP_BITOR]  = &bitOrParser;
    p.infixParsers[Token::OP_BITXOR] = &bitXorParser;
    p.infixParsers[Token::OP_LSHIFT] = &shlParser;
    p.infixParsers[Token::OP_RSHIFT] = &shrParser;

    /* Comparison/Test */
    p.infixParsers[Token::OP_EQ]     = &eqParser;
    p.infixParsers[Token::OP_NOT_EQ] = &neqParser;
    p.infixParsers[Token::OP_LT]     = &ltParser;
    p.infixParsers[Token::OP_LTEQ]   = &lteqParser;
    p.infixParsers[Token::OP_GT]     = &gtParser;
    p.infixParsers[Token::OP_GTEQ]   = &gteqParser;
    p.infixParsers[Token::OP_AND]    = &andOrParser;
    p.infixParsers[Token::OP_OR]     = &andOrParser;

    /* Cast */
    p.infixParsers[Token::OP_CAST]   = &castParser;

    /* Subexpressions/Subscript */
    p.prefixParsers[Token::LPAREN] = &parenParser;     /* (...) */
    p.prefixParsers[Token::LBRACE] = &subSelectParser; /* {...} */
    p.infixParsers[Token::LBRACE] = &subSelectParser;
    p.prefixParsers[Token::LBRACK] = &subscriptParser; /* [...] */
    p.infixParsers[Token::LBRACK] = &subscriptParser;

    /* Ident/Function */
    p.prefixParsers[Token::WORD] = &wordParser;
    p.prefixParsers[Token::SELF] = &wordParser;

    /* Wildcards */
    p.prefixParsers[Token::WILDCARD] = &wordParser;
    p.prefixParsers[Token::OP_TIMES] = &wordParser;

    /* Paths */
    p.infixParsers[Token::DOT]  = &pathParser;
}

auto compile(Environment& env, std::string_view query, bool any, bool autoWildcard) -> expected<ASTPtr, Error>
{
    auto tokens = tokenize(query);
    TRY_EXPECTED(tokens);

    Parser p(&env, *tokens, Parser::Mode::Strict);
    setupParser(p);

    auto expr = [&]() -> expected<ExprPtr, Error> {
        auto root = p.parse();
        TRY_EXPECTED(root);

        /* Expand a single value to `** == <value>` */
        if (autoWildcard && *root && (*root)->constant()) {
            root = std::make_unique<BinaryExpr<OperatorEq>>(
                std::make_unique<WildcardExpr>(), std::move(*root));
        }

        if (!*root)
            return unexpected<Error>(Error::ParserError, "Expression is null");

        if (any) {
            std::vector<ExprPtr> args;
            args.emplace_back(std::move(*root));
            return simplifyOrForward(p.env, std::make_unique<AnyExpr>(std::move(args)));
        } else {
            return root;
        }
    }();
    TRY_EXPECTED(expr);

    if (!p.match(Token::Type::NIL))
        return unexpected<Error>(Error::ExpectedEOF, "Expected end-of-input; got "s + p.current().toString());

    return std::make_unique<AST>(std::string(query), std::move(*expr));
}

auto complete(Environment& env, std::string_view query, size_t point, const ModelNode& node, const CompletionOptions& options) -> expected<std::vector<CompletionCandidate>, Error>
{
    auto tokens = tokenize(query);
    TRY_EXPECTED(tokens);

    Parser p(&env, *tokens, Parser::Mode::Relaxed);
    setupParser(p);

    Completion comp(point, options);
    if (options.limit > 0)
        comp.limit = options.limit;

    CompletionWordParser wordCompletionParser(&comp);
    CompletionPathParser pathCompletionParser(&comp);
    CompletionAndOrParser andOrCompletionParser(&comp);
    p.prefixParsers[Token::WORD]  = &wordCompletionParser;
    p.infixParsers[Token::DOT]    = &pathCompletionParser;
    p.infixParsers[Token::OP_AND] = &andOrCompletionParser;
    p.infixParsers[Token::OP_OR]  = &andOrCompletionParser;

    auto astResult = p.parse();
    TRY_EXPECTED(astResult);
    auto ast = std::move(*astResult);

    // Determine which hints to show.
    auto showConstantWildcardHint = false;
    auto showFieldWildcardHint = false;
    auto showComparisonWildcardHint = false;
    if (options.showWildcardHints) {
        // Test the query for patterns and hint for converting it
        // to a wildcard query by prepending `**.` to the query.
        if (isSingleValueExpression(ast.get()))
            showConstantWildcardHint = true;

        if (isSingleValueOrFieldExpression(ast.get()))
            showFieldWildcardHint = true;

        if (isFieldComparison(ast.get()))
            showComparisonWildcardHint = true;
    }

    Context ctx(&env);
    if (options.timeoutMs > 0)
        ctx.timeout = std::chrono::steady_clock::now() + std::chrono::milliseconds(options.timeoutMs);

    ast->eval(ctx, Value::field(node), LambdaResultFn([](Context, const Value&) {
        return Result::Continue;
    }));

    auto candidates = std::vector<CompletionCandidate>(comp.candidates.begin(), comp.candidates.end());
    if (options.sorted)
        std::ranges::stable_sort(candidates, [](const auto& left, const auto& right) {
            return left.text < right.text;
        });

    // Show special hints for wildcard expansion.
    if (showFieldWildcardHint)
        candidates.emplace_back(fmt::format("**.{}", query),
                                SourceLocation(0, query.size()),
                                CompletionCandidate::Type::HINT,
                                fmt::format("Query field '{}' recursive", query));

    if (showConstantWildcardHint)
        candidates.emplace_back(fmt::format("** = {}", query),
                                SourceLocation(0, query.size()),
                                CompletionCandidate::Type::HINT,
                                fmt::format("Query fields matching '{}' recursive", query));

    if (showComparisonWildcardHint)
        candidates.emplace_back(fmt::format("**.{}", query),
                                SourceLocation(0, query.size()),
                                CompletionCandidate::Type::HINT,
                                "Expand to recursive query");

    return candidates;
}

auto eval(Environment& env, const AST& ast, const ModelNode& node, Diagnostics* diag) -> expected<std::vector<Value>, Error>
{
    if (!node.model_)
        return unexpected<Error>(Error::NullModel, "ModelNode must have a model!");

    Context ctx(&env);

    auto mutableAST = ast.expr().clone();

    std::vector<Value> values;
    auto res = mutableAST->eval(ctx, Value::field(node), LambdaResultFn([&values](Context, Value&& value) {
        values.push_back(std::move(value));
        return Result::Continue;
    }));
    TRY_EXPECTED(res);

    if (diag) {
        diag->collect(*mutableAST);
    }

    return values;
}

auto diagnostics(Environment& env, const AST& ast, const Diagnostics& diag) -> expected<std::vector<Diagnostics::Message>, Error>
{
    return diag.buildMessages(env, ast);
}

}
