#pragma once

#include <string>
#include <vector>

#include "include/core/EditFlow.h"
#include "include/core/RequestTimeBudget.h"

enum class OptPassKind {
    Unknown,
    CleanupBufferChain,
    CollapseDoubleInverter,
    LocalSimplificationFixpoint,
    CriticalPathDepth
};

enum class OptDepthObjective {
    GlobalMaximum,
    ScopedFaninCone
};

struct OptQueryRequest {
    OptPassKind passKind = OptPassKind::Unknown;
    std::string scopeName;
};

struct OptCandidate {
    int id = -1;
    OptPassKind passKind = OptPassKind::Unknown;
    std::string reason;

    std::vector<int> gateIds;
    std::vector<int> netIds;
    std::vector<std::string> gateNames;
    std::vector<std::string> netNames;

    int estimatedGateDelta = 0;
    int estimatedNetDelta = 0;
    bool requiresEquivalenceCheck = true;
};

struct OptQueryReport {
    bool ok = false;
    OptPassKind passKind = OptPassKind::Unknown;
    std::string scopeName;
    std::vector<OptCandidate> candidates;
    std::vector<std::string> warnings;
    std::string message;
};

struct OptApplyRequest {
    OptPassKind passKind = OptPassKind::Unknown;
    std::vector<int> candidateIds;

    // 成本函數的作用域（= prompt 裡 "The cost function is ..." 那句）
    TargetScope scope = TargetScope::WHOLE_NETLIST;
    std::string scopeName;
    OptDepthObjective depthObjective = OptDepthObjective::GlobalMaximum;
    // 基底約束的作用域（= prompt 裡 "ensuring ... only ... gates" 那句）。
    // WHOLE_NETLIST = 約束整張 netlist；設成 cone = 只約束該 cone，cone 外不受限。
    // 與 scope/scopeName 完全獨立，例如：
    //   "Optimize the logic cone of n14 ... cost is the depth of the cone of n14"
    //   + "ensuring the netlist remains NAND and NOT only"
    //   → scope = NET_FANIN/"n14"，basisScope = WHOLE_NETLIST
    TargetScope basisScope = TargetScope::WHOLE_NETLIST;
    std::string basisScopeName;
    std::vector<GateType> allowedTypes;
    std::vector<GateType> bannedTypes;
    int targetDepth = -1;
    double timeLimitSeconds = request_time_budget::kGeneralToolBudgetSeconds;
    bool requireDepthImprovement = true;
    bool verbose = false;

    bool validateEquivalence = false;
    bool rollbackOnFailure = true;
};
