#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>

namespace simfil
{

/**
 * Cooperative streaming limits, not a CPU/memory sandbox. Compilation, JSON,
 * consumer execution, regex, blocking custom functions and per-value byte size
 * require separate policies/budgets; none can be preempted by these counters.
 */
struct EvaluationOptions
{
    std::size_t maxResults = 1000;
    std::size_t maxWork = 100000;
    std::size_t maxDepth = 256;
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
    const std::atomic_bool* cancel = nullptr;
};

/** Explain whether enumeration finished or why it stopped; partial results stay with the consumer. */
struct EvaluationSummary
{
    /** A result-limit stop is conservative: the last delivered value might have been the final one. */
    enum class Reason { Complete, ConsumerStopped, ResultLimit, WorkLimit, DepthLimit, Timeout, Canceled };
    Reason reason = Reason::Complete;
    std::size_t results = 0;
    std::size_t work = 0;
};

}
