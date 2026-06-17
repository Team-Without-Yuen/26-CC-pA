#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <algorithm>
#include <limits>
#include "include/core/Netlist.h"
#include "include/core/TechMapper.h"
#include "include/io/VerilogReader.h"
#include "include/io/VerilogWriter.h"

// =====================================================================
// 輔助函式：字串與 GateType 的互相轉換
// =====================================================================
GateType stringToGateType(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::toupper);
    if (s == "AND") return GateType::AND;
    if (s == "OR") return GateType::OR;
    if (s == "NAND") return GateType::NAND;
    if (s == "NOR") return GateType::NOR;
    if (s == "NOT") return GateType::NOT;
    if (s == "BUF") return GateType::BUF;
    if (s == "XOR") return GateType::XOR;
    if (s == "XNOR") return GateType::XNOR;
    return GateType::UNKNOWN;
}

std::string gateTypeToString(GateType type) {
    switch (type) {
        case GateType::AND: return "AND";
        case GateType::OR: return "OR";
        case GateType::NAND: return "NAND";
        case GateType::NOR: return "NOR";
        case GateType::NOT: return "NOT";
        case GateType::BUF: return "BUF";
        case GateType::XOR: return "XOR";
        case GateType::XNOR: return "XNOR";
        default: return "UNKNOWN";
    }
}

// =====================================================================
// 輔助函式：互動式建立 Constraints Map
// =====================================================================
void promptForConstraints(std::map<GateType, int>& constraints, const std::string& mapName) {
    std::cout << "\n>>> Setting up [" << mapName << "] <<<\n";
    std::cout << "Please enter Gate Type (e.g., AND, OR) and its Count (e.g., 1, or -1 for unlimited).\n";
    std::cout << "Type 'DONE' when you are finished with this list.\n";
    
    while (true) {
        std::string gateStr;
        std::cout << "  Gate Type (or DONE): ";
        std::cin >> gateStr;
        
        std::transform(gateStr.begin(), gateStr.end(), gateStr.begin(), ::toupper);
        if (gateStr == "DONE") break;

        GateType type = stringToGateType(gateStr);
        if (type == GateType::UNKNOWN) {
            std::cout << "  [Warning] Unknown gate type! Please try again.\n";
            continue;
        }

        int count;
        std::cout << "  Count for " << gateStr << ": ";
        while (!(std::cin >> count)) { // 防呆：避免使用者輸入非數字字元導致無窮迴圈
            std::cin.clear();
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            std::cout << "  [Error] Invalid number. Please enter an integer: ";
        }
        
        constraints[type] = count;
    }
}

// =====================================================================
// 主程式 Main
// =====================================================================
int main(int argc, char* argv[]) {
    std::string inputFilePath;
    std::string outputFilePath = "output.v"; 

    // --- Command Line 解析 ---
    if (argc < 2) {
        std::cout << "Usage: " << argv[0] << " <input_verilog_file> [output_verilog_file]\n";
        std::cout << "Please enter the input file path now: ";
        std::cin >> inputFilePath;
    } else {
        inputFilePath = argv[1];    
        if (argc >= 3) outputFilePath = argv[2]; 
    }

    std::cout << "\n=========================================\n";
    std::cout << "[Info] Input File  : " << inputFilePath << "\n";
    std::cout << "[Info] Output File : " << outputFilePath << "\n";
    std::cout << "=========================================\n\n";

    Netlist myCircuit;
    VerilogReader reader;
    
    // ==========================================
    // Step 1: 讀取並解析 Verilog
    // ==========================================
    std::cout << "[Step 1] Reading and parsing Verilog..." << std::endl;
    if (!reader.read(inputFilePath, myCircuit)) {
        std::cerr << "[Error] Failed to read Verilog file!" << std::endl;
        return -1; 
    }

    std::cout << "  -> Parsing Successful.\n";
    std::cout << "  -> Initial Gate Count : " << myCircuit.getGateCount() << "\n";
    std::cout << "  -> Initial Wire Count : " << myCircuit.getNetCount() << "\n\n";

    // ==========================================
    // Step 2: 執行 Technology Mapping (互動測試)
    // ==========================================
    std::cout << "[Step 2] Interactive Custom Technology Mapping" << std::endl;
    
    std::map<GateType, int> targetConstraints;
    std::map<GateType, int> allowedConstraints;

    // 1. 取得 Constraints
    promptForConstraints(targetConstraints, "Target Constraints (Gates to Remove)");
    promptForConstraints(allowedConstraints, "Allowed Constraints (Gates to Generate)");

    // 2. 取得 Target Scope
    std::cout << "\n>>> Select Target Scope <<<\n";
    std::cout << "  0: WHOLE_NETLIST\n";
    std::cout << "  1: NET_FANIN\n";
    std::cout << "  2: NET_FANOUT\n";
    std::cout << "  3: GATE_FANIN\n";
    std::cout << "  4: GATE_FANOUT\n";
    std::cout << "Enter choice (0-4): ";
    
    int scopeInput;
    while (!(std::cin >> scopeInput) || scopeInput < 0 || scopeInput > 4) {
        std::cin.clear();
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        std::cout << "  [Error] Invalid choice. Enter 0-4: ";
    }
    TargetScope scope = static_cast<TargetScope>(scopeInput);

    // 3. 取得 Target Name (若非 WHOLE_NETLIST)
    std::string targetName = "";
    if (scope != TargetScope::WHOLE_NETLIST) {
        std::cout << "Enter the exact name of the Target Net/Gate: ";
        std::cin >> targetName;
    }

    // 4. 取得 Verbose 設定
    std::cout << "\nEnable verbose logging? (1 for Yes, 0 for No): ";
    bool verbose;
    std::cin >> verbose;

    // 5. 呼叫核心引擎！
    TechMapper mapper;
    std::cout << "\n[Info] Firing up the mapping engine...\n";
    TechMapReport report = mapper.customMapTechnology(myCircuit, targetConstraints, allowedConstraints, scope, targetName, verbose);

    // 6. 印出精美的結算報告
    std::cout << "\n=========================================\n";
    std::cout << "          TECH MAP REPORT                \n";
    std::cout << "=========================================\n";
    std::cout << "Status  : " << (int)report.status << " (" << report.message << ")\n";
    
    std::cout << "\n[Gates Removed]:\n";
    if (report.removedCountByType.empty()) std::cout << "  (None)\n";
    for (const auto& pair : report.removedCountByType) {
        std::cout << "  - " << gateTypeToString(pair.first) << " : " << pair.second << "\n";
    }

    std::cout << "\n[Gates Added (Lookup Table & Exact Synthesis)]:\n";
    if (report.addedCountByType.empty()) std::cout << "  (None)\n";
    for (const auto& pair : report.addedCountByType) {
        std::cout << "  - " << gateTypeToString(pair.first) << " : " << pair.second << "\n";
    }

    if (!report.synthesizedTopology.empty()) {
        std::cout << "\n[Notice] Exact Synthesis was triggered and generated " 
                  << report.synthesizedTopology.size() << " abstract gates.\n";
    }

    std::cout << "=========================================\n\n";

    // ==========================================
    // Step 3: 寫出修改後的 Verilog 檔案
    // ==========================================
    std::cout << "[Step 3] Writing output Verilog..." << std::endl;
    VerilogWriter writer;
    
    if (!writer.write(outputFilePath, myCircuit)) {
        std::cerr << "[Error] Failed to write Verilog file!" << std::endl;
        return -1;
    }
    
    std::cout << "  -> Successfully wrote modified circuit to: " << outputFilePath << "\n";
    std::cout << "\n[Done] EDA flow completed smoothly.\n";

    return 0;
}