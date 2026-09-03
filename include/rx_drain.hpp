#pragma once

#include <cstddef>

namespace RxDrain {

// Bound one service pass so a continuously refilled FIFO cannot monopolize the
// application task. Thirty-two leaves headroom above the three-packet hardware
// FIFO while still yielding predictably to the rest of the firmware.
constexpr size_t kDefaultMaxPacketsPerPoll = 32;

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
    bool stopped_early = false;

    while (result.processed < max_packets && hasPending()) {
        const StepResult step_result = step();
        if (step_result == StepResult::Failed) {
            result.receive_failed = true;
            break;
        }

        ++result.processed;
        if (step_result == StepResult::Stop) {
            stopped_early = true;
            break;
        }
    }

    result.guard_exhausted = !stopped_early && !result.receive_failed &&
                             result.processed >= max_packets && hasPending();
    return result;
}

}  // namespace RxDrain
