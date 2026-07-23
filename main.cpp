#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include <unordered_set>

#include "include/core/Netlist.h"
#include "include/io/VerilogReader.h"
#include "include/io/VerilogWriter.h"
#include "include/core/MockturtleConverter.h"
#include "include/core/DepthOptimizer.h"

// =====================================================================
// GateType <-> 字串 轉換
// =====================================================================
static GateType parseGateType(const std::string& s) {
    std::string u; for (char c : s) u += std::toupper(c);
    if (u == "AND")  return GateType::AND;
    if (u == "OR")   return GateType::OR;
    if (u == "NAND") return GateType::NAND;
    if (u == "NOR")  return GateType::NOR;
    if (u == "NOT")  return GateType::NOT;
    if (u == "BUF")  return GateType::BUF;
    if (u == "XOR")  return GateType::XOR;
    if (u == "XNOR") return GateType::XNOR;
    if (u == "DFF")  return GateType::DFF;
    return GateType::UNKNOWN;
}

static const char* gateTypeName(GateType t) {
    switch (t) {
        case GateType::AND:  return "AND";
        case GateType::OR:   return "OR";
        case GateType::NAND: return "NAND";
        case GateType::NOR:  return "NOR";
        case GateType::NOT:  return "NOT";
        case GateType::BUF:  return "BUF";
        case GateType::XOR:  return "XOR";
        case GateType::XNOR: return "XNOR";
        case GateType::DFF:  return "DFF";
        default:             return "UNKNOWN";
    }
}

// 解析 "and,not,xor" 這種逗號分隔清單
static std::vector<GateType> parseGateList(const std::string& csv) {
    std::vector<GateType> out;
    std::stringstream ss(csv);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        tok.erase(std::remove_if(tok.begin(), tok.end(), ::isspace), tok.end());
        if (tok.empty()) continue;
        GateType t = parseGateType(tok);
        if (t == GateType::UNKNOWN)
            std::cerr << "[Warn] unknown gate type: " << tok << "\n";
        else
            out.push_back(t);
    }
    return out;
}

static std::string gateListToString(const std::vector<GateType>& v) {
    if (v.empty()) return "(none / unrestricted)";
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += ", ";
        s += gateTypeName(v[i]);
    }
    return s;
}

// =====================================================================
// 驗證：閘型合規性檢查
// =====================================================================
static bool isTypeAllowed(GateType t,
                          const std::vector<GateType>& allowed,
                          const std::vector<GateType>& banned) {
    if (t == GateType::DFF || t == GateType::UNKNOWN) return true;   // 時序/死閘不檢查
    for (GateType b : banned) if (b == t) return false;
    if (allowed.empty()) return true;                                 // 空白名單 = 全允許
    for (GateType a : allowed) if (a == t) return true;
    return false;
}

// 全域合規檢查
static int checkGlobalCompliance(const Netlist& nl,
                                 const std::vector<GateType>& allowed,
                                 const std::vector<GateType>& banned) {
    int violations = 0;
    for (size_t i = 0; i < nl.getGateCount(); ++i) {
        const Gate& g = nl.getGate(i);
        if (g.type == GateType::UNKNOWN || g.type == GateType::DFF) continue;
        if (!isTypeAllowed(g.type, allowed, banned)) {
            if (violations < 10)
                std::cerr << "  [VIOLATION] gate '" << g.instName
                          << "' type " << gateTypeName(g.type) << "\n";
            ++violations;
        }
    }
    return violations;
}

// Cone 內合規檢查（cone 外不檢查）
static int checkConeCompliance(Netlist& nl,
                               const std::string& coneNetName,
                               const std::vector<GateType>& allowed,
                               const std::vector<GateType>& banned) {
    ConeResult cr = nl.getTransitiveFaninCone(coneNetName);
    std::vector<int> gids = nl.getConeGateIds(cr);

    int violations = 0;
    for (int gid : gids) {
        if (gid < 0 || !nl.isValidGateId(gid)) continue;
        const Gate& g = nl.getGate(gid);
        if (g.type == GateType::UNKNOWN || g.type == GateType::DFF) continue;
        if (!isTypeAllowed(g.type, allowed, banned)) {
            if (violations < 10)
                std::cerr << "  [CONE VIOLATION] gate '" << g.instName
                          << "' type " << gateTypeName(g.type) << "\n";
            ++violations;
        }
    }
    std::cout << "  -> cone '" << coneNetName << "' size: " << gids.size() << " gates\n";
    return violations;
}

// 印出閘型分佈
static void printGateHistogram(const Netlist& nl, const std::string& tag) {
    std::cout << "  [" << tag << "] ";
    int total = 0;
    for (const auto& p : nl.countGatesByType()) {
        if (p.second == 0) continue;
        std::cout << gateTypeName(p.first) << "=" << p.second << " ";
        if (p.first != GateType::DFF) total += p.second;
    }
    std::cout << "| comb total=" << total << "\n";
}

static void printUsage(const char* prog) {
    std::cout <<
        "Usage: " << prog << " <input.v> [output.v] [options]\n"
        "\nConstraint options:\n"
        "  -allowed <list>       允許的閘型 (逗號分隔)，空 = 無限制\n"
        "  -banned  <list>       禁止的閘型 (逗號分隔)\n"
        "  -cone    <netName>    對指定 net 的 fanin cone 施加限制\n"
        "                        (此時 -allowed/-banned 代表 cone 內的約束)\n"
        "  -quiet                關閉 verbose\n"
        "\nPresets:\n"
        "  -preset free          無限制 (全 8 種閘)\n"
        "  -preset aig           純 AIG   (AND, NOT)\n"
        "  -preset xag           純 XAG   (XOR, AND, NOT)\n"
        "  -preset nand-nor      只允許 NAND, NOR\n"
        "  -preset nor-not       只允許 NOR, NOT\n"
        "  -preset xnor-and      只允許 XNOR, AND\n"
        "  -preset no-and        禁止 AND\n"
        "\nExamples:\n"
        "  " << prog << " test.v out.v -preset xag\n"
        "  " << prog << " test.v out.v -banned and\n"
        "  " << prog << " test.v out.v -cone n10 -allowed nor,not\n";
}

// =====================================================================
int main(int argc, char* argv[]) {
    if (argc < 2) { printUsage(argv[0]); return -1; }

    std::string inputFilePath  = argv[1];
    std::string outputFilePath = "output.v";
    std::vector<GateType> allowedTypes;
    std::vector<GateType> bannedTypes;
    std::string coneNetName;
    bool verbose = true;

    // ---- 解析參數 ----
    int argi = 2;
    if (argi < argc && argv[argi][0] != '-') outputFilePath = argv[argi++];

    for (; argi < argc; ++argi) {
        std::string a = argv[argi];
        auto next = [&]() -> std::string {
            return (argi + 1 < argc) ? std::string(argv[++argi]) : std::string();
        };
        if      (a == "-allowed") allowedTypes = parseGateList(next());
        else if (a == "-banned")  bannedTypes  = parseGateList(next());
        else if (a == "-cone")    coneNetName  = next();
        else if (a == "-quiet")   verbose = false;
        else if (a == "-preset") {
            std::string p = next();
            if      (p == "free")      { allowedTypes.clear(); bannedTypes.clear(); }
            else if (p == "aig")       allowedTypes = parseGateList("and,not");
            else if (p == "xag")       allowedTypes = parseGateList("xor,and,not");
            else if (p == "nand-nor")  allowedTypes = parseGateList("nand,nor");
            else if (p == "nor-not")   allowedTypes = parseGateList("nor,not");
            else if (p == "xnor-and")  allowedTypes = parseGateList("xnor,and");
            else if (p == "no-and")    bannedTypes  = parseGateList("and");
            else std::cerr << "[Warn] unknown preset: " << p << "\n";
        }
        else if (a == "-h" || a == "--help") { printUsage(argv[0]); return 0; }
        else std::cerr << "[Warn] unknown option: " << a << "\n";
    }

    std::cout << "\n=========================================\n";
    std::cout << "[Info] Input      : " << inputFilePath  << "\n";
    std::cout << "[Info] Output     : " << outputFilePath << "\n";
    std::cout << "[Info] Allowed    : " << gateListToString(allowedTypes) << "\n";
    std::cout << "[Info] Banned     : " << gateListToString(bannedTypes)  << "\n";
    std::cout << "[Info] Cone       : "
              << (coneNetName.empty() ? "(global)" : coneNetName) << "\n";
    std::cout << "=========================================\n\n";

    // ---- [Step 1] 讀檔 ----
    Netlist myCircuit;
    VerilogReader reader;
    std::cout << "[Step 1] Reading Verilog...\n";
    if (!reader.read(inputFilePath, myCircuit)) {
        std::cerr << "[Error] Failed to read Verilog file!\n";
        return -1;
    }
    std::cout << "  -> Gates: " << myCircuit.getGateCount()
              << " | Nets: "    << myCircuit.getNetCount() << "\n";
    printGateHistogram(myCircuit, "before");

    const int originalDepth = myCircuit.findGlobalCriticalPath().depth;
    std::cout << "  -> Original depth: " << originalDepth << "\n\n";

    // ---- 建立 ConeReport ----
    ConeReport coneReport;
    if (!coneNetName.empty()) {
        // 依你的 API 建立 cone report；若有現成的 query 介面請替換這裡
        coneReport.cone        = myCircuit.getTransitiveFaninCone(coneNetName);
        coneReport.gateIds     = myCircuit.getConeGateIds(coneReport.cone);
        coneReport.rootNetIds  = coneReport.cone.rootNetIds;
        coneReport.sourceName  = coneNetName;
        coneReport.type        = ConeQueryType::NetTransitiveFanin;
        coneReport.gateCount   = coneReport.gateIds.size();
        coneReport.exists      = !coneReport.gateIds.empty();
        coneReport.ok          = coneReport.exists;

        if (!coneReport.ok) {
            std::cerr << "[Error] Cone '" << coneNetName << "' not found or empty!\n";
            return -1;
        }
        std::cout << "[Info] Cone '" << coneNetName << "' has "
                  << coneReport.gateIds.size() << " gates\n\n";
    }

    // ---- [Step 2] 優化 ----
    std::cout << "[Step 2] Executing Critical Path Optimization...\n";
    TechMapper techMapper;
    DepthOptimizer depthOptimizer;

    OptimizationResult optResult = depthOptimizer.executeCriticalPathOptimization(
        myCircuit, techMapper, coneReport, allowedTypes, bannedTypes, verbose);

    if (optResult.status != OptimizationStatus::SUCCESS) {
        std::cerr << "\n  -> Optimization FAILED: " << optResult.message << "\n";
        std::cerr << "  -> (比賽情境應在此 fallback 回原始電路)\n\n";
        return -1;
    }
    std::cout << "\n  -> Depth : " << optResult.oldDepth << " -> " << optResult.newDepth
              << (optResult.depthImproved ? "  (Improved)" : "  (Not improved)") << "\n";
    std::cout << "  -> Gates : " << optResult.oldGateCount << " -> " << optResult.newGateCount
              << "  (delta " << optResult.areaDelta << ")\n";
    printGateHistogram(myCircuit, "after");

    // ---- [Step 3] 合規驗證 ----
    std::cout << "\n[Step 3] Compliance verification...\n";
    int violations = 0;
    if (coneNetName.empty()) {
        violations = checkGlobalCompliance(myCircuit, allowedTypes, bannedTypes);
        std::cout << "  -> global compliance: "
                  << (violations == 0 ? "PASS" : "FAIL") << "\n";
    } else {
        // cone 題：只檢查 cone 內；cone 外任意閘皆合法
        violations = checkConeCompliance(myCircuit, coneNetName, allowedTypes, bannedTypes);
        std::cout << "  -> cone compliance  : "
                  << (violations == 0 ? "PASS" : "FAIL") << "\n";
    }
    if (violations > 0)
        std::cerr << "  -> " << violations << " violation(s)! 輸出將不合規。\n";

    // ---- [Step 4] 深度回歸檢查 ----
    std::cout << "\n[Step 4] Depth regression check...\n";
    const int finalDepth = myCircuit.findGlobalCriticalPath().depth;
    std::cout << "  -> original=" << originalDepth << "  final=" << finalDepth << "\n";
    if (finalDepth > originalDepth) {
        std::cout << "  -> WARNING: 深度比原始更差 ("
                  << originalDepth << " -> " << finalDepth << ")\n";
        std::cout << "     受限基底題可能屬正常；無限制題則代表流程有問題。\n";
        std::cout << "     比賽情境：若原電路本身已合規，應 fallback 輸出原電路。\n";
    }

    // ---- [Step 5] 寫檔 ----
    std::cout << "\n[Step 5] Writing output...\n";
    VerilogWriter writer;
    if (!writer.write(outputFilePath, myCircuit)) {
        std::cerr << "[Error] Failed to write Verilog!\n";
        return -1;
    }
    std::cout << "  -> Wrote " << outputFilePath << "\n";

    std::cout << "\n[Summary] depth " << originalDepth << " -> " << finalDepth
              << " | compliance " << (violations == 0 ? "PASS" : "FAIL") << "\n\n";
    return (violations == 0) ? 0 : 1;
}