// src/engine/EngineStubs.cpp
//
// Phase A 專用的連結佔位檔。
//
// 為什麼需要這個檔：
//   Primitives.cpp 對 SatEngine / Fraig 的呼叫全部包在 nullptr 檢查裡，
//   執行期在 Phase A 永遠不會進去 —— 但連結器不管執行期，
//   只要程式碼提到了那些符號就要求有定義。
//
// 為什麼不直接把呼叫註解掉：
//   那些呼叫點正是 Phase B 的接線位置。註解掉之後，
//   Phase B 上線時漏掉任何一處都不會報錯，只會靜默走慢路徑（或給錯答案）。
//   保留呼叫、用會爆炸的 stub 頂住，才能讓「忘了實作」變成可見的失敗。
//
// ★ Phase B 開始時：刪除本檔，換成真正的 SatEngine.cpp / Fraig.cpp。
//   Primitives.cpp 不需要任何修改。

#include "include/SATEngine/SatEngine.h"
#include "include/SATEngine/Fraig.h"

#include <iostream>
#include <stdexcept>

namespace eqeng {

namespace {

[[noreturn]] void not_implemented(const char* who) {
    std::cerr << "\n[FATAL] " << who << " called, but Phase B is not implemented yet.\n"
              << "        Phase A must never reach this code path — "
                 "construct Primitives with nullptr for sat/fraig.\n";
    throw std::logic_error(std::string(who) + ": not implemented (Phase A stub)");
}

} // namespace

// ============================================================
//  SatEngine — Phase A 佔位
// ============================================================

struct SatEngine::Impl {};   // 空實作，僅供 unique_ptr 完成型別

SatEngine::SatEngine(const Ntk&, Config) { not_implemented("SatEngine::SatEngine"); }
SatEngine::~SatEngine() = default;

EquivResult SatEngine::are_equal(Sig, Sig)          { not_implemented("SatEngine::are_equal"); }
EquivResult SatEngine::is_const(Sig, bool)          { not_implemented("SatEngine::is_const"); }
EquivResult SatEngine::are_equal_under(Sig, Sig,
        const std::vector<std::pair<Sig, bool>>&)   { not_implemented("SatEngine::are_equal_under"); }
SatResult   SatEngine::solve_for(Sig, bool)         { not_implemented("SatEngine::solve_for"); }

const Counterexample& SatEngine::last_counterexample() const {
    not_implemented("SatEngine::last_counterexample");
}

void SatEngine::assert_equal(Sig, Sig)   { not_implemented("SatEngine::assert_equal"); }
void SatEngine::assert_const(Sig, bool)  { not_implemented("SatEngine::assert_const"); }

void SatEngine::ensure_encoded(Sig)      { not_implemented("SatEngine::ensure_encoded(Sig)"); }
void SatEngine::ensure_encoded(Node)     { not_implemented("SatEngine::ensure_encoded(Node)"); }
bool SatEngine::is_encoded(Node) const   { not_implemented("SatEngine::is_encoded"); }

// sync() 是唯一的例外：Primitives::cofactor 每次都會呼叫它。
// 雖然目前一定是 sat_ == nullptr 而不會進去，但把它定義成 no-op
// 比較安全，也不會掩蓋任何錯誤（同步表本來就允許是空操作）。
void SatEngine::sync() {}

const SatEngine::Stats& SatEngine::stats() const { not_implemented("SatEngine::stats"); }
void SatEngine::reset_stats()                    { not_implemented("SatEngine::reset_stats"); }

// ============================================================
//  Fraig — Phase A 佔位
// ============================================================

struct Fraig::Impl {};

Fraig::Fraig(Ntk&, SatEngine&, Config) { not_implemented("Fraig::Fraig"); }
Fraig::~Fraig() = default;

void Fraig::sweep() { not_implemented("Fraig::sweep"); }

// is_swept() 定義成 false 而非爆炸：
// Primitives 的查表路徑寫成 `if (fraig_ != nullptr && fraig_->is_swept())`，
// 這裡回 false 就等於「還沒 sweep，請走 SAT」，語意完全正確。
bool Fraig::is_swept() const { return false; }

uint64_t Fraig::sweep_watermark() const      { not_implemented("Fraig::sweep_watermark"); }
bool     Fraig::is_swept_node(Node) const    { not_implemented("Fraig::is_swept_node"); }
Sig      Fraig::representative(Sig) const    { not_implemented("Fraig::representative"); }
bool     Fraig::same_class(Sig, Sig) const   { not_implemented("Fraig::same_class"); }
bool     Fraig::is_known_const(Sig, bool&) const { not_implemented("Fraig::is_known_const"); }

std::vector<EquivClass> Fraig::classes(int) const { not_implemented("Fraig::classes"); }

const std::vector<Sig>& Fraig::representatives_by_node() const {
    not_implemented("Fraig::representatives_by_node");
}

const Fraig::Stats& Fraig::stats() const { not_implemented("Fraig::stats"); }

} // namespace eqeng