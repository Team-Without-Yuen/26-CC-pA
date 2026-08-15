#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "include/SATEngine/Primitives.h"
#include "include/core/Netlist.h"
#include "include/io/VerilogReader.h"

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kColdRepeats = 3;
constexpr int kWarmRepeats = 10;

struct QueryCase {
    std::string design;
    std::string prompt;
    std::string verilogPath;
    FunctionQuery query;
};

struct Outcome {
    bool ok = true;
    bool exists = false;
    bool equivalent = false;
    bool canBeZero = false;
    bool canBeOne = false;
    bool isConstant = false;
    int constValue = -1;
    std::string status;
};

struct Measurements {
    double coldPhaseB = 0.0;
    double coldLegacy = 0.0;
    double warmPhaseB = 0.0;
    double warmLegacy = 0.0;
    double rebuildPhaseB = 0.0;
    double mutationLegacy = 0.0;
    bool coldMatch = false;
    bool warmMatch = false;
    bool mutationMatch = false;
};

struct SessionCase {
    std::string design;
    std::string verilogPath;
    std::vector<FunctionQuery> queries;
};

struct SessionMeasurements {
    double coldPhaseB = 0.0;
    double coldLegacy = 0.0;
    double warmPhaseB = 0.0;
    double warmLegacy = 0.0;
    bool match = false;
};

double elapsedSeconds(const Clock::time_point& startedAt) {
    return std::chrono::duration<double>(Clock::now() - startedAt).count();
}

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const size_t middle = values.size() / 2;
    if (values.size() % 2 != 0) return values[middle];
    return (values[middle - 1] + values[middle]) / 2.0;
}

FunctionQuery equivalence(const std::string& a, const std::string& b) {
    FunctionQuery query;
    query.type = FunctionQueryType::Equivalence;
    query.netNameA = a;
    query.netNameB = b;
    query.timeLimitSeconds = 30.0;
    return query;
}

FunctionQuery scalar(FunctionQueryType type,
                     const std::string& net,
                     int value = -1) {
    FunctionQuery query;
    query.type = type;
    query.netNameA = net;
    query.constValue = value;
    query.timeLimitSeconds = 30.0;
    return query;
}

const char* typeName(FunctionQueryType type) {
    switch (type) {
    case FunctionQueryType::Equivalence: return "Equivalence";
    case FunctionQueryType::CanBeValue: return "CanBeValue";
    case FunctionQueryType::ConstantFunction: return "ConstantFunction";
    case FunctionQueryType::AlwaysZero: return "AlwaysZero";
    case FunctionQueryType::AlwaysOne: return "AlwaysOne";
    case FunctionQueryType::TruthStatus: return "TruthStatus";
    default: return "Unsupported";
    }
}

bool loadNetlist(const std::string& path, Netlist& netlist) {
    VerilogReader reader;
    return reader.read(path, netlist);
}

eqeng::Primitives::Config phaseBConfig() {
    eqeng::Primitives::Config config;
    config.verbose_rebuild = false;
    config.fraig_auto_sweep_threshold = 0;
    return config;
}

Outcome runPhaseB(eqeng::Primitives& primitives, const FunctionQuery& query) {
    Outcome result;
    result.constValue = query.constValue;
    if (query.type == FunctionQueryType::Equivalence) {
        const eqeng::EquivResult proof = primitives.equiv_checked(
            primitives.resolve(query.netNameA),
            primitives.resolve(query.netNameB),
            query.timeLimitSeconds);
        result.ok = proof != eqeng::EquivResult::Unknown;
        result.equivalent = proof == eqeng::EquivResult::Equal;
        result.exists = result.equivalent;
        result.status = result.ok
            ? (result.equivalent ? "EQUIVALENT" : "NOT_EQUIVALENT")
            : (primitives.last_proof_timed_out()
                   ? "SOLVER_TIMEOUT" : "SOLVER_UNKNOWN");
        return result;
    }

    const eqeng::SigRef signal = primitives.resolve(query.netNameA);
    const double perValueLimit = query.timeLimitSeconds / 2.0;
    const eqeng::EquivResult constOne =
        primitives.is_const_checked(signal, true, perValueLimit);
    const eqeng::EquivResult constZero =
        primitives.is_const_checked(signal, false, perValueLimit);
    if (constOne == eqeng::EquivResult::Unknown ||
        constZero == eqeng::EquivResult::Unknown) {
        result.ok = false;
        result.status = primitives.last_proof_timed_out()
            ? "SOLVER_TIMEOUT" : "SOLVER_UNKNOWN";
        return result;
    }
    result.canBeZero = constOne != eqeng::EquivResult::Equal;
    result.canBeOne = constZero != eqeng::EquivResult::Equal;
    const bool isZero = result.canBeZero && !result.canBeOne;
    const bool isOne = !result.canBeZero && result.canBeOne;

    switch (query.type) {
    case FunctionQueryType::CanBeValue:
        result.exists = query.constValue == 0
            ? result.canBeZero : result.canBeOne;
        result.status = result.exists
            ? "SATISFIABLE_VALUE" : "UNSATISFIABLE_VALUE";
        break;
    case FunctionQueryType::ConstantFunction:
        result.exists = query.constValue == 0 ? isZero : isOne;
        result.isConstant = result.exists;
        result.status = result.exists
            ? (query.constValue == 0 ? "ALWAYS_ZERO" : "ALWAYS_ONE")
            : "NOT_REQUESTED_CONSTANT";
        break;
    case FunctionQueryType::AlwaysZero:
        result.constValue = 0;
        result.exists = isZero;
        result.isConstant = isZero;
        result.status = isZero ? "ALWAYS_ZERO" : "NOT_ALWAYS_ZERO";
        break;
    case FunctionQueryType::AlwaysOne:
        result.constValue = 1;
        result.exists = isOne;
        result.isConstant = isOne;
        result.status = isOne ? "ALWAYS_ONE" : "NOT_ALWAYS_ONE";
        break;
    case FunctionQueryType::TruthStatus:
        result.exists = true;
        if (isZero) {
            result.isConstant = true;
            result.constValue = 0;
            result.status = "ALWAYS_ZERO";
        } else if (isOne) {
            result.isConstant = true;
            result.constValue = 1;
            result.status = "ALWAYS_ONE";
        } else {
            result.constValue = -1;
            result.status = "NON_CONSTANT";
        }
        break;
    default:
        result.ok = false;
        result.status = "UNSUPPORTED_BENCHMARK_MODE";
        break;
    }
    return result;
}

Outcome runLegacy(const Netlist& netlist, const FunctionQuery& query) {
    Outcome result;
    result.constValue = query.constValue;
    if (query.type == FunctionQueryType::Equivalence) {
        result.equivalent = netlist.checkEquivalence(
            query.netNameA, query.netNameB);
        result.exists = result.equivalent;
        result.status = result.equivalent ? "EQUIVALENT" : "NOT_EQUIVALENT";
        return result;
    }

    // Match the high-level report workload: every constant-family query
    // obtains both can-be-0 and can-be-1 facts.
    result.canBeZero = netlist.canNetBeValue(query.netNameA, 0);
    result.canBeOne = netlist.canNetBeValue(query.netNameA, 1);
    const bool isZero = result.canBeZero && !result.canBeOne;
    const bool isOne = !result.canBeZero && result.canBeOne;

    switch (query.type) {
    case FunctionQueryType::CanBeValue:
        result.exists = query.constValue == 0
            ? result.canBeZero : result.canBeOne;
        result.status = result.exists
            ? "SATISFIABLE_VALUE" : "UNSATISFIABLE_VALUE";
        break;
    case FunctionQueryType::ConstantFunction:
        result.exists = query.constValue == 0 ? isZero : isOne;
        result.isConstant = result.exists;
        result.status = result.exists
            ? (query.constValue == 0 ? "ALWAYS_ZERO" : "ALWAYS_ONE")
            : "NOT_REQUESTED_CONSTANT";
        break;
    case FunctionQueryType::AlwaysZero:
        result.constValue = 0;
        result.exists = isZero;
        result.isConstant = isZero;
        result.status = isZero ? "ALWAYS_ZERO" : "NOT_ALWAYS_ZERO";
        break;
    case FunctionQueryType::AlwaysOne:
        result.constValue = 1;
        result.exists = isOne;
        result.isConstant = isOne;
        result.status = isOne ? "ALWAYS_ONE" : "NOT_ALWAYS_ONE";
        break;
    case FunctionQueryType::TruthStatus:
        result.exists = true;
        if (isZero) {
            result.isConstant = true;
            result.constValue = 0;
            result.status = "ALWAYS_ZERO";
        } else if (isOne) {
            result.isConstant = true;
            result.constValue = 1;
            result.status = "ALWAYS_ONE";
        } else {
            result.constValue = -1;
            result.status = "NON_CONSTANT";
        }
        break;
    default:
        result.ok = false;
        result.status = "UNSUPPORTED_BENCHMARK_MODE";
        break;
    }
    return result;
}

bool sameOutcome(const Outcome& a, const Outcome& b) {
    return a.ok == b.ok && a.exists == b.exists &&
           a.equivalent == b.equivalent &&
           a.canBeZero == b.canBeZero && a.canBeOne == b.canBeOne &&
           a.isConstant == b.isConstant && a.constValue == b.constValue &&
           a.status == b.status;
}

void addHarmlessMutation(Netlist& netlist, const std::string& sourceName) {
    const int output = netlist.addNet("__phase_b_benchmark_unused");
    const int gate = netlist.addGate(
        "__phase_b_benchmark_buf", GateType::BUF);
    netlist.connectGateInput(gate, netlist.getNetId(sourceName));
    netlist.connectGateOutput(gate, output);
}

Measurements measure(const QueryCase& item) {
    Measurements result;
    std::vector<double> phaseBCold;
    std::vector<double> legacyCold;
    bool coldMatch = true;

    for (int repeat = 0; repeat < kColdRepeats; ++repeat) {
        Netlist phaseBNetlist;
        Netlist legacyNetlist;
        if (!loadNetlist(item.verilogPath, phaseBNetlist) ||
            !loadNetlist(item.verilogPath, legacyNetlist)) {
            throw std::runtime_error("Cannot load " + item.verilogPath);
        }

        auto startedAt = Clock::now();
        eqeng::Primitives phaseBPrimitives(
            phaseBNetlist, {}, phaseBConfig());
        phaseBPrimitives.enable_phase_b(true);
        const Outcome phaseB = runPhaseB(phaseBPrimitives, item.query);
        phaseBCold.push_back(elapsedSeconds(startedAt));

        startedAt = Clock::now();
        const Outcome legacy = runLegacy(legacyNetlist, item.query);
        legacyCold.push_back(elapsedSeconds(startedAt));
        coldMatch = coldMatch && sameOutcome(phaseB, legacy);
    }

    result.coldPhaseB = median(phaseBCold);
    result.coldLegacy = median(legacyCold);
    result.coldMatch = coldMatch;

    Netlist phaseBWarmNetlist;
    Netlist legacyWarmNetlist;
    if (!loadNetlist(item.verilogPath, phaseBWarmNetlist) ||
        !loadNetlist(item.verilogPath, legacyWarmNetlist)) {
        throw std::runtime_error("Cannot load " + item.verilogPath);
    }

    eqeng::Primitives phaseBWarmPrimitives(
        phaseBWarmNetlist, {}, phaseBConfig());
    phaseBWarmPrimitives.enable_phase_b(true);
    const Outcome phaseBReference = runPhaseB(
        phaseBWarmPrimitives, item.query);
    const Outcome legacyReference = runLegacy(legacyWarmNetlist, item.query);
    std::vector<double> phaseBWarm;
    std::vector<double> legacyWarm;
    bool warmMatch = sameOutcome(phaseBReference, legacyReference);
    for (int repeat = 0; repeat < kWarmRepeats; ++repeat) {
        auto startedAt = Clock::now();
        const Outcome phaseB = runPhaseB(
            phaseBWarmPrimitives, item.query);
        phaseBWarm.push_back(elapsedSeconds(startedAt));

        startedAt = Clock::now();
        const Outcome legacy = runLegacy(legacyWarmNetlist, item.query);
        legacyWarm.push_back(elapsedSeconds(startedAt));
        warmMatch = warmMatch && sameOutcome(phaseB, legacy);
    }
    result.warmPhaseB = median(phaseBWarm);
    result.warmLegacy = median(legacyWarm);
    result.warmMatch = warmMatch;

    addHarmlessMutation(phaseBWarmNetlist, item.query.netNameA);
    addHarmlessMutation(legacyWarmNetlist, item.query.netNameA);
    auto startedAt = Clock::now();
    const Outcome phaseBAfterMutation = runPhaseB(
        phaseBWarmPrimitives, item.query);
    result.rebuildPhaseB = elapsedSeconds(startedAt);
    startedAt = Clock::now();
    const Outcome legacyAfterMutation = runLegacy(legacyWarmNetlist, item.query);
    result.mutationLegacy = elapsedSeconds(startedAt);
    result.mutationMatch = sameOutcome(
        phaseBAfterMutation, legacyAfterMutation);
    return result;
}

SessionMeasurements measureSession(const SessionCase& item) {
    SessionMeasurements result;
    std::vector<double> coldPhaseB;
    std::vector<double> coldLegacy;
    std::vector<double> warmPhaseB;
    std::vector<double> warmLegacy;
    bool allMatch = true;

    for (int repeat = 0; repeat < kColdRepeats; ++repeat) {
        Netlist phaseBNetlist;
        Netlist legacyNetlist;
        if (!loadNetlist(item.verilogPath, phaseBNetlist) ||
            !loadNetlist(item.verilogPath, legacyNetlist)) {
            throw std::runtime_error("Cannot load " + item.verilogPath);
        }

        std::vector<Outcome> phaseBOutcomes;
        auto startedAt = Clock::now();
        eqeng::Primitives phaseBPrimitives(
            phaseBNetlist, {}, phaseBConfig());
        phaseBPrimitives.enable_phase_b(true);
        for (const FunctionQuery& query : item.queries) {
            phaseBOutcomes.push_back(runPhaseB(phaseBPrimitives, query));
        }
        coldPhaseB.push_back(elapsedSeconds(startedAt));

        std::vector<Outcome> legacyOutcomes;
        startedAt = Clock::now();
        for (const FunctionQuery& query : item.queries) {
            legacyOutcomes.push_back(runLegacy(legacyNetlist, query));
        }
        coldLegacy.push_back(elapsedSeconds(startedAt));
        for (size_t i = 0; i < phaseBOutcomes.size(); ++i) {
            allMatch = allMatch && sameOutcome(
                phaseBOutcomes[i], legacyOutcomes[i]);
        }

        phaseBOutcomes.clear();
        startedAt = Clock::now();
        for (const FunctionQuery& query : item.queries) {
            phaseBOutcomes.push_back(runPhaseB(phaseBPrimitives, query));
        }
        warmPhaseB.push_back(elapsedSeconds(startedAt));

        legacyOutcomes.clear();
        startedAt = Clock::now();
        for (const FunctionQuery& query : item.queries) {
            legacyOutcomes.push_back(runLegacy(legacyNetlist, query));
        }
        warmLegacy.push_back(elapsedSeconds(startedAt));
        for (size_t i = 0; i < phaseBOutcomes.size(); ++i) {
            allMatch = allMatch && sameOutcome(
                phaseBOutcomes[i], legacyOutcomes[i]);
        }
    }

    result.coldPhaseB = median(coldPhaseB);
    result.coldLegacy = median(coldLegacy);
    result.warmPhaseB = median(warmPhaseB);
    result.warmLegacy = median(warmLegacy);
    result.match = allMatch;
    return result;
}

std::vector<QueryCase> cases() {
    const std::string t17 = "NewTestCase/test17/test17.v";
    const std::string t18 = "NewTestCase/test18/test18.v";
    const std::string t19 = "NewTestCase/test19/test19.v";
    const std::string t20 = "NewTestCase/test20/test20.v";
    const std::string t31 = "NewTestCase/test31/test31.v";
    const std::string t33 = "NewTestCase/test33/test33.v";
    const std::string t35 = "NewTestCase/test35/test35.v";
    return {
        {"test17", "prompt15", t17, equivalence("n2122", "n2116")},
        {"test18", "prompt14", t18, equivalence("n848", "n1080")},
        {"test18", "prompt15", t18, equivalence("n1039", "n1046")},
        {"test18", "prompt16", t18, equivalence("n1035", "n1029")},
        {"test19", "prompt15", t19, equivalence("n296", "n269")},
        {"test19", "prompt16", t19, equivalence("n273", "n275")},
        {"test19", "prompt17", t19, equivalence("n270", "n243")},
        {"test20", "prompt15", t20, equivalence("n13082", "n13083")},
        {"test20", "prompt16", t20, equivalence("n2257", "n2184")},
        {"test20", "prompt17", t20, equivalence("n2233", "n2193")},
        {"test31", "prompt8", t31, scalar(FunctionQueryType::AlwaysZero, "n16")},
        {"test31", "derived_always_one", t31,
            scalar(FunctionQueryType::AlwaysOne, "n16")},
        {"test31", "derived_truth_status", t31,
            scalar(FunctionQueryType::TruthStatus, "n16")},
        {"test31", "derived_constant_zero", t31,
            scalar(FunctionQueryType::ConstantFunction, "n16", 0)},
        {"test31", "derived_can_be_zero", t31,
            scalar(FunctionQueryType::CanBeValue, "n16", 0)},
        {"test31", "derived_can_be_one", t31,
            scalar(FunctionQueryType::CanBeValue, "n16", 1)},
        {"test31", "prompt19", t31,
            equivalence("n1287", "n2404")},
        {"test33", "prompt11", t33,
            equivalence("n55146", "n55104")},
        {"test35", "prompt12", t35,
            equivalence("n29498", "n29471")}
    };
}

std::vector<SessionCase> sessions() {
    return {
        {"test17", "NewTestCase/test17/test17.v", {
            equivalence("n2122", "n2116")}},
        {"test18", "NewTestCase/test18/test18.v", {
            equivalence("n848", "n1080"),
            equivalence("n1039", "n1046"),
            equivalence("n1035", "n1029")}},
        {"test19", "NewTestCase/test19/test19.v", {
            equivalence("n296", "n269"),
            equivalence("n273", "n275"),
            equivalence("n270", "n243")}},
        {"test20", "NewTestCase/test20/test20.v", {
            equivalence("n13082", "n13083"),
            equivalence("n2257", "n2184"),
            equivalence("n2233", "n2193")}},
        {"test31", "NewTestCase/test31/test31.v", {
            scalar(FunctionQueryType::AlwaysZero, "n16"),
            equivalence("n1287", "n2404")}},
        {"test33", "NewTestCase/test33/test33.v", {
            equivalence("n55146", "n55104")}},
        {"test35", "NewTestCase/test35/test35.v", {
            equivalence("n29498", "n29471")}}
    };
}

} // namespace

int main() {
    const std::string outputPath =
        "mini test/test43/newtestcase_phase_b_benchmark.tsv";
    std::ofstream output(outputPath);
    if (!output) {
        std::cerr << "Cannot open " << outputPath << '\n';
        return 2;
    }

    output << "design\tprompt\ttype\tcold_match\twarm_match\tmutation_match"
              "\tphase_b_cold_s\tlegacy_cold_s\tcold_speedup"
              "\tphase_b_warm_s\tlegacy_warm_s\twarm_speedup"
              "\tphase_b_rebuild_s\tlegacy_after_mutation_s\n";

    int matched = 0;
    int total = 0;
    int phaseBColdWins = 0;
    int phaseBWarmWins = 0;
    for (const QueryCase& item : cases()) {
        const Measurements timing = measure(item);
        const bool allMatch = timing.coldMatch && timing.warmMatch &&
                              timing.mutationMatch;
        ++total;
        matched += allMatch ? 1 : 0;
        phaseBColdWins += timing.coldPhaseB < timing.coldLegacy ? 1 : 0;
        phaseBWarmWins += timing.warmPhaseB < timing.warmLegacy ? 1 : 0;
        const double coldSpeedup = timing.coldPhaseB > 0.0
            ? timing.coldLegacy / timing.coldPhaseB : 0.0;
        const double warmSpeedup = timing.warmPhaseB > 0.0
            ? timing.warmLegacy / timing.warmPhaseB : 0.0;
        output << item.design << '\t' << item.prompt << '\t'
               << typeName(item.query.type) << '\t'
               << (timing.coldMatch ? "yes" : "no") << '\t'
               << (timing.warmMatch ? "yes" : "no") << '\t'
               << (timing.mutationMatch ? "yes" : "no") << '\t'
               << std::fixed << std::setprecision(9)
               << timing.coldPhaseB << '\t' << timing.coldLegacy << '\t'
               << coldSpeedup << '\t'
               << timing.warmPhaseB << '\t' << timing.warmLegacy << '\t'
               << warmSpeedup << '\t'
               << timing.rebuildPhaseB << '\t'
               << timing.mutationLegacy << '\n';
    }

    output << "SUMMARY\tcases=" << total << "\tsemantic_matches=" << matched
           << "\tphase_b_cold_wins=" << phaseBColdWins
           << "\tphase_b_warm_wins=" << phaseBWarmWins << '\n';
    output.close();

    const std::string sessionOutputPath =
        "mini test/test43/newtestcase_phase_b_session_benchmark.tsv";
    std::ofstream sessionOutput(sessionOutputPath);
    if (!sessionOutput) {
        std::cerr << "Cannot open " << sessionOutputPath << '\n';
        return 2;
    }
    sessionOutput << "design\tquery_count\tsemantic_match"
                     "\tphase_b_cold_session_s\tlegacy_cold_session_s"
                     "\tcold_session_speedup\tphase_b_warm_session_s"
                     "\tlegacy_warm_session_s\twarm_session_speedup\n";
    int sessionMatches = 0;
    int phaseBSessionColdWins = 0;
    int phaseBSessionWarmWins = 0;
    const std::vector<SessionCase> sessionCases = sessions();
    for (const SessionCase& item : sessionCases) {
        const SessionMeasurements timing = measureSession(item);
        sessionMatches += timing.match ? 1 : 0;
        phaseBSessionColdWins += timing.coldPhaseB < timing.coldLegacy ? 1 : 0;
        phaseBSessionWarmWins += timing.warmPhaseB < timing.warmLegacy ? 1 : 0;
        sessionOutput << item.design << '\t' << item.queries.size() << '\t'
                      << (timing.match ? "yes" : "no") << '\t'
                      << std::fixed << std::setprecision(9)
                      << timing.coldPhaseB << '\t' << timing.coldLegacy << '\t'
                      << timing.coldLegacy / timing.coldPhaseB << '\t'
                      << timing.warmPhaseB << '\t' << timing.warmLegacy << '\t'
                      << timing.warmLegacy / timing.warmPhaseB << '\n';
    }
    sessionOutput << "SUMMARY\tsessions=" << sessionCases.size()
                  << "\tsemantic_matches=" << sessionMatches
                  << "\tphase_b_cold_wins=" << phaseBSessionColdWins
                  << "\tphase_b_warm_wins=" << phaseBSessionWarmWins << '\n';
    sessionOutput.close();

    std::cout << "NewTestCase Phase B benchmark: semantic matches "
              << matched << '/' << total << ", cold wins "
              << phaseBColdWins << '/' << total << ", warm wins "
              << phaseBWarmWins << '/' << total << '\n'
              << "Session benchmark: semantic matches " << sessionMatches << '/'
              << sessionCases.size() << ", cold wins "
              << phaseBSessionColdWins << '/' << sessionCases.size()
              << ", warm wins " << phaseBSessionWarmWins << '/'
              << sessionCases.size() << '\n'
              << "Details: " << outputPath << " and "
              << sessionOutputPath << '\n';
    return matched == total && sessionMatches ==
        static_cast<int>(sessionCases.size()) ? 0 : 1;
}
