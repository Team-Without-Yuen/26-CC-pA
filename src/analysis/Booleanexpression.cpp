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
#include <cstddef>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <sstream>
#include <vector>
#include <queue>
#include <algorithm>
#include <fstream>

namespace {

// buildExpr()/buildExprLimited() memoize by netId, so fanout sharing never
// causes a node to be *computed* twice. But the memoized value is a flat,
// no-sharing string, and every parent that reuses a shared net embeds its own
// copy of that net's already-expanded string. For a circuit with D levels of
// reconvergent fanout (e.g. AND(x, x) chained D times, or ordinary adder/mux
// carry structures), the final string length grows like O(2^D) even though
// each net is only *visited* once — memoization bounds the number of
// recursive calls, not the output size. Both a recursion-depth cap (existing,
// kFullExpressionDepthCap) and a total output-size budget (new,
// kExpressionSizeBudget) are needed so that a single BooleanExpression /
// SimplifiedBooleanExpression query can never hang or exhaust memory on a
// deeply reconvergent 1M-gate design. The budget is threaded through the
// whole call tree by reference so it is enforced globally, not per-branch.
constexpr int kFullExpressionDepthCap = 200;
constexpr std::size_t kExpressionSizeBudget = 2'000'000;

// Symbolic literal used for an algebraically-derived constant that has no
// corresponding net in the design (e.g. AND(x, NOT(x)) simplifies to a bare
// 0, not to whatever net id happens to hold the design's "1'b0"). This
// matches the string that a *real* constant net of the same value would
// already produce as a leaf (see addNet()'s "1'b0"/"1'b1" naming convention),
// so the output stays visually consistent either way.
const char* kSymbolicConst0 = "1'b0";
const char* kSymbolicConst1 = "1'b1";

// Local, purely-structural Boolean-identity simplification for 2-input gates.
// These are the same identities EditApply's SimplifyConstants /
// SimplifySameInput / CollapseDoubleInverter already apply at the netlist
// level (same-input, constant-input, complementary-input, double negation);
// applying them here too means getBooleanExpression() doesn't have to make
// the caller run EditApply first just to get a readable answer, and it never
// needs anything beyond looking at the two input nets' own flags and their
// immediate driver gate — no SAT, no global reasoning, so it can't be wrong
// about anything a full equivalence check would also be needed to prove.
enum class LocalRewrite { None, PassA, PassB, NotA, NotB, Const0, Const1 };

bool isConst0(int netId, const std::vector<Net>& nets) {
    return netId >= 0 && netId < (int)nets.size() &&
           nets[netId].isConst && nets[netId].constVal == 0;
}

bool isConst1(int netId, const std::vector<Net>& nets) {
    return netId >= 0 && netId < (int)nets.size() &&
           nets[netId].isConst && nets[netId].constVal == 1;
}

// True when net `x` is driven by NOT(net `y`), i.e. x == NOT(y) structurally.
bool isNotOf(int xNetId, int yNetId,
             const std::vector<Net>& nets, const std::vector<Gate>& gates) {
    if (xNetId < 0 || xNetId >= (int)nets.size()) return false;
    const Net& xNet = nets[xNetId];
    if (xNet.isPI || xNet.isConst) return false;
    const int driverId = xNet.driverGateId;
    if (driverId < 0 || driverId >= (int)gates.size()) return false;
    const Gate& driver = gates[driverId];
    return driver.type == GateType::NOT &&
           !driver.inputNetIds.empty() && driver.inputNetIds[0] == yNetId;
}

// If `gate` is NOT(NOT(x)), returns x's net id; otherwise returns -1.
// Cascades correctly through longer NOT chains because the grandchild net
// itself goes back through buildExpr()/buildExprLimited(), which applies the
// same rule again if it also starts with NOT(NOT(...)).
int doubleNegationGrandchild(const Gate& gate,
                              const std::vector<Net>& nets,
                              const std::vector<Gate>& gates) {
    if (gate.type != GateType::NOT || gate.inputNetIds.empty()) return -1;
    const int midNetId = gate.inputNetIds[0];
    if (midNetId < 0 || midNetId >= (int)nets.size()) return -1;
    const Net& midNet = nets[midNetId];
    if (midNet.isPI || midNet.isConst) return -1;
    const int driverId = midNet.driverGateId;
    if (driverId < 0 || driverId >= (int)gates.size()) return -1;
    const Gate& innerNot = gates[driverId];
    if (innerNot.type != GateType::NOT || innerNot.inputNetIds.empty()) return -1;
    return innerNot.inputNetIds[0];
}

LocalRewrite classifyTwoInputGate(GateType type, int aNetId, int bNetId,
                                   const std::vector<Net>& nets,
                                   const std::vector<Gate>& gates) {
    if (aNetId < 0 || bNetId < 0) return LocalRewrite::None;

    if (aNetId == bNetId) {
        switch (type) {
            case GateType::AND:  case GateType::OR:   return LocalRewrite::PassA;
            case GateType::NAND: case GateType::NOR:  return LocalRewrite::NotA;
            case GateType::XOR:  return LocalRewrite::Const0;
            case GateType::XNOR: return LocalRewrite::Const1;
            default: return LocalRewrite::None;
        }
    }

    if (isNotOf(aNetId, bNetId, nets, gates) || isNotOf(bNetId, aNetId, nets, gates)) {
        switch (type) {
            case GateType::AND:  case GateType::NOR:  return LocalRewrite::Const0;
            case GateType::OR:   case GateType::NAND: return LocalRewrite::Const1;
            case GateType::XOR:  return LocalRewrite::Const1;
            case GateType::XNOR: return LocalRewrite::Const0;
            default: return LocalRewrite::None;
        }
    }

    const bool aC0 = isConst0(aNetId, nets), aC1 = isConst1(aNetId, nets);
    const bool bC0 = isConst0(bNetId, nets), bC1 = isConst1(bNetId, nets);
    if (!aC0 && !aC1 && !bC0 && !bC1) return LocalRewrite::None;

    switch (type) {
        case GateType::AND:
            if (aC0 || bC0) return LocalRewrite::Const0;
            if (aC1) return LocalRewrite::PassB;
            if (bC1) return LocalRewrite::PassA;
            break;
        case GateType::OR:
            if (aC1 || bC1) return LocalRewrite::Const1;
            if (aC0) return LocalRewrite::PassB;
            if (bC0) return LocalRewrite::PassA;
            break;
        case GateType::NAND:
            if (aC0 || bC0) return LocalRewrite::Const1;
            if (aC1) return LocalRewrite::NotB;
            if (bC1) return LocalRewrite::NotA;
            break;
        case GateType::NOR:
            if (aC1 || bC1) return LocalRewrite::Const0;
            if (aC0) return LocalRewrite::NotB;
            if (bC0) return LocalRewrite::NotA;
            break;
        case GateType::XOR:
            if (aC0) return LocalRewrite::PassB;
            if (bC0) return LocalRewrite::PassA;
            if (aC1) return LocalRewrite::NotB;
            if (bC1) return LocalRewrite::NotA;
            break;
        case GateType::XNOR:
            if (aC1) return LocalRewrite::PassB;
            if (bC1) return LocalRewrite::PassA;
            if (aC0) return LocalRewrite::NotB;
            if (bC0) return LocalRewrite::NotA;
            break;
        default:
            break;
    }
    return LocalRewrite::None;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 內部 helper：遞迴展開 netId 對應的 Boolean expression
// memo：已算過的 netId → expression 字串，避免重複展開 fanout sharing
// depth：防止無限遞迴（理論上 DAG 不會，但防呆）
// sizeBudget：全域共用的輸出字串大小預算，防止 reconvergent fanout 造成
//             expression 字串指數爆炸；歸零後所有節點一律退化為 leaf。
// truncated：只要曾經因為 depth 或 sizeBudget 而放棄展開某個節點就設為 true，
//            讓呼叫端可以誠實回報「這不是完整展開」。
// ─────────────────────────────────────────────────────────────────────────────
static std::string buildExpr(
    int netId,
    const std::vector<Gate>& gates,
    const std::vector<Net>& nets,
    std::unordered_map<int, std::string>& memo,
    int depth,
    std::size_t& sizeBudget,
    bool& truncated)
{
    if (netId < 0 || netId >= (int)nets.size())
        return "?";

    // 已算過直接回傳
    auto it = memo.find(netId);
    if (it != memo.end())
        return it->second;

    // 防止過深展開，或是輸出預算已經用完（reconvergent fanout 防爆）
    if (depth > kFullExpressionDepthCap || sizeBudget == 0) {
        truncated = true;
        memo[netId] = nets[netId].name;
        return nets[netId].name;
    }

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

    // NOT(NOT(x)) → x：不需要展開兩層 NOT 的字串再丟掉，直接跳到孫節點。
    const int doubleNegChild = doubleNegationGrandchild(g, nets, gates);
    if (doubleNegChild >= 0) {
        std::string result = buildExpr(doubleNegChild, gates, nets, memo, depth + 1, sizeBudget, truncated);
        memo[netId] = result;
        return result;
    }

    // 2-input gate 的 local Boolean identity（same-input / complementary-input /
    // constant-input）：符合的話只需要展開用得到的那個 child，不需要兩個都展開
    // 再組字串又丟掉。
    LocalRewrite rewrite = LocalRewrite::None;
    if (g.inputNetIds.size() == 2) {
        rewrite = classifyTwoInputGate(
            g.type, g.inputNetIds[0], g.inputNetIds[1], nets, gates);
    }

    std::string result;
    bool resultReady = false;

    switch (rewrite) {
        case LocalRewrite::Const0:
            result = kSymbolicConst0;
            resultReady = true;
            break;
        case LocalRewrite::Const1:
            result = kSymbolicConst1;
            resultReady = true;
            break;
        case LocalRewrite::PassA:
            result = buildExpr(g.inputNetIds[0], gates, nets, memo, depth + 1, sizeBudget, truncated);
            resultReady = true;
            break;
        case LocalRewrite::PassB:
            result = buildExpr(g.inputNetIds[1], gates, nets, memo, depth + 1, sizeBudget, truncated);
            resultReady = true;
            break;
        case LocalRewrite::NotA:
            result = "NOT(" + buildExpr(g.inputNetIds[0], gates, nets, memo, depth + 1, sizeBudget, truncated) + ")";
            resultReady = true;
            break;
        case LocalRewrite::NotB:
            result = "NOT(" + buildExpr(g.inputNetIds[1], gates, nets, memo, depth + 1, sizeBudget, truncated) + ")";
            resultReady = true;
            break;
        case LocalRewrite::None:
            break;
    }

    if (!resultReady) {
        // 取得每個 input 的 expression
        std::vector<std::string> inputExprs;
        for (int inNetId : g.inputNetIds) {
            if (inNetId < 0)
                inputExprs.push_back("?");
            else
                inputExprs.push_back(buildExpr(inNetId, gates, nets, memo, depth + 1, sizeBudget, truncated));
        }

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
    }

    // 這顆節點自己的展開已經花掉多少預算就扣多少；若它本身已經超過剩餘預算，
    // 直接退化成 leaf，不把已經算出來的巨大字串繼續往上層傳遞。
    if (result.size() >= sizeBudget) {
        truncated = true;
        sizeBudget = 0;
        result = net.name;
    } else {
        sizeBudget -= result.size();
    }

    memo[netId] = result;
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// getBooleanExpression
// 回傳指定 net 的 Boolean expression 字串。
// 若 net 不存在回傳空字串。
// 若電路很大，expression 可能非常長；可用 getSimplifiedBooleanExpression
// 限制展開深度。若內部 depth cap 或 size budget 被觸發（見 buildExpr 註解），
// wasTruncated（若有提供）會被設為 true，呼叫端可據此判斷這不是完整展開。
// ─────────────────────────────────────────────────────────────────────────────
std::string Netlist::getBooleanExpression(const std::string& netName, bool* wasTruncated) const {
    int netId = getNetId(netName);
    if (netId < 0) {
        if (wasTruncated) *wasTruncated = false;
        return "";
    }

    std::unordered_map<int, std::string> memo;
    std::size_t sizeBudget = kExpressionSizeBudget;
    bool truncated = false;
    std::string expr = buildExpr(netId, gates, nets, memo, 0, sizeBudget, truncated);
    if (wasTruncated) *wasTruncated = truncated;
    return expr;
}

// ─────────────────────────────────────────────────────────────────────────────
// getBooleanExpressionOfNet（用 net ID 版本）
// ─────────────────────────────────────────────────────────────────────────────
std::string Netlist::getBooleanExpressionOfNet(int netId) const {
    if (netId < 0 || netId >= (int)nets.size()) return "";
    std::unordered_map<int, std::string> memo;
    std::size_t sizeBudget = kExpressionSizeBudget;
    bool truncated = false;
    return buildExpr(netId, gates, nets, memo, 0, sizeBudget, truncated);
}

// ─────────────────────────────────────────────────────────────────────────────
// getSimplifiedBooleanExpression
// 限制展開深度 maxDepth，超過的節點直接用 net name 替代。
// 避免大電路產生過長字串。maxDepth 本身沒有上限檢查，呼叫端可能傳入很大的值；
// 額外加上與 buildExpr 相同的 sizeBudget 安全網，避免 reconvergent fanout
// 電路即使在很大的 maxDepth 下也會讓字串長度指數爆炸。
// ─────────────────────────────────────────────────────────────────────────────
static std::string buildExprLimited(
    int netId,
    const std::vector<Gate>& gates,
    const std::vector<Net>& nets,
    std::unordered_map<int, std::string>& memo,
    int depth,
    int maxDepth,
    std::size_t& sizeBudget)
{
    if (netId < 0 || netId >= (int)nets.size()) return "?";

    auto it = memo.find(netId);
    if (it != memo.end()) return it->second;

    const Net& net = nets[netId];

    // 超過深度限制，或輸出預算已耗盡，直接用 net name
    if (depth >= maxDepth || sizeBudget == 0) {
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

    // NOT(NOT(x)) → x（與 buildExpr 相同的捷徑，見上方註解）
    const int doubleNegChild = doubleNegationGrandchild(g, nets, gates);
    if (doubleNegChild >= 0) {
        std::string result =
            buildExprLimited(doubleNegChild, gates, nets, memo, depth + 1, maxDepth, sizeBudget);
        memo[netId] = result;
        return result;
    }

    // same-input / complementary-input / constant-input 化簡（與 buildExpr 相同）
    LocalRewrite rewrite = LocalRewrite::None;
    if (g.inputNetIds.size() == 2) {
        rewrite = classifyTwoInputGate(
            g.type, g.inputNetIds[0], g.inputNetIds[1], nets, gates);
    }

    std::string result;
    bool resultReady = false;

    switch (rewrite) {
        case LocalRewrite::Const0:
            result = kSymbolicConst0;
            resultReady = true;
            break;
        case LocalRewrite::Const1:
            result = kSymbolicConst1;
            resultReady = true;
            break;
        case LocalRewrite::PassA:
            result = buildExprLimited(g.inputNetIds[0], gates, nets, memo, depth + 1, maxDepth, sizeBudget);
            resultReady = true;
            break;
        case LocalRewrite::PassB:
            result = buildExprLimited(g.inputNetIds[1], gates, nets, memo, depth + 1, maxDepth, sizeBudget);
            resultReady = true;
            break;
        case LocalRewrite::NotA:
            result = "NOT(" + buildExprLimited(g.inputNetIds[0], gates, nets, memo, depth + 1, maxDepth, sizeBudget) + ")";
            resultReady = true;
            break;
        case LocalRewrite::NotB:
            result = "NOT(" + buildExprLimited(g.inputNetIds[1], gates, nets, memo, depth + 1, maxDepth, sizeBudget) + ")";
            resultReady = true;
            break;
        case LocalRewrite::None:
            break;
    }

    if (!resultReady) {
        std::vector<std::string> inputExprs;
        for (int inNetId : g.inputNetIds) {
            if (inNetId < 0)
                inputExprs.push_back("?");
            else
                inputExprs.push_back(
                    buildExprLimited(inNetId, gates, nets, memo, depth + 1, maxDepth, sizeBudget));
        }

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
    }

    if (result.size() >= sizeBudget) {
        sizeBudget = 0;
        result = net.name;
    } else {
        sizeBudget -= result.size();
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
    std::size_t sizeBudget = kExpressionSizeBudget;
    return buildExprLimited(netId, gates, nets, memo, 0, maxDepth, sizeBudget);
}

// ─────────────────────────────────────────────────────────────────────────────
// getPrimaryInputSupportBreakdown
// 跟 getPrimaryInputsOfNet() 走同一次 fanin BFS，但把 leaf 分成三桶：
// 真正的 top-level primary input、DFF.Q pseudo primary input、
// 沒有 driver 也不是 PI 的懸空 leaf。三桶互斥、聯集等於 getPrimaryInputsOfNet()
// 的結果。
// ─────────────────────────────────────────────────────────────────────────────
Netlist::PrimaryInputSupport Netlist::getPrimaryInputSupportBreakdown(const std::string& netName) const {
    PrimaryInputSupport support;

    int netId = getNetId(netName);
    if (netId < 0) return support;

    std::unordered_set<int> visited;

    // BFS 往回追 fanin
    std::queue<int> q;
    q.push(netId);
    visited.insert(netId);

    while (!q.empty()) {
        int nid = q.front(); q.pop();
        const Net& net = nets[nid];

        if (net.isPI) {
            support.realPrimaryInputs.push_back(net.name);
            continue;
        }

        if (net.isConst) continue;

        if (net.driverGateId < 0) {
            // 沒有 driver 但不是 PI，視為懸空 leaf（跟真正的 PI 是不同的事）
            support.undrivenLeaves.push_back(net.name);
            continue;
        }

        const Gate& g = gates[net.driverGateId];

        // DFF.Q 視為 pseudo PI
        if (g.type == GateType::DFF) {
            support.dffPseudoInputs.push_back(net.name);
            DffStateBoundaryRecord boundary;
            boundary.netName = net.name;
            boundary.dffName = g.instName;
            boundary.netId = net.id;
            boundary.dffGateId = g.id;
            support.dffStateBoundaries.push_back(boundary);
            continue;
        }

        for (int inNetId : g.inputNetIds) {
            if (inNetId < 0) continue;
            if (visited.insert(inNetId).second)
                q.push(inNetId);
        }
    }

    auto sortUnique = [](std::vector<std::string>& names) {
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
    };
    sortUnique(support.realPrimaryInputs);
    sortUnique(support.dffPseudoInputs);
    sortUnique(support.undrivenLeaves);
    std::sort(support.dffStateBoundaries.begin(),
              support.dffStateBoundaries.end(),
              [](const DffStateBoundaryRecord& lhs,
                 const DffStateBoundaryRecord& rhs) {
                  if (lhs.netName != rhs.netName) return lhs.netName < rhs.netName;
                  return lhs.dffName < rhs.dffName;
              });
    support.dffStateBoundaries.erase(
        std::unique(support.dffStateBoundaries.begin(),
                    support.dffStateBoundaries.end(),
                    [](const DffStateBoundaryRecord& lhs,
                       const DffStateBoundaryRecord& rhs) {
                        return lhs.netId == rhs.netId;
                    }),
        support.dffStateBoundaries.end());
    for (size_t index = 0; index < support.dffStateBoundaries.size(); ++index) {
        support.dffStateBoundaries[index].stateVariableName =
            "state_q" + std::to_string(index);
    }

    support.all.reserve(support.realPrimaryInputs.size() +
                         support.dffPseudoInputs.size() +
                         support.undrivenLeaves.size());
    support.all.insert(support.all.end(), support.realPrimaryInputs.begin(), support.realPrimaryInputs.end());
    support.all.insert(support.all.end(), support.dffPseudoInputs.begin(), support.dffPseudoInputs.end());
    support.all.insert(support.all.end(), support.undrivenLeaves.begin(), support.undrivenLeaves.end());
    sortUnique(support.all);

    return support;
}

// ─────────────────────────────────────────────────────────────────────────────
// getPrimaryInputsOfNet
// 回傳 netName 的 fanin cone 中所有 PI net 的名稱集合。
// 用於回答「n12 depends on which primary inputs」類問題。
// 是 getPrimaryInputSupportBreakdown() 三桶的聯集；要分開來看請改用後者。
// ─────────────────────────────────────────────────────────────────────────────
std::vector<std::string> Netlist::getPrimaryInputsOfNet(const std::string& netName) const {
    return getPrimaryInputSupportBreakdown(netName).all;
}

Netlist::BooleanEquationArtifactResult Netlist::writeBooleanEquationArtifact(
    const std::string& netName,
    const std::string& outputFilePath,
    double timeLimitSeconds) const {
    BooleanEquationArtifactResult result;
    result.format = "NAMED_DAG_EQUATIONS_V2";
    result.outputFilePath = outputFilePath;

    const int rootNetId = getNetId(netName);
    if (rootNetId < 0 || !isValidNetId(rootNetId) || nets[rootNetId].isRemoved) {
        result.message = "Boolean equation target net was not found.";
        return result;
    }
    if (outputFilePath.empty()) {
        result.message = "Boolean equation artifact requires an output path.";
        return result;
    }

    request_time_budget::RequestDeadline deadline(timeLimitSeconds);
    if (!deadline.valid()) {
        result.message = "Boolean equation artifact requires a finite, positive time limit.";
        return result;
    }

    std::ofstream output(outputFilePath, std::ios::out | std::ios::trunc);
    if (!output.is_open()) {
        result.message = "Failed to open Boolean equation artifact: " + outputFilePath;
        return result;
    }
    result.fileCreated = true;

    output << "# Complete named-net Boolean DAG equation artifact\n"
           << "Format: " << result.format << "\n"
           << "Target: " << netName << "\n"
           << "Boundary semantics: PI, CONSTANT, DFF_Q, and UNDRIVEN are leaves\n\n";

    struct VisitFrame {
        int netId = -1;
        bool exiting = false;
    };

    std::vector<unsigned char> state(nets.size(), 0);
    std::vector<VisitFrame> stack;
    std::vector<int> equationNetIds;
    std::vector<int> boundaryNetIds;
    bool invalidStructure = false;
    bool unsupportedGate = false;
    bool cycleDetected = false;
    size_t workCounter = 0;

    stack.push_back({rootNetId, false});
    while (!stack.empty()) {
        if ((++workCounter & 1023u) == 0u && deadline.expired()) {
            result.timedOut = true;
            break;
        }

        const VisitFrame frame = stack.back();
        stack.pop_back();
        const int currentNetId = frame.netId;
        if (currentNetId < 0 || currentNetId >= static_cast<int>(nets.size()) ||
            nets[currentNetId].isRemoved) {
            invalidStructure = true;
            continue;
        }

        if (frame.exiting) {
            if (state[currentNetId] == 1) {
                state[currentNetId] = 2;
                equationNetIds.push_back(currentNetId);
            }
            continue;
        }
        if (state[currentNetId] == 2) continue;
        if (state[currentNetId] == 1) {
            cycleDetected = true;
            continue;
        }

        const Net& net = nets[currentNetId];
        const int driverId = net.driverGateId;
        const bool invalidDriver = driverId >= static_cast<int>(gates.size());
        const bool boundary = net.isPI || net.isConst || driverId < 0 ||
            invalidDriver || isGateRemoved(driverId) ||
            (driverId >= 0 && gates[driverId].type == GateType::DFF);
        if (boundary) {
            state[currentNetId] = 2;
            boundaryNetIds.push_back(currentNetId);
            if (invalidDriver) invalidStructure = true;
            continue;
        }

        const Gate& gate = gates[driverId];
        if (gate.type == GateType::UNKNOWN) unsupportedGate = true;
        state[currentNetId] = 1;
        stack.push_back({currentNetId, true});
        for (auto input = gate.inputNetIds.rbegin();
             input != gate.inputNetIds.rend(); ++input) {
            if (*input < 0 || *input >= static_cast<int>(nets.size()) ||
                nets[*input].isRemoved) {
                invalidStructure = true;
                continue;
            }
            if (state[*input] == 1) {
                cycleDetected = true;
                continue;
            }
            if (state[*input] == 0) stack.push_back({*input, false});
        }
    }
    if (deadline.expired()) result.timedOut = true;

    std::sort(boundaryNetIds.begin(), boundaryNetIds.end(),
              [&](int lhs, int rhs) { return nets[lhs].name < nets[rhs].name; });
    boundaryNetIds.erase(
        std::unique(boundaryNetIds.begin(), boundaryNetIds.end()),
        boundaryNetIds.end());

    std::unordered_map<int, std::string> stateVariableByNetId;
    size_t stateVariableIndex = 0;
    for (int boundaryNetId : boundaryNetIds) {
        const Net& boundaryNet = nets[boundaryNetId];
        const int driverId = boundaryNet.driverGateId;
        if (driverId >= 0 && driverId < static_cast<int>(gates.size()) &&
            !isGateRemoved(driverId) && gates[driverId].type == GateType::DFF) {
            stateVariableByNetId.emplace(
                boundaryNetId, "state_q" + std::to_string(stateVariableIndex++));
        }
    }

    output << "Boundaries:\n";
    for (int boundaryNetId : boundaryNetIds) {
        if (result.timedOut) break;
        if ((++workCounter & 1023u) == 0u && deadline.expired()) {
            result.timedOut = true;
            break;
        }
        const Net& net = nets[boundaryNetId];
        std::string kind = "UNDRIVEN";
        if (net.isConst) {
            kind = "CONSTANT";
        } else if (net.isPI) {
            kind = "PI";
        } else if (net.driverGateId >= 0 &&
                   net.driverGateId < static_cast<int>(gates.size()) &&
                   !isGateRemoved(net.driverGateId) &&
                   gates[net.driverGateId].type == GateType::DFF) {
            kind = "DFF_Q";
        }
        output << "  " << net.name << " : " << kind;
        const auto stateVariable = stateVariableByNetId.find(boundaryNetId);
        if (stateVariable != stateVariableByNetId.end()) {
            const Gate& dff = gates[net.driverGateId];
            output << " (state_variable=" << stateVariable->second
                   << ", source=" << dff.instName << ".Q)";
        }
        output << "\n";
        ++result.boundaryCount;
    }

    output << "\nCurrent-state variables:\n";
    for (int boundaryNetId : boundaryNetIds) {
        const auto stateVariable = stateVariableByNetId.find(boundaryNetId);
        if (stateVariable == stateVariableByNetId.end()) continue;
        const Net& net = nets[boundaryNetId];
        const Gate& dff = gates[net.driverGateId];
        output << "  " << stateVariable->second << " = " << dff.instName
               << ".Q (net " << net.name << ")\n";
    }

    output << "\nEquations:\n";
    if (!result.timedOut) {
        for (int equationNetId : equationNetIds) {
            if ((++workCounter & 1023u) == 0u && deadline.expired()) {
                result.timedOut = true;
                break;
            }
            const Net& net = nets[equationNetId];
            const int driverId = net.driverGateId;
            if (driverId < 0 || driverId >= static_cast<int>(gates.size()) ||
                isGateRemoved(driverId)) {
                invalidStructure = true;
                continue;
            }
            const Gate& gate = gates[driverId];
            output << "  " << net.name << " = " << gateTypeToString(gate.type)
                   << "(";
            for (size_t inputIndex = 0;
                 inputIndex < gate.inputNetIds.size(); ++inputIndex) {
                if (inputIndex != 0) output << ", ";
                const int inputNetId = gate.inputNetIds[inputIndex];
                if (inputNetId < 0 || inputNetId >= static_cast<int>(nets.size()) ||
                    nets[inputNetId].isRemoved) {
                    output << "?";
                    invalidStructure = true;
                } else {
                    const auto stateVariable = stateVariableByNetId.find(inputNetId);
                    output << (stateVariable != stateVariableByNetId.end()
                        ? stateVariable->second
                        : nets[inputNetId].name);
                }
            }
            output << ")\n";
            ++result.equationCount;
            if (!output.good()) break;
        }
    }
    if (deadline.expired()) result.timedOut = true;

    const bool bodyIoFailed = !output.good();
    const bool logicallyComplete = !result.timedOut && !invalidStructure &&
        !unsupportedGate && !cycleDetected && !bodyIoFailed &&
        result.equationCount == equationNetIds.size();
    const auto rootStateVariable = stateVariableByNetId.find(rootNetId);
    output << "\nRoot: " << netName;
    if (rootStateVariable != stateVariableByNetId.end()) {
        output << " (current-state variable " << rootStateVariable->second << ")";
    }
    output << "\n"
           << "Equation count: " << result.equationCount << "\n"
           << "Boundary count: " << result.boundaryCount << "\n"
           << "Complete: " << (logicallyComplete ? "yes" : "no") << "\n";
    if (result.timedOut) output << "Reason: request deadline exceeded\n";
    else if (cycleDetected) output << "Reason: combinational cycle detected\n";
    else if (unsupportedGate) output << "Reason: unsupported gate type\n";
    else if (invalidStructure) output << "Reason: invalid or removed graph reference\n";
    else if (bodyIoFailed) output << "Reason: output I/O failure\n";
    output.flush();
    const bool finalIoFailed = !output.good();
    output.close();

    result.complete = logicallyComplete && !finalIoFailed;
    result.ok = result.complete;
    if (result.complete) {
        result.message = "Complete named-net Boolean DAG equations were written.";
    } else if (result.timedOut) {
        result.message = "Boolean equation artifact was incomplete because the request deadline expired.";
    } else if (bodyIoFailed || finalIoFailed) {
        result.message = "Boolean equation artifact could not be written completely.";
    } else {
        result.message = "Boolean equation artifact was incomplete because the target cone was invalid or unsupported.";
    }
    return result;
}
