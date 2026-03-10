// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#include "simfil/sourcelocation.h"
#include "simfil/value.h"
#include "simfil/error.h"
#include "simfil/expression.h"

#include <tl/expected.hpp>
#include <optional>
#include <vector>
#include <string>
#include <cstdlib>

namespace simfil
{

class AST;
struct Environment;
struct ModelNode;

/** Query Diagnostics. */
struct Diagnostics
{
public:
    struct FieldExprData
    {
        SourceLocation location;
        std::uint32_t hits = 0;
        std::uint32_t evaluations = 0;
        std::string name;
    };


    struct ComparisonExprData
    {
        SourceLocation location;
        TypeFlags leftTypes;
        TypeFlags rightTypes;
        std::uint32_t falseResults = 0u;
        std::uint32_t trueResults = 0u;
    };

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
     * Get diagnostics data for a single Expr.
     */
    template <class DiagnosticsDataType>
    auto get(const Expr& expr) -> DiagnosticsDataType&;

    /**
     * Append/merge another diagnostics object into this one.
     */
    Diagnostics& append(const Diagnostics& other);

    /**
     * Serialize/deserialize diagnostics data to/from binary streams using bitsery.
     */
    auto write(std::ostream& stream) const -> tl::expected<void, Error>;
    auto read(std::istream& stream) -> tl::expected<void, Error>;

    /**
     * Build the exprIndex_ map for the AST.
     */
    auto prepareIndices(const Expr& ast) -> void;

    /** ExprId to diagnostics data index mapping. */
    std::vector<size_t> exprIndex_;

    /** FieldExpr diagnostics data. */
    std::vector<FieldExprData> fieldData_;

    /** ComparisonExpr diagnostics data. */
    std::vector<ComparisonExprData> comparisonData_;

private:
    friend auto diagnostics(Environment& env, const AST& ast, const Diagnostics& diag) -> tl::expected<std::vector<Message>, Error>;

    /**
     * Build messages from this objecst diagnostics data.
     */
    auto buildMessages(Environment& env, const AST& ast) const -> std::vector<Message>;

    mutable std::mutex mtx_;
};

namespace detail
{

template <class T>
struct DiagnosticsStorage;

template <>
struct DiagnosticsStorage<Diagnostics::FieldExprData>
{
    static auto get(Diagnostics& diag)
    {
        return &diag.fieldData_;
    }
};

template <>
struct DiagnosticsStorage<Diagnostics::ComparisonExprData>
{
    static auto get(Diagnostics& diag)
    {
        return &diag.comparisonData_;
    }
};

}

/**
 * Get typed diagnostics data for a single Expr.
 */
template <class DiagnosticsDataType>
auto Diagnostics::get(const Expr& expr) -> DiagnosticsDataType&
{
    auto* data = detail::DiagnosticsStorage<DiagnosticsDataType>::get(*this);

    const auto id = expr.id();
    if (exprIndex_.size() <= id) [[unlikely]] {
        exprIndex_.resize(id + 1u);
        exprIndex_[id] = data->size();
    }

    const auto index = exprIndex_[id];
    if (data->size() <= index) {
        data->resize(index + 1u);
    }

    return (*data)[index];
}

}
