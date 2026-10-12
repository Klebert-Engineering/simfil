#pragma once

#include "simfil/token.h"
#include "simfil/environment.h"

#include "expressions.h"

#include <string>
#include <set>
#include <memory>

namespace simfil
{

class ExprVisitor;

/**
 * List of completion candidates.
 */
struct Completion
{
    Completion(std::size_t point, CompletionOptions options)
        : point(point), options(std::move(options))
    {}

    Completion(const Completion&) = delete;
    Completion& operator=(const Completion&) = delete;

    auto add(std::string str, SourceLocation location, CompletionCandidate::Type type, std::string hint = {})
    {
        candidates.emplace(std::move(str), location, type, hint);
    }

    auto size() const
    {
        return candidates.size();
    }

    /** Walk a relaxed AST over schema domains, without constructing ModelNodes. */
    void completeDomain(Context ctx, const Expr& expression, SchemaId root);

    std::size_t point;
    std::size_t limit = 1000;
    std::set<CompletionCandidate> candidates;
    CompletionOptions options;

private:
    /** Bounded domain inference visits unknown expressions conservatively. */
    auto walkDomain(Context& ctx, const Expr& expression, const std::vector<SchemaId>& input,
                    const std::vector<SchemaId>& expected, std::size_t depth, bool insidePath = false) -> std::vector<SchemaId>;
    /** Expand logical alternatives, optionally including reachable descendant domains. */
    auto expandDomains(Context& ctx, const std::vector<SchemaId>& input, bool recursive) -> std::vector<SchemaId>;
    /** Collect field/function/enum candidates in the current operand context. */
    void suggestDomain(Context& ctx, const std::vector<SchemaId>& input,
                       const std::vector<SchemaId>& expected, std::string_view prefix,
                       SourceLocation location, bool inPath, bool literalsOnly);
    /** Consume shared work allowance for AST and graph traversal. */
    auto visitDomain(Context& ctx) -> bool;
    std::size_t domainVisits_ = 0;
    SchemaId rootSchema_ = NoSchemaId;
};

class CompletionFieldOrWordExpr : public Expr
{
public:
    CompletionFieldOrWordExpr(std::string prefix, Completion* comp, const Token& token, bool inPath);

    auto type() const -> Type override;
    auto ieval(Context ctx, const Value& value, const ResultFn& result) const -> tl::expected<Result, Error> override;
    auto accept(ExprVisitor& v) const -> void override;
    auto toString() const -> std::string override;

    std::string prefix_;
    Completion* comp_;
    bool inPath_;
};

class CompletionAndExpr : public Expr
{
public:
    CompletionAndExpr(ExprPtr left, ExprPtr right, const Completion* comp);

    auto type() const -> Type override;
    auto ieval(Context ctx, const Value& val, const ResultFn& res) const -> tl::expected<Result, Error> override;
    void accept(ExprVisitor& v) const override;
    auto toString() const -> std::string override;

    ExprPtr left_, right_;
};

class CompletionOrExpr : public Expr
{
public:
    CompletionOrExpr(ExprPtr left, ExprPtr right, const Completion* comp);

    auto type() const -> Type override;
    auto ieval(Context ctx, const Value& val, const ResultFn& res) const -> tl::expected<Result, Error> override;
    void accept(ExprVisitor& v) const override;
    auto toString() const -> std::string override;

    ExprPtr left_, right_;
};

class CompletionWordExpr : public Expr
{
public:
    CompletionWordExpr(std::string prefix, Completion* comp, const Token& token);

    auto type() const -> Type override;
    auto constant() const -> bool override;
    auto ieval(Context ctx, const Value& value, const ResultFn& result) const -> tl::expected<Result, Error> override;
    auto accept(ExprVisitor& v) const -> void override;
    auto toString() const -> std::string override;

    std::string prefix_;
    Completion* comp_;
};

/**
 * A special expression to prevent constant value
 * evaluation during completion.
 */
class CompletionConstExpr : public ConstExpr
{
public:
    using ConstExpr::ConstExpr;

    auto constant() const -> bool override;
    auto ieval(Context ctx, const Value&, const ResultFn& res) const -> tl::expected<Result, Error> override;
};

}
