// src/main.cpp
//
// 互動式引擎測試台：讀一次 Verilog，之後在同一個 session 裡連續下指令。
// 重點在於能實際走完「snapshot → 修改 → cec」這條 §5.5 的流程，
// 並觀察 §5.4 的 dirty / generation 行為（rebuild 何時發生、舊 SigRef 何時被攔）。
//
// 用法：
//   ./eqeng <input.v>                 進入互動模式
//   ./eqeng <input.v> -c "cmd; cmd"   執行分號分隔的指令後離開
//   ./eqeng <input.v> < script.txt    從檔案餵指令
//
// 指令一覽見 help。

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <kitty/kitty.hpp>

#include "include/SATEngine/Primitives.h"
#include "include/core/Netlist.h"
#include "include/io/VerilogReader.h"

using namespace eqeng;

namespace {

// ---------------- 小工具 ----------------

class Timer {
public:
    Timer() : t0_(std::chrono::steady_clock::now()) {}
    double lap() {
        const auto now = std::chrono::steady_clock::now();
        const double d = std::chrono::duration<double>(now - t0_).count();
        t0_ = now;
        return d;
    }
private:
    std::chrono::steady_clock::time_point t0_;
};

std::vector<std::string> split_ws(const std::string& s) {
    std::istringstream is(s);
    std::vector<std::string> out;
    std::string tok;
    while (is >> tok) out.push_back(tok);
    return out;
}

std::vector<std::string> split_char(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : s) {
        if (c == sep) { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string join(std::vector<std::string> v, std::size_t cap = 12) {
    std::sort(v.begin(), v.end());
    std::string s = "{";
    for (std::size_t i = 0; i < v.size() && i < cap; ++i) {
        if (i) s += ", ";
        s += v[i];
    }
    if (v.size() > cap) s += ", ...+" + std::to_string(v.size() - cap);
    return s + "}";
}

// ---------------- REPL 狀態 ----------------

struct Session {
    Netlist&    nl;
    Primitives& prim;
    AigSnapshot snap;          // 最近一次 snapshot
    bool        hasSnap = false;
};

void print_help() {
    std::cout << 
        "\n可用指令：\n"
        "  info                        建模摘要與健全性\n"
        "  gen                         目前 generation / dirty / rebuild 次數\n"
        "  equiv <netA> <netB>         功能等價\n"
        "  const <net>                 是否恆為 0 / 1\n"
        "  support <net>               functional support\n"
        "  depends <net> <var>         是否功能相依\n"
        "  cuts <net> [k]              枚舉 cut + NPN 辨識（mux / maj3 / xor3）\n"
        "  names <net>                 這條 net 所屬的名字群組\n"
        "  classes [n]                 結構等價類（至少 n 條，預設 2）\n"
        "  points                      列出所有 CEC 比較點\n"
        "\n"
        "  snapshot                    ★ 修改前拍照\n"
        "  cec                         與快照比對（全比較點）\n"
        "  cec-except <p1> <p2> ...    排除指定比較點後比對\n"
        "  cec-only <p1> <p2> ...      只比對指定比較點\n"
        "\n"
        "  edit-buf <net>              在 net 的下游插一對 NOT（純重構，功能不變）\n"
        "  edit-swap <gate> <from> <to>  把 gate 的某個輸入從 from 改接 to（會改變功能）\n"
        "  edit-rename <old> <new>     改 net 名稱\n"
        "\n"
        "  stale                       示範：拿修改前的舊 SigRef 會被攔下\n"
        "  stats                       引擎統計\n"
        "  help / quit\n\n";
}

// ---------------- 指令實作 ----------------

void cmd_info(Session& s) {
    auto& model = s.prim.model();          // 會觸發 ensure_fresh
    const auto& st = model.stats();
    std::cout << "  AIG size   : " << st.aig_size << "\n"
              << "  real PIs   : " << st.num_real_pis << "\n"
              << "  DFFs       : " << st.num_dff << "\n"
              << "  free PIs   : " << st.num_free_pis << "\n"
              << "  real POs   : " << st.num_real_pos << "\n"
              << "  bound nets : " << st.num_bound_nets
              << " / " << s.nl.getNetCount() << "\n"
              << "  comb gates : " << st.num_comb_gates << "\n";
    if (st.num_topo_dropped > 0 || st.num_unresolved > 0) {
        std::cout << "  ** NOT SOUND: " << st.num_topo_dropped
                  << " gate(s) unresolved, " << st.num_unresolved
                  << " input(s) tied to const0 -- results unreliable\n";
    } else {
        std::cout << "  soundness  : OK\n";
    }
}

void cmd_gen(Session& s) {
    std::cout << "  generation : " << s.prim.generation() << "\n"
              << "  dirty      : " << (s.nl.isDirty() ? "yes" : "no") << "\n"
              << "  revision   : " << s.nl.revision() << "\n"
              << "  rebuilds   : " << s.prim.stats().rebuilds
              << "  (" << std::fixed << std::setprecision(2)
              << s.prim.stats().rebuild_seconds << "s total)\n"
              << "  snapshot   : " << (s.hasSnap ? "held" : "none");
    if (s.hasSnap)
        std::cout << "  (rev=" << s.snap.revision()
                  << ", " << s.snap.num_outputs() << " points)";
    std::cout << "\n";
}

void cmd_equiv(Session& s, const std::string& a, const std::string& b) {
    auto sa = s.prim.try_resolve(a);
    auto sb = s.prim.try_resolve(b);
    if (!sa) { std::cout << "  net not found: " << a << "\n"; return; }
    if (!sb) { std::cout << "  net not found: " << b << "\n"; return; }

    Timer t;
    const auto r = s.prim.equiv_checked(*sa, *sb);
    std::cout << "  -> " << to_string(r) << "  (" << t.lap() << "s)\n";
}

void cmd_const(Session& s, const std::string& net) {
    auto sig = s.prim.try_resolve(net);
    if (!sig) { std::cout << "  net not found\n"; return; }

    Timer t;
    const auto r0 = s.prim.is_const_checked(*sig, false);
    const auto r1 = s.prim.is_const_checked(*sig, true);
    std::cout << "  -> const0: " << to_string(r0)
              << " | const1: " << to_string(r1)
              << "  (" << t.lap() << "s)\n";
}

void cmd_support(Session& s, const std::string& net) {
    auto sig = s.prim.try_resolve(net);
    if (!sig) { std::cout << "  net not found\n"; return; }

    Timer t;
    auto sup = s.prim.functional_support(*sig);
    const double sec = t.lap();

    std::vector<std::string> nm;
    for (const auto& v : sup) {
        auto n = s.prim.names_of(v);
        nm.push_back(n.empty() ? "<unnamed PI>" : n.front());
    }
    std::cout << "  -> " << sup.size() << " var(s) " << join(nm, 24)
              << "  (" << sec << "s)\n";
}

void cmd_depends(Session& s, const std::string& net, const std::string& var) {
    auto f = s.prim.try_resolve(net);
    auto v = s.prim.try_resolve(var);
    if (!f) { std::cout << "  net not found: " << net << "\n"; return; }
    if (!v) { std::cout << "  net not found: " << var << "\n"; return; }
    if (!s.prim.is_free_var(*v)) {
        std::cout << "  '" << var << "' is not a PI (free variable); "
                     "depends_on requires one\n";
        return;
    }
    Timer t;
    const auto r = s.prim.depends_on_checked(*f, *v);
    // Equal = 兩個 cofactor 相同 = 不相依
    std::cout << "  -> " << (r == EquivResult::NotEqual ? "depends"
                            : r == EquivResult::Equal   ? "independent"
                                                        : "unknown")
              << "  (" << t.lap() << "s)\n";
}

void cmd_cuts(Session& s, const std::string& net, int k) {
    auto sig = s.prim.try_resolve(net);
    if (!sig) { std::cout << "  net not found\n"; return; }
    if (s.prim.is_free_var(*sig) || s.prim.is_constant(*sig)) {
        std::cout << "  net is a PI/constant; no cuts\n";
        return;
    }

    kitty::dynamic_truth_table tMux(3), tMaj(3), tXor3(3);
    kitty::create_from_hex_string(tMux,  "d8");   // s ? b : a
    kitty::create_from_hex_string(tMaj,  "e8");   // MAJ3 = carry
    kitty::create_from_hex_string(tXor3, "96");   // XOR3 = sum

    Timer t;
    auto cuts = s.prim.enumerate_cuts(*sig, k);
    const double sec = t.lap();
    std::cout << "  -> " << cuts.size() << " cut(s)  (" << sec << "s)\n";

    std::size_t shown = 0;
    for (const auto& c : cuts) {
        if (shown++ >= 8) break;
        std::string tag;
        if (c.leaves.size() == 3) {
            if      (s.prim.npn_matches(c, tMux))  tag = "  <- MUX";
            else if (s.prim.npn_matches(c, tMaj))  tag = "  <- MAJ3 (carry)";
            else if (s.prim.npn_matches(c, tXor3)) tag = "  <- XOR3 (sum)";
        }
        std::string hex;
        try { hex = kitty::to_hex(s.prim.truth_of(c, *sig)); }
        catch (const std::exception&) { hex = "?"; }

        std::cout << "     [" << c.leaves.size() << "] "
                  << join(s.prim.cut_leaf_names(c), 6)
                  << "  tt=0x" << hex << tag << "\n";
    }
}

void cmd_names(Session& s, const std::string& net) {
    auto sig = s.prim.try_resolve(net);
    if (!sig) { std::cout << "  net not found\n"; return; }
    std::cout << "  -> " << join(s.prim.names_of(*sig), 24) << "\n";
}

void cmd_classes(Session& s, int minSize) {
    Timer t;
    auto cls = s.prim.equivalence_classes(minSize);
    const double sec = t.lap();
    std::cout << "  -> " << cls.size() << " group(s)  (" << sec << "s)\n";
    for (std::size_t i = 0; i < cls.size() && i < 10; ++i)
        std::cout << "     [" << cls[i].size() << "] " << join(cls[i]) << "\n";
    if (cls.size() > 10)
        std::cout << "     ... (" << cls.size() - 10 << " more)\n";
}

void cmd_points(Session& s) {
    auto pts = s.prim.comparison_points();
    std::cout << "  -> " << pts.size() << " comparison point(s)\n";
    for (std::size_t i = 0; i < pts.size() && i < 20; ++i)
        std::cout << "     " << pts[i] << "\n";
    if (pts.size() > 20)
        std::cout << "     ... (" << pts.size() - 20 << " more)\n";
}

void print_cec(const CecResult& r) {
    std::cout << "  -> " << to_string(r.status) << " : " << r.message << "\n"
              << "     compared : " << r.compared_outputs.size() << " point(s)\n";

    if (!r.mismatched_outputs.empty())
        std::cout << "     mismatch : " << join(r.mismatched_outputs, 12) << "\n";

    if (!r.counterexample.empty()) {
        std::cout << "     counterexample (first 10): ";
        for (std::size_t i = 0; i < r.counterexample.size() && i < 10; ++i)
            std::cout << r.counterexample[i].first << "="
                      << (r.counterexample[i].second ? 1 : 0) << " ";
        std::cout << "\n";
    }
    if (r.interface_mismatch) {
        std::cout << "     interface changed:\n";
        if (!r.outputs_only_in_before.empty())
            std::cout << "       removed : " << join(r.outputs_only_in_before) << "\n";
        if (!r.outputs_only_in_after.empty())
            std::cout << "       added   : " << join(r.outputs_only_in_after) << "\n";
    }
}

void cmd_snapshot(Session& s) {
    Timer t;
    s.snap    = s.prim.snapshot();
    s.hasSnap = true;
    std::cout << "  -> captured " << s.snap.num_outputs() << " comparison point(s), "
              << s.snap.num_inputs() << " input(s), rev=" << s.snap.revision()
              << "  (" << t.lap() << "s)\n";
}

void cmd_cec(Session& s, const std::vector<std::string>& names, int mode) {
    if (!s.hasSnap) {
        std::cout << "  no snapshot held -- run 'snapshot' before editing\n";
        return;
    }
    Timer t;
    CecResult r;
    if      (mode == 1) r = s.prim.equiv_to_snapshot_except(s.snap, names);
    else if (mode == 2) r = s.prim.equiv_to_snapshot_only(s.snap, names);
    else                r = s.prim.equiv_to_snapshot(s.snap);
    const double sec = t.lap();
    print_cec(r);
    std::cout << "     time     : " << sec << "s\n";
}

// ---------------- 修改指令 ----------------
// 只做最少的幾種，目的是製造「應等價」與「應不等價」兩類修改來驗 CEC。

void cmd_edit_buf(Session& s, const std::string& net) {
    const int src = s.nl.getNetId(net);
    if (src < 0) { std::cout << "  net not found\n"; return; }

    const Net& srcNet = s.nl.getNet(src);
    if (srcNet.isPO) {
        std::cout << "  '" << net << "' is a PO; inserting here would need a "
                     "different rewiring path. Pick an internal net.\n";
        return;
    }
    const auto loads = srcNet.loadGateIds;      // 先複製，等下會改到原 vector
    if (loads.empty()) { std::cout << "  net has no loads; nothing to do\n"; return; }

    static int uid = 0;
    const std::string tag = "_bufins" + std::to_string(uid++);

    const int n1g = s.nl.addGate("bi_n1" + tag, GateType::NOT);
    const int n1  = s.nl.addNet("bi_t1" + tag);
    s.nl.connectGateInput(n1g, src);
    s.nl.connectGateOutput(n1g, n1);

    const int n2g = s.nl.addGate("bi_n2" + tag, GateType::NOT);
    const int n2  = s.nl.addNet("bi_t2" + tag);
    s.nl.connectGateInput(n2g, n1);
    s.nl.connectGateOutput(n2g, n2);

    // 原本吃 src 的閘（不含剛插入的 n1g）改吃 n2
    int moved = 0;
    for (const int gid : loads) {
        if (gid == n1g) continue;
        Gate& g = s.nl.getGateMutable(gid);
        if (g.type == GateType::UNKNOWN) continue;
        for (auto& in : g.inputNetIds) if (in == src) { in = n2; ++moved; }
        s.nl.getNetMutable(n2).loadGateIds.push_back(gid);
    }
    Net& from = s.nl.getNetMutable(src);
    auto& L = from.loadGateIds;
    L.erase(std::remove_if(L.begin(), L.end(),
                           [&](int gid) { return gid != n1g; }), L.end());

    std::cout << "  inserted NOT-NOT after '" << net << "', rerouted "
              << moved << " load(s). Function should be UNCHANGED.\n";
}

void cmd_edit_swap(Session& s, const std::string& gate,
                   const std::string& from, const std::string& to) {
    const int gid = s.nl.getGateId(gate);
    const int f   = s.nl.getNetId(from);
    const int t   = s.nl.getNetId(to);
    if (gid < 0) { std::cout << "  gate not found: " << gate << "\n"; return; }
    if (f < 0)   { std::cout << "  net not found: " << from << "\n"; return; }
    if (t < 0)   { std::cout << "  net not found: " << to << "\n"; return; }

    Gate& g = s.nl.getGateMutable(gid);
    int n = 0;
    for (auto& in : g.inputNetIds) if (in == f) { in = t; ++n; }
    if (n == 0) { std::cout << "  gate does not read '" << from << "'\n"; return; }

    Net& fn = s.nl.getNetMutable(f);
    fn.loadGateIds.erase(
        std::remove(fn.loadGateIds.begin(), fn.loadGateIds.end(), gid),
        fn.loadGateIds.end());
    s.nl.getNetMutable(t).loadGateIds.push_back(gid);

    std::cout << "  rewired " << gate << ": " << from << " -> " << to
              << " (" << n << " pin(s)). Function likely CHANGED.\n";
}

void cmd_edit_rename(Session& s, const std::string& oldN, const std::string& newN) {
    if (!s.nl.renameNet(oldN, newN)) { std::cout << "  rename failed\n"; return; }
    std::cout << "  renamed '" << oldN << "' -> '" << newN << "'\n"
              << "  (note: NameMap is rebuilt on the next query -- "
                 "old names stop resolving)\n";
}

void cmd_stale(Session& s) {
    // 示範 §5.4：修改前拿的 SigRef，修改後使用會被攔下。
    auto any = s.prim.try_resolve("__none__");
    (void)any;

    std::string probe;
    for (std::size_t i = 0; i < s.nl.getNetCount() && probe.empty(); ++i) {
        const Net& n = s.nl.getNet(i);
        if (!n.isRemoved && !n.isConst && s.prim.try_resolve(n.name))
            probe = n.name;
    }
    if (probe.empty()) { std::cout << "  no usable net\n"; return; }

    const SigRef old = s.prim.resolve(probe);
    const uint32_t g0 = s.prim.generation();
    std::cout << "  held SigRef for '" << probe << "' at generation " << g0 << "\n";

    // 製造一次修改
    static int uid = 0;
    s.nl.addNet("__stale_probe" + std::to_string(uid++));
    std::cout << "  netlist modified (dirty=" << (s.nl.isDirty() ? "yes" : "no") << ")\n";

    try {
        (void)s.prim.equiv(old, old);
        std::cout << "  ** NOT caught -- stale detection is broken **\n";
    } catch (const StaleSignal&) {
        std::cout << "  StaleSignal thrown as expected; generation is now "
                  << s.prim.generation() << "\n"
                  << "  fix: call resolve(name) again after any edit\n";
    }
}

void cmd_stats(Session& s) {
    const auto& st = s.prim.stats();
    std::cout << "  equiv calls : " << st.equiv_calls
              << "  (trivial " << st.equiv_trivial
              << ", lookup " << st.equiv_by_lookup
              << ", sat " << st.equiv_by_sat
              << ", unknown " << st.equiv_unknown << ")\n"
              << "  miters      : " << st.miters_built << "\n"
              << "  cofactor nodes : " << st.cofactors_built << "\n"
              << "  cut enums   : " << st.cut_enumerations << "\n"
              << "  rebuilds    : " << st.rebuilds
              << "  (" << std::fixed << std::setprecision(2)
              << st.rebuild_seconds << "s)\n"
              << "  snapshots   : " << st.snapshots_taken
              << "  | cec runs : " << st.cec_runs << "\n"
              << "  stale rejected : " << st.stale_rejected << "\n";
    if (st.equiv_unknown > 0)
        std::cout << "  [note] Unknown results mean Phase A hit its internal limits;\n"
                     "         this is what Phase B (incremental CaDiCaL) fixes.\n";
}

// ---------------- 指令分派 ----------------

bool run_command(Session& s, const std::string& line) {
    const auto tok = split_ws(line);
    if (tok.empty()) return true;
    const std::string& cmd = tok[0];

    auto need = [&](std::size_t n) {
        if (tok.size() < n) {
            std::cout << "  usage error -- see 'help'\n";
            return false;
        }
        return true;
    };

    try {
        if (cmd == "quit" || cmd == "exit" || cmd == "q") return false;
        else if (cmd == "help" || cmd == "?")   print_help();
        else if (cmd == "info")                 cmd_info(s);
        else if (cmd == "gen")                  cmd_gen(s);
        else if (cmd == "equiv")   { if (need(3)) cmd_equiv(s, tok[1], tok[2]); }
        else if (cmd == "const")   { if (need(2)) cmd_const(s, tok[1]); }
        else if (cmd == "support") { if (need(2)) cmd_support(s, tok[1]); }
        else if (cmd == "depends") { if (need(3)) cmd_depends(s, tok[1], tok[2]); }
        else if (cmd == "cuts")    { if (need(2)) cmd_cuts(s, tok[1],
                                        tok.size() > 2 ? std::stoi(tok[2]) : 0); }
        else if (cmd == "names")   { if (need(2)) cmd_names(s, tok[1]); }
        else if (cmd == "classes") cmd_classes(s, tok.size() > 1 ? std::stoi(tok[1]) : 2);
        else if (cmd == "points")  cmd_points(s);
        else if (cmd == "snapshot") cmd_snapshot(s);
        else if (cmd == "cec")      cmd_cec(s, {}, 0);
        else if (cmd == "cec-except") {
            if (need(2)) cmd_cec(s, {tok.begin() + 1, tok.end()}, 1);
        }
        else if (cmd == "cec-only") {
            if (need(2)) cmd_cec(s, {tok.begin() + 1, tok.end()}, 2);
        }
        else if (cmd == "edit-buf")    { if (need(2)) cmd_edit_buf(s, tok[1]); }
        else if (cmd == "edit-swap")   { if (need(4)) cmd_edit_swap(s, tok[1], tok[2], tok[3]); }
        else if (cmd == "edit-rename") { if (need(3)) cmd_edit_rename(s, tok[1], tok[2]); }
        else if (cmd == "stale")   cmd_stale(s);
        else if (cmd == "stats")   cmd_stats(s);
        else std::cout << "  unknown command: " << cmd << "  (try 'help')\n";
    }
    catch (const StaleSignal& e) {
        std::cout << "  [StaleSignal] " << e.what() << "\n";
    }
    catch (const std::exception& e) {
        std::cout << "  [error] " << e.what() << "\n";
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <input.v> [-c \"cmd; cmd; ...\"]\n";
        return 1;
    }
    const std::string inputFilePath = argv[1];

    std::string batch;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-c" && i + 1 < argc) batch = argv[++i];
    }

    // ---------- 讀 Verilog ----------
    Netlist myCircuit;
    VerilogReader reader;
    std::cout << "[Step 1] Reading Verilog...\n";
    Timer t;
    if (!reader.read(inputFilePath, myCircuit)) {
        std::cerr << "[Error] Failed to read Verilog file!\n";
        return -1;
    }
    std::cout << "  -> Gates: " << myCircuit.getGateCount()
              << " | Nets: "    << myCircuit.getNetCount()
              << " | " << std::fixed << std::setprecision(2) << t.lap() << "s\n";

    // ---------- 建引擎（lazy：AIG 等第一個指令才建）----------
    AigModel::Options bopt;
    bopt.fold_async_controls = true;
    bopt.undriven_as_free_pi = true;

    Primitives::Config cfg;
    cfg.verbose_rebuild = true;      // 看得到 rebuild 何時發生

    Primitives prim(myCircuit, bopt, cfg);
    Session sess{myCircuit, prim, AigSnapshot{}, false};

    // ---------- 批次模式 ----------
    if (!batch.empty()) {
        for (const auto& raw : split_char(batch, ';')) {
            const std::string c = trim(raw);
            if (c.empty()) continue;
            std::cout << "\n> " << c << "\n";
            if (!run_command(sess, c)) break;
        }
        return 0;
    }

    // ---------- 互動模式 ----------
    std::cout << "\n引擎就緒（AIG 尚未建立 —— 第一個指令才會觸發）。輸入 help 看指令。\n";
    std::string line;
    while (true) {
        std::cout << "\n> " << std::flush;
        if (!std::getline(std::cin, line)) break;   // EOF / 管線結束
        if (!run_command(sess, trim(line))) break;
    }

    std::cout << "\n";
    cmd_stats(sess);
    return 0;
}