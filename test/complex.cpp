#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "simfil/simfil.h"
#include "simfil/model/json.h"

using namespace simfil;

static const auto invoice = json::parse(R"json(
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
)json");

static auto joined_result(const ModelNode* model, std::string_view query)
{
    Environment env;
    auto ast = compile(env, query, false);
    INFO("AST: " << ast->toString());

    auto res = eval(env, *ast, model);

    std::string vals;
    for (const auto& vv : res) {
        if (!vals.empty())
            vals.push_back('|');
        vals += vv.toString();
    }
    return vals;
}

#define REQUIRE_RESULT(model, query, result) \
    REQUIRE(joined_result(model->roots[0], query) == result)

TEST_CASE("Invoice", "[yaml.complex.invoice-sum]") {
    REQUIRE_RESULT(invoice, "account.order.*.product.*.(price * quantity)",
                            "68.900000|21.670000|137.800000|107.990000");
    REQUIRE_RESULT(invoice, "sum(account.order.*.product.*.(price * quantity))",
                            "336.360000");
    REQUIRE_RESULT(invoice, "**.(price * quantity)",
                            "68.900000|21.670000|137.800000|107.990000");
    REQUIRE_RESULT(invoice, "sum(**.(price * quantity))",
                            "336.360000");
}

TEST_CASE("Runtime Error", "[yaml.complex.runtime-error]") {
    REQUIRE_THROWS(joined_result(invoice->roots[0], "1 / (nonexisting as int)")); /* Division by zero */
    REQUIRE_THROWS(joined_result(invoice->roots[0], "not nonexisting == 0"));     /* Invalid operands int and bool */
    REQUIRE_THROWS(joined_result(invoice->roots[0], "not *.nonexisting == 0"));   /* Invalid operands int and bool */
}
