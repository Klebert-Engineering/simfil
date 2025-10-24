#pragma once

#include "simfil/diagnostics.h"
#include "simfil/simfil.h"
#include "simfil/environment.h"
#include "simfil/exception-handler.h"
#include "simfil/function.h"
#include "simfil/model/json.h"
#include "simfil/result.h"
#include "simfil/value.h"

#include "src/expressions.h"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <stdexcept>

using namespace simfil;

static const char* const TestModel = R"json(
{
  "number": 123,
  "string": "TEXT",
  "__long__name__": true,
  "abc def": true,
  "a": 1,
  "b": 2,
  "c": ["a", "b", "c"],
  "d": [0, 1, 2],
  "sub": {
    "a": "sub a",
    "b": "sub b",
    "sub": {
      "a": "sub sub a",
      "b": "sub sub b"
    }
  },
  "geoPoint": {
    "geometry": {
      "type": "Point",
      "coordinates": [1, 2]
    }
  },
  "geoLineString": {
    "geometry": {
      "type": "LineString",
      "coordinates": [[1, 2], [3, 4]]
    }
  },
  "geoPolygon": {
    "geometry": {
      "type": "Polygon",
      "coordinates": [[[1, 2], [3, 4], [5, 6]]]
    }
  }
}
)json";

class PanicFn : public simfil::Function
{
public:
    auto ident() const -> const FnInfo& override
    {
        static const FnInfo info{
          "panic",
          "Raise an error",
          "panic()"
        };

        return info;
    }

    auto eval(Context ctx, const Value&, const std::vector<ExprPtr>&, const ResultFn& res) const -> tl::expected<Result, Error> override
    {
        if (ctx.phase != Context::Phase::Compilation)
            return tl::unexpected<Error>(Error::RuntimeError, "Panic!");

        return res(ctx, Value::undef());
    }
};

auto Compile(std::string_view query, bool autoWildcard = false) -> ASTPtr;
auto CompileError(std::string_view query, bool autoWildcard = false) -> Error;
auto JoinedResult(std::string_view query, std::optional<std::string> json = {}) -> std::string;
auto CompleteQuery(std::string_view query, size_t point, std::optional<std::string> json = {}, const CompletionOptions* = nullptr) -> std::vector<CompletionCandidate>;
auto GetDiagnosticMessages(std::string_view query) -> std::vector<Diagnostics::Message>;
