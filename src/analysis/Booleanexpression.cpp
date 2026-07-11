// =========================================================================
// BooleanExpression.cpp
// Symbolic traversal：從指定 net 往回追 fanin cone，
// 把電路展開成人類可讀的 Boolean expression 字串。
//
// 支援的 gate type：AND / OR / NAND / NOR / NOT / BUF / XOR / XNOR
// DFF.Q 視為 pseudo PI，不穿越 DFF。
//
// 用法：
//   std::string expr = netlist.getBooleanExpression("n12");
//   // 回傳例如：AND(OR(a, b), NOT(c))
// =========================================================================
#include "include/core/Netlist.h"
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <sstream>
#include <vector>
#include <queue>
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
// 內部 helper：遞迴展開 netId 對應的 Boolean expression
// memo：已算過的 netId → expression 字串，避免重複展開 fanout sharing
// depth：防止無限遞迴（理論上 DAG 不會，但防呆）
// ─────────────────────────────────────────────────────────────────────────────
static std::string buildExpr(
    int netId,
    const std::vector<Gate>& gates,
    const std::vector<Net>& nets,
    std::unordered_map<int, std::string>& memo,
    int depth)
{
    if (netId < 0 || netId >= (int)nets.size())
        return "?";

    // 已算過直接回傳
    auto it = memo.find(netId);
    if (it != memo.end())
        return it->second;

    // 防止過深展開（電路可能很大）
    if (depth > 200)
        return nets[netId].name;

    const Net& net = nets[netId];

    // PI / constant / DFF.Q → 直接用 net name 作為 leaf
    if (net.isPI || net.isConst || net.driverGateId < 0) {
        memo[netId] = net.name;
        return net.name;
    }

    int gateId = net.driverGateId;
    if (gateId < 0 || gateId >= (int)gates.size()) {
        memo[netId] = net.name;
        return net.name;
    }

    const Gate& g = gates[gateId];

    // DFF.Q → 視為 pseudo PI，不穿越
    if (g.type == GateType::DFF) {
        memo[netId] = net.name;
        return net.name;
    }

    // 取得每個 input 的 expression
    std::vector<std::string> inputExprs;
    for (int inNetId : g.inputNetIds) {
        if (inNetId < 0)
            inputExprs.push_back("?");
        else
            inputExprs.push_back(buildExpr(inNetId, gates, nets, memo, depth + 1));
    }

    std::string result;

    switch (g.type) {
        case GateType::BUF:
            result = (inputExprs.size() >= 1) ? inputExprs[0] : "?";
            break;
        case GateType::NOT:
            result = (inputExprs.size() >= 1)
                ? "NOT(" + inputExprs[0] + ")"
                : "NOT(?)";
            break;
        case GateType::AND:
            if (inputExprs.size() == 2)
                result = "AND(" + inputExprs[0] + ", " + inputExprs[1] + ")";
            else if (inputExprs.size() == 1)
                result = inputExprs[0];
            else result = "AND(?)";
            break;
        case GateType::OR:
            if (inputExprs.size() == 2)
                result = "OR(" + inputExprs[0] + ", " + inputExprs[1] + ")";
            else result = "OR(?)";
            break;
        case GateType::NAND:
            if (inputExprs.size() == 2)
                result = "NAND(" + inputExprs[0] + ", " + inputExprs[1] + ")";
            else result = "NAND(?)";
            break;
        case GateType::NOR:
            if (inputExprs.size() == 2)
                result = "NOR(" + inputExprs[0] + ", " + inputExprs[1] + ")";
            else result = "NOR(?)";
            break;
        case GateType::XOR:
            if (inputExprs.size() == 2)
                result = "XOR(" + inputExprs[0] + ", " + inputExprs[1] + ")";
            else result = "XOR(?)";
            break;
        case GateType::XNOR:
            if (inputExprs.size() == 2)
                result = "XNOR(" + inputExprs[0] + ", " + inputExprs[1] + ")";
            else result = "XNOR(?)";
            break;
        default:
            result = net.name; // UNKNOWN 或未支援的 type
            break;
    }

    memo[netId] = result;
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// getBooleanExpression
// 回傳指定 net 的 Boolean expression 字串。
// 若 net 不存在回傳空字串。
// 若電路很大，expression 可能非常長；可用 getSimplifiedBooleanExpression
// 限制展開深度。
// ─────────────────────────────────────────────────────────────────────────────
std::string Netlist::getBooleanExpression(const std::string& netName) const {
    int netId = getNetId(netName);
    if (netId < 0) return "";

    std::unordered_map<int, std::string> memo;
    return buildExpr(netId, gates, nets, memo, 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// getBooleanExpressionOfNet（用 net ID 版本）
// ─────────────────────────────────────────────────────────────────────────────
std::string Netlist::getBooleanExpressionOfNet(int netId) const {
    if (netId < 0 || netId >= (int)nets.size()) return "";
    std::unordered_map<int, std::string> memo;
    return buildExpr(netId, gates, nets, memo, 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// getSimplifiedBooleanExpression
// 限制展開深度 maxDepth，超過的節點直接用 net name 替代。
// 避免大電路產生過長字串。
// ─────────────────────────────────────────────────────────────────────────────
static std::string buildExprLimited(
    int netId,
    const std::vector<Gate>& gates,
    const std::vector<Net>& nets,
    std::unordered_map<int, std::string>& memo,
    int depth,
    int maxDepth)
{
    if (netId < 0 || netId >= (int)nets.size()) return "?";

    auto it = memo.find(netId);
    if (it != memo.end()) return it->second;

    const Net& net = nets[netId];

    // 超過深度限制，直接用 net name
    if (depth >= maxDepth) {
        memo[netId] = net.name;
        return net.name;
    }

    if (net.isPI || net.isConst || net.driverGateId < 0) {
        memo[netId] = net.name;
        return net.name;
    }

    int gateId = net.driverGateId;
    if (gateId < 0 || gateId >= (int)gates.size()) {
        memo[netId] = net.name;
        return net.name;
    }

    const Gate& g = gates[gateId];
    if (g.type == GateType::DFF) {
        memo[netId] = net.name;
        return net.name;
    }

    std::vector<std::string> inputExprs;
    for (int inNetId : g.inputNetIds) {
        if (inNetId < 0)
            inputExprs.push_back("?");
        else
            inputExprs.push_back(
                buildExprLimited(inNetId, gates, nets, memo, depth + 1, maxDepth));
    }

    std::string result;
    switch (g.type) {
        case GateType::BUF:
            result = (inputExprs.size() >= 1) ? inputExprs[0] : "?"; break;
        case GateType::NOT:
            result = "NOT(" + (inputExprs.size() >= 1 ? inputExprs[0] : "?") + ")"; break;
        case GateType::AND:
            result = (inputExprs.size() == 2)
                ? "AND(" + inputExprs[0] + ", " + inputExprs[1] + ")" : "AND(?)"; break;
        case GateType::OR:
            result = (inputExprs.size() == 2)
                ? "OR(" + inputExprs[0] + ", " + inputExprs[1] + ")" : "OR(?)"; break;
        case GateType::NAND:
            result = (inputExprs.size() == 2)
                ? "NAND(" + inputExprs[0] + ", " + inputExprs[1] + ")" : "NAND(?)"; break;
        case GateType::NOR:
            result = (inputExprs.size() == 2)
                ? "NOR(" + inputExprs[0] + ", " + inputExprs[1] + ")" : "NOR(?)"; break;
        case GateType::XOR:
            result = (inputExprs.size() == 2)
                ? "XOR(" + inputExprs[0] + ", " + inputExprs[1] + ")" : "XOR(?)"; break;
        case GateType::XNOR:
            result = (inputExprs.size() == 2)
                ? "XNOR(" + inputExprs[0] + ", " + inputExprs[1] + ")" : "XNOR(?)"; break;
        default:
            result = net.name; break;
    }

    memo[netId] = result;
    return result;
}

std::string Netlist::getSimplifiedBooleanExpression(
    const std::string& netName, int maxDepth) const
{
    int netId = getNetId(netName);
    if (netId < 0) return "";
    std::unordered_map<int, std::string> memo;
    return buildExprLimited(netId, gates, nets, memo, 0, maxDepth);
}

// ─────────────────────────────────────────────────────────────────────────────
// getPrimaryInputsOfNet
// 回傳 netName 的 fanin cone 中所有 PI net 的名稱集合。
// 用於回答「n12 depends on which primary inputs」類問題。
// ─────────────────────────────────────────────────────────────────────────────
std::vector<std::string> Netlist::getPrimaryInputsOfNet(const std::string& netName) const {
    int netId = getNetId(netName);
    if (netId < 0) return {};

    std::unordered_set<int> visited;
    std::vector<std::string> piNames;

    // BFS 往回追 fanin
    std::queue<int> q;
    q.push(netId);
    visited.insert(netId);

    while (!q.empty()) {
        int nid = q.front(); q.pop();
        const Net& net = nets[nid];

        if (net.isPI) {
            piNames.push_back(net.name);
            continue;
        }

        if (net.isConst) continue;

        if (net.driverGateId < 0) {
            // 沒有 driver 但不是 PI，視為 leaf
            piNames.push_back(net.name);
            continue;
        }

        const Gate& g = gates[net.driverGateId];

        // DFF.Q 視為 pseudo PI
        if (g.type == GateType::DFF) {
            piNames.push_back(net.name);
            continue;
        }

        for (int inNetId : g.inputNetIds) {
            if (inNetId < 0) continue;
            if (visited.insert(inNetId).second)
                q.push(inNetId);
        }
    }

    // 排序去重
    std::sort(piNames.begin(), piNames.end());
    piNames.erase(std::unique(piNames.begin(), piNames.end()), piNames.end());
    return piNames;
}