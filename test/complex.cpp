#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <sstream>

#include "simfil/model/model.h"
#include "simfil/model/string-pool.h"
#include "simfil/simfil.h"
#include "simfil/model/json.h"

using namespace simfil;

static const auto invoice = R"json(
{
"account": {
  "name": "Demo",
  "order": [
    {
      "id": "order1",
      "product": [
        {
          "name": "Thing",
          "id": "abc",
          "price": 34.45,
          "quantity": 2
        },
        {
          "name": "Another Thing",
          "id": "xyz",
          "price": 21.67,
          "quantity": 1
        }
      ]
    },
    {
      "id": "order12",
      "product": [
        {
          "name": "Thing",
          "id": "abc",
          "price": 34.45,
          "quantity": 4
        },
        {
          "name": "Something Expensive",
          "id": "xyz",
          "price": 107.99,
          "quantity": 1
        }
      ]
    }
  ]
}
}
)json";

static auto joined_result(std::string_view query)
{
    auto model = json::parse(invoice);
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
    REQUIRE(joined_result(query) == (result))

TEST_CASE("Invoice", "[complex.invoice-sum]") {
    REQUIRE_RESULT("account.order.*.product.*.(price * quantity)",
                            "68.900000|21.670000|137.800000|107.990000");
    REQUIRE_RESULT("sum(account.order.*.product.*.(price * quantity))",
                            "336.360000");
    REQUIRE_RESULT("**.(price * quantity)",
                            "68.900000|21.670000|137.800000|107.990000");
    REQUIRE_RESULT("sum(**.(price * quantity))",
                            "336.360000");
}

TEST_CASE("Regular Expression", "[complex.regexp]") {
    REQUIRE_RESULT("re'a.c' = 'abc'", "abc");
    REQUIRE_RESULT("'abc' = re'a.c'", "abc");
    REQUIRE_RESULT("re'a.x' != 'abc'", "abc");
    REQUIRE_RESULT("'abc' != re'a.x'", "abc");
}

TEST_CASE("Runtime Error", "[complex.runtime-error]") {
    REQUIRE_THROWS(joined_result("1 / (nonexisting as int)")); /* Division by zero */
    REQUIRE_THROWS(joined_result("not nonexisting == 0"));     /* Invalid operands int and bool */
    REQUIRE_THROWS(joined_result("not *.nonexisting == 0"));   /* Invalid operands int and bool */
}

TEST_CASE("Multimap JSON", "[multimap.serialization]") {
    auto model = std::make_shared<simfil::ModelPool>();
    auto root = model->newObject(6);
    model->addRoot(root);

    // Add a single key
    root->addField("a", static_cast<uint16_t>(1));

    // Add a single key multiple times
    root->addField("b", static_cast<uint16_t>(1));
    root->addField("b", static_cast<uint16_t>(2));
    root->addField("b", static_cast<uint16_t>(3));

    // Make sure existing arrays do not get appended to
    auto array = model->newArray(1);
    array->append(static_cast<uint16_t>(1));
    root->addField("c", array);
    root->addField("c", static_cast<uint16_t>(2));

    INFO(model->toJson().dump(2));
    REQUIRE(model->toJson() == nlohmann::json::parse(R"([{"a":[1],"b":[1,2,3],"c":[[1],2],"_multimap":true}])"));
}

TEST_CASE("Serialization", "[complex.serialization]") {
    auto model = json::parse(invoice);

    SECTION("Test Model write/read")
    {
        std::stringstream stream;
        model->write(stream);

        auto recoveredModel = std::make_shared<ModelPool>();
        recoveredModel->read(stream);

        std::function<void(ModelNode::Ptr, ModelNode::Ptr)> require_equals = [&](auto l, auto r)
        {
            REQUIRE(l->type() == r->type());
            switch (l->type()) {
            case ValueType::Object:
            case ValueType::Array: {
                REQUIRE(l->size() == r->size());
                for (auto i = 0; i < l->size(); ++i) {
                    if (l->type() == ValueType::Object)
                        REQUIRE(l->keyAt(i) == r->keyAt(i));
                    require_equals(l->at(i), r->at(i));
                }
                break;
            }
            default: REQUIRE(l->value() == r->value());
            }
        };

        REQUIRE(model->numRoots() == recoveredModel->numRoots());
        require_equals(model->root(0), recoveredModel->root(0));
    }

    SECTION("Test Fields write/read")
    {
        std::stringstream stream;
        model->strings()->write(stream);

        const auto recoveredFields = std::make_shared<StringPool>();
        recoveredFields->read(stream);

        REQUIRE(model->strings()->size() == recoveredFields->size());
        REQUIRE(model->strings()->highest() == recoveredFields->highest());
        REQUIRE(model->strings()->bytes() == recoveredFields->bytes());
        for (StringId sId = 0; sId <= recoveredFields->highest(); ++sId)
            REQUIRE(model->strings()->resolve(sId) == recoveredFields->resolve(StringId(sId)));
    }
}
