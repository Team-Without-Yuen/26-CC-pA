#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "include/SATEngine/Primitives.h"
#include "include/core/BitParallelSimulation.h"
#include "include/core/Netlist.h"
#include "include/io/VerilogReader.h"

namespace {

using Clock = std::chrono::steady_clock;
using Signature = BitParallelSimulationSignature;
using Pair = std::pair<std::string, std::string>;
using PairSet = std::set<Pair>;

struct BatchResult {
    bool complete = false;
    bool timedOut = false;
    size_t unknownCount = 0;
    size_t satChecks = 0;
    size_t candidatePairsConsidered = 0;
    size_t rejectedBySimulation = 0;
    size_t classCount = 0;
    double seconds = 0.0;
    PairSet pairs;
};

struct BenchmarkCase {
    std::string design;
    std::string mode;
    std::string path;
    std::string target;
    bool equivalentPairs = false;
    bool findAll = true;
    double timeLimitSeconds = 55.0;
    bool allowPhaseBTimeoutPartial = false;
    std::uint32_t randomSeed = 0;
    size_t randomGateCount = 0;
};

double elapsedSeconds(const Clock::time_point& startedAt) {
    return std::chrono::duration<double>(Clock::now() - startedAt).count();
}

Pair canonicalPair(std::string a, std::string b) {
    if (b < a) std::swap(a, b);
    return {std::move(a), std::move(b)};
}

void addNamedGate(Netlist& netlist,
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
}

bool buildRandomNetlist(const BenchmarkCase& item, Netlist& netlist) {
    std::mt19937 rng(item.randomSeed);
    std::vector<std::string> available;
    for (size_t index = 0; index < 12; ++index) {
        const std::string name = "pi" + std::to_string(index);
        netlist.addPrimaryInput(name);
        available.push_back(name);
    }

    const std::vector<GateType> binaryTypes = {
        GateType::AND, GateType::OR, GateType::NAND, GateType::NOR,
        GateType::XOR, GateType::XNOR};
    for (size_t index = 0; index < item.randomGateCount; ++index) {
        const bool unary = (rng() % 7U) == 0;
        const GateType type = unary
            ? ((rng() & 1U) ? GateType::NOT : GateType::BUF)
            : binaryTypes[rng() % binaryTypes.size()];
        const std::string inputA = available[rng() % available.size()];
        std::vector<std::string> inputs = {inputA};
        if (!unary) {
            inputs.push_back(available[rng() % available.size()]);
        }
        const std::string output = "r" + std::to_string(index);
        addNamedGate(netlist, "g_" + output, type, inputs, output);
        available.push_back(output);

        // Exact duplicates exercise equivalence classes without depending on
        // accidental random functional collisions.
        if (index % 19U == 7U) {
            const std::string clone = output + "_eq";
            if (inputs.size() == 2 && (rng() & 1U)) {
                std::swap(inputs[0], inputs[1]);
            }
            addNamedGate(netlist, "g_" + clone, type, inputs, clone);
            available.push_back(clone);
        }
    }

    const size_t internalBegin = 12;
    const size_t internalCount = available.size() - internalBegin;
    if (internalCount < 2) return false;
    const size_t lhsIndex = rng() % internalCount;
    size_t rhsIndex = rng() % (internalCount - 1);
    if (rhsIndex >= lhsIndex) ++rhsIndex;
    const std::string& lhsSource = available[internalBegin + lhsIndex];
    const std::string& rhsSource = available[internalBegin + rhsIndex];
    addNamedGate(netlist, "g_nand_lhs", GateType::BUF,
                 {lhsSource}, "nand_lhs");
    addNamedGate(netlist, "g_nand_rhs", GateType::BUF,
                 {rhsSource}, "nand_rhs");
    addNamedGate(netlist, "g_target_nand", GateType::NAND,
                 {"nand_lhs", "nand_rhs"}, "target_nand");
    return true;
}

bool loadNetlist(const BenchmarkCase& item, Netlist& netlist) {
    if (item.randomGateCount != 0) {
        return buildRandomNetlist(item, netlist);
    }
    if (item.path.empty()) {
        netlist.addPrimaryInput("a");
        netlist.addPrimaryInput("b");
        netlist.addPrimaryInput("c");
        auto addGate = [&](const std::string& name, GateType type,
                           const std::vector<std::string>& inputs,
                           const std::string& output) {
            addNamedGate(netlist, name, type, inputs, output);
        };
        addGate("g_and", GateType::AND, {"a", "b"}, "and_ab");
        addGate("g_nand", GateType::NAND, {"a", "b"}, "nand_ab");
        addGate("g_and_alt", GateType::NOT, {"nand_ab"}, "and_alt");
        addGate("g_or", GateType::OR, {"a", "c"}, "or_ac");
        addGate("g_nor", GateType::NOR, {"a", "c"}, "nor_ac");
        addGate("g_or_alt", GateType::NOT, {"nor_ac"}, "or_alt");
        addGate("g_xor", GateType::XOR, {"b", "c"}, "xor_bc");
        addGate("g_xor_alt", GateType::XOR, {"c", "b"}, "xor_alt");
        addGate("g_decoy", GateType::AND, {"a", "c"}, "decoy");
        addGate("g_ia", GateType::BUF, {"a"}, "ia");
        addGate("g_ib", GateType::BUF, {"b"}, "ib");
        addGate("g_ic", GateType::BUF, {"c"}, "ic");
        addGate("g_target", GateType::NAND, {"ia", "ib"}, "target_nand");
        return true;
    }
    VerilogReader reader;
    return reader.read(item.path, netlist);
}

eqeng::Primitives::Config phaseBConfig() {
    eqeng::Primitives::Config config;
    config.verbose_rebuild = false;
    config.fraig_auto_sweep_threshold = 0;
    return config;
}

bool containsRequiredOnes(const Signature& candidate,
                          const Signature& requiredOnes) {
    if (candidate.size() != requiredOnes.size()) return false;
    for (size_t word = 0; word < candidate.size(); ++word) {
        if ((candidate[word] & requiredOnes[word]) != requiredOnes[word]) {
            return false;
        }
    }
    return true;
}

bool nandSignatureMatches(const Signature& a,
                          const Signature& b,
                          const Signature& target,
                          std::uint64_t lastWordMask) {
    if (a.size() != b.size() || a.size() != target.size()) return false;
    for (size_t word = 0; word < a.size(); ++word) {
        std::uint64_t value = ~(a[word] & b[word]);
        if (word + 1 == a.size()) value &= lastWordMask;
        if (value != target[word]) return false;
    }
    return true;
}

BatchResult legacyEquivalentPairs(Netlist& netlist, double limitSeconds) {
    BatchResult result;
    const auto startedAt = Clock::now();
    auto remaining = [&]() {
        return limitSeconds - elapsedSeconds(startedAt);
    };

    const BitParallelSimulationResult simulation =
        simulateNetlistBitParallel(netlist, 256);
    std::map<Signature, std::vector<int>> buckets;
    size_t eligible = 0;
    for (size_t index = 0; index < netlist.getGateCount(); ++index) {
        const int gateId = static_cast<int>(index);
        if (!netlist.isValidGateId(gateId) ||
            !netlist.isCombinationalGate(gateId)) {
            continue;
        }
        const Gate& gate = netlist.getGate(gateId);
        if (!netlist.isValidNetId(gate.outputNetId)) continue;
        const Net& output = netlist.getNet(gate.outputNetId);
        if (output.isRemoved || output.isConst || output.name.empty() ||
            output.driverGateId != gateId ||
            static_cast<size_t>(gate.outputNetId) >= simulation.known.size() ||
            !simulation.known[gate.outputNetId]) {
            continue;
        }
        buckets[simulation.signatures[gate.outputNetId]].push_back(gateId);
        ++eligible;
    }
    const size_t allPairs = eligible < 2 ? 0 : eligible * (eligible - 1) / 2;
    size_t sameBucketPairs = 0;
    for (const auto& entry : buckets) {
        const size_t size = entry.second.size();
        if (size >= 2) sameBucketPairs += size * (size - 1) / 2;
    }
    result.rejectedBySimulation = allPairs - sameBucketPairs;

    bool stop = false;
    std::vector<std::vector<int>> provenClasses;
    for (const auto& entry : buckets) {
        if (entry.second.size() < 2) continue;
        std::vector<std::vector<int>> bucketClasses;
        for (int candidateGateId : entry.second) {
            if (remaining() <= 0.0) {
                result.timedOut = true;
                stop = true;
                break;
            }
            bool matched = false;
            bool unresolved = false;
            for (std::vector<int>& eqClass : bucketClasses) {
                const int representativeGateId = eqClass.front();
                FunctionQuery query;
                query.type = FunctionQueryType::Equivalence;
                query.netNameA = netlist.getNet(
                    netlist.getGate(representativeGateId).outputNetId).name;
                query.netNameB = netlist.getNet(
                    netlist.getGate(candidateGateId).outputNetId).name;
                query.timeLimitSeconds = std::max(0.001, remaining());
                ++result.candidatePairsConsidered;
                ++result.satChecks;
                const FunctionReport proof = netlist.runFunctionQuery(query);
                if (proof.solverUnknown || proof.solverTimedOut ||
                    proof.unsupported || !proof.ok) {
                    ++result.unknownCount;
                    unresolved = true;
                    if (proof.solverTimedOut) {
                        result.timedOut = true;
                        stop = true;
                        break;
                    }
                    continue;
                }
                if (!proof.equivalent) continue;
                eqClass.push_back(candidateGateId);
                matched = true;
                break;
            }
            if (stop) break;
            if (!matched && !unresolved) {
                bucketClasses.push_back({candidateGateId});
            }
        }
        for (std::vector<int>& eqClass : bucketClasses) {
            if (eqClass.size() >= 2) {
                provenClasses.push_back(std::move(eqClass));
            }
        }
        if (stop) break;
    }
    for (const std::vector<int>& eqClass : provenClasses) {
        ++result.classCount;
        for (size_t i = 0; i < eqClass.size(); ++i) {
            for (size_t j = i + 1; j < eqClass.size(); ++j) {
                result.pairs.insert(canonicalPair(
                    netlist.getNet(netlist.getGate(eqClass[i]).outputNetId).name,
                    netlist.getNet(netlist.getGate(eqClass[j]).outputNetId).name));
            }
        }
    }
    if (!result.timedOut && result.unknownCount == 0) {
        result.complete = true;
    }
    result.seconds = elapsedSeconds(startedAt);
    return result;
}

BatchResult phaseBEquivalentPairs(Netlist& netlist, double limitSeconds) {
    FunctionSearchQuery query;
    query.type = FunctionSearchQueryType::EquivalentGatePairs;
    query.mode = FunctionSearchMode::FindAll;
    query.scope = FunctionSearchScope::WholeDesign;
    query.maxResults = 0;
    query.maxStoredMatches = 0;
    query.simulationPatternCount = 256;
    query.timeLimitSeconds = limitSeconds;
    query.expandEquivalentPairs = true;
    query.writeMatchesToFile = false;

    const FunctionSearchReport report = netlist.runFunctionSearchQuery(query);
    BatchResult result;
    result.complete = report.complete;
    result.timedOut = report.timedOut;
    result.unknownCount = report.satUnknownCount;
    result.satChecks = report.satChecks;
    result.candidatePairsConsidered = report.candidatePairsConsidered;
    result.rejectedBySimulation = report.candidatePairsRejectedBySimulation;
    result.classCount = report.equivalenceClassCount;
    result.seconds = report.elapsedSeconds;
    for (const FunctionSearchEquivalenceClass& eqClass :
         report.equivalenceClasses) {
        for (size_t i = 0; i < eqClass.netNames.size(); ++i) {
            for (size_t j = i + 1; j < eqClass.netNames.size(); ++j) {
                result.pairs.insert(canonicalPair(
                    eqClass.netNames[i], eqClass.netNames[j]));
            }
        }
    }
    return result;
}

FunctionSearchQuery nandQuery(const std::string& target,
                              bool findAll,
                              double limitSeconds) {
    FunctionSearchQuery query;
    query.type = FunctionSearchQueryType::NandEquivalentInputPairs;
    query.mode = findAll ? FunctionSearchMode::FindAll
                         : FunctionSearchMode::FindAny;
    query.targetNetName = target;
    query.internalSignalsOnly = true;
    query.allowSameSignalPair = false;
    query.maxResults = 0;
    query.maxStoredMatches = 1000000;
    query.simulationPatternCount = 256;
    query.timeLimitSeconds = limitSeconds;
    query.writeMatchesToFile = false;
    return query;
}

BatchResult legacyNand(Netlist& netlist,
                       const std::string& target,
                       bool findAll,
                       double limitSeconds) {
    const FunctionSearchReport report = netlist.runFunctionSearchQuery(
        nandQuery(target, findAll, limitSeconds));
    BatchResult result;
    result.complete = report.complete;
    result.timedOut = report.timedOut;
    result.unknownCount = report.satUnknownCount;
    result.satChecks = report.satChecks;
    result.candidatePairsConsidered = report.candidatePairsConsidered;
    result.rejectedBySimulation = report.candidatePairsRejectedBySimulation;
    result.seconds = report.elapsedSeconds;
    for (const FunctionSearchMatch& match : report.matches) {
        result.pairs.insert(canonicalPair(match.netNameA, match.netNameB));
    }
    return result;
}

BatchResult phaseBNand(Netlist& netlist,
                       const std::string& targetName,
                       bool findAll,
                       double limitSeconds) {
    BatchResult result;
    const auto startedAt = Clock::now();
    auto remaining = [&]() {
        return limitSeconds - elapsedSeconds(startedAt);
    };
    const int targetId = netlist.getNetId(targetName);
    if (!netlist.isValidNetId(targetId)) return result;

    const BitParallelSimulationResult simulation =
        simulateNetlistBitParallel(netlist, 256);
    if (static_cast<size_t>(targetId) >= simulation.known.size() ||
        !simulation.known[targetId]) {
        return result;
    }
    const Signature& targetSignature = simulation.signatures[targetId];
    Signature requiredOnes(targetSignature.size(), 0);
    for (size_t word = 0; word < targetSignature.size(); ++word) {
        requiredOnes[word] = ~targetSignature[word];
    }
    requiredOnes.back() &= simulation.lastWordMask;

    std::vector<int> eligible;
    for (size_t index = 0; index < netlist.getNetCount(); ++index) {
        const int netId = static_cast<int>(index);
        const Net& net = netlist.getNet(netId);
        if (netId == targetId || net.isRemoved || net.isConst || net.name.empty()) {
            continue;
        }
        const bool hasDriver = netlist.isValidGateId(net.driverGateId) &&
            netlist.getGate(net.driverGateId).type != GateType::UNKNOWN;
        if (net.isPI || net.isPO || !hasDriver) continue;
        if (static_cast<size_t>(netId) >= simulation.known.size() ||
            !simulation.known[netId]) {
            continue;
        }
        if (containsRequiredOnes(simulation.signatures[netId], requiredOnes)) {
            eligible.push_back(netId);
        }
    }

    eqeng::Primitives primitives(netlist, {}, phaseBConfig());
    primitives.enable_phase_b(true);
    const eqeng::SigRef target = primitives.resolve(targetName);
    std::vector<eqeng::SigRef> signals;
    signals.reserve(eligible.size());
    for (int netId : eligible) {
        signals.push_back(primitives.resolve(netlist.getNet(netId).name));
    }

    bool stop = false;
    for (size_t i = 0; i < eligible.size() && !stop; ++i) {
        for (size_t j = i + 1; j < eligible.size(); ++j) {
            if ((result.candidatePairsConsidered & 0xfffU) == 0 &&
                remaining() <= 0.0) {
                result.timedOut = true;
                stop = true;
                break;
            }
            ++result.candidatePairsConsidered;
            if (!nandSignatureMatches(simulation.signatures[eligible[i]],
                                      simulation.signatures[eligible[j]],
                                      targetSignature,
                                      simulation.lastWordMask)) {
                ++result.rejectedBySimulation;
                continue;
            }

            const eqeng::SigRef nand = !primitives.make_and(signals[i], signals[j]);
            ++result.satChecks;
            const eqeng::EquivResult proof = primitives.equiv_checked(
                nand, target, std::max(0.001, remaining()));
            if (proof == eqeng::EquivResult::Unknown) {
                ++result.unknownCount;
                if (primitives.last_proof_timed_out()) {
                    result.timedOut = true;
                    stop = true;
                    break;
                }
                continue;
            }
            if (proof == eqeng::EquivResult::NotEqual) continue;
            result.pairs.insert(canonicalPair(
                netlist.getNet(eligible[i]).name,
                netlist.getNet(eligible[j]).name));
            if (!findAll) {
                result.complete = true;
                stop = true;
                break;
            }
        }
    }
    if (findAll && !result.timedOut && result.unknownCount == 0) {
        result.complete = true;
    }
    result.seconds = elapsedSeconds(startedAt);
    return result;
}

BatchResult runLegacy(Netlist& netlist, const BenchmarkCase& item) {
    return item.equivalentPairs
        ? legacyEquivalentPairs(netlist, item.timeLimitSeconds)
        : legacyNand(netlist, item.target, item.findAll, item.timeLimitSeconds);
}

BatchResult runPhaseB(Netlist& netlist, const BenchmarkCase& item) {
    return item.equivalentPairs
        ? phaseBEquivalentPairs(netlist, item.timeLimitSeconds)
        : phaseBNand(netlist, item.target, item.findAll, item.timeLimitSeconds);
}

std::vector<BenchmarkCase> cases() {
    std::vector<BenchmarkCase> result = {
        {"test29", "equivalent_all", "NewTestCase/test29/test29.v", "", true, true, 55.0},
        {"test30", "equivalent_all", "NewTestCase/test30/test30.v", "", true, true, 55.0},
        {"test35", "nand_any", "NewTestCase/test35/test35.v", "n25", false, false, 55.0},
        {"test35", "nand_all", "NewTestCase/test35/test35.v", "n25", false, true, 290.0},
        {"synthetic", "equivalent_all", "", "", true, true, 10.0},
        {"synthetic", "nand_all", "", "target_nand", false, true, 10.0}
    };
    const std::vector<std::pair<std::uint32_t, size_t>> randomCases = {
        {0x10203040U, 48}, {0x31415926U, 96}, {0x5eed1234U, 160},
        {0xc001d00dU, 256}, {0xabcdef01U, 384}, {0x76543210U, 512},
        {0x13579bdfU, 1024}, {0x2468ace0U, 2048}};
    for (size_t index = 0; index < randomCases.size(); ++index) {
        const std::string design = "random_" + std::to_string(index + 1);
        const auto [seed, gateCount] = randomCases[index];
        result.push_back({design, "equivalent_all", "", "", true, true,
                          55.0, false, seed, gateCount});
        result.push_back({design, "nand_any", "", "target_nand", false,
                          false, 55.0, false, seed, gateCount});
    }
    return result;
}

PairSet requiredRandomEquivalentPairs(size_t gateCount) {
    PairSet required;
    for (size_t index = 7; index < gateCount; index += 19) {
        const std::string output = "r" + std::to_string(index);
        required.insert(canonicalPair(output, output + "_eq"));
    }
    return required;
}

void writeMismatchSamples(std::ostream& output,
                          const PairSet& legacy,
                          const PairSet& phaseB) {
    size_t emitted = 0;
    for (const Pair& pair : legacy) {
        if (phaseB.count(pair) == 0 && emitted++ < 5) {
            output << "missing_in_phase_b=" << pair.first << ',' << pair.second << '\n';
        }
    }
    emitted = 0;
    for (const Pair& pair : phaseB) {
        if (legacy.count(pair) == 0 && emitted++ < 5) {
            output << "extra_in_phase_b=" << pair.first << ',' << pair.second << '\n';
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::string selected = argc >= 2 ? argv[1] : "";
    const double overrideLimit = argc >= 3 ? std::stod(argv[2]) : 0.0;
    std::string suffix;
    if (!selected.empty()) {
        suffix = "_" + selected;
        std::replace(suffix.begin(), suffix.end(), ':', '_');
        if (overrideLimit > 0.0) {
            suffix += "_" + std::to_string(static_cast<int>(overrideLimit));
        }
    }
    const std::string outputPath =
        "mini test/test44/function_search_phase_b_batch_benchmark" + suffix + ".tsv";
    const std::string mismatchPath =
        "mini test/test44/function_search_phase_b_mismatches" + suffix + ".txt";
    std::ofstream output(outputPath);
    std::ofstream mismatchOutput(mismatchPath);
    if (!output || !mismatchOutput) {
        std::cerr << "Cannot open benchmark outputs\n";
        return 2;
    }
    output << "design\tmode\texpectation_met\tset_match\tcomplete_match\tlegacy_complete"
              "\tphase_b_complete\tlegacy_pairs\tphase_b_pairs"
              "\tlegacy_sat_checks\tphase_b_sat_checks\tlegacy_unknown"
              "\tphase_b_unknown\tlegacy_seconds\tphase_b_seconds\tspeedup\n";

    int passed = 0;
    int total = 0;
    for (BenchmarkCase item : cases()) {
        const std::string caseName = item.design + ":" + item.mode;
        if (!selected.empty() && selected != caseName &&
            !(selected == "random" && item.randomGateCount != 0)) {
            continue;
        }
        if (overrideLimit > 0.0) item.timeLimitSeconds = overrideLimit;
        Netlist legacyNetlist;
        Netlist phaseBNetlist;
        if (!loadNetlist(item, legacyNetlist) ||
            !loadNetlist(item, phaseBNetlist)) {
            std::cerr << "Failed to read " << item.path << '\n';
            return 2;
        }
        const BatchResult legacy = runLegacy(legacyNetlist, item);
        const BatchResult phaseB = runPhaseB(phaseBNetlist, item);
        const bool setMatch = legacy.pairs == phaseB.pairs;
        const bool completeMatch = legacy.complete == phaseB.complete;
        bool expectedMatches = true;
        if (item.design == "synthetic" && item.mode == "equivalent_all") {
            const PairSet required = {
                canonicalPair("and_ab", "and_alt"),
                canonicalPair("or_ac", "or_alt"),
                canonicalPair("xor_bc", "xor_alt"),
                canonicalPair("nand_ab", "target_nand")};
            expectedMatches = std::includes(
                legacy.pairs.begin(), legacy.pairs.end(),
                required.begin(), required.end()) &&
                std::includes(
                    phaseB.pairs.begin(), phaseB.pairs.end(),
                    required.begin(), required.end());
        } else if (item.design == "synthetic" && item.mode == "nand_all") {
            const Pair expected = canonicalPair("ia", "ib");
            expectedMatches = legacy.pairs.count(expected) != 0 &&
                              phaseB.pairs.count(expected) != 0;
        } else if (item.randomGateCount != 0 && item.equivalentPairs) {
            const PairSet required =
                requiredRandomEquivalentPairs(item.randomGateCount);
            expectedMatches = std::includes(
                legacy.pairs.begin(), legacy.pairs.end(),
                required.begin(), required.end()) &&
                std::includes(
                    phaseB.pairs.begin(), phaseB.pairs.end(),
                    required.begin(), required.end());
        } else if (item.randomGateCount != 0 && !item.findAll) {
            expectedMatches = legacy.pairs.size() == 1 &&
                              phaseB.pairs.size() == 1;
        }
        const bool safeTimeoutPartial = item.allowPhaseBTimeoutPartial &&
            legacy.complete && phaseB.timedOut &&
            std::includes(
                legacy.pairs.begin(), legacy.pairs.end(),
                phaseB.pairs.begin(), phaseB.pairs.end());
        const bool casePassed = expectedMatches &&
            ((setMatch && completeMatch &&
              legacy.unknownCount == phaseB.unknownCount) ||
             safeTimeoutPartial);
        ++total;
        passed += casePassed ? 1 : 0;
        output << item.design << '\t' << item.mode << '\t'
               << (casePassed ? "yes" : "no") << '\t'
               << (setMatch ? "yes" : "no") << '\t'
               << (completeMatch ? "yes" : "no") << '\t'
               << (legacy.complete ? "yes" : "no") << '\t'
               << (phaseB.complete ? "yes" : "no") << '\t'
               << legacy.pairs.size() << '\t' << phaseB.pairs.size() << '\t'
               << legacy.satChecks << '\t' << phaseB.satChecks << '\t'
               << legacy.unknownCount << '\t' << phaseB.unknownCount << '\t'
               << std::fixed << std::setprecision(6)
               << legacy.seconds << '\t' << phaseB.seconds << '\t'
               << (phaseB.seconds > 0.0 ? legacy.seconds / phaseB.seconds : 0.0)
               << '\n';
        if (!setMatch) {
            mismatchOutput << '[' << item.design << ':' << item.mode << "]\n";
            writeMismatchSamples(mismatchOutput, legacy.pairs, phaseB.pairs);
        }
    }
    output << "SUMMARY\tpassed=" << passed << "\ttotal=" << total << '\n';
    mismatchOutput << "SUMMARY mismatched_cases=" << (total - passed) << '\n';
    std::cout << "Function Search Phase B batch benchmark: " << passed << '/'
              << total << " cases matched\nDetails: " << outputPath << '\n';
    return passed == total ? 0 : 1;
}
