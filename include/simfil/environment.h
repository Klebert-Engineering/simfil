// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#include "simfil/value.h"
#include "simfil/model/model.h"

#include <algorithm>
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
    auto trace(const std::string& name, std::function<void(Trace&)> fn) -> void;

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

    /* lower-case function ident -> function */
    std::map<std::string, const Function*, CaseInsensitiveCompare> functions;

    /* lower-case constant ident -> value */
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

    Context(Environment*, Phase = Phase::Evaluation);
};

/**
 * Debug interface
 */
struct Debug
{
    std::function<void(const Expr&, Context&, Value&, const ResultFn&)> evalBegin;
    std::function<void(const Expr&)> evalEnd;
};

}
