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
    // Step 2: 執行 Technology Mapping (Optimize Pattern 測試)
    // ==========================================
    std::cout << "[Step 2] Testing optimizePattern Engine..." << std::endl;
    TechMapper mapper;

    // 2.1 建構我們要尋找與優化的目標形狀 (LHS Target)
    int testChoice = 1;
    std::cout << "\nSelect Test Target:\n";
    std::cout << "  1) Area Test  : F = (A & B) | (A & C)\n";
    std::cout << "  2) Depth Test : F = ((A & B) & C) & D\n";
    std::cout << "> ";
    std::cin >> testChoice;

    std::shared_ptr<PatternNode> lhsTarget;
    if (testChoice == 1) {
        // 匹配 test_area 模組的圖形
        lhsTarget = _OR( _AND(A(), B()), _AND(A(), C()) );
    } else {
        // 匹配 test_depth 模組的圖形 (注意這是一個深度為 3 的左傾樹)
        lhsTarget = _AND( _AND( _AND(A(), B()), C()), D() ); 
    }

    // 2.2 互動式設定優化目標
    int goalChoice = 1;
    std::cout << "\nSelect Optimization Goal:\n";
    std::cout << "  1) AREA (Minimize gate count)\n";
    std::cout << "  2) DEPTH (Minimize critical path logic levels)\n";
    std::cout << "> ";
    std::cin >> goalChoice;
    OptimizationGoal goal = (goalChoice == 2) ? OptimizationGoal::DEPTH : OptimizationGoal::AREA;

    // 2.3 互動式設定最大容許面積 (僅在 DEPTH 模式下發揮作用)
    int maxAreaOverhead = 0; // 對 AREA 模式來說不需要 overhead
    if (goal == OptimizationGoal::DEPTH) {
        std::cout << "\nEnter Max Area Overhead (e.g., 1 or 2): ";
        std::cin >> maxAreaOverhead;
    }

    // 2.4 執行全域自動優化引擎
    std::string ruleName = "Test_Global_Optimization";
    bool verbose = true;
    
    TechMapReport report = mapper.optimizePattern(
        myCircuit, 
        lhsTarget, 
        goal, 
        TargetScope::WHOLE_NETLIST, 
        ruleName, 
        verbose,
        maxAreaOverhead // 傳入我們剛剛討論新增的面積天花板參數
    );

    // 2.5 顯示報告結果
    std::cout << "\n=========================================\n";
    std::cout << "[Report] Optimization Result\n";
    std::cout << "=========================================\n";
    if (report.status == TechMapStatus::SUCCESS) {
        std::cout << " -> Status: SUCCESS\n";
        std::cout << " -> Message: " << report.message << "\n";
        
        // 統計拔除與新增的閘數量
        int removedTotal = 0, addedTotal = 0;
        for (const auto& pair : report.removedCountByType) removedTotal += pair.second;
        for (const auto& pair : report.addedCountByType) addedTotal += pair.second;
        
        std::cout << " -> Gates Removed: " << removedTotal << "\n";
        std::cout << " -> Gates Added  : " << addedTotal << "\n";
        std::cout << " -> Net Area Gain: " << (addedTotal - removedTotal) << " gates\n";
    } else {
        std::cout << " -> Status: FAILED or NO CHANGE\n";
        std::cout << " -> Reason: " << report.message << "\n";
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
    std::cout << "  -> Final Gate Count : " << myCircuit.getGateCount() << "\n";
    std::cout << "\n[Done] EDA flow completed smoothly.\n";

    return 0;
}