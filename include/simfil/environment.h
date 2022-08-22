// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#include "simfil/value.h"

#include <map>
#include <memory>
#include <vector>
#include <chrono>
#include <functional>
#include <mutex>

namespace simfil
{

class Expr;
class Function;
class ModelNode;
struct Debug;

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
    explicit Environment();
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
     * @param fn    Callback called thread-safe
     */
    auto trace(const std::string& name, std::function<void(Trace&)> fn) -> void;

    /**
     * Query function by name.
     */
    auto findFunction(const std::string&) const -> const Function*;

public:
    std::unique_ptr<std::mutex> warnMtx;
    std::vector<std::pair<std::string, std::string>> warnings;

    std::unique_ptr<std::mutex> traceMtx;
    std::map<std::string, Trace> traces;

    std::map<std::string, const Function*> functions;

    Debug* debug = nullptr;
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
 * Result value callback.
 * Return `false` to stop evaluation.
 */
enum Result { Continue = 1, Stop = 0 };
using ResultFn = std::function<Result(Context, Value)>;

/**
 * Debug interface
 */
struct Debug
{
    std::function<void(const Expr&, Context&, Value&, ResultFn&)> evalBegin;
    std::function<void(const Expr&)> evalEnd;
};

}
