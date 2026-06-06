#include <iostream>
#include <map>
#include <string>
#include <vector>
#include "include/core/Netlist.h"
#include "include/core/TechMapper.h"
#include "include/io/VerilogReader.h"
#include "include/io/VerilogWriter.h"

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

    // 印出修改前的電路狀態
    std::cout << "  -> Parsing Successful.\n";
    std::cout << "  -> Initial Gate Count : " << myCircuit.getGateCount() << "\n";
    std::cout << "  -> Initial Wire Count : " << myCircuit.getNetCount() << "\n\n";

    // ==========================================
    // Step 2: 執行 Technology Mapping (電路改寫)
    // ==========================================
    std::cout << "[Step 2] Executing Technology Mapping (Modification Mode)..." << std::endl;
    
    TechMapper mapper; 

    // 設定情境：把所有複雜的邏輯閘拔除
    std::vector<GateType> targetTypes = {
        GateType::AND, GateType::OR, GateType::XOR, GateType::XNOR, GateType::BUF
    };
    // 允許使用的 Cell Library：只允許使用 NAND, NOR, NOT
    std::vector<GateType> allowedTypes = {
        GateType::NAND, GateType::NOR, GateType::NOT
    };

    // 【修改重點】設定為強制展開模式 (isOneToMany = true)
    // 因為我們現在只是要「修改/拆解」電路，不需要做面積縮減的 Pattern Matching
    bool isOneToMany = true;
    
    // 啟動單向替換引擎
    int gateChange = mapper.mapTechnology(myCircuit, targetTypes, allowedTypes, isOneToMany);

    std::cout << "  -> Modification Complete.\n";
    if (gateChange < 0) {
        std::cout << "  -> Gate count reduced by " << (-gateChange) << " gates.\n";
    } else if (gateChange > 0) {
        std::cout << "  -> Gate count increased by " << gateChange << " gates (due to decomposition).\n";
    } else {
        std::cout << "  -> Gate count remained unchanged.\n";
    }

    // 計算真正存活的 Gate 數量 (過濾掉被標記為 UNKNOWN 的 Tombstone 墓碑)
    size_t activeGateCount = 0;
    for (size_t i = 0; i < myCircuit.getGateCount(); ++i) {
        if (myCircuit.getGate(i).type != GateType::UNKNOWN) {
            activeGateCount++;
        }
    }
    std::cout << "  -> Final Active Gate Count : " << activeGateCount << "\n\n";

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