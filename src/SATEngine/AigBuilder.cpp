#include "SATEngine/AigBuilder.h"

#include <algorithm>
#include <iostream>
#include <unordered_map>
#include <vector>
#include "core/Netlist.h"

namespace eqeng {
namespace {

constexpr int kNoNet = -1;

inline bool is_comb_gate(GateType t) {
    return t != GateType::DFF && t != GateType::UNKNOWN;
}

// 取 DFF 具名 pin 對應的 net；找不到回 -1。
int pin_net(const Gate& g, const char* pin) {
    const size_t n = std::min(g.inputPinNames.size(), g.inputNetIds.size());
    for (size_t k = 0; k < n; ++k)
        if (g.inputPinNames[k] == pin) return g.inputNetIds[k];
    return kNoNet;
}

// 組合閘的拓撲排序（Kahn）。
//
// 與 MockturtleConverter 版本的差異：這裡的閘最多 2 個輸入，
// tied-input 去重只要比較 in0 == in1 即可，不必為每顆閘配一個 unordered_set。
// 100 萬 gate 時這個差異很有感。
std::vector<int> topo_order(const Netlist& nl, const std::vector<char>& netReady_in) {
    const int netCount  = static_cast<int>(nl.getNetCount());
    const int gateCount = static_cast<int>(nl.getGateCount());

    std::vector<char> ready = netReady_in;
    std::vector<int>  order;
    order.reserve(gateCount);

    std::vector<int> inDeg(gateCount, 0);
    std::vector<int> queue;
    queue.reserve(gateCount);

    auto pending_count = [&](const Gate& g) {
        int cnt = 0;
        int prev = kNoNet;
        for (int netId : g.inputNetIds) {
            if (netId < 0 || netId >= netCount) continue;
            if (netId == prev) continue;              // tied input：只算一次
            if (!ready[netId]) ++cnt;
            prev = netId;
        }
        return cnt;
    };

    for (int g = 0; g < gateCount; ++g) {
        const Gate& gate = nl.getGate(g);
        if (!is_comb_gate(gate.type)) continue;
        inDeg[g] = pending_count(gate);
        if (inDeg[g] == 0) queue.push_back(g);
    }

    size_t head = 0;
    while (head < queue.size()) {
        const int gid = queue[head++];
        order.push_back(gid);

        const int outNet = nl.getGate(gid).outputNetId;
        if (outNet < 0 || outNet >= netCount) continue;
        if (ready[outNet]) continue;
        ready[outNet] = 1;

        int prevFired = kNoNet;
        for (int loadId : nl.getNet(outNet).loadGateIds) {
            if (loadId < 0 || loadId >= gateCount) continue;
            if (loadId == prevFired) continue;        // 同一條 net 對同一顆閘只扣一次
            prevFired = loadId;
            if (!is_comb_gate(nl.getGate(loadId).type)) continue;
            if (--inDeg[loadId] == 0) queue.push_back(loadId);
        }
    }
    return order;
}

} // namespace

AigModel::AigModel(const Netlist& nl)
    : nl_(&nl), aig_(), names_(aig_, nl) {}

std::unique_ptr<AigModel> AigModel::build(const Netlist& nl, Options opt) {
    std::unique_ptr<AigModel> m(new AigModel(nl));
    m->do_build(opt);
    return m;
}

bool AigModel::is_free_var(Sig s) const {
    return aig_.is_pi(aig_.get_node(s));
}

void AigModel::mark_conservative() {
    if (health_ == ModelHealth::Sound) {
        health_ = ModelHealth::Conservative;
        healthMessage_ = "sound with floating signals modeled as free variables";
    }
}

void AigModel::mark_invalid(const std::string& reason) {
    ++invalidIssueCount_;
    if (health_ != ModelHealth::Invalid) {
        health_ = ModelHealth::Invalid;
        healthMessage_ = reason;
        return;
    }
    constexpr uint32_t kMaxDetailedIssues = 8;
    if (invalidIssueCount_ <= kMaxDetailedIssues &&
        healthMessage_.find(reason) == std::string::npos) {
        healthMessage_ += "; " + reason;
    } else if (invalidIssueCount_ == kMaxDetailedIssues + 1) {
        healthMessage_ += "; additional issues omitted";
    }
}

void AigModel::do_build(const Options& opt) {
    const Netlist& nl   = *nl_;
    const int netCount  = static_cast<int>(nl.getNetCount());
    const int gateCount = static_cast<int>(nl.getGateCount());

    std::vector<Sig>  netSig(netCount, aig_.get_constant(false));
    std::vector<char> hasSig(netCount, 0);
    std::vector<char> netReady(netCount, 0);

    auto assign = [&](int netId, Sig s) {
        if (netId < 0 || netId >= netCount) return;
        netSig[netId] = s;
        hasSig[netId] = 1;
        netReady[netId] = 1;
        if (!nl.getNet(netId).isRemoved) {
            names_.bind(netId, s);            // 名字綁定：每條有 signal 的 net 都綁
            ++stats_.num_bound_nets;
        }
    };

    // ---- 1. 常數 ----
    for (int i = 0; i < netCount; ++i) {
        const Net& net = nl.getNet(i);
        if (!net.isConst) continue;
        if (net.constVal == 0)      assign(i, aig_.get_constant(false));
        else if (net.constVal == 1) assign(i, aig_.get_constant(true));
    }

    // ---- 2. 真實 PI（嚴格依宣告順序）----
    for (const auto& port : nl.getPrimaryInputs()) {
        for (int netId : port.netIds) {
            if (netId < 0 || netId >= netCount) continue;
            if (hasSig[netId]) continue;                  // 常數/重複宣告防呆
            assign(netId, aig_.create_pi());
            piNetIds_.push_back(netId);
            ++stats_.num_real_pis;
        }
    }

    // ---- 3. DFF Q 當 pseudo-PI（組合抽象；gate id 遞增）----
    std::vector<int> dffGateIds;
    for (int g = 0; g < gateCount; ++g) {
        const Gate& gate = nl.getGate(g);
        if (gate.type != GateType::DFF) continue;
        dffGateIds.push_back(g);

        DffInfo info;
        info.gateId  = g;
        info.qNetId  = gate.outputNetId;
        info.dNetId  = pin_net(gate, "D");
        info.rnNetId = pin_net(gate, "RN");
        info.snNetId = pin_net(gate, "SN");

        if (gate.outputNetId >= 0 && gate.outputNetId < netCount && !hasSig[gate.outputNetId]) {
            Sig q = aig_.create_pi();
            assign(gate.outputNetId, q);
            info.q = q;
            piNetIds_.push_back(gate.outputNetId);
        } else if (gate.outputNetId >= 0 && gate.outputNetId < netCount) {
            mark_invalid("DFF '" + gate.instName + "' Q output has multiple drivers");
            info.q = netSig[gate.outputNetId];
        } else {
            mark_invalid("DFF '" + gate.instName + "' has no valid Q output net");
            info.q = aig_.create_pi();                    // Q 未接：仍需佔位維持索引一致
            piNetIds_.push_back(kNoNet);
        }
        dffs_.push_back(info);
        ++stats_.num_dff;
    }

    // ---- 4. 懸空輸入 → 自由 PI（net id 遞增，順序穩定）----
    // 綁 const0 會讓行為不同的電路被誤判等價，所以預設造自由變數。
    if (opt.undriven_as_free_pi) {
        std::vector<char> referenced(netCount, 0);
        for (int g = 0; g < gateCount; ++g) {
            const Gate& gate = nl.getGate(g);
            if (gate.type == GateType::UNKNOWN) continue;
            for (int netId : gate.inputNetIds)
                if (netId >= 0 && netId < netCount) referenced[netId] = 1;
        }
        // A PO may be the only consumer of an undriven net. It still needs a
        // free variable; otherwise the old implementation silently tied it to 0.
        for (const auto& port : nl.getPrimaryOutputs())
            for (int netId : port.netIds)
                if (netId >= 0 && netId < netCount) referenced[netId] = 1;

        for (int i = 0; i < netCount; ++i) {
            if (!referenced[i] || hasSig[i]) continue;
            if (nl.getNet(i).isRemoved) continue;
            if (nl.getNet(i).driverGateId >= 0) continue;  // 有 driver，等拓撲排序處理
            assign(i, aig_.create_pi());
            piNetIds_.push_back(i);
            ++stats_.num_free_pis;
            mark_conservative();
            if (opt.verbose)
                std::cerr << "[AigBuilder] undriven net '" << nl.getNet(i).name
                          << "' treated as free PI\n";
        }
    }

    // ---- 5. 組合閘（拓撲順序）----
    auto sig_of_input = [&](const Gate& g, size_t idx) -> Sig {
        if (idx >= g.inputNetIds.size()) {
            ++stats_.num_unresolved;
            mark_invalid("gate '" + g.instName + "' is missing required input " +
                         std::to_string(idx));
            return aig_.get_constant(false);
        }
        const int netId = g.inputNetIds[idx];
        if (netId < 0 || netId >= netCount || !hasSig[netId]) {
            ++stats_.num_unresolved;
            mark_invalid("gate '" + g.instName + "' input " + std::to_string(idx) +
                         " cannot be resolved");
            if (opt.verbose)
                std::cerr << "[AigBuilder][WARN] gate '" << g.instName
                          << "' input " << idx << " unresolved; tied to const0\n";
            return aig_.get_constant(false);
        }
        return netSig[netId];
    };

    const std::vector<int> order = topo_order(nl, netReady);
    for (const int gid : order) {
        const Gate& g = nl.getGate(gid);
        const Sig a = sig_of_input(g, 0);
        const bool unary = g.type == GateType::NOT || g.type == GateType::BUF;
        const Sig b = unary ? aig_.get_constant(false) : sig_of_input(g, 1);

        Sig out;
        switch (g.type) {
            case GateType::AND:  out = aig_.create_and(a, b);   break;
            case GateType::OR:   out = aig_.create_or(a, b);    break;
            case GateType::NAND: out = !aig_.create_and(a, b);  break;
            case GateType::NOR:  out = !aig_.create_or(a, b);   break;
            case GateType::XOR:  out = aig_.create_xor(a, b);   break;
            case GateType::XNOR: out = !aig_.create_xor(a, b);  break;
            case GateType::NOT:  out = !a;                      break;
            case GateType::BUF:  out = a;                       break;   // net alias：兩個名字同一 signal
            default:
                mark_invalid("gate '" + g.instName + "' has unsupported gate type");
                out = aig_.get_constant(false);
                break;
        }
        if (g.outputNetId < 0 || g.outputNetId >= netCount) {
            mark_invalid("gate '" + g.instName + "' has no valid output net");
        } else if (hasSig[g.outputNetId]) {
            mark_invalid("gate '" + g.instName + "' output has multiple drivers");
        } else {
            assign(g.outputNetId, out);
        }
        ++stats_.num_comb_gates;
    }

    {
        int combTotal = 0;
        for (int g = 0; g < gateCount; ++g)
            if (is_comb_gate(nl.getGate(g).type)) ++combTotal;
        stats_.num_topo_dropped = static_cast<uint32_t>(combTotal - static_cast<int>(order.size()));
        if (stats_.num_topo_dropped > 0) {
            mark_invalid(std::to_string(stats_.num_topo_dropped) +
                         " combinational gate(s) could not be topologically ordered");
            std::cerr << "[AigBuilder][WARN] " << stats_.num_topo_dropped
                      << " combinational gate(s) unresolved (combinational loop?)\n";
        }
    }

    // ---- 6. 真實 PO ----
    for (const auto& port : nl.getPrimaryOutputs()) {
        for (int netId : port.netIds) {
            if (netId < 0 || netId >= netCount) {
                mark_invalid("primary output '" + port.name + "' has an invalid net");
                continue;
            }
            if (!hasSig[netId]) {
                ++stats_.num_unresolved;
                mark_invalid("primary output net '" + nl.getNet(netId).name +
                             "' cannot be resolved");
            }
            const Sig s = hasSig[netId] ? netSig[netId] : aig_.get_constant(false);
            aig_.create_po(s);
            poSigs_.push_back(s);
            poNetIds_.push_back(netId);
            ++stats_.num_real_pos;
        }
    }

    // ---- 7. DFF next-state 當 pseudo-PO，並把 async set/reset 折進去 ----
    // eff_D = RN & (D | ~SN)
    //   RN=0            → 0        （async reset，優先）
    //   RN=1, SN=0      → 1        （async set）
    //   RN=1, SN=1      → D
    // RN/SN 未接時視為恆 1；此時 create_and/create_or 會被 mockturtle 直接摺掉，
    // 不會多出任何節點。
    for (size_t i = 0; i < dffGateIds.size(); ++i) {
        DffInfo& info = dffs_[i];

        const Gate& dff = nl.getGate(info.gateId);
        auto ctrl_sig = [&](int netId, bool defaultVal, const char* pin) -> Sig {
            if (netId == kNoNet)
                return aig_.get_constant(defaultVal);
            if (netId < 0 || netId >= netCount || !hasSig[netId]) {
                ++stats_.num_unresolved;
                mark_invalid("DFF '" + dff.instName + "' " + pin +
                             " input cannot be resolved");
                return aig_.get_constant(defaultVal);
            }
            return netSig[netId];
        };

        if (info.dNetId < 0 || info.dNetId >= netCount || !hasSig[info.dNetId]) {
            ++stats_.num_unresolved;
            mark_invalid("DFF '" + dff.instName + "' D input cannot be resolved");
            info.d_raw = aig_.get_constant(false);
        } else {
            info.d_raw = netSig[info.dNetId];
        }

        if (opt.fold_async_controls) {
            const Sig rn = ctrl_sig(info.rnNetId, true, "RN");   // 未接 → 不 reset
            const Sig sn = ctrl_sig(info.snNetId, true, "SN");   // 未接 → 不 set
            info.d_eff = aig_.create_and(rn, aig_.create_or(info.d_raw, !sn));
        } else {
            info.d_eff = info.d_raw;
        }
        aig_.create_po(info.d_eff);
    }

    stats_.aig_size = aig_.size();

    if (opt.verbose) {
        std::cerr << "[AigBuilder] PI=" << stats_.num_real_pis
                  << " DFF=" << stats_.num_dff
                  << " freePI=" << stats_.num_free_pis
                  << " PO=" << stats_.num_real_pos
                  << " bound_nets=" << stats_.num_bound_nets
                  << " aig_size=" << stats_.aig_size << "\n";
    }
}

} // namespace eqeng
