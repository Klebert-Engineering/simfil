// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#include "simfil/value.h"
#include "simfil/token.h"
#include "simfil/error.h"

#include <tl/expected.hpp>
#include <optional>
#include <vector>
#include <string>
#include <memory>

namespace simfil
{

class AST;
class Expr;
struct Environment;
struct ModelNode;

/** Query Diagnostics. */
struct Diagnostics
{
public:
    using ExprId = std::uint32_t;

    struct Message
    {
        /* User message */
        std::string message;

        /* Location the message refers to */
        SourceLocation location;

        /* Optional query string that applies this fix */
        std::optional<std::string> fix;
    };

    Diagnostics();
    Diagnostics(Diagnostics&&) noexcept;
    ~Diagnostics();

    /**
     * Append/merge another diagnostics object into this one.
     */
    Diagnostics& append(const Diagnostics& other);

    /**
     * Serialize/deserialize diagnostics data to/from binary streams using bitsery.
     */
    auto write(std::ostream& stream) const -> tl::expected<void, Error>;
    auto read(std::istream& stream) -> tl::expected<void, Error>;

    struct Data;
private:
    friend auto eval(Environment&, const AST&, const ModelNode&, Diagnostics*) -> tl::expected<std::vector<Value>, Error>;
    friend auto diagnostics(Environment& env, const AST& ast, const Diagnostics& diag) -> tl::expected<std::vector<Message>, Error>;

    std::unique_ptr<Data> data;

    /**
     * Collect diagnostics data from an AST.
     */
    auto collect(Expr& ast) -> void;

    /**
     * Build messages from this objecst diagnostics data.
     */
    auto buildMessages(Environment& env, const AST& ast) const -> std::vector<Message>;
};

}
