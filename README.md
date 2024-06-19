# `simfil`

[![](https://img.shields.io/badge/Coverage-HTML-orange)](https://htmlpreview.github.io/?https://gist.githubusercontent.com/johannes-wolf/61e57af50757b03e0c7cd119ec2d2f4b/raw/ed28c457ebc09ce8ddddc9cec6668e130d59b64c/coverage.html)
[![](https://gist.githubusercontent.com/johannes-wolf/61e57af50757b03e0c7cd119ec2d2f4b/raw/0ae49c7509dea18b4c110b8bf416f2715a214933/badge.svg)](https://github.com/Klebert-Engineering/simfil)
[![Conan Center](https://img.shields.io/conan/v/simfil)](https://conan.io/center/recipes/simfil)

`simfil` is a C++ 17 library and a language for querying structured map feature data. The library provides an efficient in-memory storage pool for map data, optimized for the `simfil` query language, along with a query interpreter to query the actual data.

Although `simfil` is made for querying map feature data, it is easy to plug in custom data models, such as the generic JSON interface the library also comes with.

## Design Considerations

- **Simplicity**: the language should be very simple but yet powerful to allow for complex queries.
- **Speed**: querying models should be fast and scale to multiple cores.
- **Efficiency**: the internal model should be memory efficient, as `simfil` is designed to query huge amounts of data.

## Query Language

For details about the language see the [Language Guide](simfil-language.md).

### Examples

All examples shown below can be executed by loading the json file [`examples/example.json`](examples/example.json) using the interactive command line tool `<builddir>/repl/simfil-repl INPUT` (see [Using the Interactive Command Line Tool](#Using the Interactive Command Line Tool)).

#### All person's names

```
*.name
```
```
John
Angelika
Franz
```

#### All persons older than 30 years using a [subquery](simfil-language.md#sub-queries)
Subqueries, denoted by braces `{...}` can be used as predicates for the current object. Insides a subquery, the current value is accessible through the special name `_`.
```
*.{age > 30}.name
```
```
Angelika
Franz
```

#### Combining name and surname constructing a temporary value using string [concatenation](simfil-language.md#operators)
```
*.(name + " " + surname)
```
```
John Doe
Angelika Musterfrau
Franz Eder
```

#### All persons with attribute "A" > 0 using nested subqueries
```
*.{attribs.*.{id == "A" and value > 0}}.name
```
```
Franz
```

#### Find Primes in Range 1 to 25
Some types have special operators. The type `irange` supports the unpack operator `...` to expand its list of values.
```
range(1,25)...{count((_ % range(1,_)...) == 0) == 2}
```
```
2, 3, 5, 7, 11, 13, 17, 19, 23
```

## Building the Project
`simfil` uses CMake as build system and can be built using all three major compilers, GCC, Clang and MSVC. Dependencies outsides the repository are automatically downloaded using either CMakes `FetchContent` system or [Conan](https://conan.io).

### With Conan
```sh
conan install . --build missing
cmake --preset conan-release -DSIMFIL_WITH_TESTS=ON -DSIMFIL_WITH_REPL=ON
cmake --build --preset conan-release
ctest --preset conan-release
```

### With FetchContent
```sh
mkdir -p build && cd build
cmake .. -DSIMFIL_WITH_TESTS=ON -DSIMFIL_WITH_REPL=ON && cmake --build .
ctest
```

## Using the Interactive Command Line Tool
The project contains an interactive command line program (repl: “Read-Eval-Print-Loop”) to to test queries against a JSON datasource: `simfil-repl`.

```sh
<build-dir>/repl/simfil-repl <json-file>
```

All of the example queries above can be tested by loading [example.json](examples/example.json):
```sh
<buil-dir>/repl/simfil-repl <worktree>/examples/example.json
```

The repl provides some extra commands for testing queries:
- `/any` Toggle wrapping input with an `any(...)` call to only get boolean results (default `off`)
- `/mt` Toggle multithreading (default `on`)
- `/verbose` Toggle verbosity (default `on`)

## Extending the Language
The query language can be extended by additional functions and addititonal types.

## Using the Library
### Conan Package
#### Using Conan
Simfil is published on [conan.io](https://conan.io/center/recipes/simfil). All you have to do is to add it to your
conanfile:

``` conan
[requires]
simfil/0.1.1

[generators]
CMakeDeps
CMakeToolchain
```

#### Using Conan Editable Mode
You can link the local simfil source directory as a [Conan 2 editable mode package ](https://docs.conan.io/2/tutorial/developing_packages/editable_packages.html) via `conan editable add <simfil-dir>`. Note that you have to pass
`--build=editable` to your `conan install` invocation, otherwise the CMake build fails with errors about not finding the library.

To use the editable package, just set the version to the one in this
repositories `conanfile.py`, which is `dev`:

```conan
[requires]
simfil/dev

[generators]
CMakeDeps
CMakeToolchain
```

#### Installing the Package Locally
To use this library as a dependency you can install it locally using
`conan create <simfil-dir> --build=missing -s compiler.cppstd=20`, which
exports the package into the local conan registry. Note that for developing simfil it is recomendet to use Conans "editable mode".

Note: Installing locally with simfil registered as an editable package at the same time will fail. You have to first remove the
package from editable mode, the error messages do not give a hint about the conflict!

### CMake FetchContent
To link against `simfil` vial CMake, all you have to do is to add the following to you `CMakeLists.txt`:
```cmake
# Using CMakes FetchContent
FetchContent_Declare(simfil
  GIT_REPOSITORY "https://github.com/Klebert-Engineering/simfil.git"
  GIT_TAG        "main"
  GIT_SHALLOW    ON)
FetchContent_MakeAvailable(simfil)

# Link against the simfil target
target_link_libraries(<my-app> PUBLIC simfil)
```

### Minimal Usage Example
```c++
#include "simfil/simfil.h"
#include "simfil/model/model.h"
#include "simfil/model/fields.h"

// Shared string pool used for string interning
auto strings = std::make_shared<simfil::Fields>();

// Declare a model with one object
auto model = std::make_shared<simfil::ModelPool>(strings);

auto obj = model->newObject();
obj->addField("name", "demo");
model->addRoot(obj);

// Compilation and evaluation environment
// to register custom functions or callbacks.
auto env = simfil::Environment{strings};

// Compile query string to a simfil::Expression.
auto query = simfil::compile(env, "name", false);

// Evalualte query and get result of type simfil::Value.
auto result = simfil::eval(env, *query, *model);

for (auto&& value : result)
    std::cout << value.toString() << "\n";
```

The full source of the example can be found [here](./examples/minimal/main.cpp).

## Dependencies
- [nlohmann/json](https://github.com/nlohmann/json) for JSON model support (switch: `SIMFIL_WITH_MODEL_JSON`, default: `YES`).
- [fraillt/bitsery](https://github.com/fraillt/bitsery) for binary en- and decoding.
- [slavenf/sfl-library](https://github.com/slavenf/sfl-library.git) for small and segmented vector containers.
- [fmtlib/fmt](https://github.com/fmtlib/fmt) string formatting library.
