#include "simfil/simfil.h"
#include "simfil/model.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/benchmark/catch_benchmark.hpp>
#include <vector>
#include <cstdint>

#if __has_include(<valgrind/callgrind.h>)
#    include <valgrind/callgrind.h>
#else
#    define RUNNING_ON_VALGRIND false
#    define CALLGRIND_START_INSTRUMENTATION (void)0
#    define CALLGRIND_STOP_INSTRUMENTATION (void)0
#endif


using namespace simfil;
using FieldList = std::vector<std::pair<std::string,
    ModelPool::ModelNodeIndex>>;

/* Generate array with `gen` items. */
template <class _Gen>
static auto generate_array(ModelPool& model, _Gen&& gen)
{
    auto values = gen(model);
    std::vector<ModelPool::Member> items;
    items.reserve(values.size());
    for (auto&& value : std::move(values)) {
        items.emplace_back(Strings::Empty, value);
    }
    return model.addArray(items);
}

/* Generate array of `gen` with `n` items. */
template <class _Gen>
static auto generate_n(ModelPool& model, std::size_t n, _Gen&& gen)
{
    std::vector<ModelPool::ModelNodeIndex> items;
    items.reserve(n);
    for (auto i = 0u; i < n; ++i)
        items.emplace_back(gen(model));
    return generate_array(model, [i = std::move(items)](auto&) -> auto {
        return i;
    });
}

/* Generate object with <key, value> `gen` fields. */
template <class _Gen>
static auto generate_object(ModelPool& model, _Gen&& gen)
{
    std::vector<ModelPool::Member> members;
    for (auto&& [key, value] : gen(model)) {
        members.emplace_back(model.strings->emplace(key), value);
    }
    return model.addObject(members);
}

/* Generate single value. */
template <class _Value>
static auto generate_value(ModelPool& model, _Value&& value)
{
    return model.addValue(std::forward<_Value>(value));
}

/* Generate random integer value. */
static auto rand_num(ModelPool& model)
{
    return generate_value<int64_t>(model, rand());
}

/* Generate object tree with depth `_Size`.
 * - Leaf nodes are set to random integer values
 * - Keys are "SUB_%d" with '%d' being the field index
 */
template <std::size_t _HeadSize, std::size_t... _Size>
static auto generate_sub_tree_n(ModelPool& m)
{
    if (_HeadSize <= 0) {
        return rand_num(m);
    }

    return generate_object(m, [&](auto& m) {
        FieldList fields;
        for (auto j = 0; j < _HeadSize; ++j) {
            fields.emplace_back(std::string("SUB_") + std::to_string(j), generate_sub_tree_n<_Size..., 0>(m));
        }
        return fields;
    });
}

/* Generate `n` test model objects. */
static auto generate_model(std::size_t n)
{
    auto model = std::make_shared<simfil::ModelPool>();
    model->addRoot(generate_n(*model, n, [&, i = 0](auto& m) mutable {
        ++i;
        return generate_object(m, [&](auto& m) {
            FieldList fields;
            fields.emplace_back("id", generate_value<int64_t>(m, i));
            fields.emplace_back("type", rand_num(m));
            fields.emplace_back("value", rand_num(m));
            fields.emplace_back("properties", generate_sub_tree_n<5, 2, 1, 1, 3>(m));
            fields.emplace_back("numbers", generate_n(m, 50, [](auto& m) { return rand_num(m); }));
            return fields;
        });
    }));
    model->validate();
    return model;
}

static auto result(const ModelPool& model, std::string_view query)
{
    Environment env(model.strings);
    auto ast = compile(env, query, false);
    INFO("AST: " << ast->toString());
    return eval(env, *ast, model);
}

static auto joined_result(const ModelPool& model, std::string_view query)
{
    auto res = result(model, query);

    std::string vals;
    for (const auto& vv : res) {
        if (!vals.empty())
            vals.push_back('|');
        vals += vv.toString();
    }
    return vals;
}

TEST_CASE("Small model performance queries", "[perf.big-model-benchmark]") {
    if (RUNNING_ON_VALGRIND) {
        SKIP("Skipping benchmarks when running under valgrind");
    }

    const auto model = generate_model(1000);

    BENCHMARK("Query typeof Recursive") {
        return result(*model, "count(typeof ** == 'notatype')");
    };

    BENCHMARK("Query field id Recursive") {
        return result(*model, "count(**.id == 25)");
    };

    BENCHMARK("Query field id") {
        return result(*model, "count(*.id == 25)");
    };
}

#define REQUIRE_RESULT(query, result) \
    REQUIRE(joined_result(*model, query) == (result))

TEST_CASE("Big model queries", "[perf.big-model-queries]") {
    const auto model = generate_model(1000);

    REQUIRE_RESULT("count(typeof ** == 'notatype')", "0");
    REQUIRE_RESULT("count(**.id == 250)", "1");
    REQUIRE_RESULT("count(*.id == 250)", "1");
}
