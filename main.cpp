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

    std::cout << "\n=== [Sanity Check] Verifying DFF Connections ===\n";
    for (size_t i = 0; i < myCircuit.getGateCount(); ++i) {
        const Gate& g = myCircuit.getGate(i);
        if (g.type == GateType::DFF) {
            std::cout << "DFF Instance: " << g.instName << "\n";
            for (size_t j = 0; j < g.inputNetIds.size(); ++j) {
                int netId = g.inputNetIds[j];
                std::string pinName = (j < g.inputPinNames.size()) ? g.inputPinNames[j] : "UNKNOWN";
                
                if (netId == -1) {
                    std::cout << "  - Pin " << pinName << " is UNCONNECTED (NetId: -1)\n";
                    continue;
                }

                const Net& n = myCircuit.getNet(netId);
                std::cout << "  - Pin " << pinName << " connected to Net: " << n.name << " (ID: " << netId << ")\n";
                
                // 檢查反向連結
                bool foundReverseLink = false;
                for (int loadId : n.loadGateIds) {
                    if (loadId == (int)i) {
                        foundReverseLink = true;
                        break;
                    }
                }
                if (!foundReverseLink) {
                    std::cout << "    [ERROR] CRITICAL BUG! Net " << n.name << " (ID: " << netId 
                            << ") does NOT have this DFF in its loadGateIds!\n";
                } else {
                    std::cout << "    [OK] Reverse link verified.\n";
                }
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

        // 1. 先用力壓深度
        OptimizationResult optResult = optimizer.reduceDepth(myCircuit, candidate, config, optAllowedTypes, optBannedTypes, true);

        if (optResult.status == OptimizationStatus::SUCCESS && optResult.changed) {
            std::cout << "  -> Depth Sweep successful. Depth reduced from " << optResult.oldDepth << " to " << optResult.newDepth << ".\n";
            globalCircuitChanged = true; 

            // =========================================================
            // 【策略 2 實作】：即時 Area Recovery Pass
            // 在不破壞剛壓好的 newDepth 前提下，立刻清理多餘的邏輯分支
            // =========================================================
            std::cout << "  -> Running Area Recovery on the optimized path...\n";
            OptimizationCandidate areaCandidate = myCircuit.buildOptimizationCandidate(endpointReport, optResult.newDepth);
            
            // 呼叫 runExactAreaPass，strictDepthLimit 設為 optResult.newDepth
            optimizer.runExactAreaPass(myCircuit, mapper, areaCandidate, optResult.newDepth, optAllowedTypes, optBannedTypes, false);
            
        } else {
            std::cout << "  -> Optimizer stuck on this path. Moving to next phase or stopping.\n";
        }
    }

    std::cout << "\n[Global Optimization Finished] Ran " << globalIteration << " iterations.\n";


    // =========================================================
    // 【策略 3 實作】：Step 3 - Final Technology Mapping (Gate Resynthesis)
    // =========================================================
    std::cout << "\n[Step 3] Running Final Gate Resynthesis (Technology Mapping)...\n";
    
    // 開放所有的邏輯閘，讓 SAT 引擎可以自動把碎裂的 AND/XOR 重新打包成 NAND/NOR 等精簡結構
    std::vector<GateType> allAllowedTypes = {
        GateType::NAND, GateType::NOR, GateType::AND, 
        GateType::OR, GateType::XOR, GateType::XNOR, GateType::NOT
    };

    // 收集所有需要做最後打包的端點 (PO + DFF)
    std::vector<int> allEndpoints;
    for (const auto& port : myCircuit.getPrimaryOutputs()) {
        for (int netId : port.netIds) allEndpoints.push_back(netId);
    }
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
            if (dPinNetId != -1) allEndpoints.push_back(dPinNetId);
        }
    }

    // 針對全電路的每個端點，用最寬鬆的 AllowedTypes 跑一次 Area Pass 進行打包
    for (int netId : allEndpoints) {
        std::string netName = myCircuit.getNet(netId).name;
        CombinationalPath path = myCircuit.findCriticalPathToNet(netName);
        
        if (path.exists() && path.depth() > 0) {
            DepthReport rep;
            rep.endpointName = netName;
            rep.endpointNetId = netId;
            rep.depth = path.depth();
            rep.criticalPath = path;
            
            OptimizationCandidate cand = myCircuit.buildOptimizationCandidate(rep, path.depth());
            
            // 使用 allAllowedTypes 進行打包，限制深度不可增加
            optimizer.runExactAreaPass(myCircuit, mapper, cand, path.depth(), allAllowedTypes, optBannedTypes, false);
        }
    }

    // 最後清掃因重新打包而產生的浮接邏輯閘
    // myCircuit.removeDanglingGates(); // 假設你有這個函數，沒有的話用 DCE pass 替代
    std::cout << "[Step 3] Technology Mapping Complete.\n";*/

    // ==========================================
    // Step 3: 寫出修改後的 Verilog 檔案
    // ==========================================
    /*for (size_t i = 0; i < myCircuit.getGateCount(); ++i) {
        if (myCircuit.getGate(i).type == GateType::DFF) {
            std::cout << "[Debug Final] DFF: " << myCircuit.getGate(i).instName 
                    << " is connected to Net ID: " << myCircuit.getGate(i).inputNetIds[3] // 假設 index 3 是 D 腳位
                    << " (Name: " << myCircuit.getNet(myCircuit.getGate(i).inputNetIds[3]).name << ")\n";
        }
    }*/

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