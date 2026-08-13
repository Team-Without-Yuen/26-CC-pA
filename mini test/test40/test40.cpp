#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "include/SATEngine/Primitives.h"
#include "include/core/Netlist.h"
#include "include/io/VerilogReader.h"

namespace {

using Clock = std::chrono::steady_clock;

struct QueryCase {
    std::string label;
    FunctionQuery query;
    bool expectSemanticMatch = true;
};

struct DesignCase {
    std::string label;
    std::string verilogPath;
    std::vector<QueryCase> queries;
};

struct AigOutcome {
    bool ok = false;
    bool exists = false;
    bool equivalent = false;
    bool canBeZero = false;
    bool canBeOne = false;
    bool isConstant = false;
    bool timedOut = false;
    int constValue = -1;
    std::string status;
};

double elapsedSeconds(const Clock::time_point& startedAt) {
    return std::chrono::duration<double>(Clock::now() - startedAt).count();
}

FunctionQuery equivalence(const std::string& a, const std::string& b) {
    FunctionQuery query;
    query.type = FunctionQueryType::Equivalence;
    query.netNameA = a;
    query.netNameB = b;
    query.timeLimitSeconds = 30.0;
    return query;
}

FunctionQuery scalarQuery(FunctionQueryType type,
                          const std::string& net,
                          int value = -1) {
    FunctionQuery query;
    query.type = type;
    query.netNameA = net;
    query.constValue = value;
    query.timeLimitSeconds = 30.0;
    return query;
}

FunctionQuery withTimeLimit(FunctionQuery query, double seconds) {
    query.timeLimitSeconds = seconds;
    return query;
}

int addGate(Netlist& netlist,
            const std::string& name,
            GateType type,
            const std::vector<std::string>& inputs,
            const std::string& output) {
    const int outputId = netlist.addNet(output);
    const int gateId = netlist.addGate(name, type);
    for (const std::string& input : inputs) {
        netlist.connectGateInput(gateId, netlist.getNetId(input));
    }
    netlist.connectGateOutput(gateId, outputId);
    return outputId;
}

Netlist makeSyntheticCircuit() {
    Netlist netlist;
    netlist.addPrimaryInput("a");
    netlist.addPrimaryInput("b");
    netlist.addPrimaryInput("c");
    addGate(netlist, "g_and3", GateType::AND, {"a", "b", "c"}, "and3");
    addGate(netlist, "g_and_ab", GateType::AND, {"a", "b"}, "and_ab");
    addGate(netlist, "g_and_ref", GateType::AND, {"and_ab", "c"}, "and_ref");
    addGate(netlist, "g_or", GateType::OR, {"a", "b"}, "variable");
    addGate(netlist, "g_zero", GateType::XOR, {"a", "a"}, "const0");
    addGate(netlist, "g_one", GateType::XNOR, {"b", "b"}, "const1");
    return netlist;
}

std::vector<DesignCase> makeDesignCases() {
    return {
        {"synthetic", "", {
            {"three_input_equivalent", equivalence("and3", "and_ref")},
            {"non_equivalent", equivalence("and3", "variable")},
            {"always_zero", scalarQuery(FunctionQueryType::AlwaysZero, "const0")},
            {"always_one", scalarQuery(FunctionQueryType::AlwaysOne, "const1")},
            {"truth_non_constant", scalarQuery(FunctionQueryType::TruthStatus, "variable")},
            {"constant_zero_yes", scalarQuery(FunctionQueryType::ConstantFunction, "const0", 0)},
            {"constant_zero_no", scalarQuery(FunctionQueryType::ConstantFunction, "const0", 1)},
            {"can_be_zero", scalarQuery(FunctionQueryType::CanBeValue, "variable", 0)},
            {"can_be_one", scalarQuery(FunctionQueryType::CanBeValue, "variable", 1)}
        }},
        {"test17", "NewTestCase/test17/test17.v", {
            {"prompt_15", equivalence("n2122", "n2116")}
        }},
        {"test18", "NewTestCase/test18/test18.v", {
            {"prompt_14", equivalence("n848", "n1080")},
            {"prompt_15", equivalence("n1039", "n1046")},
            {"prompt_16", equivalence("n1035", "n1029")}
        }},
        {"test19", "NewTestCase/test19/test19.v", {
            {"prompt_15", equivalence("n296", "n269")},
            {"prompt_16", equivalence("n273", "n275")},
            {"prompt_17", equivalence("n270", "n243")}
        }},
        {"test20", "NewTestCase/test20/test20.v", {
            {"prompt_15", equivalence("n13082", "n13083")},
            {"prompt_16", equivalence("n2257", "n2184")},
            {"prompt_17", equivalence("n2233", "n2193")}
        }},
        {"test31", "NewTestCase/test31/test31.v", {
            {"prompt_8", scalarQuery(FunctionQueryType::AlwaysZero, "n16")},
            {"prompt_19", equivalence("n1287", "n2404")}
        }},
        {"test33", "NewTestCase/test33/test33.v", {
            {"prompt_11", equivalence("n55146", "n55104")},
            {"prompt_11_warm_repeat", equivalence("n55146", "n55104")},
            {"prompt_11_tiny_budget",
             withTimeLimit(equivalence("n55146", "n55104"), 1e-12), false}
        }},
        {"test35", "NewTestCase/test35/test35.v", {
            {"prompt_12", equivalence("n29498", "n29471")},
            {"prompt_12_warm_repeat", equivalence("n29498", "n29471")}
        }}
    };
}

AigOutcome runAig(eqeng::Primitives& primitives, const FunctionQuery& query) {
    AigOutcome outcome;
    outcome.constValue = query.constValue;

    if (query.type == FunctionQueryType::Equivalence) {
        const eqeng::EquivResult result = primitives.equiv_checked(
            primitives.resolve(query.netNameA),
            primitives.resolve(query.netNameB),
            query.timeLimitSeconds);
        if (result == eqeng::EquivResult::Unknown) {
            outcome.timedOut = primitives.last_proof_timed_out();
            outcome.status = outcome.timedOut ? "SOLVER_TIMEOUT" : "SOLVER_UNKNOWN";
            return outcome;
        }
        outcome.ok = true;
        outcome.equivalent = result == eqeng::EquivResult::Equal;
        outcome.exists = outcome.equivalent;
        outcome.status = outcome.equivalent ? "EQUIVALENT" : "NOT_EQUIVALENT";
        return outcome;
    }

    const eqeng::SigRef signal = primitives.resolve(query.netNameA);
    const double perValueLimit = query.timeLimitSeconds / 2.0;
    const eqeng::EquivResult constZero =
        primitives.is_const_checked(signal, false, perValueLimit);
    const bool firstTimedOut = primitives.last_proof_timed_out();
    const eqeng::EquivResult constOne =
        primitives.is_const_checked(signal, true, perValueLimit);
    const bool secondTimedOut = primitives.last_proof_timed_out();
    if (constZero == eqeng::EquivResult::Unknown ||
        constOne == eqeng::EquivResult::Unknown) {
        outcome.timedOut = firstTimedOut || secondTimedOut;
        outcome.status = outcome.timedOut ? "SOLVER_TIMEOUT" : "SOLVER_UNKNOWN";
        return outcome;
    }

    const bool isConstZero = constZero == eqeng::EquivResult::Equal;
    const bool isConstOne = constOne == eqeng::EquivResult::Equal;
    outcome.ok = true;
    outcome.canBeZero = !isConstOne;
    outcome.canBeOne = !isConstZero;

    switch (query.type) {
    case FunctionQueryType::CanBeValue:
        outcome.exists = query.constValue == 0
            ? outcome.canBeZero : outcome.canBeOne;
        outcome.status = outcome.exists
            ? "SATISFIABLE_VALUE" : "UNSATISFIABLE_VALUE";
        break;
    case FunctionQueryType::ConstantFunction:
        outcome.exists = query.constValue == 0 ? isConstZero : isConstOne;
        outcome.isConstant = outcome.exists;
        outcome.status = outcome.exists
            ? (query.constValue == 0 ? "ALWAYS_ZERO" : "ALWAYS_ONE")
            : "NOT_REQUESTED_CONSTANT";
        break;
    case FunctionQueryType::AlwaysZero:
        outcome.constValue = 0;
        outcome.exists = isConstZero;
        outcome.isConstant = outcome.exists;
        outcome.status = outcome.exists ? "ALWAYS_ZERO" : "NOT_ALWAYS_ZERO";
        break;
    case FunctionQueryType::AlwaysOne:
        outcome.constValue = 1;
        outcome.exists = isConstOne;
        outcome.isConstant = outcome.exists;
        outcome.status = outcome.exists ? "ALWAYS_ONE" : "NOT_ALWAYS_ONE";
        break;
    case FunctionQueryType::TruthStatus:
        outcome.exists = true;
        if (isConstZero) {
            outcome.isConstant = true;
            outcome.constValue = 0;
            outcome.status = "ALWAYS_ZERO";
        } else if (isConstOne) {
            outcome.isConstant = true;
            outcome.constValue = 1;
            outcome.status = "ALWAYS_ONE";
        } else {
            outcome.constValue = -1;
            outcome.status = "NON_CONSTANT";
        }
        break;
    default:
        outcome.ok = false;
        outcome.status = "UNSUPPORTED_BENCHMARK_MODE";
        break;
    }
    return outcome;
}

bool sameSemantics(const FunctionReport& legacy,
                   const AigOutcome& aig,
                   FunctionQueryType type) {
    if (legacy.ok != aig.ok || legacy.status != aig.status ||
        legacy.exists != aig.exists) {
        return false;
    }
    if (!legacy.ok) return legacy.solverTimedOut == aig.timedOut;
    if (type == FunctionQueryType::Equivalence) {
        return legacy.equivalent == aig.equivalent;
    }
    return legacy.canBeZero == aig.canBeZero &&
           legacy.canBeOne == aig.canBeOne &&
           legacy.isConstant == aig.isConstant &&
           legacy.constValue == aig.constValue;
}

const char* queryTypeName(FunctionQueryType type) {
    switch (type) {
    case FunctionQueryType::Equivalence: return "Equivalence";
    case FunctionQueryType::CanBeValue: return "CanBeValue";
    case FunctionQueryType::ConstantFunction: return "ConstantFunction";
    case FunctionQueryType::AlwaysZero: return "AlwaysZero";
    case FunctionQueryType::AlwaysOne: return "AlwaysOne";
    case FunctionQueryType::TruthStatus: return "TruthStatus";
    default: return "Other";
    }
}

} // namespace

int main() {
    const std::string outputPath =
        "mini test/test40/function_query_aig_differential_results.txt";
    std::ofstream output(outputPath);
    if (!output) {
        std::cerr << "Cannot open " << outputPath << '\n';
        return 2;
    }

    output << "design\tcase\ttype\tlegacy_status\taig_status\tmatch"
              "\texpected_match\texpectation_met\tlegacy_seconds"
              "\taig_build_seconds\taig_proof_seconds\n";

    int total = 0;
    int matched = 0;
    int expectationsMet = 0;
    bool passed = true;
    for (const DesignCase& design : makeDesignCases()) {
        Netlist netlist;
        if (design.verilogPath.empty()) {
            netlist = makeSyntheticCircuit();
        } else {
            VerilogReader reader;
            if (!reader.read(design.verilogPath, netlist)) {
                std::cerr << "Failed to read " << design.verilogPath << '\n';
                passed = false;
                continue;
            }
        }

        eqeng::Primitives::Config config;
        config.verbose_rebuild = false;
        eqeng::Primitives primitives(netlist, {}, config);
        const auto buildStartedAt = Clock::now();
        const eqeng::ModelHealth health = primitives.model_health();
        const double buildSeconds = elapsedSeconds(buildStartedAt);
        if (health == eqeng::ModelHealth::Invalid) {
            std::cerr << "Invalid AIG model for " << design.label << ": "
                      << primitives.model_health_message() << '\n';
            passed = false;
            continue;
        }

        bool firstAigQuery = true;
        for (const QueryCase& item : design.queries) {
            const auto legacyStartedAt = Clock::now();
            const FunctionReport legacy = netlist.runFunctionQuery(item.query);
            const double legacySeconds = elapsedSeconds(legacyStartedAt);

            const auto aigStartedAt = Clock::now();
            const AigOutcome aig = runAig(primitives, item.query);
            const double aigProofSeconds = elapsedSeconds(aigStartedAt);
            const bool match = sameSemantics(legacy, aig, item.query.type);
            const bool expectationMet = match == item.expectSemanticMatch;

            ++total;
            matched += match ? 1 : 0;
            expectationsMet += expectationMet ? 1 : 0;
            passed = passed && expectationMet;
            output << design.label << '\t' << item.label << '\t'
                   << queryTypeName(item.query.type) << '\t'
                   << legacy.status << '\t' << aig.status << '\t'
                   << (match ? "yes" : "no") << '\t'
                   << (item.expectSemanticMatch ? "yes" : "no") << '\t'
                   << (expectationMet ? "yes" : "no") << '\t'
                   << std::fixed << std::setprecision(6)
                   << legacySeconds << '\t'
                   << (firstAigQuery ? buildSeconds : 0.0) << '\t'
                   << aigProofSeconds << '\n';
            firstAigQuery = false;
        }
    }

    output << "SUMMARY\tmatched=" << matched << "\ttotal=" << total
           << "\texpectations_met=" << expectationsMet
           << "\tresult=" << (passed ? "PASS" : "FAIL") << '\n';
    output.close();

    std::cout << "Function Query legacy/AIG differential: " << matched << '/'
              << total << " semantic matches, " << expectationsMet << '/'
              << total << " expectations met, result="
              << (passed ? "PASS" : "FAIL") << '\n'
              << "Details: " << outputPath << '\n';
    return passed ? 0 : 1;
}
