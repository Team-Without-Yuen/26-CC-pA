#include "include/core/Netlist.h"
#include "include/io/VerilogReader.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct TestReport {
    int passed = 0;
    int failed = 0;

    void check(bool condition, const std::string& name) {
        if (condition) {
            ++passed;
            std::cout << "[PASS] " << name << "\n";
        } else {
            ++failed;
            std::cout << "[FAIL] " << name << "\n";
        }
    }
};

bool containsString(const std::vector<std::string>& values, const std::string& target) {
    return std::find(values.begin(), values.end(), target) != values.end();
}

bool loadCircuit(Netlist& netlist) {
    VerilogReader reader;
    return reader.read("mini test/test4/function_expr_circuit.v", netlist);
}

void testBooleanExpression(TestReport& report, const Netlist& netlist) {
    Netlist::FunctionQuery query;
    query.type = Netlist::FunctionQueryType::BooleanExpression;
    query.netNameA = "y";

    const Netlist::FunctionReport result = netlist.runFunctionQuery(query);
    report.check(result.ok &&
                 result.exists &&
                 result.status == "BOOLEAN_EXPRESSION" &&
                 result.expression == "OR(AND(a, b), NOT(c))" &&
                 result.expressionLength == result.expression.size() &&
                 !result.expressionDepthLimited &&
                 result.maxExpressionDepth == -1 &&
                 containsString(result.supportPrimaryInputs, "a") &&
                 containsString(result.supportPrimaryInputs, "b") &&
                 containsString(result.supportPrimaryInputs, "c") &&
                 result.supportPrimaryInputs.size() == 3 &&
                 result.supportRealPrimaryInputs == result.supportPrimaryInputs &&
                 result.supportDffPseudoInputs.empty() &&
                 result.supportUndrivenLeaves.empty(),
                 "function_query boolean_expression y");
}

void testGateTypeExpressions(TestReport& report, const Netlist& netlist) {
    Netlist::FunctionQuery query;
    query.type = Netlist::FunctionQueryType::BooleanExpression;

    query.netNameA = "x";
    Netlist::FunctionReport result = netlist.runFunctionQuery(query);
    report.check(result.ok && result.expression == "XOR(b, c)",
                 "function_query boolean_expression xor");

    query.netNameA = "nand_out";
    result = netlist.runFunctionQuery(query);
    report.check(result.ok && result.expression == "NAND(a, c)",
                 "function_query boolean_expression nand");

    query.netNameA = "xnor_out";
    result = netlist.runFunctionQuery(query);
    report.check(result.ok && result.expression == "XNOR(a, b)",
                 "function_query boolean_expression xnor");
}

void testSimplifiedExpression(TestReport& report, const Netlist& netlist) {
    Netlist::FunctionQuery query;
    query.type = Netlist::FunctionQueryType::SimplifiedBooleanExpression;
    query.netNameA = "limited";
    query.maxExpressionDepth = 1;

    const Netlist::FunctionReport result = netlist.runFunctionQuery(query);
    report.check(result.ok &&
                 result.exists &&
                 result.status == "SIMPLIFIED_BOOLEAN_EXPRESSION" &&
                 result.expression == "NOT(n_deep2)" &&
                 result.expressionDepthLimited &&
                 result.maxExpressionDepth == 1,
                 "function_query simplified_expression limited");
}

void testDffBoundarySupport(TestReport& report, const Netlist& netlist) {
    Netlist::FunctionQuery query;
    query.type = Netlist::FunctionQueryType::BooleanExpression;
    query.netNameA = "q_expr";

    const Netlist::FunctionReport expr = netlist.runFunctionQuery(query);
    report.check(expr.ok &&
                 expr.expression == "OR(q, a)" &&
                 containsString(expr.supportPrimaryInputs, "a") &&
                 containsString(expr.supportPrimaryInputs, "q") &&
                 expr.supportPrimaryInputs.size() == 2 &&
                 expr.supportRealPrimaryInputs == std::vector<std::string>{"a"} &&
                 expr.supportDffPseudoInputs == std::vector<std::string>{"q"} &&
                 expr.supportUndrivenLeaves.empty(),
                 "function_query boolean_expression dff_q_boundary");

    query.type = Netlist::FunctionQueryType::PrimaryInputsOfNet;
    const Netlist::FunctionReport support = netlist.runFunctionQuery(query);
    report.check(support.ok &&
                 support.exists &&
                 support.status == "PRIMARY_INPUT_SUPPORT" &&
                 containsString(support.supportPrimaryInputs, "a") &&
                 containsString(support.supportPrimaryInputs, "q") &&
                 support.supportPrimaryInputs.size() == 2 &&
                 support.supportRealPrimaryInputs == std::vector<std::string>{"a"} &&
                 support.supportDffPseudoInputs == std::vector<std::string>{"q"} &&
                 support.supportUndrivenLeaves.empty(),
                 "function_query primary_inputs_of_net dff_q_boundary");
}

void testUndrivenSupportBreakdown(TestReport& report, const Netlist& netlist) {
    Netlist::FunctionQuery query;
    query.type = Netlist::FunctionQueryType::PrimaryInputsOfNet;
    query.netNameA = "floating_expr";

    const Netlist::FunctionReport result = netlist.runFunctionQuery(query);
    report.check(result.ok &&
                 result.exists &&
                 result.supportPrimaryInputs ==
                     std::vector<std::string>({"a", "floating_leaf"}) &&
                 result.supportRealPrimaryInputs ==
                     std::vector<std::string>({"a"}) &&
                 result.supportDffPseudoInputs.empty() &&
                 result.supportUndrivenLeaves ==
                     std::vector<std::string>({"floating_leaf"}),
                 "function_query support breakdown undriven leaf");
}

void testRemovedGateCreatesUndrivenBoundary(TestReport& report, const Netlist& original) {
    Netlist netlist = original;
    const int removedGateId = netlist.getGateId("g_and");
    const bool removed = netlist.removeGateAndDetachPins(removedGateId);

    Netlist::FunctionQuery query;
    query.type = Netlist::FunctionQueryType::PrimaryInputsOfNet;
    query.netNameA = "y";
    const Netlist::FunctionReport result = netlist.runFunctionQuery(query);

    report.check(removed &&
                 netlist.isGateRemoved(removedGateId) &&
                 result.ok &&
                 result.supportPrimaryInputs ==
                     std::vector<std::string>({"c", "n_and"}) &&
                 result.supportRealPrimaryInputs ==
                     std::vector<std::string>({"c"}) &&
                 result.supportDffPseudoInputs.empty() &&
                 result.supportUndrivenLeaves ==
                     std::vector<std::string>({"n_and"}),
                 "function_query removed gate becomes undriven boundary");
}

void testSatFunctionQueries(TestReport& report, const Netlist& netlist) {
    Netlist::FunctionQuery query;
    query.type = Netlist::FunctionQueryType::AlwaysZero;
    query.netNameA = "const_zero";

    Netlist::FunctionReport result = netlist.runFunctionQuery(query);
    report.check(result.ok &&
                 result.exists &&
                 result.status == "ALWAYS_ZERO" &&
                 result.solverRan &&
                 !result.solverTimedOut &&
                 !result.solverUnknown &&
                 !result.unsupported &&
                 result.canBeZero &&
                 !result.canBeOne,
                 "function_query sat always_zero");

    query.type = Netlist::FunctionQueryType::AlwaysOne;
    query.netNameA = "const_one";
    result = netlist.runFunctionQuery(query);
    report.check(result.ok &&
                 result.exists &&
                 result.status == "ALWAYS_ONE" &&
                 result.solverRan &&
                 !result.solverTimedOut &&
                 !result.solverUnknown &&
                 result.canBeOne &&
                 !result.canBeZero,
                 "function_query sat always_one");

    query.type = Netlist::FunctionQueryType::TruthStatus;
    query.netNameA = "y";
    result = netlist.runFunctionQuery(query);
    report.check(result.ok &&
                 result.exists &&
                 result.status == "NON_CONSTANT" &&
                 result.solverRan &&
                 result.canBeZero &&
                 result.canBeOne &&
                 !result.isConstant,
                 "function_query sat truth_status");

    query.type = Netlist::FunctionQueryType::CanBeValue;
    query.netNameA = "const_zero";
    query.constValue = 1;
    result = netlist.runFunctionQuery(query);
    report.check(result.ok &&
                 !result.exists &&
                 result.status == "UNSATISFIABLE_VALUE" &&
                 result.solverRan &&
                 result.solverStatus == "UNSAT",
                 "function_query sat can_be_value_unsat");

    query.type = Netlist::FunctionQueryType::Equivalence;
    query.netNameA = "y";
    query.netNameB = "y_copy";
    query.constValue = -1;
    result = netlist.runFunctionQuery(query);
    report.check(result.ok &&
                 result.exists &&
                 result.equivalent &&
                 result.status == "EQUIVALENT" &&
                 result.solverRan &&
                 result.solverStatus == "UNSAT",
                 "function_query sat equivalence");
}

void testInvalidQueries(TestReport& report, const Netlist& netlist) {
    Netlist::FunctionQuery query;
    query.type = Netlist::FunctionQueryType::BooleanExpression;
    query.netNameA = "missing_net";
    Netlist::FunctionReport result = netlist.runFunctionQuery(query);
    report.check(!result.ok &&
                 !result.exists &&
                 result.status == "NET_NOT_FOUND",
                 "function_query boolean_expression missing_net");

    query.type = Netlist::FunctionQueryType::SimplifiedBooleanExpression;
    query.netNameA = "y";
    query.maxExpressionDepth = -1;
    result = netlist.runFunctionQuery(query);
    report.check(!result.ok &&
                 result.status == "INVALID_ARGUMENT",
                 "function_query simplified_expression invalid_depth");
}

} // namespace

int main() {
    TestReport report;
    Netlist netlist;

    report.check(loadCircuit(netlist), "test4 load function expression circuit");
    if (report.failed == 0) {
        testBooleanExpression(report, netlist);
        testGateTypeExpressions(report, netlist);
        testSimplifiedExpression(report, netlist);
        testDffBoundarySupport(report, netlist);
        testUndrivenSupportBreakdown(report, netlist);
        testRemovedGateCreatesUndrivenBoundary(report, netlist);
        testSatFunctionQueries(report, netlist);
        testInvalidQueries(report, netlist);
    }

    std::cout << "Summary: " << report.passed << " passed, "
              << report.failed << " failed.\n";
    return report.failed == 0 ? 0 : 1;
}
