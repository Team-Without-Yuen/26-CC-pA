#pragma once

#include "include/core/NetlistQueries.h"

class Netlist;

namespace eqeng {
class Primitives;
}

// Internal bridge used while the AIG backend is validated against the existing
// Function Query implementation. Public tools should continue to use the
// established query/report types and must not handle SigRef directly.
bool isAigFunctionQuerySupported(FunctionQueryType type);

FunctionReport runAigFunctionQuery(
    const Netlist& netlist,
    eqeng::Primitives& primitives,
    const FunctionQuery& query);

