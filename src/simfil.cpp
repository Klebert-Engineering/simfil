#include "simfil/simfil.h"
#include "simfil/token.h"
#include "simfil/operator.h"
#include "simfil/value.h"
#include "simfil/function.h"
#include "simfil/expression.h"
#include "simfil/parser.h"
#include "simfil/environment.h"
#include "stx/format.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <deque>
#include <unordered_map>
#include <ostream>
#include <iostream>
#include <stdexcept>
#include <cassert>
#include <functional>
#include <vector>
#include <deque>


namespace simfil
{

using namespace std::string_literals;

namespace strings
{
static const std::string_view TypenameNull("null");
static const std::string_view TypenameBool("bool");
static const std::string_view TypenameInt("int");
static const std::string_view TypenameFloat("float");
static const std::string_view TypenameString("string");
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
    EQUALITY    = 2,  // ==, !=, =~, !~
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
        throw std::runtime_error("Expected expression"s);

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

        throw std::runtime_error("Expected "s + typeNames + " got "s + type2str(e->type()));
    }
}

/**
 * Helper for calling the result function if it has never been executed
 * at the time of destruction.
 */
struct CountedResultFn
{
    size_t calls = 0;
    bool finished = false;
    ResultFn fn;
    Context nonctx;

    CountedResultFn(ResultFn fn, Context ctx)
        : fn(std::move(fn))
        , nonctx(std::move(ctx))
    {}

    CountedResultFn(const CountedResultFn&) = delete;
    CountedResultFn(CountedResultFn&&) = delete;

    auto operator()(Context ctx, Value vv) -> Result
    {
        assert(!finished);
        ++calls;
        return fn(std::move(ctx), std::move(vv));
    }

    /* NOTE: You _must_ call finish before destruction! */
    auto ensureCall()
    {
        assert(!finished);
        if (calls == 0 && !finished) {
            finished = true;
            if (nonctx.phase == Context::Phase::Compilation)
                fn(nonctx, Value::undef());
            else
                fn(nonctx, Value::null());
        }
    }
};

class WildcardExpr : public Expr
{
public:
    WildcardExpr()
    {}

    auto type() const -> Type override
    {
        return Type::PATH;
    }

    auto ieval(Context ctx, Value val, ResultFn ores) const -> Result override
    {
        if (ctx.phase == Context::Phase::Compilation)
            return ores(ctx, Value::undef());

        auto res = CountedResultFn(std::move(ores), ctx);

        std::function<Result(Value, int)> iterate = [&iterate, &res, &ctx](Value val, int depth) {
            if (!val.node)
                return Result::Continue;

            if (res(ctx, val) == Result::Stop)
                return Result::Stop;

            for (const auto& sub : val.node->children()) {
                if (iterate(Value::field(sub->value(), sub), depth + 1) == Result::Stop)
                    return Result::Stop;
            }

            return Result::Continue;
        };

        auto r = iterate(val, 0);
        res.ensureCall();
        return r;
    }

    auto toString() const -> std::string override
    {
        return "**";
    }
};

/**
 * Returns every child of the current node or null.
 */
class AnyChildExpr : public Expr
{
public:
    AnyChildExpr()
    {}

    auto type() const -> Type override
    {
        return Type::PATH;
    }

    auto ieval(Context ctx, Value val, ResultFn res) const -> Result override
    {
        if (ctx.phase == Context::Phase::Compilation)
            return res(ctx, Value::undef());

        if (!val.node)
            return res(ctx, Value::null());

        if (val.node->size() > 0) {
            for (const auto& sub : val.node->children()) {
                if (res(ctx, Value::field(sub->value(), sub)) == Result::Stop)
                    return Result::Stop;
            }
        } else {
            return res(ctx, Value::null());
        }

        return Result::Continue;
    }

    auto toString() const -> std::string override
    {
        return "*";
    }
};

class FieldExpr : public Expr
{
public:
    FieldExpr(std::string name)
        : name_(std::move(name))
    {}

    auto type() const -> Type override
    {
        return Type::PATH;
    }

    auto ieval(Context ctx, Value val, ResultFn res) const -> Result override
    {
        if (val.isa(ValueType::Undef))
            return res(ctx, std::move(val));

        /* Special case: _ points to the current node */
        if (name_ == "_")
            return res(ctx, val);

        if (!val.node)
            return res(ctx, Value::null());

        auto nameStringId = ctx.env->stringCache()->get(name_);
        if (!nameStringId)
            /* If the field name is not in the string cache, then there
               is no field with that name. */
            return res(ctx, Value::null());

        /* Enter sub-node */
        if (auto sub = val.node->get(nameStringId)) {
            return res(ctx, Value::field(sub->value(), sub));
        }

        if (ctx.phase == Context::Phase::Compilation)
            return res(ctx, Value::undef());
        return res(ctx, Value::null());
    }

    auto toString() const -> std::string override
    {
        return name_;
    }

    std::string name_;
};

class MultiConstExpr : public Expr
{
public:
    static constexpr size_t Limit = 10000;

    MultiConstExpr(std::vector<Value>&& vec)
        : values_(std::move(vec))
    {}

    auto type() const -> Type override
    {
        return Type::VALUE;
    }

    auto ieval(Context ctx, Value, ResultFn res) const -> Result override
    {
        for (const auto& v : values_) {
            if (res(ctx, v) == Result::Stop)
                return Result::Stop;
        }

        return Result::Continue;
    }

    auto toString() const -> std::string override
    {
        auto list = ""s;
        for (const auto& v : values_) {
            if (!list.empty())
                list += " ";
            list += v.toString();
        }

        return "{"s + list + "}"s;
    }

protected:
    const std::vector<Value> values_;
};

class ConstExpr : public Expr
{
public:
    template <class _CType>
    explicit ConstExpr(_CType&& value)
        : value_(Value::make(std::forward<_CType>(value)))
    {}

    explicit ConstExpr(Value value)
        : value_(std::move(value))
    {}

    auto type() const -> Type override
    {
        return Type::VALUE;
    }

    auto ieval(Context ctx, Value, ResultFn res) const -> Result override
    {
        return res(ctx, value_);
    }

    auto toString() const -> std::string override
    {
        if (value_.isa(ValueType::String))
            return "\""s + value_.toString() + "\""s;
        return value_.toString();
    }

protected:
    const Value value_;
};

class SubscriptExpr : public Expr
{
public:
    SubscriptExpr(ExprPtr left, ExprPtr index)
        : left_(std::move(left))
        , index_(std::move(index))
    {}

    auto type() const -> Type override
    {
        return Type::SUBSCRIPT;
    }

    auto ieval(Context ctx, Value val, ResultFn ores) const -> Result override
    {
        auto res = CountedResultFn(std::move(ores), ctx);

        auto r = left_->eval(ctx, val, [this, &val, &res](auto ctx, Value lval) {
            return index_->eval(ctx, val, [this, &res, &lval](auto ctx, Value ival) {
                /* Field subscript */
                if (lval.node) {
                    ModelNodePtr node;

                    /* Array subscript */
                    if (ival.isa(ValueType::Int)) {
                        auto index = ival.as<ValueType::Int>();
                        node = lval.node->at(index);
                    }
                    /* String subscript */
                    else if (ival.isa(ValueType::String)) {
                        auto key = ival.as<ValueType::String>();
                        if (auto keyStrId = ctx.env->stringCache()->get(key))
                            node = lval.node->get(keyStrId);
                    }

                    if (node)
                        return res(ctx, Value::field(node->value(), node));
                    else
                        ctx.env->warn("Invalid subscript index type "s + valueType2String(ival.type), this->toString());
                } else {
                    return res(ctx, BinaryOperatorDispatcher<OperatorSubscript>::dispatch(lval, ival));
                }

                return Result::Continue;
            });
        });
        res.ensureCall();
        return r;
    }

    auto toString() const -> std::string override
    {
        return "(index "s + left_->toString() + " "s + index_->toString() + ")"s;
    }

    ExprPtr left_;
    ExprPtr index_;
};

class SubExpr : public Expr
{
public:
    explicit SubExpr(ExprPtr sub)
        : left_(std::make_unique<FieldExpr>("_"))
        , sub_(std::move(sub))
    {}

    SubExpr(ExprPtr left, ExprPtr sub)
        : left_(std::move(left))
        , sub_(std::move(sub))
    {}

    auto type() const -> Type override
    {
        return Type::SUBEXPR;
    }

    auto ieval(Context ctx, Value val, ResultFn ores) const -> Result override
    {
        /* Do not return null unless we have _no_ matching value. */
        auto res = CountedResultFn(std::move(ores), ctx);

        auto r = left_->eval(ctx, val, [this, &res](auto ctx, auto lv) {
            return sub_->eval(ctx, lv, [this, &res, &lv](auto ctx, auto vv) {
                auto bv = UnaryOperatorDispatcher<OperatorBool>::dispatch(vv);
                if (bv.isa(ValueType::Undef))
                    return Result::Continue;

                if (bv.isa(ValueType::Bool) && bv.template as<ValueType::Bool>())
                    return res(ctx, lv);

                return Result::Continue;
            });
        });
        res.ensureCall();
        return r;
    }

    auto toString() const -> std::string override
    {
        return "(sub "s + left_->toString() + " "s + sub_->toString() + ")"s;
    }

    ExprPtr left_, sub_;
};

class CallExpression : public Expr
{
public:
    CallExpression(std::string name, std::vector<ExprPtr> args)
        : name_(std::move(name))
        , args_(std::move(args))
        , fn_(nullptr)
    {}

    auto type() const -> Type override
    {
        return Type::VALUE;
    }

    auto ieval(Context ctx, Value val, ResultFn res) const -> Result override
    {
        if (!fn_)
            fn_ = ctx.env->findFunction(name_);
        if (!fn_)
            throw std::runtime_error("Unknown function "s + name_);

        auto anyval = false;
        auto result = fn_->eval(ctx, val, args_, [&res, &anyval](auto ctx, auto vv) {
            anyval = true;
            return res(ctx, std::move(vv));
        });

        if (!anyval)
            return res(ctx, Value::null()); /* Expressions _must_ return at least one value! */
        return result;
    }

    auto toString() const -> std::string override
    {
        if (args_.empty())
            return "("s + name_ + ")"s;

        std::string s = "("s + name_;
        for (const auto& arg : args_) {
            s += " "s + arg->toString();
        }
        return s + ")"s;
    }

    std::string name_;
    std::vector<ExprPtr> args_;
    mutable const Function* fn_;
};

class PathExpr : public Expr
{
public:
    PathExpr(ExprPtr right)
        : left_(std::make_unique<FieldExpr>("_"))
        , right_(std::move(right))
    {}

    PathExpr(ExprPtr left, ExprPtr right)
        : left_(std::move(left))
        , right_(std::move(right))
    {
        assert(left_.get());
        assert(right_.get());
    }

    auto type() const -> Type override
    {
        return Type::PATH;
    }

    auto ieval(Context ctx, Value val, ResultFn ores) const -> Result override
    {
        auto res = CountedResultFn(std::move(ores), ctx);

        auto r = left_->eval(ctx, val, [this, &res](auto ctx, auto v) {
            if (v.isa(ValueType::Undef))
                return Result::Continue;

            if (v.isa(ValueType::Null) && !v.node)
                return Result::Continue;

            return right_->eval(ctx, std::move(v), [this, &res](auto ctx, auto vv) {
                if (vv.isa(ValueType::Undef))
                    return Result::Continue;

                if (vv.isa(ValueType::Null) && !vv.node)
                    return Result::Continue;

                return res(ctx, std::move(vv));
            });
        });
        res.ensureCall();
        return r;
    };

    auto toString() const -> std::string override
    {
        return "(. "s + left_->toString() + " "s + right_->toString() + ")"s;
    }

    ExprPtr left_, right_;
};

/**
 * Exists is a unary operator that requires context access and is
 * therefore implemented as its own expression.
 */
class ExistsExpr : public Expr
{
public:
    ExistsExpr(ExprPtr sub)
        : sub_(std::move(sub))
    {}

    auto type() const -> Type override
    {
        return Type::VALUE;
    }

    auto ieval(Context ctx, Value val, ResultFn res) const -> Result override
    {
        if (ctx.phase != Context::Phase::Evaluation)
            return res(ctx, Value::undef());

        if (!val.node)
            return res(ctx, Value::f());

        return sub_->eval(ctx, val, [&res](auto ctx, auto v) {
            return res(ctx, Value(ValueType::Bool, v.node != nullptr));
        });
    }

    auto toString() const -> std::string override
    {
        return "(exists "s + sub_->toString() + ")"s;
    }

    ExprPtr sub_;
};

/** Calls `unpack` onto values of type Object. Forwards the value(s) otherwise.
 *
 * 1... => 1
 * range(1, 10)... => 1,2,3,4,5,6,7,8,9,10
 */
class UnpackExpr : public Expr
{
public:
    UnpackExpr(ExprPtr sub)
        : sub_(std::move(sub))
    {}

    auto type() const -> Type override
    {
        return Type::VALUE;
    }

    auto ieval(Context ctx, Value val, ResultFn res) const -> Result override
    {
        auto anyval = false;
        auto r = sub_->eval(ctx, val, [&res, &anyval](auto ctx, Value v) {
            if (v.isa(ValueType::Object)) {
                const auto& obj = v.as<ValueType::Object>();
                auto r = Result::Continue;
                obj.meta->unpack(obj, [&](Value vv) {
                    anyval = true;
                    return res(ctx, std::move(vv)) == Result::Continue;
                });

                if (r == Result::Stop)
                    return Result::Stop;
            } else {
                anyval = true;
                if (res(ctx, std::move(v)) == Result::Stop)
                    return Result::Stop;
            }
            return Result::Continue;
        });

        if (!anyval)
            r = res(ctx, Value::null());
        return r;
    }

    auto toString() const -> std::string override
    {
        return "(... "s + sub_->toString() + ")"s;
    }

    ExprPtr sub_;
};

/**
 * Generic unary operator expression.
 */
template <class _Operator>
class UnaryExpr : public Expr
{
public:
    UnaryExpr(ExprPtr sub)
        : sub_(std::move(sub))
    {}

    auto type() const -> Type override
    {
        return Type::VALUE;
    }

    auto ieval(Context ctx, Value val, ResultFn res) const -> Result override
    {
        return sub_->eval(ctx, val, [&](auto ctx, auto vv) {
            return res(ctx, UnaryOperatorDispatcher<_Operator>::dispatch(std::move(vv)));
        });
    }

    auto toString() const -> std::string override
    {
        return "("s + _Operator::name() + " "s + sub_->toString() + ")"s;
    }

    ExprPtr sub_;
};

/**
 * Generic binary operator expression.
 */
template <class _Operator>
class BinaryExpr : public Expr
{
public:
    BinaryExpr(ExprPtr left, ExprPtr right)
        : left_(std::move(left))
        , right_(std::move(right))
    {
        assert(left_.get());
        assert(right_.get());
    }

    auto type() const -> Type override
    {
        return Type::VALUE;
    }

    auto ieval(Context ctx, Value val, ResultFn res) const -> Result override
    {
        return left_->eval(ctx, val, [this, &res, &val](auto ctx, auto lv) {
            return right_->eval(ctx, val, [this, &res, &lv](auto ctx, auto rv) {
                return res(ctx, BinaryOperatorDispatcher<_Operator>::dispatch(std::move(lv),
                                                                              std::move(rv)));
            });
        });
    }

    auto toString() const -> std::string override
    {
        return "("s + _Operator::name() + " "s + left_->toString() + " "s + right_->toString() + ")"s;
    }

    ExprPtr left_, right_;
};

class UnaryWordOpExpr : public Expr
{
public:
    UnaryWordOpExpr(Token t, ExprPtr left)
        : ident_(std::get<std::string>(t.value))
        , left_(std::move(left))
    {}

    auto type() const -> Type override
    {
        return Type::VALUE;
    }

    auto ieval(Context ctx, Value val, ResultFn res) const -> Result override
    {
        return left_->eval(ctx, val, [this, &res](auto ctx, Value val) {
            if (val.isa(ValueType::Undef))
                return res(ctx, std::move(val));

            if (val.isa(ValueType::Object)) {
                const auto& obj = val.as<ValueType::Object>();
                return res(ctx, obj.meta->unaryOp(ident_, obj));
            }

            throw std::runtime_error(stx::format("Invalid operator '{}' for value of type {}",
                                                 ident_, valueType2String(val.type)));
        });
    }

    auto toString() const -> std::string override
    {
        return "("s + ident_ + " "s + left_->toString() + ")"s;
    }

    std::string ident_;
    ExprPtr left_;
};

class BinaryWordOpExpr : public Expr
{
public:
    BinaryWordOpExpr(Token t, ExprPtr left, ExprPtr right)
        : ident_(std::get<std::string>(t.value))
        , left_(std::move(left))
        , right_(std::move(right))
    {}

    auto type() const -> Type override
    {
        return Type::VALUE;
    }

    auto ieval(Context ctx, Value val, ResultFn res) const -> Result override
    {
        return left_->eval(ctx, val, [this, &res, &val](auto ctx, auto lval) {
            return right_->eval(ctx, val, [this, &res, &lval](auto ctx, auto rval) {
                if (lval.isa(ValueType::Undef) || rval.isa(ValueType::Undef))
                    return res(ctx, Value::undef());

                if (lval.isa(ValueType::Object)) {
                    const auto& obj = lval.template as<ValueType::Object>();
                    return res(ctx, obj.meta->binaryOp(ident_, obj, rval));
                }

                if (rval.isa(ValueType::Object)) {
                    const auto& obj = rval.template as<ValueType::Object>();
                    return res(ctx, obj.meta->binaryOp(ident_, lval, obj));
                }

                throw std::runtime_error(stx::format("Invalid operator '{}' for values of type {} and {}",
                                                     ident_, valueType2String(lval.type), valueType2String(rval.type)));
            });
        });
    }

    auto toString() const -> std::string override
    {
        return "("s + ident_ + " "s + left_->toString() + " "s + right_->toString() + ")"s;
    }

    std::string ident_;
    ExprPtr left_, right_;
};

class AndExpr : public Expr
{
public:
    AndExpr(ExprPtr left, ExprPtr right)
        : left_(std::move(left))
        , right_(std::move(right))
    {
        assert(left_.get());
        assert(right_.get());
    }

    auto type() const -> Type override
    {
        return Type::VALUE;
    }

    auto ieval(Context ctx, Value val, ResultFn res) const -> Result override
    {
        /* Operator and behaves like in lua:
         * 'a and b' returns a if 'not a?' else b is returned */
        return left_->eval(ctx, val, [this, &res, &val](auto ctx, auto lval) {
            if (lval.isa(ValueType::Undef))
                return res(ctx, lval);

            if (auto v = UnaryOperatorDispatcher<OperatorBool>::dispatch(lval); v.isa(ValueType::Bool))
                if (!v.template as<ValueType::Bool>())
                    return res(ctx, std::move(lval));

            return right_->eval(ctx, val, [&res](auto ctx, auto rval) {
                return res(ctx, std::move(rval));
            });
        });
    }

    auto toString() const -> std::string override
    {
        return "(and "s + left_->toString() + " "s + right_->toString() + ")"s;
    }

    ExprPtr left_, right_;
};

class OrExpr : public Expr
{
public:
    OrExpr(ExprPtr left, ExprPtr right)
        : left_(std::move(left))
        , right_(std::move(right))
    {
        assert(left_.get());
        assert(right_.get());
    }

    auto type() const -> Type override
    {
        return Type::VALUE;
    }

    auto ieval(Context ctx, Value val, ResultFn res) const -> Result override
    {
        /* Operator or behaves like in lua:
         * 'a or b' returns a if 'a?' else b is returned */
        return left_->eval(ctx, val, [this, &res, &val](auto ctx, auto lval) {
            if (lval.isa(ValueType::Undef))
                return res(ctx, lval);

            if (auto v = UnaryOperatorDispatcher<OperatorBool>::dispatch(lval); v.isa(ValueType::Bool))
                if (v.template as<ValueType::Bool>())
                    return res(ctx, std::move(lval));

            return right_->eval(ctx, val, [&](auto ctx, auto rval) {
                return res(ctx, std::move(rval));
            });
        });

    }

    auto toString() const -> std::string override
    {
        return "(or "s + left_->toString() + " "s + right_->toString() + ")"s;
    }

    ExprPtr left_, right_;
};

/**
 * Tries to evaluate the input expression on a stub context.
 * Returns the evaluated result on success, erwise the original expression is returned.
 */
static auto simplifyOrForward(Environment* env, ExprPtr expr) -> ExprPtr
{
    if (!expr)
        return nullptr;

    std::deque<Value> values;
    auto stub = Context(env, Context::Phase::Compilation);
    (void)expr->eval(stub, Value::undef(), [&, n = 0](auto ctx, auto vv) mutable {
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
    });

    /* Warn about constant results */
    if (!values.empty() && std::all_of(values.begin(), values.end(), [](const Value& v) {
        return v.isa(ValueType::Null);
    }))
        env->warn("Expression is alway null"s, expr->toString());

    if (!values.empty() && values[0].isa(ValueType::Bool) && std::all_of(values.begin(), values.end(), [&](const Value& v) {
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
            throw std::runtime_error("'as' expected typename got "s + type.toString());

        auto name = std::get<std::string>(type.value);
        return simplifyOrForward(p.env, [&]() -> ExprPtr {
            if (name == strings::TypenameBool)
                return std::make_unique<UnaryExpr<OperatorBool>>(std::move(left));
            if (name == strings::TypenameInt)
                return std::make_unique<UnaryExpr<OperatorAsInt>>(std::move(left));
            if (name == strings::TypenameFloat)
                return std::make_unique<UnaryExpr<OperatorAsFloat>>(std::move(left));
            if (name == strings::TypenameString)
                return std::make_unique<UnaryExpr<OperatorAsString>>(std::move(left));

            throw std::runtime_error("Invalid type name for cast '"s + name + "'"s);
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
template <class _Operator,
          int _Precedence>
class BinaryOpParser : public InfixParselet
{
public:
    auto parse(Parser& p, ExprPtr left, Token t) const -> ExprPtr override
    {
        auto right = p.parsePrecedence(precedence());
        return simplifyOrForward(p.env, std::make_unique<BinaryExpr<_Operator>>(std::move(left),
                                                                                std::move(right)));
    }

    int precedence() const override
    {
        return _Precedence;
    }
};

/**
 * Parser for unary operators.
 *
 * ('-' | '~' | 'not') <expr>
 */
template <class _Operator>
class UnaryOpParser : public PrefixParselet
{
    auto parse(Parser& p, Token t) const -> ExprPtr override
    {
        auto sub = p.parsePrecedence(Precedence::UNARY);
        return simplifyOrForward(p.env, std::make_unique<UnaryExpr<_Operator>>(std::move(sub)));
    }
};

/**
 * Parse postfix unary operator.
 */
template <class _Operator>
class UnaryPostOpParser : public InfixParselet
{
    auto parse(Parser& p, ExprPtr left, Token t) const -> ExprPtr override
    {
        return p.parseInfix(simplifyOrForward(p.env, std::make_unique<UnaryExpr<_Operator>>(std::move(left))), 0);
    }

    auto precedence() const -> int override
    {
        return Precedence::POST_UNARY;
    }
};

/**
 * Parse exists operator.
 */
class ExistsOpParser : public InfixParselet
{
    auto parse(Parser& p, ExprPtr left, Token t) const -> ExprPtr override
    {
        return p.parseInfix(simplifyOrForward(p.env, std::make_unique<ExistsExpr>(std::move(left))), 0);
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
        auto right = p.parsePrecedence(precedence(), true);
        if (right)
            return simplifyOrForward(p.env, std::make_unique<BinaryWordOpExpr>(std::move(t),
                                                                               std::move(left),
                                                                               std::move(right)));
        /* Parse as unary operator */
        return p.parseInfix(simplifyOrForward(p.env, std::make_unique<UnaryWordOpExpr>(std::move(t),
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
template <class _Type>
class ScalarParser : public PrefixParselet
{
    auto parse(Parser& p, Token t) const -> ExprPtr override
    {
        return std::make_unique<ConstExpr>(std::get<_Type>(t.value));
    }
};

/**
 * Parser emitting constant expressions.
 */
class ConstParser : public PrefixParselet
{
public:
    template <class _ValueType>
    ConstParser(_ValueType value)
        : value_(Value::make(value))
    {}

    ConstParser(Value value)
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
            std::transform(word.begin(), word.end(), word.begin(), [](auto c) {
                return tolower(c);
            });

            auto arguments = p.parseList(Token::RPAREN);
            return simplifyOrForward(p.env, std::make_unique<CallExpression>(word, std::move(arguments)));
        }

        /* Single field name */
        return std::make_unique<FieldExpr>(std::move(word));
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
        auto right = p.parsePrecedence(precedence());
        return std::make_unique<PathExpr>(std::move(left), std::move(right));
    }

    auto precedence() const -> int override
    {
        return Precedence::PATH;
    }
};

auto compile(Environment& env, std::string_view sv, bool any) -> ExprPtr
{
    Parser p(&env, sv);

    /* Scalars */
    p.prefixParsers[Token::C_TRUE]  = std::make_unique<ConstParser>(Value::t());
    p.prefixParsers[Token::C_FALSE] = std::make_unique<ConstParser>(Value::f());
    p.prefixParsers[Token::C_NULL]  = std::make_unique<ConstParser>(Value::null());
    p.prefixParsers[Token::INT]     = std::make_unique<ScalarParser<int64_t>>();
    p.prefixParsers[Token::FLOAT]   = std::make_unique<ScalarParser<double>>();
    p.prefixParsers[Token::STRING]  = std::make_unique<ScalarParser<std::string>>();

    /* Unary Operators */
    p.prefixParsers[Token::OP_SUB]    = std::make_unique<UnaryOpParser<OperatorNegate>>();
    p.prefixParsers[Token::OP_BITINV] = std::make_unique<UnaryOpParser<OperatorBitInv>>();
    p.prefixParsers[Token::OP_NOT]    = std::make_unique<UnaryOpParser<OperatorNot>>();
    p.prefixParsers[Token::OP_LEN]    = std::make_unique<UnaryOpParser<OperatorLen>>();
    p.infixParsers[Token::OP_BOOL]    = std::make_unique<UnaryPostOpParser<OperatorBool>>();
    p.prefixParsers[Token::OP_TYPEOF] = std::make_unique<UnaryOpParser<OperatorTypeof>>();
    p.infixParsers[Token::OP_EXISTS]  = std::make_unique<ExistsOpParser>();
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
    p.infixParsers[Token::OP_MATCH]  = std::make_unique<BinaryOpParser<OperatorMatch,Precedence::EQUALITY>>();
    p.infixParsers[Token::OP_NOT_MATCH] = std::make_unique<BinaryOpParser<OperatorNotMatch, Precedence::EQUALITY>>();
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
        if (any) {
            std::vector<ExprPtr> root;
            root.emplace_back(p.parse());
            return simplifyOrForward(p.env, std::make_unique<CallExpression>("any"s, std::move(root)));
        } else {
            return p.parse();
        }
    }();

    if (!p.match(Token::Type::NIL))
        throw std::runtime_error("Expected end-of-input; got "s + p.current().toString());
    return expr;
}

auto eval(Environment& env, const Expr& ast, ModelPool const& model, size_t rootIndex) -> std::vector<Value>
{
    if (env.stringCache() != model.strings)
        throw std::runtime_error("Environment must use same string resource as model.");

    Context ctx(&env);

    std::vector<Value> res;
    ast.eval(ctx, Value::field(Value::null(), model.root(rootIndex)), [&res](auto ctx, auto vv) {
        res.push_back(std::move(vv));
        return Result::Continue;
    });

    return res;
}

}
