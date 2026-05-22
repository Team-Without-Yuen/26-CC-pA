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

    return 0; 
}
