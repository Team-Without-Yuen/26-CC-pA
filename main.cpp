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

    std::string netA, netB;
    netA = "n2233";
    netB = "n2193";
    std::cout << "\n[LEC] Checking equivalence between '" << netA << "' and '" << netB << "'...\n";

    bool isEquivalent = myCircuit.checkEquivalence(netA, netB);

    if (isEquivalent) {
        std::cout << " => Result: [ EQUIVALENT ] (UNSAT: No counterexample found)\n";
    } else {
        std::cout << " => Result: [ NOT EQUIVALENT ] (SAT: Counterexample found / Not Found)\n";
    }
    std::cout << "----------------------------------------\n";

    // --- 第五種：最長組合邏輯路徑 ---
    std::string startNet = "n0[0]";
    std::string endNet   = "n63[1]";
    std::cout << "\n[Longest Path] " << startNet << " -> " << endNet << "\n";

    std::pair<int, std::vector<std::string>> longestResult = myCircuit.getLongestPath(startNet, endNet);
    int depth = longestResult.first;
    std::vector<std::string> path = longestResult.second;

    if (depth < 0) {
        std::cout << "  -> No path found between " << startNet << " and " << endNet << ".\n";
    } else {
        std::cout << "  -> Depth: " << depth << " gate(s)\n";
        std::cout << "  -> Path (" << path.size() << " nets):\n";
        for (int i = 0; i < (int)path.size(); i++)
            std::cout << "       " << path[i] << "\n";
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
    ConeResult faninCone = myCircuit.getTransitiveFaninCone("n3");
    if (faninCone.rootNetId < 0) {
        std::cout << "  -> Net 'n3' not found.\n";
    } else {
        std::cout << "  -> Total nets in fanin cone: " << faninCone.netIds.size() << "\n";
        std::cout << "  -> Net list:\n";
        for (std::unordered_set<int>::iterator it = faninCone.netIds.begin(); it != faninCone.netIds.end(); ++it)
            std::cout << "       " << myCircuit.getNet(*it).name << "\n";
    }
    std::cout << "----------------------------------------\n";

    // --- Transitive Fanout Cone of n0[0] ---
    std::cout << "\n[Fanout Cone] Computing transitive fanout cone of n0[0]...\n";
    ConeResult fanoutCone = myCircuit.getTransitiveFanoutCone("n0[0]");
    if (fanoutCone.rootNetId < 0) {
        std::cout << "  -> Net 'n0[0]' not found.\n";
    } else {
        std::cout << "  -> Total nets in fanout cone: " << fanoutCone.netIds.size() << "\n";
        std::cout << "  -> Net list:\n";
        for (std::unordered_set<int>::iterator it = fanoutCone.netIds.begin(); it != fanoutCone.netIds.end(); ++it)
            std::cout << "       " << myCircuit.getNet(*it).name << "\n";
    }
    std::cout << "----------------------------------------\n";

        // 第三種：找所有路徑（含避開節點）
    std::vector<std::string> avoid1;
    avoid1.push_back("n1127");
    std::vector<std::vector<std::string>> paths3 = myCircuit.getAllPaths("n12", "n25[0]", avoid1);
    std::cout << "\n[All Paths] n12 -> n25[0] (avoiding n1127)\n";
    std::cout << "  -> Total paths: " << paths3.size() << "\n";

    // 第六種：起點是 PI，終點是 PO
    std::vector<std::string> avoid2;
    avoid2.push_back("n4156");
    std::vector<std::vector<std::string>> paths6 = myCircuit.getAllPaths("n13", "n25[1]", avoid2, true);
    std::cout << "\n[All Paths PI->PO] n13 -> n25[1] (avoiding n4156)\n";
    std::cout << "  -> Total paths: " << paths6.size() << "\n";
    for (int i = 0; i < (int)paths6.size(); i++) {
        std::cout << "  Path " << i+1 << ": ";
        for (int j = 0; j < (int)paths6[i].size(); j++)
            std::cout << paths6[i][j] << " ";
        std::cout << "\n";
    }
    return 0; 
}