#pragma once

#include <string>
#include <vector>

enum class OptPassKind {
    Unknown,
    CleanupBufferChain,
    CollapseDoubleInverter,
    LocalSimplificationFixpoint
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
    bool validateEquivalence = false;
    bool rollbackOnFailure = true;
};

