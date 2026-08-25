#include "include/core/Netlist.h"
#include "include/SATEngine/SatTime.h"
#include "include/SATEngine/Primitives.h"
#include "include/core/FunctionalPatternEngine.h"
#include "include/core/BitParallelSimulation.h"
#include <string>
#include <algorithm>
#include <vector>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <iostream>

namespace {

// b 是否為 a 的結構反相：b 由一顆 NOT 驅動，且該 NOT 的輸入就是 a。
// 只做結構判定，不做 SAT——功能等價的反相關係由 functional merge 負責。
bool isStructuralComplement(const Netlist& netlist, int aNetId, int bNetId) {
    if (!netlist.isValidNetId(aNetId) || !netlist.isValidNetId(bNetId)) return false;
    if (aNetId == bNetId) return false;

    const int driverId = netlist.getNet(bNetId).driverGateId;
    if (!netlist.isValidGateId(driverId) || netlist.isGateRemoved(driverId)) return false;

    const Gate& driver = netlist.getGate(driverId);
    if (driver.type != GateType::NOT) return false;
    if (driver.inputNetIds.size() != 1) return false;
    return driver.inputNetIds[0] == aNetId;
}

// 兩條 net 是否互為反相（任一方向）。
bool areComplementary(const Netlist& netlist, int x, int y) {
    return isStructuralComplement(netlist, x, y) ||
           isStructuralComplement(netlist, y, x);
}

// 取得（必要時建立）值為 value 的常數 net。
// parser 把 Verilog 字面值直接當 net 名，因此常數 net 一律用正規名字。
int ensureConstantNet(Netlist& netlist, int value) {
    const std::string name = (value == 1) ? "1'b1" : "1'b0";
    const int existing = netlist.getNetId(name);
    
    if (netlist.isValidNetId(existing) && !netlist.getNet(existing).isRemoved) {
        return existing;
    }
    
    return netlist.addNet(name); 
}

// DFF 的比對 key：四個具名 pin 各自接的 net。
// 不能像 combinational gate 那樣排序 —— pin 是具名的，
// DFF(.D(a),.CK(b)) 與 DFF(.D(b),.CK(a)) 是完全不同的電路。
// 而且 parser 依 Verilog 宣告順序存 pin，兩顆同功能的 DFF 宣告順序可能不同，
// 所以要按 pin name 正規化，不是按 index 比。
struct DffPinKey {
    int d  = -1;
    int ck = -1;
    int rn = -1;
    int sn = -1;
    bool hasUnknownPin = false;   // 出現 D/CK/RN/SN 以外的 pin

    bool operator==(const DffPinKey& other) const {
        return !hasUnknownPin && !other.hasUnknownPin &&
               d == other.d && ck == other.ck &&
               rn == other.rn && sn == other.sn;
    }
};

struct DffPinKeyHash {
    std::size_t operator()(const DffPinKey& key) const noexcept {
        std::size_t seed = std::hash<int>{}(key.d);
        auto mix = [&seed](int value) {
            seed ^= std::hash<int>{}(value) + 0x9e3779b97f4a7c15ull +
                    (seed << 6) + (seed >> 2);
        };
        mix(key.ck);
        mix(key.rn);
        mix(key.sn);
        return seed;
    }
};

DffPinKey makeDffPinKey(const Netlist& netlist, int gateId) {
    DffPinKey key;
    if (!netlist.isValidGateId(gateId)) return key;
    const Gate& gate = netlist.getGate(gateId);
    if (gate.type != GateType::DFF) return key;

    for (size_t i = 0; i < gate.inputNetIds.size(); ++i) {
        const int netId = gate.inputNetIds[i];
        if (netId < 0 || !netlist.isValidNetId(netId)) continue;
        if (netlist.getNet(netId).isRemoved) continue;
        const std::string& pin =
            i < gate.inputPinNames.size() ? gate.inputPinNames[i] : std::string();
        if (pin == "D")       key.d  = netId;
        else if (pin == "CK") key.ck = netId;
        else if (pin == "RN") key.rn = netId;
        else if (pin == "SN") key.sn = netId;
        else return DffPinKey{};   // 未知 pin：回空 key，之後不參與合併
    }
    return key;
}

// 合併候選的基本安全條件。
bool isSafeDffMergeCandidate(const Netlist& netlist, int gateId) {
    if (!netlist.isValidGateId(gateId) || netlist.isGateRemoved(gateId)) return false;
    const Gate& gate = netlist.getGate(gateId);
    if (gate.type != GateType::DFF || gate.id != gateId) return false;
    if (!netlist.isValidNetId(gate.outputNetId)) return false;

    const Net& output = netlist.getNet(gate.outputNetId);
    if (output.isRemoved || output.isPI || output.isConst ||
        output.driverGateId != gateId) {
        return false;
    }
    return true;
}

bool outputDependsOnNet(const Netlist& netlist, int outputNetId, int ancestorNetId) {
    if (!netlist.isValidNetId(outputNetId) || !netlist.isValidNetId(ancestorNetId)) {
        return false;
    }

    std::queue<int> pending;
    std::unordered_set<int> visited;
    pending.push(outputNetId);
    visited.insert(outputNetId);

    while (!pending.empty()) {
        const int netId = pending.front();
        pending.pop();
        if (netId == ancestorNetId) {
            return true;
        }

        const Net& net = netlist.getNet(netId);
        if (!netlist.isValidGateId(net.driverGateId)) {
            continue;
        }
        const Gate& driver = netlist.getGate(net.driverGateId);
        if (driver.type == GateType::DFF || driver.type == GateType::UNKNOWN) {
            continue;
        }
        for (int inputNetId : driver.inputNetIds) {
            if (netlist.isValidNetId(inputNetId) && visited.insert(inputNetId).second) {
                pending.push(inputNetId);
            }
        }
    }
    return false;
}

void rebuildNetLoadGateIds(Netlist& netlist) {
    for (size_t netIndex = 0; netIndex < netlist.getNetCount(); ++netIndex) {
        netlist.getNetMutable(static_cast<int>(netIndex)).loadGateIds.clear();
    }
    for (size_t gateIndex = 0; gateIndex < netlist.getGateCount(); ++gateIndex) {
        const int gateId = static_cast<int>(gateIndex);
        const Gate& gate = netlist.getGate(gateId);
        if (gate.type == GateType::UNKNOWN) {
            continue;
        }
        for (int inputNetId : gate.inputNetIds) {
            if (!netlist.isValidNetId(inputNetId) ||
                netlist.getNet(inputNetId).isRemoved) {
                continue;
            }
            netlist.getNetMutable(inputNetId).loadGateIds.push_back(gateId);
        }
    }
}

bool isSafeDoubleInverterPair(const Netlist& netlist, int g1id, int g2id) {
    if (!netlist.isValidGateId(g1id) || !netlist.isValidGateId(g2id) ||
        g1id == g2id) {
        return false;
    }

    const Gate& g1 = netlist.getGate(g1id);
    const Gate& g2 = netlist.getGate(g2id);
    if (g1.id != g1id || g2.id != g2id ||
        g1.type != GateType::NOT || g2.type != GateType::NOT ||
        g1.inputNetIds.size() != 1 || g2.inputNetIds.size() != 1) {
        return false;
    }

    const int inNetId = g1.inputNetIds[0];
    const int midNetId = g1.outputNetId;
    const int outNetId = g2.outputNetId;
    if (!netlist.isValidNetId(inNetId) ||
        !netlist.isValidNetId(midNetId) ||
        !netlist.isValidNetId(outNetId) ||
        inNetId == midNetId || midNetId == outNetId || inNetId == outNetId) {
        return false;
    }

    const Net& inNet = netlist.getNet(inNetId);
    const Net& midNet = netlist.getNet(midNetId);
    const Net& outNet = netlist.getNet(outNetId);
    if (inNet.isRemoved || midNet.isRemoved || outNet.isRemoved ||
        midNet.isPI || midNet.isPO || midNet.isConst ||
        outNet.isPI || outNet.isPO || outNet.isConst) {
        return false;
    }
    if (midNet.driverGateId != g1id || outNet.driverGateId != g2id ||
        g2.inputNetIds[0] != midNetId ||
        midNet.loadGateIds.size() != 1 || midNet.loadGateIds[0] != g2id ||
        std::count(inNet.loadGateIds.begin(), inNet.loadGateIds.end(), g1id) != 1) {
        return false;
    }

    std::unordered_map<int, size_t> listedLoadCounts;
    for (int loadGateId : outNet.loadGateIds) {
        if (!netlist.isValidGateId(loadGateId) ||
            netlist.getGate(loadGateId).type == GateType::UNKNOWN) {
            return false;
        }
        ++listedLoadCounts[loadGateId];
    }
    for (const auto& item : listedLoadCounts) {
        const Gate& loadGate = netlist.getGate(item.first);
        const size_t pinCount = static_cast<size_t>(std::count(
            loadGate.inputNetIds.begin(), loadGate.inputNetIds.end(), outNetId));
        if (pinCount != item.second) {
            return false;
        }
    }

    return true;
}

bool isSafeStructuralMergeCandidate(const Netlist& netlist, int gateId) {
    if (!netlist.isValidGateId(gateId)) return false;
    const Gate& gate = netlist.getGate(gateId);
    if (gate.type == GateType::UNKNOWN || gate.type == GateType::DFF ||
        gate.id != gateId || !netlist.isValidNetId(gate.outputNetId)) {
        return false;
    }

    const Net& output = netlist.getNet(gate.outputNetId);
    if (output.isRemoved || output.isPI || output.isConst ||
        output.driverGateId != gateId) {
        return false;
    }
    for (int inputNetId : gate.inputNetIds) {
        if (!netlist.isValidNetId(inputNetId) ||
            netlist.getNet(inputNetId).isRemoved ||
            inputNetId == gate.outputNetId) {
            return false;
        }
    }
    return true;
}

bool collectActiveStructuralLoads(
    const Netlist& netlist,
    int outputNetId,
    std::vector<int>& affectedGateIds)
{
    affectedGateIds.clear();
    if (!netlist.isValidNetId(outputNetId) ||
        netlist.getNet(outputNetId).isRemoved) {
        return false;
    }

    std::unordered_map<int, size_t> listedLoadCounts;
    for (int loadGateId : netlist.getNet(outputNetId).loadGateIds) {
        if (!netlist.isValidGateId(loadGateId)) return false;
        if (netlist.getGate(loadGateId).type == GateType::UNKNOWN) continue;
        ++listedLoadCounts[loadGateId];
    }
    affectedGateIds.reserve(listedLoadCounts.size());
    for (const auto& item : listedLoadCounts) {
        const Gate& loadGate = netlist.getGate(item.first);
        const size_t pinCount = static_cast<size_t>(std::count(
            loadGate.inputNetIds.begin(), loadGate.inputNetIds.end(), outputNetId));
        if (pinCount != item.second) return false;
        affectedGateIds.push_back(item.first);
    }
    return true;
}

void eraseOneLoad(std::vector<int>& loads, int gateId) {
    auto it = std::find(loads.begin(), loads.end(), gateId);
    if (it != loads.end()) loads.erase(it);
}

NetlistEditReport finalizeEditReport(
    Netlist& netlist,
    const Netlist& before,
    int changedCount,
    const std::string& operationName,
    NetlistEditOperationKind kind,
    const std::string& successMessage,
    const std::string& rollbackMessage,
    EquivalenceCheckMethod equivalenceMethod = EquivalenceCheckMethod::NotChecked,
    const std::string& equivalenceMessage = "",
    bool computeDepthChange = true)
{
    NetlistEditReport report = Netlist::buildEditReport(before, netlist, operationName, kind);
    report.changed = report.changed || changedCount > 0;
    if (computeDepthChange) {
        report.costChange = Netlist::buildDepthCostChange(before, netlist);
    }
    report.message = report.success ? successMessage : rollbackMessage;

    if (report.success && equivalenceMethod != EquivalenceCheckMethod::NotChecked) {
        Netlist::certifyEquivalence(report, equivalenceMethod, equivalenceMessage);
    }

    if (!report.success) {
        netlist.restoreFrom(before);
        report.rolledBack = true;
    }

    return report;
}

NetlistEditReport finalizeBooleanPrimitiveReport(
    Netlist& netlist,
    const Netlist& before,
    bool operationSucceeded,
    const std::string& operationName,
    const std::string& successMessage,
    const std::string& failureMessage,
    const std::vector<std::string>& changedGateNames = {},
    const std::vector<std::string>& changedNetNames = {},
    const std::vector<int>& changedGateIds = {},
    const std::vector<int>& changedNetIds = {})
{
    NetlistEditReport report = Netlist::buildEditReport(
        before,
        netlist,
        operationName,
        NetlistEditOperationKind::PrimitiveMutation);

    report.changed = report.changed || operationSucceeded;
    report.changedGateNames = changedGateNames;
    report.changedNetNames = changedNetNames;
    report.changedGateIds = changedGateIds;
    report.changedNetIds = changedNetIds;

    if (!operationSucceeded) {
        netlist.restoreFrom(before);
        report.rolledBack = true;
        report.success = false;
        report.message = failureMessage;
        return report;
    }

    report.message = report.success
        ? successMessage
        : operationName + " failed validation and was rolled back.";

    if (!report.success) {
        netlist.restoreFrom(before);
        report.rolledBack = true;
    }

    return report;
}

}

// ─────────────────────────────────────────────────────────────────────────────
//  trimDeadLogic  【BUG FIX: 補上 DFF 作為 timing endpoint】
//  移除所有不影響任何 PO 或 DFF input 的 gate 和 net
//  回傳移除的 gate 數量
// ─────────────────────────────────────────────────────────────────────────────
int Netlist::trimDeadLogic() {
    const int NG = (int)gates.size();
    const int NN = (int)nets.size();

    std::unordered_set<int> usefulGates;
    std::unordered_set<int> usefulNets;
    std::queue<int> q;

    auto pushNet = [&](int nid) {
        if (nid >= 0 && nid < NN && usefulNets.insert(nid).second) q.push(nid);
    };

    // --- 起點 1：所有真實 PO net ---
    for (int i = 0; i < NN; ++i)
        if (nets[i].isPO) pushNet(i);

    // --- 起點 2：所有 DFF 的 D 輸入（時序終點，必須保留）---
    //     DFF 本身無條件視為有用；其控制腳(CK/RN/SN)與 D 都要保住上游。
    for (int i = 0; i < NG; ++i) {
        if (gates[i].type != GateType::DFF) continue;
        usefulGates.insert(i);
        for (int inNetId : gates[i].inputNetIds) pushNet(inNetId);
    }

    // 沒有任何 PO 或 DFF input endpoint 時，不猜測可觀察端點。
    if (q.empty()) {
        std::cerr << "[trimDeadLogic][WARN] no PO or DFF input endpoint found; "
                     "skipping trim to stay safe.\n";
        return 0;
    }

    // --- 反向 BFS：標記所有對「PO / DFF.D」有貢獻的組合閘 ---
    while (!q.empty()) {
        int currNet = q.front(); q.pop();

        int dgid = nets[currNet].driverGateId;
        if (dgid < 0 || dgid >= NG) continue;              // PI / 常數：無上游
        if (gates[dgid].type == GateType::UNKNOWN) continue;
        if (gates[dgid].type == GateType::DFF) continue;   // DFF 已在起點處理，別穿透
        if (!usefulGates.insert(dgid).second) continue;    // 已標記過

        for (int inNetId : gates[dgid].inputNetIds) pushNet(inNetId);
    }

    // --- 刪除：未被標記的組合閘 → UNKNOWN，並同步斷線 ---
    int deadCount = 0;
    for (int i = 0; i < NG; ++i) {
        Gate& g = gates[i];
        if (g.type == GateType::UNKNOWN || g.type == GateType::DFF) continue;
        if (usefulGates.count(i)) continue;                // 有用，保留

        // 冪等的斷線：從每個 fanin net 的 loadGateIds 移除自己
        for (int inId : g.inputNetIds) {
            if (inId < 0 || inId >= NN) continue;
            auto& L = nets[inId].loadGateIds;
            L.erase(std::remove(L.begin(), L.end(), i), L.end());
        }
        // 清掉自己 output net 的 driver（該 net 就此變懸空 / 待回收）
        if (g.outputNetId >= 0 && g.outputNetId < NN)
            nets[g.outputNetId].driverGateId = -1;

        g.type = GateType::UNKNOWN;                         // 統一死活判定旗標
        // g.inputNetIds.clear(); g.outputNetId = -1;       // 視需要可清空
        ++deadCount;
    }

    if (deadCount > 0) markDirty();
    return deadCount;
}

/*NetlistEditReport Netlist::trimDeadLogicWithReport() {
    Netlist before = cloneForRollback();
    int removed = trimDeadLogic();
    return finalizeEditReport(
        *this,
        before,
        removed,
        "trimDeadLogic",
        NetlistEditOperationKind::Cleanup,
        "Dead logic trimming completed.",
        "Dead logic trimming failed validation and was rolled back.",
        EquivalenceCheckMethod::StructuralIdentity,
        "Equivalence certified by removing logic that is unreachable from PO and DFF input endpoints.",
        false);
}*/

// 取代 trimDeadLogic() 和 removeDanglingLogic()。
DeadLogicSummary Netlist::removeDeadLogic(const DeadLogicOptions& options) {
    DeadLogicSummary summary;
    summary.includeSequential = options.includeSequential;

    const int NG = static_cast<int>(gates.size());
    const int NN = static_cast<int>(nets.size());

    // id 是連續的 vector 索引，用 bitmap 取代 unordered_set。
    std::vector<uint8_t> netUseful(NN, 0);
    std::vector<uint8_t> gateUseful(NG, 0);

    // reachability 不在乎走訪順序，用 vector 當 stack 比 queue<int>（deque）快。
    std::vector<int> work;
    work.reserve(static_cast<size_t>(NN) / 4 + 16);

    auto pushNet = [&](int netId) {
        if (netId < 0 || netId >= NN) return;
        if (nets[netId].isRemoved) return;
        if (netUseful[netId]) return;
        netUseful[netId] = 1;
        work.push_back(netId);
    };

    // ---- 起點 1：所有 PO net ----
    for (int i = 0; i < NN; ++i) {
        if (!nets[i].isRemoved && nets[i].isPO) pushNet(i);
    }

    // ---- 起點 2：DFF（僅 combinational-only 模式）----
    // includeSequential 時不預先 seed，改由走訪經 Q net 到達；
    // Q 沒人用的 DFF 因此不會被標記為 useful，自然被回收。
    // Q net 若是 PO，起點 1 已經 seed 過，DFF 一定會被保留。
    if (!options.includeSequential) {
        for (int i = 0; i < NG; ++i) {
            if (isGateRemoved(i)) continue;
            if (gates[i].type != GateType::DFF) continue;
            gateUseful[i] = 1;
            for (int inNetId : gates[i].inputNetIds) pushNet(inNetId);
        }
    }

    // 沒有任何 PO 或 DFF endpoint：不猜測 observable 邊界，保守不動。
    if (work.empty()) {
        return summary;
    }

    // ---- 反向可達性走訪 ----
    size_t sinceDeadlineCheck = 0;
    while (!work.empty()) {
        if (options.deadline && ++sinceDeadlineCheck >= 4096) {
            sinceDeadlineCheck = 0;
            if (options.deadline->expired()) {
                // 尚未 mutate，直接退出即為安全狀態，不需要 rollback。
                summary.timedOut = true;
                return summary;
            }
        }

        const int netId = work.back();
        work.pop_back();

        const int driverId = nets[netId].driverGateId;
        if (driverId < 0 || driverId >= NG) continue;   // PI / const / undriven
        if (isGateRemoved(driverId)) continue;

        const GateType type = gates[driverId].type;
        // combinational-only 模式：DFF 已在起點處理，不穿透（避免把上一級的
        // fanin cone 誤判為當前 cone 的一部分）。
        if (type == GateType::DFF && !options.includeSequential) continue;

        if (gateUseful[driverId]) continue;
        gateUseful[driverId] = 1;

        for (int inNetId : gates[driverId].inputNetIds) pushNet(inNetId);
    }

    // ---- GC：gate ----
    for (int i = 0; i < NG; ++i) {
        if (isGateRemoved(i)) continue;
        if (gateUseful[i]) continue;

        // 來源檔案的 multi-driver：這顆 gate 的 output net 由別人驅動，
        // 反向走訪永遠到不了它，會被誤判為 dead。移除會改變寫回的內容
        // （原本兩顆 driver 變成一顆），因此保守保留。
        const int outNet = gates[i].outputNetId;
        if (isValidNetId(outNet) && nets[outNet].driverGateId != i) {
            continue;
        }

        const bool isDff = gates[i].type == GateType::DFF;
        if (markGateRemoved(i)) {
            ++summary.removedGateCount;
            if (isDff) ++summary.removedDffCount;
        }
    }

    // ---- GC：net ----
    // 死 gate 已在上一步從各 net 的 loadGateIds 移除，且其 output net 的
    // driverGateId 已清為 -1，因此這裡 removeNetIfUnused 的 driver guard 會放行。
    for (int i = 0; i < NN; ++i) {
        if (nets[i].isRemoved) continue;
        if (netUseful[i]) continue;
        if (nets[i].isPI || nets[i].isPO || nets[i].isConst) continue;

        if (removeNetIfUnused(i)) ++summary.removedNetCount;
    }

    if (summary.removedGateCount > 0 || summary.removedNetCount > 0) markDirty();
    return summary;
}

NetlistEditReport Netlist::removeDeadLogicWithReport(const DeadLogicOptions& options) {
    Netlist before = cloneForRollback();
    const DeadLogicSummary summary = removeDeadLogic(options);
    const int changedCount =
        static_cast<int>(summary.removedGateCount + summary.removedNetCount);

    NetlistEditReport report = finalizeEditReport(
        *this,
        before,
        changedCount,
        "removeDeadLogic",
        NetlistEditOperationKind::Cleanup,
        summary.timedOut
            ? "Dead logic removal hit the time limit during reachability marking; "
              "no change was applied."
            : "Dead logic removal completed.",
        "Dead logic removal failed validation and was rolled back.",
        EquivalenceCheckMethod::CertifiedRewrite,
        options.includeSequential
            ? "Equivalence certified by removing logic that is unreachable from any "
              "primary output, including registers whose Q is unobservable."
            : "Equivalence certified by removing logic that is unreachable from "
              "primary output and DFF input endpoints.");

    report.deadLogic = summary;

    if (summary.timedOut) {
        report.addWarning(
            "Reachability marking did not finish within the time limit; the design is "
            "unchanged. Answer with the timeout status, not with a removal count.");
    }
    if (report.success && changedCount == 0 && !summary.timedOut) {
        report.addWarning("No dead logic was found.");
    }
    return report;
}

// ─────────────────────────────────────────────────────────────────────────────
//  collapseBackToBackInverters  【BUG FIX: 補上 PO guard】
//  找出所有 NOT → NOT 的連續對，把兩個 NOT 刪掉，直接把前面的 net 接到後面
//  例如：A → NOT1 → B → NOT2 → C  →  A → C
//  回傳移除的 inverter pair 數量
//
//  用 worklist 取代「每 collapse 一對就整個重掃全部 gate」的作法：後者對 K 個
//  可 collapse 的 pair 要付 O(gates 數 * K)，在 SafeCleanupFixpoint /
//  LocalSimplificationFixpoint 的巢狀 fixpoint 迴圈裡會被重複放大，對大電路
//  是明確的 timeout 風險。一次 collapse 唯一可能新增的候選，是 inNet 的
//  driver（若也是 NOT）：collapse 後 inNet 的 loadGateIds 少了 g1、多了原本
//  outNet 的 loads，driver 的 fanout 形狀因此改變，才需要重新檢查。
// ─────────────────────────────────────────────────────────────────────────────
int Netlist::collapseBackToBackInverters() {
    int collapsed = 0;

    std::queue<int> worklist;
    std::unordered_set<int> queued;
    auto enqueue = [&](int gateId) {
        if (gateId < 0 || gateId >= (int)gates.size()) return;
        if (gates[gateId].type != GateType::NOT) return;
        if (queued.insert(gateId).second) worklist.push(gateId);
    };

    for (int i = 0; i < (int)gates.size(); i++) enqueue(i);

    while (!worklist.empty()) {
        const int i = worklist.front();
        worklist.pop();
        queued.erase(i);

        if (gates[i].type != GateType::NOT ||
            !isValidNetId(gates[i].outputNetId)) {
            continue;
        }
        const Net& midNet = nets[gates[i].outputNetId];
        if (midNet.loadGateIds.size() != 1) continue;

        const int g2id = midNet.loadGateIds[0];
        if (!isSafeDoubleInverterPair(*this, i, g2id)) continue;

        const int inNetId = gates[i].inputNetIds[0];
        const int inNetDriverGateId = nets[inNetId].driverGateId;
        if (!bypassDoubleInverter(i, g2id)) continue;

        ++collapsed;

        // inNet's load list just changed shape; if its driver is itself a NOT
        // gate it may now newly qualify as the next g1 (e.g. a NOT chain
        // collapsing inward one pair at a time).
        enqueue(inNetDriverGateId);
    }
    if (collapsed > 0) markDirty();
    return collapsed;
}

NetlistEditReport Netlist::collapseBackToBackInvertersWithReport() {
    Netlist before = cloneForRollback();
    int collapsed = collapseBackToBackInverters();
    return finalizeEditReport(
        *this,
        before,
        collapsed,
        "collapseBackToBackInverters",
        NetlistEditOperationKind::Simplification,
        "Back-to-back inverter collapse completed.",
        "Back-to-back inverter collapse failed validation and was rolled back.",
        EquivalenceCheckMethod::LocalRewriteRule,
        "Equivalence certified by the local Boolean identity NOT(NOT(x)) = x.",
        false);
}

// ─────────────────────────────────────────────────────────────────────────────
//  mergeEquivalentGates
//  合併結構等價的 gate（相同 type + 相同 input net 集合）
//  回傳合併的 gate 數量
// ─────────────────────────────────────────────────────────────────────────────
/*int Netlist::mergeEquivalentGates() {
    int merged = 0;

    std::map<std::pair<int, std::vector<int>>, int> seen;

    for (int i = 0; i < (int)gates.size(); i++) {
        Gate& g = gates[i];
        if (g.type == GateType::UNKNOWN) continue;
        if (g.outputNetId < 0) continue;

        // 只對可交換（Commutative）的邏輯閘排序，保護 DFF 的腳位順序
        std::vector<int> sigInputs = g.inputNetIds;
        if (g.type == GateType::AND || g.type == GateType::OR ||
            g.type == GateType::NAND || g.type == GateType::NOR ||
            g.type == GateType::XOR || g.type == GateType::XNOR) {
            std::sort(sigInputs.begin(), sigInputs.end());
        }
        std::pair<int, std::vector<int>> key = std::make_pair((int)g.type, sigInputs);

        auto it = seen.find(key);
        if (it == seen.end()) {
            seen[key] = i;
        } else {
            int keepGateId = it->second;
            int deadGateId = i;

            int keepOutNet = gates[keepGateId].outputNetId;
            int deadOutNet = gates[deadGateId].outputNetId;

            if (keepOutNet < 0 || deadOutNet < 0) continue;
            if (keepOutNet == deadOutNet) continue;

            for (int j = 0; j < (int)nets[deadOutNet].loadGateIds.size(); j++) {
                int loadGateId = nets[deadOutNet].loadGateIds[j];
                Gate& loadGate = gates[loadGateId];
                for (int k = 0; k < (int)loadGate.inputNetIds.size(); k++) {
                    if (loadGate.inputNetIds[k] == deadOutNet) {
                        loadGate.inputNetIds[k] = keepOutNet;
                        nets[keepOutNet].loadGateIds.push_back(loadGateId);
                    }
                }
            }

            if (nets[deadOutNet].isPO) {
                nets[keepOutNet].isPO = true;
                for (int pi = 0; pi < (int)primaryOutputs.size(); pi++) {
                    for (int pj = 0; pj < (int)primaryOutputs[pi].netIds.size(); pj++) {
                        if (primaryOutputs[pi].netIds[pj] == deadOutNet)
                            primaryOutputs[pi].netIds[pj] = keepOutNet;
                    }
                }
            }

            nets[deadOutNet].loadGateIds.clear();
            nets[deadOutNet].driverGateId = -1;

            // 清除 deadGate 在 input nets 上的負載（避免 dangling pointer）
            for (int inNetId : gates[deadGateId].inputNetIds) {
                auto& loads = nets[inNetId].loadGateIds;
                loads.erase(std::remove(loads.begin(), loads.end(), deadGateId), loads.end());
            }

            gates[deadGateId].type = GateType::UNKNOWN;
            gates[deadGateId].outputNetId = -1;
            gates[deadGateId].inputNetIds.clear();

            merged++;
        }
    }

    if (merged > 0) {
        markDirty();
        DeadLogicOptions options;
        removeDeadLogic(options);
    }
    return merged;
}

NetlistEditReport Netlist::mergeEquivalentGatesWithReport() {
    Netlist before = cloneForRollback();
    int merged = mergeEquivalentGates();
    return finalizeEditReport(
        *this,
        before,
        merged,
        "mergeEquivalentGates",
        NetlistEditOperationKind::Simplification,
        "Equivalent gate merge completed.",
        "Equivalent gate merge failed validation and was rolled back.",
        EquivalenceCheckMethod::StructuralIdentity,
        "Equivalence certified by merging gates with identical type and input structure.",
        false);
}*/

// =============================================================================
//  新增 function（對應 CLEANUP_SIMPLIFICATION_NOTES.md 各 section）
// =============================================================================

// ─────────────────────────────────────────────────────────────────────────────
//  Section 1.5: Gate removed flag helpers
// ─────────────────────────────────────────────────────────────────────────────
bool Netlist::isGateRemoved(int gateId) const {
    if (gateId < 0 || gateId >= (int)gates.size()) return true;
    return gates[gateId].type == GateType::UNKNOWN;
}

bool Netlist::markGateRemoved(int gateId) {
    if (gateId < 0 || gateId >= (int)gates.size()) return false;
    if (gates[gateId].type == GateType::UNKNOWN) return false;

    int outNetId = gates[gateId].outputNetId;

    for (int inNetId : gates[gateId].inputNetIds) {
        if (inNetId < 0) continue;
        auto& loads = nets[inNetId].loadGateIds;
        loads.erase(std::remove(loads.begin(), loads.end(), gateId), loads.end());
    }

    if (outNetId >= 0)
        nets[outNetId].driverGateId = -1;

    gates[gateId].type = GateType::UNKNOWN;
    gates[gateId].inputNetIds.clear();
    gates[gateId].outputNetId = -1;
    markDirty();
    return true;
}

int Netlist::compactRemovedGates() {
    std::unordered_set<int> deadSet;
    for (int i = 0; i < (int)gates.size(); i++)
        if (gates[i].type == GateType::UNKNOWN) deadSet.insert(i);
    if (deadSet.empty()) return 0;

    for (int i = 0; i < (int)nets.size(); i++) {
        std::vector<int> newLoads;
        for (int lgid : nets[i].loadGateIds)
            if (!deadSet.count(lgid)) newLoads.push_back(lgid);
        nets[i].loadGateIds = newLoads;
    }

    std::vector<int> oldToNew(gates.size(), -1);
    std::vector<Gate> newGates;
    for (int i = 0; i < (int)gates.size(); i++) {
        if (!deadSet.count(i)) {
            int newId = (int)newGates.size();
            oldToNew[i] = newId;
            newGates.push_back(gates[i]);
            newGates.back().id = newId;
        }
    }

    for (int i = 0; i < (int)nets.size(); i++) {
        if (nets[i].driverGateId >= 0)
            nets[i].driverGateId = oldToNew[nets[i].driverGateId];
        for (int& lgid : nets[i].loadGateIds)
            lgid = oldToNew[lgid];
    }

    gateNameToId.clear();
    for (int i = 0; i < (int)newGates.size(); i++)
        gateNameToId[newGates[i].instName] = i;

    int removed = (int)gates.size() - (int)newGates.size();
    gates = newGates;
    markDirty();
    return removed;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Section 1.7: Validation / Rollback
// ─────────────────────────────────────────────────────────────────────────────
Netlist::StructureViolations Netlist::collectStructureViolations(bool verbose) const {
    StructureViolations v;

    // 1M gates 上壞掉的結構可能有數萬條，全部印出來會淹沒 terminal，
    // 而 LLM 的 context 也吃不下。只印前幾條，其餘靠計數呈現。
    constexpr int kMaxReported = 20;
    int reported = 0;
    auto report = [&](const std::string& message) {
        if (!verbose) return;
        if (reported < kMaxReported) {
            std::cerr << "[validateStructure] " << message << "\n";
        } else if (reported == kMaxReported) {
            std::cerr << "[validateStructure] ... further messages suppressed\n";
        }
        ++reported;
    };

    // expectedLoads 是 pin-level：一顆 gate 有 K 個 pin 接同一條 net 就出現 K 次。
    std::vector<std::vector<int>> expectedLoads(nets.size());

    for (int gi = 0; gi < (int)gates.size(); ++gi) {
        const Gate& g = gates[gi];
        if (g.type == GateType::UNKNOWN) continue;

        if (g.id != gi) {
            ++v.gateIdMismatch;
            report("gate id mismatch: gate[" + std::to_string(gi) +
                   "].id=" + std::to_string(g.id));
        }

        if (g.outputNetId < 0 && g.type != GateType::DFF) {
            ++v.gateMissingOutput;
            report("live gate[" + std::to_string(gi) + "] (" + g.instName +
                   ") has no output net");
        }

        if (g.outputNetId >= 0) {
            if (g.outputNetId >= (int)nets.size()) {
                ++v.gateOutputOutOfRange;
                report("gate[" + std::to_string(gi) + "] outputNetId out of range");
            } else if (nets[g.outputNetId].driverGateId != gi) {
                // 常見於來源檔案本身就有多個 driver：後來的 gate 覆寫了
                // net 的 driverGateId，但先前那顆 gate 仍指著這條 net。
                ++v.gateOutputNotDriver;
                report("gate[" + std::to_string(gi) + "] (" + g.instName +
                       ") outputNet[" + std::to_string(g.outputNetId) +
                       "].driverGateId=" +
                       std::to_string(nets[g.outputNetId].driverGateId) + " mismatch");
            }
        }

        for (int inNetId : g.inputNetIds) {
            if (inNetId < 0) continue;
            if (inNetId >= (int)nets.size()) {
                ++v.gateInputOutOfRange;
                report("gate[" + std::to_string(gi) + "] inputNetId out of range");
                continue;
            }
            if (nets[inNetId].isRemoved) {
                ++v.gateInputRemoved;
                report("gate[" + std::to_string(gi) + "] references removed input net[" +
                       std::to_string(inNetId) + "]");
                continue;
            }
            expectedLoads[inNetId].push_back(gi);
        }
    }

    for (int ni = 0; ni < (int)nets.size(); ++ni) {
        if (nets[ni].isRemoved) continue;

        const int driverGateId = nets[ni].driverGateId;
        if (driverGateId >= 0) {
            if (driverGateId >= (int)gates.size()) {
                ++v.netInvalidDriver;
                report("net[" + std::to_string(ni) + "] has invalid driverGateId=" +
                       std::to_string(driverGateId));
            } else if (gates[driverGateId].type == GateType::UNKNOWN) {
                ++v.netDriverRemoved;
                report("net[" + std::to_string(ni) + "] is driven by removed gate[" +
                       std::to_string(driverGateId) + "]");
            } else if (gates[driverGateId].outputNetId != ni) {
                ++v.netDriverOutputMismatch;
                report("net[" + std::to_string(ni) + "] driverGate[" +
                       std::to_string(driverGateId) + "] does not output to this net");
            }
        }

        for (int lgid : nets[ni].loadGateIds) {
            if (lgid < 0 || lgid >= (int)gates.size()) {
                ++v.netInvalidLoad;
                report("net[" + std::to_string(ni) + "] has invalid loadGateId=" +
                       std::to_string(lgid));
                continue;
            }
            bool found = false;
            for (int inNetId : gates[lgid].inputNetIds) {
                if (inNetId == ni) { found = true; break; }
            }
            if (!found) {
                ++v.netLoadMissingInput;
                report("net[" + std::to_string(ni) + "] loadGate[" +
                       std::to_string(lgid) + "] does not have this net as input");
            }
        }

        std::vector<int> actualLoads = nets[ni].loadGateIds;
        std::sort(actualLoads.begin(), actualLoads.end());
        std::sort(expectedLoads[ni].begin(), expectedLoads[ni].end());
        if (actualLoads != expectedLoads[ni]) {
            ++v.netLoadMultiplicity;
            report("net[" + std::to_string(ni) +
                   "] pin-level loadGateIds multiplicity mismatch");
        }
    }

    return v;
}

bool Netlist::validateStructure() const {
    return collectStructureViolations(true).clean();
}

bool Netlist::validateProblemAConstraints() const {
    bool ok = true;

    for (int ni = 0; ni < (int)nets.size(); ni++) {
        if (nets[ni].isRemoved) continue;
        if (!nets[ni].isPO) continue;
        if (nets[ni].driverGateId < 0 && !nets[ni].isPI && !nets[ni].isConst) {
            std::cerr << "[validateProblemAConstraints] PO net[" << ni
                      << "] (" << nets[ni].name << ") has no driver\n";
            ok = false;
        }
    }

    for (int gi = 0; gi < (int)gates.size(); gi++) {
        if (gates[gi].type != GateType::DFF) continue;
        if (gates[gi].outputNetId < 0) {
            std::cerr << "[validateProblemAConstraints] DFF gate[" << gi
                      << "] (" << gates[gi].instName << ") has invalid outputNetId\n";
            ok = false;
        }
    }

    return ok;
}

Netlist Netlist::cloneForRollback() const {
    return *this;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Section 2.1: Buffer cleanup helpers
// ─────────────────────────────────────────────────────────────────────────────
bool Netlist::replaceAllLoadsOfNet(int oldNetId, int newNetId) {
    if (oldNetId < 0 || oldNetId >= (int)nets.size()) return false;
    if (newNetId < 0 || newNetId >= (int)nets.size()) return false;
    if (oldNetId == newNetId) return true; // 已經是同一個，直接成功

    Net& oldNet = nets[oldNetId];
    Net& newNet = nets[newNetId];

    if (oldNet.loadGateIds.empty()) return true; // 沒東西要搬，視為成功

    // 1. Rewire every matching input pin. loadGateIds is pin-level, so a gate
    // appears once for each input pin connected to the net.
    for (int lgid : oldNet.loadGateIds) {
        if (lgid < 0 || lgid >= (int)gates.size()) continue;
        
        Gate& g = gates[lgid];
        for (int k = 0; k < (int)g.inputNetIds.size(); k++) {
            if (g.inputNetIds[k] == oldNetId) {
                g.inputNetIds[k] = newNetId;
                if (!deferLoadListMaintenance) {
                    newNet.loadGateIds.push_back(lgid);
                }
            }
        }
    }

    // 2. 清除舊 Net 的負載
    oldNet.loadGateIds.clear();

    markDirty();
    return true;
}

NetlistEditReport Netlist::replaceAllLoadsOfNetWithReport(int oldNetId, int newNetId) {
    Netlist before = cloneForRollback();
    const std::string oldNetName =
        (oldNetId >= 0 && oldNetId < (int)nets.size()) ? nets[oldNetId].name : "";
    const std::string newNetName =
        (newNetId >= 0 && newNetId < (int)nets.size()) ? nets[newNetId].name : "";

    const bool ok = replaceAllLoadsOfNet(oldNetId, newNetId);
    NetlistEditReport report = Netlist::buildEditReport(
        before,
        *this,
        "replaceAllLoadsOfNet",
        NetlistEditOperationKind::PrimitiveMutation);
    report.changed = report.changed || ok;

    if (!oldNetName.empty()) {
        report.changedNetNames.push_back(oldNetName);
        report.changedNetIds.push_back(oldNetId);
    }
    if (!newNetName.empty()) {
        report.changedNetNames.push_back(newNetName);
        report.changedNetIds.push_back(newNetId);
    }

    if (!ok) {
        if (report.changed) {
            restoreFrom(before);
            report.rolledBack = true;
        }
        report.success = false;
        report.message = "Load replacement failed: net id was invalid, source and target were identical, or the source had no loads.";
        return report;
    }

    report.message = report.success
        ? "Load replacement completed."
        : "Load replacement failed validation and was rolled back.";

    if (!report.success) {
        restoreFrom(before);
        report.rolledBack = true;
    }

    return report;
}

bool Netlist::bypassBufferGate(int bufGateId) {
    if (bufGateId < 0 || bufGateId >= (int)gates.size()) return false;
    Gate& g = gates[bufGateId];

    if (g.type != GateType::BUF) return false;
    if (g.inputNetIds.size() != 1) return false;

    int inNetId  = g.inputNetIds[0];
    int outNetId = g.outputNetId;

    if (inNetId < 0 || outNetId < 0) return false;
    if (nets[outNetId].isPO) return false;
    if (nets[outNetId].isConst) return false;
    if (inNetId == outNetId) return false;
    if (nets[outNetId].loadGateIds.empty()) return false;

    replaceAllLoadsOfNet(outNetId, inNetId);

    // 從 inNet 的 loadGateIds 移除 BUF gate 自身
    auto& loads = nets[inNetId].loadGateIds;
    loads.erase(std::remove(loads.begin(), loads.end(), bufGateId), loads.end());

    nets[outNetId].driverGateId = -1;
    g.type = GateType::UNKNOWN;
    g.inputNetIds.clear();
    g.outputNetId = -1;

    markDirty();
    return true;
}

std::vector<int> Netlist::findAllRemovableBufferGates() const {
    std::vector<int> result;
    for (int i = 0; i < (int)gates.size(); i++) {
        const Gate& g = gates[i];
        if (g.type != GateType::BUF) continue;
        if (g.inputNetIds.size() != 1) continue;

        int inNetId  = g.inputNetIds[0];
        int outNetId = g.outputNetId;

        if (inNetId < 0 || outNetId < 0) continue;
        if (nets[outNetId].isPO) continue;
        if (nets[outNetId].isConst) continue;
        if (inNetId == outNetId) continue;
        if (nets[outNetId].loadGateIds.empty()) continue;

        result.push_back(i);
    }
    return result;
}

int Netlist::cleanupAllRemovableBuffers() {
    int removed = 0;
    bool changed = true;

    while (changed) {
        changed = false;
        std::vector<int> candidates = findAllRemovableBufferGates();
        for (int bufGateId : candidates) {
            if (bypassBufferGate(bufGateId)) {
                removed++;
                changed = true;
            }
        }
    }

    if (removed > 0) compactRemovedGates();
    return removed;
}

NetlistEditReport Netlist::cleanupAllRemovableBuffersWithReport() {
    Netlist before = cloneForRollback();

    int removed = cleanupAllRemovableBuffers();
    NetlistEditReport report = buildEditReport(
        before,
        *this,
        "cleanupAllRemovableBuffers",
        NetlistEditOperationKind::Cleanup);

    report.changed = report.changed || removed > 0;
    report.costChange = buildDepthCostChange(before, *this);
    if (report.success) {
        certifyEquivalence(
            report,
            EquivalenceCheckMethod::LocalRewriteRule,
            "Equivalence certified by the local Boolean identity BUF(x) = x.");
    }
    report.message = report.success
        ? "Removable buffer cleanup completed."
        : "Removable buffer cleanup failed validation and was rolled back.";

    if (!report.success) {
        restoreFrom(before);
        report.rolledBack = true;
    }

    return report;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Section 2.3: Double inverter removal
// ─────────────────────────────────────────────────────────────────────────────
std::vector<std::pair<int,int>> Netlist::findDoubleInverterPairs() const {
    std::vector<std::pair<int,int>> result;
    for (int i = 0; i < (int)gates.size(); i++) {
        const Gate& g1 = gates[i];
        if (g1.type != GateType::NOT) continue;
        if (!isValidNetId(g1.outputNetId)) continue;

        const Net& midNet = nets[g1.outputNetId];
        if (midNet.loadGateIds.size() != 1) continue;

        const int g2id = midNet.loadGateIds[0];
        if (!isSafeDoubleInverterPair(*this, i, g2id)) continue;

        result.emplace_back(i, g2id);
    }
    return result;
}

bool Netlist::bypassDoubleInverter(int g1id, int g2id) {
    if (!isSafeDoubleInverterPair(*this, g1id, g2id)) return false;

    Gate& g1 = gates[g1id];
    Gate& g2 = gates[g2id];

    int inNetId  = g1.inputNetIds[0];
    int midNetId = g1.outputNetId;
    int outNetId = g2.outputNetId;

    if (!replaceAllLoadsOfNet(outNetId, inNetId)) return false;

    auto& loads = nets[inNetId].loadGateIds;
    loads.erase(std::remove(loads.begin(), loads.end(), g1id), loads.end());

    g1.type = GateType::UNKNOWN; g1.inputNetIds.clear(); g1.outputNetId = -1;
    g2.type = GateType::UNKNOWN; g2.inputNetIds.clear(); g2.outputNetId = -1;
    nets[midNetId].driverGateId = -1; nets[midNetId].loadGateIds.clear();
    nets[outNetId].driverGateId = -1; nets[outNetId].loadGateIds.clear();

    markDirty();
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Section 2.4: Constant propagation helpers
// ─────────────────────────────────────────────────────────────────────────────
bool Netlist::isConst0Net(int netId) const {
    if (netId < 0 || netId >= (int)nets.size()) return false;
    return nets[netId].isConst && (nets[netId].constVal == 0);
}

bool Netlist::isConst1Net(int netId) const {
    if (netId < 0 || netId >= (int)nets.size()) return false;
    return nets[netId].isConst && (nets[netId].constVal == 1);
}

// Every constant net in this codebase is created through addNet("1'b0"/"1'b1")
// and constant nets cannot be renamed, so the canonical net is reachable in
// O(1) through the name index.
// simplifyGateWithConstant()/simplifySameInputGate() call these twice per
// candidate gate, so an O(net count) scan here turns SimplifyConstants /
// LocalSimplificationFixpoint / SafeCleanupFixpoint into an O(candidates *
// net count) pass on large designs.
int Netlist::getConst0NetId() const {
    const int fastId = getNetId("1'b0");
    if (fastId >= 0 && fastId < (int)nets.size() &&
        nets[fastId].isConst && nets[fastId].constVal == 0) {
        return fastId;
    }
    return -1;
}

int Netlist::getConst1NetId() const {
    const int fastId = getNetId("1'b1");
    if (fastId >= 0 && fastId < (int)nets.size() &&
        nets[fastId].isConst && nets[fastId].constVal == 1) {
        return fastId;
    }
    return -1;
}

bool Netlist::simplifyGateWithConstant(int gateId) {
    if (gateId < 0 || gateId >= (int)gates.size()) return false;
    Gate& g = gates[gateId];
    if (g.type == GateType::UNKNOWN || g.type == GateType::DFF) return false;
    if (g.outputNetId < 0) return false;

    int outNetId = g.outputNetId;
    int const0   = getConst0NetId();
    int const1   = getConst1NetId();

    auto rewriteGateInPlace = [&](GateType newType, const std::vector<int>& newInputs) -> bool {
        if (g.type == newType && g.inputNetIds == newInputs) return false;

        if (!deferLoadListMaintenance) {
            for (int inNetId : g.inputNetIds) {
                if (inNetId < 0 || inNetId >= (int)nets.size()) continue;
                auto& loads = nets[inNetId].loadGateIds;
                loads.erase(std::remove(loads.begin(), loads.end(), gateId), loads.end());
            }
        }

        g.type = newType;
        g.inputNetIds = newInputs;
        if (!deferLoadListMaintenance) {
            for (int inNetId : newInputs) {
                if (inNetId < 0 || inNetId >= (int)nets.size()) continue;
                nets[inNetId].loadGateIds.push_back(gateId);
            }
        }
        markDirty();
        return true;
    };

    auto replaceGateWithNet = [&](int srcNetId) -> bool {
        if (srcNetId < 0) return false;
        if (nets[outNetId].isPO) {
            return rewriteGateInPlace(GateType::BUF, {srcNetId});
        }
        replaceAllLoadsOfNet(outNetId, srcNetId);
        if (!deferLoadListMaintenance) {
            for (int inNetId : g.inputNetIds) {
                if (inNetId < 0) continue;
                auto& loads = nets[inNetId].loadGateIds;
                loads.erase(std::remove(loads.begin(), loads.end(), gateId), loads.end());
            }
        }
        nets[outNetId].driverGateId = -1;
        g.type = GateType::UNKNOWN;
        g.inputNetIds.clear();
        g.outputNetId = -1;
        markDirty();
        return true;
    };

    auto insertNotAndReplace = [&](int srcNetId) -> bool {
        if (srcNetId < 0) return false;
        return rewriteGateInPlace(GateType::NOT, {srcNetId});
    };

    GateType t = g.type;

    if (t == GateType::NOT || t == GateType::BUF) {
        if (g.inputNetIds.size() != 1) return false;
        int a = g.inputNetIds[0];
        if (isConst0Net(a)) return replaceGateWithNet(t == GateType::NOT ? const1 : const0);
        if (isConst1Net(a)) return replaceGateWithNet(t == GateType::NOT ? const0 : const1);
        return false;
    }

    if (g.inputNetIds.size() < 2) return false;

    std::vector<int> nonConstantInputs;
    int zeroCount = 0;
    int oneCount = 0;
    for (int inputNetId : g.inputNetIds) {
        if (isConst0Net(inputNetId)) {
            zeroCount++;
        } else if (isConst1Net(inputNetId)) {
            oneCount++;
        } else {
            nonConstantInputs.push_back(inputNetId);
        }
    }
    if (zeroCount == 0 && oneCount == 0) return false;

    auto reducePositiveGate = [&](GateType reducedType, int emptyValue) -> bool {
        if (nonConstantInputs.empty()) {
            return replaceGateWithNet(emptyValue == 0 ? const0 : const1);
        }
        if (nonConstantInputs.size() == 1) {
            return replaceGateWithNet(nonConstantInputs.front());
        }
        return rewriteGateInPlace(reducedType, nonConstantInputs);
    };

    auto reduceNegativeGate = [&](GateType reducedType, int emptyValue) -> bool {
        if (nonConstantInputs.empty()) {
            return replaceGateWithNet(emptyValue == 0 ? const0 : const1);
        }
        if (nonConstantInputs.size() == 1) {
            return insertNotAndReplace(nonConstantInputs.front());
        }
        return rewriteGateInPlace(reducedType, nonConstantInputs);
    };

    if (t == GateType::AND) {
        if (zeroCount > 0) return replaceGateWithNet(const0);
        return reducePositiveGate(GateType::AND, 1);
    }
    if (t == GateType::OR) {
        if (oneCount > 0) return replaceGateWithNet(const1);
        return reducePositiveGate(GateType::OR, 0);
    }
    if (t == GateType::NAND) {
        if (zeroCount > 0) return replaceGateWithNet(const1);
        return reduceNegativeGate(GateType::NAND, 0);
    }
    if (t == GateType::NOR) {
        if (oneCount > 0) return replaceGateWithNet(const0);
        return reduceNegativeGate(GateType::NOR, 1);
    }
    if (t == GateType::XOR || t == GateType::XNOR) {
        const bool invertResult =
            (t == GateType::XNOR) ^ ((oneCount % 2) != 0);
        if (nonConstantInputs.empty()) {
            return replaceGateWithNet(invertResult ? const1 : const0);
        }
        if (nonConstantInputs.size() == 1) {
            return invertResult
                ? insertNotAndReplace(nonConstantInputs.front())
                : replaceGateWithNet(nonConstantInputs.front());
        }
        return rewriteGateInPlace(
            invertResult ? GateType::XNOR : GateType::XOR,
            nonConstantInputs);
    }

    return false;
}

bool supportsConstantSimplification(GateType type) {
    return type == GateType::AND || type == GateType::OR ||
           type == GateType::NOT || type == GateType::NAND ||
           type == GateType::NOR || type == GateType::XOR ||
           type == GateType::XNOR || type == GateType::BUF;
}

std::vector<int> findConstantSimplificationCandidates(
    const Netlist& netlist,
    GateType type,
    int constValue,
    int inputCount)
{
    std::vector<int> result;
    for (int gateId : netlist.findGatesWithConstInput(type, constValue, inputCount)) {
        if (supportsConstantSimplification(netlist.getGate(gateId).type)) {
            result.push_back(gateId);
        }
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Section 2.5: Same-input simplification
// ─────────────────────────────────────────────────────────────────────────────
std::vector<int> Netlist::findSameInputGates() const {
    std::vector<int> result;
    for (int i = 0; i < (int)gates.size(); i++) {
        const Gate& g = gates[i];
        if (g.type == GateType::UNKNOWN || g.type == GateType::DFF) continue;
        if (g.inputNetIds.size() < 2) continue;
        if (g.inputNetIds[0] >= 0 && g.inputNetIds[0] == g.inputNetIds[1])
            result.push_back(i);
    }
    return result;
}

// 從所有 fanin net 的 load list 移除這顆 gate。
// a == b 時 loadGateIds 裡有兩份，remove 會一次清掉全部，正確。
void Netlist::detachGateInputs(int gateId) {
    if (!isValidGateId(gateId)) return;
    for (int inNetId : gates[gateId].inputNetIds) {
        if (!isValidNetId(inNetId)) continue;
        auto& loads = nets[inNetId].loadGateIds;
        loads.erase(std::remove(loads.begin(), loads.end(), gateId), loads.end());
    }
}

// 就地改寫 gate 的型別與輸入，並同步維護 load list。
bool Netlist::rewriteInPlace(int gateId, GateType newType,
                             const std::vector<int>& newInputs) {
    if (!isValidGateId(gateId)) return false;
    Gate& g = gates[gateId];
    if (g.type == newType && g.inputNetIds == newInputs) return false;

    detachGateInputs(gateId);
    g.type = newType;
    g.inputNetIds = newInputs;
    for (int inNetId : newInputs) {
        if (!isValidNetId(inNetId)) continue;
        nets[inNetId].loadGateIds.push_back(gateId);
    }
    markDirty();
    return true;
}

bool Netlist::simplifySameInputGate(int gateId) {
    if (gateId < 0 || gateId >= (int)gates.size()) return false;
    Gate& g = gates[gateId];
    if (g.type == GateType::UNKNOWN || g.type == GateType::DFF) return false;
    if (g.inputNetIds.size() < 2) return false;
    if (g.outputNetId < 0) return false;

    const int a = g.inputNetIds[0];
    const int b = g.inputNetIds[1];
    if (a < 0 || b < 0) return false;

    const bool sameInput = (a == b);
    const bool complementary = !sameInput && areComplementary(*this, a, b);
    if (!sameInput && !complementary) return false;

    const int outNetId = g.outputNetId;
    const GateType t = g.type;

    // 常數 net 可能尚未存在（電路裡沒有任何 1'b0/1'b1 字面值）。
    // 用 ensureConstantNet 而非 getConstXNetId，後者會回 -1 導致靜默不化簡。
    auto constNet = [&](int value) { return ensureConstantNet(*this, value); };

    // 把整顆 gate 換成一條既有 net。outNet 是 PO 時保留 gate 並改成 BUF，
    // 以維持 port 的 driver 結構。
    auto replaceWithNet = [&](int srcNetId) -> bool {
        if (!isValidNetId(srcNetId)) return false;
        if (srcNetId == outNetId) return false;

        if (nets[outNetId].isPO) {
            return rewriteInPlace(gateId, GateType::BUF, {srcNetId});
        }
        replaceAllLoadsOfNet(outNetId, srcNetId);
        detachGateInputs(gateId);
        nets[outNetId].driverGateId = -1;
        g.type = GateType::UNKNOWN;
        g.inputNetIds.clear();
        g.outputNetId = -1;
        markDirty();
        return true;
    };

    auto replaceWithNot = [&](int srcNetId) -> bool {
        return rewriteInPlace(gateId, GateType::NOT, {srcNetId});
    };

    if (sameInput) {
        // AND(x,x)=x  OR(x,x)=x  XOR(x,x)=0  XNOR(x,x)=1
        // NAND(x,x)=~x  NOR(x,x)=~x
        switch (t) {
            case GateType::AND:
            case GateType::OR:   return replaceWithNet(a);
            case GateType::XOR:  return replaceWithNet(constNet(0));
            case GateType::XNOR: return replaceWithNet(constNet(1));
            case GateType::NAND:
            case GateType::NOR:  return replaceWithNot(a);
            default: return false;
        }
    }

    // complementary：x 與 ~x
    // AND(x,~x)=0   OR(x,~x)=1    XOR(x,~x)=1
    // NAND(x,~x)=1  NOR(x,~x)=0   XNOR(x,~x)=0
    switch (t) {
        case GateType::AND:  return replaceWithNet(constNet(0));
        case GateType::OR:   return replaceWithNet(constNet(1));
        case GateType::XOR:  return replaceWithNet(constNet(1));
        case GateType::NAND: return replaceWithNet(constNet(1));
        case GateType::NOR:  return replaceWithNet(constNet(0));
        case GateType::XNOR: return replaceWithNet(constNet(0));
        default: return false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Section 2.6: Dangling logic removal
// ─────────────────────────────────────────────────────────────────────────────
std::unordered_set<int> Netlist::getEssentialGateIdsForTimingEndpoints() const {
    std::unordered_set<int> usefulGates;
    std::unordered_set<int> visitedNets;
    std::queue<int> q;

    // PO seeds
    for (int i = 0; i < (int)nets.size(); i++) {
        if (nets[i].isPO)
            visitedNets.insert(i), q.push(i);
    }

    // DFF seeds (inputs)
    for (int i = 0; i < (int)gates.size(); i++) {
        if (gates[i].type != GateType::DFF) continue;
        usefulGates.insert(i);
        for (int inNetId : gates[i].inputNetIds) {
            if (inNetId >= 0 && visitedNets.insert(inNetId).second)
                q.push(inNetId);
        }
    }

    // backward traversal
    while (!q.empty()) {
        int netId = q.front(); q.pop();
        const Net& net = nets[netId];

        if (net.driverGateId < 0 || net.driverGateId >= (int)gates.size())
            continue;

        int g = net.driverGateId;
        if (!usefulGates.insert(g).second) continue;

        for (int inNetId : gates[g].inputNetIds) {
            if (inNetId >= 0 && visitedNets.insert(inNetId).second)
                q.push(inNetId);
        }
    }

    return usefulGates;
}

std::vector<int> Netlist::findDanglingGateIds() const {
    std::unordered_set<int> essential = getEssentialGateIdsForTimingEndpoints();
    std::vector<int> dangling;
    for (int i = 0; i < (int)gates.size(); i++) {
        if (gates[i].type == GateType::UNKNOWN) continue;
        if (!essential.count(i)) dangling.push_back(i);
    }
    return dangling;
}


// =========================================================================
// 2. 懸空邏輯清理 (Dangling Logic Removal)
// =========================================================================
/*int Netlist::removeDanglingLogic() {
    std::unordered_set<int> usefulGates;
    std::unordered_set<int> usefulNets;
    std::queue<int> q;

    // 起點 A：真正的 Primary Outputs
    for (int i = 0; i < (int)nets.size(); i++) {
        if (nets[i].isPO) {
            usefulNets.insert(i);
            q.push(i);
        }
    }

    // 起點 B：循序邏輯元素 (DFFs)
    // 對於純組合邏輯最佳化而言，DFF 必須被視為「不可刪除的有用節點」
    // 且驅動 DFF 的訊號線 (D, CLK, RN, SN) 必須被視為 Pseudo-PO 嚴格保護！
    for (int i = 0; i < (int)gates.size(); i++) {
        if (!isValidGateId(i) || isGateRemoved(i)) continue;
        
        if (gates[i].type == GateType::DFF) {
            usefulGates.insert(i); // 保護 DFF 本身不被刪除
            
            // 將 DFF 的所有輸入線加入有用清單，並當作 BFS 的起點往上游追溯
            for (int inNetId : gates[i].inputNetIds) {
                if (inNetId >= 0 && !usefulNets.count(inNetId)) {
                    usefulNets.insert(inNetId);
                    q.push(inNetId);
                }
            }
        }
    }

    // 防呆：如果連 PO 或 DFF 都沒有，代表電路異常
    if (q.empty()) {
        return 0; 
    }

    // 2. 核心演算法：反向 BFS 擴散標記 (Reverse Transitive Fanin Traversal)
    while (!q.empty()) {
        int currNetId = q.front();
        q.pop();

        int driverId = nets[currNetId].driverGateId;
        
        // 如果這條線有 Driver，且該 Driver 是有效的合法閘
        if (driverId >= 0 && isValidGateId(driverId) && !isGateRemoved(driverId)) {
            // 如果這個 Gate 尚未被標記為有用
            if (!usefulGates.count(driverId)) {
                usefulGates.insert(driverId);
                
                // 將這個 Gate 的所有輸入線加入有用清單，並推入 Queue 繼續往上游走訪
                for (int inNetId : gates[driverId].inputNetIds) {
                    if (inNetId >= 0 && !usefulNets.count(inNetId)) {
                        usefulNets.insert(inNetId);
                        q.push(inNetId);
                    }
                }
            }
        }
    }

    // 3. 最終刪除階段 (Garbage Collection)
    int removedCount = 0;
    for (int i = 0; i < (int)gates.size(); i++) {
        if (!isValidGateId(i) || isGateRemoved(i)) continue;

        // 如果這個閘不在 usefulGates 裡面，代表它與任何 PO 都沒有關聯
        if (!usefulGates.count(i)) {
            // 統一呼叫 API 來刪除，確保 loadGateIds 等關聯指標被乾淨清除
            if (markGateRemoved(i)) {
                removedCount++;
            }
        }
    }

    return removedCount;
}

NetlistEditReport Netlist::removeDanglingLogicWithReport() {
    Netlist before = cloneForRollback();
    int removed = removeDanglingLogic();
    return finalizeEditReport(
        *this,
        before,
        removed,
        "removeDanglingLogic",
        NetlistEditOperationKind::Cleanup,
        "Dangling logic removal completed.",
        "Dangling logic removal failed validation and was rolled back.",
        EquivalenceCheckMethod::StructuralIdentity,
        "Equivalence certified by removing logic that does not drive any PO or DFF input endpoint.",
        false);
}*/

// ─────────────────────────────────────────────────────────────────────────────
//  Section 2.7: Structural hashing
// ─────────────────────────────────────────────────────────────────────────────
std::uint64_t Netlist::makeStructuralKey(int gateId) const {
    if (gateId < 0 || gateId >= (int)gates.size()) return 0;
    const Gate& g = gates[gateId];
    if (g.type == GateType::UNKNOWN) return 0;
    if (g.inputNetIds.empty() || g.inputNetIds.size() > 2) return 0;

    int a = g.inputNetIds[0];
    int b = g.inputNetIds.size() > 1 ? g.inputNetIds[1] : -1;

    // 可交換的 gate 排序，讓 AND(x,y) 與 AND(y,x) 得到相同 key。
    // DFF 不在此列，但它的 4 個 input 已經被上面的 size 檢查擋掉。
    const bool commutative =
        g.type == GateType::AND  || g.type == GateType::OR   ||
        g.type == GateType::NAND || g.type == GateType::NOR  ||
        g.type == GateType::XOR  || g.type == GateType::XNOR;
    if (commutative && a > b) std::swap(a, b);

    // net id 上限 100 萬遠小於 2^30；-1（不存在的第二輸入）編碼為 0。
    const std::uint64_t ta = static_cast<std::uint64_t>(a + 1) & 0x3FFFFFFFull;
    const std::uint64_t tb = static_cast<std::uint64_t>(b + 1) & 0x3FFFFFFFull;
    return (static_cast<std::uint64_t>(g.type) << 60) | (ta << 30) | tb;
}

std::vector<std::vector<int>> Netlist::findStructurallyEquivalentGateGroups() const {
    std::unordered_map<std::uint64_t, std::vector<int>> keyToGates;
    for (int i = 0; i < (int)gates.size(); i++) {
        if (gates[i].type == GateType::UNKNOWN) continue;
        if (gates[i].outputNetId < 0) continue;
        const std::uint64_t key = makeStructuralKey(i);
        if (key != 0) keyToGates[key].push_back(i);
    }

    std::vector<std::vector<int>> result;
    for (auto& kv : keyToGates)
        if (kv.second.size() >= 2)
            result.push_back(kv.second);
    return result;
}

StructuralMergeSummary Netlist::mergeStructurallyEquivalentGates() {
    StructuralMergeSummary summary;

    std::unordered_map<std::uint64_t, int> canonicalByKey;
    // 記錄真的發生過合併的 key，用來算 equivalenceClassCount。
    std::unordered_set<std::uint64_t> mergedKeys;

    std::queue<int> worklist;
    std::vector<unsigned char> queued(gates.size(), 0);
    auto enqueue = [&](int gateId) {
        if (!isSafeStructuralMergeCandidate(*this, gateId) || queued[gateId] != 0) {
            return;
        }
        queued[gateId] = 1;
        worklist.push(gateId);
    };
    for (int gateId = 0; gateId < static_cast<int>(gates.size()); ++gateId) {
        enqueue(gateId);
    }

    while (!worklist.empty()) {
        int gateId = worklist.front();
        worklist.pop();
        queued[gateId] = 0;
        if (!isSafeStructuralMergeCandidate(*this, gateId)) continue;

        const std::uint64_t key = makeStructuralKey(gateId);
        if (key == 0) continue;
        auto canonicalIt = canonicalByKey.find(key);
        if (canonicalIt == canonicalByKey.end()) {
            canonicalByKey.emplace(key, gateId);
            continue;
        }

        int keepGateId = canonicalIt->second;
        if (!isSafeStructuralMergeCandidate(*this, keepGateId) ||
            makeStructuralKey(keepGateId) != key) {
            canonicalIt->second = gateId;
            continue;
        }
        if (keepGateId == gateId) continue;

        int deadGateId = gateId;
        int keepOutNet = gates[keepGateId].outputNetId;
        int deadOutNet = gates[deadGateId].outputNetId;
        const bool keepIsPo = nets[keepOutNet].isPO;
        const bool deadIsPo = nets[deadOutNet].isPO;
        // 兩邊都是 PO：合併會讓兩個 port 指向同一條 net，破壞 port 對應。
        if (keepIsPo && deadIsPo) {
            continue;
        }
        if (deadIsPo) {
            std::swap(keepGateId, deadGateId);
            std::swap(keepOutNet, deadOutNet);
            canonicalIt->second = keepGateId;
        }
        if (keepOutNet == deadOutNet) continue;

        std::vector<int> affectedGateIds;
        if (!collectActiveStructuralLoads(*this, deadOutNet, affectedGateIds) ||
            !replaceAllLoadsOfNet(deadOutNet, keepOutNet)) {
            continue;
        }

        nets[deadOutNet].driverGateId = -1;
        gates[deadGateId].type = GateType::UNKNOWN;
        gates[deadGateId].outputNetId = -1;
        gates[deadGateId].inputNetIds.clear();
        ++summary.mergedGateCount;
        mergedKeys.insert(key);

        for (int affectedGateId : affectedGateIds) {
            enqueue(affectedGateId);
        }
    }

    summary.equivalenceClassCount = mergedKeys.size();

    if (summary.mergedGateCount > 0) {
        rebuildNetLoadGateIds(*this);
        markDirty();
    }
    return summary;
}

NetlistEditReport Netlist::mergeStructurallyEquivalentGatesWithReport() {
    Netlist before = cloneForRollback();
    const StructuralMergeSummary summary = mergeStructurallyEquivalentGates();

    NetlistEditReport report = finalizeEditReport(
        *this,
        before,
        static_cast<int>(summary.mergedGateCount),
        "mergeStructurallyEquivalentGates",
        NetlistEditOperationKind::Simplification,
        "Structurally equivalent gate merge completed.",
        "Structurally equivalent gate merge failed validation and was rolled back.",
        EquivalenceCheckMethod::StructuralIdentity,
        "Equivalence certified by structural hashing over gate type and input nets.",
        false);

    report.structuralMerge = summary;
    if (report.success && summary.mergedGateCount == 0) {
        report.addWarning("No structural duplicate gate was found.");
    }
    return report;
}

// =============================================================================
//  B-5: Unique name generator（P0）
//  【說明】產生不會撞名的 gate / net 名稱，避免 repeated pass 後命名衝突。
// =============================================================================

std::string Netlist::makeUniqueGateName(const std::string& prefix) const {
    int counter = 0;
    while (true) {
        std::string name = prefix + "_" + std::to_string(counter++);
        if (gateNameToId.find(name) == gateNameToId.end()) return name;
    }
}

std::string Netlist::makeUniqueNetName(const std::string& prefix) const {
    int counter = 0;
    while (true) {
        std::string name = prefix + "_" + std::to_string(counter++);
        if (netNameToId.find(name) == netNameToId.end()) return name;
    }
}

int Netlist::addGateWithUniqueName(const std::string& prefix, GateType type) {
    return addGate(makeUniqueGateName(prefix), type);
}

int Netlist::addNetWithUniqueName(const std::string& prefix) {
    return addNet(makeUniqueNetName(prefix));
}

// =============================================================================
//  B-4: Gate replacement primitives（P0）
//  【說明】把 gate 直接替換成 net / NOT(net) / constant，是 constant propagation
//          和 same-input 化簡的共同底層。
// =============================================================================

// 把 gate 的 output 改成直接由 sourceNet 驅動（gate 本身消失）
bool Netlist::replaceGateWithNet(int gateId, int sourceNetId) {
    if (gateId < 0 || gateId >= (int)gates.size()) return false;
    if (sourceNetId < 0 || sourceNetId >= (int)nets.size()) return false;
    Gate& g = gates[gateId];
    if (g.type == GateType::UNKNOWN) return false;
    int outNetId = g.outputNetId;
    if (outNetId < 0) return false;
    if (nets[outNetId].isPO) return false; // PO-safe 版本請用 preservePortNetAndReplaceDriver

    replaceAllLoadsOfNet(outNetId, sourceNetId);
    for (int inNetId : g.inputNetIds) {
        if (inNetId < 0) continue;
        auto& loads = nets[inNetId].loadGateIds;
        loads.erase(std::remove(loads.begin(), loads.end(), gateId), loads.end());
    }
    nets[outNetId].driverGateId = -1;
    g.type = GateType::UNKNOWN; g.inputNetIds.clear(); g.outputNetId = -1;
    markDirty();
    return true;
}

NetlistEditReport Netlist::replaceGateWithNetWithReport(int gateId, int sourceNetId) {
    Netlist before = cloneForRollback();
    const std::string gateName =
        (gateId >= 0 && gateId < (int)gates.size()) ? gates[gateId].instName : "";
    const std::string sourceNetName =
        (sourceNetId >= 0 && sourceNetId < (int)nets.size()) ? nets[sourceNetId].name : "";
    const int outputNetId =
        (gateId >= 0 && gateId < (int)gates.size()) ? gates[gateId].outputNetId : -1;
    const std::string outputNetName =
        (outputNetId >= 0 && outputNetId < (int)nets.size()) ? nets[outputNetId].name : "";

    const bool ok = replaceGateWithNet(gateId, sourceNetId);
    return finalizeBooleanPrimitiveReport(
        *this,
        before,
        ok,
        "replaceGateWithNet",
        "Gate replacement with net completed.",
        "Gate replacement with net failed: gate/source net was invalid, removed, or the output net is PO.",
        !gateName.empty() ? std::vector<std::string>{gateName} : std::vector<std::string>{},
        !sourceNetName.empty() || !outputNetName.empty()
            ? std::vector<std::string>{sourceNetName, outputNetName}
            : std::vector<std::string>{},
        gateId >= 0 ? std::vector<int>{gateId} : std::vector<int>{},
        sourceNetId >= 0 || outputNetId >= 0
            ? std::vector<int>{sourceNetId, outputNetId}
            : std::vector<int>{});
}

// 把 gate 替換成常數 net（直接改接所有 load）
bool Netlist::replaceGateWithConstant(int gateId, int constNetId) {
    return replaceGateWithNet(gateId, constNetId);
}

NetlistEditReport Netlist::replaceGateWithConstantWithReport(int gateId, int constNetId) {
    Netlist before = cloneForRollback();
    const std::string gateName =
        (gateId >= 0 && gateId < (int)gates.size()) ? gates[gateId].instName : "";
    const std::string constNetName =
        (constNetId >= 0 && constNetId < (int)nets.size()) ? nets[constNetId].name : "";

    const bool ok = replaceGateWithConstant(gateId, constNetId);
    NetlistEditReport report = finalizeBooleanPrimitiveReport(
        *this,
        before,
        ok,
        "replaceGateWithConstant",
        "Gate replacement with constant completed.",
        "Gate replacement with constant failed: gate/constant net was invalid, removed, or the output net is PO.",
        !gateName.empty() ? std::vector<std::string>{gateName} : std::vector<std::string>{},
        !constNetName.empty() ? std::vector<std::string>{constNetName} : std::vector<std::string>{},
        gateId >= 0 ? std::vector<int>{gateId} : std::vector<int>{},
        constNetId >= 0 ? std::vector<int>{constNetId} : std::vector<int>{});
    if (constNetId >= 0 && constNetId < (int)nets.size() && !nets[constNetId].isConst) {
        report.addWarning("constNetId does not refer to a constant net.");
    }
    return report;
}

// 把 gate 替換成 NOT(sourceNet)（gate 被改寫成 NOT gate）
bool Netlist::replaceGateWithNotOfNet(int gateId, int sourceNetId) {
    if (gateId < 0 || gateId >= (int)gates.size()) return false;
    if (sourceNetId < 0 || sourceNetId >= (int)nets.size()) return false;
    Gate& g = gates[gateId];
    if (g.type == GateType::UNKNOWN) return false;
    if (g.outputNetId < 0 || nets[g.outputNetId].isPO) return false;

    // 從所有原本的 input net 移除此 gate
    for (int inNetId : g.inputNetIds) {
        if (inNetId < 0) continue;
        auto& loads = nets[inNetId].loadGateIds;
        loads.erase(std::remove(loads.begin(), loads.end(), gateId), loads.end());
    }
    g.type = GateType::NOT;
    g.inputNetIds = { sourceNetId };
    nets[sourceNetId].loadGateIds.push_back(gateId);
    markDirty();
    return true;
}

NetlistEditReport Netlist::replaceGateWithNotOfNetWithReport(int gateId, int sourceNetId) {
    Netlist before = cloneForRollback();
    const std::string gateName =
        (gateId >= 0 && gateId < (int)gates.size()) ? gates[gateId].instName : "";
    const std::string sourceNetName =
        (sourceNetId >= 0 && sourceNetId < (int)nets.size()) ? nets[sourceNetId].name : "";
    const int outputNetId =
        (gateId >= 0 && gateId < (int)gates.size()) ? gates[gateId].outputNetId : -1;
    const std::string outputNetName =
        (outputNetId >= 0 && outputNetId < (int)nets.size()) ? nets[outputNetId].name : "";

    const bool ok = replaceGateWithNotOfNet(gateId, sourceNetId);
    return finalizeBooleanPrimitiveReport(
        *this,
        before,
        ok,
        "replaceGateWithNotOfNet",
        "Gate replacement with NOT(source net) completed.",
        "Gate replacement with NOT(source net) failed: gate/source net was invalid, removed, or the output net is PO.",
        !gateName.empty() ? std::vector<std::string>{gateName} : std::vector<std::string>{},
        !sourceNetName.empty() || !outputNetName.empty()
            ? std::vector<std::string>{sourceNetName, outputNetName}
            : std::vector<std::string>{},
        gateId >= 0 ? std::vector<int>{gateId} : std::vector<int>{},
        sourceNetId >= 0 || outputNetId >= 0
            ? std::vector<int>{sourceNetId, outputNetId}
            : std::vector<int>{});
}

// 新增一個 gate 並讓它驅動指定的 outputNet
// 回傳新 gate 的 ID；失敗回傳 -1
int Netlist::createGateDrivingNet(
    GateType type,
    const std::vector<int>& inputNetIds,
    int outputNetId,
    const std::string& nameHint)
{
    if (outputNetId < 0 || outputNetId >= (int)nets.size()) return -1;
    if (nets[outputNetId].isRemoved) return -1;

    int gateId = addGateWithUniqueName(nameHint.empty() ? "_cg" : nameHint, type);

    int oldDriver = nets[outputNetId].driverGateId;
    if (oldDriver >= 0 && oldDriver < (int)gates.size() && oldDriver != gateId) {
        if (gates[oldDriver].outputNetId == outputNetId) {
            gates[oldDriver].outputNetId = -1;
        }
    }

    gates[gateId].outputNetId = outputNetId;
    nets[outputNetId].driverGateId = gateId;

    for (int inNetId : inputNetIds) {
        if (inNetId < 0 || inNetId >= (int)nets.size()) continue;
        gates[gateId].inputNetIds.push_back(inNetId);
        nets[inNetId].loadGateIds.push_back(gateId);
    }
    markDirty();
    return gateId;
}

// 把 gate 的所有 pin 斷開，標記為 UNKNOWN（不 compact）
bool Netlist::removeGateAndDetachPins(int gateId) {
    return markGateRemoved(gateId);
}

// =============================================================================
//  B-1: Pin-level 精準改線 primitive（P0）
//  【說明】讓 rewrite pass 可以精確改指定的 input pin，不影響其他 pin。
// =============================================================================

// 斷開指定 gate 的第 pinIndex 個 input pin
bool Netlist::disconnectGateInputPin(int gateId, int pinIndex) {
    if (gateId < 0 || gateId >= (int)gates.size()) return false;
    Gate& g = gates[gateId];
    if (pinIndex < 0 || pinIndex >= (int)g.inputNetIds.size()) return false;

    int oldNetId = g.inputNetIds[pinIndex];
    if (oldNetId >= 0 && oldNetId < (int)nets.size()) {
        auto& loads = nets[oldNetId].loadGateIds;
        eraseOneLoad(loads, gateId);
    }
    g.inputNetIds[pinIndex] = -1;
    markDirty();
    return true;
}

// 把指定 gate 的第 pinIndex 個 input pin 接到 netId
bool Netlist::connectGateInputPin(int gateId, int pinIndex, int netId) {
    if (gateId < 0 || gateId >= (int)gates.size()) return false;
    if (pinIndex < 0) return false;
    if (netId < 0 || netId >= (int)nets.size()) return false;
    if (nets[netId].isRemoved) return false;
    Gate& g = gates[gateId];

    // 如果 pinIndex 超出現有大小，先擴充
    if (pinIndex >= (int)g.inputNetIds.size())
        g.inputNetIds.resize(pinIndex + 1, -1);

    // 先斷開舊的
    int oldNetId = g.inputNetIds[pinIndex];
    if (oldNetId >= 0 && oldNetId < (int)nets.size()) {
        auto& loads = nets[oldNetId].loadGateIds;
        eraseOneLoad(loads, gateId);
    }

    g.inputNetIds[pinIndex] = netId;
    nets[netId].loadGateIds.push_back(gateId);
    markDirty();
    return true;
}

// 把指定 gate 的第 pinIndex 個 input pin 改接到 newNetId（先斷舊再連新）
bool Netlist::reconnectGateInputPin(int gateId, int pinIndex, int newNetId) {
    return connectGateInputPin(gateId, pinIndex, newNetId);
}

// 用名稱版本：把指定 gate 的 pinName pin 改接到 newNetName
bool Netlist::reconnectGateInputPinByName(
    const std::string& gateName,
    const std::string& pinName,
    const std::string& newNetName)
{
    int gateId = getGateId(gateName);
    int newNetId = getNetId(newNetName);
    if (gateId < 0 || newNetId < 0) return false;

    Gate& g = gates[gateId];
    // 找 pinName 對應的 index
    for (int i = 0; i < (int)g.inputPinNames.size(); i++) {
        if (g.inputPinNames[i] == pinName) {
            return reconnectGateInputPin(gateId, i, newNetId);
        }
    }
    return false;
}

// =============================================================================
//  B-2: PO-safe output rewrite primitive（P1）
//  【說明】當 output net 是 PO 時，不能直接移掉 net，要保留 port name，改變 driver。
// =============================================================================

// 把 targetNet 的 driver 改成 newDriverGate
// targetNet 保留（含 PO flag / name），只換掉是誰在驅動它
bool Netlist::replaceDriverOfNet(int targetNetId, int newDriverGateId) {
    if (targetNetId < 0 || targetNetId >= (int)nets.size()) return false;
    if (newDriverGateId < 0 || newDriverGateId >= (int)gates.size()) return false;
    if (nets[targetNetId].isRemoved) return false;
    if (gates[newDriverGateId].type == GateType::UNKNOWN) return false;

    int oldDriver = nets[targetNetId].driverGateId;
    if (oldDriver >= 0 && oldDriver < (int)gates.size())
        gates[oldDriver].outputNetId = -1;

    int oldOutputNet = gates[newDriverGateId].outputNetId;
    if (oldOutputNet >= 0 && oldOutputNet < (int)nets.size() &&
        nets[oldOutputNet].driverGateId == newDriverGateId) {
        nets[oldOutputNet].driverGateId = -1;
    }

    nets[targetNetId].driverGateId = newDriverGateId;
    gates[newDriverGateId].outputNetId = targetNetId;
    markDirty();
    return true;
}

NetlistEditReport Netlist::replaceDriverOfNetWithReport(int targetNetId, int newDriverGateId) {
    Netlist before = cloneForRollback();
    const std::string targetNetName =
        (targetNetId >= 0 && targetNetId < (int)nets.size()) ? nets[targetNetId].name : "";
    const int oldDriverGateId =
        (targetNetId >= 0 && targetNetId < (int)nets.size()) ? nets[targetNetId].driverGateId : -1;
    const std::string oldDriverGateName =
        (oldDriverGateId >= 0 && oldDriverGateId < (int)gates.size()) ? gates[oldDriverGateId].instName : "";
    const std::string newDriverGateName =
        (newDriverGateId >= 0 && newDriverGateId < (int)gates.size()) ? gates[newDriverGateId].instName : "";

    const bool ok = replaceDriverOfNet(targetNetId, newDriverGateId);

    std::vector<std::string> changedGateNames;
    if (!oldDriverGateName.empty()) changedGateNames.push_back(oldDriverGateName);
    if (!newDriverGateName.empty()) changedGateNames.push_back(newDriverGateName);
    std::vector<int> changedGateIds;
    if (oldDriverGateId >= 0) changedGateIds.push_back(oldDriverGateId);
    if (newDriverGateId >= 0) changedGateIds.push_back(newDriverGateId);

    return finalizeBooleanPrimitiveReport(
        *this,
        before,
        ok,
        "replaceDriverOfNet",
        "Net driver replacement completed.",
        "Net driver replacement failed: target net or new driver gate was invalid or removed.",
        changedGateNames,
        !targetNetName.empty() ? std::vector<std::string>{targetNetName} : std::vector<std::string>{},
        changedGateIds,
        targetNetId >= 0 ? std::vector<int>{targetNetId} : std::vector<int>{});
}

// 把 gate 的 output 改接到 newOutputNetId（舊的 output net 不動，只斷開）
bool Netlist::rewireGateOutputToExistingNet(int gateId, int newOutputNetId) {
    if (gateId < 0 || gateId >= (int)gates.size()) return false;
    if (newOutputNetId < 0 || newOutputNetId >= (int)nets.size()) return false;
    if (gates[gateId].type == GateType::UNKNOWN) return false;
    if (nets[newOutputNetId].isRemoved) return false;

    int oldOutNet = gates[gateId].outputNetId;
    if (oldOutNet >= 0 && oldOutNet < (int)nets.size())
        if (nets[oldOutNet].driverGateId == gateId)
            nets[oldOutNet].driverGateId = -1;

    int oldDriver = nets[newOutputNetId].driverGateId;
    if (oldDriver >= 0 && oldDriver < (int)gates.size() && oldDriver != gateId) {
        if (gates[oldDriver].outputNetId == newOutputNetId) {
            gates[oldDriver].outputNetId = -1;
        }
    }

    gates[gateId].outputNetId = newOutputNetId;
    nets[newOutputNetId].driverGateId = gateId;
    markDirty();
    return true;
}

NetlistEditReport Netlist::rewireGateOutputToExistingNetWithReport(int gateId, int newOutputNetId) {
    Netlist before = cloneForRollback();
    const std::string gateName =
        (gateId >= 0 && gateId < (int)gates.size()) ? gates[gateId].instName : "";
    const int oldOutputNetId =
        (gateId >= 0 && gateId < (int)gates.size()) ? gates[gateId].outputNetId : -1;
    const std::string oldOutputNetName =
        (oldOutputNetId >= 0 && oldOutputNetId < (int)nets.size()) ? nets[oldOutputNetId].name : "";
    const std::string newOutputNetName =
        (newOutputNetId >= 0 && newOutputNetId < (int)nets.size()) ? nets[newOutputNetId].name : "";
    const int oldTargetDriverGateId =
        (newOutputNetId >= 0 && newOutputNetId < (int)nets.size()) ? nets[newOutputNetId].driverGateId : -1;
    const std::string oldTargetDriverGateName =
        (oldTargetDriverGateId >= 0 && oldTargetDriverGateId < (int)gates.size())
            ? gates[oldTargetDriverGateId].instName
            : "";

    const bool ok = rewireGateOutputToExistingNet(gateId, newOutputNetId);

    std::vector<std::string> changedGateNames;
    if (!gateName.empty()) changedGateNames.push_back(gateName);
    if (!oldTargetDriverGateName.empty()) changedGateNames.push_back(oldTargetDriverGateName);
    std::vector<std::string> changedNetNames;
    if (!oldOutputNetName.empty()) changedNetNames.push_back(oldOutputNetName);
    if (!newOutputNetName.empty()) changedNetNames.push_back(newOutputNetName);
    std::vector<int> changedGateIds;
    if (gateId >= 0) changedGateIds.push_back(gateId);
    if (oldTargetDriverGateId >= 0) changedGateIds.push_back(oldTargetDriverGateId);
    std::vector<int> changedNetIds;
    if (oldOutputNetId >= 0) changedNetIds.push_back(oldOutputNetId);
    if (newOutputNetId >= 0) changedNetIds.push_back(newOutputNetId);

    return finalizeBooleanPrimitiveReport(
        *this,
        before,
        ok,
        "rewireGateOutputToExistingNet",
        "Gate output rewire completed.",
        "Gate output rewire failed: gate or target net was invalid or removed.",
        changedGateNames,
        changedNetNames,
        changedGateIds,
        changedNetIds);
}

// 保留 PO net（不改名、不移除），但插入一個新 gate 來驅動它
// 等於：新建 gate(type, inputNetIds) -> poNetId
bool Netlist::preservePortNetAndReplaceDriver(
    int poNetId,
    GateType newGateType,
    const std::vector<int>& inputNetIds)
{
    if (poNetId < 0 || poNetId >= (int)nets.size()) return false;

    // 移除舊 driver 的 outputNetId
    int oldDriver = nets[poNetId].driverGateId;
    if (oldDriver >= 0 && oldDriver < (int)gates.size()) {
        gates[oldDriver].outputNetId = -1;
        markDirty();
    }

    // 新增一個 gate 驅動這個 PO net
    int newGateId = createGateDrivingNet(newGateType, inputNetIds, poNetId, "_po_drv");
    return newGateId >= 0;
}

// 把 targetNet 的功能換成 sourceNet 的功能，但保留 targetNet 的名稱與 PO flag
// 做法：在 targetNet 前面插入一個 BUF，讓 sourceNet -> BUF -> targetNet
bool Netlist::replaceNetFunctionWithNetKeepingName(int targetNetId, int sourceNetId) {
    if (targetNetId < 0 || targetNetId >= (int)nets.size()) return false;
    if (sourceNetId < 0 || sourceNetId >= (int)nets.size()) return false;
    if (targetNetId == sourceNetId) return true;

    // 如果 targetNet 不是 PO，直接 replaceAllLoadsOfNet 即可
    if (!nets[targetNetId].isPO) {
        return replaceAllLoadsOfNet(targetNetId, sourceNetId);
    }

    // 如果是 PO，插入一個 BUF：sourceNet -> BUF -> targetNet
    return preservePortNetAndReplaceDriver(targetNetId, GateType::BUF, { sourceNetId });
}

NetlistEditReport Netlist::replaceNetFunctionWithNetKeepingNameWithReport(int targetNetId, int sourceNetId) {
    Netlist before = cloneForRollback();
    const std::string targetNetName =
        (targetNetId >= 0 && targetNetId < (int)nets.size()) ? nets[targetNetId].name : "";
    const std::string sourceNetName =
        (sourceNetId >= 0 && sourceNetId < (int)nets.size()) ? nets[sourceNetId].name : "";
    const int oldDriverGateId =
        (targetNetId >= 0 && targetNetId < (int)nets.size()) ? nets[targetNetId].driverGateId : -1;
    const std::string oldDriverGateName =
        (oldDriverGateId >= 0 && oldDriverGateId < (int)gates.size()) ? gates[oldDriverGateId].instName : "";

    const bool ok = replaceNetFunctionWithNetKeepingName(targetNetId, sourceNetId);
    NetlistEditReport report = Netlist::buildEditReport(
        before,
        *this,
        "replaceNetFunctionWithNetKeepingName",
        NetlistEditOperationKind::PrimitiveMutation);
    report.changed = report.changed || (ok && targetNetId != sourceNetId);

    if (!oldDriverGateName.empty()) {
        report.changedGateNames.push_back(oldDriverGateName);
        report.changedGateIds.push_back(oldDriverGateId);
    }
    const int newDriverGateId =
        (targetNetId >= 0 && targetNetId < (int)nets.size()) ? nets[targetNetId].driverGateId : -1;
    if (newDriverGateId >= 0 && newDriverGateId < (int)gates.size() &&
        newDriverGateId != oldDriverGateId) {
        report.changedGateNames.push_back(gates[newDriverGateId].instName);
        report.changedGateIds.push_back(newDriverGateId);
    }
    if (!targetNetName.empty()) {
        report.changedNetNames.push_back(targetNetName);
        report.changedNetIds.push_back(targetNetId);
    }
    if (!sourceNetName.empty()) {
        report.changedNetNames.push_back(sourceNetName);
        report.changedNetIds.push_back(sourceNetId);
    }

    if (!ok) {
        if (report.changed) {
            restoreFrom(before);
            report.rolledBack = true;
        }
        report.success = false;
        report.message = "Net function replacement failed: target/source net was invalid.";
        return report;
    }

    report.message = report.success
        ? "Net function replacement completed while keeping the target net name."
        : "Net function replacement failed validation and was rolled back.";

    if (!report.success) {
        restoreFrom(before);
        report.rolledBack = true;
    }

    return report;
}

// =============================================================================
//  B-3: Net merge / net bypass primitive（P1）
//  【說明】完整處理 PI / PO / constant / driver / loads 的 net merge。
// =============================================================================

// 把 fromNet 完全合併到 toNet：fromNet 的所有 load 改接到 toNet，fromNet 清空
//把fromNet的東西繼承給toNet，刪除fromNet
bool Netlist::mergeNetIntoNet(int fromNetId, int toNetId) {
    if (fromNetId < 0 || fromNetId >= (int)nets.size()) return false;
    if (toNetId < 0 || toNetId >= (int)nets.size()) return false;
    if (fromNetId == toNetId) return true;

    // 如果 fromNet 是 PO，把 PO 語意也轉移到 toNet
    if (nets[fromNetId].isPO) {
        nets[toNetId].isPO = true;
        for (int pi = 0; pi < (int)primaryOutputs.size(); pi++)
            for (int pj = 0; pj < (int)primaryOutputs[pi].netIds.size(); pj++)
                if (primaryOutputs[pi].netIds[pj] == fromNetId)
                    primaryOutputs[pi].netIds[pj] = toNetId;
        nets[fromNetId].isPO = false;
    }

    replaceAllLoadsOfNet(fromNetId, toNetId);

    // 清除 fromNet 的 driver 連線
    int drv = nets[fromNetId].driverGateId;
    if (drv >= 0 && drv < (int)gates.size())
        if (gates[drv].outputNetId == fromNetId)
            gates[drv].outputNetId = -1;
    nets[fromNetId].driverGateId = -1;

    markDirty();
    return true;
}

NetlistEditReport Netlist::mergeNetIntoNetWithReport(int fromNetId, int toNetId) {
    Netlist before = cloneForRollback();
    const std::string fromNetName =
        (fromNetId >= 0 && fromNetId < (int)nets.size()) ? nets[fromNetId].name : "";
    const std::string toNetName =
        (toNetId >= 0 && toNetId < (int)nets.size()) ? nets[toNetId].name : "";
    const int fromDriverGateId =
        (fromNetId >= 0 && fromNetId < (int)nets.size()) ? nets[fromNetId].driverGateId : -1;
    const std::string fromDriverGateName =
        (fromDriverGateId >= 0 && fromDriverGateId < (int)gates.size())
            ? gates[fromDriverGateId].instName
            : "";

    const bool ok = mergeNetIntoNet(fromNetId, toNetId);
    NetlistEditReport report = finalizeBooleanPrimitiveReport(
        *this,
        before,
        ok,
        "mergeNetIntoNet",
        "Net merge completed.",
        "Net merge failed: source/target net id was invalid.",
        !fromDriverGateName.empty() ? std::vector<std::string>{fromDriverGateName} : std::vector<std::string>{},
        !fromNetName.empty() || !toNetName.empty()
            ? std::vector<std::string>{fromNetName, toNetName}
            : std::vector<std::string>{},
        fromDriverGateId >= 0 ? std::vector<int>{fromDriverGateId} : std::vector<int>{},
        fromNetId >= 0 || toNetId >= 0
            ? std::vector<int>{fromNetId, toNetId}
            : std::vector<int>{});
    if (ok && fromNetId == toNetId) {
        report.changed = false;
        report.addWarning("fromNetId and toNetId are identical; merge is a no-op.");
    }
    return report;
}

// bypass removedNet，讓所有接到它的 gate 改接到 replacementNet
// 同時保留 PO 語意（若 removedNet 是 PO）
bool Netlist::bypassNetKeepingPortSemantics(int removedNetId, int replacementNetId) {
    return mergeNetIntoNet(removedNetId, replacementNetId);
}

NetlistEditReport Netlist::bypassNetKeepingPortSemanticsWithReport(int removedNetId, int replacementNetId) {
    Netlist before = cloneForRollback();
    const std::string removedNetName =
        (removedNetId >= 0 && removedNetId < (int)nets.size()) ? nets[removedNetId].name : "";
    const std::string replacementNetName =
        (replacementNetId >= 0 && replacementNetId < (int)nets.size()) ? nets[replacementNetId].name : "";
    const bool ok = bypassNetKeepingPortSemantics(removedNetId, replacementNetId);
    return finalizeBooleanPrimitiveReport(
        *this,
        before,
        ok,
        "bypassNetKeepingPortSemantics",
        "Net bypass completed while preserving port semantics.",
        "Net bypass failed: removed/replacement net id was invalid.",
        {},
        !removedNetName.empty() || !replacementNetName.empty()
            ? std::vector<std::string>{removedNetName, replacementNetName}
            : std::vector<std::string>{},
        {},
        removedNetId >= 0 || replacementNetId >= 0
            ? std::vector<int>{removedNetId, replacementNetId}
            : std::vector<int>{});
}

// 把 oldNet 的所有 load 改接到 newNet（allowDuplicateLoads 控制是否允許重複）
bool Netlist::redirectAllLoads(int oldNetId, int newNetId, bool allowDuplicateLoads) {
    if (oldNetId < 0 || oldNetId >= (int)nets.size()) return false;
    if (newNetId < 0 || newNetId >= (int)nets.size()) return false;
    if (oldNetId == newNetId) return false;

    // loadGateIds is pin-level, so suppressing repeated gate IDs would make
    // the reverse adjacency disagree with gates that have multiple matching
    // input pins. Keep the legacy parameter for API compatibility only.
    (void)allowDuplicateLoads;
    return replaceAllLoadsOfNet(oldNetId, newNetId);
}

NetlistEditReport Netlist::redirectAllLoadsWithReport(int oldNetId, int newNetId, bool allowDuplicateLoads) {
    Netlist before = cloneForRollback();
    const std::string oldNetName =
        (oldNetId >= 0 && oldNetId < (int)nets.size()) ? nets[oldNetId].name : "";
    const std::string newNetName =
        (newNetId >= 0 && newNetId < (int)nets.size()) ? nets[newNetId].name : "";
    const std::vector<int> oldLoadGateIds =
        (oldNetId >= 0 && oldNetId < (int)nets.size()) ? nets[oldNetId].loadGateIds : std::vector<int>{};

    const bool ok = redirectAllLoads(oldNetId, newNetId, allowDuplicateLoads);

    std::vector<int> changedGateIds;
    std::vector<std::string> changedGateNames;
    std::unordered_set<int> recordedGateIds;
    for (int gateId : oldLoadGateIds) {
        if (gateId < 0 || gateId >= (int)gates.size()) continue;
        if (!recordedGateIds.insert(gateId).second) continue;
        changedGateIds.push_back(gateId);
        changedGateNames.push_back(gates[gateId].instName);
    }

    return finalizeBooleanPrimitiveReport(
        *this,
        before,
        ok,
        "redirectAllLoads",
        "Load redirection completed.",
        "Load redirection failed: old/new net id was invalid or identical.",
        changedGateNames,
        !oldNetName.empty() || !newNetName.empty()
            ? std::vector<std::string>{oldNetName, newNetName}
            : std::vector<std::string>{},
        changedGateIds,
        oldNetId >= 0 || newNetId >= 0
            ? std::vector<int>{oldNetId, newNetId}
            : std::vector<int>{});
}

// 如果 net 沒有任何 load 且不是 PO / PI / const，把它從 netlist 移除
bool Netlist::removeNetIfUnused(int netId) {
    if (netId < 0 || netId >= (int)nets.size()) return false;
    const Net& n = nets[netId];
    if (n.isRemoved) return false;
    if (n.isPO || n.isPI || n.isConst) return false;
    if (!n.loadGateIds.empty()) return false;

    // driver 仍存活時不可移除:剝掉活 gate 的 output pin 會讓 writer 少印一個 port,
    // 產生語法合法但功能錯誤的 netlist,且 validateStructure 抓不到。
    if (n.driverGateId >= 0 && isValidGateId(n.driverGateId) &&
        !isGateRemoved(n.driverGateId)) {
        return false;
    }

    // 通知 driver gate
    if (n.driverGateId >= 0 && n.driverGateId < (int)gates.size())
        if (gates[n.driverGateId].outputNetId == netId)
            gates[n.driverGateId].outputNetId = -1;

    // 用 sentinel 標記（不真的 erase，避免 ID shift）
    nets[netId].driverGateId = -1;
    nets[netId].loadGateIds.clear();
    nets[netId].isRemoved = true;
    // 不 erase，保留 slot，讓 compact 時處理
    markDirty();
    return true;
}

NetlistEditReport Netlist::removeNetIfUnusedWithReport(int netId) {
    Netlist before = cloneForRollback();
    const std::string netName =
        (netId >= 0 && netId < (int)nets.size()) ? nets[netId].name : "";
    const int driverGateId =
        (netId >= 0 && netId < (int)nets.size()) ? nets[netId].driverGateId : -1;
    const std::string driverGateName =
        (driverGateId >= 0 && driverGateId < (int)gates.size()) ? gates[driverGateId].instName : "";

    const bool ok = removeNetIfUnused(netId);
    NetlistEditReport report = finalizeBooleanPrimitiveReport(
        *this,
        before,
        ok,
        "removeNetIfUnused",
        "Unused net removal completed.",
        "Unused net removal failed: net was invalid, PI/PO/const, already removed, or still had loads.",
        !driverGateName.empty() ? std::vector<std::string>{driverGateName} : std::vector<std::string>{},
        !netName.empty() ? std::vector<std::string>{netName} : std::vector<std::string>{},
        driverGateId >= 0 ? std::vector<int>{driverGateId} : std::vector<int>{},
        netId >= 0 ? std::vector<int>{netId} : std::vector<int>{});
    if (report.success) {
        certifyEquivalence(
            report,
            EquivalenceCheckMethod::StructuralIdentity,
            "Equivalence certified by removing one internal net with no loads and no PI/PO/constant role.");
    }
    return report;
}

// 掃描所有 net，移除沒有 load 且非 PO/PI/const 的 net(計算出移除掉的net數量)
int Netlist::removeUnusedNets() {
    int removed = 0;
    for (int i = 0; i < (int)nets.size(); i++)
        if (removeNetIfUnused(i)) removed++;
    return removed;
}

NetlistEditReport Netlist::removeUnusedNetsWithReport() {
    Netlist before = cloneForRollback();
    std::vector<int> candidateNetIds;
    std::vector<std::string> candidateNetNames;
    for (int i = 0; i < (int)nets.size(); ++i) {
        if (nets[i].isRemoved || nets[i].isPO || nets[i].isPI || nets[i].isConst) continue;
        if (!nets[i].loadGateIds.empty()) continue;
        candidateNetIds.push_back(i);
        candidateNetNames.push_back(nets[i].name);
    }

    const int removed = removeUnusedNets();
    NetlistEditReport report = finalizeEditReport(
        *this,
        before,
        removed,
        "removeUnusedNets",
        NetlistEditOperationKind::Cleanup,
        "Unused net cleanup completed.",
        "Unused net cleanup failed validation and was rolled back.",
        EquivalenceCheckMethod::StructuralIdentity,
        "Equivalence certified by removing internal nets with no loads and no PI/PO/constant role.",
        false);
    report.changedNetIds = candidateNetIds;
    report.changedNetNames = candidateNetNames;
    if (removed == 0) {
        report.addWarning("No unused internal nets were removed.");
    }
    return report;
}

// =============================================================================
//  B-6: Transaction / rollback primitive（P0）
//  【說明】正式的 snapshot / restore API，讓 rewrite pass 失敗時可以乾淨還原。
// =============================================================================

bool Netlist::restoreFrom(const Netlist& backup) {
    const uint64_t currentRevision = revision_;
    *this = backup;
    revision_ = std::max(currentRevision, revision_) + 1;
    dirty_ = true;
    return true;
}

bool Netlist::validateAfterMutation() const {
    return validateStructure() && validateProblemAConstraints();
}

// =============================================================================
//  B-7: Cleanup fixpoint runner（P1）
//  【說明】反覆執行 constant propagation / same-input 化簡，直到沒有新的機會。
// =============================================================================

// 對所有 gate 執行一輪 constant propagation，回傳化簡的 gate 數
int Netlist::simplifyAllGatesWithConstants() {
    return simplifyGatesWithConstants(GateType::UNKNOWN, -1, -1);
}

NetlistEditReport Netlist::simplifyAllGatesWithConstantsWithReport() {
    return simplifyGatesWithConstantsWithReport(GateType::UNKNOWN, -1, -1);
}

int Netlist::simplifyGatesWithConstants(
    GateType type,
    int constValue,
    int inputCount)
{
    const bool wasDeferred = deferLoadListMaintenance;
    deferLoadListMaintenance = true;
    int count = 0;

    // 化簡本身會製造新的常數輸入：replaceGateWithNet 會把下游 gate 的
    // inputNetIds 改接到常數 net，那些 gate 因此變成新的候選。
    // 候選清單只算一次的話，只能清掉第一層，鏈狀結構會大量漏掉。
    for (;;) {
        // 每輪重算候選前必須先修好 load list：defer 模式下 rewriteGateInPlace
        // 不維護 loadGateIds，而下一輪的 replaceAllLoadsOfNet 依賴它。
        rebuildNetLoadGateIds(*this);

        const std::vector<int> candidates =
            findConstantSimplificationCandidates(*this, type, constValue, inputCount);
        int round = 0;
        for (int gateId : candidates) {
            if (simplifyGateWithConstant(gateId)) ++round;
        }
        count += round;
        if (round == 0) break;
    }

    deferLoadListMaintenance = wasDeferred;
    if (!wasDeferred) {
        rebuildNetLoadGateIds(*this);
    }
    return count;
}

NetlistEditReport Netlist::simplifyGatesWithConstantsWithReport(
    GateType type,
    int constValue,
    int inputCount)
{
    Netlist before = cloneForRollback();

    ConstantSimplificationSummary summary;
    summary.targetGateType = type;
    summary.targetConstValue = constValue;
    summary.targetInputCount = inputCount;

    const bool wasDeferred = deferLoadListMaintenance;
    deferLoadListMaintenance = true;

    // 同一顆 gate 可能在多輪中重複成為候選（例如先減少輸入、再被完全消除）。
    // 記錄用集合去重，避免 candidate/simplified 清單膨脹與重複計數。
    std::unordered_set<int> recordedCandidates;
    std::unordered_set<int> recordedSimplified;
    std::unordered_set<int> recordedSkipped;

    int roundCount = 0;

    // 化簡會製造新的常數輸入，必須跑到 fixpoint；
    // 單輪只能清掉直接接到常數 net 的第一層。
    for (;;) {
        // 每輪重算候選前先修好 load list：defer 模式下 rewriteGateInPlace 與
        // replaceAllLoadsOfNet 都不維護 loadGateIds，下一輪會據此漏改下游。
        rebuildNetLoadGateIds(*this);

        const std::vector<int> candidates =
            findConstantSimplificationCandidates(*this, type, constValue, inputCount);
        if (candidates.empty()) break;

        int round = 0;
        for (int gateId : candidates) {
            const std::string gateName =
                (gateId >= 0 && gateId < (int)gates.size()) ? gates[gateId].instName : "";

            if (recordedCandidates.insert(gateId).second) {
                summary.candidateGateIds.push_back(gateId);
                summary.candidateGateNames.push_back(gateName);
            }

            if (simplifyGateWithConstant(gateId)) {
                ++round;
                // 曾經被跳過、後來成功的 gate 要從 skipped 移到 simplified。
                recordedSkipped.erase(gateId);
                if (recordedSimplified.insert(gateId).second) {
                    summary.simplifiedGateIds.push_back(gateId);
                    summary.simplifiedGateNames.push_back(gateName);
                }
            } else if (!recordedSimplified.count(gateId)) {
                recordedSkipped.insert(gateId);
            }
        }

        ++roundCount;
        if (round == 0) break;
    }

    deferLoadListMaintenance = wasDeferred;
    if (!wasDeferred) {
        rebuildNetLoadGateIds(*this);
    }

    // skipped 清單最後才產生：中途成功的 gate 已經被移出 recordedSkipped。
    for (int gateId : summary.candidateGateIds) {
        if (recordedSkipped.count(gateId) == 0) continue;
        summary.skippedGateIds.push_back(gateId);
        summary.skippedGateNames.push_back(
            (gateId >= 0 && gateId < (int)gates.size()) ? gates[gateId].instName : "");
    }

    summary.candidateCount = summary.candidateGateIds.size();
    summary.simplifiedCount = summary.simplifiedGateIds.size();
    summary.skippedCount = summary.skippedGateIds.size();

    // eliminated = 候選中簡化後不再是 target type 的數量。
    // 不用全域淨差：多輪之間若有新的同型別 gate 產生會被抵消，答案偏小。
    if (type != GateType::UNKNOWN) {
        int eliminated = 0;
        for (int gateId : summary.simplifiedGateIds) {
            if (!isValidGateId(gateId)) { ++eliminated; continue; }
            if (gates[gateId].type != type) ++eliminated;
        }
        summary.eliminatedTargetGateCount = eliminated;
    }

    NetlistEditReport report = finalizeEditReport(
        *this,
        before,
        static_cast<int>(summary.simplifiedCount),
        "simplifyGatesWithConstants",
        NetlistEditOperationKind::Simplification,
        "Constant simplification completed.",
        "Constant simplification failed validation and was rolled back.",
        EquivalenceCheckMethod::LocalRewriteRule,
        "Equivalence certified by local constant-folding Boolean identities.",
        false);

    report.constantSimplification = summary;
    report.changedGateIds = summary.simplifiedGateIds;
    report.changedGateNames = summary.simplifiedGateNames;
    if (summary.skippedCount > 0) {
        report.addWarning(
            "Some matching gates could not be simplified because their structure is unsupported.");
    }

    std::cerr << "[simplifyGatesWithConstants] rounds=" << roundCount
              << " candidates=" << summary.candidateCount
              << " simplified=" << summary.simplifiedCount
              << " skipped=" << summary.skippedCount
              << " eliminated=" << summary.eliminatedTargetGateCount << "\n";

    return report;
}

// 對所有 gate 執行一輪 same-input 化簡，回傳化簡的 gate 數
int Netlist::simplifyAllSameInputGates() {
    int count = 0;
    for (int i = 0; i < (int)gates.size(); i++)
        if (simplifySameInputGate(i)) count++;
    return count;
}

NetlistEditReport Netlist::simplifyAllSameInputGatesWithReport() {
    Netlist before = cloneForRollback();
    int simplified = simplifyAllSameInputGates();
    return finalizeEditReport(
        *this,
        before,
        simplified,
        "simplifyAllSameInputGates",
        NetlistEditOperationKind::Simplification,
        "Same-input simplification completed.",
        "Same-input simplification failed validation and was rolled back.",
        EquivalenceCheckMethod::LocalRewriteRule,
        "Equivalence certified by same-input Boolean identities such as AND(x,x)=x and XOR(x,x)=0.",
        false);
}

// 反覆執行所有 local simplification，直到 fixpoint（沒有任何改變）
// 回傳總共化簡的 gate 數
FixpointResult Netlist::runLocalSimplificationFixpoint(
    const request_time_budget::RequestDeadline* deadline) {
    FixpointResult result;

    auto expired = [&]() { return deadline != nullptr && deadline->expired(); };

    for (;;) {
        if (expired()) { result.timedOut = true; break; }

        int round = 0;

        round += simplifyAllGatesWithConstants();
        if (expired()) { result.changedCount += round; result.timedOut = true; break; }

        round += simplifyAllSameInputGates();
        if (expired()) { result.changedCount += round; result.timedOut = true; break; }

        round += cleanupAllRemovableBuffers();
        if (expired()) { result.changedCount += round; result.timedOut = true; break; }

        round += collapseBackToBackInverters();
        if (expired()) { result.changedCount += round; result.timedOut = true; break; }

        DeadLogicOptions options;
        options.deadline = deadline;
        const DeadLogicSummary dead = removeDeadLogic(options);
        // 只用 gate 數判斷是否收斂：net 回收不會再開啟新的 simplification 機會。
        round += static_cast<int>(dead.removedGateCount);
        if (dead.timedOut) result.timedOut = true;

        result.changedCount += round;
        ++result.roundCount;
        if (round == 0 || result.timedOut) break;
    }
    return result;
}

NetlistEditReport Netlist::runLocalSimplificationFixpointWithReport(
    const request_time_budget::RequestDeadline* deadline) {
    Netlist before = cloneForRollback();

    const FixpointResult fixpoint = runLocalSimplificationFixpoint(deadline);

    NetlistEditReport report = finalizeEditReport(
        *this,
        before,
        fixpoint.changedCount,
        "runLocalSimplificationFixpoint",
        NetlistEditOperationKind::Simplification,
        fixpoint.timedOut
            ? "Local simplification fixpoint stopped at the time limit; the design was not fully simplified."
            : "Local simplification fixpoint completed.",
        "Local simplification fixpoint failed validation and was rolled back.",
        EquivalenceCheckMethod::LocalRewriteRule,
        "Equivalence certified by composing local cleanup and simplification rewrite rules.",
        false);

    CleanupFixpointSummary summary;
    summary.changedCount = static_cast<size_t>(std::max(0, fixpoint.changedCount));
    summary.roundCount   = static_cast<size_t>(std::max(0, fixpoint.roundCount));
    summary.timedOut     = fixpoint.timedOut;
    report.cleanupFixpoint = summary;

    if (fixpoint.timedOut) {
        report.addWarning(
            "Simplification did not converge within the time limit; more opportunities "
            "may remain. Do not claim the design is fully simplified.");
    } else if (report.success && fixpoint.changedCount == 0) {
        report.addWarning("No local simplification opportunities were found.");
    }
    return report;
}

// 反覆執行安全 cleanup pass，直到 fixpoint。
// 適合對應「trim/prune/remove unused or redundant logic」這類高階 prompt。
FixpointResult Netlist::runSafeCleanupFixpoint(
    const request_time_budget::RequestDeadline* deadline) {
    FixpointResult result;

    auto expired = [&]() { return deadline != nullptr && deadline->expired(); };

    for (;;) {
        if (expired()) { result.timedOut = true; break; }

        const FixpointResult local = runLocalSimplificationFixpoint(deadline);
        result.changedCount += local.changedCount;
        if (local.timedOut) { result.timedOut = true; break; }

        int outer = 0;
        outer += static_cast<int>(mergeStructurallyEquivalentGates().mergedGateCount);
        if (expired()) {
            result.changedCount += outer;
            result.timedOut = true;
            break;
        }

        DeadLogicOptions options;
        options.deadline = deadline;
        const DeadLogicSummary dead = removeDeadLogic(options);
        outer += static_cast<int>(dead.removedGateCount);
        if (dead.timedOut) result.timedOut = true;

        result.changedCount += outer;
        ++result.roundCount;
        if (outer == 0 || result.timedOut) break;
    }
    return result;
}

NetlistEditReport Netlist::runSafeCleanupFixpointWithReport(
    const request_time_budget::RequestDeadline* deadline) {
    Netlist before = cloneForRollback();

    const FixpointResult fixpoint = runSafeCleanupFixpoint(deadline);

    NetlistEditReport report = finalizeEditReport(
        *this,
        before,
        fixpoint.changedCount,
        "runSafeCleanupFixpoint",
        NetlistEditOperationKind::Simplification,
        fixpoint.timedOut
            ? "Safe cleanup fixpoint stopped at the time limit; the design was not fully cleaned."
            : "Safe cleanup fixpoint completed.",
        "Safe cleanup fixpoint failed validation and was rolled back.",
        EquivalenceCheckMethod::LocalRewriteRule,
        "Equivalence certified by composing safe local rewrite, structural cleanup, and unreachable-logic removal rules.",
        false);

    CleanupFixpointSummary summary;
    summary.changedCount = static_cast<size_t>(std::max(0, fixpoint.changedCount));
    summary.roundCount   = static_cast<size_t>(std::max(0, fixpoint.roundCount));
    summary.timedOut     = fixpoint.timedOut;
    report.cleanupFixpoint = summary;

    if (fixpoint.timedOut) {
        report.addWarning(
            "Cleanup did not converge within the time limit; more cleanup opportunities "
            "may remain. Do not claim the design is fully cleaned.");
    } else if (report.success && fixpoint.changedCount == 0) {
        report.addWarning("No safe cleanup opportunities were found.");
    }
    return report;
}

// =============================================================================
//  B-8: compactRemovedGatesWithIdMap（P2）
//  【說明】compaction 後回傳 oldToNew / newToOld ID map，讓外部能追蹤 gate ID 變化。
// =============================================================================

Netlist::CompactResult Netlist::compactRemovedGatesWithIdMap() {
    CompactResult result;
    result.removedGateCount = 0;

    std::unordered_set<int> deadSet;
    for (int i = 0; i < (int)gates.size(); i++)
        if (gates[i].type == GateType::UNKNOWN) deadSet.insert(i);

    if (deadSet.empty()) return result;

    for (int i = 0; i < (int)nets.size(); i++) {
        std::vector<int> newLoads;
        for (int lgid : nets[i].loadGateIds)
            if (!deadSet.count(lgid)) newLoads.push_back(lgid);
        nets[i].loadGateIds = newLoads;
    }

    std::vector<Gate> newGates;
    for (int i = 0; i < (int)gates.size(); i++) {
        if (!deadSet.count(i)) {
            int newId = (int)newGates.size();
            result.oldToNewGateId[i] = newId;
            result.newToOldGateId[newId] = i;
            newGates.push_back(gates[i]);
            newGates.back().id = newId;
        }
    }

    for (int i = 0; i < (int)nets.size(); i++) {
        if (nets[i].driverGateId >= 0) {
            auto it = result.oldToNewGateId.find(nets[i].driverGateId);
            nets[i].driverGateId = (it != result.oldToNewGateId.end()) ? it->second : -1;
        }
        for (int& lgid : nets[i].loadGateIds) {
            auto it = result.oldToNewGateId.find(lgid);
            lgid = (it != result.oldToNewGateId.end()) ? it->second : -1;
        }
    }

    gateNameToId.clear();
    for (int i = 0; i < (int)newGates.size(); i++)
        gateNameToId[newGates[i].instName] = i;

    result.removedGateCount = (int)gates.size() - (int)newGates.size();
    gates = newGates;
    markDirty();
    return result;
}

namespace {

// 一條恆常數 net 的處理結果。
enum class TieOutcome { Tied, SkippedPo, Skipped };

using SimulationSignature = BitParallelSimulationSignature;
using SimulationResult = BitParallelSimulationResult;

// 把恆常數 net 的所有 load 改接到常數 net。
// 不移除 driver gate —— 交給稍後的 removeDeadLogic 一次收掉。
TieOutcome tieNetToConstant(Netlist& netlist, int netId, int constNetId) {
    if (!netlist.isValidNetId(netId) || !netlist.isValidNetId(constNetId)) {
        return TieOutcome::Skipped;
    }
    const Net& net = netlist.getNet(netId);
    if (net.isRemoved || net.isConst || net.isPI) return TieOutcome::Skipped;

    // PO net 必須保留 port 名稱與 driver 結構，本層不處理。
    if (net.isPO) return TieOutcome::SkippedPo;

    if (net.loadGateIds.empty()) return TieOutcome::Skipped;
    if (netId == constNetId) return TieOutcome::Skipped;

    return netlist.replaceAllLoadsOfNet(netId, constNetId)
        ? TieOutcome::Tied
        : TieOutcome::Skipped;
}

std::uint64_t splitMix64(std::uint64_t value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

SimulationSignature makeLeafSignature(int netId, size_t wordCount) {
    SimulationSignature signature(wordCount, 0);
    for (size_t word = 0; word < wordCount; ++word) {
        const std::uint64_t seed =
            (static_cast<std::uint64_t>(static_cast<unsigned int>(netId)) << 32) ^
            static_cast<std::uint64_t>(word) ^ 0x6a09e667f3bcc909ULL;
        signature[word] = splitMix64(seed);
    }
    return signature;
}

bool isSupportedSimulationGate(const Gate& gate) {
    switch (gate.type) {
    case GateType::AND:
    case GateType::OR:
    case GateType::NAND:
    case GateType::NOR:
        return !gate.inputNetIds.empty();
    case GateType::NOT:
    case GateType::BUF:
        return gate.inputNetIds.size() == 1;
    case GateType::XOR:
    case GateType::XNOR:
        return gate.inputNetIds.size() == 2;
    default:
        return false;
    }
}

bool evaluateSimulationGate(const Gate& gate,
                            const std::vector<SimulationSignature>& signatures,
                            std::uint64_t lastWordMask,
                            SimulationSignature& output) {
    if (!isSupportedSimulationGate(gate) || gate.outputNetId < 0) {
        return false;
    }

    const size_t wordCount = output.size();
    for (int inputNetId : gate.inputNetIds) {
        if (inputNetId < 0 || static_cast<size_t>(inputNetId) >= signatures.size() ||
            signatures[inputNetId].size() != wordCount) {
            return false;
        }
    }

    for (size_t word = 0; word < wordCount; ++word) {
        std::uint64_t value = 0;
        switch (gate.type) {
        case GateType::AND:
        case GateType::NAND:
            value = ~std::uint64_t{0};
            for (int inputNetId : gate.inputNetIds) value &= signatures[inputNetId][word];
            if (gate.type == GateType::NAND) value = ~value;
            break;
        case GateType::OR:
        case GateType::NOR:
            value = 0;
            for (int inputNetId : gate.inputNetIds) value |= signatures[inputNetId][word];
            if (gate.type == GateType::NOR) value = ~value;
            break;
        case GateType::NOT:
            value = ~signatures[gate.inputNetIds.front()][word];
            break;
        case GateType::BUF:
            value = signatures[gate.inputNetIds.front()][word];
            break;
        case GateType::XOR:
        case GateType::XNOR:
            value = signatures[gate.inputNetIds[0]][word] ^
                    signatures[gate.inputNetIds[1]][word];
            if (gate.type == GateType::XNOR) value = ~value;
            break;
        default:
            return false;
        }
        output[word] = value;
    }

    if (!output.empty()) {
        output.back() &= lastWordMask;
    }
    return true;
}

SimulationResult simulateNetlist(const Netlist& netlist, size_t patternCount) {
    const size_t wordCount = (patternCount + 63) / 64;
    const size_t remainingBits = patternCount % 64;
    const std::uint64_t lastWordMask = remainingBits == 0
        ? ~std::uint64_t{0}
        : ((std::uint64_t{1} << remainingBits) - 1);

    SimulationResult result;
    result.patternCount = patternCount;
    result.lastWordMask = lastWordMask;
    result.signatures.resize(netlist.getNetCount());
    result.known.assign(netlist.getNetCount(), false);

    std::vector<int> unresolvedInputs(netlist.getGateCount(), -1);
    std::vector<std::vector<int>> waitingGates(netlist.getNetCount());
    std::queue<int> readyGates;

    auto assignLeaf = [&](int netId, const SimulationSignature& signature) {
        result.signatures[netId] = signature;
        result.known[netId] = true;
    };

    for (size_t index = 0; index < netlist.getNetCount(); ++index) {
        const int netId = static_cast<int>(index);
        const Net& net = netlist.getNet(netId);
        if (net.isRemoved) {
            continue;
        }
        if (net.isConst) {
            SimulationSignature constant(wordCount, net.constVal == 1 ? ~std::uint64_t{0} : 0);
            if (!constant.empty()) constant.back() &= lastWordMask;
            assignLeaf(netId, constant);
            continue;
        }

        const bool hasDriver = netlist.isValidGateId(net.driverGateId) &&
                               netlist.getGate(net.driverGateId).type != GateType::UNKNOWN;
        const bool dffOutput = hasDriver &&
                               netlist.getGate(net.driverGateId).type == GateType::DFF;
        if (!hasDriver || dffOutput) {
            assignLeaf(netId, makeLeafSignature(netId, wordCount));
        }
    }

    for (size_t index = 0; index < netlist.getGateCount(); ++index) {
        const int gateId = static_cast<int>(index);
        const Gate& gate = netlist.getGate(gateId);
        if (!netlist.isCombinationalGate(gateId) ||
            !isSupportedSimulationGate(gate) ||
            !netlist.isValidNetId(gate.outputNetId) ||
            netlist.getNet(gate.outputNetId).isRemoved) {
            continue;
        }

        int unresolved = 0;
        bool invalidInput = false;
        for (int inputNetId : gate.inputNetIds) {
            if (!netlist.isValidNetId(inputNetId) || netlist.getNet(inputNetId).isRemoved) {
                invalidInput = true;
                break;
            }
            if (!result.known[inputNetId]) {
                ++unresolved;
                waitingGates[inputNetId].push_back(gateId);
            }
        }
        if (invalidInput) {
            continue;
        }
        unresolvedInputs[gateId] = unresolved;
        if (unresolved == 0) {
            readyGates.push(gateId);
        }
    }

    while (!readyGates.empty()) {
        const int gateId = readyGates.front();
        readyGates.pop();
        if (!netlist.isValidGateId(gateId)) {
            continue;
        }

        const Gate& gate = netlist.getGate(gateId);
        if (!netlist.isValidNetId(gate.outputNetId) || result.known[gate.outputNetId]) {
            continue;
        }

        SimulationSignature output(wordCount, 0);
        if (!evaluateSimulationGate(gate, result.signatures, lastWordMask, output)) {
            continue;
        }
        result.signatures[gate.outputNetId] = std::move(output);
        result.known[gate.outputNetId] = true;
        result.topoOrder.push_back(gateId);

        for (int waitingGateId : waitingGates[gate.outputNetId]) {
            if (waitingGateId < 0 ||
                static_cast<size_t>(waitingGateId) >= unresolvedInputs.size() ||
                unresolvedInputs[waitingGateId] <= 0) {
                continue;
            }
            --unresolvedInputs[waitingGateId];
            if (unresolvedInputs[waitingGateId] == 0) {
                readyGates.push(waitingGateId);
            }
        }
    }

    return result;
}

std::vector<SimulationSignature> computeObservabilityMasks(
    const Netlist& netlist,
    const SimulationResult& simulation) {

    const size_t wordCount = simulation.signatures.empty()
        ? 0 : simulation.signatures[0].size();
    const int NN = static_cast<int>(netlist.getNetCount());
    std::vector<SimulationSignature> obs(NN, SimulationSignature(wordCount, 0));
    if (wordCount == 0) return obs;

    auto setObservable = [&](int netId) {
        if (netId < 0 || netId >= NN) return;
        for (size_t w = 0; w < wordCount; ++w) obs[netId][w] = ~std::uint64_t{0};
        obs[netId].back() &= simulation.lastWordMask;
    };

    // 觀測起點：PO net，以及 DFF 的所有輸入 pin（sequential boundary）。
    for (int i = 0; i < NN; ++i) {
        if (!netlist.getNet(i).isRemoved && netlist.getNet(i).isPO) setObservable(i);
    }
    for (size_t g = 0; g < netlist.getGateCount(); ++g) {
        const int gateId = static_cast<int>(g);
        if (netlist.isGateRemoved(gateId)) continue;
        if (netlist.getGate(gateId).type != GateType::DFF) continue;
        for (int inNet : netlist.getGate(gateId).inputNetIds) setObservable(inNet);
    }

    // 反向拓樸：output 的 obs 定案後才能算 fanin 的 obs。
    for (auto it = simulation.topoOrder.rbegin();
         it != simulation.topoOrder.rend(); ++it) {
        const int gateId = *it;
        if (!netlist.isValidGateId(gateId) || netlist.isGateRemoved(gateId)) continue;
        const Gate& gate = netlist.getGate(gateId);
        const int outNet = gate.outputNetId;
        if (!netlist.isValidNetId(outNet)) continue;

        const std::vector<int>& ins = gate.inputNetIds;
        for (size_t i = 0; i < ins.size(); ++i) {
            const int inNet = ins[i];
            if (!netlist.isValidNetId(inNet)) continue;

            for (size_t w = 0; w < wordCount; ++w) {
                std::uint64_t sensitize = ~std::uint64_t{0};
                switch (gate.type) {
                    case GateType::AND:
                    case GateType::NAND:
                        for (size_t j = 0; j < ins.size(); ++j)
                            if (j != i && ins[j] >= 0)
                                sensitize &= simulation.signatures[ins[j]][w];
                        break;
                    case GateType::OR:
                    case GateType::NOR:
                        for (size_t j = 0; j < ins.size(); ++j)
                            if (j != i && ins[j] >= 0)
                                sensitize &= ~simulation.signatures[ins[j]][w];
                        break;
                    case GateType::XOR:
                    case GateType::XNOR:
                    case GateType::NOT:
                    case GateType::BUF:
                        break;                    // 恆傳導
                    default:
                        sensitize = 0;
                        break;
                }
                obs[inNet][w] |= obs[outNet][w] & sensitize;
            }
        }
    }
    for (int i = 0; i < NN; ++i) obs[i].back() &= simulation.lastWordMask;
    return obs;
}

} // namespace

namespace {

struct SinglePathTrace {
    bool ok = false;                  // 是否為無重收斂的單一路徑
    std::vector<int> gateIds;         // 路徑上的 gate（不含起點 driver）
};

// 從 netId 沿 fanout 走。每一步的 net 必須恰好一個 load，
// 且該 load 不是 DFF（DFF 控制腳另外排除），直到抵達 PO 或 DFF 輸入。
SinglePathTrace traceSinglePath(const Netlist& netlist, int netId, size_t maxLength) {
    SinglePathTrace trace;
    int current = netId;

    for (size_t step = 0; step < maxLength; ++step) {
        if (!netlist.isValidNetId(current)) return trace;
        const Net& net = netlist.getNet(current);
        if (net.isRemoved) return trace;

        if (net.isPO) {                       // 抵達觀測點
            trace.ok = !trace.gateIds.empty();
            return trace;
        }
        if (net.loadGateIds.size() != 1) return trace;   // 重收斂或無 load

        const int loadGateId = net.loadGateIds.front();
        if (!netlist.isValidGateId(loadGateId) ||
            netlist.isGateRemoved(loadGateId)) return trace;

        const Gate& load = netlist.getGate(loadGateId);
        if (load.type == GateType::DFF) {     // 抵達 sequential boundary
            trace.ok = !trace.gateIds.empty();
            return trace;
        }
        if (!netlist.isValidNetId(load.outputNetId)) return trace;

        trace.gateIds.push_back(loadGateId);
        current = load.outputNetId;
    }
    return trace;                              // 超過長度上限
}

// 該 net 是否位於任何 DFF 的 CK/RN/SN fanin cone。
// Boolean 等價不代表時序安全，這些 pin 的 cone 一律排除。
bool inDffControlCone(const Netlist& netlist,
                      const std::vector<uint8_t>& controlConeMask,
                      int netId) {
    return netlist.isValidNetId(netId) && controlConeMask[netId] != 0;
}

std::vector<uint8_t> markDffControlCones(const Netlist& netlist) {
    const int NN = static_cast<int>(netlist.getNetCount());
    std::vector<uint8_t> mask(NN, 0);
    std::vector<int> work;

    for (size_t g = 0; g < netlist.getGateCount(); ++g) {
        const int gateId = static_cast<int>(g);
        if (netlist.isGateRemoved(gateId)) continue;
        const Gate& gate = netlist.getGate(gateId);
        if (gate.type != GateType::DFF) continue;
        for (size_t p = 0; p < gate.inputNetIds.size(); ++p) {
            const std::string& pin =
                p < gate.inputPinNames.size() ? gate.inputPinNames[p] : std::string();
            if (pin == "D") continue;                 // D cone 是資料路徑，可處理
            const int netId = gate.inputNetIds[p];
            if (netId >= 0 && !mask[netId]) { mask[netId] = 1; work.push_back(netId); }
        }
    }

    while (!work.empty()) {
        const int netId = work.back();
        work.pop_back();
        const int driverId = netlist.getNet(netId).driverGateId;
        if (!netlist.isValidGateId(driverId) || netlist.isGateRemoved(driverId)) continue;
        const Gate& driver = netlist.getGate(driverId);
        if (driver.type == GateType::DFF) continue;
        for (int inNet : driver.inputNetIds) {
            if (inNet >= 0 && !mask[inNet]) { mask[inNet] = 1; work.push_back(inNet); }
        }
    }
    return mask;
}

// 該 gate 的非控制值（側輸入需為此值，故障才能穿透）。
// 回傳 -1 表示恆傳導（XOR/XNOR/NOT/BUF），不需要側條件。
int nonControllingValue(GateType type) {
    switch (type) {
        case GateType::AND:
        case GateType::NAND: return 1;
        case GateType::OR:
        case GateType::NOR:  return 0;
        default:             return -1;
    }
}

} // namespace

void Netlist::removeConstantRedundancy(
    const RedundancyRemovalOptions& options,
    RedundancyRemovalSummary& summary,
    RedundancyDiagnostics& diagnostics) {

    eqeng::Primitives& primitives = booleanPrimitives();

    // sweep 用整個 request 的 deadline，不自己起算碼表。
    // 截斷時未掃描的節點會讓 fraig_lookup 回 Unknown 而退回 SAT，
    // 結果仍然正確，只是少了查表加速 —— 因此 truncated 不影響完整性。
    eqeng::FraigDeadlineScope guard(primitives, options.deadline);
    primitives.request_fraig_sweep(false);          // simulate-only
    diagnostics.fraigComplete  = primitives.fraig_complete();
    diagnostics.fraigTruncated = primitives.fraig_sweep_truncated();

    const SimulationResult simulation =
        simulateNetlist(*this, options.simulationPatternCount);

    std::vector<std::pair<int, int>> confirmed;   // (netId, value)

    for (size_t index = 0; index < nets.size(); ++index) {
        const int netId = static_cast<int>(index);
        const Net& net = nets[netId];
        if (net.isRemoved || net.isConst || net.isPI || net.isPO) continue;
        if (net.loadGateIds.empty()) continue;
        if (!isValidGateId(net.driverGateId)) continue;
        if (!simulation.known[netId]) continue;

        if (options.deadline && options.deadline->expired()) {
            summary.timedOut = true;
            break;
        }

        // 模擬篩選：任一 word 出現相反值 → 一定不是常數，免 SAT。
        bool allZero = true;
        bool allOne = true;
        const SimulationSignature& signature = simulation.signatures[netId];
        for (size_t w = 0; w < signature.size(); ++w) {
            const std::uint64_t mask = (w + 1 == signature.size())
                ? simulation.lastWordMask : ~std::uint64_t{0};
            const std::uint64_t value = signature[w] & mask;
            if (value != 0) allZero = false;
            if (value != mask) allOne = false;
            if (!allZero && !allOne) break;
        }
        if (!allZero && !allOne) continue;

        ++diagnostics.constantNetCandidateCount;

        // 名字每次重新 resolve —— SigRef 不可跨 rebuild 沿用。
        const std::optional<eqeng::SigRef> signal = primitives.try_resolve(net.name);
        if (!signal || !primitives.is_trustworthy(*signal)) {
            ++diagnostics.constantNetSkippedCount;
            continue;
        }

        const bool targetValue = allOne;
        const eqeng::EquivResult result = primitives.is_const_checked(
            *signal, targetValue, options.perQuerySeconds);
        if (result == eqeng::EquivResult::Unknown) {
            // 「沒證出來」不等於「證明不是常數」。這是 partial 結果，
            // 必須反映在完整性上，否則會漏掉可移除的邏輯卻宣稱搜尋完整。
            ++diagnostics.constantUnknownCount;
            continue;
        }
        if (result != eqeng::EquivResult::Equal) {
            // NotEqual：確定不是常數，這是完整的否定結論。
            ++diagnostics.constantNetSkippedCount;
            continue;
        }
        confirmed.emplace_back(netId, targetValue ? 1 : 0);
    }

    diagnostics.constantPhaseComplete = !summary.timedOut && diagnostics.constantUnknownCount == 0;

    // ---- Apply ----
    // 先全部確認完再一次套用：中途改動 netlist 會 bump revision，
    // 讓所有既有 SigRef 失效並觸發 AIG 重建。
    for (const auto& item : confirmed) {
        const int constNetId = ensureConstantNet(*this, item.second);
        if (!isValidNetId(constNetId)) {
            ++diagnostics.constantNetSkippedCount;
            continue;
        }
        switch (tieNetToConstant(*this, item.first, constNetId)) {
            case TieOutcome::Tied:      ++diagnostics.constantNetTiedCount;      break;
            case TieOutcome::SkippedPo: ++diagnostics.constantNetSkippedPoCount; break;
            case TieOutcome::Skipped:   ++diagnostics.constantNetSkippedCount;   break;
        }
    }

    if (diagnostics.constantNetTiedCount > 0) markDirty();
}

void Netlist::removeSinglePathRedundancy(
    const RedundancyRemovalOptions& options,
    RedundancyRemovalSummary& summary,
    RedundancyDiagnostics& diagnostics) {

    eqeng::Primitives& primitives = booleanPrimitives();

    const SimulationResult simulation =
        simulateNetlist(*this, options.simulationPatternCount);
    const std::vector<SimulationSignature> obs =
        computeObservabilityMasks(*this, simulation);
    const std::vector<uint8_t> controlCone = markDffControlCones(*this);

    constexpr size_t kMaxPathLength = 64;

    struct RedundantPin { int gateId; int pinIndex; bool stuckValue; };
    std::vector<RedundantPin> proven;

    for (size_t g = 0; g < gates.size() && !summary.timedOut; ++g) {
        const int gateId = static_cast<int>(g);
        if (isGateRemoved(gateId)) continue;
        const Gate& gate = gates[gateId];
        if (gate.type == GateType::DFF) continue;

        for (size_t p = 0; p < gate.inputNetIds.size() && !summary.timedOut; ++p) {
            const int netId = gate.inputNetIds[p];
            if (!isValidNetId(netId) || nets[netId].isRemoved) continue;
            if (!simulation.known[netId]) continue;

            // 一根 pin 有 sa0/sa1 兩個 fault。計數單位統一成 fault，
            // 才能和 rejected / examined / aborted 對得起來。
            diagnostics.pinCandidateCount += 2;

            if (controlCone[netId] != 0) {
                diagnostics.pinSkippedDffControl += 2;
                continue;
            }

            for (int stuck = 0; stuck <= 1; ++stuck) {
                if (options.deadline && options.deadline->expired()) {
                    summary.timedOut = true;
                    break;
                }
                if (diagnostics.satChecks >= options.maxSatChecks) {
                    ++diagnostics.satBudgetExhausted;
                    continue;
                }

                // 模擬篩選：obs & (value ⊕ stuck) 非零 ⟹ 一定可測。
                bool provablyTestable = false;
                for (size_t w = 0; w < simulation.signatures[netId].size(); ++w) {
                    const std::uint64_t value = simulation.signatures[netId][w];
                    const std::uint64_t activated = stuck ? ~value : value;
                    if (obs[netId][w] & activated) { provablyTestable = true; break; }
                }
                if (provablyTestable) {
                    ++diagnostics.pinRejectedBySimulation;
                    continue;
                }

                const SinglePathTrace trace =
                    traceSinglePath(*this, gate.outputNetId, kMaxPathLength);
                if (!trace.ok) {
                    ++diagnostics.pinSkippedReconvergent;
                    continue;
                }

                const std::optional<eqeng::SigRef> pinSignal =
                    primitives.try_resolve(nets[netId].name);
                if (!pinSignal || !primitives.is_trustworthy(*pinSignal)) {
                    ++diagnostics.pinSkippedReconvergent;
                    continue;
                }

                // 側條件 miter：(pin ⊕ stuck) ∧ ⋀ side_conditions
                eqeng::SigRef miter = stuck ? !*pinSignal : *pinSignal;
                bool buildOk = true;

                std::vector<int> pathGates;
                pathGates.push_back(gateId);
                pathGates.insert(pathGates.end(),
                                 trace.gateIds.begin(), trace.gateIds.end());

                int prevNetId = netId;
                for (int pathGateId : pathGates) {
                    const Gate& pathGate = gates[pathGateId];
                    const int nonControl = nonControllingValue(pathGate.type);
                    if (nonControl >= 0) {
                        for (int sideNetId : pathGate.inputNetIds) {
                            if (sideNetId == prevNetId) continue;
                            if (!isValidNetId(sideNetId)) { buildOk = false; break; }
                            const std::optional<eqeng::SigRef> side =
                                primitives.try_resolve(nets[sideNetId].name);
                            if (!side || !primitives.is_trustworthy(*side)) {
                                buildOk = false;
                                break;
                            }
                            miter = primitives.make_and(
                                miter, nonControl == 1 ? *side : !*side);
                        }
                    }
                    if (!buildOk) break;
                    prevNetId = pathGate.outputNetId;
                }
                if (!buildOk) {
                    ++diagnostics.pinSkippedReconvergent;
                    continue;
                }

                ++diagnostics.pinExaminedBySat;
                ++diagnostics.satChecks;

                // miter 恆為 0 ⟹ 故障永遠無法激發並傳播 ⟹ redundant。
                const eqeng::EquivResult result =
                    primitives.is_const_checked(miter, false, options.perQuerySeconds);
                if (result == eqeng::EquivResult::Equal) {
                    proven.push_back({gateId, static_cast<int>(p), stuck != 0});
                } else if (result == eqeng::EquivResult::Unknown) {
                    ++diagnostics.abortedCandidateCount;
                }
            }
        }
    }

    diagnostics.satPhaseComplete =
        !summary.timedOut &&
        diagnostics.abortedCandidateCount == 0 &&
        diagnostics.satBudgetExhausted == 0;

    // ---- Apply ----
    // 候選之間不獨立：移除一個 redundancy 可能讓另一個變成必要的。
    // 但 fanout cone 互不相交的候選彼此無法影響，可以安全地一起套用 ——
    // 這比「一次只套一個」實用得多，又不需要重新證明。
    //
    // claimedNet 標記已被套用候選的 fanout cone 覆蓋的 net。
    // seenStamp + stamp 是共用的 visited buffer，避免每個候選都配置一個 vector。
    if (!proven.empty()) {
        std::vector<uint8_t> claimedNet(nets.size(), 0);
        std::vector<uint32_t> seenStamp(nets.size(), 0);
        uint32_t stamp = 0;
        std::vector<int> work;

        // 走 netId 的 transitive fanout，對每個 net 呼叫 visit。
        // DFF 是 sequential boundary，不穿透。
        auto walkFanout = [&](int startNetId, const auto& visit) {
            ++stamp;
            work.clear();
            work.push_back(startNetId);
            while (!work.empty()) {
                const int netId = work.back();
                work.pop_back();
                if (!isValidNetId(netId)) continue;
                if (seenStamp[netId] == stamp) continue;
                seenStamp[netId] = stamp;
                if (!visit(netId)) return false;
                for (int loadId : nets[netId].loadGateIds) {
                    if (!isValidGateId(loadId) || isGateRemoved(loadId)) continue;
                    if (gates[loadId].type == GateType::DFF) continue;
                    const int outNet = gates[loadId].outputNetId;
                    if (isValidNetId(outNet)) work.push_back(outNet);
                }
            }
            return true;
        };

        for (const RedundantPin& pin : proven) {
            if (!isValidGateId(pin.gateId) || isGateRemoved(pin.gateId)) continue;
            const int outNet = gates[pin.gateId].outputNetId;
            if (!isValidNetId(outNet)) continue;

            // 先探測：這個候選的 fanout cone 是否碰到已套用的區域。
            const bool free = walkFanout(outNet, [&](int netId) {
                return claimedNet[netId] == 0;
            });
            if (!free) {
                ++diagnostics.deferredOverlapCount;
                continue;
            }

            const int constNetId = ensureConstantNet(*this, pin.stuckValue ? 1 : 0);
            if (!isValidNetId(constNetId)) continue;
            // ensureConstantNet 可能 addNet 讓 nets 重新配置，
            // 兩個以 nets.size() 為長度的 bitmap 必須同步擴充。
            if (claimedNet.size() < nets.size()) {
                claimedNet.resize(nets.size(), 0);
                seenStamp.resize(nets.size(), 0);
            }

            const int oldNetId = gates[pin.gateId].inputNetIds[pin.pinIndex];
            gates[pin.gateId].inputNetIds[pin.pinIndex] = constNetId;
            if (isValidNetId(oldNetId)) {
                auto& loads = nets[oldNetId].loadGateIds;
                loads.erase(std::remove(loads.begin(), loads.end(), pin.gateId),
                            loads.end());
            }
            nets[constNetId].loadGateIds.push_back(pin.gateId);
            ++summary.provenRedundantPinCount;

            // 佔用這個候選的 fanout cone，之後重疊的候選一律延後。
            walkFanout(outNet, [&](int netId) {
                claimedNet[netId] = 1;
                return true;
            });
        }

        if (diagnostics.deferredOverlapCount > 0) {
            diagnostics.satPhaseComplete = false;
        }
        if (summary.provenRedundantPinCount > 0) markDirty();
    }
}

NetlistEditReport Netlist::removeRedundantLogicWithReport(
    const RedundancyRemovalOptions& options) {

    const auto startedAt = std::chrono::steady_clock::now();
    Netlist before = cloneForRollback();
    const NetlistStats startStats = before.collectNetlistStats();
    const size_t gatesAtStart = startStats.activeGateCount;
    const size_t netsAtStart  = startStats.activeNetCount;

    RedundancyRemovalSummary summary;
    RedundancyDiagnostics diagnostics;

    // 總預算：deadline 的 budgetSeconds() 是這個 request 一開始的額度，
    // 不隨時間遞減，適合當比例的分母。
    const double totalBudget =
        (options.deadline != nullptr && options.deadline->valid())
            ? options.deadline->budgetSeconds()
            : options.assumedBudgetSeconds;

    auto hasBudget = [&](double fraction, double floorSeconds) {
        if (options.deadline == nullptr) return true;
        const double need = std::max(floorSeconds, totalBudget * fraction);
        return options.deadline->remainingSeconds() > need;
    };

    // 每個 SAT 階段之後都要再化簡一次：常數傳播、merge 與 pin tie-off
    // 都會製造新的化簡機會（常數輸入、same-input gate、失去 load 的邏輯）。
    auto runCleanup = [&]() {
        if (!hasBudget(options.minStageFraction, options.minStageFloorSeconds)) return;
        const FixpointResult result = runSafeCleanupFixpoint(options.deadline);
        diagnostics.structuralChangedCount += result.changedCount;
        if (result.timedOut) summary.timedOut = true;
    };

    // ---- 階段 1：結構化簡到 fixpoint ----
    // 涵蓋 constant folding、same/complementary input、buffer/inverter 清理、
    // structural duplicate merge 與 dead logic。這是 gate 縮減的主力，
    // 也讓後續昂貴的 SAT 階段面對已經縮小過的電路。
    runCleanup();

    // ---- 階段 2：SAT-proven 常數傳播 ----
    if (!summary.timedOut &&
        hasBudget(options.minSatStageFraction, options.minSatStageFloorSeconds)) {
        removeConstantRedundancy(options, summary, diagnostics);
        if (diagnostics.constantNetTiedCount > 0) runCleanup();
    } else if (!summary.timedOut) {
        diagnostics.constantPhaseComplete = false;
    }

    // ---- 階段 3：SAT-proven functional merge ----
    // 它內部有 whole-design SAT 與自己的 rollback；失敗只會退回本階段開始時的
    // 狀態，不影響前面已完成的縮減。成本最高，所以門檻也最高。
    if (!summary.timedOut &&
        hasBudget(options.minFunctionalMergeFraction,
                  options.minFunctionalMergeFloorSeconds)) {
        // 分一半剩餘時間給它，另一半留給後面的 stuck-at 與收尾。
        const double mergeBudget = options.deadline
            ? options.deadline->remainingSeconds() * 0.5
            : totalBudget * 0.3;
        const NetlistEditReport mergeReport = mergeFunctionallyEquivalentGatesWithReport(
            TargetScope::WHOLE_NETLIST,
            std::string(),
            GateType::UNKNOWN,
            options.simulationPatternCount,
            mergeBudget);
        if (mergeReport.success && mergeReport.functionalMerge) {
            diagnostics.mergedFunctionalCount =
                mergeReport.functionalMerge->mergedGateCount;
        }
        if (diagnostics.mergedFunctionalCount > 0) runCleanup();
    } else {
        diagnostics.functionalMergeSkipped = true;
    }

    // ---- 階段 4：單路徑 untestable stuck-at ----
    // 放在所有 merge 之後：它的單路徑判定依賴 fanout 結構，
    // 而 merge 會改變 fanout；此時的電路也已經是最小的，候選最少。
    if (!summary.timedOut &&
        hasBudget(options.minSatStageFraction, options.minSatStageFloorSeconds)) {
        removeSinglePathRedundancy(options, summary, diagnostics);
        if (summary.provenRedundantPinCount > 0) runCleanup();
    } else if (!summary.timedOut) {
        diagnostics.satPhaseComplete = false;
    }

    // ---- 階段 5：收尾 ----
    DeadLogicOptions deadLogicOptions;
    deadLogicOptions.deadline = options.deadline;
    const DeadLogicSummary dead = removeDeadLogic(deadLogicOptions);
    if (dead.timedOut) summary.timedOut = true;
    removeUnusedNets();

    // ---- 統計 ----
    // 多個階段都會移除 gate，不能只讀某一個 pass 的數字；
    // 用 before/after 的 active count 差才是這次操作的總縮減量。
    const NetlistStats afterStats = collectNetlistStats();
    summary.removedGateCount = gatesAtStart > afterStats.activeGateCount
        ? gatesAtStart - afterStats.activeGateCount : 0;
    summary.removedNetCount = netsAtStart > afterStats.activeNetCount
        ? netsAtStart - afterStats.activeNetCount : 0;

    summary.complete =
        diagnostics.constantPhaseComplete &&
        diagnostics.satPhaseComplete &&
        !diagnostics.functionalMergeSkipped &&
        !summary.timedOut;
    summary.elapsedSeconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - startedAt).count();

    NetlistEditReport report = finalizeEditReport(
        *this, before,
        static_cast<int>(summary.removedGateCount),
        "removeRedundantLogic",
        NetlistEditOperationKind::Simplification,
        "Redundant logic removal completed.",
        "Redundant logic removal failed validation and was rolled back.",
        EquivalenceCheckMethod::CertifiedRewrite,
        "Equivalence certified by composing structural simplification, SAT-proven "
        "constant propagation, SAT-proven duplicate merging, untestable stuck-at "
        "fault removal, and unreachable-logic cleanup.");

    report.redundancyRemoval = summary;

    if (!summary.complete) {
        report.addWarning(
            "Redundancy removal did not run every stage within the time budget. "
            "removed_gate_count is what was achieved, not the total possible.");
    }
    if (diagnostics.constantNetSkippedPoCount > 0) {
        report.addWarning(
            "Constant primary-output nets were left unchanged to preserve port structure.");
    }

    std::cerr << "[removeRedundantLogic]"
              << " structural=" << diagnostics.structuralChangedCount
              << " const_candidates=" << diagnostics.constantNetCandidateCount
              << " const_tied=" << diagnostics.constantNetTiedCount
              << " const_unknown=" << diagnostics.constantUnknownCount
              << " func_merged=" << diagnostics.mergedFunctionalCount
              << " func_skipped=" << (diagnostics.functionalMergeSkipped ? "yes" : "no")
              << " pin_candidates=" << diagnostics.pinCandidateCount
              << " sat_checks=" << diagnostics.satChecks
              << " proven_pins=" << summary.provenRedundantPinCount
              << " dead_removed=" << dead.removedGateCount
              << " total_removed=" << summary.removedGateCount
              << " elapsed=" << summary.elapsedSeconds << "\n";

    return report;
}

DffMergeSummary Netlist::mergeDuplicateDffs() {
    DffMergeSummary summary;

    std::unordered_map<DffPinKey, int, DffPinKeyHash> canonicalByKey;
    std::unordered_set<size_t> mergedKeyHashes;
    const DffPinKeyHash hasher;

    for (int gateId = 0; gateId < static_cast<int>(gates.size()); ++gateId) {
        if (!isSafeDffMergeCandidate(*this, gateId)) continue;

        const DffPinKey key = makeDffPinKey(*this, gateId);
        if (key.hasUnknownPin) continue;   // 無法安全比對的 pin 組合

        auto canonicalIt = canonicalByKey.find(key);
        if (canonicalIt == canonicalByKey.end()) {
            canonicalByKey.emplace(key, gateId);
            continue;
        }

        int keepGateId = canonicalIt->second;
        if (!isSafeDffMergeCandidate(*this, keepGateId) ||
            !(makeDffPinKey(*this, keepGateId) == key)) {
            canonicalIt->second = gateId;
            continue;
        }
        if (keepGateId == gateId) continue;

        int deadGateId = gateId;
        int keepOutNet = gates[keepGateId].outputNetId;
        int deadOutNet = gates[deadGateId].outputNetId;

        // 兩顆 Q 都是 PO：合併會讓兩個 port 指向同一條 net，破壞 port 對應。
        const bool keepIsPo = nets[keepOutNet].isPO;
        const bool deadIsPo = nets[deadOutNet].isPO;
        if (keepIsPo && deadIsPo) {
            ++summary.skippedPoCount;
            continue;
        }
        if (deadIsPo) {
            std::swap(keepGateId, deadGateId);
            std::swap(keepOutNet, deadOutNet);
            canonicalIt->second = keepGateId;
        }
        if (keepOutNet == deadOutNet) continue;

        // 任一顆的 Q 出現在共用的 D cone 裡時，合併可能把時序回授
        // 折成組合迴路。D 未接（-1）時沒有 cone 可查，直接視為安全。
        if (key.d >= 0 &&
            (outputDependsOnNet(*this, key.d, keepOutNet) ||
             outputDependsOnNet(*this, key.d, deadOutNet))) {
            ++summary.skippedFeedbackCount;
            continue;
        }

        std::vector<int> affectedGateIds;
        if (!collectActiveStructuralLoads(*this, deadOutNet, affectedGateIds) ||
            !replaceAllLoadsOfNet(deadOutNet, keepOutNet)) {
            continue;
        }

        nets[deadOutNet].driverGateId = -1;
        gates[deadGateId].type = GateType::UNKNOWN;
        gates[deadGateId].outputNetId = -1;
        gates[deadGateId].inputNetIds.clear();
        gates[deadGateId].inputPinNames.clear();
        ++summary.mergedDffCount;
        mergedKeyHashes.insert(hasher(key));
    }

    summary.equivalenceClassCount = mergedKeyHashes.size();

    if (summary.mergedDffCount > 0) {
        rebuildNetLoadGateIds(*this);
        markDirty();
    }
    return summary;
}

DffMergeSummary Netlist::mergeDuplicateDffsToFixpoint() {
    DffMergeSummary total;
    for (;;) {
        const DffMergeSummary round = mergeDuplicateDffs();
        total.mergedDffCount        += round.mergedDffCount;
        total.equivalenceClassCount += round.equivalenceClassCount;
        total.skippedPoCount         = round.skippedPoCount;
        total.skippedFeedbackCount   = round.skippedFeedbackCount;
        if (round.mergedDffCount == 0) break;
    }
    return total;
}

NetlistEditReport Netlist::mergeDuplicateDffsWithReport() {
    Netlist before = cloneForRollback();
    const DffMergeSummary summary = mergeDuplicateDffsToFixpoint();

    NetlistEditReport report = finalizeEditReport(
        *this,
        before,
        static_cast<int>(summary.mergedDffCount),
        "mergeDuplicateDffs",
        NetlistEditOperationKind::Simplification,
        "Duplicate flip-flop merge completed.",
        "Duplicate flip-flop merge failed validation and was rolled back.",
        EquivalenceCheckMethod::StructuralIdentity,
        "Equivalence certified because merged registers share identical D, CK, RN, and SN connections.",
        false);

    report.dffMerge = summary;
    if (report.success && summary.mergedDffCount == 0) {
        report.addWarning("No duplicate flip-flop was found.");
    }
    if (summary.skippedPoCount > 0) {
        report.addWarning(
            "Some duplicate register pairs were skipped because both Q outputs are "
            "primary outputs and merging would collapse two ports onto one net.");
    }
    return report;
}
