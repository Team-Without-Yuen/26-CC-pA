#include "include/core/MockturtleConverter.h"
#include <unordered_map>
#include <vector>
#include <string>

namespace {

// 內部輔助函式：針對 Netlist 中的組合邏輯閘進行拓撲排序
// 採用 Kahn's Algorithm (基於入度計算)，避免遞迴造成的 Stack Overflow
std::vector<int> ComputeTopologicalOrder(const Netlist& nl) {
    std::vector<int> topoOrder;
    std::vector<bool> netVisited(nl.getNetCount(), false);

    // 1. 標記所有常數 Net 為已就緒 (起點)
    for (size_t i = 0; i < nl.getNetCount(); ++i) {
        if (nl.getNet(i).isConst) {
            netVisited[i] = true;
        }
    }

    // 2. 標記所有 Primary Inputs (PI) 為已就緒 (起點)
    for (const auto& port : nl.getPrimaryInputs()) {
        for (int netId : port.netIds) {
            if (netId >= 0 && netId < (int)nl.getNetCount()) {
                netVisited[netId] = true;
            }
        }
    }

    // 3. 標記所有 DFF 的輸出 Net (Q) 為已就緒 (時序電路的起點，視為 Pseudo-PI)
    for (size_t i = 0; i < nl.getGateCount(); ++i) {
        const auto& gate = nl.getGate(i);
        if (gate.type == GateType::DFF) {
            if (gate.outputNetId >= 0 && gate.outputNetId < (int)nl.getNetCount()) {
                netVisited[gate.outputNetId] = true;
            }
        }
    }

    // 4. 計算各個組合邏輯閘的入度 (即尚未就緒的 input net 數量)
    std::vector<int> inDegree(nl.getGateCount(), 0);
    std::vector<int> queue;
    queue.reserve(nl.getGateCount());

    for (size_t i = 0; i < nl.getGateCount(); ++i) {
        const auto& gate = nl.getGate(i);
        if (gate.type == GateType::DFF) continue; // DFF 不參與組合邏輯的排序

        int needed = 0;
        for (int netId : gate.inputNetIds) {
            if (netId >= 0 && !netVisited[netId]) {
                needed++;
            }
        }
        inDegree[i] = needed;
        if (needed == 0) {
            queue.push_back(i);
        }
    }

    // 5. 拓撲排序核心：依序彈出入度為 0 的邏輯閘，並更新受其驅動的下一級邏輯閘
    size_t head = 0;
    while (head < queue.size()) {
        int gateId = queue[head++];
        topoOrder.push_back(gateId);

        const auto& gate = nl.getGate(gateId);
        int outNetId = gate.outputNetId;
        if (outNetId >= 0 && outNetId < (int)nl.getNetCount()) {
            netVisited[outNetId] = true;
            const auto& outNet = nl.getNet(outNetId);
            for (int nextGateId : outNet.loadGateIds) {
                if (nextGateId >= 0 && nextGateId < (int)nl.getGateCount()) {
                    const auto& nextGate = nl.getGate(nextGateId);
                    if (nextGate.type == GateType::DFF) continue; // DFF 輸入端為 Pseudo-PO，不阻塞排序

                    inDegree[nextGateId]--;
                    if (inDegree[nextGateId] == 0) {
                        queue.push_back(nextGateId);
                    }
                }
            }
        }
    }

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

    // 4. 依照拓撲順序建立組合邏輯閘
    std::vector<int> topoOrder = ComputeTopologicalOrder(nl);
    for (int gateId : topoOrder) {
        const auto& gate = nl.getGate(gateId);
        
        // 安全地獲取 2-input 訊號；若未連接線路則預設接至常數 0
        signal_t a = (gate.inputNetIds.size() > 0 && gate.inputNetIds[0] != -1) ? netToSignal[gate.inputNetIds[0]] : ntk.get_constant(false);
        signal_t b = (gate.inputNetIds.size() > 1 && gate.inputNetIds[1] != -1) ? netToSignal[gate.inputNetIds[1]] : ntk.get_constant(false);

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

    // Step 2: 遍歷 Mockturtle 內部邏輯並重建 Gates 
    int gate_counter = 0;
    int net_counter = 0;

    ntk.foreach_gate([&](auto n) {
        std::vector<typename Ntk::signal> fanins;
        ntk.foreach_fanin(n, [&](auto f) { fanins.push_back(f); });

        int input_nets[2];
        for (int i = 0; i < 2; ++i) {
            int base_net_id = nodeToNet[ntk.get_node(fanins[i])];
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

        int internal_drive_net = nodeToNet[ntk.get_node(po_signal)];

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
        int internal_drive_net = nodeToNet[ntk.get_node(pseudo_po_signal)];

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

    return new_nl;
}

} // namespace

Netlist XagToNetlist(const mockturtle::xag_network& xag, const Netlist& old_nl) {
    return DoMockturtleToNetlist<mockturtle::xag_network>(xag, old_nl);
}

Netlist AigToNetlist(const mockturtle::aig_network& aig, const Netlist& old_nl) {
    return DoMockturtleToNetlist<mockturtle::aig_network>(aig, old_nl);
}