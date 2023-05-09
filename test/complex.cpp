#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <sstream>
#include <iostream>

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
    Environment env(model->fieldNames());
    auto ast = compile(env, query, false);
    INFO("AST: " << ast->toString());

    auto res = eval(env, *ast, *model);

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

TEST_CASE("Invoice", "[yaml.complex.invoice-sum]") {
    REQUIRE_RESULT("account.order.*.product.*.(price * quantity)",
                            "68.900000|21.670000|137.800000|107.990000");
    REQUIRE_RESULT("sum(account.order.*.product.*.(price * quantity))",
                            "336.360000");
    REQUIRE_RESULT("**.(price * quantity)",
                            "68.900000|21.670000|137.800000|107.990000");
    REQUIRE_RESULT("sum(**.(price * quantity))",
                            "336.360000");
}

TEST_CASE("Runtime Error", "[yaml.complex.runtime-error]") {
    REQUIRE_THROWS(joined_result("1 / (nonexisting as int)")); /* Division by zero */
    REQUIRE_THROWS(joined_result("not nonexisting == 0"));     /* Invalid operands int and bool */
    REQUIRE_THROWS(joined_result("not *.nonexisting == 0"));   /* Invalid operands int and bool */
}

TEST_CASE("Serialization", "[yaml.complex.serialization]") {
    auto model = json::parse(invoice);

    std::stringstream serializedModel;
    model->write(serializedModel);

    auto buf = serializedModel.str();
    std::cout << "Serialized model: " << buf.size() << " bytes:" <<  std::endl;
    for (auto chIdx = 0; chIdx < buf.size(); ++chIdx)
        std::cout << "- at " << chIdx << ": " << (uint32_t)(uint8_t)buf[chIdx] << std::endl;


    auto recoveredModel = std::make_shared<ModelPool>();
    recoveredModel->read(serializedModel);

    std::function<void(ModelNode::Ptr,ModelNode::Ptr)> require_equals = [&] (auto l, auto r) {
        REQUIRE(l->type() == r->type());
        switch(l->type()) {
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
        default:
            REQUIRE(l->value() == r->value());
        }
    };

    REQUIRE(model->numRoots() == recoveredModel->numRoots());
    require_equals(model->root(0), recoveredModel->root(0));
}
