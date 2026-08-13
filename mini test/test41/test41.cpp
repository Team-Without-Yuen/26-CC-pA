#include "include/core/EditFlow.h"
#include "include/core/NetlistQueries.h"
#include "include/core/OptimizationFlow.h"
#include "include/core/PathTypes.h"
#include "include/core/RequestTimeBudget.h"

#include <cmath>
#include <chrono>
#include <iostream>
#include <thread>

namespace {

bool equals(double lhs, double rhs) {
    return std::fabs(lhs - rhs) < 1.0e-9;
}

} // namespace

int main() {
    bool ok = true;
    auto expect = [&](bool condition, const char* name) {
        std::cout << (condition ? "PASS " : "FAIL ") << name << '\n';
        ok = ok && condition;
    };

    expect(equals(request_time_budget::kBasicRequestLimitSeconds, 60.0),
           "official basic request limit");
    expect(equals(request_time_budget::kGeneralRequestLimitSeconds, 300.0),
           "official general request limit");
    expect(equals(request_time_budget::kBasicToolBudgetSeconds, 55.0),
           "basic backend reserve");
    expect(equals(request_time_budget::kGeneralToolBudgetSeconds, 290.0),
           "general backend reserve");

    expect(equals(FunctionQuery{}.timeLimitSeconds, 290.0),
           "function query default");
    expect(equals(FunctionSearchQuery{}.timeLimitSeconds, 290.0),
           "function search default");
    expect(equals(PathQuery{}.enumerationTimeLimitSeconds, 290.0),
           "path query default");
    expect(equals(RegisterPathQuery{}.enumerationTimeLimitSeconds, 290.0),
           "register path query default");
    expect(equals(EditApplyRequest{}.timeLimitSeconds, 290.0),
           "edit apply default");
    expect(equals(OptApplyRequest{}.timeLimitSeconds, 290.0),
           "optimization apply default");
    expect(equals(SequentialPatternQuery{}.functionalTimeLimitSeconds, 290.0),
           "sequential total default");
    expect(equals(SequentialPatternQuery{}.functionalPerDffTimeLimitSeconds, 0.0),
           "sequential automatic per-DFF allocation");

    const request_time_budget::RequestDeadline liveDeadline(10.0);
    expect(liveDeadline.valid() && !liveDeadline.expired(),
           "shared deadline starts valid");
    expect(liveDeadline.boundedStageSeconds(3.0) > 0.0 &&
               liveDeadline.boundedStageSeconds(3.0) <= 3.0,
           "stage cap is bounded by shared deadline");

    const request_time_budget::RequestDeadline expiredDeadline(1.0e-6);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    expect(expiredDeadline.expired() &&
               equals(expiredDeadline.remainingSeconds(), 0.0),
           "exhausted shared deadline reports no remaining time");

    return ok ? 0 : 1;
}
