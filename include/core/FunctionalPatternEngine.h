#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

class Netlist;
struct BitParallelSimulationResult;

namespace eqeng {
class Primitives;
}

namespace request_time_budget {
class RequestDeadline;
}

enum class FunctionalPatternKind {
    Buffer,
    Inverter,
    And,
    Nand,
    Or,
    Nor,
    Xor,
    Xnor,
    Mux,
    MuxHold
};

enum class FunctionalPatternProofStatus {
    ProvenMatch,
    ProvenNonMatch,
    Unknown,
    Unsupported
};

enum class FunctionalPatternRole {
    Target,
    Feedback,
    Control,
    Data
};

enum class FunctionalPatternDetectionMethod {
    FunctionalCofactorSat
};

enum class FunctionalPatternProofKind {
    EquivalentUnderCondition
};

struct FunctionalPatternRoleBinding {
    FunctionalPatternRole role = FunctionalPatternRole::Target;
    int netId = -1;
    std::string netName;
    bool inverted = false;
};

struct FunctionalPatternProofResult {
    FunctionalPatternProofKind kind =
        FunctionalPatternProofKind::EquivalentUnderCondition;
    int lhsNetId = -1;
    int rhsNetId = -1;
    int conditionNetId = -1;
    int conditionValue = -1;
    bool solverRan = false;
    bool proven = false;
    bool timedOut = false;
    bool unknown = false;
    bool unsupported = false;
    std::string solverStatus;
    std::string description;
};

struct FunctionalPatternProofRequest {
    FunctionalPatternKind kind = FunctionalPatternKind::Buffer;
    int targetNetId = -1;
    std::vector<int> operandNetIds;
    std::vector<bool> operandInverted;
};

struct FunctionalPatternEvaluation {
    FunctionalPatternKind kind = FunctionalPatternKind::Buffer;
    FunctionalPatternProofStatus status = FunctionalPatternProofStatus::Unknown;
    bool complete = false;
    bool solverRan = false;
    bool timedOut = false;
    std::string solverStatus;
    std::string message;
};

struct FunctionalPatternContext {
    int targetGateId = -1;
    int targetNetId = -1;
    int feedbackNetId = -1;
    const BitParallelSimulationResult* simulation = nullptr;
    std::vector<int> coneGateIds;
    std::vector<int> coneNetIds;
    std::vector<int> preferredControlNetIds;
    std::vector<int> preferredDataNetIds;
};

struct FunctionalPatternSearchOptions {
    size_t maxCandidates = 64;
    size_t maxMatches = 8;
    bool findAllMatches = true;
    bool resolveDataNets = true;
    size_t maxDataCandidatesPerMatch = 16;
    double timeLimitSeconds = 5.0;
};

struct FunctionalPatternMatch {
    FunctionalPatternKind kind = FunctionalPatternKind::MuxHold;
    FunctionalPatternDetectionMethod detectionMethod =
        FunctionalPatternDetectionMethod::FunctionalCofactorSat;
    std::vector<FunctionalPatternRoleBinding> bindings;
    std::vector<FunctionalPatternProofResult> proofs;
    int activeLevel = -1;
    int holdLevel = -1;
    bool proven = false;
    bool dataFunctionResolved = false;
    bool dataSearchAttempted = false;
    bool dataSearchComplete = true;
    bool dataSearchTimedOut = false;
    size_t dataCandidateCount = 0;
    size_t dataCandidatesExamined = 0;
    bool complete = true;
    std::vector<int> evidenceGateIds;
    std::string message;
};

struct FunctionalPatternSearchResult {
    bool ok = false;
    bool complete = true;
    bool timedOut = false;
    bool candidateLimitReached = false; // maxCandidates limited non-rejected candidates
    size_t candidateCount = 0;          // all structural control candidates
    size_t searchableCandidateCount = 0; // structural candidates not safely rejected by simulation
    size_t candidatesExamined = 0;      // searchable candidates entering the search loop
    size_t unexaminedCandidateCount = 0; // searchable candidates not entering the search loop
    size_t inconclusiveCandidateCount = 0; // entered candidates with timeout/unknown/unsupported work
    size_t simulationCandidateCount = 0;
    size_t simulationRejectedCandidateCount = 0;
    size_t satCheckCount = 0;
    double elapsedSeconds = 0.0;
    std::vector<FunctionalPatternMatch> matches;
    std::string status;
    std::string message;
};

class FunctionalPatternMatcher {
public:
    virtual ~FunctionalPatternMatcher() = default;
    virtual FunctionalPatternKind kind() const = 0;
    virtual FunctionalPatternSearchResult search(
        const Netlist& netlist,
        eqeng::Primitives& primitives,
        const FunctionalPatternContext& context,
        const FunctionalPatternSearchOptions& options,
        const request_time_budget::RequestDeadline& deadline) const = 0;
};

class FunctionalPatternEngine {
public:
    FunctionalPatternEngine();

    FunctionalPatternEvaluation proveSpecifiedOperands(
        const Netlist& netlist,
        eqeng::Primitives& primitives,
        const FunctionalPatternProofRequest& request,
        const request_time_budget::RequestDeadline& deadline) const;

    FunctionalPatternSearchResult search(
        FunctionalPatternKind kind,
        const Netlist& netlist,
        eqeng::Primitives& primitives,
        const FunctionalPatternContext& context,
        const FunctionalPatternSearchOptions& options,
        const request_time_budget::RequestDeadline& deadline) const;

private:
    std::vector<std::unique_ptr<FunctionalPatternMatcher>> matchers;
};

FunctionalPatternContext buildSequentialPatternContext(
    const Netlist& netlist,
    int dffGateId,
    int dNetId,
    int qNetId);
