// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#include "simfil/value.h"
#include "simfil/token.h"

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

    struct Data;
private:
    friend auto eval(Environment&, const AST&, const ModelNode&, Diagnostics*) -> std::vector<Value>;
    friend auto diagnostics(Environment& env, const AST& ast, const Diagnostics& diag) -> std::vector<Message>;

    using ExprId = size_t;

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
