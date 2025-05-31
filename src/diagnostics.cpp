// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#include "simfil/diagnostics.h"

#include "levenshtein.h"
#include "expressions.h"

#include <fmt/ranges.h>

namespace simfil
{

struct Diagnostics::Data
{
    std::mutex mtx;

    // Number a FieldExpr field was found in the model
    std::unordered_map<ExprId, uint32_t> fieldHits;

    // Coparison operator operand types
    std::unordered_map<ExprId, std::tuple<TypeFlags, TypeFlags>> comparisonOperandTypes;

    [[nodiscard]]
    auto lock()
    {
        return std::unique_lock(mtx);
    }
};

Diagnostics::Diagnostics()
    : data(std::make_unique<Diagnostics::Data>())
{}

Diagnostics::Diagnostics(Diagnostics&& other)
    : data(std::move(other.data))
{}

Diagnostics::~Diagnostics() = default;

Diagnostics& Diagnostics::append(const Diagnostics& other)
{
    auto otherLock = other.data->lock();
    auto lock = data->lock();

    for (const auto& [key, value] : other.data->fieldHits) {
        data->fieldHits[key] += value;
    }

    for (const auto& [key, value] : other.data->comparisonOperandTypes) {
        auto& [left, right] = data->comparisonOperandTypes[key];
        left.set(std::get<0>(value));
        right.set(std::get<1>(value));
    }

    return *this;
}

static auto findSimilarString(std::string_view source, const StringPool& pool) -> std::string
{
    std::string_view best;
    auto bestScore = std::numeric_limits<int>::max();

    const auto isDollar = source[0] == '$';
    for (const auto& target : pool.strings()) {
        const auto targetIsDollar = target[0] == '$';
        if (isDollar != targetIsDollar)
            continue;
        if (target == source)
            continue;

        const auto score = levenshtein(source, target);
        if (score < bestScore) {
            bestScore = score;
            best = target;
        }
    }

    return std::string(best);
}


auto Diagnostics::buildMessages(Environment& env, const AST& ast) const -> std::vector<Diagnostics::Message>
{
    struct Visitor : ExprVisitor
    {
        const AST& ast;
        const Environment& env;
        const Diagnostics::Data& diagnostics;
        std::vector<Diagnostics::Message> messages;

        Visitor(const AST& ast, const Environment& env, const Diagnostics::Data& diagnostics)
            : ast(ast)
            , env(env)
            , diagnostics(diagnostics)
        {}

        using ExprVisitor::visit;

        void visit(FieldExpr& e) override
        {
            ExprVisitor::visit(e);

            auto iter = diagnostics.fieldHits.find(index());
            if (iter == diagnostics.fieldHits.end())
                return;

            if (iter->second > 0)
                return;

            // Generate "did you mean ...?" messages for missing fields
            auto guess = findSimilarString(e.name_, *env.strings());
            if (!guess.empty()) {
                std::string fix = ast.query();
                if (auto loc = e.sourceLocation(); loc.size > 0)
                    fix.replace(loc.begin, loc.size, guess);

                addMessage(fmt::format("No matches for field '{}'. Did you mean '{}'?", e.name_, guess), e, fix);
            } else {
                addMessage(fmt::format("No matches for field '{}'.", e.name_, guess), e, {});
            }
        }

        void visitComparisonOperator(ComparisonExprBase& e, bool expectedResult)
        {
            const auto [falseResults, trueResults] = e.resultCounts();
            if ((expectedResult && trueResults > 0) || (!expectedResult && falseResults > 0))
                return;

            auto iter = diagnostics.comparisonOperandTypes.find(index());
            if (iter == diagnostics.comparisonOperandTypes.end())
                return;

            const auto [leftTypes, rightTypes] = iter->second;
            const auto intersection = leftTypes.flags & rightTypes.flags;
            if (intersection.any())
                return;

            addMessage(fmt::format("All values compared to {}. Left hand types are {}, right hand types are {}.",
                                   expectedResult ? "false" : "true",
                                   fmt::join(leftTypes.typeNames(), "|"),
                                   fmt::join(rightTypes.typeNames(), "|")),
                       e, {});
        }

        void visit(BinaryExpr<OperatorEq>& e) override
        {
            ExprVisitor::visit(e);
            visitComparisonOperator(e, true);
        }

        void visit(BinaryExpr<OperatorNeq>& e) override
        {
            ExprVisitor::visit(e);
            visitComparisonOperator(e, false);
        }

        void visit(BinaryExpr<OperatorLt>& e) override
        {
            ExprVisitor::visit(e);
            visitComparisonOperator(e, true);
        }

        void visit(BinaryExpr<OperatorLtEq>& e) override
        {
            ExprVisitor::visit(e);
            visitComparisonOperator(e, true);
        }

        void visit(BinaryExpr<OperatorGt>& e) override
        {
            ExprVisitor::visit(e);
            visitComparisonOperator(e, false);
        }

        void visit(BinaryExpr<OperatorGtEq>& e) override
        {
            ExprVisitor::visit(e);
            visitComparisonOperator(e, false);
        }

        void addMessage(std::string text, const Expr& expr, std::optional<std::string> fix)
        {
            Diagnostics::Message msg;
            msg.message = std::move(text);
            msg.location = expr.sourceLocation();
            msg.fix = std::move(fix);

            messages.push_back(std::move(msg));
        }
    };

    Visitor visitor(ast, env, *data);

    auto lock = data->lock();
    ast.expr().accept(visitor);

    return visitor.messages;
}

auto Diagnostics::collect(Expr& ast) -> void
{
    struct CollectDiagnostics : ExprVisitor
    {
        Diagnostics::Data& diagnostics;
        bool suppressDiagnostics = false;

        explicit CollectDiagnostics(Diagnostics::Data& diag)
            : diagnostics(diag)
        {}

        using ExprVisitor::visit;

        auto visit(FieldExpr& e) -> void override {
            ExprVisitor::visit(e);

            if (e.evaluations_ > 0)
                diagnostics.fieldHits[index()] += e.hits_;
        }

        auto visitComparisonOperator(const ComparisonExprBase& e) -> void
        {
            const auto [thisLeft, thisRight] = e.operandTypes();
            if ((thisLeft.flags | thisRight.flags).any()) {
                auto& [left, right] = diagnostics.comparisonOperandTypes[index()];
                left.set(thisLeft);
                right.set(thisRight);
            }
        }

        auto visit(AnyExpr& e) -> void override {
            ExprVisitor::visit(static_cast<Expr&>(e));

            // Only provide diagnostic results for
            // child expressions if none of them matched.
            if (e.trueResults_ == 0 && e.falseResults_ > 0) {
                for (const auto& arg : e.args_)
                    if (arg)
                        arg->accept(*this);
            }
        }

        auto visit(OrExpr& e) -> void override {
            ExprVisitor::visit(static_cast<Expr&>(e));

            // Suppress diagnostics of the right hand side
            // expression if the left hand side matched and
            // vice versa.
            e.left_->accept(*this);
            if (e.rightEvaluations_ > 0)
                e.right_->accept(*this);
        }

        auto visit(BinaryExpr<OperatorEq>& e) -> void override {
            ExprVisitor::visit(e);
            visitComparisonOperator(e);
        }

        auto visit(BinaryExpr<OperatorNeq>& e) -> void override {
            ExprVisitor::visit(e);
            visitComparisonOperator(e);
        }

        auto visit(BinaryExpr<OperatorLt>& e) -> void override {
            ExprVisitor::visit(e);
            visitComparisonOperator(e);
        }

        auto visit(BinaryExpr<OperatorLtEq>& e) -> void override {
            ExprVisitor::visit(e);
            visitComparisonOperator(e);
        }

        auto visit(BinaryExpr<OperatorGt>& e) -> void override {
            ExprVisitor::visit(e);
            visitComparisonOperator(e);
        }

        auto visit(BinaryExpr<OperatorGtEq>& e) -> void override {
            ExprVisitor::visit(e);
            visitComparisonOperator(e);
        }
    };

    CollectDiagnostics visitor(*data);

    auto lock = data->lock();
    ast.accept(visitor);
}

}
