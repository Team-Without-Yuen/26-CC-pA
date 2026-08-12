#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>

namespace request_time_budget {

// Official per-request limits. Basic operations are netlist/log I/O; query,
// transformation, and optimization commands use the general request limit.
inline constexpr double kBasicRequestLimitSeconds = 60.0;
inline constexpr double kGeneralRequestLimitSeconds = 300.0;

// Backend work stops before the official limit so the caller can serialize the
// report, write artifacts, and emit the required request terminator.
inline constexpr double kBasicToolBudgetSeconds = 55.0;
inline constexpr double kGeneralToolBudgetSeconds = 290.0;

// A request owns one deadline. Nested backends receive this object by
// reference so entering another layer never restarts the request clock.
class RequestDeadline {
public:
    using Clock = std::chrono::steady_clock;

    explicit RequestDeadline(double budgetSeconds)
        : startedAt_(Clock::now()), budgetSeconds_(budgetSeconds) {}

    bool valid() const {
        return std::isfinite(budgetSeconds_) && budgetSeconds_ > 0.0;
    }

    double budgetSeconds() const { return budgetSeconds_; }

    double elapsedSeconds() const {
        return std::chrono::duration<double>(Clock::now() - startedAt_).count();
    }

    double remainingSeconds() const {
        return valid() ? std::max(0.0, budgetSeconds_ - elapsedSeconds()) : 0.0;
    }

    bool expired() const { return remainingSeconds() <= 0.0; }

    double boundedStageSeconds(double stageCapSeconds) const {
        if (!std::isfinite(stageCapSeconds) || stageCapSeconds <= 0.0) {
            return 0.0;
        }
        return std::min(stageCapSeconds, remainingSeconds());
    }

private:
    Clock::time_point startedAt_;
    double budgetSeconds_;
};

} // namespace request_time_budget
