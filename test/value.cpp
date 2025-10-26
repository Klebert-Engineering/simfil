#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <cstdint>

#include "simfil/value.h"
#include "simfil/model/model.h"
#include "simfil/transient.h"

using namespace simfil;
using namespace std::string_literals;

TEST_CASE("Value Constructors", "[value.value-constructor]") {
    SECTION("Boolean") {
        auto trueVal = Value::t();
        REQUIRE(trueVal.isa(ValueType::Bool));
        REQUIRE(trueVal.asBool() == true);
        
        auto falseVal = Value::f();
        REQUIRE(falseVal.isa(ValueType::Bool));
        REQUIRE(falseVal.asBool() == false);
    }
    
    SECTION("Null") {
        auto nullVal = Value::null();
        REQUIRE(nullVal.isa(ValueType::Null));
    }
    
    SECTION("Undef") {
        auto undefVal = Value::undef();
        REQUIRE(undefVal.isa(ValueType::Undef));
    }
    
    SECTION("Model") {
        auto modelVal = Value::model();
        REQUIRE(modelVal.isa(ValueType::Object));
    }
    
    SECTION("StrRef") {
        auto strVal = Value::strref("test");
        REQUIRE(strVal.isa(ValueType::String));
        REQUIRE(strVal.as<ValueType::String>() == "test");
    }

    SECTION("Make Bool") {
        auto val = Value::make(true);
        REQUIRE(val.isa(ValueType::Bool));
        REQUIRE(val.asBool() == true);

        auto val2 = Value::make(false);
        REQUIRE(val2.isa(ValueType::Bool));
        REQUIRE(val2.asBool() == false);
    }

    SECTION("Make int64_t") {
        auto val = Value::make(int64_t(42));
        REQUIRE(val.isa(ValueType::Int));
        REQUIRE(val.as<ValueType::Int>() == 42);
    }

    SECTION("Make double") {
        auto val = Value::make(3.14);
        REQUIRE(val.isa(ValueType::Float));
        REQUIRE(val.as<ValueType::Float>() == Catch::Approx(3.14));
    }

    SECTION("Make string") {
        auto val = Value::make("hello"s);
        REQUIRE(val.isa(ValueType::String));
        REQUIRE(val.as<ValueType::String>() == "hello");
    }

    SECTION("Make string_view") {
        std::string_view sv = "world";
        auto val = Value::make(sv);
        REQUIRE(val.isa(ValueType::String));
        REQUIRE(val.as<ValueType::String>() == "world");
    }

    SECTION("Type constructor") {
        Value val(ValueType::Null);
        REQUIRE(val.isa(ValueType::Null));
    }

    SECTION("Type and value constructor") {
        Value val(ValueType::Int, int64_t(123));
        REQUIRE(val.isa(ValueType::Int));
        REQUIRE(val.as<ValueType::Int>() == 123);
    }

    SECTION("ScalarValueType constructor") {
        ScalarValueType scalar = (int64_t)123;
        Value val(std::move(scalar));
        REQUIRE(val.isa(ValueType::Int));
        REQUIRE(val.as<ValueType::Int>() == 123);

        ScalarValueType scalar2 = "Brainstorm"s;
        Value val2(std::move(scalar2));
        REQUIRE(val2.isa(ValueType::String));
        REQUIRE(val2.as<ValueType::String>() == "Brainstorm");
    }

    SECTION("Copy & Move constructor") {
        Value original = Value::make<int64_t>(123);

        Value copied = original;
        REQUIRE(copied.isa(ValueType::Int));
        REQUIRE(copied.as<ValueType::Int>() == 123);
        REQUIRE(original.isa(ValueType::Int));
        REQUIRE(original.as<ValueType::Int>() == 123);

        Value moved = std::move(original);
        REQUIRE(moved.isa(ValueType::Int));
        REQUIRE(moved.as<ValueType::Int>() == 123);
    }
}

TEST_CASE("Value field() methods", "[value.field]") {
    auto model = std::make_shared<ModelPool>();
    
    SECTION("field() from scalar ModelNode") {
        auto intNode = model->newValue(int64_t(42));
        auto val = Value::field(*intNode);
        REQUIRE(val.isa(ValueType::Int));
        REQUIRE(val.as<ValueType::Int>() == 42);
        
        auto strNode = model->newValue("test"s);
        auto val2 = Value::field(*strNode);
        REQUIRE(val2.isa(ValueType::String));
        REQUIRE(val2.as<ValueType::String>() == "test");
    }
    
    SECTION("field() from object ModelNode") {
        auto objNode = model->newObject(1);
        objNode->addField("key", model->newValue(int64_t(123)));
        
        auto val = Value::field(*objNode);
        REQUIRE(val.isa(ValueType::Object));
        REQUIRE(val.node() != nullptr);
    }
    
    SECTION("field() from array ModelNode") {
        auto arrNode = model->newArray(2);
        arrNode->append(model->newValue(int64_t(1)));
        arrNode->append(model->newValue(int64_t(2)));
        
        auto val = Value::field(*arrNode);
        REQUIRE(val.isa(ValueType::Array));
        REQUIRE(val.node() != nullptr);
    }
    
    SECTION("field() from model_ptr") {
        auto nodePtr = model->newValue((int64_t)88);
        auto val = Value::field(nodePtr);
        REQUIRE(val.isa(ValueType::Int));
        REQUIRE(val.as<ValueType::Int>() == 88);
    }
}

TEST_CASE("Value As", "[value.as]") {
    SECTION("as<ValueType::Int>()") {
        auto val = Value::make(int64_t(42));
        REQUIRE(val.as<ValueType::Int>() == 42);
    }
    
    SECTION("as<ValueType::Float>()") {
        auto val = Value::make(3.14);
        REQUIRE(val.as<ValueType::Float>() == Catch::Approx(3.14));
    }
    
    SECTION("as<ValueType::String>() from string") {
        auto val = Value::make("hello"s);
        REQUIRE(val.as<ValueType::String>() == "hello");
    }
    
    SECTION("as<ValueType::String>() from string_view") {
        auto val = Value::strref("world");
        REQUIRE(val.as<ValueType::String>() == "world");
    }
    
    SECTION("as<ValueType::Object>()") {
        auto model = std::make_shared<ModelPool>();
        auto objNode = model->newObject(0);
        auto val = Value::field(objNode);
        auto ptr = val.as<ValueType::Object>();
        REQUIRE(!!ptr);
    }
    
    SECTION("as<ValueType::Array>()") {
        auto model = std::make_shared<ModelPool>();
        auto arrNode = model->newArray(0);
        auto val = Value::field(arrNode);
        auto ptr = val.as<ValueType::Array>();
        REQUIRE(!!ptr);
    }
}

TEST_CASE("Value visit() method", "[value.visit]") {
    SECTION("Visit UndefinedType") {
        auto val = Value::undef();
        auto result = val.visit([](const auto& v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, UndefinedType>) {
                return "undefined";
            }
            return "other";
        });
        REQUIRE(result == "undefined")
;
    }
    
    SECTION("Visit NullType") {
        auto val = Value::null();
        auto result = val.visit([](const auto& v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, NullType>) {
                return "null";
            }
            return "other";
        });
        REQUIRE(result == "null");
    }
    
    SECTION("Visit bool") {
        auto val = Value::t()
;
        auto result = val.visit([](const auto& v) -> bool {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, bool>) {
                return v;
            }
            return false;
        });
        REQUIRE(result == true);
    }
    
    SECTION("Visit int64_t") {
        auto val = Value::make(int64_t(42));
        auto result = val.visit([](const auto& v) -> int64_t {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, int64_t>) {
                return v;
            }
            return 0;
        });
        REQUIRE(result == 42);
    }
    
    SECTION("Visit double") {
        auto val = Value::make(3.14);
        auto result = val.visit([](const auto& v) -> double {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, double>) {
                return v;
            }
            return 0.0;
        });
        REQUIRE(result == Catch::Approx(3.14));
    }
    
    SECTION("Visit string") {
        auto val = Value::make("Preordain"s);
        auto result = val.visit([](const auto& v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::string>) {
                return v;
            }
            return "";
        });
        REQUIRE(result == "Preordain");
    }
    
    SECTION("Visit ModelNode with valid pointer") {
        auto model = std::make_shared<ModelPool>();
        auto objNode = model->newObject(0);
        auto val = Value::field(objNode);
        
        auto result = val.visit([](const auto& v) -> bool {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, ModelNode>) {
                return true;
            } else if constexpr (std::is_same_v<T, NullType>) {
                return false;
            }
            return false;
        });
        REQUIRE(result);
    }
    
    SECTION("Visit ModelNode with null pointer") {
        Value val(ValueType::Object);
        auto result = val.visit([](const auto& v) -> bool {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, NullType>) {
                return true;
            }
            return false;
        });
        REQUIRE(result);
    }
}

TEST_CASE("Value toString() method", "[value.toString]") {
    REQUIRE(Value::undef().toString() == "undef");
    REQUIRE(Value::null().toString() == "null");
    REQUIRE(Value::t().toString() == "true");
    REQUIRE(Value::f().toString() == "false");
    REQUIRE(Value::make(int64_t(123)).toString() == "123");
    REQUIRE(Value::make(int64_t(-123)).toString() == "-123");
    REQUIRE(Value::make(double(3.14)).toString().find("3.14") == 0);
    REQUIRE(Value::make("Ponder"s).toString() == "Ponder");
}

TEST_CASE("Value utility methods", "[value.utilities]") {
    SECTION("getScalar() method") {
        auto intVal = Value::make(int64_t(123));
        auto scalar = intVal.getScalar();
        REQUIRE(std::holds_alternative<int64_t>(scalar));
        REQUIRE(std::get<int64_t>(scalar) == 123);
        
        auto strVal = Value::make("test"s);
        auto scalar2 = strVal.getScalar();
        REQUIRE(std::holds_alternative<std::string>(scalar2));
        REQUIRE(std::get<std::string>(scalar2) == "test");
    }
    
    SECTION("stringViewValue() method") {
        auto strVal = Value::strref("Snap");
        auto svPtr = strVal.stringViewValue();
        REQUIRE(svPtr != nullptr);
        REQUIRE(*svPtr == "Snap");
        
        auto intVal = Value::make<int64_t>(123);
        auto svPtr2 = intVal.stringViewValue();
        REQUIRE(svPtr2 == nullptr); // No string_view pointer
    }
    
    SECTION("nodePtr() method") {
        auto model = std::make_shared<ModelPool>();
        auto objNode = model->newObject(0);
        auto val = Value::field(objNode);
        REQUIRE(val.nodePtr() != nullptr);

        REQUIRE(Value::make<int64_t>(123).nodePtr() == nullptr);
    }
    
    SECTION("node() method") {
        auto model = std::make_shared<ModelPool>();
        auto objNode = model->newObject(0);
        auto val = Value::field(objNode);
        REQUIRE(val.node() != nullptr);
        
        REQUIRE(Value::make<int64_t>(42).node() == nullptr);
    }
}

TEST_CASE("valueType2String() function", "[value.type2string]") {
    REQUIRE(valueType2String(ValueType::Undef) == "undef"s);
    REQUIRE(valueType2String(ValueType::Null) == "null"s);
    REQUIRE(valueType2String(ValueType::Bool) == "bool"s);
    REQUIRE(valueType2String(ValueType::Int) == "int"s);
    REQUIRE(valueType2String(ValueType::Float) == "float"s);
    REQUIRE(valueType2String(ValueType::String) == "string"s);
    REQUIRE(valueType2String(ValueType::TransientObject) == "transient"s);
    REQUIRE(valueType2String(ValueType::Object) == "object"s);
    REQUIRE(valueType2String(ValueType::Array) == "array"s);
}

TEST_CASE("getNumeric() function", "[value.numeric]") {
    SECTION("getNumeric from ith int") {
        auto val = Value::make(int64_t(123));
        auto [ok, result] = getNumeric<double>(val);
        REQUIRE(ok);
        REQUIRE(result == Catch::Approx(123.0));
    }
    
    SECTION("getNumeric from float") {
        auto val = Value::make(3.14);
        auto [ok, result] = getNumeric<double>(val);
        REQUIRE(ok);
        REQUIRE(result == Catch::Approx(3.14));
    }
    
    SECTION("getNumeric from non-numeric") {
        auto val = Value::make("Cloudpost"s);
        auto [ok, result] = getNumeric<double>(val);
        REQUIRE_FALSE(ok);
    }
    
    SECTION("getNumeric from double") {
        ScalarValueType scalar = 1.5;
        auto [ok, result] = getNumeric<float>(scalar);
        REQUIRE(ok);
        REQUIRE(result == Catch::Approx(1.5f));
    }
    
    SECTION("getNumeric from int64_t") {
        ScalarValueType scalar = int64_t(123);
        auto [ok, result] = getNumeric<int>(scalar);
        REQUIRE(ok);
        REQUIRE(result == 123);
    }
}
