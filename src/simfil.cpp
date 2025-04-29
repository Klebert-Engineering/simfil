#include "simfil/simfil.h"
#include "simfil/model/nodes.h"
#include "simfil/model/string-pool.h"
#include "simfil/token.h"
#include "simfil/operator.h"
#include "simfil/value.h"
#include "simfil/function.h"
#include "simfil/expression.h"
#include "simfil/parser.h"
#include "simfil/environment.h"
#include "simfil/model/model.h"
#include "simfil/types.h"
#include "fmt/core.h"

#include "expressions.h"
#include "levenshtein.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <deque>
#include <unordered_map>
#include <stdexcept>
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
 *
 */
template <class ...Type>
static auto expect(const ExprPtr& e, Type... types)
{
    const auto type2str = [](Expr::Type t) {
        switch (t) {
        case Expr::Type::FIELD:     return "field"s;
        case Expr::Type::PATH:      return "path"s;
        case Expr::Type::SUBEXPR:   return "subexpression"s;
        case Expr::Type::SUBSCRIPT: return "subscript"s;
        case Expr::Type::VALUE:     return "value"s;
        }
        return "error"s;
    };

    if (!e)
        raise<std::runtime_error>("Expected expression"s);

    if constexpr (sizeof...(types) >= 1) {
        Expr::Type list[] = {types...};
        for (auto i = 0; i < sizeof...(types); ++i) {
            if (e->type() == list[i])
                return;
        }

        std::string typeNames;
        for (auto i = 0; i < sizeof...(types); ++i) {
            if (!typeNames.empty())
                typeNames += " or ";
            typeNames += type2str(list[i]);
        }

        raise<std::runtime_error>("Expected "s + typeNames + " got "s + type2str(e->type()));
    }
}

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
struct scoped {
    std::function<void()> f;

    explicit scoped(std::function<void()> f) : f(std::move(f)) {}
    scoped(scoped&& s) noexcept : f(std::move(s.f)) { s.f = nullptr; }
    scoped(const scoped& s) = delete;
    ~scoped() {
        try { if (f) { f(); } } catch (...) {}
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
static auto simplifyOrForward(Environment* env, ExprPtr expr) -> ExprPtr
{
    if (!expr)
        return nullptr;

    std::deque<Value> values;
    auto stub = Context(env, Context::Phase::Compilation);
    (void)expr->eval(stub, Value::undef(), LambdaResultFn([&, n = 0](Context ctx, Value vv) mutable {
        if (!vv.isa(ValueType::Undef) || vv.node) {
            values.push_back(std::move(vv));
            return Result::Continue;
        }

        if (++n > MultiConstExpr::Limit) {
            values.clear();
            return Result::Stop;
        }

        values.clear();
        return Result::Stop;
    }));

    /* Warn about constant results */
    if (!values.empty() && std::ranges::all_of(values.begin(), values.end(), [](const Value& v) {
        return v.isa(ValueType::Null);
    }))
        env->warn("Expression is alway null"s, expr->toString());

    if (!values.empty() && values[0].isa(ValueType::Bool) && std::ranges::all_of(values.begin(), values.end(), [&](const Value& v) {
        return v.isBool(values[0].as<ValueType::Bool>());
    }))
        env->warn("Expression is always "s + values[0].toString(), expr->toString());

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
    auto parse(Parser& p, ExprPtr left, Token t) const -> ExprPtr override
    {
        auto right = p.parsePrecedence(precedence());

        if (t.type == Token::OP_AND)
          return simplifyOrForward(p.env, std::make_unique<AndExpr>(std::move(left),
                                                                    std::move(right)));
        else if (t.type == Token::OP_OR)
          return simplifyOrForward(p.env, std::make_unique<OrExpr>(std::move(left),
                                                                   std::move(right)));
        assert(0);
        return nullptr;
    }

    int precedence() const override
    {
        return Precedence::LOGIC;
    }
};

class CastParser : public InfixParselet
{
public:
    auto parse(Parser& p, ExprPtr left, Token t) const -> ExprPtr override
    {
        auto type = p.consume();
        if (type.type == Token::C_NULL)
            return std::make_unique<ConstExpr>(Value::null());

        if (type.type != Token::Type::WORD)
            raise<std::runtime_error>("'as' expected typename got "s + type.toString());

        auto name = std::get<std::string>(type.value);
        return simplifyOrForward(p.env, [&]() -> ExprPtr {
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

            raise<std::runtime_error>("Invalid type name for cast '"s + name + "'"s);
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
    auto parse(Parser& p, ExprPtr left, Token t) const -> ExprPtr override
    {
        auto right = p.parsePrecedence(precedence());
        return simplifyOrForward(p.env, std::make_unique<BinaryExpr<Operator>>(std::move(left),
                                                                               std::move(right)));
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
    auto parse(Parser& p, Token t) const -> ExprPtr override
    {
        auto sub = p.parsePrecedence(Precedence::UNARY);
        return simplifyOrForward(p.env, std::make_unique<UnaryExpr<Operator>>(std::move(sub)));
    }
};

/**
 * Parse postfix unary operator.
 */
template <class Operator>
class UnaryPostOpParser : public InfixParselet
{
    auto parse(Parser& p, ExprPtr left, Token t) const -> ExprPtr override
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
    auto parse(Parser& p, ExprPtr left, Token t) const -> ExprPtr override
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
    auto parse(Parser& p, ExprPtr left, Token t) const -> ExprPtr override
    {
        /* Try parse as binary operator */
        if (auto right = p.parsePrecedence(precedence(), true); right)
            return simplifyOrForward(p.env, std::make_unique<BinaryWordOpExpr>(std::get<std::string>(t.value),
                                                                               std::move(left),
                                                                               std::move(right)));
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
    auto parse(Parser& p, Token t) const -> ExprPtr override
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
    auto parse(Parser& p, Token t) const -> ExprPtr override
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

    auto parse(Parser& p, Token t) const -> ExprPtr override
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
    auto parse(Parser& p, Token t) const -> ExprPtr override
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
    auto parse(Parser& p, Token t) const -> ExprPtr override
    {
        auto _ = scopedNotInPath(p);
        return simplifyOrForward(p.env, std::make_unique<SubscriptExpr>(std::make_unique<FieldExpr>("_"),
                                                                        p.parseTo(Token::RBRACK)));
    }

    auto parse(Parser& p, ExprPtr left, Token t) const -> ExprPtr override
    {
        expect(left,
               Expr::Type::PATH,
               Expr::Type::VALUE,
               Expr::Type::SUBEXPR,
               Expr::Type::SUBSCRIPT);
        auto _ = scopedNotInPath(p);
        return simplifyOrForward(p.env, std::make_unique<SubscriptExpr>(std::move(left),
                                                                        p.parseTo(Token::RBRACK)));
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
    auto parse(Parser& p, Token t) const -> ExprPtr override
    {
        auto _ = scopedNotInPath(p);
        /* Prefix sub-selects are transformed to a right side path expression,
         * with the current node on the left. As "standalone" sub-selects are not useful. */
        return simplifyOrForward(p.env, std::make_unique<SubExpr>(std::make_unique<FieldExpr>("_"),
                                                                  p.parseTo(Token::RBRACE)));
    }

    auto parse(Parser& p, ExprPtr left, Token t) const -> ExprPtr override
    {
        expect(left,
               Expr::Type::PATH,
               Expr::Type::VALUE,
               Expr::Type::SUBEXPR,
               Expr::Type::SUBSCRIPT);
        auto _ = scopedNotInPath(p);
        return simplifyOrForward(p.env, std::make_unique<SubExpr>(std::move(left),
                                                                  p.parseTo(Token::RBRACE)));
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
    auto parse(Parser& p, Token t) const -> ExprPtr override
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
            return simplifyOrForward(p.env, std::make_unique<CallExpression>(word, std::move(arguments)));
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
 * Parser for parsing '.' separated paths.
 *
 * <expr> '.' <expr>
 */
class PathParser : public InfixParselet
{
    auto parse(Parser& p, ExprPtr left, Token t) const -> ExprPtr override
    {
        auto inPath = true;
        std::swap(p.ctx.inPath, inPath);

        scoped _([&p, inPath]() {
            p.ctx.inPath = inPath;
        });

        auto right = p.parsePrecedence(precedence());
        return std::make_unique<PathExpr>(std::move(left), std::move(right));
    }

    auto precedence() const -> int override
    {
        return Precedence::PATH;
    }
};

auto compile(Environment& env, std::string_view sv, bool any, bool autoWildcard) -> ASTPtr
{
    Parser p(&env, sv);

    /* Scalars */
    p.prefixParsers[Token::C_TRUE]  = std::make_unique<ConstParser>(Value::t());
    p.prefixParsers[Token::C_FALSE] = std::make_unique<ConstParser>(Value::f());
    p.prefixParsers[Token::C_NULL]  = std::make_unique<ConstParser>(Value::null());
    p.prefixParsers[Token::INT]     = std::make_unique<ScalarParser<int64_t>>();
    p.prefixParsers[Token::FLOAT]   = std::make_unique<ScalarParser<double>>();
    p.prefixParsers[Token::STRING]  = std::make_unique<ScalarParser<std::string>>();
    p.prefixParsers[Token::REGEXP]  = std::make_unique<RegExpParser>();

    /* Unary Operators */
    p.prefixParsers[Token::OP_SUB]    = std::make_unique<UnaryOpParser<OperatorNegate>>();
    p.prefixParsers[Token::OP_BITINV] = std::make_unique<UnaryOpParser<OperatorBitInv>>();
    p.prefixParsers[Token::OP_NOT]    = std::make_unique<UnaryOpParser<OperatorNot>>();
    p.prefixParsers[Token::OP_LEN]    = std::make_unique<UnaryOpParser<OperatorLen>>();
    p.infixParsers[Token::OP_BOOL]    = std::make_unique<UnaryPostOpParser<OperatorBool>>();
    p.prefixParsers[Token::OP_TYPEOF] = std::make_unique<UnaryOpParser<OperatorTypeof>>();
    p.infixParsers[Token::OP_UNPACK]  = std::make_unique<UnpackOpParser>();
    p.infixParsers[Token::WORD]       = std::make_unique<WordOpParser>();

    /* Binary Operators */
    p.infixParsers[Token::OP_ADD]   = std::make_unique<BinaryOpParser<OperatorAdd, Precedence::TERM>>();
    p.infixParsers[Token::OP_SUB]   = std::make_unique<BinaryOpParser<OperatorSub, Precedence::TERM>>();
    p.infixParsers[Token::OP_TIMES] = std::make_unique<BinaryOpParser<OperatorMul, Precedence::PRODUCT>>();
    p.infixParsers[Token::OP_DIV]   = std::make_unique<BinaryOpParser<OperatorDiv, Precedence::PRODUCT>>();
    p.infixParsers[Token::OP_MOD]   = std::make_unique<BinaryOpParser<OperatorMod, Precedence::PRODUCT>>();

    /* Bit Operators */
    p.infixParsers[Token::OP_BITAND] = std::make_unique<BinaryOpParser<OperatorBitAnd, Precedence::BITWISE>>();
    p.infixParsers[Token::OP_BITOR]  = std::make_unique<BinaryOpParser<OperatorBitOr, Precedence::BITWISE>>();
    p.infixParsers[Token::OP_BITXOR] = std::make_unique<BinaryOpParser<OperatorBitXor, Precedence::BITWISE>>();
    p.infixParsers[Token::OP_LSHIFT] = std::make_unique<BinaryOpParser<OperatorShl, Precedence::BITWISE>>();
    p.infixParsers[Token::OP_RSHIFT] = std::make_unique<BinaryOpParser<OperatorShr, Precedence::BITWISE>>();

    /* Comparison/Test */
    p.infixParsers[Token::OP_EQ]     = std::make_unique<BinaryOpParser<OperatorEq,   Precedence::EQUALITY>>();
    p.infixParsers[Token::OP_NOT_EQ] = std::make_unique<BinaryOpParser<OperatorNeq,  Precedence::EQUALITY>>();
    p.infixParsers[Token::OP_LT]     = std::make_unique<BinaryOpParser<OperatorLt,   Precedence::EQUALITY>>();
    p.infixParsers[Token::OP_LTEQ]   = std::make_unique<BinaryOpParser<OperatorLtEq, Precedence::EQUALITY>>();
    p.infixParsers[Token::OP_GT]     = std::make_unique<BinaryOpParser<OperatorGt,   Precedence::EQUALITY>>();
    p.infixParsers[Token::OP_GTEQ]   = std::make_unique<BinaryOpParser<OperatorGtEq, Precedence::EQUALITY>>();
    p.infixParsers[Token::OP_AND]    = std::make_unique<AndOrParser>();
    p.infixParsers[Token::OP_OR]     = std::make_unique<AndOrParser>();

    /* Cast */
    p.infixParsers[Token::OP_CAST]   = std::make_unique<CastParser>();

    /* Subexpressions/Subscript */
    p.prefixParsers[Token::LPAREN] = std::make_unique<ParenParser>();     /* (...) */
    p.prefixParsers[Token::LBRACE] = std::make_unique<SubSelectParser>(); /* {...} */
    p.infixParsers[Token::LBRACE] = std::make_unique<SubSelectParser>();
    p.prefixParsers[Token::LBRACK] = std::make_unique<SubscriptParser>(); /* [...] */
    p.infixParsers[Token::LBRACK] = std::make_unique<SubscriptParser>();

    /* Ident/Function */
    p.prefixParsers[Token::WORD] = std::make_unique<WordParser>();
    p.prefixParsers[Token::SELF] = std::make_unique<WordParser>();

    /* Wildcards */
    p.prefixParsers[Token::WILDCARD] = std::make_unique<WordParser>();
    p.prefixParsers[Token::OP_TIMES] = std::make_unique<WordParser>();

    /* Paths */
    p.infixParsers[Token::DOT]  = std::make_unique<PathParser>();

    auto expr = [&](){
        auto root = p.parse();

        /* Expand a single value to `** == <value>` */
        if (autoWildcard && root && root->constant()) {
            root = std::make_unique<BinaryExpr<OperatorEq>>(
                std::make_unique<WildcardExpr>(), std::move(root));
        }

        if (any) {
            std::vector<ExprPtr> args;
            args.emplace_back(std::move(root));
            return simplifyOrForward(p.env, std::make_unique<CallExpression>("any"s, std::move(args)));
        } else {
            return root;
        }
    }();

    if (!p.match(Token::Type::NIL))
        raise<std::runtime_error>("Expected end-of-input; got "s + p.current().toString());

    return std::make_unique<AST>(std::string(sv), std::move(expr));
}

auto eval(Environment& env, const AST& ast, const ModelNode& node, Diagnostics* diagnostics) -> std::vector<Value>
{
    if (!node.model_)
        raise<std::runtime_error>("ModelNode must have a model!");

    Context ctx(&env);

    auto mutableAST = ast.expr().clone();

    std::vector<Value> res;
    mutableAST->eval(ctx, Value::field(node), LambdaResultFn([&res](Context ctx, Value vv) {
        res.push_back(std::move(vv));
        return Result::Continue;
    }));

    if (diagnostics) {
        /* NOTE: Make sure everything here is thread-safe! Preinitialize maps in the Diagnostics ctor etc.
         *       Try to be lock-free, as locking here can hurt performance.
         */
        struct MergeDiagnostics : ExprVisitor
        {
            Diagnostics& diagnostics;

            explicit MergeDiagnostics(Diagnostics& diag)
                : diagnostics(diag)
            {}

            auto visit(FieldExpr& e) -> void override {
                ExprVisitor::visit(e);
                diagnostics.fieldHits[index()] += e.hits_;
            }
        };

        MergeDiagnostics visitor(*diagnostics);
        mutableAST->accept(visitor);
    }

    return res;
}

static auto findSimilarString(std::string_view source, const StringPool& pool) -> std::string
{
    std::string_view best;
    auto bestScore = std::numeric_limits<int>::max();

    const auto isDollar = source[0] == '$';
    for (const auto& target : pool.strings()) {
        const auto targetIsDollar = target[0] == '$';
        if (isDollar != targetIsDollar)
            continue;
        if (target == source)
            continue;

        const auto score = levenshtein(source, target);
        if (score < bestScore) {
            bestScore = score;
            best = target;
        }
    }

    return std::string(best);
}

auto diagnostics(Environment& env, const AST& ast, const Diagnostics& diag) -> std::vector<Diagnostics::Message>
{
    struct Visitor : ExprVisitor
    {
        const AST& ast;
        const Environment& env;
        const Diagnostics& diagnonstics;
        std::vector<Diagnostics::Message> messages;

        Visitor(const AST& ast, const Environment& env, const Diagnostics& diagnonstics)
            : ast(ast)
            , env(env)
            , diagnonstics(diagnonstics)
        {}

        void visit(FieldExpr& e) override
        {
            ExprVisitor::visit(e);

            // Generate "did you mean ...?" messages for missing fields
            if (auto iter = diagnonstics.fieldHits.find(index()); iter != diagnonstics.fieldHits.end() && iter->second == 0) {
                auto guess = findSimilarString(e.name_, *env.strings());
                if (!guess.empty()) {
                    std::string fix = ast.query();
                    if (auto loc = e.sourceLocation(); loc.size > 0)
                        fix.replace(loc.begin, loc.size, guess);

                    addMessage(fmt::format("No matches for field '{}'. Did you mean '{}'?", e.name_, guess), e, fix);
                } else {
                    addMessage(fmt::format("No matches for field '{}'.", e.name_, guess), e, {});
                }
            }
        }

        void addMessage(std::string text, const Expr& e, std::optional<std::string> fix)
        {
            Diagnostics::Message msg;
            msg.message = std::move(text);
            msg.location = e.sourceLocation();
            msg.fix = std::move(fix);

            messages.push_back(std::move(msg));
        }
    };

    Visitor visitor(ast, env, diag);
    ast.expr().accept(visitor);

    return visitor.messages;
}

Diagnostics::Diagnostics() = default;

Diagnostics::Diagnostics(const AST& ast)
{
    struct InitDiagnostics : ExprVisitor
    {
        Diagnostics& self;

        explicit InitDiagnostics(Diagnostics& self)
            : self(self)
        {}

        auto visit(FieldExpr& e) -> void override
        {
            ExprVisitor::visit(e);
            self.fieldHits[index()] = 0;
        }
    };

    InitDiagnostics visitor(*this);
    ast.expr().accept(visitor);
}

Diagnostics& Diagnostics::append(const Diagnostics& other)
{
    for (const auto& [key, value] : other.fieldHits) {
        fieldHits[key] += value;
    }
    return *this;
}

}
