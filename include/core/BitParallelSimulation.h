#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

class Netlist;

using BitParallelSimulationSignature = std::vector<std::uint64_t>;

struct BitParallelSimulationResult {
    std::vector<BitParallelSimulationSignature> signatures;
    std::vector<bool> known;
    std::vector<int> topoOrder;        // evaluate 的實際順序
    size_t patternCount = 0;
    std::uint64_t lastWordMask = ~std::uint64_t{0};
};

BitParallelSimulationResult simulateNetlistBitParallel(
    const Netlist& netlist,
    size_t patternCount);
