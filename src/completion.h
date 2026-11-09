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
 * Request-local candidate set and schema-inference budget. Completion expressions
 * borrow this object, which must outlive their parsing and synchronous evaluation.
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

    /** Walk a relaxed AST over schema types, without constructing ModelNodes. */
    void completeType(Context ctx, const Expr& expression, SchemaType root);

    std::size_t point;
    std::size_t limit = 1000;
    std::set<CompletionCandidate> candidates;
    CompletionOptions options;

private:
    /**
     * Infer possible result TypeIds while collecting candidates at cursor nodes.
     * input is the current value context; expected narrows comparison operands.
     * NoTypeId marks unknown inference, while an empty expected vector permits
     * root shorthand suggestions. No expression is executed by this traversal.
     */
    auto walkType(Context& ctx, const Expr& expression, const std::vector<TypeId>& input,
                    const std::vector<TypeId>& expected, std::size_t depth, bool insidePath = false) -> std::vector<TypeId>;

    /** Expand logical alternatives, optionally including reachable descendant types. */
    auto expandTypes(Context& ctx, const std::vector<TypeId>& input, bool recursive) -> std::vector<TypeId>;

    /** Collect field/function/enum candidates in the current operand context. */
    void suggestType(Context& ctx, const std::vector<TypeId>& input,
                       const std::vector<TypeId>& expected, std::string_view prefix,
                       SourceLocation location, bool inPath, bool literalsOnly);

    /** Consume shared work allowance for AST and graph traversal. */
    auto visitType(Context& ctx) -> bool;
    std::size_t typeVisits_ = 0;
    SchemaType rootSchema_;
};

/** Cursor placeholder that gathers fields, plus functions/constants when outside an explicit path. */
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

/** Keep only cursor-containing operands and visit both without boolean short-circuiting. */
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

/** Completion counterpart of or; a matching left operand must not suppress right-side suggestions. */
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

/** Cursor inside a quoted value, gathering literal candidates with the complete token replacement span. */
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
