#pragma once

#include "include/SATEngine/Primitives.h"
#include "include/core/Netlist.h"

// Owns the current named netlist and its single shared functional index.
// The context is deliberately non-copyable and non-movable because Primitives
// stores a reference to the Netlist member.
class DesignAnalysisContext {
public:
    DesignAnalysisContext();
    explicit DesignAnalysisContext(Netlist netlist);
    DesignAnalysisContext(Netlist netlist,
                          eqeng::AigModel::Options buildOptions,
                          eqeng::Primitives::Config primitiveConfig);

    DesignAnalysisContext(const DesignAnalysisContext&) = delete;
    DesignAnalysisContext& operator=(const DesignAnalysisContext&) = delete;
    DesignAnalysisContext(DesignAnalysisContext&&) = delete;
    DesignAnalysisContext& operator=(DesignAnalysisContext&&) = delete;

    Netlist& netlist() { return netlist_; }
    const Netlist& netlist() const { return netlist_; }

    eqeng::Primitives& primitives() { return primitives_; }

    // Uses the AIG adapter for the validated first-batch modes. Other Function
    // Query modes retain the existing implementation until their report
    // details can be preserved by an AIG/hybrid backend.
    FunctionReport runFunctionQuery(const FunctionQuery& query);

private:
    Netlist netlist_;
    eqeng::Primitives primitives_;
};

