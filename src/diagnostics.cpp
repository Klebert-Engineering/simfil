// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#include "simfil/diagnostics.h"
#include "simfil/expression-visitor.h"

#include "expressions.h"
#include "simfil/sourcelocation.h"

#include <algorithm>
#include <ranges>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <bitsery/bitsery.h>
#include <bitsery/adapter/stream.h>
#include <bitsery/ext/std_bitset.h>
#include <bitsery/traits/string.h>
#include <bitsery/traits/vector.h>

namespace simfil
{

template<typename S>
void serialize(S& s, TypeFlags& flags)
{
    s.ext(flags.flags, bitsery::ext::StdBitset{});
}

template<typename S>
void serialize(S& s, Diagnostics& data)
{
    s.container(data.exprIndex_, std::numeric_limits<uint16_t>::max(), [](auto& s2, std::size_t& v) {
        s2.value8b(v);
    });
    s.container(data.fieldData_, std::numeric_limits<uint16_t>::max(), [](auto& s2, Diagnostics::FieldExprData& data) {
        s2.value4b(data.location.offset);
        s2.value4b(data.location.size);
        s2.value4b(data.hits);
        s2.value4b(data.evaluations);
        s2.text1b(data.name, 0xff);
    });
    s.container(data.comparisonData_, std::numeric_limits<uint16_t>::max(), [](auto& s2, Diagnostics::ComparisonExprData& data) {
        s2.value4b(data.location.offset);
        s2.value4b(data.location.size);
        s2.object(data.leftTypes);
        s2.object(data.rightTypes);
        s2.value4b(data.trueResults);
        s2.value4b(data.falseResults);
    });
}

Diagnostics::Diagnostics() = default;

Diagnostics::Diagnostics(Diagnostics&& other) noexcept
    : exprIndex_(std::move(other.exprIndex_))
    , fieldData_(std::move(other.fieldData_))
    , comparisonData_(std::move(other.comparisonData_))
{}

Diagnostics::~Diagnostics() = default;

Diagnostics& Diagnostics::append(const Diagnostics& other)
{
#if !defined(NDEBUG)
    if (!exprIndex_.empty())
        assert(std::ranges::equal(exprIndex_, other.exprIndex_));
#endif

    // Either our indices are empty or equal to the rhs, so
    // we can just copy them every time.
    exprIndex_ = other.exprIndex_;

    for (auto i = 0u; i < std::max<std::size_t>(fieldData_.size(), other.fieldData_.size()); ++i) {
        auto* ours = i < fieldData_.size() ? &fieldData_[i] : nullptr;
        auto* theirs = i < other.fieldData_.size() ? &other.fieldData_[i] : nullptr;

        if (ours && theirs) {
            assert(ours->name == theirs->name);
            ours->hits += theirs->hits;
            ours->evaluations += theirs->evaluations;
        }

        if (!ours && theirs) {
            fieldData_.emplace_back(*theirs);
        }
    }

    for (auto i = 0u; i < std::max<std::size_t>(comparisonData_.size(), other.comparisonData_.size()); ++i) {
        auto* ours = i < comparisonData_.size() ? &comparisonData_[i] : nullptr;
        auto* theirs = i < other.comparisonData_.size() ? &other.comparisonData_[i] : nullptr;

        if (ours && theirs) {
            ours->leftTypes.set(theirs->leftTypes);
            ours->rightTypes.set(theirs->rightTypes);
            ours->trueResults += theirs->trueResults;
            ours->falseResults += theirs->falseResults;
        }

        if (!ours && theirs) {
            comparisonData_.emplace_back(*theirs);
        }
    }

    return *this;
}

auto Diagnostics::write(std::ostream& stream) const -> tl::expected<void, Error>
{
    using OutputAdapter = bitsery::OutputStreamAdapter;
    
    bitsery::Serializer<OutputAdapter> serializer{stream};
    std::unique_lock lock(mtx_);
    serializer.object(*this);
    
    if (!stream.good()) {
        return tl::unexpected(Error(Error::IOError, "Failed to write diagnostics data to stream"));
    }
    
    return {};
}

auto Diagnostics::read(std::istream& stream) -> tl::expected<void, Error>
{
    using InputAdapter = bitsery::InputStreamAdapter;
    
    bitsery::Deserializer<InputAdapter> deserializer{stream};
    std::unique_lock lock(mtx_);
    deserializer.object(*this);
    
    if (!stream.good()) {
        return tl::unexpected(Error(Error::IOError, "Failed to read diagnostics data from stream"));
    }
    
    return {};
}

auto Diagnostics::buildMessages(Environment& env, const AST& ast) const -> std::vector<Diagnostics::Message>
{
    std::vector<Diagnostics::Message> messages;

    auto addMessage = [&](SourceLocation loc, std::string text) {
        Diagnostics::Message msg;
        msg.message = std::move(text);
        //msg.fix = std::move(fix);
        msg.location = loc;

        messages.push_back(std::move(msg));
    };

    for (const auto& data : fieldData_) {
        if (data.hits == 0u)
            addMessage(data.location, fmt::format("No matches for field '{}'", data.name));
    }

    for (const auto& data : comparisonData_) {
        auto leftTypes = data.leftTypes;
        if (leftTypes.test(ValueType::Int) || leftTypes.test(ValueType::Float)) {
            leftTypes.set(ValueType::Int);
            leftTypes.set(ValueType::Float);
        }

        auto rightTypes = data.rightTypes;
        if (rightTypes.test(ValueType::Int) || rightTypes.test(ValueType::Float)) {
            rightTypes.set(ValueType::Int);
            rightTypes.set(ValueType::Float);
        }

        const auto intersection = leftTypes.flags & rightTypes.flags;
        if (intersection.none()) {
            const auto allTrue = data.trueResults > 0 && data.falseResults == 0;
            const auto allFalse = data.falseResults > 0 && data.trueResults == 0;
            const auto prefix =
                allTrue ? "All values compared to true. " :
                allFalse ? "All values compared to false. " : "";

            addMessage(data.location, fmt::format("{}Left hand types are {}, right hand types are {}.",
                                                  prefix,
                                                  fmt::join(leftTypes.typeNames(), "|"),
                                                  fmt::join(rightTypes.typeNames(), "|")));
        }
    }

    return messages;
}

auto Diagnostics::prepareIndices(const Expr& ast) -> void
{
    struct Visitor : ExprVisitor
    {
        std::size_t fieldIndex_ = 0u;
        std::size_t comparisonIndex_ = 0u;
        std::vector<std::size_t> indices_;

        Visitor()
        {
            indices_.reserve(32);
        }

        using ExprVisitor::visit;

        auto visit(const FieldExpr& e) -> void override {
            ExprVisitor::visit(e);
            if (e.id() >= indices_.size())
                indices_.resize(e.id() + 1);
            indices_[e.id()] = fieldIndex_++;
        }

        auto visitComparisonOperator(const ComparisonExprBase& e) -> void
        {
            if (e.id() >= indices_.size())
                indices_.resize(e.id() + 1);
            indices_[e.id()] = comparisonIndex_++;
        }

        auto visit(const BinaryExpr<OperatorEq>& e) -> void override {
            ExprVisitor::visit(e);
            visitComparisonOperator(e);
        }

        auto visit(const BinaryExpr<OperatorNeq>& e) -> void override {
            ExprVisitor::visit(e);
            visitComparisonOperator(e);
        }

        auto visit(const BinaryExpr<OperatorLt>& e) -> void override {
            ExprVisitor::visit(e);
            visitComparisonOperator(e);
        }

        auto visit(const BinaryExpr<OperatorLtEq>& e) -> void override {
            ExprVisitor::visit(e);
            visitComparisonOperator(e);
        }

        auto visit(const BinaryExpr<OperatorGt>& e) -> void override {
            ExprVisitor::visit(e);
            visitComparisonOperator(e);
        }

        auto visit(const BinaryExpr<OperatorGtEq>& e) -> void override {
            ExprVisitor::visit(e);
            visitComparisonOperator(e);
        }
    };

    Visitor visitor;
    ast.accept(visitor);
    exprIndex_ = std::move(visitor.indices_);
    fieldData_.reserve(visitor.fieldIndex_);
    comparisonData_.reserve(visitor.comparisonIndex_);
}

}
