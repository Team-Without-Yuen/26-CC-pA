#include <iostream>
#include <string>

// 引入你的自定義標頭檔
#include "include/core/Netlist.h"
#include "include/io/VerilogReader.h"
#include "include/io/VerilogWriter.h"
#include "include/core/MockturtleConverter.h"
#include "include/core/DepthOptimizer.h"

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
    
    // ---------------------------------------------------------
    // [Step 1] 讀取並解析 Verilog
    // ---------------------------------------------------------
    std::cout << "[Step 1] Reading and parsing Verilog..." << std::endl;
    if (!reader.read(inputFilePath, myCircuit)) {
        std::cerr << "[Error] Failed to read Verilog file!" << std::endl;
        return -1; 
    }

    std::cout << "  -> Parsing Successful.\n";
    std::cout << "  -> Initial Gate Count : " << myCircuit.getGateCount() << "\n";
    std::cout << "  -> Initial Wire Count : " << myCircuit.getNetCount() << "\n\n";

    // ---------------------------------------------------------
    // [Step 2] 使用 Mockturtle 進行 Critical Path 優化
    // ---------------------------------------------------------
    std::cout << "[Step 2] Executing Critical Path Optimization..." << std::endl;

    // 準備核心引擎
    TechMapper techMapper;
    DepthOptimizer depthOptimizer;

    // 準備測試參數
    // 1. 建立一個空的 ConeReport (ok 和 exists 預設為 false，代表全域優化)
    ConeReport emptyConeReport; 

    // 2. 設定我們想要的目標邏輯閘組合 (這裡以測試全域 AIG 為例)
    std::vector<GateType> allowedTypes = {GateType::AND, GateType::NOT, GateType::XOR};
    std::vector<GateType> bannedTypes = {}; 

    // 執行優化，並開啟 verbose 模式觀察我們剛才寫的流程日誌
    bool verbose = true;
    OptimizationResult optResult = depthOptimizer.executeCriticalPathOptimization(
        myCircuit, 
        techMapper, 
        allowedTypes, 
        emptyConeReport, 
        bannedTypes, 
        verbose
    );

    // 檢查與回報優化結果
    if (optResult.status == OptimizationStatus::SUCCESS) {
        std::cout << "  -> Optimization Completed Successfully!\n";
        std::cout << "  -> Depth Status     : " << optResult.oldDepth << " -> " << optResult.newDepth 
                  << (optResult.depthImproved ? " (Improved!)" : " (No change)") << "\n";
        std::cout << "  -> Final Gate Count : " << optResult.newGateCount << " (Delta: " << optResult.areaDelta << ")\n\n";
    } else {
        std::cerr << "  -> Optimization Failed or Aborted: " << optResult.message << "\n\n";
        // 如果專案有實作 Rollback 機制，可以在這裡觸發
    }

    // ---------------------------------------------------------
    // [Step 3] 輸出寫回 Verilog 檔案
    // ---------------------------------------------------------
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