#include "simfil/simfil.h"
#include "simfil/environment.h"
#include "simfil/exception-handler.h"
#include "simfil/expression-visitor.h"
#include "simfil/model/json.h"
#include "simfil/value.h"
#include "src/expressions.h"

#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <stdexcept>

#include "common.hpp"

using namespace simfil;

static constexpr auto StaticTestKey = StringPool::NextStaticId;

namespace
{

class EnvironmentValueFn final : public Function
{
public:
    explicit EnvironmentValueFn(int64_t value) : value_(value) {}

    auto ident() const -> const FnInfo& override
    {
        static const FnInfo info{
            "environmentValue",
            "Return an environment-specific test value",
            "environmentValue()"};
        return info;
    }

    auto eval(Context ctx, const Value&, const std::vector<ExprPtr>&, const ResultFn& res) const
        -> tl::expected<Result, Error> override
    {
        return res(
            ctx,
            ctx.phase == Context::Phase::Compilation ? Value::undef() : Value::make(value_));
    }

private:
    int64_t value_;
};

auto sharedAst(Environment& env, std::string_view query) -> SharedAST
{
    auto compiled = compile(env, query, false);
    REQUIRE(compiled);
    return SharedAST(std::move(*compiled));
}

}  // namespace

#define REQUIRE_RESULT(query, result) \
    REQUIRE(JoinedResult((query)) == (result))

#define REQUIRE_AST(input, output) \
    REQUIRE(Compile(input, false)->expr().toString() == (output))

#define REQUIRE_AST_AUTOWILDCARD(input, output) \
    REQUIRE(Compile(input, true)->expr().toString() == (output))

#define REQUIRE_ERROR(input) \
    REQUIRE(CompileError(input, false).message != "")

#define REQUIRE_PANIC(input) \
    REQUIRE_RESULT((input), "ERROR: Panic!")


TEST_CASE("Int", "[ast.integer]") {
    REQUIRE_AST("1", "1");
    REQUIRE_AST("123", "123");
    REQUIRE_AST("-1", "-1");
    REQUIRE_AST("0xff", "255");
    REQUIRE_AST("-0xff", "-255");
}

TEST_CASE("Float", "[ast.float]") {
    REQUIRE_AST("1.0", "1.000000");
    REQUIRE_AST("1.5", "1.500000");
    REQUIRE_AST("-1.0", "-1.000000");
    REQUIRE_AST("1e2", "100.000000");
    REQUIRE_AST("1e-2", "0.010000");
}

TEST_CASE("Path", "[ast.path]") {
    REQUIRE_AST("a", "a");
    REQUIRE_AST("a.b", "(. a b)");
    REQUIRE_AST("a.b.c", "(. (. a b) c)");
}

TEST_CASE("Wildcard", "[ast.wildcard]") {
    REQUIRE_AST("*", "*");
    REQUIRE_AST("**", "**");
    REQUIRE_AST("*.a", "*.a");
    REQUIRE_AST("**.a", "**.a"); /* Optimization rewrites this from (. ** a) to **.a */
    REQUIRE_AST("**.a.b.c", "(. (. **.a b) c)");
    REQUIRE_AST("a.**.b", "(. a **.b)");
    REQUIRE_AST("a.**.b.**.c", "(. (. a **.b) **.c)");

    REQUIRE_AST("* == *", "(== * *)");     /* Do not optimize away */
    REQUIRE_AST("** == **", "(== ** **)"); /* Do not optimize away */
}

TEST_CASE(
    "Compiled expressions bind environment-dependent state per evaluator",
    "[evaluation.binding]")
{
    auto firstModel = simfil::json::parse(R"({"target": 1})");
    auto secondModel = simfil::json::parse(R"({"unrelated": 0, "target": 2})");
    REQUIRE(firstModel);
    REQUIRE(secondModel);

    Environment compileEnv(firstModel.value()->strings());
    auto ast = sharedAst(compileEnv, "target");

    Environment firstEnv(firstModel.value()->strings());
    Environment secondEnv(secondModel.value()->strings());
    BoundExpression first(ast, firstEnv);
    BoundExpression second(ast, secondEnv);

    auto firstResult = first.eval(**firstModel.value()->root(0));
    auto secondResult = second.eval(**secondModel.value()->root(0));
    REQUIRE(firstResult);
    REQUIRE(secondResult);
    REQUIRE(firstResult->size() == 1);
    REQUIRE(secondResult->size() == 1);
    CHECK(firstResult->front().toString() == "1");
    CHECK(secondResult->front().toString() == "2");
}

TEST_CASE("Manually constructed ASTs assign distinct runtime cache slots", "[evaluation.binding]")
{
    auto model = simfil::json::parse(R"({"outer": {"target": 42}})");
    REQUIRE(model);

    auto rootExpression = std::make_unique<PathExpr>(
        std::make_unique<FieldExpr>("outer"),
        std::make_unique<FieldExpr>("target"));
    SharedAST ast = std::make_shared<AST>("outer.target", std::move(rootExpression));
    Environment env(model.value()->strings());
    BoundExpression expression(std::move(ast), env);

    auto result = expression.eval(**model.value()->root(0));
    REQUIRE(result);
    REQUIRE(result->size() == 1);
    CHECK(result->front().toString() == "42");
}

TEST_CASE(
    "Shared expressions retain compile-time constants used by extrema functions",
    "[evaluation.binding]")
{
    auto model = simfil::json::parse("{}");
    REQUIRE(model);

    Environment compileEnv(model.value()->strings());
    compileEnv.constants.insert_or_assign("fallback", Value::make(int64_t{7}));
    Environment runtimeEnv(model.value()->strings());
    runtimeEnv.constants.insert_or_assign("fallback", Value::make(int64_t{99}));

    for (const auto function : {"min", "max"}) {
        auto ast = sharedAst(compileEnv, fmt::format("{}(missing, fallback)", function));
        BoundExpression expression(std::move(ast), runtimeEnv);
        auto result = expression.eval(**model.value()->root(0));
        CAPTURE(function);
        REQUIRE(result);
        REQUIRE(result->size() == 1);
        CHECK(result->front().toString() == "7");
    }
}

TEST_CASE("Compiled expressions resolve functions per bound environment", "[evaluation.binding]")
{
    auto model = simfil::json::parse("{}");
    REQUIRE(model);

    Environment compileEnv(model.value()->strings());
    EnvironmentValueFn compileFunction(0);
    compileEnv.functions["environmentValue"] = &compileFunction;
    auto ast = sharedAst(compileEnv, "environmentValue()");

    Environment firstEnv(model.value()->strings());
    Environment secondEnv(model.value()->strings());
    EnvironmentValueFn firstFunction(11);
    EnvironmentValueFn secondFunction(22);
    firstEnv.functions["environmentValue"] = &firstFunction;
    secondEnv.functions["environmentValue"] = &secondFunction;

    BoundExpression first(ast, firstEnv);
    BoundExpression second(ast, secondEnv);
    auto firstResult = first.eval(**model.value()->root(0));
    auto secondResult = second.eval(**model.value()->root(0));
    REQUIRE(firstResult);
    REQUIRE(secondResult);
    CHECK(firstResult->front().toString() == "11");
    CHECK(secondResult->front().toString() == "22");

    Environment lateEnv(model.value()->strings());
    BoundExpression late(ast, lateEnv);
    auto missingResult = late.eval(**model.value()->root(0));
    REQUIRE_FALSE(missingResult);
    CHECK(missingResult.error().type == Error::UnknownFunction);
    lateEnv.functions["environmentValue"] = &firstFunction;
    auto lateResult = late.eval(**model.value()->root(0));
    REQUIRE(lateResult);
    CHECK(lateResult->front().toString() == "11");
}

TEST_CASE("Expression bindings retry fields added to their string pool", "[evaluation.binding]")
{
    auto model = simfil::json::parse("{}");
    REQUIRE(model);
    Environment env(model.value()->strings());
    auto ast = sharedAst(env, "later");
    BoundExpression expression(ast, env);

    auto root = model.value()->root(0);
    REQUIRE(root);
    auto before = expression.eval(**root);
    REQUIRE(before);
    REQUIRE(before->size() == 1);
    CHECK(before->front().isa(ValueType::Null));

    auto object = model.value()->resolve<Object>(**root);
    REQUIRE(object);
    REQUIRE(object->addField("later", int64_t{7}));
    auto after = expression.eval(**root);
    REQUIRE(after);
    REQUIRE(after->size() == 1);
    CHECK(after->front().toString() == "7");
}

TEST_CASE(
    "Immutable compiled expressions support concurrent independent bindings",
    "[evaluation.binding]")
{
    auto compileModel = simfil::json::parse(R"({"target": 0})");
    REQUIRE(compileModel);
    Environment compileEnv(compileModel.value()->strings());
    auto ast = sharedAst(compileEnv, "**.target");

    const auto results = RunConcurrentWorkers<8>(
        [ast](std::size_t worker)
        {
            auto model = simfil::json::parse(
                fmt::format(R"({{"padding{}": 0, "target": {}}})", worker, worker));
            if (!model)
                return false;

            Environment env(model.value()->strings());
            BoundExpression expression(ast, env);
            for (auto iteration = 0; iteration < 100; ++iteration) {
                auto result = expression.eval(**model.value()->root(0));
                if (!result || result->size() != 1 ||
                    result->front().toString() != std::to_string(worker))
                    return false;
            }
            return true;
        });
    for (auto result : results)
        CHECK(result);
}

TEST_CASE("OperatorConst", "[ast.operator]") {
    /* Arithmetic */
    REQUIRE_AST("1+2", "3");
    REQUIRE_AST("1.5+2", "3.500000");
    REQUIRE_AST("2*2", "4");
    REQUIRE_AST("2.5*2", "5.000000");
    REQUIRE_AST("8/2", "4");
    REQUIRE_AST("3/2.0", "1.500000");
    REQUIRE_AST("-a",  "(- a)");
    REQUIRE_AST("a+2", "(+ a 2)");
    REQUIRE_AST("2+a", "(+ 2 a)");
    REQUIRE_AST("a+b", "(+ a b)");
    REQUIRE_PANIC("1+panic()");
    REQUIRE_PANIC("panic()+1");

    auto GetError = [&](std::string_view query) -> std::string {
        Environment env(Environment::WithNewStringCache);
        auto ast = compile(env, query);

        REQUIRE(!ast);
        if (!ast)
            return ast.error().message;
        return "Ok";
    };

    /* Division by zero */
    REQUIRE(GetError("1/0") == "Division by zero");
    REQUIRE(GetError("1%0") == "Division by zero");

    /* String */
    REQUIRE_AST("'a'+null", "\"anull\"");
    REQUIRE_AST("null+'a'", "\"nulla\"");

    /* Comparison */
    REQUIRE_AST("1==1", "true");
    REQUIRE_AST("1!=1", "false");

    REQUIRE_AST("2>1",  "true");
    REQUIRE_AST("1>=1",  "true");
    REQUIRE_AST("1.0>=1",  "true");
    REQUIRE_AST("1>=1.0",  "true");
    REQUIRE_AST("1<2", "true");
    REQUIRE_AST("2<1", "false");
    REQUIRE_AST("2<=2", "true");
    REQUIRE_AST("2<=1", "false");
    REQUIRE_AST("1<=1.1", "true");
    REQUIRE_AST("1.0<=1", "true");
    REQUIRE_AST("1.0<=1.0", "true");
    REQUIRE_AST("'a'<'b'", "true");
    REQUIRE_AST("'a'<='b'", "true");
    REQUIRE_AST("'b'>'a'", "true");
    REQUIRE_AST("'b'>='b'", "true");
    REQUIRE_AST("b\"89899\" > 5", "true");
    REQUIRE_AST("b\"89899\" > \"normal-string\"", "false");

    /* Null behaviour */
    REQUIRE_AST("1<null", "false");
    REQUIRE_AST("1>null", "false");
    REQUIRE_AST("null<1", "false");
    REQUIRE_AST("null>1", "false");
    REQUIRE_AST("null<null", "false");
    REQUIRE_AST("null>null", "false");
    REQUIRE_AST("null==null", "true");
    REQUIRE_AST("1+null", "null");
    REQUIRE_AST("null+1", "null");
    REQUIRE_AST("1*null", "null");
    REQUIRE_AST("null*1", "null");
    REQUIRE_AST("1-null", "null");
    REQUIRE_AST("null-1", "null");
    REQUIRE_AST("1/null", "null");
    REQUIRE_AST("null/1", "null");
    REQUIRE_AST("null%null", "null");
    REQUIRE_AST("null/null", "null");

    /* Typeof */
    REQUIRE_AST("typeof 'abc'", "\"string\"");
    REQUIRE_AST("typeof 123",   "\"int\"");
    REQUIRE_AST("typeof 123.1", "\"float\"");
    REQUIRE_AST("typeof true",  "\"bool\"");
    REQUIRE_AST("typeof null",  "\"null\"");
    REQUIRE_AST("typeof b\"ff\"", "\"bytes\"");

    /* Precedence */
    REQUIRE_AST("1+1*2",   "3");
    REQUIRE_AST("(1+1)*2", "4");
    REQUIRE_AST("2*1+1",   "3");
    REQUIRE_AST("2*(1+1)", "4");

    /* Casts */
    REQUIRE_AST("1 as float",      "1.000000");
    REQUIRE_AST("true as float",   "1.000000");
    REQUIRE_AST("1.5 as int",      "1");
    REQUIRE_AST("'123' as int",    "123");
    REQUIRE_AST("'123' as float",  "123.000000");
    REQUIRE_AST("'123' as bool",   "true");
    REQUIRE_AST("'123' as null",   "null");
    REQUIRE_AST("123 as string",   "\"123\"");
    REQUIRE_AST("123.1 as string", "\"123.100000\"");
    REQUIRE_AST("true as string",  "\"true\"");
    REQUIRE_AST("false as string", "\"false\"");
    REQUIRE_AST("null as string",  "\"null\"");
    REQUIRE_AST("range(1,3) as string", "\"1..3\"");
    REQUIRE_AST("b\"89899\" as string", "\"89899\"");
    REQUIRE_AST("\"A normal string\" as bytes", "b\"A normal string\"");
    REQUIRE_AST("0xff as bytes == 0xff", "true");
    REQUIRE_AST("true as bytes == 1", "true");
    REQUIRE_AST("false as bytes == 0", "true");
    REQUIRE_ERROR("1.5 as bytes");

    /* Bool Cast */
    REQUIRE_AST("123?", "true");
    REQUIRE_AST("0.0?", "true");
    REQUIRE_AST("false?", "false");
    REQUIRE_AST("null?", "false");

    /* Unpack */
    REQUIRE_AST("null ...",         "null");
    REQUIRE_AST("1 ...",            "1");
    REQUIRE_AST("1.5 ...",          "1.500000");
    REQUIRE_AST("'ab'...",          "\"ab\"");
    REQUIRE_AST("a ...",            "(... a)");
    REQUIRE_AST("range(1,3)...",    "{1 2 3}");

    /* Call */
    REQUIRE_AST("range(a,3)",    "(range a 3)");
    REQUIRE_AST("range(1,a)",    "(range 1 a)");
    REQUIRE_AST("range(a,b)",    "(range a b)");
    REQUIRE_AST("range(a,b)...", "(... (range a b))");

    /* Index */
    REQUIRE_AST("'abc'[1]", "\"b\"");
    REQUIRE_AST("a[1]",     "(index a 1)");
    REQUIRE_AST("'abc'[a]", "(index \"abc\" a)");

    /* Sub */
    REQUIRE_AST("'abc'{_}", "\"abc\"");
    REQUIRE_AST("1{1}",     "1");
    REQUIRE_AST("a{1}",     "(sub a 1)");
    REQUIRE_AST("1{a}",     "(sub 1 a)");
    REQUIRE_AST("a{a}",     "(sub a a)");

    /* Length */
    REQUIRE_AST("#'abc'",   "3");

    /* Bit */
    REQUIRE_AST("~0xf0",       "-241");
    REQUIRE_AST("0x80 >> 2",   "32");
    REQUIRE_AST("32 << 2",     "128");
    REQUIRE_AST("0xf0 & 0x80", "128");
    REQUIRE_AST("0xf0 | 0x01", "241");
}

TEST_CASE("OperatorLength", "[ast.operator-length]") {
    REQUIRE_ERROR("#0");
    REQUIRE_ERROR("#0.0");
    REQUIRE_ERROR("#true");
    REQUIRE_AST("#null", "null");
    REQUIRE_AST("#'abc'", "3");
}

TEST_CASE("CompareIncompatibleTypes", "[ast.compare-incompatible]") {
    REQUIRE_AST("1=\"A\"", "false");
    REQUIRE_AST("1!=\"A\"", "true");
    REQUIRE_AST("1>\"A\"", "false");
    REQUIRE_AST("1>=\"A\"", "false");
    REQUIRE_AST("1<\"A\"", "false");
    REQUIRE_AST("1<=\"A\"", "false");

    /* Regular Expressions */
    REQUIRE_AST("re\"A\"=1", "false");
    REQUIRE_AST("re\"A\"!=1", "true");
    REQUIRE_AST("1==re\"A\"", "false");
    REQUIRE_AST("1!=re\"A\"", "true");

    /* Ranges */
    REQUIRE_AST("range(0,10)=\"A\"", "false");
    REQUIRE_AST("range(0,10)!=\"A\"", "true");
}

TEST_CASE("Deprecated auto wildcard has no non-schema fallback", "[ast.auto-expand-constant]") {
    REQUIRE_AST_AUTOWILDCARD("a = 1",   "(== a 1)");
    REQUIRE_AST_AUTOWILDCARD("a.* = 1", "(== (. a *) 1)");
    REQUIRE_AST_AUTOWILDCARD("** = 1",  "(== ** 1)");
    REQUIRE_AST_AUTOWILDCARD("1",       "1");
    REQUIRE_AST_AUTOWILDCARD("1+4",     "5");
    REQUIRE_AST_AUTOWILDCARD("ABC",     "ABC");
}

TEST_CASE("CompareIncompatibleTypesFields", "[ast.compare-incompatible-types-fields]") {
    const char* const doc = R"json(
        [
            {"field": 1, "another": 1.5},
            {"field": "text"},
            {"field": true, "another": false}
        ]
    )json";

    const auto model = simfil::json::parse(doc);
    REQUIRE(model);
    auto test = [&model](auto query) {
        Environment env(model.value()->strings());

        auto ast = compile(env, query, false);
        if (!ast)
            INFO(ast.error().message);
        REQUIRE(ast.has_value());
        REQUIRE(*ast);

        INFO("AST: " << (*ast)->expr().toString());

        auto root = model.value()->root(0);
        REQUIRE(root);

        return eval(env, **ast, **root, nullptr).value().front().template as<ValueType::Bool>();
    };

    /* Test some field with different value types for different objects */
    REQUIRE(test("any(*.field=1)") == true);
    REQUIRE(test("any(*.field!=1)") == true);
    REQUIRE(test("any(*.field>1)") == false);
    REQUIRE(test("any(*.field>=1)") == true);
    REQUIRE(test("any(*.field<100)") == true);

    /* Test some field that does not exist for every object */
    REQUIRE(test("any(*.another>1)") == true);
    REQUIRE(test("any(*.another=false)") == true);
    REQUIRE(test("any(*.another=\"text\")") == false);

    /* Test that all-expressions need to hold true for all objects */
    REQUIRE(test("all(*.another=1)") == false);
}

TEST_CASE("OperatorNegate", "[ast.operator-negate]") {
    REQUIRE_ERROR("-('abc')");
    REQUIRE_ERROR("-(true)");
    REQUIRE_PANIC("-panic()");
    REQUIRE_AST("-(1)", "-1");
    REQUIRE_AST("-(1.1)", "-1.100000");
    REQUIRE_AST("-(null)", "null");
}

TEST_CASE("OperatorSubstract", "[ast.operator-substract]") {
    REQUIRE_AST("1-2", "-1");
    REQUIRE_AST("1-2.0", "-1.000000");
    REQUIRE_AST("1-null", "null");
    REQUIRE_AST("1.0-2", "-1.000000");
    REQUIRE_AST("1.0-2.0", "-1.000000");
    REQUIRE_AST("1.0-null", "null");
    REQUIRE_ERROR("1-true");
    REQUIRE_ERROR("true-1");
    REQUIRE_ERROR("true-false");
}

TEST_CASE("OperatorNot", "[ast.operator-not]") {
    REQUIRE_AST("not true", "false");
    REQUIRE_AST("not false", "true");
    REQUIRE_AST("not 'abc'", "false");
    REQUIRE_AST("not 1", "false");
    REQUIRE_AST("not 1.0", "false");
    REQUIRE_AST("not null", "true");
}

TEST_CASE("OperatorAndOr", "[ast.operator-and-or]") {
    /* Or */
    REQUIRE_AST("null or 1", "1");
    REQUIRE_AST("false or 1", "1");
    REQUIRE_AST("1 or null", "1");
    REQUIRE_AST("1 or 2",    "1");
    REQUIRE_AST("a or b",    "(or a b)");

    /* And */
    REQUIRE_AST("null and 1", "null");
    REQUIRE_AST("false and 1", "false");
    REQUIRE_AST("1 and null", "null");
    REQUIRE_AST("true and 2", "2");
    REQUIRE_AST("1 and 2",    "2");
    REQUIRE_AST("a and b",    "(and a b)");
}

TEST_CASE("Constants", "[ast.constant]") {
    REQUIRE_AST("a_number", "123");
}

TEST_CASE("Unquoted words are fields without schema metadata", "[ast.symbol]") {
    REQUIRE_AST("ABC", "ABC");
    REQUIRE_AST("ABC == ABC", "(== ABC ABC)");
    REQUIRE_AST("a.ABC", "(. a ABC)");
    REQUIRE_AST("a.ABC.DEF", "(. (. a ABC) DEF)");
    REQUIRE_AST("a.(ABC)", "(. a ABC)");
    REQUIRE_AST("a.(_.ABC)", "(. a (. _ ABC))");
    REQUIRE_AST("a[ABC]", "(index a ABC)");
    REQUIRE_AST("a[_.ABC]", "(index a (. _ ABC))");
    REQUIRE_AST("a{ABC}", "(sub a ABC)");
    REQUIRE_AST("a{_.ABC}", "(sub a (. _ ABC))");
}

TEST_CASE("ModeSetter", "[ast.mode-setter]") {
    REQUIRE_AST("any(true)",   "true");
    REQUIRE_AST("any(a.b)",    "(any (. a b))");
    REQUIRE_AST("each(true)",  "true");
    REQUIRE_AST("each(a.b)",   "(each (. a b))");
    REQUIRE_AST("count(true)", "1");
    REQUIRE_AST("count(a.b)",  "(count (. a b))");
}

TEST_CASE("UtilityFns", "[ast.functions]") {
    REQUIRE_PANIC("range(panic(), 5)");
    REQUIRE_PANIC("range(1, panic())");
    REQUIRE_AST("range(a,b)",    "(range a b)"); /* Ca not optimize */
    REQUIRE_AST("range(1,5)",    "1..5");
    REQUIRE_AST("range(1,5)==0", "false");
    REQUIRE_AST("range(1,5)==3", "true");
    REQUIRE_AST("range(1,5)==6", "false");
    REQUIRE_AST("range(1,5)...", "{1 2 3 4 5}");
    REQUIRE_AST("range(5,1)",    "5..1");
    REQUIRE_AST("range(5,1)==0", "false");
    REQUIRE_AST("range(5,1)==3", "true");
    REQUIRE_AST("range(5,1)==6", "false");
    REQUIRE_AST("range(5,1)...", "{5 4 3 2 1}");
    REQUIRE_AST("arr(a,b)",      "(arr a b)");
    REQUIRE_AST("arr(2,1)",      "{2 1}");

    REQUIRE_AST("trace(a.b)",         "(trace (. a b))");
    REQUIRE_AST("trace('test', a.b)", "(trace \"test\" (. a b))");

    /* Test case-insensitivity */
    REQUIRE_AST("TRACE(1)",      "(TRACE 1)");
    REQUIRE_AST("Trace(1)",      "(Trace 1)");
}

TEST_CASE("PanicFunction", "[eval.panic-function]") {
    REQUIRE_RESULT("panic()", "ERROR: Panic!");
}

TEST_CASE("OperatorOrShortCircuit", "[eval.operator-or-short-circuit]") {
    REQUIRE_RESULT("true or panic()", "true");
}

TEST_CASE("OperatorAndShortCircuit", "[eval.operator-and-short-circuit]") {
    REQUIRE_RESULT("false and panic()", "false");
}

TEST_CASE("OperatorOr", "[eval.operator-or]") {
    REQUIRE_RESULT("false or false", "false");
    REQUIRE_RESULT("false or true", "true");
    REQUIRE_RESULT("true or false", "true");
    REQUIRE_RESULT("true or true", "true");
}

TEST_CASE("OperatorAnd", "[eval.operator-and]") {
    REQUIRE_RESULT("false and false", "false");
    REQUIRE_RESULT("false and true", "false");
    REQUIRE_RESULT("true and false", "false");
    REQUIRE_RESULT("true and true", "true");
}

TEST_CASE("Path Wildcard", "[yaml.path-wildcard]") {
    REQUIRE_RESULT("sub.*", R"(sub a|sub b|{"a":"sub sub a","b":"sub sub b"})");
    REQUIRE_RESULT("sub.**", R"({"a":"sub a","b":"sub b","sub":{"a":"sub sub a","b":"sub sub b"}}|sub a|sub b|)"
                             R"({"a":"sub sub a","b":"sub sub b"}|sub sub a|sub sub b)");
    REQUIRE_RESULT("**.a", "1|sub a|sub sub a");
    REQUIRE_RESULT("(sub.*.{typeof _ != 'model'} + sub.*.{typeof _ != 'model'})._", "sub asub a|sub asub b|sub bsub a|sub bsub b"); /* . filters null */
    REQUIRE_RESULT("sub.*.{typeof _ != 'model'} + sub.*.{typeof _ != 'model'}", "sub asub a|sub asub b|sub bsub a|sub bsub b"); /* {_} filters null */
    REQUIRE_RESULT("count(*)", "12");
    REQUIRE_RESULT("count(**)", "51");
    REQUIRE_RESULT("count(sub.**.a)", "2");
    REQUIRE_RESULT("count(**.{typeof _ == 'string'})", "11");
    REQUIRE_RESULT("count(sub.**.{typeof _ == 'string'})", "4");
}

TEST_CASE("Array Access", "[yaml.array-access]") {
    REQUIRE_RESULT("c[0]", "a");
    REQUIRE_RESULT("c[1]", "b");
    REQUIRE_RESULT("c[2]", "c");
    REQUIRE_RESULT("#c",   "3");

    REQUIRE_RESULT("c[-1]","null"); /* Out of bounds */
    REQUIRE_RESULT("c[4]", "null"); /* Out of bounds */

    REQUIRE_RESULT("c",    R"(["a","b","c"])");
    REQUIRE_RESULT("c._",  R"(["a","b","c"])"); /* No implicit child traversal! */
    REQUIRE_RESULT("c.*",  "a|b|c");
    REQUIRE_RESULT("c.**", R"(["a","b","c"]|a|b|c)");

    REQUIRE_RESULT("c[arr(0,2)]",      "a|c");
    REQUIRE_RESULT("c[range(0,2)...]", "a|b|c");
    REQUIRE_RESULT("c[d.*]",           "a|b|c");

    REQUIRE_RESULT("typeof c.* == 'string'", "true|true|true");
    REQUIRE_RESULT("c.* != 'a'",             "false|true|true");
}

TEST_CASE("Single Values", "[yaml.single-values]") {

    auto json = R"({"__long__name__":true,"a":1,"abc def":true,"b":2,"c":["a","b","c"],"d":[0,1,2],"geoLineString":{"geometry":{"coordinates":[[1,2],[3,4]],"type":"LineString"}},"geoPoint":{"geometry":{"coordinates":[1,2],"type":"Point"}},"geoPolygon":{"geometry":{"coordinates":[[[1,2],[3,4],[5,6]]],"type":"Polygon"}},"number":123,"string":"TEXT","sub":{"a":"sub a","b":"sub b","sub":{"a":"sub sub a","b":"sub sub b"}}})";
    auto sub_json = R"({"a":"sub a","b":"sub b","sub":{"a":"sub sub a","b":"sub sub b"}})";
    auto sub_sub_json = R"({"a":"sub sub a","b":"sub sub b"})";

    REQUIRE_RESULT("_", json);
    REQUIRE_RESULT("_._", json);
    REQUIRE_RESULT("a", "1");
    REQUIRE_RESULT("['a']", "1");
    REQUIRE_RESULT("b", "2");
    REQUIRE_RESULT("sub", sub_json);
    REQUIRE_RESULT("sub.a", "sub a");
    REQUIRE_RESULT("sub.sub", sub_sub_json);
    REQUIRE_RESULT("sub.sub.a", "sub sub a");
}

TEST_CASE("Model Functions", "[yaml.model-functions]") {
    SECTION("Test any(...)") {
        REQUIRE_RESULT("any(arr(null, null))", "false");
        REQUIRE_RESULT("any(arr(true, null))", "true");
        REQUIRE_RESULT("any(arr(null, true))", "true");
        REQUIRE_RESULT("any(arr(true, true))", "true");
    }
    SECTION("Test each(...)") {
        REQUIRE_RESULT("each(arr(null, null))", "false");
        REQUIRE_RESULT("each(arr(true, null))", "false");
        REQUIRE_RESULT("each(arr(null, true))", "false");
        REQUIRE_RESULT("each(arr(true, true))", "true");
    }
    SECTION("Test arr(...)") {
        REQUIRE_RESULT("arr(2,3,5,7,'ok')", "2|3|5|7|ok");

        REQUIRE_PANIC("arr(0,panic(),2)");
    }
    SECTION("Test split(...)") {
        REQUIRE_RESULT("split('hello.this.is.a.test.', '.')", "hello|this|is|a|test|");
        REQUIRE_RESULT("split('hello.this.is.a.test.', '.', false)", "hello|this|is|a|test");

        REQUIRE_PANIC("split(panic(), '.')");
        REQUIRE_PANIC("split('a.b.c', panic())");
    }
    SECTION("Test select(...)") {
        REQUIRE_RESULT("select(split('a.b.c.d', '.'), a)", "b");
        REQUIRE_RESULT("select(split('a.b.c.d', '.'), 0)", "a");
        REQUIRE_RESULT("select(split('a.b.c.d', '.'), 1, 2)", "b|c");
        REQUIRE_RESULT("select(split('a.b.c.d', '.'), 1, 0)", "b|c|d");

        REQUIRE_PANIC("select(panic(), 0)");
        REQUIRE_PANIC("select(0, panic())");
    }
    SECTION("Test sum(...)") {
        REQUIRE_RESULT("sum(range(1, 10)...)", "55");
        REQUIRE_RESULT("sum(range(1, 10)..., $sum + $val)", "55");
        REQUIRE_RESULT("sum(range(1, 10)..., $sum + $val, 10)", "65");
        REQUIRE_RESULT("sum(range(1, 10)..., $sum * $val, 1)", "3628800");

        REQUIRE_PANIC("sum(panic())");
        REQUIRE_PANIC("sum(range(1, 10)..., panic())");
        REQUIRE_PANIC("sum(range(1, 10)..., 0, panic())");
    }

    SECTION("Test extrema") {
        struct ExtremumExpectations {
            std::string_view name;
            std::string_view scalarResult;
            std::string_view arrayResult;
            std::string_view numericResult;
            std::string_view stringResult;
            std::string_view modelResult;
        };

        const auto call = [](std::string_view name, std::string_view arguments) {
            return std::string(name) + "(" + std::string(arguments) + ")";
        };

        for (const auto& expectation : {
                 ExtremumExpectations{"min", "4", "3", "1", "alpha", "7"},
                 ExtremumExpectations{"max", "7", "8", "1.500000", "beta", "123"},
             }) {
            CAPTURE(expectation.name);
            REQUIRE_RESULT(call(expectation.name, "4, 6, 7"), expectation.scalarResult);
            REQUIRE_RESULT(call(expectation.name, "arr(8, 3, 5)"), expectation.arrayResult);
            REQUIRE_RESULT(call(expectation.name, "1.5, 1"), expectation.numericResult);
            REQUIRE_RESULT(call(expectation.name, "'beta', 'alpha'"), expectation.stringResult);
            REQUIRE_RESULT(call(expectation.name, "number, 7"), expectation.modelResult);
            REQUIRE_RESULT(call(expectation.name, "missing.value, 7"), "7");
            REQUIRE_RESULT(call(expectation.name, "missing.value"), "null");
            REQUIRE_RESULT(call(expectation.name, "null, 4"), "4");
            REQUIRE_RESULT(call(expectation.name, "null"), "null");
            REQUIRE_ERROR(call(expectation.name, ""));
            REQUIRE_PANIC(call(expectation.name, "panic()"));
            REQUIRE_PANIC(call(expectation.name, "4, panic()"));
        }
    }
    SECTION("Count non-false values of arr(...)") {
        REQUIRE_RESULT("count(arr(null, null))", "0");
        REQUIRE_RESULT("count(arr(true, null))", "1");
        REQUIRE_RESULT("count(arr(null, true))", "1");
        REQUIRE_RESULT("count(arr(true, true))", "2");

        REQUIRE_PANIC("count(panic())");
    }
    SECTION("Count model keys") {
        REQUIRE_RESULT("count(keys(**))", "50");

        REQUIRE_PANIC("keys(panic())");
        REQUIRE_PANIC("keys(**.{panic()})");
        REQUIRE_PANIC("panic(keys(**))");
    }
}

TEST_CASE("Sub-Selects", "[yaml.sub-selects]") {
    SECTION("Filter out null values") {
        REQUIRE_RESULT("count(** as int)", "51"); /* Unfiltered */
        REQUIRE_RESULT("count(**{typeof _ != 'null' and typeof _ != 'model'})", "31"); /* Filtered */
    }
    SECTION("Filter out all values") {
        REQUIRE_RESULT("**{false}", "null"); /* Non-Value returns single 'null' */
    }
    SECTION("Filter out all strings") {
        REQUIRE_RESULT("each(typeof **{typeof _ == 'string'} == 'string')", "true");
    }
}

TEST_CASE("Value Expansion", "[yaml.value-expansion]") {
    SECTION("Compare expanded list of values against single value") {
        REQUIRE_RESULT("arr(1,2,3) == 2", "false|true|false");
        REQUIRE_RESULT("2 == arr(1,2,3)", "false|true|false");
    }
    SECTION("Compare two expanded list of values") {
        REQUIRE_RESULT("arr(1,2,3) == arr(1,2,3)", "true|false|false|false|true|false|false|false|true");
    }

    SECTION("Compare value to lists") {
        REQUIRE_RESULT("each(range(1,10)... == 1)", "false");
        REQUIRE_RESULT("each(range(1,100)... == 1)", "false");
        REQUIRE_RESULT("each(range(1,1000)... == 1)", "false");
        REQUIRE_RESULT("each(range(1,10000)... == 1)", "false");
    }
}

TEST_CASE("Model Pool Validation", "[model.validation]") {
    auto pool = std::make_shared<ModelPool>();

    // Recognize dangling object member pointer
    pool->clear();
    pool->newObject()->addField(
        "good",
        ModelNode::Ptr::make(pool, ModelNodeAddress{ModelPool::Objects, 666}));
    REQUIRE(!pool->validate());

    // Recognize dangling array member pointer
    pool->clear();
    pool->newObject()->addField(
        "good",
        ModelNode::Ptr::make(pool, ModelNodeAddress{ModelPool::Arrays, 666}));
    REQUIRE(!pool->validate());

    // Recognize dangling root
    pool->clear();
    pool->addRoot(
        ModelNode::Ptr::make(pool, ModelNodeAddress{ModelPool::Objects, 666}));
    REQUIRE(!pool->validate());

    // An empty model should be valid
    pool->clear();
    REQUIRE(pool->validate());

    // An empty object should also be valid
    pool->newObject()->addField("good", pool->newObject());
    REQUIRE(pool->validate());
}

TEST_CASE("Procedural Object Node", "[model.procedural]") {
    auto pool = std::make_shared<ModelPool>();
    pool->strings()->addStaticKey(StaticTestKey, "test");

    struct DerivedProceduralObject : public ProceduralObject<2, DerivedProceduralObject> {
        DerivedProceduralObject(ModelConstPtr pool, ModelNodeAddress a, detail::mp_key key)
            : ProceduralObject<2, DerivedProceduralObject>(
                  static_cast<ArrayIndex>(a.index()),
                  std::move(pool),
                  a,
                  key)
        {
            fields_.emplace_back(
                StaticTestKey,
                [] (const auto& self) { return model_ptr<ValueNode>::make(std::string_view("static"), self.model_); }); //NOSONAR
        }
    };

    auto baseObj = pool->newObject();
    baseObj->addField("mood", "blue");

    auto proceduralObj = model_ptr<DerivedProceduralObject>::make(pool, baseObj->addr());
    REQUIRE(proceduralObj->get(pool->strings()->get("mood"))->value() == ScalarValueType(std::string_view("blue")));
    REQUIRE(proceduralObj->get(StaticTestKey)->value() == ScalarValueType(std::string_view("static")));
}

TEST_CASE("Object/Array Extend", "[model.extend]") {
    auto pool = std::make_shared<ModelPool>();

    SECTION("Extend object")
    {
        auto testObjectA = pool->newObject();
        testObjectA->addField("name", "hans");
        testObjectA->addField("occupation", "baker");

        auto testObjectB = pool->newObject();
        testObjectB->addField("height", (int64_t)220);
        testObjectB->addField("age", (int64_t)55);

        {
            REQUIRE(testObjectA->size() == 2);
            REQUIRE(testObjectB->size() == 2);
            auto nameValue = testObjectA->get("name");
            REQUIRE(nameValue);
            REQUIRE(Value(nameValue.value()->value()).toString() == "hans");
            auto occupationValue = testObjectA->get("occupation");
            REQUIRE(occupationValue);
            REQUIRE(Value(occupationValue.value()->value()).toString() == "baker");
            REQUIRE(!testObjectA->get("height"));
            REQUIRE(!testObjectA->get("age"));
            testObjectA->extend(testObjectB);
        }

        {
            REQUIRE(testObjectA->size() == 4);
            REQUIRE(testObjectB->size() == 2);
            auto nameValue = testObjectA->get("name");
            REQUIRE(nameValue);
            REQUIRE(Value(nameValue.value()->value()).toString() == "hans");
            auto occupationValue = testObjectA->get("occupation");
            REQUIRE(occupationValue);
            REQUIRE(Value(occupationValue.value()->value()).toString() == "baker");
            auto heightValue = testObjectA->get("height");
            REQUIRE(heightValue);
            REQUIRE(Value(heightValue.value()->value()).as<ValueType::Int>() == 220ll);
            auto ageValue = testObjectA->get("age");
            REQUIRE(ageValue);
            REQUIRE(Value(ageValue.value()->value()).as<ValueType::Int>() == 55ll);
        }
    }

    SECTION("Extend array")
    {
        auto testArrayA = pool->newArray();
        // The bool overload is used if we don't cast to strings here explicitly.
        testArrayA->append(std::string("hans"));
        testArrayA->append(std::string("baker"));

        auto testArrayB = pool->newArray();
        testArrayB->append((int64_t)220);
        testArrayB->append((int64_t)55);

        REQUIRE(testArrayA->size() == 2);
        REQUIRE(testArrayB->size() == 2);
        REQUIRE(Value(testArrayA->at(0)->value()).toString() == "hans");
        REQUIRE(Value(testArrayA->at(1)->value()).toString() == "baker");
        REQUIRE(!testArrayA->at(2));
        REQUIRE(!testArrayA->at(3));
        testArrayA->extend(testArrayB);

        REQUIRE(testArrayA->size() == 4);
        REQUIRE(testArrayB->size() == 2);
        REQUIRE(Value(testArrayA->at(0)->value()).toString() == "hans");
        REQUIRE(Value(testArrayA->at(1)->value()).toString() == "baker");
        REQUIRE(Value(testArrayA->at(2)->value()).as<ValueType::Int>() == 220ll);
        REQUIRE(Value(testArrayA->at(3)->value()).as<ValueType::Int>() == 55ll);
    }
}

TEST_CASE("Switch Model String Pool", "[model.setStrings]")
{
    auto pool = std::make_shared<ModelPool>();
    auto oldFieldDict = pool->strings();
    auto newFieldDict = std::make_shared<simfil::StringPool>(*oldFieldDict);
    pool->setStrings(newFieldDict);
    REQUIRE(pool->strings() == newFieldDict);

    auto obj = pool->newObject();
    obj->addField("hello", "world");

    oldFieldDict->emplace("gobbledigook");
    pool->setStrings(oldFieldDict);

    REQUIRE(pool->strings() == oldFieldDict);
    REQUIRE_NOTHROW(pool->validate());
    REQUIRE(oldFieldDict->size() != newFieldDict->size());
}

TEST_CASE("StringPool copy owns lookup views", "[string-pool]")
{
    auto source = std::make_shared<simfil::StringPool>();
    auto id = source->emplace("owned-dynamic-field");
    REQUIRE(id);

    auto sourceView = source->resolve(*id);
    REQUIRE(sourceView);

    auto copy = std::make_shared<simfil::StringPool>(*source);
    auto copyView = copy->resolve(*id);
    REQUIRE(copyView);

    REQUIRE(*copyView == *sourceView);
    REQUIRE(copyView->data() != sourceView->data());
    REQUIRE(copy->get("owned-dynamic-field") == *id);
}

TEST_CASE("Model and string pools report retained memory", "[memory][model][string-pool]")
{
    auto pool = std::make_shared<ModelPool>();
    auto object = pool->newObject(8);
    object->addField("long-enough-field-name-to-allocate", "long-enough-value-to-allocate");
    pool->addRoot(object);

    auto const modelUsage = pool->memoryUsageStats().total();
    REQUIRE(modelUsage.logicalBytes > 0);
    REQUIRE(modelUsage.allocatedBytes >= modelUsage.logicalBytes);

    auto const stringUsage = pool->strings()->memoryUsage();
    REQUIRE(stringUsage.logicalBytes >= std::string_view("long-enough-field-name-to-allocate").size());
    REQUIRE(stringUsage.allocatedBytes >= stringUsage.logicalBytes);
}

TEST_CASE("Exception Handler", "[exception]")
{
    bool handlerCalled = false;
    std::string message;

    simfil::ThrowHandler::instance().set([&](auto&& type, auto&& msg){
        handlerCalled = true;
        message = msg;
    });

    REQUIRE_THROWS(raise<std::runtime_error>("TestMessage"));
    REQUIRE(handlerCalled);
    REQUIRE(message == "TestMessage");

    // Reset throw-handler, so it isn't erroneously used by other tests.
    simfil::ThrowHandler::instance().set(nullptr);
}

TEST_CASE("Visit AST", "[visit.ast]")
{
    Environment env(Environment::WithNewStringCache);

    auto ast = compile(env, "**.field = 123", false, false);
    if (!ast)
        INFO(ast.error().message);
    REQUIRE(ast.has_value());
    REQUIRE(*ast);

    struct Visitor : ExprVisitor
    {
        std::optional<std::string> visitedFieldName;

        using ExprVisitor::visit;

        auto visit(const FieldExpr& expr) -> void override
        {
            ExprVisitor::visit(expr);

            visitedFieldName = expr.name_;
        }

        auto visit(const WildcardFieldExpr& expr) -> void override
        {
            ExprVisitor::visit(expr);

            visitedFieldName = expr.name_;
        }
    };

    Visitor visitor;
    (*ast)->expr().accept(visitor);

    REQUIRE(visitor.visitedFieldName == "field");
}

TEST_CASE("Visitors traverse unary children once", "[visit.ast]")
{
    UnaryExpr<OperatorNot> expr(std::make_unique<FieldExpr>("field"));

    struct Visitor : ExprVisitor
    {
        int fieldVisits = 0;

        using ExprVisitor::visit;

        auto visit(const FieldExpr& expr) -> void override
        {
            ExprVisitor::visit(expr);
            ++fieldVisits;
        }
    };

    Visitor visitor;
    expr.accept(visitor);

    REQUIRE(visitor.fieldVisits == 1);
}

TEST_CASE("Parsed token locations are preserved", "[ast.source-location]")
{
    Environment env(Environment::WithNewStringCache);

    auto fieldAst = compile(env, "field", false, false);
    REQUIRE(fieldAst);

    const auto* fieldExpr = dynamic_cast<const FieldExpr*>(&(*fieldAst)->expr());
    REQUIRE(fieldExpr);
    REQUIRE(fieldExpr->sourceLocation().offset == 0);
    REQUIRE(fieldExpr->sourceLocation().size == 5);

    auto binaryAst = compile(env, "field + 1", false, false);
    REQUIRE(binaryAst);

    const auto* binaryExpr = dynamic_cast<const BinaryExpr<OperatorAdd>*>(&(*binaryAst)->expr());
    REQUIRE(binaryExpr);
    REQUIRE(binaryExpr->sourceLocation().offset == 6);
    REQUIRE(binaryExpr->sourceLocation().size == 1);
}

TEST_CASE("AST expr ids are reenumerated after rewrites", "[ast.expr-id]")
{
    auto ast = Compile("**.field = 123", false);

    std::vector<Expr::ExprId> ids;
    const auto collectIds = [&](const auto& self, const Expr& expr) -> void {
        ids.emplace_back(expr.id());
        for (auto i = 0u; i < expr.numChildren(); ++i)
            self(self, *expr.childAt(i));
    };

    collectIds(collectIds, ast->expr());

    REQUIRE(ids == std::vector<Expr::ExprId>{0, 1, 2});
}
