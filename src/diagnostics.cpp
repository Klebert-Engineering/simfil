// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#include "simfil/diagnostics.h"

#include "levenshtein.h"
#include "expressions.h"

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

            // Generate "did you mean ...?" messages for missing fields
            if (auto iter = diagnostics.fieldHits.find(index()); iter != diagnostics.fieldHits.end() && iter->second == 0) {
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
        }

        void addMessage(std::string text, const Expr& e, std::optional<std::string> fix)
        {
            Diagnostics::Message msg;
            msg.message = std::move(text);
            msg.location = e.sourceLocation();
            msg.fix = std::move(fix);

            messages.push_back(std::move(msg));
        }
    };

    auto lock = data->lock();
    Visitor visitor(ast, env, *data);
    ast.expr().accept(visitor);

    return visitor.messages;
}

auto Diagnostics::collect(Expr& ast) -> void
{
    struct MergeDiagnostics : ExprVisitor
    {
        Diagnostics::Data& diagnostics;

        explicit MergeDiagnostics(Diagnostics::Data& diag)
            : diagnostics(diag)
        {}

        using ExprVisitor::visit;

        auto visit(FieldExpr& e) -> void override {
            ExprVisitor::visit(e);

            if (e.evaluations_ > 0)
                diagnostics.fieldHits[index()] += e.hits_;
        }
    };

    auto lock = data->lock();
    MergeDiagnostics visitor(*data);
    ast.accept(visitor);
}

}
