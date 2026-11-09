#include <iostream>
#include <memory>

#include "simfil/simfil.h"
#include "simfil/model/model.h"
#include "simfil/model/string-pool.h"

int main()
{
    // Shared string pool.
    // Model fields and expression evaluation must use the same string-ID namespace.
    auto strings = std::make_shared<simfil::StringPool>();

    // Data model pool
    auto model = std::make_shared<simfil::ModelPool>(strings);

    // Put object into the pool
    auto obj = model->newObject();
    obj->addField("name", "demo");

    // Set object as root.
    // Allocating an object does not publish it as a root automatically.
    model->addRoot(obj);

    // Compilation and evaluation environment
    // to register custom functions or callbacks.
    auto env = simfil::Environment{strings};

    // Compile the query into an owning AST; false keeps the selected value
    // instead of wrapping the query in the default any() predicate.
    auto ast = simfil::compile(env, "name", false);
    if (!ast) {
        std::cerr << ast.error().message << '\n';
        return -1;
    }

    // Evaluate against the published root and collect simfil::Value results.
    auto root = model->root(0);
    if (!root) {
        std::cerr << root.error().message << '\n';
        return -1;
    }

    auto result = simfil::eval(env, **ast, **root, nullptr);
    if (!result) {
        std::cerr << result.error().message << '\n';
        return -1;
    }
    for (auto&& value : result.value())
        std::cout << value.toString() << "\n";
}
