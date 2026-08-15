// tests/depth_opt_bench.cpp
//
// 第 2 步驗收工具。用法：
//   ./depth_opt_bench <netlist.v> [--cone <net>] [--basis <types>] [--basis-cone <net>]
//                     [--patience N] [--iters N] [--budget SEC] [-v]
//
// 範例（test40 的最後那道題）：
//   ./depth_opt_bench testcase/test40/test40.v --cone n14 --basis NAND,NOT -v
//
// 會跑：baseline(patience=1) vs 新設定，並排印出結果。

#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "include/core/Netlist.h"
#include "include/core/TechMapper.h"
#include "include/core/DepthOptimizer.h"
#include "include/core/DepthOptimizerRequest.h"
#include "include/core/RequestTimeBudget.h"

#include "include/io/VerilogReader.h"
#include "include/io/VerilogWriter.h"

using namespace depth_opt;

// -------------------------------------------------------------------------
// TODO(你): 這兩個函式填上你的實際 API 就能編譯
// -------------------------------------------------------------------------
static bool loadNetlist(const std::string& path, Netlist& out) {
    VerilogReader reader;
    if (!reader.read(path, out)) {
        std::cerr << "[Error] Failed to read Verilog file!" << std::endl;
        return false; 
    }
    return true;
}

static bool writeNetlist(const Netlist& nl, const std::string& path) {
    VerilogWriter writer;
    if (!writer.write(path, nl)) {
        std::cerr << "[Error] Failed to write Verilog file!" << std::endl;
        return false;
    }
    return true;   // 寫檔失敗不影響量測，先當成功
}

// -------------------------------------------------------------------------

static GateType parseGateType(const std::string& s) {
    if (s == "AND")  return GateType::AND;
    if (s == "OR")   return GateType::OR;
    if (s == "NAND") return GateType::NAND;
    if (s == "NOR")  return GateType::NOR;
    if (s == "NOT")  return GateType::NOT;
    if (s == "BUF")  return GateType::BUF;
    if (s == "XOR")  return GateType::XOR;
    if (s == "XNOR") return GateType::XNOR;
    throw std::runtime_error("unknown gate type: " + s);
}

static std::vector<GateType> parseGateList(const std::string& csv) {
    std::vector<GateType> out;
    size_t start = 0;
    while (start <= csv.size()) {
        const size_t comma = csv.find(',', start);
        const size_t end   = (comma == std::string::npos) ? csv.size() : comma;
        std::string tok = csv.substr(start, end - start);
        // trim
        while (!tok.empty() && isspace((unsigned char)tok.front())) tok.erase(tok.begin());
        while (!tok.empty() && isspace((unsigned char)tok.back()))  tok.pop_back();
        if (!tok.empty()) out.push_back(parseGateType(tok));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

// -------------------------------------------------------------------------

struct RunConfig {
    std::string label;
    IterationPolicy policy;
};

struct RunOutcome {
    std::string label;
    bool ok = false;
    int  oldGlobal = -1, newGlobal = -1;
    int  oldCone   = -1, newCone   = -1;
    int  oldGates  = -1, newGates  = -1;
    double seconds = 0.0;
    std::string status;
    std::string message;
};

static const char* statusName(OptimizationStatus s) {
    switch (s) {
        case OptimizationStatus::SUCCESS:                      return "SUCCESS";
        case OptimizationStatus::NO_IMPROVEMENT:               return "NO_IMPROVEMENT";
        case OptimizationStatus::TIMEOUT:                      return "TIMEOUT";
        case OptimizationStatus::ERROR_INVALID_REQUEST:        return "ERR_INVALID_REQUEST";
        case OptimizationStatus::ERROR_CONSTRAINT_UNSATISFIED: return "ERR_CONSTRAINT";
        case OptimizationStatus::ERROR_NOT_EQUIVALENT:         return "ERR_NOT_EQUIV";
        case OptimizationStatus::ERROR_AREA_EXCEEDED:          return "ERR_AREA";
    }
    return "?";
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr <<
            "usage: depth_opt_bench <netlist.v> [options]\n"
            "  --cone <net>        cost = depth of this cone (default: global max depth)\n"
            "  --basis <types>     comma-separated allowed types, e.g. NAND,NOT\n"
            "  --ban <types>       comma-separated banned types\n"
            "  --basis-cone <net>  restrict the basis to this cone only (default: whole netlist)\n"
            "  --patience N        override patience for the second run (default 1)\n"
            "  --iters N           max iterations (default 10)\n"
            "  --budget SEC        request time budget (default 290)\n"
            "  --out <file.v>      write the best result\n"
            "  -v                  verbose\n";
        return 1;
    }

    const std::string inputPath = argv[1];
    std::string costConeName, basisConeName, outPath = "out.v";
    std::vector<GateType> allowed, banned;
    int  patience = 1, iters = 10;
    double budget = request_time_budget::kGeneralToolBudgetSeconds;
    bool verbose = false;

    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) throw std::runtime_error("missing value after " + a);
            return argv[++i];
        };
        if      (a == "--cone")       costConeName  = next();
        else if (a == "--basis")      allowed       = parseGateList(next());
        else if (a == "--ban")        banned        = parseGateList(next());
        else if (a == "--basis-cone") basisConeName = next();
        else if (a == "--patience")   patience      = std::stoi(next());
        else if (a == "--iters")      iters         = std::stoi(next());
        else if (a == "--budget")     budget        = std::stod(next());
        //else if (a == "--out")        outPath       = next();
        else if (a == "-v")           verbose       = true;
        else { std::cerr << "unknown option: " << a << "\n"; return 1; }
    }

    // ---- 組 request ----
    OptimizationRequest request;
    if (!costConeName.empty()) {
        ConeRef ref;
        ref.type       = ConeQueryType::NetTransitiveFanin;
        ref.sourceName = costConeName;
        request.cost.metric = CostMetric::ConeDepth;
        request.cost.cone   = ref;
    }

    BasisConstraint basis;
    basis.allowed = allowed;
    basis.banned  = banned;
    if (!basisConeName.empty()) {
        ConeRef ref;
        ref.type       = ConeQueryType::NetTransitiveFanin;
        ref.sourceName = basisConeName;
        basis.scope    = ref;
    }
    request.basisConstraints.push_back(basis);

    const RequestValidation v = validateRequest(request);
    if (!v.ok) {
        std::cerr << "invalid request: " << v.message << "\n";
        return 1;
    }

    // ---- 讀進原始 netlist，之後每次跑都從這份 clone ----
    Netlist original;
    if (!loadNetlist(inputPath, original)) {
        std::cerr << "failed to load " << inputPath << "\n";
        return 1;
    }

    std::cout << "input      : " << inputPath << "\n"
              << "cost       : " << describeCost(request.cost) << "\n"
              << "basis scope: " << describeBasis(basis) << "\n"
              << "gates      : " << [&]{ int n=0; for (auto&p:original.countGatesByType()) n+=p.second; return n; }()
              << "\n\n";

    // ---- 兩組設定：baseline(patience=1，等同舊行為) vs 新設定 ----
    std::vector<RunConfig> configs = {
        { "patience(p=" + std::to_string(patience) + ")", IterationPolicy{ iters, patience } },
    };

    std::vector<RunOutcome> outcomes;
    Netlist bestNetlist;
    bool haveBest = false;
    CostMeasurement bestCost;

    for (const auto& cfg : configs) {
        std::cout << "===== run: " << cfg.label << " =====\n";

        Netlist working = original.cloneForRollback();
        request_time_budget::RequestDeadline deadline(budget);
        TechMapper   mapper(&deadline);
        DepthOptimizerConfig optCfg;
        optCfg.stage2Policy = cfg.policy;
        DepthOptimizer optimizer(optCfg);

        const auto t0 = std::chrono::steady_clock::now();
        const OptimizationResult r = optimizer.executeCriticalPathOptimization(
            working, mapper, request, verbose, &deadline);
        const auto t1 = std::chrono::steady_clock::now();

        RunOutcome o;
        o.label     = cfg.label;
        o.ok        = (r.status == OptimizationStatus::SUCCESS ||
                       r.status == OptimizationStatus::NO_IMPROVEMENT);
        o.oldGlobal = r.oldGlobalDepth; o.newGlobal = r.newGlobalDepth;
        o.oldCone   = r.oldConeDepth;   o.newCone   = r.newConeDepth;
        o.oldGates  = r.oldGateCount;   o.newGates  = r.newGateCount;
        o.seconds   = std::chrono::duration<double>(t1 - t0).count();
        o.status    = statusName(r.status);
        o.message   = r.message;
        outcomes.push_back(o);

        if (o.ok) {
            const CostMeasurement m = measureCost(working, request.cost);
            if (m.ok && (!haveBest || m.betterThan(bestCost, request.cost.metric))) {
                bestCost    = m;
                bestNetlist = working.cloneForRollback();
                haveBest    = true;
            }
        }
        std::cout << "\n";
    }

    // ---- 並排結果 ----
    std::cout << "================= summary =================\n";
    std::cout << std::left  << std::setw(18) << "config"
              << std::right << std::setw(16) << "global depth"
              << std::setw(14) << "cone depth"
              << std::setw(12) << "gates"
              << std::setw(9)  << "sec"
              << "  status\n";
    std::cout << std::string(80, '-') << "\n";
    for (const auto& o : outcomes) {
        std::cout << std::left  << std::setw(18) << o.label
                  << std::right << std::setw(9) << o.oldGlobal << " ->" << std::setw(5) << o.newGlobal
                  << std::setw(7) << o.oldCone   << " ->" << std::setw(5) << o.newCone
                  << std::setw(6) << o.oldGates  << " ->" << std::setw(6) << o.newGates
                  << std::setw(9) << std::fixed << std::setprecision(1) << o.seconds
                  << "  " << o.status << "\n";
    }
    std::cout << std::string(80, '-') << "\n";

    if (haveBest) {
        writeNetlist(bestNetlist, outPath);
        std::cout << "wrote best result to " << outPath << "\n";
    }
    return 0;
}