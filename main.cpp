#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <algorithm>
#include <limits>
#include <queue> 
#include "include/core/Netlist.h"
#include "include/core/TechMapper.h"
#include "include/core/DepthOptimizer.h"
#include "include/io/VerilogReader.h"
#include "include/io/VerilogWriter.h"

int main(int argc, char* argv[]) {
    std::string inputFilePath;
    std::string outputFilePath = "output.v"; 

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
    
    std::cout << "[Step 1] Reading and parsing Verilog..." << std::endl;
    if (!reader.read(inputFilePath, myCircuit)) {
        std::cerr << "[Error] Failed to read Verilog file!" << std::endl;
        return -1; 
    }

    std::cout << "  -> Parsing Successful.\n";
    std::cout << "  -> Initial Gate Count : " << myCircuit.getGateCount() << "\n";
    std::cout << "  -> Initial Wire Count : " << myCircuit.getNetCount() << "\n\n";

    std::cout << "[Step 2] Converting to XAG and Running Depth Optimization..." << std::endl;
    std::vector<GateType> xagAllowedTypes = {GateType::AND, GateType::NOT, GateType::XOR};
    
    TechMapper mapper;
    TechMapReport mapReport = mapper.convertToBasis(
        myCircuit, 
        TargetScope::WHOLE_NETLIST, 
        "",                         
        xagAllowedTypes,            
        {},                         
        false                       
    );
    std::cout << "  -> xag Conversion Completed. Current Gate Count: " << myCircuit.getGateCount() << "\n\n";

    // ---------------------------------------------------------
    // 💡 【強制測試：直接指定名稱為 "y" 的 Net】
    // ---------------------------------------------------------
    std::cout << "---------------------------------------------------------\n";
    std::cout << " [Forced Boolean Expression Analysis] Testing Net 'n3[23]'\n";
    std::cout << "---------------------------------------------------------\n";
    
    std::string testNetName = "n3[23]";
    
    // 檢查這個 net 是否存在於電路中
    if (myCircuit.getNetId(testNetName) != -1) {
        // 呼叫你的布林展開功能
        std::string expr = myCircuit.getSimplifiedBooleanExpression(testNetName, 10);
        std::vector<std::string> pis = myCircuit.getPrimaryInputsOfNet(testNetName);

        std::cout << " Net Name: " << testNetName << "\n";
        std::cout << "  -> Expression: " << expr << "\n";
        std::cout << "  -> Depends on (" << pis.size() << " PIs): ";
        for (const auto& pi : pis) std::cout << pi << " ";
        std::cout << "\n";
    } else {
        std::cout << " [Error] Net '" << testNetName << "' does not exist in the netlist!\n";
        std::cout << " Available nets inside netlist: \n   ";
        // 順便把所有存在的 net 名字印出來瞧瞧
        for (int i = 0; i < myCircuit.getNetCount(); ++i) {
            std::cout << myCircuit.getNet(i).name << " ";
        }
        std::cout << "\n";
    }
    std::cout << "---------------------------------------------------------\n\n";
    // ---------------------------------------------------------
    // 2.1 尋找測試目標
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
    std::cout << "================================================\n\n";

    // ==========================================
    // Step 1.5: 將電路轉換為 XAG (XOR-AND Graph) 基礎邏輯
    // ==========================================
    std::cout << "[Step 1.5] Converting Netlist to XAG Basis..." << std::endl;
    TechMapper mapper;
    
    // XAG 基礎由 AND, XOR, NOT (與輔助的 BUF) 組成
    std::vector<GateType> xagBasisTypes = {GateType::AND, GateType::XOR, GateType::NOT, GateType::BUF};
    
    // 呼叫轉換 API (假設你的全域列舉值為 TargetScope::GLOBAL，請依據你的標頭檔微調)
    mapper.convertToBasis(myCircuit, TargetScope::WHOLE_NETLIST, "Global_XAG_Conversion", xagBasisTypes, {}, true);


    /*// ==========================================
    // Step 2: 執行 Global Depth Optimization
    // ==========================================
    std::cout << "[Step 2] Running Global Depth Optimization..." << std::endl;

    DepthOptimizerConfig config;
    config.enableBufferAndNotBypass = true;
    config.enableTreeBalancing = true;
    config.enableDeMorganPushing = true;
    config.enableConeResynthesis = true;
    config.maxAreaIncreasePerPath = 200; // 保持你原本的 Rollback 設定

    // ★ 限制探索空間，加速 SAT 尋找最淺深度的過程
    std::vector<GateType> optAllowedTypes = {GateType::AND, GateType::XOR, GateType::NOT}; 
    std::vector<GateType> optBannedTypes = {}; 

    DepthOptimizer optimizer;
    bool globalCircuitChanged = true;
    int globalIteration = 0;
    const int MAX_GLOBAL_ITERATIONS = 50; 

    // =========================================================
    // ★ 全域最佳化大迴圈 (Global Sweep Loop) ★
    // =========================================================
    while (globalCircuitChanged && globalIteration < MAX_GLOBAL_ITERATIONS) {
        globalCircuitChanged = false;
        globalIteration++;
        std::cout << "\n=================================================\n";
        std::cout << " [Global Sweep Iteration " << globalIteration << "]\n";
        std::cout << "=================================================\n";

        int maxDepth = -1;
        int worstNetId = -1;
        CombinationalPath worstPath;

        // 檢查 Primary Outputs
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

        // 檢查 DFF 的 D 腳
        for (size_t i = 0; i < myCircuit.getGateCount(); ++i) {
            const Gate& g = myCircuit.getGate(i);
            if (g.type == GateType::DFF) { 
                int dPinNetId = -1;
                if (!g.inputPinNames.empty()) {
                    for (size_t p = 0; p < g.inputPinNames.size(); ++p) {
                        if (g.inputPinNames[p] == "D") { dPinNetId = g.inputNetIds[p]; break; }
                    }
                } else if (g.inputNetIds.size() > 3) {
                    dPinNetId = g.inputNetIds[3]; 
                }

                if (dPinNetId != -1) {
                    std::string netName = myCircuit.getNet(dPinNetId).name;
                    CombinationalPath path = myCircuit.findCriticalPathToNet(netName);
                    if (path.exists() && path.depth() > maxDepth) {
                        maxDepth = path.depth();
                        worstNetId = dPinNetId;
                        worstPath = path;
                    }
                }
            }
        }

        if (maxDepth <= 0 || worstNetId == -1) {
            std::cout << "  -> No valid critical path found or depth is already 0. Sweep finished.\n";
            break;
        }

        std::string targetNetName = myCircuit.getNet(worstNetId).name;
        std::cout << "  -> Found Critical Endpoint : " << targetNetName << " (Depth: " << maxDepth << ")\n";

        DepthReport endpointReport;
        endpointReport.endpointType = DepthEndpointType::PrimaryOutput; 
        endpointReport.endpointName = targetNetName;
        endpointReport.endpointNetId = worstNetId;
        endpointReport.depth = maxDepth;
        endpointReport.criticalPath = worstPath;

        int targetDepth = (maxDepth > 0) ? maxDepth - 1 : 0;
        OptimizationCandidate candidate = myCircuit.buildOptimizationCandidate(endpointReport, targetDepth);

        DepthOptimizerConfig config;
        config.enableBufferAndNotBypass = true;
        config.enableTreeBalancing = true;
        config.enableDeMorganPushing = true;
        config.maxAreaIncreasePerPath = 200; 

        std::vector<GateType> optAllowedTypes = { GateType::AND, GateType::NOT, GateType::BUF, GateType::XOR };
        std::vector<GateType> optBannedTypes = {}; 

        std::cout << "  -> [Optimization] Starting depth reduction...\n";
        DepthOptimizer optimizer(config);
        OptimizationResult optResult = optimizer.reduceDepth(myCircuit, candidate, config, optAllowedTypes, optBannedTypes, true);

        // 印出優化結果報告
        std::cout << "\n=========================================\n";
        std::cout << " [Optimization Result Report]\n";
        std::cout << "=========================================\n";
        std::cout << " Status               : " << (optResult.changed ? "SUCCESS" : "NO_IMPROVEMENT") << "\n";
        std::cout << " Old Depth            : " << optResult.oldDepth << "\n";
        std::cout << " New Depth            : " << optResult.newDepth << "\n";
        std::cout << " Old Gate Count       : " << optResult.oldGateCount << "\n";
        std::cout << " New Gate Count       : " << optResult.newGateCount << "\n";
        std::cout << "=========================================\n\n";

        // 💡 優化後再次觀察布林式變化
        if (optResult.changed) {
            std::cout << " -> [Post-Opt Expression]: " << myCircuit.getSimplifiedBooleanExpression(targetNetName, 10) << "\n\n";
        }
    }*/

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