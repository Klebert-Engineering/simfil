#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>

namespace simfil
{

/**
 * Limits for one callback-based evaluation, checked at expression/stream steps.
 * A limit stops enumeration and preserves the results already delivered. Zero
 * means no allowance for that counter, rather than an unlimited budget.
 * Compilation, consumers, regex and blocking custom functions need their own
 * limits: these cooperative checks cannot interrupt them or bound value sizes.
 */
struct EvaluationOptions
{
    std::size_t maxResults = 1000; ///< Maximum values delivered to the final consumer.
    std::size_t maxWork = 100000;  ///< Charged expression entries, emissions and traversal steps.
    std::size_t maxDepth = 256;   ///< Maximum expression/traversal nesting accepted by a step.

    /** Absolute monotonic deadline; max() disables the deadline check. */
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();

    /** Optional flag borrowed until the synchronous evaluation returns. */
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
