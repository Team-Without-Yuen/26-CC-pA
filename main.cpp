#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <algorithm>
#include <limits>
#include "include/core/Netlist.h"
#include "include/core/TechMapper.h"
#include "include/core/DepthOptimizer.h"
#include "include/io/VerilogReader.h"
#include "include/io/VerilogWriter.h"

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
    // Step 2: 轉換為 XAG 並執行 Depth Optimization
    // ==========================================
    std::cout << "[Step 2] Converting to XAG and Running Depth Optimization..." << std::endl;

    // ---------------------------------------------------------
    // 2.0 執行 Technology Mapping: 將全域電路轉換為 XAG
    // ---------------------------------------------------------
    std::cout << "  -> Converting Netlist to XAG format...\n";
    std::vector<GateType> xagAllowedTypes = {GateType::AND, GateType::NOT, GateType::XOR};
    
    TechMapper mapper;
    // 呼叫你的 Basis Conversion 引擎 (假設全域的 Enum 值為 TargetScope::GLOBAL，若不同請自行修改)
    TechMapReport mapReport = mapper.convertToBasis(
        myCircuit, 
        TargetScope::WHOLE_NETLIST, // 掃描範圍：全域
        "",                  // 目標名稱：無                // verbose: 印出詳細日誌
        xagAllowedTypes,      // 白名單：只允許 AND, NOT
        {},                   // 黑名單：不限制
        false                // verbose: 不印出詳細日誌
    );
    std::cout << "  -> xag Conversion Completed. Current Gate Count: " << myCircuit.getGateCount() << "\n\n";

    // ---------------------------------------------------------
    // 2.1 尋找測試目標：找出所有 Primary Output 中，深度最深的 Endpoint
    // ---------------------------------------------------------
    int maxDepth = -1;
    int worstNetId = -1;
    CombinationalPath worstPath;

    for (const auto& port : myCircuit.getPrimaryOutputs()) {
        for (int netId : port.netIds) {
            std::string netName = myCircuit.getNet(netId).name;
            
            CombinationalPath path = myCircuit.findCriticalPathToNet(netName);
            if (path.exists() && path.depth() > maxDepth) {
                maxDepth = path.depth();
                worstNetId = netId;
                worstPath = path;
            }
        }
    }

    if (maxDepth == -1) {
        std::cout << "  -> [Skip] No valid combinational path found in the circuit.\n\n";
    } else {
        std::string targetNetName = myCircuit.getNet(worstNetId).name;
        std::cout << "  -> Target Endpoint : " << targetNetName << " (Initial Depth: " << maxDepth << ")\n";

        // 2.2 建立 DepthReport
        DepthReport endpointReport;
        endpointReport.endpointType = DepthEndpointType::PrimaryOutput;
        endpointReport.endpointName = targetNetName;
        endpointReport.endpointNetId = worstNetId;
        endpointReport.depth = maxDepth;
        endpointReport.criticalPath = worstPath;

        // 2.3 建立 OptimizationCandidate (設定 targetDepth 為比目前少 1 層)
        int targetDepth = (maxDepth > 0) ? maxDepth - 1 : 0;
        OptimizationCandidate candidate = myCircuit.buildOptimizationCandidate(endpointReport, targetDepth);

        // 2.4 設定 DepthOptimizer 參數
        DepthOptimizerConfig config;
        config.enableBufferAndNotBypass = true;
        config.enableTreeBalancing = true;
        config.enableDeMorganPushing = true;
        config.maxAreaIncreasePerPath = 200; // 限制單一 Path 最佳化最多只能增加 200 顆 Gate

        // 【關鍵修改】：為了維持 xag 結構，優化引擎的白名單只能允許 AND, NOT 與 BUF
        std::vector<GateType> optAllowedTypes = {
            GateType::AND, GateType::NOT, GateType::BUF, GateType::XOR
        };
        std::vector<GateType> optBannedTypes = {}; 

        std::cout << "  -> [Optimization] Starting depth reduction...\n";
        // 2.5 實例化 Optimizer 並執行
        DepthOptimizer optimizer(config);
        OptimizationResult optResult = optimizer.reduceDepth(myCircuit, candidate, config, optAllowedTypes, optBannedTypes, true);

        // 2.6 將 OptimizationResult 完整輸出
        std::cout << "\n=========================================\n";
        std::cout << " [Optimization Result Report]\n";
        std::cout << "=========================================\n";
        std::cout << " Pass Name            : " << optResult.passName << "\n";
        
        std::string statusStr;
        switch (optResult.status) {
            case OptimizationStatus::SUCCESS:              statusStr = "SUCCESS (Optimized)"; break;
            case OptimizationStatus::NO_IMPROVEMENT:       statusStr = "NO_IMPROVEMENT"; break;
            case OptimizationStatus::ERROR_NOT_EQUIVALENT: statusStr = "ERROR_NOT_EQUIVALENT (Rolled back)"; break;
            case OptimizationStatus::ERROR_AREA_EXCEEDED:  statusStr = "ERROR_AREA_EXCEEDED (Rolled back)"; break;
            default:                                       statusStr = "UNKNOWN"; break;
        }
        std::cout << " Status               : " << statusStr << "\n";
        std::cout << " Message              : " << optResult.message << "\n";
        std::cout << "-----------------------------------------\n";
        std::cout << " Changed (Netlist)    : " << (optResult.changed ? "Yes" : "No") << "\n";
        std::cout << " Depth Improved       : " << (optResult.depthImproved ? "Yes" : "No") << "\n";
        std::cout << " Equivalence Checked  : " << (optResult.equivalenceChecked ? "Yes" : "No") << "\n";
        std::cout << " Equivalent           : " << (optResult.equivalent ? "Yes" : "No") << "\n";
        std::cout << "-----------------------------------------\n";
        std::cout << " Old Depth            : " << optResult.oldDepth << "\n";
        std::cout << " New Depth            : " << optResult.newDepth << "\n";
        std::cout << " Old Gate Count       : " << optResult.oldGateCount << "\n";
        std::cout << " New Gate Count       : " << optResult.newGateCount << "\n";
        
        std::cout << " Area Delta           : ";
        if (optResult.areaDelta > 0) std::cout << "+" << optResult.areaDelta << " (Bloat)\n";
        else if (optResult.areaDelta < 0) std::cout << optResult.areaDelta << " (Shrink)\n";
        else std::cout << "0 (Unchanged)\n";
        std::cout << "=========================================\n\n";
    }

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
    std::cout << "  -> Final Gate Count : " << myCircuit.getGateCount() << "\n";
    std::cout << "\n[Done] EDA flow completed smoothly.\n";

    return 0;
}