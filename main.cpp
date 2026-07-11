#include <iostream>
#include <string>
#include <mockturtle/networks/xag.hpp>
#include <mockturtle/views/depth_view.hpp>
#include <mockturtle/algorithms/cut_rewriting.hpp>
#include <mockturtle/algorithms/node_resynthesis/xag_npn.hpp>
#include <mockturtle/algorithms/cleanup.hpp>

// 引入你的自定義標頭檔
#include "include/core/Netlist.h"
#include "include/io/VerilogReader.h"
#include "include/io/VerilogWriter.h"
#include "include/core/MockturtleConverter.h"

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
    // [Step 2] 使用 Mockturtle 進行 XAG 轉換與 Critical Path 優化
    // ---------------------------------------------------------
    std::cout << "[Step 2] Running Mockturtle Optimization (XAG)..." << std::endl;

    // 2.1 將 Netlist 轉換為 XAG
    std::cout << "  -> Converting Netlist to XAG..." << std::endl;
    mockturtle::xag_network xag = NetlistToXag(myCircuit);

    // 2.2 使用 depth_view 測量優化前的深度 (Critical Path)
    mockturtle::depth_view depth_xag_before{xag};
    std::cout << "  [Stats Before] Gates: " << xag.num_gates() 
              << ", Depth (Levels): " << depth_xag_before.depth() << "\n";

    // 2.3 執行 Depth-oriented Rewriting (縮減 Critical Path)
    std::cout << "  -> Optimizing XAG for depth..." << std::endl;
    mockturtle::xag_npn_resynthesis<mockturtle::xag_network> resyn;
    mockturtle::cut_rewriting_params ps;
    ps.cut_enumeration_ps.cut_size = 4; // LUT size 參數，可依比賽需求調整 (通常 4~6)
    
    mockturtle::cut_rewriting(xag, resyn, ps);

    // 清理 rewriting 產生但未使用的孤兒節點 (Dangling nodes)
    xag = mockturtle::cleanup_dangling(xag);

    // 2.4 測量優化後的深度
    mockturtle::depth_view depth_xag_after{xag};
    std::cout << "  [Stats After]  Gates: " << xag.num_gates() 
              << ", Depth (Levels): " << depth_xag_after.depth() << "\n";

    // 2.5 將優化後的 XAG 轉回全新的 Netlist
    std::cout << "  -> Converting optimized XAG back to Netlist..." << std::endl;
    Netlist optimizedCircuit = XagToNetlist(xag, myCircuit);

    // 覆蓋原本的電路
    myCircuit = optimizedCircuit;

    std::cout << "  -> Conversion Successful.\n";
    std::cout << "  -> Optimized Gate Count : " << myCircuit.getGateCount() << "\n";
    std::cout << "  -> Optimized Wire Count : " << myCircuit.getNetCount() << "\n\n";

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