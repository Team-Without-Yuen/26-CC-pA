#include <iostream>
#include <map>
#include <string>
#include <vector>
#include "include/core/Netlist.h"

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

    std::cout << "\n[Info] Input File  : " << inputFilePath << "\n";
    std::cout << "[Info] Output File : " << outputFilePath << "\n\n";

    Netlist myCircuit;
    VerilogReader reader;
    
    std::cout << "[Step 1] Reading and parsing Verilog..." << std::endl;
    if (!reader.read(inputFilePath, myCircuit)) return -1; 

    std::cout << "         -> Parsing successful!\n";
    std::cout << "         -> Primary Inputs : " << myCircuit.getPrimaryInputs().size() << "\n";
    std::cout << "         -> Primary Outputs: " << myCircuit.getPrimaryOutputs().size() << "\n";
    std::cout << "         -> Total Nets     : " << myCircuit.getNetCount() << "\n";
    std::cout << "         -> Total Gates    : " << myCircuit.getGateCount() << "\n\n";

    std::cout << "[Basic Query] Gate type breakdown:\n";
    std::map<GateType, int> gateCounts = myCircuit.countGatesByType();
    std::vector<GateType> orderedTypes = {
        GateType::AND, GateType::OR, GateType::NOT,
        GateType::NAND, GateType::NOR, GateType::XOR,
        GateType::XNOR, GateType::BUF, GateType::DFF
    };
    for (GateType type : orderedTypes) {
        std::cout << "         -> " << gateTypeToString(type) << ": " << gateCounts[type] << "\n";
    }
    std::cout << "\n";

    // bool isEquivalent = myCircuit.checkEquivalence(netA, netB);

    /*if (isEquivalent) {
        std::cout << " => Result: [ EQUIVALENT ] (UNSAT: No counterexample found)\n";
    } else {
        std::cout << " => Result: [ NOT EQUIVALENT ] (SAT: Counterexample found / Not Found)\n";
    }
    std::cout << "----------------------------------------\n";*/

     // --- 第五種：最長組合邏輯路徑 ---
    std::string startNet = "n0[0]";
    std::string endNet   = "n63[1]";
    std::cout << "\n[Longest Path] " << startNet << " -> " << endNet << "\n";
 
    auto [depth, path] = myCircuit.getLongestPath(startNet, endNet);
 
    if (depth < 0) {
        std::cout << "  -> No path found between " << startNet << " and " << endNet << ".\n";
    } else {
        std::cout << "  -> Depth: " << depth << " gate(s)\n";
        std::cout << "  -> Path (" << path.size() << " nets):\n";
        for (const auto& net : path)
            std::cout << "       " << net << "\n";
    }
    std::cout << "----------------------------------------\n";
 
   
    // --- 輸出 longcombipath_output.v ---
    VerilogWriter writer;
    std::string outPath = "testcase/test14/longcombipath_output.v";
    std::cout << "\n[Step 2] Writing output to " << outPath << "...\n";
    if (!writer.write(outPath, myCircuit)) return -1;
    std::cout << "  -> Done.\n";
   
    
    // --- Transitive Fanin Cone of n3 ---
    std::cout << "\n[Fanin Cone] Computing transitive fanin cone of n3...\n";
    auto faninCone = myCircuit.getTransitiveFaninCone("n3");
    if (faninCone.rootNetId < 0) {
        std::cout << "  -> Net 'n3' not found.\n";
    } else {
        std::cout << "  -> Total nets in fanin cone: " << faninCone.netIds.size() << "\n";
        std::cout << "  -> Net list:\n";
        for (int id : faninCone.netIds)
            std::cout << "       " << myCircuit.getNet(id).name << "\n";
    }
    std::cout << "----------------------------------------\n";
 
    // --- Transitive Fanout Cone of n0[0] ---
    std::cout << "\n[Fanout Cone] Computing transitive fanout cone of n0[0]...\n";
    auto fanoutCone = myCircuit.getTransitiveFanoutCone("n0[0]");
    if (fanoutCone.rootNetId < 0) {
        std::cout << "  -> Net 'n0[0]' not found.\n";
    } else {
        std::cout << "  -> Total nets in fanout cone: " << fanoutCone.netIds.size() << "\n";
        std::cout << "  -> Net list:\n";
        for (int id : fanoutCone.netIds)
            std::cout << "       " << myCircuit.getNet(id).name << "\n";
    }
    std::cout << "----------------------------------------\n";


    return 0; 
}
