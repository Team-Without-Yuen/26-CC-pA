#include "include/core/MockturtleConverter.h"
#include <unordered_map>
#include <unordered_set>
#include <iostream>
#include <vector>
#include <string>

namespace {

// 內部輔助函式：針對 Netlist 中的組合邏輯閘進行拓撲排序
// 採用 Kahn's Algorithm (基於入度計算)，避免遞迴造成的 Stack Overflow
std::vector<int> ComputeTopologicalOrder(const Netlist& nl) {
    const int netCount  = (int)nl.getNetCount();
    const int gateCount = (int)nl.getGateCount();

    std::vector<int> topoOrder;
    topoOrder.reserve(gateCount);
    std::vector<bool> netVisited(netCount, false);

    // 1~3. 標記常數 / PI / DFF-Q 為已就緒 (sources)
    for (int i = 0; i < netCount; ++i)
        if (nl.getNet(i).isConst) netVisited[i] = true;

    for (const auto& port : nl.getPrimaryInputs())
        for (int netId : port.netIds)
            if (netId >= 0 && netId < netCount) netVisited[netId] = true;

    for (int i = 0; i < gateCount; ++i) {
        const auto& gate = nl.getGate(i);
        if (gate.type == GateType::DFF &&
            gate.outputNetId >= 0 && gate.outputNetId < netCount)
            netVisited[gate.outputNetId] = true;
    }

    // 4. 入度 = 「相異」且尚未就緒的 input net 數
    //    以相異 net 計數，不再受 loadGateIds 是否去重影響 (tied-input 安全)
    std::vector<int> inDegree(gateCount, 0);
    std::vector<int> queue;
    queue.reserve(gateCount);

    for (int g = 0; g < gateCount; ++g) {
        const auto& gate = nl.getGate(g);
        if (gate.type == GateType::DFF || gate.type == GateType::UNKNOWN) continue;

        std::unordered_set<int> pending;                 // 去重
        for (int netId : gate.inputNetIds)
            if (netId >= 0 && netId < netCount && !netVisited[netId])
                pending.insert(netId);

        inDegree[g] = (int)pending.size();
        if (inDegree[g] == 0) queue.push_back(g);
    }

    // 5. Kahn 主迴圈:同一條 net 對同一顆 gate 只 decrement 一次
    size_t head = 0;
    while (head < queue.size()) {
        int gateId = queue[head++];
        topoOrder.push_back(gateId);

        int outNetId = nl.getGate(gateId).outputNetId;
        if (outNetId < 0 || outNetId >= netCount) continue;
        if (netVisited[outNetId]) continue;              // 防呆:避免重複釋放
        netVisited[outNetId] = true;

        std::unordered_set<int> firedGate;               // 「這條 net」內對 gate 去重
        for (int nextGateId : nl.getNet(outNetId).loadGateIds) {
            if (nextGateId < 0 || nextGateId >= gateCount) continue;
            if (!firedGate.insert(nextGateId).second) continue;   // tied-input:已扣過
            const GateType nextType = nl.getGate(nextGateId).type;
            if (nextType == GateType::DFF || nextType == GateType::UNKNOWN) continue;
            if (--inDegree[nextGateId] == 0) queue.push_back(nextGateId);
        }
    }

    // 診斷:偵測未能排序的組合閘 (輸入懸空 / 組合迴路)
    int combCount = 0;
    for (int g = 0; g < gateCount; ++g)
        if (nl.getGate(g).type != GateType::DFF &&
            nl.getGate(g).type != GateType::UNKNOWN) {
            ++combCount;
        }
    if ((int)topoOrder.size() != combCount)
        std::cerr << "[TopoSort][WARN] " << (combCount - (int)topoOrder.size())
                  << " combinational gate(s) unresolved "
                     "(undriven input or combinational loop).\n";

    return topoOrder;
}

// 泛型轉換核心邏輯
// Ntk Mockturtle 的網路型態 (mockturtle::xag_network 或 mockturtle::aig_network)
template<typename Ntk>
Ntk DoNetlistConversion(const Netlist& nl) {
    Ntk ntk;
    using signal_t = typename Ntk::signal;
    
    // 建立 Netlist Net ID 到 Mockturtle 內部 Signal 的對應表
    std::unordered_map<int, signal_t> netToSignal;

    // 1. 處理常數 (Constants)
    for (size_t i = 0; i < nl.getNetCount(); ++i) {
        const auto& net = nl.getNet(i);
        if (net.isConst) {
            if (net.constVal == 0) {
                netToSignal[net.id] = ntk.get_constant(false);
            } else if (net.constVal == 1) {
                netToSignal[net.id] = ntk.get_constant(true);
            }
        }
    }

    // 2. 建立真實的 Primary Inputs (PI)
    // 嚴格依照 getPrimaryInputs 的宣告順序建立，此順序對日後反向轉回至關重要！
    for (const auto& port : nl.getPrimaryInputs()) {
        for (int netId : port.netIds) {
            if (netId >= 0) {
                netToSignal[netId] = ntk.create_pi();
            }
        }
    }

    // 3. 建立 Pseudo-PIs (將所有 DFF 的輸出端 Q 視為組合邏輯的輸入)
    std::vector<int> dffGateIds;
    for (size_t i = 0; i < nl.getGateCount(); ++i) {
        const auto& gate = nl.getGate(i);
        if (gate.type == GateType::DFF) {
            dffGateIds.push_back(gate.id);
            if (gate.outputNetId >= 0) {
                netToSignal[gate.outputNetId] = ntk.create_pi();
            }
        }
    }

    // 安全查表:net 未建立 signal 時明確警告並退回 const0，取代靜默 operator[]
    auto sigOf = [&](const Gate& gate, int idx) -> signal_t {
        if (idx >= (int)gate.inputNetIds.size()) return ntk.get_constant(false);
        int netId = gate.inputNetIds[idx];
        if (netId < 0) return ntk.get_constant(false);           // 明確懸空

        auto it = netToSignal.find(netId);
        if (it == netToSignal.end()) {
            std::cerr << "[NetlistToNtk][WARN] gate '" << gate.instName
                      << "' input net " << netId
                      << " unresolved; tying to const0.\n";
            return ntk.get_constant(false);
        }
        return it->second;
    };

    // 4. 依照拓撲順序建立組合邏輯閘
    std::vector<int> topoOrder = ComputeTopologicalOrder(nl);
    for (int gateId : topoOrder) {
        const auto& gate = nl.getGate(gateId);
        
        // 安全地獲取 2-input 訊號；若未連接線路則預設接至常數 0
        signal_t a = sigOf(gate, 0);
        signal_t b = sigOf(gate, 1);

        signal_t outSig;
        // Mockturtle 具備統一介面，即使是 AIG，呼叫 create_xor 亦會自動在底層拆解為 AND+Inverter
        switch (gate.type) {
            case GateType::AND:  outSig = ntk.create_and(a, b); break;
            case GateType::OR:   outSig = ntk.create_or(a, b); break;
            case GateType::NAND: outSig = ntk.create_not(ntk.create_and(a, b)); break;
            case GateType::NOR:  outSig = ntk.create_not(ntk.create_or(a, b)); break;
            case GateType::XOR:  outSig = ntk.create_xor(a, b); break;
            case GateType::XNOR: outSig = ntk.create_not(ntk.create_xor(a, b)); break;
            case GateType::NOT:  outSig = ntk.create_not(a); break;
            case GateType::BUF:  outSig = a; break;
            default:             outSig = ntk.get_constant(false); break;
        }

        if (gate.outputNetId >= 0) {
            netToSignal[gate.outputNetId] = outSig;
        }
    }

    // 5. 建立真實的 Primary Outputs (PO)
    // 嚴格維持 getPrimaryOutputs 的宣告順序
    for (const auto& port : nl.getPrimaryOutputs()) {
        for (int netId : port.netIds) {
            if (netId >= 0) {
                signal_t sig = netToSignal.count(netId) ? netToSignal[netId] : ntk.get_constant(false);
                ntk.create_po(sig);
            }
        }
    }

    // 6. 建立 Pseudo-POs (將所有 DFF 的輸入端 D 視為組合邏輯的終點)
    // 這裡拜訪 DFF 的順序必須跟步驟 3 完全一致！
    for (int gateId : dffGateIds) {
        const auto& gate = nl.getGate(gateId);
        // 假設 inputNetIds[0] 固定存放 DFF 的 D pin 輸入
        int dNetId = -1;
        for (size_t k = 0; k < gate.inputPinNames.size(); ++k) {
            if (gate.inputPinNames[k] == "D") {
                dNetId = gate.inputNetIds[k];
                break;
            }
        }

        if (dNetId != -1) {
            signal_t sig = netToSignal.count(dNetId) ? netToSignal[dNetId] : ntk.get_constant(false);
            ntk.create_po(sig);
        } else {
            ntk.create_po(ntk.get_constant(false)); // 萬一沒有 D pin 的防呆
        }
    }

    return ntk;
}

} // namespace

mockturtle::xag_network NetlistToXag(const Netlist& nl) {
    return DoNetlistConversion<mockturtle::xag_network>(nl);
}

mockturtle::aig_network NetlistToAig(const Netlist& nl) {
    return DoNetlistConversion<mockturtle::aig_network>(nl);
}

namespace {

// 輔助函數：用來產生內部轉換時的流水號名稱
std::string getNewGateName(int id) { return "opt_g" + std::to_string(id); }
std::string getNewNetName(int id) { return "opt_n" + std::to_string(id); }

// 泛型轉換核心邏輯：將 Mockturtle 網路轉回 Netlist
template<typename Ntk>
Netlist DoMockturtleToNetlist(const Ntk& ntk, const Netlist& old_nl) {
    Netlist new_nl;
    
    // 記錄 Mockturtle 的 Node Index 對應到新 Netlist 的哪一條 Net ID
    std::vector<int> nodeToNet(ntk.size(), -1); 

    // 用來記錄舊 PI/常數 Net ID 對應到新 Net ID 的查表
    std::unordered_map<int, int> oldNetToNewNet;

    // --- Step 0: 建立常數 Net ---
    int const0_net = new_nl.addNet("1'b0");
    new_nl.setNetConst(const0_net, true, 0);
    nodeToNet[ntk.get_node(ntk.get_constant(false))] = const0_net;
    int const1_net = new_nl.addNet("1'b1"); 
    new_nl.setNetConst(const1_net, true, 1);

    for (size_t i = 0; i < old_nl.getNetCount(); ++i) {
        if (old_nl.getNet(i).isConst) {
            if (old_nl.getNet(i).constVal == 0) oldNetToNewNet[i] = const0_net;
            if (old_nl.getNet(i).constVal == 1) oldNetToNewNet[i] = const1_net;
        }
    }

    // --- Step 1: 複製 PI 與 DFF (對齊原本的介面) ---
    int pi_index = 0;
    
    // 1a. 重建真實 Primary Inputs
    for (const auto& port : old_nl.getPrimaryInputs()) {
        new_nl.addPrimaryInput(port.name, port.msb, port.lsb);
        const auto& new_port = new_nl.getPrimaryInputs().back();
        
        for (size_t i = 0; i < port.netIds.size(); ++i) {
            int old_net_id = port.netIds[i];
            int new_net_id = new_port.netIds[i];
            
            oldNetToNewNet[old_net_id] = new_net_id; // 記錄對應關係
            
            auto pi_node = ntk.pi_at(pi_index++);
            nodeToNet[pi_node] = new_net_id; 
        }
    }

    // 1b. 預先重建真實 PO，並保留所有 PO 的 Net ID
    std::vector<std::pair<int, int>> po_net_mappings; // <old_net_id, new_net_id>
    for (const auto& old_port : old_nl.getPrimaryOutputs()) {
        new_nl.addPrimaryOutput(old_port.name, old_port.msb, old_port.lsb);
        const auto& new_port = new_nl.getPrimaryOutputs().back();
        
        // 將舊的 Net ID 與新建立的 Net ID 一一配對
        for (size_t i = 0; i < old_port.netIds.size(); ++i) {
            po_net_mappings.push_back({old_port.netIds[i], new_port.netIds[i]});
        }
    }

    // 計算「真實 PI」的總比特數
    int num_real_pis = 0;
    for (const auto& port : old_nl.getPrimaryInputs()) {
        num_real_pis += port.netIds.size();
    }

    // 1c. 建立「內部節點 -> PO Net ID」的直通查表
    std::unordered_map<uint64_t, int> nodeToDirectPoNet;
    int temp_po_index = 0;
    for (const auto& mapping :po_net_mappings) {
        int po_net_id = mapping.second; 
        auto po_signal = ntk.po_at(temp_po_index++);
        
        // 只有「非反相」的訊號才允許直通
        if (!ntk.is_complemented(po_signal)) {
            auto node = ntk.get_node(po_signal);
            
            // 判斷這個節點是不是「真實的 PI」
            bool is_real_pi = ntk.is_pi(node) && (ntk.pi_index(node) < num_real_pis);

            // 只要不是「真實 PI」且不是「常數」，就允許直通！
            if (!is_real_pi && !ntk.is_constant(node) && nodeToDirectPoNet.count(node) == 0) {
                nodeToDirectPoNet[node] = po_net_id;
            }
        }
    }

    // 1d. 重建 DFF 與 DFF 的輸出 (Q)
    std::vector<int> new_dff_ids;
    for (size_t i = 0; i < old_nl.getGateCount(); ++i) {
        const auto& old_gate = old_nl.getGate(i);
        if (old_gate.type == GateType::DFF) {
            int new_dff_id = new_nl.addGate(old_gate.instName, GateType::DFF);
            new_dff_ids.push_back(new_dff_id);
            
            // 接回 CK, RN, SN 等控制訊號
            for (size_t p = 0; p < old_gate.inputPinNames.size(); ++p) {
                if (old_gate.inputPinNames[p] != "D") {
                    int old_net = old_gate.inputNetIds[p];
                    int new_net = oldNetToNewNet.count(old_net) ? oldNetToNewNet[old_net] : -1;
                    if (new_net != -1) {
                        new_nl.connectGateInput(new_dff_id, new_net, old_gate.inputPinNames[p]);
                    }
                }
            }

            auto pseudo_pi_node = ntk.pi_at(pi_index++); 
            int dff_q_net;

            // 判斷這個 DFF 的 Q 是否直通某個 PO
            if (nodeToDirectPoNet.count(pseudo_pi_node)) {
                dff_q_net = nodeToDirectPoNet[pseudo_pi_node]; // 直接拿 PO 的線當輸出！
            } else {
                dff_q_net = new_nl.addNet(old_gate.instName + "_Q");
            }
            
            new_nl.connectGateOutput(new_dff_id, dff_q_net);
            nodeToNet[pseudo_pi_node] = dff_q_net;
        }
    }

    // 安全查表:node 尚未對應到 net 時警告並退回 const0
    auto netOfNode = [&](auto node) -> int {
        int netId = nodeToNet[node];
        if (netId < 0) {
            /*std::cerr << "[NtkToNetlist][WARN] node " << (uint64_t)node
                      << " referenced before assignment; using const0.\n";*/
            return const0_net;
        }
        return netId;
    };

    // Step 2: 遍歷 Mockturtle 內部邏輯並重建 Gates 
    int gate_counter = 0;
    int net_counter = 0;

    ntk.foreach_gate([&](auto n) {
        std::vector<typename Ntk::signal> fanins;
        ntk.foreach_fanin(n, [&](auto f) { fanins.push_back(f); });

        int input_nets[2];
        for (int i = 0; i < 2; ++i) {
            int base_net_id = netOfNode(ntk.get_node(fanins[i]));   // 前面已改的安全查表
            if (ntk.is_complemented(fanins[i])) {
                int not_gate_id = new_nl.addGate(getNewGateName(gate_counter++), GateType::NOT);
                int not_out_net = new_nl.addNet(getNewNetName(net_counter++));
                new_nl.connectGateInput(not_gate_id, base_net_id);
                new_nl.connectGateOutput(not_gate_id, not_out_net);
                input_nets[i] = not_out_net;
            } else {
                input_nets[i] = base_net_id;
            }
        }

        GateType type = ntk.is_and(n) ? GateType::AND : GateType::XOR;
        int new_gate_id = new_nl.addGate(getNewGateName(gate_counter++), type);
        
        int out_net_id;
        // 判斷這個組合邏輯閘是否直通某個 PO
        if (nodeToDirectPoNet.count(n)) {
            out_net_id = nodeToDirectPoNet[n]; // 直接拿 PO 的線當輸出！
        } else {
            out_net_id = new_nl.addNet(getNewNetName(net_counter++));
        }
        
        new_nl.connectGateInput(new_gate_id, input_nets[0]);
        new_nl.connectGateInput(new_gate_id, input_nets[1]);
        new_nl.connectGateOutput(new_gate_id, out_net_id);

        nodeToNet[n] = out_net_id;
    });

    // Step 3: 處理 PO 與 DFF 輸入
    int po_index = 0;

    // 3a. 處理真實 Primary Outputs
    for (const auto& mapping : po_net_mappings) {
        int old_net_id = mapping.first;       // 用來查舊電路
        int external_net_id = mapping.second; // 用來建新電路
        auto po_signal = ntk.po_at(po_index++);
        
        // 去原始電路查這條 PO 網線的底細
        const Net& old_po_net = old_nl.getNet(old_net_id);
        
        // 懸空偵測
        if (old_po_net.driverGateId == -1 && !old_po_net.isPI) {
            continue; 
        }

        int internal_drive_net = netOfNode(ntk.get_node(po_signal));

        if (ntk.is_complemented(po_signal)) {
            // 反相的情況，使用 NOT Gate 橋接
            int not_gate_id = new_nl.addGate(getNewGateName(gate_counter++), GateType::NOT);
            new_nl.connectGateInput(not_gate_id, internal_drive_net);
            new_nl.connectGateOutput(not_gate_id, external_net_id);
        } else {
            // 只有當內部線與外部線「不同」時，才需要安插邏輯
            if (internal_drive_net != external_net_id) {
                int buf_gate_id = new_nl.addGate(getNewGateName(gate_counter++), GateType::BUF);
                new_nl.connectGateInput(buf_gate_id, internal_drive_net);
                new_nl.connectGateOutput(buf_gate_id, external_net_id);
            }
        }
    }

    // 3b. 處理 DFF 輸入端
    for (int new_dff_id : new_dff_ids) {
        auto pseudo_po_signal = ntk.po_at(po_index++);
        int internal_drive_net = netOfNode(ntk.get_node(pseudo_po_signal));

        if (ntk.is_complemented(pseudo_po_signal)) {
            int not_gate_id = new_nl.addGate(getNewGateName(gate_counter++), GateType::NOT);
            int final_d_net_id = new_nl.addNet(getNewNetName(net_counter++));
            new_nl.connectGateInput(not_gate_id, internal_drive_net);
            new_nl.connectGateOutput(not_gate_id, final_d_net_id);
            new_nl.connectGateInput(new_dff_id, final_d_net_id, "D");
        } else {
            new_nl.connectGateInput(new_dff_id, internal_drive_net, "D");
        }
    }

    // === NOT dedup pass ===
    // 把「輸入同一條 net」的多顆 NOT 合併成一顆：
    //   保留每條來源 net 的第一顆 NOT，其餘 NOT 的 fanout 全部改接到那顆
    mergeDuplicateInverters(new_nl);

    return new_nl;
}

} // namespace

Netlist XagToNetlist(const mockturtle::xag_network& xag, const Netlist& old_nl) {
    return DoMockturtleToNetlist<mockturtle::xag_network>(xag, old_nl);
}

Netlist AigToNetlist(const mockturtle::aig_network& aig, const Netlist& old_nl) {
    return DoMockturtleToNetlist<mockturtle::aig_network>(aig, old_nl);
}

// 合併輸入同一條 net 的重複 NOT：保留第一顆，其餘 fanout 改接、標死。
// 在完整 netlist 上做，安全不成環；只省 area、不動深度。
int mergeDuplicateInverters(Netlist& netlist) {
    std::unordered_map<int, int> canonicalInvOut;  // src_net -> 代表 NOT 的輸出 net
    int merged = 0;

    for (int g = 0; g < (int)netlist.getGateCount(); ++g) {
        Gate& gate = netlist.getGateMutable(g);
        if (gate.type != GateType::NOT) continue;
        if (gate.inputNetIds.empty() || gate.inputNetIds[0] < 0) continue;
        if (gate.outputNetId < 0) continue;

        int src    = gate.inputNetIds[0];
        int outNet = gate.outputNetId;

        // 保護：輸出是 PO net 就不動這顆（廢掉會讓 PO 失去 driver）
        // 這裡使用 const getter 即可，因為只是讀取 isPO
        if (netlist.getNet(outNet).isPO) continue;

        auto it = canonicalInvOut.find(src);
        if (it == canonicalInvOut.end()) {
            canonicalInvOut[src] = outNet;   // 第一顆，當代表
            continue;
        }

        int canonOut = it->second;
        if (canonOut == outNet) continue;    // 防呆

        redirectNetLoads(netlist, outNet, canonOut);  // 這顆的下游改吃代表的輸出 (呼叫 Optimizer 內部的 helper)
        detachGate(netlist, g);                       // 標死並斷線
        ++merged;
    }
    return merged;
}

// 把所有「吃 fromNet 當輸入」的閘，改成吃 toNet；並搬移 loadGateIds。
void redirectNetLoads(Netlist& netlist, int fromNet, int toNet) {
    if (fromNet < 0 || toNet < 0 ||
        fromNet >= (int)netlist.getNetCount() || toNet >= (int)netlist.getNetCount()) return;

    // 透過 public API 取得可修改的 (Mutable) Net 參考
    Net& fromNetObj = netlist.getNetMutable(fromNet);
    Net& toNetObj = netlist.getNetMutable(toNet);

    for (int gid : fromNetObj.loadGateIds) {
        if (gid < 0 || gid >= (int)netlist.getGateCount()) continue;
        
        // 透過 public API 取得可修改的 Gate 參考
        Gate& g = netlist.getGateMutable(gid);
        if (g.type == GateType::UNKNOWN) continue;

        for (auto& in : g.inputNetIds) {
            if (in == fromNet) in = toNet;
        }

        toNetObj.loadGateIds.push_back(gid);
    }
    fromNetObj.loadGateIds.clear();
    netlist.markDirty();
}

// 把一顆閘標死並從其 fanin 的 loadGateIds 斷開。
void detachGate(Netlist& netlist, int gid) {
    if (gid < 0 || gid >= (int)netlist.getGateCount()) return;
    
    // 透過 public API 取得可修改的 Gate 參考
    Gate& g = netlist.getGateMutable(gid);

    for (int in : g.inputNetIds) {
        if (in < 0 || in >= (int)netlist.getNetCount()) continue;
        
        // 透過 public API 取得前級 Net 並移除負載
        Net& n = netlist.getNetMutable(in);
        auto& L = n.loadGateIds;
        L.erase(std::remove(L.begin(), L.end(), gid), L.end());
    }
    
    if (g.outputNetId >= 0 && g.outputNetId < (int)netlist.getNetCount()) {
        netlist.getNetMutable(g.outputNetId).driverGateId = -1;
    }

    g.type = GateType::UNKNOWN;
    netlist.markDirty();
}

// ============================================================================
// GenericLowering — 把 mockturtle 的 AIG/XAG 直接轉成「任意目標 basis」的 Netlist
// ============================================================================
//
// 取代原本的兩段式流程:
//     XagToNetlist (每條反相邊插一顆 NOT)  →  convertToBasis (逐閘 pattern 展開)
// 這兩段各自無條件加一層,關鍵路徑上最多 +2 層/節點。
//
// 本檔案改用 dual-rail bubble pushing:
//   每個節點同時追蹤「正相」與「反相」兩個到達層數。一個 AND 節點
//   F = A·B 有四種等價實現,分別產生不同極性:
//
//       AND(A, B)      → F      需要輸入極性 (c0, c1)
//       NOR(!A, !B)    → F      需要輸入極性 (!c0, !c1)
//       NAND(A, B)     → !F     需要輸入極性 (c0, c1)
//       OR(!A, !B)     → !F     需要輸入極性 (!c0, !c1)
//
//   四者都是 1 層。所以「反相輸出免費」不是 NAND 的特權,而是
//   {NAND, OR} 這組的性質;{AND, NOR} 那組則是正相免費。
//   DP 在拓撲序上選最淺的組合,對深度是最佳的。
//
// 這個函式同時吃掉了舊流程的 Stage 4 (basis enforcement) 與
// Stage 5 (inverter absorption):輸出 by construction 就是合規的,
// 而 bubble pushing 本身就是全域最佳版本的 inverter absorption。
//
// XagToNetlist / AigToNetlist 變成特例:
//     allowed = {AND, XOR, NOT}  等價於  XagToNetlist
//     allowed = {AND, NOT}       等價於  AigToNetlist
// ============================================================================

namespace lowering {

// ============================================================================
// 1. LoweringBasis
// ============================================================================

LoweringBasis LoweringBasis::fromLists(const std::vector<GateType>& allowed,
                                       const std::vector<GateType>& banned) {
    LoweringBasis b;
    static const GateType kComb[] = {
        GateType::AND, GateType::OR, GateType::NAND, GateType::NOR,
        GateType::NOT, GateType::BUF, GateType::XOR, GateType::XNOR };

    for (GateType t : kComb) {
        bool ok = allowed.empty() ||
                  std::find(allowed.begin(), allowed.end(), t) != allowed.end();
        if (ok && std::find(banned.begin(), banned.end(), t) != banned.end()) ok = false;
        b.allow[static_cast<int>(t)] = ok;
    }
    return b;
}

bool LoweringBasis::canInvert() const {
    return has(GateType::NOT)  || has(GateType::NAND) || has(GateType::NOR) ||
           has(GateType::XOR)  || has(GateType::XNOR);
}

bool LoweringBasis::hasAndForm() const {
    return has(GateType::AND) || has(GateType::NOR) ||
           has(GateType::NAND) || has(GateType::OR);
}

bool LoweringBasis::hasXorForm() const {
    return has(GateType::XOR) || has(GateType::XNOR);
}

bool LoweringBasis::needsConstForInvert() const {
    return !has(GateType::NOT) && !has(GateType::NAND) && !has(GateType::NOR);
}

bool LoweringBasis::complete() const {
    return canInvert() && hasAndForm();
}

std::string LoweringBasis::describe() const {
    static const std::pair<GateType, const char*> kNames[] = {
        {GateType::AND,"AND"}, {GateType::OR,"OR"}, {GateType::NAND,"NAND"},
        {GateType::NOR,"NOR"}, {GateType::NOT,"NOT"}, {GateType::BUF,"BUF"},
        {GateType::XOR,"XOR"}, {GateType::XNOR,"XNOR"} };
    std::string s;
    for (const auto& [t, n] : kNames)
        if (has(t)) { if (!s.empty()) s += ","; s += n; }
    return s.empty() ? "<empty>" : s;
}

// ============================================================================
// 2. 內部表示 (Lowering Graph) — 僅本編譯單元可見
// ============================================================================
namespace {

constexpr int    kInf     = INT_MAX / 4;
constexpr double kInfArea = 1e18;

// 指向 LNode 的參照,帶極性
struct LRef {
    uint32_t node = 0;
    bool     comp = false;
};

struct LNode {
    enum class Kind : uint8_t { CONST, PI, AND, XOR };

    Kind     kind = Kind::AND;
    uint32_t fanin[2] = {0, 0};
    bool     comp[2]  = {false, false};
    uint8_t  basisId  = 0;      // 0 = default, 1 = cone
    int      extNet   = -1;     // PI / CONST 用:外部已存在的 net id
};

// 一個 slot (node, polarity) 的實現方式
enum class Impl : uint8_t {
    NONE,
    EXTERNAL,   // PI / DFF.Q / 常數,net 已存在
    AND_POS,    // AND(f0@c0, f1@c1)        → 正相
    NOR_POS,    // NOR(f0@!c0, f1@!c1)      → 正相
    NAND_NEG,   // NAND(f0@c0, f1@c1)       → 反相
    OR_NEG,     // OR(f0@!c0, f1@!c1)       → 反相
    XOR_G,      // XOR(f0@q0, f1@q1)
    XNOR_G,     // XNOR(f0@q0, f1@q1)
    INVERT,     // 從同一節點的另一個 slot 反相過來
};

struct SlotPlan {
    Impl   kind  = Impl::NONE;
    bool   q0    = false;
    bool   q1    = false;
    int    depth = kInf;
    double area  = kInfArea;   // area flow
};

// ---------------------------------------------------------------------------
// 找出 cone root 對應的 PO index。
// PO 順序與 DoMockturtleToNetlist 一致:先所有真實 PO bit,再所有 DFF 的 D。
// ---------------------------------------------------------------------------
int findPoIndexByName(const Netlist& old_nl, const std::string& name) {
    int idx = 0;
    for (const auto& port : old_nl.getPrimaryOutputs()) {
        for (int netId : port.netIds) {
            if (old_nl.isValidNetId(netId) && old_nl.getNet(netId).name == name) return idx;
            ++idx;
        }
    }
    // pseudo PO:DFF 的 D 端
    for (size_t i = 0; i < old_nl.getGateCount(); ++i) {
        const auto& g = old_nl.getGate(i);
        if (g.type != GateType::DFF) continue;
        for (size_t p = 0; p < g.inputPinNames.size(); ++p) {
            if (g.inputPinNames[p] != "D") continue;
            const int netId = g.inputNetIds[p];
            if (old_nl.isValidNetId(netId) && old_nl.getNet(netId).name == name) return idx;
        }
        // 也接受直接指名 DFF instance
        if (g.instName == name) return idx;
        ++idx;
    }
    return -1;
}

// 從某個 PO 往回標記 transitive fanin(在 mockturtle 網路上)
template <typename Ntk>
std::vector<uint8_t> markConeNodes(const Ntk& ntk, int poIndex) {
    std::vector<uint8_t> inCone(ntk.size(), 0);
    if (poIndex < 0 || poIndex >= (int)ntk.num_pos()) return inCone;

    std::vector<uint64_t> stack;
    const auto root = ntk.get_node(ntk.po_at(poIndex));
    stack.push_back(root);
    inCone[root] = 1;

    while (!stack.empty()) {
        const auto n = stack.back();
        stack.pop_back();
        if (ntk.is_pi(n) || ntk.is_constant(n)) continue;
        ntk.foreach_fanin(ntk.index_to_node(n), [&](auto const& f) {
            const auto fn = ntk.get_node(f);
            if (!inCone[fn]) { inCone[fn] = 1; stack.push_back(fn); }
        });
    }
    return inCone;
}

} // anonymous namespace

// ============================================================================
// 3. 主函式
// ============================================================================

template <typename Ntk>
LoweringResult DoLowerToNetlist(const Ntk& ntk,
                                const Netlist& old_nl,
                                const LoweringSpec& spec) {
    LoweringResult out;

    // -----------------------------------------------------------------------
    // 0. 功能完備性檢查 —— 在動任何東西之前
    // -----------------------------------------------------------------------
    std::array<LoweringBasis, 2> bases{ spec.defaultBasis,
                                        spec.coneBasis.value_or(spec.defaultBasis) };
    const int numBases = spec.hasCone() ? 2 : 1;

    for (int i = 0; i < numBases; ++i) {
        if (!bases[i].complete()) {
            out.message = std::string("basis {") + bases[i].describe() +
                "} is not functionally complete: " +
                (!bases[i].canInvert() ? "cannot realize inversion"
                                       : "cannot realize a two-input product");
            return out;
        }
    }

    // -----------------------------------------------------------------------
    // 1. Cone 標記
    // -----------------------------------------------------------------------
    std::vector<uint8_t> inCone;
    if (spec.hasCone()) {
        const int poIdx = findPoIndexByName(old_nl, spec.coneRootName);
        if (poIdx < 0) {
            out.message = "cone root '" + spec.coneRootName +
                          "' is not a primary output or a DFF D-pin net.";
            return out;
        }
        inCone = markConeNodes(ntk, poIdx);
        for (uint8_t v : inCone) out.coneNodeCount += v;
    }
    auto basisIdOf = [&](uint64_t nodeIdx) -> uint8_t {
        return (!inCone.empty() && inCone[nodeIdx]) ? 1 : 0;
    };

    // -----------------------------------------------------------------------
    // 2. 建立 Lowering Graph
    //    XOR 節點在「該節點的 basis 沒有 XOR/XNOR」時展開成 3 個 AND:
    //        p = AND(x, y)         q = AND(!x, !y)         r = AND(!p, !q)
    //    r = !(x·y) · !(!x·!y) = x XOR y
    //    展開後三個節點各自參與極性最佳化,通常比寫死的 4-NAND 樹(3 層)好。
    // -----------------------------------------------------------------------
    std::vector<LNode> nodes;
    nodes.reserve(ntk.size() * 2);
    std::vector<LRef> ntkToL(ntk.size());

    auto addNode = [&](LNode::Kind kind, uint32_t f0, bool c0,
                       uint32_t f1, bool c1, uint8_t basisId) -> uint32_t {
        LNode n;
        n.kind = kind;
        n.fanin[0] = f0; n.comp[0] = c0;
        n.fanin[1] = f1; n.comp[1] = c1;
        n.basisId  = basisId;
        nodes.push_back(n);
        return (uint32_t)(nodes.size() - 1);
    };

    // 常數節點
    const uint32_t lConst = [&] {
        LNode n; n.kind = LNode::Kind::CONST;
        nodes.push_back(n);
        return (uint32_t)(nodes.size() - 1);
    }();
    ntkToL[ntk.get_node(ntk.get_constant(false))] = LRef{lConst, false};

    // PI 節點(含 DFF.Q 的 pseudo PI)照 mockturtle 的 PI 順序建立
    std::vector<uint32_t> piL(ntk.num_pis(), 0);
    for (uint32_t i = 0; i < ntk.num_pis(); ++i) {
        LNode n; n.kind = LNode::Kind::PI;
        nodes.push_back(n);
        piL[i] = (uint32_t)(nodes.size() - 1);
        ntkToL[ntk.pi_at(i)] = LRef{piL[i], false};
    }

    // 邏輯節點(mockturtle 的 foreach_gate 是拓撲序)
    ntk.foreach_gate([&](auto const& n) {
        const uint64_t nIdx = ntk.node_to_index(n);
        const uint8_t  bid  = basisIdOf(nIdx);

        LRef f[2];
        int k = 0;
        ntk.foreach_fanin(n, [&](auto const& s) {
            const LRef base = ntkToL[ntk.get_node(s)];
            f[k++] = LRef{ base.node, (bool)(base.comp ^ ntk.is_complemented(s)) };
        });

        const bool isAnd = ntk.is_and(n);

        if (isAnd) {
            ntkToL[nIdx] = LRef{
                addNode(LNode::Kind::AND, f[0].node, f[0].comp,
                                          f[1].node, f[1].comp, bid), false };
            return;
        }

        // ---- XOR 節點 ----
        // XOR 的補數可以自由外提:XOR(!a,b) = !XOR(a,b)
        const bool cx = f[0].comp ^ f[1].comp;

        if (bases[bid].hasXorForm()) {
            ntkToL[nIdx] = LRef{
                addNode(LNode::Kind::XOR, f[0].node, false,
                                          f[1].node, false, bid), cx };
            return;
        }

        // 展開成 3 個 AND
        const uint32_t p = addNode(LNode::Kind::AND, f[0].node, false, f[1].node, false, bid);
        const uint32_t q = addNode(LNode::Kind::AND, f[0].node, true,  f[1].node, true,  bid);
        const uint32_t r = addNode(LNode::Kind::AND, p, true, q, true, bid);
        ntkToL[nIdx] = LRef{ r, cx };
    });

    const uint32_t numL = (uint32_t)nodes.size();

    // -----------------------------------------------------------------------
    // 3. Forward DP — 每個節點兩個 slot 的最小到達層數
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // 3a. 結構 fanout —— area flow 的分母
    //
    // 一個 LNode 被幾個地方消費：其他 LNode 的 fanin，加上直接接到 PO 的次數。
    // 建圖之後就固定，不隨 DP 的選擇改變（這正是 area flow 的近似之處：
    // 實際上不同極性的 fanout 不同，這裡不區分）。
    // -----------------------------------------------------------------------
    std::vector<uint32_t> fanoutCount(numL, 0);
    for (uint32_t i = 0; i < numL; ++i) {
        const LNode& n = nodes[i];
        if (n.kind == LNode::Kind::AND || n.kind == LNode::Kind::XOR) {
            ++fanoutCount[n.fanin[0]];
            ++fanoutCount[n.fanin[1]];
        }
    }
    for (uint32_t i = 0; i < ntk.num_pos(); ++i) {
        const LRef base = ntkToL[ntk.get_node(ntk.po_at(i))];
        ++fanoutCount[base.node];
    }

    // -----------------------------------------------------------------------
    // 3b. Forward DP —— 每個節點兩個 slot 的最小成本
    // -----------------------------------------------------------------------
    std::vector<std::array<SlotPlan, 2>> plan(numL);

    // PI / CONST 的 slot 是現成的：深度 0、面積 0
    plan[lConst][0] = SlotPlan{Impl::EXTERNAL, false, false, 0, 0.0};
    plan[lConst][1] = SlotPlan{Impl::EXTERNAL, false, false, 0, 0.0};
    for (uint32_t pi : piL) plan[pi][0] = SlotPlan{Impl::EXTERNAL, false, false, 0, 0.0};

    const bool areaFirst = (spec.objective == LoweringObjective::MinArea);

    // 字典序比較。兩個指標都算，只是誰優先不同。
    auto consider = [&](SlotPlan& slot, Impl kind, bool q0, bool q1,
                        int depth, double area) {
        constexpr double kEps = 1e-9;
        bool better;
        if (areaFirst) {
            if (area < slot.area - kEps)                    better = true;
            else if (area > slot.area + kEps)               better = false;
            else                                            better = (depth < slot.depth);
        } else {
            if (depth != slot.depth)                        better = (depth < slot.depth);
            else                                            better = (area < slot.area - kEps);
        }
        if (better) slot = SlotPlan{kind, q0, q1, depth, area};
    };

    // fanin 某極性分攤過來的 area flow
    auto afShare = [&](uint32_t fanin, int pol) -> double {
        const double a = plan[fanin][pol].area;
        if (a >= kInfArea) return kInfArea;
        const double share = static_cast<double>(std::max<uint32_t>(1u, fanoutCount[fanin]));
        return a / share;
    };

    for (uint32_t i = 0; i < numL; ++i) {
        const LNode& n = nodes[i];
        const LoweringBasis& B = bases[n.basisId];

        if (n.kind == LNode::Kind::AND) {
            const int p0 = n.comp[0] ? 1 : 0;   // fanin0 @ c0
            const int p1 = n.comp[1] ? 1 : 0;   // fanin1 @ c1
            const int n0 = 1 - p0;              // fanin0 @ !c0
            const int n1 = 1 - p1;

            const int dSame = (plan[n.fanin[0]][p0].depth >= kInf ||
                               plan[n.fanin[1]][p1].depth >= kInf)
                            ? kInf
                            : std::max(plan[n.fanin[0]][p0].depth,
                                       plan[n.fanin[1]][p1].depth) + 1;
            const int dFlip = (plan[n.fanin[0]][n0].depth >= kInf ||
                               plan[n.fanin[1]][n1].depth >= kInf)
                            ? kInf
                            : std::max(plan[n.fanin[0]][n0].depth,
                                       plan[n.fanin[1]][n1].depth) + 1;

            const double aSame = 1.0 + afShare(n.fanin[0], p0) + afShare(n.fanin[1], p1);
            const double aFlip = 1.0 + afShare(n.fanin[0], n0) + afShare(n.fanin[1], n1);

            // 正相
            if (B.has(GateType::AND))
                consider(plan[i][0], Impl::AND_POS,  n.comp[0],  n.comp[1], dSame, aSame);
            if (B.has(GateType::NOR))
                consider(plan[i][0], Impl::NOR_POS, !n.comp[0], !n.comp[1], dFlip, aFlip);
            // 反相
            if (B.has(GateType::NAND))
                consider(plan[i][1], Impl::NAND_NEG, n.comp[0],  n.comp[1], dSame, aSame);
            if (B.has(GateType::OR))
                consider(plan[i][1], Impl::OR_NEG,  !n.comp[0], !n.comp[1], dFlip, aFlip);

        } else if (n.kind == LNode::Kind::XOR) {
            // fanin 的 comp 已在建圖時正規化為 false。
            // XOR(f0@q0, f1@q1) = f0 ^ f1 ^ q0 ^ q1 → slot p 需要 q0^q1 == p
            // XNOR 則需要 q0^q1 == !p
            for (int p = 0; p < 2; ++p) {
                for (int q0 = 0; q0 < 2; ++q0) {
                    for (int q1 = 0; q1 < 2; ++q1) {
                        const int d0 = plan[n.fanin[0]][q0].depth;
                        const int d1 = plan[n.fanin[1]][q1].depth;
                        if (d0 >= kInf || d1 >= kInf) continue;
                        const int    d = std::max(d0, d1) + 1;
                        const double a = 1.0 + afShare(n.fanin[0], q0)
                                             + afShare(n.fanin[1], q1);
                        const int parity = q0 ^ q1;
                        if (B.has(GateType::XOR)  && parity == p)
                            consider(plan[i][p], Impl::XOR_G,  q0, q1, d, a);
                        if (B.has(GateType::XNOR) && parity == (p ^ 1))
                            consider(plan[i][p], Impl::XNOR_G, q0, q1, d, a);
                    }
                }
            }
        }

        // 反相鬆弛：從另一個 slot 插一顆 NOT（等價閘）過來。
        // NOT 是這個 slot 專屬的一顆閘，不分攤，所以 area 直接 +1。
        // 做兩次就收斂 —— slot A 由 slot B 反相而來時，B 不可能再由 A
        // 反相而改善（那需要 cost+2 < cost）。
        if (B.canInvert()) {
            for (int r = 0; r < 2; ++r) {
                for (int p = 0; p < 2; ++p) {
                    const SlotPlan& other = plan[i][1 - p];
                    if (other.depth >= kInf) continue;
                    consider(plan[i][p], Impl::INVERT, false, false,
                             other.depth + 1, other.area + 1.0);
                }
            }
        }
    }

    // -----------------------------------------------------------------------
    // 4. Backward marking — 只有真正被消費的 slot 才會被生成
    //    少了這一趟,兩個極性都生 = 面積直接翻倍。
    // -----------------------------------------------------------------------
    std::vector<std::array<uint8_t, 2>> need(numL, {0, 0});

    std::vector<std::pair<uint32_t, int>> stack;
    auto require = [&](uint32_t node, int pol) {
        if (need[node][pol]) return;
        need[node][pol] = 1;
        stack.emplace_back(node, pol);
    };

    // 從所有 PO(含 pseudo PO / DFF.D)出發
    std::vector<std::pair<uint32_t, int>> poNeed(ntk.num_pos());
    for (uint32_t i = 0; i < ntk.num_pos(); ++i) {
        const auto sig  = ntk.po_at(i);
        const LRef base = ntkToL[ntk.get_node(sig)];
        const int  pol  = (base.comp ^ ntk.is_complemented(sig)) ? 1 : 0;
        poNeed[i] = {base.node, pol};
        require(base.node, pol);
    }

    while (!stack.empty()) {
        const auto [i, p] = stack.back();
        stack.pop_back();

        const SlotPlan& sp = plan[i][p];
        const LNode& n = nodes[i];

        switch (sp.kind) {
            case Impl::INVERT:
                require(i, 1 - p);
                break;
            case Impl::AND_POS: case Impl::NOR_POS:
            case Impl::NAND_NEG: case Impl::OR_NEG:
            case Impl::XOR_G: case Impl::XNOR_G:
                require(n.fanin[0], sp.q0 ? 1 : 0);
                require(n.fanin[1], sp.q1 ? 1 : 0);
                break;
            default:
                break;   // EXTERNAL / NONE
        }
    }

    // 檢查有沒有無法實現的 slot(理論上完備性檢查過就不會發生)
    for (uint32_t i = 0; i < numL; ++i) {
        for (int p = 0; p < 2; ++p) {
            if (need[i][p] && plan[i][p].kind == Impl::NONE) {
                out.message = "internal error: unrealizable slot under the given basis "
                              "(node " + std::to_string(i) + ", polarity " + std::to_string(p) + ").";
                return out;
            }
        }
    }

    for (uint32_t i = 0; i < ntk.num_pos(); ++i) {
        const auto [ln, pol] = poNeed[i];
        if (plan[ln][pol].area < kInfArea) out.estimatedAreaFlow += plan[ln][pol].area;
    }

    // -----------------------------------------------------------------------
    // 5. Emit — 建立實際的 Netlist
    //    PI / PO / DFF / 常數的處理與 DoMockturtleToNetlist 保持一致。
    // -----------------------------------------------------------------------
    Netlist nl;
    std::unordered_map<int, int> oldNetToNewNet;

    const int const0Net = nl.addNet("1'b0");
    nl.setNetConst(const0Net, true, 0);
    const int const1Net = nl.addNet("1'b1");
    nl.setNetConst(const1Net, true, 1);

    for (size_t i = 0; i < old_nl.getNetCount(); ++i) {
        if (!old_nl.getNet(i).isConst) continue;
        if (old_nl.getNet(i).constVal == 0) oldNetToNewNet[i] = const0Net;
        if (old_nl.getNet(i).constVal == 1) oldNetToNewNet[i] = const1Net;
    }

    // slot → net id
    std::vector<std::array<int, 2>> slotNet(numL, {-1, -1});
    slotNet[lConst][0] = const0Net;
    slotNet[lConst][1] = const1Net;

    // ---- 5a. 真實 PI ----
    int piIndex = 0;
    for (const auto& port : old_nl.getPrimaryInputs()) {
        nl.addPrimaryInput(port.name, port.msb, port.lsb);
        const auto& newPort = nl.getPrimaryInputs().back();
        for (size_t i = 0; i < port.netIds.size(); ++i) {
            oldNetToNewNet[port.netIds[i]] = newPort.netIds[i];
            slotNet[piL[piIndex++]][0] = newPort.netIds[i];
        }
    }
    const int numRealPis = piIndex;

    // ---- 5b. PO port 宣告 ----
    std::vector<std::pair<int, int>> poNetMappings;   // <old_net, new_net>
    for (const auto& oldPort : old_nl.getPrimaryOutputs()) {
        nl.addPrimaryOutput(oldPort.name, oldPort.msb, oldPort.lsb);
        const auto& newPort = nl.getPrimaryOutputs().back();
        for (size_t i = 0; i < oldPort.netIds.size(); ++i)
            poNetMappings.push_back({oldPort.netIds[i], newPort.netIds[i]});
    }

    // ---- 5c. slot 直通 PO 的認領表 ----
    // 讓一個 slot 的產生閘直接把輸出接到 PO net 上,省掉一顆 BUF。
    // 只有「真正的邏輯閘」能認領:PI 與常數不能改名。
    std::unordered_map<uint64_t, int> slotClaimsPo;
    auto slotKey = [](uint32_t node, int pol) -> uint64_t {
        return ((uint64_t)node << 1) | (uint64_t)pol;
    };
    for (uint32_t i = 0; i < poNetMappings.size() && i < poNeed.size(); ++i) {
        const auto [ln, pol] = poNeed[i];
        if (nodes[ln].kind == LNode::Kind::PI || nodes[ln].kind == LNode::Kind::CONST) continue;
        const uint64_t key = slotKey(ln, pol);
        if (slotClaimsPo.count(key)) continue;          // 已被別的 PO 認領
        slotClaimsPo[key] = poNetMappings[i].second;
    }

    // ---- 5d. DFF ----
    int gateCounter = 0, netCounter = 0;
    auto newGateName = [&] { return "g_lo_" + std::to_string(gateCounter++); };
    auto newNetName  = [&] { return "n_lo_" + std::to_string(netCounter++); };

    std::vector<int> newDffIds;
    for (size_t i = 0; i < old_nl.getGateCount(); ++i) {
        const auto& oldGate = old_nl.getGate(i);
        if (oldGate.type != GateType::DFF) continue;

        const int dffId = nl.addGate(oldGate.instName, GateType::DFF);
        newDffIds.push_back(dffId);

        for (size_t p = 0; p < oldGate.inputPinNames.size(); ++p) {
            if (oldGate.inputPinNames[p] == "D") continue;
            const int oldNet = oldGate.inputNetIds[p];
            auto it = oldNetToNewNet.find(oldNet);
            if (it != oldNetToNewNet.end())
                nl.connectGateInput(dffId, it->second, oldGate.inputPinNames[p]);
        }

        const uint32_t qL = piL[piIndex++];

        // Q 是 sequential boundary —— DFF 本身沒被重寫，這條線在最佳化前後
        // 是同一個訊號，沒有理由改名。
        std::string qName;
        if (old_nl.isValidNetId(oldGate.outputNetId))
            qName = old_nl.getNet(oldGate.outputNetId).name;
        if (qName.empty()) qName = oldGate.instName + "_Q";

        int qNet = nl.getNetId(qName);      // Q 同時是 PO 時，port 已經建過這條 net
        if (qNet < 0) qNet = nl.addNet(qName);

        // 這個 slot 若被 PO 認領過，認領作廢：net 已經有正確的名字，
        // 而且 PO port 建的就是同一條線。
        slotClaimsPo.erase(slotKey(qL, 0));

        nl.connectGateOutput(dffId, qNet);
        slotNet[qL][0] = qNet;
    }
    (void)numRealPis;

    // ---- 5e. 產生邏輯閘 ----
    auto emitGate = [&](GateType type, int inA, int inB, int outNet) -> int {
        const int gid = nl.addGate(newGateName(), type);
        const int nid = (outNet >= 0) ? outNet : nl.addNet(newNetName());
        nl.connectGateInput(gid, inA);
        if (inB >= 0) nl.connectGateInput(gid, inB);
        nl.connectGateOutput(gid, nid);
        return nid;
    };

    // 依 basis 挑一種「反相」實現,全都是 1 層
    auto emitInvert = [&](const LoweringBasis& B, int inNet, int outNet) -> int {
        if (B.has(GateType::NOT))  return emitGate(GateType::NOT,  inNet, -1,        outNet);
        if (B.has(GateType::NAND)) return emitGate(GateType::NAND, inNet, inNet,     outNet);
        if (B.has(GateType::NOR))  return emitGate(GateType::NOR,  inNet, inNet,     outNet);
        if (B.has(GateType::XOR))  return emitGate(GateType::XOR,  inNet, const1Net, outNet);
        return emitGate(GateType::XNOR, inNet, const0Net, outNet);   // XNOR(a, 1'b0)
    };

    auto emitSlot = [&](uint32_t i, int p) {
        if (slotNet[i][p] >= 0) return;
        const SlotPlan& sp = plan[i][p];
        const LNode& n = nodes[i];
        const LoweringBasis& B = bases[n.basisId];

        auto claimed = slotClaimsPo.find(slotKey(i, p));
        const int outNet = (claimed != slotClaimsPo.end()) ? claimed->second : -1;

        int result = -1;
        switch (sp.kind) {
            case Impl::INVERT:
                result = emitInvert(B, slotNet[i][1 - p], outNet);
                break;
            case Impl::AND_POS:
                result = emitGate(GateType::AND,  slotNet[n.fanin[0]][sp.q0],
                                                  slotNet[n.fanin[1]][sp.q1], outNet);
                break;
            case Impl::NOR_POS:
                result = emitGate(GateType::NOR,  slotNet[n.fanin[0]][sp.q0],
                                                  slotNet[n.fanin[1]][sp.q1], outNet);
                break;
            case Impl::NAND_NEG:
                result = emitGate(GateType::NAND, slotNet[n.fanin[0]][sp.q0],
                                                  slotNet[n.fanin[1]][sp.q1], outNet);
                break;
            case Impl::OR_NEG:
                result = emitGate(GateType::OR,   slotNet[n.fanin[0]][sp.q0],
                                                  slotNet[n.fanin[1]][sp.q1], outNet);
                break;
            case Impl::XOR_G:
                result = emitGate(GateType::XOR,  slotNet[n.fanin[0]][sp.q0],
                                                  slotNet[n.fanin[1]][sp.q1], outNet);
                break;
            case Impl::XNOR_G:
                result = emitGate(GateType::XNOR, slotNet[n.fanin[0]][sp.q0],
                                                  slotNet[n.fanin[1]][sp.q1], outNet);
                break;
            default:
                return;   // EXTERNAL 已經有 net 了
        }
        slotNet[i][p] = result;
    };

    // 拓撲序走一次。同一節點若兩個 slot 都要,先生非 INVERT 的那個。
    for (uint32_t i = 0; i < numL; ++i) {
        const bool inv0 = plan[i][0].kind == Impl::INVERT;
        if (need[i][0] && !inv0) emitSlot(i, 0);
        if (need[i][1]) emitSlot(i, 1);
        if (need[i][0] && inv0)  emitSlot(i, 0);
    }

    // ---- 5f. PO 收尾 ----
    // 沒被認領的 PO(由 PI/常數驅動、或多個 PO 共用同一訊號)需要橋接。
    // BUF 不在 basis 裡時用兩顆反相閘代替。
    const LoweringBasis& topB = bases[0];
    for (uint32_t i = 0; i < poNetMappings.size() && i < poNeed.size(); ++i) {
        const int oldNetId = poNetMappings[i].first;
        const int extNetId = poNetMappings[i].second;

        const Net& oldPoNet = old_nl.getNet(oldNetId);
        if (oldPoNet.driverGateId == -1 && !oldPoNet.isPI) continue;   // 懸空

        const auto [ln, pol] = poNeed[i];
        const int src = slotNet[ln][pol];
        if (src < 0 || src == extNetId) continue;                      // 已直通

        if (topB.has(GateType::BUF)) {
            emitGate(GateType::BUF, src, -1, extNetId);
        } else {
            const int mid = emitInvert(topB, src, -1);
            emitInvert(topB, mid, extNetId);
        }
    }

    // ---- 5g. DFF.D ----
    {
        uint32_t poIdx = (uint32_t)poNetMappings.size();
        for (int dffId : newDffIds) {
            if (poIdx >= poNeed.size()) break;
            const auto [ln, pol] = poNeed[poIdx++];
            const int d = slotNet[ln][pol];
            nl.connectGateInput(dffId, (d >= 0 ? d : const0Net), "D");
        }
    }

    // ---- DFF D-pin 名稱回填 ----
    //
    // 原始 D net 的驅動邏輯被整個重寫，餵給 D 的現在是一條新產生的 n_lo_xxx。
    // 但「這顆 DFF 的 next-state」這個語意還在，所以把它改名回原本的名字。
    //
    // 三種保不住的情況，一律記進 unpreservedDffNetNames 讓呼叫端回報：
    //   (a) 兩顆 DFF 的 D 被合成同一條線 —— 只有第一顆拿得到名字
    //   (b) D 被化簡成 PI 或常數 —— 那條線已經有語意，不能覆蓋
    //   (c) 名字已被其他 boundary 佔用（例如 shift register 的 Q→D，
    //       那個名字在 Q 那段就保留好了，這裡跳過是正確的）
    {
        // 依 DFF 出現順序收集原始 D net 名稱，與 newDffIds 對齊
        std::vector<std::string> wantedDNames;
        wantedDNames.reserve(newDffIds.size());
        for (size_t i = 0; i < old_nl.getGateCount(); ++i) {
            const auto& g = old_nl.getGate(i);
            if (g.type != GateType::DFF) continue;
            std::string name;
            for (size_t p = 0; p < g.inputPinNames.size(); ++p) {
                if (g.inputPinNames[p] != "D") continue;
                const int netId = g.inputNetIds[p];
                if (old_nl.isValidNetId(netId)) name = old_nl.getNet(netId).name;
                break;
            }
            wantedDNames.push_back(name);
        }

        for (size_t i = 0; i < newDffIds.size() && i < wantedDNames.size(); ++i) {
            const std::string& wanted = wantedDNames[i];
            if (wanted.empty()) continue;
            if (nl.getNetId(wanted) >= 0) continue;      // (c) 名字已被佔用

            const int dNet = nl.getGateInputNetId(newDffIds[i], "D");
            if (!nl.isValidNetId(dNet)) continue;

            const Net& n = nl.getNet(dNet);
            if (n.isPI || n.isPO || n.isConst) {         // (b)
                out.unpreservedDffNetNames.push_back(wanted);
                continue;
            }
            if (!nl.renameNet(n.name, wanted)) {         // (a) 或其他失敗
                out.unpreservedDffNetNames.push_back(wanted);
            }
        }
    }

    mergeDuplicateInverters(nl);
    nl.trimDeadLogic();

     // debug: 統計各種 gate 的數量
    /*int cnt[10] = {0};
    for (uint32_t i = 0; i < numL; ++i)
        for (int p = 0; p < 2; ++p)
            if (need[i][p]) cnt[(int)plan[i][p].kind]++;
    std::cout << "  [lowering] AND_POS=" << cnt[(int)Impl::AND_POS]
              << " NOR_POS="  << cnt[(int)Impl::NOR_POS]
              << " NAND_NEG=" << cnt[(int)Impl::NAND_NEG]
              << " OR_NEG="   << cnt[(int)Impl::OR_NEG]
              << " XOR_G="    << cnt[(int)Impl::XOR_G]
              << " INVERT="   << cnt[(int)Impl::INVERT]
              << " / LNodes=" << numL
              << " xag.size=" << ntk.size() << "\n";*/
    

    out.netlist   = std::move(nl);
    out.depth     = out.netlist.findGlobalCriticalPath().depth;
    out.gateCount = 0;
    for (const auto& kv : out.netlist.countGatesByType()) out.gateCount += kv.second;
    out.ok        = true;
    out.message   = "lowered to basis {" + bases[0].describe() + "}" +
                    (spec.hasCone() ? (", cone {" + bases[1].describe() + "}") : "");
    return out;
}

// ============================================================================
// 4. 顯式實例化
// ============================================================================

template LoweringResult DoLowerToNetlist<mockturtle::xag_network>(
    const mockturtle::xag_network&, const Netlist&, const LoweringSpec&);
template LoweringResult DoLowerToNetlist<mockturtle::aig_network>(
    const mockturtle::aig_network&, const Netlist&, const LoweringSpec&);

// ============================================================================
// 5. 便利包裝
// ============================================================================

LoweringResult LowerXag(const mockturtle::xag_network& xag,
                        const Netlist& old_nl,
                        const LoweringSpec& spec) {
    return DoLowerToNetlist<mockturtle::xag_network>(xag, old_nl, spec);
}

LoweringResult LowerAig(const mockturtle::aig_network& aig,
                        const Netlist& old_nl,
                        const LoweringSpec& spec) {
    return DoLowerToNetlist<mockturtle::aig_network>(aig, old_nl, spec);
}

} // namespace lowering
