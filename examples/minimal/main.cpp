#include <iostream>
#include <memory>

#include "simfil/simfil.h"
#include "simfil/model/model.h"
#include "simfil/model/string-pool.h"

int main()
{
    // Shared string pool.
    auto strings = std::make_shared<simfil::StringPool>();

    // Data model pool
    auto model = std::make_shared<simfil::ModelPool>(strings);

    // Put object into the pool
    auto obj = model->newObject();
    obj->addField("name", "demo");

    // Set object as root
    model->addRoot(obj);

    // Compilation and evaluation environment
    // to register custom functions or callbacks.
    auto env = simfil::Environment{strings};

    // Compile query string to a simfil::Expression.
    auto query = simfil::compile(env, "name", false);

    // Evalualte query and get result of type simfil::Value.
    auto result = simfil::eval(env, *query, *model->root(0));

    for (auto&& value : result)
        std::cout << value.toString() << "\n";
}
