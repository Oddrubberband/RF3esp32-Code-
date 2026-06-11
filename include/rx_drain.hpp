#pragma once

#include <cstddef>

namespace RxDrain {

constexpr size_t kDefaultMaxPacketsPerPoll = 8;

enum class StepResult {
    Processed,
    Stop,
    Failed
};

struct DrainResult {
    size_t processed = 0;
    bool receive_failed = false;
    bool guard_exhausted = false;
};

template <typename HasPendingFn, typename StepFn>
DrainResult drainPending(HasPendingFn hasPending,
                         StepFn step,
                         size_t max_packets = kDefaultMaxPacketsPerPoll)
{
    DrainResult result{};

    while (result.processed < max_packets && hasPending()) {
        const StepResult step_result = step();
        if (step_result == StepResult::Failed) {
            result.receive_failed = true;
            break;
        }

        ++result.processed;
        if (step_result == StepResult::Stop) {
            break;
        }
    }

    result.guard_exhausted = result.processed >= max_packets && hasPending();
    return result;
}

}  // namespace RxDrain
