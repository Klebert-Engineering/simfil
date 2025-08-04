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

class Expr;
class Function;
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
     * @param fn    IterCallback called thread-safe
     */
    auto trace(const std::string& name, const std::function<void(Trace&)>& fn) -> void;

    /**
     * Query function by name.
     */
    auto findFunction(const std::string& name) const -> const Function*;

    /**
     * Query constant by name.
     */
    auto findConstant(const std::string& name) const -> const Value*;

    /**
     * Obtain a strong reference to this environment's string cache.
     * Guaranteed not-null.
     */
    [[nodiscard]]
    auto strings() const -> std::shared_ptr<StringPool>;

public:
    std::unique_ptr<std::mutex> warnMtx;
    std::vector<std::pair<std::string, std::string>> warnings;

    std::unique_ptr<std::mutex> traceMtx;
    std::map<std::string, Trace> traces;

    /* function ident -> function */
    std::map<std::string, const Function*, CaseInsensitiveCompare> functions;

    /* constant ident -> value */
    std::map<std::string, Value, CaseInsensitiveCompare> constants;

    Debug* debug = nullptr;
    std::shared_ptr<StringPool> stringPool;
};

/**
 * Evaluation context.
 */
struct Context
{
    Environment* const env;

    /* Current phase under which the evaluation
     * takes place. */
    enum Phase
    {
        Compilation,
        Evaluation,
    };
    Phase phase = Evaluation;

    /* Timeout after which the evaluation should be canceled. */
    std::chrono::time_point<std::chrono::steady_clock> timeout = std::chrono::time_point<std::chrono::steady_clock>::max();

    Context(Environment* env, Phase = Phase::Evaluation);

    auto canceled() const -> bool
    {
        if (phase != Phase::Compilation)
            return timeout < std::chrono::steady_clock::now();
        return false;
    }
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
    // Auto insert a wildcard if the first token is a field name.
    bool autoWildcard = false;

    // Limit of candidates.
    size_t limit = 15;

    // Timeout in milliseconds, 0 means no timeout.
    size_t timeoutMs = 0;

    // Enable smart-case completion
    bool smartCase = true;

    // Sort candidates
    bool sorted = true;
};

/**
 * Completion candidate
 */
struct CompletionCandidate
{
    std::string text;
    SourceLocation location;
    enum class Type {
      WORD  = 1,
      FIELD = 2,
    } type;

    auto operator<=>(const CompletionCandidate& r) const
    {
        return std::tie(text, location.offset, location.size, type) <=> std::tie(r.text, r.location.offset, r.location.size, r.type);
    }
};

}
