// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#include "simfil/diagnostics.h"
#include "simfil/expression-visitor.h"

#include "expressions.h"
#include "simfil/sourcelocation.h"

#include <algorithm>
#include <mutex>
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

namespace
{

void mergeFieldData(
    Diagnostics::FieldExprData& ours,
    Diagnostics::FieldExprData const& theirs)
{
    if (ours.name.empty()) {
        ours.name = theirs.name;
    }
    if (ours.location == SourceLocation{0, 0}) {
        ours.location = theirs.location;
    }
    ours.hits += theirs.hits;
    ours.evaluations += theirs.evaluations;
}

void mergeComparisonData(
    Diagnostics::ComparisonExprData& ours,
    Diagnostics::ComparisonExprData const& theirs)
{
    if (ours.location == SourceLocation{0, 0}) {
        ours.location = theirs.location;
    }
    ours.leftTypes.set(theirs.leftTypes);
    ours.rightTypes.set(theirs.rightTypes);
    ours.evaluations += theirs.evaluations;
    ours.trueResults += theirs.trueResults;
    ours.falseResults += theirs.falseResults;
}

template <typename Entry, typename MergeFn>
void mergeIndexedData(
    std::vector<Entry>& ours,
    std::vector<Entry> const& theirs,
    MergeFn&& mergeEntry)
{
    auto const count = std::max(ours.size(), theirs.size());
    for (std::size_t i = 0; i < count; ++i) {
        auto* ourEntry = i < ours.size() ? &ours[i] : nullptr;
        auto const* theirEntry = i < theirs.size() ? &theirs[i] : nullptr;

        if (!theirEntry) {
            continue;
        }
        if (!ourEntry) {
            ours.emplace_back(*theirEntry);
            continue;
        }

        mergeEntry(*ourEntry, *theirEntry);
    }
}

void normalizeNumericTypes(TypeFlags& flags)
{
    if (flags.test(ValueType::Int) || flags.test(ValueType::Float)) {
        flags.set(ValueType::Int);
        flags.set(ValueType::Float);
    }
}

std::string comparisonPrefix(Diagnostics::ComparisonExprData const& data)
{
    auto const allTrue = data.trueResults > 0 && data.falseResults == 0;
    if (allTrue) {
        return "All values compared to true. ";
    }

    auto const allFalse = data.falseResults > 0 && data.trueResults == 0;
    if (allFalse) {
        return "All values compared to false. ";
    }

    return {};
}

}  // namespace

template<typename S>
void serialize(S& s, TypeFlags& flags)
{
    s.ext(flags.flags, bitsery::ext::StdBitset{});
}

template<typename S>
void serialize(S& s, Diagnostics& data)
{
    s.container(data.exprIndex_, std::numeric_limits<uint16_t>::max(), [](auto& s2, std::uint32_t& v) {
        s2.value4b(v);
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
        s2.value4b(data.evaluations);
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
    if (this == &other) {
        return *this;
    }

    std::scoped_lock lock(mtx_, other.mtx_);

#if !defined(NDEBUG)
    if (!exprIndex_.empty())
        assert(std::ranges::equal(exprIndex_, other.exprIndex_));
#endif

    // Either our indices are empty or equal to the rhs, so
    // we can just copy them every time.
    exprIndex_ = other.exprIndex_;

    mergeIndexedData(fieldData_, other.fieldData_, mergeFieldData);
    mergeIndexedData(comparisonData_, other.comparisonData_, mergeComparisonData);

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

auto Diagnostics::buildMessages() const -> std::vector<Diagnostics::Message>
{
    std::vector<Diagnostics::Message> messages;

    auto addMessage = [&](SourceLocation loc, std::string text) {
        Diagnostics::Message msg;
        msg.message = std::move(text);
        msg.location = loc;

        messages.push_back(std::move(msg));
    };

    for (const auto& data : fieldData_) {
        if (data.hits == 0 && data.evaluations > 0)
            addMessage(data.location, fmt::format("No matches for field '{}'", data.name));
    }

    for (const auto& data : comparisonData_) {
        auto leftTypes = data.leftTypes;
        normalizeNumericTypes(leftTypes);

        auto rightTypes = data.rightTypes;
        normalizeNumericTypes(rightTypes);

        const auto intersection = leftTypes.flags & rightTypes.flags;
        if (intersection.none() && data.evaluations > 0) {
            addMessage(
                data.location,
                fmt::format(
                    "{}Left hand types are {}, right hand types are {}.",
                    comparisonPrefix(data),
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
        std::uint32_t fieldIndex_ = 0u;
        std::uint32_t comparisonIndex_ = 0u;
        std::vector<std::uint32_t> indices_;

        Visitor()
        {
            indices_.reserve(32);
        }

        using ExprVisitor::visit;

        auto visit(const FieldExpr& e) -> void override {
            ExprVisitor::visit(e);
            if (e.id() >= indices_.size())
                indices_.resize(e.id() + 1, Diagnostics::InvalidIndex);
            indices_[e.id()] = fieldIndex_++;
        }

        auto visit(const WildcardFieldExpr& e) -> void override {
            ExprVisitor::visit(e);
            if (e.id() >= indices_.size())
                indices_.resize(e.id() + 1, Diagnostics::InvalidIndex);
            indices_[e.id()] = fieldIndex_++;
        }

        auto visitComparisonOperator(const ComparisonExprBase& e) -> void
        {
            if (e.id() >= indices_.size())
                indices_.resize(e.id() + 1, Diagnostics::InvalidIndex);
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
