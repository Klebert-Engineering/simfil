#include "simfil/simfil.h"
#include "simfil/model.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/benchmark/catch_benchmark.hpp>
#include <vector>
#include <cstdint>

using namespace simfil;
using FieldList = std::vector<std::pair<std::string,
    ModelPool::ModelNodeIndex>>;

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

template <class _Gen>
static auto generate_object(ModelPool& model, _Gen&& gen)
{
    std::vector<ModelPool::Member> members;
    for (auto&& [key, value] : gen(model)) {
        members.emplace_back(model.strings->emplace(key), value);
    }
    return model.addObject(members);
}

template <class _Value>
static auto generate_value(ModelPool& model, _Value&& value)
{
    return model.addValue(std::forward<_Value>(value));
}

static auto rand_num(ModelPool& model)
{
    return generate_value<int64_t>(model, rand());
}

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

static auto joined_result(const ModelPool& model, std::string_view query)
{
    Environment env(model.strings);
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

TEST_CASE("Big Model (500)", "[perf.big-model]") {
    const auto model = generate_model(500);

    BENCHMARK("Query typeof Recursive") {
        return joined_result(*model, "count(typeof ** == 'notatype')").size(); /* Return something to prevent optimization */ 
    };

    BENCHMARK("Query field id Recursive") {
        return joined_result(*model, "count(**.id == 250)").size();
    };

    BENCHMARK("Query field id") {
        return joined_result(*model, "count(id == 250)").size();
    };
}
