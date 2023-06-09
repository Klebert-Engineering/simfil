# SIMFIL

[![](https://img.shields.io/badge/Coverage-HTML-orange)](https://htmlpreview.github.io/?https://gist.githubusercontent.com/johannes-wolf/61e57af50757b03e0c7cd119ec2d2f4b/raw/ed28c457ebc09ce8ddddc9cec6668e130d59b64c/coverage.html)
[![](https://gist.githubusercontent.com/johannes-wolf/61e57af50757b03e0c7cd119ec2d2f4b/raw/0ae49c7509dea18b4c110b8bf416f2715a214933/badge.svg)](https://github.com/Klebert-Engineering/simfil)

Simfil is a language for querying tree structured data.

## Motivation
- **Simplicity**: the language should be very simple but yet powerful to allow for complex queries.
- **Speed**: querying models should be fast and scale to multiple cores.
- **Efficiency**: the internal model should be memory efficient, as simfil is designed to query huge amounts of data.

## Query Language
For details about the language see the [Language Guide](simfil-language.md).

### Examples
All examples shown below can be executed by loading the json file [`examples/example.json`](examples/example.json) using the repl `<builddir>/repl/simfil-repl INPUT`.

#### All persons names
```
*.name
```
```
John
Angelika
Franz
```

#### All persons older than 30 years using a [subquery](simfil-language.md#sub-queries)
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

## Build
```sh
mkdir build && cd build
cmake .. && cmake --build .
```

## Repl
The project contains an interactive command line program to to test queries against a JSON datasource: `simfil-repl`.

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

## Extensions
### Language Extensions
The query language can be extended by additional functions and addititonal types.
An example of how to add functions can be found in the repl: [repl.cpp](repl/repl.cpp).
For an example of how to add new types to simfil, see [ext-geo.h](include/simfil/ext-geo.h).

## Using simfil via CMake
To link against simfil vial CMake, all you have to do is to add the following to you `CMakeLists.txt`:
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

## Dependencies
- [nlohmann/json](https://github.com/nlohmann/json) for JSON model support (switch: `SIMFIL_WITH_MODEL_JSON`, default: `YES`).
- [fraillt/bitsery](https://github.com/fraillt/bitsery) for binary en- and decoding.
- [slavenf/sfl-library](https://github.com/slavenf/sfl-library.git) for small vector container.
- [klebert-engineering/stx](https://github.com/Klebert-Engineering/stx.git) for string formatting.
