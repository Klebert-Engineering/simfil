#include "simfil/simfil.h"
#include "simfil/exception-handler.h"
#include "simfil/model/json.h"

#include <catch2/catch_test_macros.hpp>

using namespace simfil;

static const auto StaticTestKey = StringPool::NextStaticId;


static auto getASTString(std::string_view input)
{
    Environment env(Environment::WithNewStringCache);
    return compile(env, input, false);
}

#define REQUIRE_AST(input, output) \
    REQUIRE(getASTString(input)->toString() == output);

#define REQUIRE_UNDEF(input) \
    REQUIRE(getASTString(input)-> == output);

TEST_CASE("Path", "[ast.path]") {
    REQUIRE_AST("a", "a");
    REQUIRE_AST("a.b", "(. a b)");
    REQUIRE_AST("a.b.c", "(. (. a b) c)");
}

TEST_CASE("Wildcard", "[ast.wildcard]") {
    REQUIRE_AST("*", "*");
    REQUIRE_AST("**", "**");
    REQUIRE_AST("**.a", "(. ** a)");
    REQUIRE_AST("a.**.b", "(. (. a **) b)");
    REQUIRE_AST("a.**.b.**.c", "(. (. (. (. a **) b) **) c)");

    REQUIRE_AST("* == *", "(== * *)");     /* Do not optimize away */
    REQUIRE_AST("** == **", "(== ** **)"); /* Do not optimize away */
}

TEST_CASE("OperatorConst", "[ast.operator]") {
    /* Arithmetic */
    REQUIRE_AST("-1",  "-1");
    REQUIRE_AST("1+2", "3");
    REQUIRE_AST("1-2", "-1");
    REQUIRE_AST("2*2", "4");
    REQUIRE_AST("8/2", "4");
    REQUIRE_AST("-a",  "(- a)");
    REQUIRE_AST("a+2", "(+ a 2)");
    REQUIRE_AST("2+a", "(+ 2 a)");
    REQUIRE_AST("a+b", "(+ a b)");
    REQUIRE_AST("1+null", "null");
    REQUIRE_AST("null+1", "null");
    REQUIRE_AST("1*null", "null");
    REQUIRE_AST("null*1", "null");
    REQUIRE_AST("1-null", "null");
    REQUIRE_AST("null-1", "null");
    REQUIRE_AST("1/null", "null");
    REQUIRE_AST("null/1", "null");

    /* Division by zero */
    CHECK_THROWS(getASTString("1/0"));
    CHECK_THROWS(getASTString("1%0"));

    /* String */
    REQUIRE_AST("'a'+null", "\"anull\"");
    REQUIRE_AST("null+'a'", "\"nulla\"");

    /* Comparison */
    REQUIRE_AST("1==1", "true");
    REQUIRE_AST("1!=1", "false");

    REQUIRE_AST("2>1",  "true");
    REQUIRE_AST("1<2", "true");
    REQUIRE_AST("2<1", "false");
    REQUIRE_AST("2<=2", "true");
    REQUIRE_AST("2<=1", "false");
    REQUIRE_AST("1<=1.1", "true");

    /* Null behaviour */
    REQUIRE_AST("1<null", "false");
    REQUIRE_AST("1>null", "false");
    REQUIRE_AST("null<1", "false");
    REQUIRE_AST("null>1", "false");
    REQUIRE_AST("null<null", "false");
    REQUIRE_AST("null>null", "false");
    REQUIRE_AST("null==null", "true");

    /* Typeof */
    REQUIRE_AST("typeof 'abc'", "\"string\"");
    REQUIRE_AST("typeof 123",   "\"int\"");
    REQUIRE_AST("typeof 123.1", "\"float\"");
    REQUIRE_AST("typeof true",  "\"bool\"");
    REQUIRE_AST("typeof null",  "\"null\"");

    /* Precedence */
    REQUIRE_AST("1+1*2",   "3");
    REQUIRE_AST("(1+1)*2", "4");
    REQUIRE_AST("2*1+1",   "3");
    REQUIRE_AST("2*(1+1)", "4");

    /* Casts */
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

TEST_CASE("CompareIncompatibleTypesFields", "[ast.compare-incompatible-types-fields]") {
    const char* const doc = R"json(
        [
            {"field": 1, "another": 1.5},
            {"field": "text"},
            {"field": true, "another": false}
        ]
    )json";

    const auto model = simfil::json::parse(doc);
    auto test = [&model](auto query) {
        Environment env(model->strings());

        auto ast = compile(env, query, false);
        INFO("AST: " << ast->toString());

        return eval(env, *ast, *model->root(0)).front().template as<ValueType::Bool>();
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

TEST_CASE("OperatorAndOr", "[ast.operator-and-or]") {
    /* Or */
    REQUIRE_AST("null or 1", "1");
    REQUIRE_AST("1 or null", "1");
    REQUIRE_AST("1 or 2",    "1");
    REQUIRE_AST("a or b",    "(or a b)");

    /* And */
    REQUIRE_AST("null and 1", "null");
    REQUIRE_AST("1 and null", "null");
    REQUIRE_AST("1 and 2",    "2");
    REQUIRE_AST("a and b",    "(and a b)");
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
    REQUIRE_AST("range(a,b)",    "(range a b)"); /* Can not optimize */
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
    REQUIRE_AST("TRACE(1)",      "(trace 1)");
    REQUIRE_AST("Trace(1)",      "(trace 1)");
}

static const char* const doc = R"json(
{
  "a": 1,
  "b": 2,
  "c": ["a", "b", "c"],
  "d": [0, 1, 2],
  "sub": {
    "a": "sub a",
    "b": "sub b",
    "sub": {
      "a": "sub sub a",
      "b": "sub sub b"
    }
  },
  "geoPoint": {
    "geometry": {
      "type": "Point",
      "coordinates": [1, 2]
    }
  },
  "geoLineString": {
    "geometry": {
      "type": "LineString",
      "coordinates": [[1, 2], [3, 4]]
    }
  },
  "geoPolygon": {
    "geometry": {
      "type": "Polygon",
      "coordinates": [[[1, 2], [3, 4], [5, 6]]]
    }
  }
}
)json";

static auto joined_result(std::string_view query)
{
    auto model = simfil::json::parse(doc);
    Environment env(model->strings());

    auto ast = compile(env, query, false);
    INFO("AST: " << ast->toString());

    auto res = eval(env, *ast, *model->root(0));

    std::string vals;
    for (const auto& vv : res) {
        if (!vals.empty())
            vals.push_back('|');
        vals += vv.toString();
    }
    return vals;
}

#define REQUIRE_RESULT(query, result) \
    REQUIRE(joined_result(query) == result)

TEST_CASE("Path Wildcard", "[yaml.path-wildcard]") {
    REQUIRE_RESULT("sub.*", R"(sub a|sub b|{"a":"sub sub a","b":"sub sub b"})");
    REQUIRE_RESULT("sub.**", R"({"a":"sub a","b":"sub b","sub":{"a":"sub sub a","b":"sub sub b"}}|sub a|sub b|)"
                             R"({"a":"sub sub a","b":"sub sub b"}|sub sub a|sub sub b)");
    REQUIRE_RESULT("(sub.*.{typeof _ != 'model'} + sub.*.{typeof _ != 'model'})._", "sub asub a|sub asub b|sub bsub a|sub bsub b"); /* . filters null */
    REQUIRE_RESULT("sub.*.{typeof _ != 'model'} + sub.*.{typeof _ != 'model'}", "sub asub a|sub asub b|sub bsub a|sub bsub b"); /* {_} filters null */
    REQUIRE_RESULT("count(*)", "8");
    REQUIRE_RESULT("count(**)", "47");
    REQUIRE_RESULT("count(sub.**.a)", "2");
    REQUIRE_RESULT("count(**.{typeof _ == 'string'})", "10");
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

    auto json = R"({"a":1,"b":2,"c":["a","b","c"],"d":[0,1,2],"geoLineString":{"geometry":{"coordinates":[[1,2],[3,4]],"type":"LineString"}},"geoPoint":{"geometry":{"coordinates":[1,2],"type":"Point"}},"geoPolygon":{"geometry":{"coordinates":[[[1,2],[3,4],[5,6]]],"type":"Polygon"}},"sub":{"a":"sub a","b":"sub b","sub":{"a":"sub sub a","b":"sub sub b"}}})";
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

TEST_CASE("Model Functions", "[yaml.mode-functions]") {
    SECTION("Test any(... ) with values generated by arr(...)") {
        REQUIRE_RESULT("any(arr(null, null))", "false");
        REQUIRE_RESULT("any(arr(true, null))", "true");
        REQUIRE_RESULT("any(arr(null, true))", "true");
        REQUIRE_RESULT("any(arr(true, true))", "true");
    }
    SECTION("Test each(... ) with values generated by arr(...)") {
        REQUIRE_RESULT("each(arr(null, null))", "false");
        REQUIRE_RESULT("each(arr(true, null))", "false");
        REQUIRE_RESULT("each(arr(null, true))", "false");
        REQUIRE_RESULT("each(arr(true, true))", "true");
    }
    SECTION("Test arr(... )") {
        REQUIRE_RESULT("arr(2,3,5,7,'ok')", "2|3|5|7|ok");
    }
    SECTION("Test split(... )") {
        REQUIRE_RESULT("split('hello.this.is.a.test.', '.')", "hello|this|is|a|test|");
        REQUIRE_RESULT("split('hello.this.is.a.test.', '.', false)", "hello|this|is|a|test");
    }
    SECTION("Test select(... )") {
        REQUIRE_RESULT("select(split('a.b.c.d', '.'), a)", "b");
        REQUIRE_RESULT("select(split('a.b.c.d', '.'), 0)", "a");
        REQUIRE_RESULT("select(split('a.b.c.d', '.'), 1, 2)", "b|c");
        REQUIRE_RESULT("select(split('a.b.c.d', '.'), 1, 0)", "b|c|d");
    }
    SECTION("Test sum(... )") {
        REQUIRE_RESULT("sum(range(1, 10)...)", "55");
        REQUIRE_RESULT("sum(range(1, 10)..., $sum + $val)", "55");
        REQUIRE_RESULT("sum(range(1, 10)..., $sum + $val, 10)", "65");
        REQUIRE_RESULT("sum(range(1, 10)..., $sum * $val, 1)", "3628800");
    }
    SECTION("Count non-false values generated by arr(...)") {
        REQUIRE_RESULT("count(arr(null, null))", "0");
        REQUIRE_RESULT("count(arr(true, null))", "1");
        REQUIRE_RESULT("count(arr(null, true))", "1");
        REQUIRE_RESULT("count(arr(true, true))", "2");
    }
}

TEST_CASE("Sub-Selects", "[yaml.sub-selects]") {
    SECTION("Filter out null values") {
        REQUIRE_RESULT("count(** as int)", "47"); /* Unfiltered */
        REQUIRE_RESULT("count(**{typeof _ != 'null' and typeof _ != 'model'})", "27"); /* Filtered */
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
    pool->newObject()->addField("good", ModelNode::Ptr::make(pool, ModelNodeAddress{ModelPool::Objects, 666}));
    REQUIRE_THROWS(pool->validate());

    // Recognize dangling array member pointer
    pool->clear();
    pool->newObject()->addField("good", ModelNode::Ptr::make(pool, ModelNodeAddress{ModelPool::Arrays, 666}));
    REQUIRE_THROWS(pool->validate());

    // Recognize dangling root
    pool->clear();
    pool->addRoot(ModelNode::Ptr::make(pool, ModelNodeAddress{ModelPool::Objects, 666}));
    REQUIRE_THROWS(pool->validate());

    // An empty model should be valid
    pool->clear();
    REQUIRE_NOTHROW(pool->validate());

    // An empty object should also be valid
    pool->newObject()->addField("good", pool->newObject());
    REQUIRE_NOTHROW(pool->validate());
}

TEST_CASE("Procedural Object Node", "[model.procedural]") {
    auto pool = std::make_shared<ModelPool>();
    pool->strings()->addStaticKey(StaticTestKey, "test");

    struct DerivedProceduralObject : public ProceduralObject<2, DerivedProceduralObject> {
        DerivedProceduralObject(ModelConstPtr pool, ModelNodeAddress a)
            : ProceduralObject<2, DerivedProceduralObject>((ArrayIndex)a.index(), std::move(pool), a)
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

        REQUIRE(testObjectA->size() == 2);
        REQUIRE(testObjectB->size() == 2);
        REQUIRE(Value(testObjectA->get("name")->value()).toString() == "hans");
        REQUIRE(Value(testObjectA->get("occupation")->value()).toString() == "baker");
        REQUIRE(!testObjectA->get("height"));
        REQUIRE(!testObjectA->get("age"));
        testObjectA->extend(testObjectB);

        REQUIRE(testObjectA->size() == 4);
        REQUIRE(testObjectB->size() == 2);
        REQUIRE(Value(testObjectA->get("name")->value()).toString() == "hans");
        REQUIRE(Value(testObjectA->get("occupation")->value()).toString() == "baker");
        REQUIRE(Value(testObjectA->get("height")->value()).as<ValueType::Int>() == 220ll);
        REQUIRE(Value(testObjectA->get("age")->value()).as<ValueType::Int>() == 55ll);
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
