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
// Gate::inputNetIds supports arbitrary fan-in.  Use integer stamps to count
// each distinct input/load once without allocating a hash set per gate.  This
// also handles non-adjacent tied inputs such as AND(a, b, a).
std::vector<int> topo_order(const Netlist& nl, const std::vector<char>& netReady_in) {
    const int netCount  = static_cast<int>(nl.getNetCount());
    const int gateCount = static_cast<int>(nl.getGateCount());

    std::vector<char> ready = netReady_in;
    std::vector<int>  order;
    order.reserve(gateCount);

    std::vector<int> inDeg(gateCount, 0);
    std::vector<int> queue;
    queue.reserve(gateCount);

    std::vector<int> inputSeenByGate(netCount, -1);
    std::vector<int> loadFiredByNet(gateCount, -1);

    auto pending_count = [&](const Gate& g, int gateIndex) {
        int cnt = 0;
        for (int netId : g.inputNetIds) {
            if (netId < 0 || netId >= netCount) continue;
            if (inputSeenByGate[netId] == gateIndex) continue;
            inputSeenByGate[netId] = gateIndex;
            if (!ready[netId]) ++cnt;
        }
        return cnt;
    };

    for (int g = 0; g < gateCount; ++g) {
        const Gate& gate = nl.getGate(g);
        if (!is_comb_gate(gate.type)) continue;
        inDeg[g] = pending_count(gate, g);
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

        for (int loadId : nl.getNet(outNet).loadGateIds) {
            if (loadId < 0 || loadId >= gateCount) continue;
            if (loadFiredByNet[loadId] == outNet) continue;
            loadFiredByNet[loadId] = outNet;
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

bool AigModel::is_net_trustworthy(int netId) const {
    if (netId < 0 || netId >= static_cast<int>(netTaint_.size())) return false;
    return netTaint_[netId] == 0;
}

bool AigModel::is_dff_trustworthy(int dffIndex) const {
    if (dffIndex < 0 || dffIndex >= static_cast<int>(dffTaint_.size())) return false;
    return dffTaint_[dffIndex] == 0;
}

void AigModel::taint_root(int netId, const std::string& reason) {
    mark_invalid(reason);                       // 全域健康仍照舊記錄
    if (netId < 0 || netId >= static_cast<int>(netTaint_.size())) return;
    if (netTaint_[netId] == 0) ++numTaintedNets_;
    if (netTaint_[netId] != 1) {
        netTaint_[netId] = 1;
        if (taintRoots_.size() < kMaxTaintRoots) {
            taintRoots_.push_back(netId);
            taintReason_.emplace(netId, reason);
        }
    }
}

void AigModel::taint_derived(int netId) {
    if (netId < 0 || netId >= static_cast<int>(netTaint_.size())) return;
    if (netTaint_[netId] != 0) return;
    netTaint_[netId] = 2;
    ++numTaintedNets_;
}

std::string AigModel::taint_summary() const {
    if (numTaintedNets_ == 0) return "no tainted nets";
    std::string s = std::to_string(numTaintedNets_) + " tainted net(s); root cause(s): ";
    for (size_t i = 0; i < taintRoots_.size(); ++i) {
        if (i) s += "; ";
        const int id = taintRoots_[i];
        auto it = taintReason_.find(id);
        s += nl_->isValidNetId(id) ? nl_->getNet(id).name : std::to_string(id);
        if (it != taintReason_.end()) s += " (" + it->second + ")";
    }
    if (numTaintedNets_ > taintRoots_.size()) s += "; additional roots omitted";
    return s;
}

void AigModel::do_build(const Options& opt) {
    const Netlist& nl   = *nl_;
    const int netCount  = static_cast<int>(nl.getNetCount());
    const int gateCount = static_cast<int>(nl.getGateCount());

    std::vector<Sig>  netSig(netCount, aig_.get_constant(false));
    std::vector<char> hasSig(netCount, 0);
    std::vector<char> netReady(netCount, 0);

    // ---- 0. 局部可信度（cone-local health）----
    // 污染記在 net 上而非 AIG node 上。
    // 理由：輸入無法解析時會塞 const0 進去，而 create_and(x, const0) 會塌成常數、
    // create_xor(x, const0) 會塌成 x 本身。若把污染記在結果節點，前者會污染全電路
    // 共用的常數節點，後者會污染 x 的整個 fanout —— 兩種都會造成大範圍誤判。
    //
    // 不變量：一條 net 未被污染 ⟺ 有唯一合法 driver、arity 正確、且所有輸入 net 皆未污染。
    // 由歸納法，未污染的 net 其整個 fanin cone 必定乾淨，可以放心證明。
    netTaint_.assign(netCount, 0);

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
            // 正常：Q 是自由變數
            info.qPiIndex = static_cast<int>(piNetIds_.size());
            Sig q = aig_.create_pi();
            assign(gate.outputNetId, q);
            info.q = q;
            piNetIds_.push_back(gate.outputNetId);
        } else if (gate.outputNetId >= 0 && gate.outputNetId < netCount) {
            // Q 已經有 driver：不建 pseudo-PI。
            // 組合抽象要求 Q 是自由變數，這裡不成立 —— 硬塞一個 PI 反而會
            // 讓 miter 拿到一個與實際電路無關的變數。qPiIndex 維持 -1。
            taint_root(gate.outputNetId,
                       "DFF '" + gate.instName + "' Q output has multiple drivers");
            info.q = netSig[gate.outputNetId];
        } else {
            // 沒有有效的 Q net：同樣不建 PI，也不佔位。
            mark_invalid("DFF '" + gate.instName + "' has no valid Q output net");
            info.q = aig_.get_constant(false);
        }
        dffs_.push_back(info);
        ++stats_.num_dff;
    }

    // ---- 4. 懸空輸入 → 自由 PI（net id 遞增，順序穩定）----
    // 綁 const0 會讓行為不同的電路被誤判等價，所以預設造自由變數。
    // 自由變數是保守但「正確」的建模，因此 **不算污染**，只把全域健康降為 Conservative。
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
    //
    // sig_of_input 必須放在迴圈「內部」：它要知道當前這顆閘是誰，
    // 才能把污染記到這顆閘的輸出 net 上，並從輸入 net 繼承污染。
    const std::vector<int> order = topo_order(nl, netReady);

    for (const int gid : order) {
        const Gate& g = nl.getGate(gid);

        // 這顆閘的輸出函數是否已被替代值或上游污染。
        bool        tainted = false;      // 含「從上游繼承」
        std::string taintReason;          // 非空 = 這顆閘本身就是污染源

        auto note_root = [&](const std::string& r) {
            tainted = true;
            if (taintReason.empty()) taintReason = r;
        };

        auto sig_of_input = [&](size_t idx) -> Sig {
            if (idx >= g.inputNetIds.size()) {
                ++stats_.num_unresolved;
                note_root("gate '" + g.instName + "' is missing required input " +
                          std::to_string(idx));
                return aig_.get_constant(false);
            }
            const int netId = g.inputNetIds[idx];
            if (netId < 0 || netId >= netCount || !hasSig[netId]) {
                ++stats_.num_unresolved;
                note_root("gate '" + g.instName + "' input " + std::to_string(idx) +
                          " cannot be resolved");
                if (opt.verbose)
                    std::cerr << "[AigBuilder][WARN] gate '" << g.instName
                              << "' input " << idx << " unresolved; tied to const0\n";
                return aig_.get_constant(false);
            }
            // ★ 污染沿 fanout 傳播：輸入不可信 → 輸出也不可信
            if (netTaint_[netId] != 0) tainted = true;
            return netSig[netId];
        };

        auto invalid_arity = [&](const std::string& expected) {
            note_root("gate '" + g.instName + "' has " +
                      std::to_string(g.inputNetIds.size()) +
                      " input(s); expected " + expected);
            return aig_.get_constant(false);
        };

        // 三個 fold 共用一份實作，避免重複。
        auto fold = [&](auto op) -> Sig {
            if (g.inputNetIds.empty()) return invalid_arity("at least one");
            Sig result = sig_of_input(0);
            for (size_t i = 1; i < g.inputNetIds.size(); ++i)
                result = op(result, sig_of_input(i));
            return result;
        };
        auto op_and = [&](Sig a, Sig b) { return aig_.create_and(a, b); };
        auto op_or  = [&](Sig a, Sig b) { return aig_.create_or (a, b); };
        auto op_xor = [&](Sig a, Sig b) { return aig_.create_xor(a, b); };

        Sig out = aig_.get_constant(false);
        switch (g.type) {
            case GateType::AND:  out =  fold(op_and); break;
            case GateType::OR:   out =  fold(op_or);  break;
            case GateType::NAND: out = !fold(op_and); break;
            case GateType::NOR:  out = !fold(op_or);  break;
            case GateType::XOR:  out =  fold(op_xor); break;
            case GateType::XNOR: out = !fold(op_xor); break;
            case GateType::NOT:
                out = g.inputNetIds.size() == 1
                    ? !sig_of_input(0)
                    : invalid_arity("exactly one");
                break;
            case GateType::BUF:
                out = g.inputNetIds.size() == 1
                    ? sig_of_input(0)
                    : invalid_arity("exactly one");
                break;
            default:
                note_root("gate '" + g.instName + "' has unsupported gate type");
                break;
        }

        if (g.outputNetId < 0 || g.outputNetId >= netCount) {
            // 沒有輸出 net：無處可記污染。下游因為讀不到這條 net，
            // 會在各自的 sig_of_input 裡被標成 unresolved，所以不會漏。
            mark_invalid("gate '" + g.instName + "' has no valid output net");
        } else if (hasSig[g.outputNetId]) {
            taint_root(g.outputNetId,
                       "net '" + nl.getNet(g.outputNetId).name + "' has multiple drivers");
        } else {
            assign(g.outputNetId, out);
            if (tainted) {
                if (!taintReason.empty()) taint_root(g.outputNetId, taintReason);
                else                      taint_derived(g.outputNetId);   // 純繼承，不重複記全域
            }
        }
        ++stats_.num_comb_gates;
    }

    // 被拓撲排序丟掉的閘（組合迴路 / 斷掉的 fanin 鏈）：
    // 逐一把輸出 net 標為污染源，讓診斷能指到具體位置，
    // 而不是只給一個「有 N 顆閘排不進去」的總數。
    {
        std::vector<char> inOrder(gateCount, 0);
        for (const int gid : order) inOrder[gid] = 1;

        int dropped = 0;
        for (int g = 0; g < gateCount; ++g) {
            if (!is_comb_gate(nl.getGate(g).type)) continue;
            if (inOrder[g]) continue;
            ++dropped;
            const int outNet = nl.getGate(g).outputNetId;
            if (outNet >= 0 && outNet < netCount) {
                taint_root(outNet, "gate '" + nl.getGate(g).instName +
                                   "' could not be topologically ordered "
                                   "(combinational loop?)");
            } else {
                mark_invalid("gate '" + nl.getGate(g).instName +
                             "' could not be topologically ordered");
            }
        }
        stats_.num_topo_dropped = static_cast<uint32_t>(dropped);
        /*if (dropped > 0)
            std::cerr << "[AigBuilder][WARN] " << dropped
                      << " combinational gate(s) unresolved (combinational loop?)\n";*/
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
                taint_root(netId, "primary output net '" + nl.getNet(netId).name +
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
    //
    // DFF 的污染記在 dffTaint_（與 dffs_ 對齊）而非 net 上，
    // 因為 "$D:inst" 這個比較點沒有對應的具名 net。
    dffTaint_.assign(dffs_.size(), 0);

    for (size_t i = 0; i < dffGateIds.size(); ++i) {
        DffInfo&    info = dffs_[i];
        const Gate& dff  = nl.getGate(info.gateId);
        bool        tainted = false;

        auto ctrl_sig = [&](int netId, bool defaultVal, const char* pin) -> Sig {
            // pin 本來就沒接：合法，依 cell 定義使用 inactive value，不算污染。
            if (netId == kNoNet)
                return aig_.get_constant(defaultVal);
            // pin 有接但解不出來：模型不完整。
            if (netId < 0 || netId >= netCount || !hasSig[netId]) {
                ++stats_.num_unresolved;
                mark_invalid("DFF '" + dff.instName + "' " + pin +
                             " input cannot be resolved");
                tainted = true;
                return aig_.get_constant(defaultVal);
            }
            if (netTaint_[netId] != 0) tainted = true;
            return netSig[netId];
        };

        if (info.dNetId < 0 || info.dNetId >= netCount || !hasSig[info.dNetId]) {
            ++stats_.num_unresolved;
            mark_invalid("DFF '" + dff.instName + "' D input cannot be resolved");
            tainted = true;
            info.d_raw = aig_.get_constant(false);
        } else {
            if (netTaint_[info.dNetId] != 0) tainted = true;
            info.d_raw = netSig[info.dNetId];
        }

        if (opt.fold_async_controls) {
            const Sig rn = ctrl_sig(info.rnNetId, true, "RN");   // 未接 → 不 reset
            const Sig sn = ctrl_sig(info.snNetId, true, "SN");   // 未接 → 不 set
            info.d_eff = aig_.create_and(rn, aig_.create_or(info.d_raw, !sn));
        } else {
            info.d_eff = info.d_raw;
        }

        // Q 本身不可信（多 driver）→ "$Q:" 與 "$D:" 都不可信。
        if (info.qNetId >= 0 && info.qNetId < netCount && netTaint_[info.qNetId] != 0)
            tainted = true;

        dffTaint_[i] = tainted ? 1 : 0;
        aig_.create_po(info.d_eff);
    }

    stats_.aig_size = aig_.size();

    if (opt.verbose) {
        std::cerr << "[AigBuilder] PI=" << stats_.num_real_pis
                  << " DFF=" << stats_.num_dff
                  << " freePI=" << stats_.num_free_pis
                  << " PO=" << stats_.num_real_pos
                  << " bound_nets=" << stats_.num_bound_nets
                  << " aig_size=" << stats_.aig_size
                  << " tainted_nets=" << numTaintedNets_ << "\n";
        if (numTaintedNets_ > 0)
            std::cerr << "[AigBuilder] " << taint_summary() << "\n";
    }

    // 自檢：pi_net_ids 必須與 AIG 的 PI 數一致。
    // 不一致代表某條路徑建了 PI 卻沒登記（或反之），
    // 而後果是反例解讀與比較點命名整批錯位 —— 不會 crash，只會靜默給錯答案。
    if (piNetIds_.size() != aig_.num_pis()) {
        mark_invalid("internal: pi_net_ids size " + std::to_string(piNetIds_.size()) +
                     " != aig.num_pis " + std::to_string(aig_.num_pis()));
    }
}

} // namespace eqeng
