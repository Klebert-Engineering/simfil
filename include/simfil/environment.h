// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#include "simfil/model/nodes.h"
#include "simfil/value.h"
#include "simfil/token.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <memory>
#include <vector>
#include <chrono>
#include <functional>
#include <mutex>
#include <string_view>

namespace simfil
{

namespace detail
{
class ExpressionRuntime;
class EvaluationControl;
}

class Expr;
class Function;
class Diagnostics;
struct ResultFn;
struct Debug;

/** Case-insensitive comparator. */
struct CaseInsensitiveCompare
{
    auto operator()(const std::string_view& l, const std::string_view& r) const -> bool
    {
        return std::lexicographical_compare(l.begin(), l.end(), r.begin(), r.end(), [](auto lc, auto rc) {
            return tolower(lc) < tolower(rc);
        });
    }
};


/** Trace call stats. */
struct Trace
{
    std::size_t calls = 0; /* Number of calls */
    std::chrono::microseconds totalus;
    std::vector<Value> values;

    /**
     * Append/merge another trace result into this one.
     */
    Trace& append(Trace&& other)
    {
        calls += other.calls;
        totalus += other.totalus;
        values.insert(values.end(),
                      std::make_move_iterator(other.values.begin()),
                      std::make_move_iterator(other.values.end()));
        return *this;
    }
};

/**
 * Query dictionary, registered functions/constants and execution diagnostics.
 * Configure registrations before evaluation and keep function objects alive while
 * they are registered. warn()/trace() synchronize their own collections; the
 * public configuration maps and schema binding require caller synchronization.
 */
struct Environment
{
public:

    /**
     * Construct a SIMFIL execution environment with a string cache,
     * which is used to map field names to short integer IDs.
     * @param strings The string cache used by this environment.
     *  Must be the same cache that is also used by ModelPools which
     *  are queried using this environment.
     */
    explicit Environment(std::shared_ptr<StringPool> strings);

    /**
     * Constructor for instantiating an environment explicitly with
     * a new string cache - make sure that your models use it!
     *
     * Call like this:
     *   Environment myEnv(Environment::WithNewStringCache);
     */
    enum NewStringCache_ { WithNewStringCache };
    explicit Environment(NewStringCache_);

    /** Destructor */
    ~Environment();

    /**
     * Log a runtime warning.
     *
     * @param msg     Message
     * @param detail  Detail information (context)
     */
    auto warn(std::string msg, std::string detail = "") -> void;

    /**
     * Log a trace call.
     *
     * @param name  Trace identifier
     * @param fn    Callback invoked while holding traceMtx. It must not call trace()
     *              recursively or retain the mutable Trace reference after returning.
     */
    auto trace(const std::string& name, const std::function<void(Trace&)>& fn) -> void;

    /**
     * Look up a case-insensitive function name, returning nullptr when unregistered.
     * The returned function is borrowed from the caller-owned registration.
     */
    auto findFunction(const std::string& name) const -> const Function*;

    /**
     * Look up a case-insensitive constant name, returning nullptr when absent.
     * The pointer borrows storage in constants and must not outlive that entry.
     */
    auto findConstant(const std::string& name) const -> const Value*;

    /**
     * Obtain a strong reference to this environment's string cache.
     * Guaranteed not-null.
     */
    [[nodiscard]]
    auto strings() const -> std::shared_ptr<StringPool>;

    /** Retain a published default root without mutating the model or query dictionary. */
    void setSchema(SchemaType root);

    /** Borrow the configured root; callers pass it explicitly to schema-aware compilation. */
    auto schema() const noexcept -> SchemaType { return rootSchema_.borrow(); }

public:
    std::unique_ptr<std::mutex> warnMtx;
    std::vector<std::pair<std::string, std::string>> warnings;

    std::unique_ptr<std::mutex> traceMtx;
    std::map<std::string, Trace> traces;

    /* function ident -> function */
    std::map<std::string, const Function*, CaseInsensitiveCompare> functions;

    /* constant ident -> value */
    std::map<std::string, Value, CaseInsensitiveCompare> constants;

private:
    SchemaType rootSchema_;

public:

    /**
     * Enable cached schema-guided wildcard field traversal plans.
     *
     * Disabling this keeps the older behavior where wildcard field lookups only
     * ask each node schema whether the requested field can appear below it.
     */
    bool enableWildcardFieldPlans = true;

    Debug* debug = nullptr;
    std::shared_ptr<StringPool> stringPool;
};

/**
 * Evaluation context.
 *
 * Non-owning state passed through a synchronous expression call. Copies keep the
 * same environment, diagnostics and request budget, while nesting depth and phase
 * are local to the copy. Owners must remain alive until evaluation returns.
 */
struct Context
{
    Environment* const env;
    Diagnostics* const diag;

    /**
     * Evaluation-local caches for environment-dependent expression state.
     *
     * This is populated by the public evaluation APIs. Expression evaluation
     * during compilation deliberately leaves it null and performs uncached
     * lookups instead.
     */
    detail::ExpressionRuntime* runtime = nullptr;

    /** Optional request-local cooperative budget; never retained by an expression binding. */
    detail::EvaluationControl* evaluation = nullptr;

    /* Current phase under which the evaluation
     * takes place. */
    enum Phase
    {
        Compilation,
        Evaluation,
    };
    Phase phase = Evaluation;

    // Keep nesting beside the phase tag to avoid padding in frequently copied contexts.
    std::uint32_t evaluationDepth = 0;

    /* Timeout after which the evaluation should be canceled. */
    std::optional<std::chrono::time_point<std::chrono::steady_clock>> timeout;

    Context() = delete;
    Context(Environment* env, Diagnostics* diag, Phase = Phase::Evaluation);

    auto canceled() const -> bool
    {
        if (evaluation && !evaluationRunning())
            return true;
        if (phase != Phase::Compilation && timeout)
            return *timeout < std::chrono::steady_clock::now();
        return false;
    }

    /** Charge cooperative work; custom functions should call this in long non-emitting loops. */
    auto step(std::size_t depth = 0) const -> bool;

private:
    /** Keep the execution-budget implementation out of public model headers. */
    auto evaluationRunning() const -> bool;
};

/**
 * Debug interface
 */
struct Debug
{
    std::function<void(const Expr&, Context&, Value&, const ResultFn&)> evalBegin;
    std::function<void(const Expr&)> evalEnd;
};

/**
 * Options for the autocompletion.
 */
struct CompletionOptions
{
    // Show hints about completing certain queries to a wildcard query.
    bool showWildcardHints = true;

    // Candidate cap; zero selects the internal fallback of 1000, not an unlimited result.
    size_t limit = 15;

    // Timeout in milliseconds, 0 means no timeout.
    size_t timeoutMs = 0;

    // Enable smart-case completion
    bool smartCase = true;

    // Sort candidates
    bool sorted = true;

    /** Bound schema-only traversal even when timeoutMs is disabled. */
    size_t maxSchemaVisits = 10000;

    /** Bound AST inference recursion; graph cycles are deduplicated by schema ID. */
    size_t maxSchemaDepth = 128;
};

/**
 * Completion candidate
 */
struct CompletionCandidate
{
    enum class Type {
      CONSTANT = 1,
      FIELD    = 2,
      FUNCTION = 3,
      HINT     = 4,
    };

    std::string text;        // Text to insert
    SourceLocation location; // Location to insert the text at
    Type type;               // Type of the completion
    std::string hint;        // Additional info

    CompletionCandidate(std::string text, SourceLocation location, Type type, std::string hint)
        : text(std::move(text)), location(std::move(location)), type(type), hint(hint)
    {}

    auto operator<=>(const CompletionCandidate& r) const
    {
        return std::tie(text, location.offset, location.size, type, hint) <=> std::tie(r.text, r.location.offset, r.location.size, r.type, r.hint);
    }
};

}
