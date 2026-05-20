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

    std::cout << "[Basic Query] API spot checks:\n";
    std::cout << "         -> XOR gates listed by getGatesByType(): "
              << myCircuit.getGatesByType(GateType::XOR).size() << "\n";
    std::cout << "         -> Gates with any constant input: "
              << myCircuit.findGatesWithConstInput().size() << "\n";
    std::cout << "         -> NAND gates with constant 1 input: "
              << myCircuit.findGatesWithConstInput(GateType::NAND, 1).size() << "\n";
    const Gate* g0 = myCircuit.findGate("g0");
    if (g0) {
        std::cout << "         -> g0 type: " << gateTypeToString(g0->type)
                  << ", inputs: " << g0->inputNetIds.size()
                  << ", outputNetId: " << g0->outputNetId << "\n";
        std::vector<int> g0Successors = myCircuit.getImmediateSuccessors("g0");
        std::cout << "         -> g0 immediate successors: " << g0Successors.size();
        if (!g0Successors.empty()) {
            std::cout << " (";
            for (size_t i = 0; i < g0Successors.size(); ++i) {
                if (i) std::cout << ", ";
                std::cout << myCircuit.getGate(g0Successors[i]).instName;
            }
            std::cout << ")";
        }
        std::cout << "\n";
    } else {
        std::cout << "         -> g0 not found\n";
    }
    std::vector<int> n0Fanout = myCircuit.getDirectFanoutGatesOfNet("n0");
    std::cout << "         -> direct fanout gates of net n0: " << n0Fanout.size();
    if (!n0Fanout.empty()) {
        std::cout << " (";
        for (size_t i = 0; i < n0Fanout.size() && i < 10; ++i) {
            if (i) std::cout << ", ";
            std::cout << myCircuit.getGate(n0Fanout[i]).instName;
        }
        if (n0Fanout.size() > 10) {
            std::cout << ", ...";
        }
        std::cout << ")";
    }
    std::cout << "\n";
    std::cout << "\n";

    VerilogWriter writer;
    
    std::cout << "[Step 2] Writing output Verilog..." << std::endl;
    if (!writer.write(outputFilePath, myCircuit)) return -1;

    std::cout << "\n========================================\n";
    std::cout << "  Process Completed Successfully!       \n";
    std::cout << "========================================\n";

    return 0; 
}
