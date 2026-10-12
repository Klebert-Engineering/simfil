#pragma once

#include "simfil/evaluation.h"

namespace simfil::detail
{

/** One invocation owns its budget; Context copies share this state but bindings never retain it. */
class EvaluationControl
{
public:
    /** Borrow the request's immutable limits for the duration of synchronous evaluation. */
    explicit EvaluationControl(const EvaluationOptions& options) : options(options) {}

    /** Preserve the first stop reason even if an aggregate attempts a final result. */
    auto stop(EvaluationSummary::Reason reason) -> bool
    {
        if (summary.reason == EvaluationSummary::Reason::Complete)
            summary.reason = reason;
        return false;
    }

    /** Check asynchronous cancellation/deadline without consuming work. */
    auto running() -> bool
    {
        if (summary.reason != EvaluationSummary::Reason::Complete)
            return false;
        if (options.cancel && options.cancel->load(std::memory_order_relaxed))
            return stop(EvaluationSummary::Reason::Canceled);
        if (options.deadline != std::chrono::steady_clock::time_point::max()
            && std::chrono::steady_clock::now() >= options.deadline)
            return stop(EvaluationSummary::Reason::Timeout);
        return true;
    }

    /** Charge an expression, intermediate value, or traversed model node/edge. */
    auto step(std::size_t depth) -> bool
    {
        if (!running())
            return false;
        if (depth > options.maxDepth)
            return stop(EvaluationSummary::Reason::DepthLimit);
        if (summary.work >= options.maxWork)
            return stop(EvaluationSummary::Reason::WorkLimit);
        ++summary.work;
        return true;
    }

    const EvaluationOptions& options;
    EvaluationSummary summary;
};

}
