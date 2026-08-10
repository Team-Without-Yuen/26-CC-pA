#include "include/core/DesignAnalysisContext.h"

#include <utility>

#include "include/core/AigFunctionQueryAdapter.h"

DesignAnalysisContext::DesignAnalysisContext()
    : DesignAnalysisContext(Netlist{}) {}

DesignAnalysisContext::DesignAnalysisContext(Netlist netlist)
    : netlist_(std::move(netlist)), primitives_(netlist_) {}

DesignAnalysisContext::DesignAnalysisContext(
    Netlist netlist,
    eqeng::AigModel::Options buildOptions,
    eqeng::Primitives::Config primitiveConfig)
    : netlist_(std::move(netlist)),
      primitives_(netlist_, buildOptions, primitiveConfig) {}

FunctionReport DesignAnalysisContext::runFunctionQuery(
    const FunctionQuery& query) {
    if (isAigFunctionQuerySupported(query.type)) {
        return runAigFunctionQuery(netlist_, primitives_, query);
    }
    return netlist_.runFunctionQuery(query);
}

