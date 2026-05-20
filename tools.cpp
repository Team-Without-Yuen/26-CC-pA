#include <iostream>
#include <sstream>
#include <string>
#include "include/core/Netlist.h"
#include "include/io/VerilogReader.h"
#include "include/io/VerilogWriter.h"

int main() {
    std::string inputFilePath;
    std::string outputFilePath = "output.v";
    std::string line;

    while (1) {
        std::cout << "> ";

        if (!std::getline(std::cin, line)) {
            break;
        }

        std::stringstream ss(line);

        std::string A;
        std::string B;

        ss >> A >> B;

        if (A.empty()) {
            continue;
        }

        if (A == "inputFile") {
            if (B.empty()) {
                std::cout << "[Error] inputFile needs a path.\n";
                continue;
            }

            inputFilePath = B;
            std::cout << "[Info] Input File set to: " << inputFilePath << "\n";
        }
        else if (A == "outputFile") {
            if (B.empty()) {
                std::cout << "[Error] outputFile needs a path.\n";
                continue;
            }

            if (inputFilePath.empty()) {
                std::cout << "[Error] Please set inputFile first.\n";
                continue;
            }

            outputFilePath = B;

            std::cout << "\n[Info] Input File  : " << inputFilePath << "\n";
            std::cout << "[Info] Output File : " << outputFilePath << "\n\n";

            Netlist myCircuit;
            VerilogReader reader;

            std::cout << "[Step 1] Reading and parsing Verilog..." << std::endl;
            if (!reader.read(inputFilePath, myCircuit)) {
                return -1;
            }

            std::cout << "         -> Parsing successful!\n";
            std::cout << "         -> Primary Inputs : "
                      << myCircuit.getPrimaryInputs().size() << "\n";
            std::cout << "         -> Primary Outputs: "
                      << myCircuit.getPrimaryOutputs().size() << "\n";
            std::cout << "         -> Total Nets     : "
                      << myCircuit.getNetCount() << "\n";
            std::cout << "         -> Total Gates    : "
                      << myCircuit.getGateCount() << "\n\n";

            VerilogWriter writer;

            std::cout << "[Step 2] Writing output Verilog..." << std::endl;
            if (!writer.write(outputFilePath, myCircuit)) {
                return -1;
            }

            std::cout << "\n========================================\n";
            std::cout << "  Process Completed Successfully!       \n";
            std::cout << "========================================\n";

            break;
        }
        else {
            std::cout << "[Error] Unknown function: " << A << "\n";
            std::cout << "        Supported functions: inputFile, outputFile\n";
        }
    }

    return 0;
}