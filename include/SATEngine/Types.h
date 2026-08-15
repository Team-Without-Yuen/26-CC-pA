#pragma once

#include <cstdint>
#include <vector>
#include <mockturtle/networks/aig.hpp>
#include <kitty/dynamic_truth_table.hpp>

namespace eqeng {

using Ntk  = mockturtle::aig_network;
using Sig  = Ntk::signal;
using Node = Ntk::node;
using TruthTable = kitty::dynamic_truth_table;

class Primitives;   // SigRef / Cut 需要 friend

// 無效的 generation。預設建構的 SigRef 帶這個值，一經使用即被攔截。
constexpr uint32_t kInvalidGeneration = 0;

// 高階 API 拿不到 raw Sig，因此「跨修改重用」從可偵測變成不可能。
// 真的需要 raw 時走 Primitives::raw() / wrap()，那是有意識的逃生口。
class SigRef {
public:
    SigRef() = default;

    bool     valid()      const { return gen_ != kInvalidGeneration; }
    uint32_t generation() const { return gen_; }

    // 這個 signal 的函數是否已被「無法解析的輸入」污染。
    // 污染會沿著 cofactor / make_* 等衍生運算傳播。
    bool     tainted()    const { return tainted_; }

    // 取反：只翻 complement bit，generation 不變。
    SigRef operator!() const { return SigRef(!sig_, gen_, tainted_); }

    // 同一版之內才有比較意義；跨版比較一律回 false（而不是假裝相等）。
    bool operator==(const SigRef& o) const {
        return gen_ == o.gen_ && gen_ != kInvalidGeneration && sig_ == o.sig_;
    }
    bool operator!=(const SigRef& o) const { return !(*this == o); }

private:
    friend class Primitives;
    SigRef(Sig s, uint32_t gen, bool tainted = false)
        : sig_(s), gen_(gen), tainted_(tainted) {}

    Sig      sig_{};
    uint32_t gen_ = kInvalidGeneration;
    bool     tainted_ = false;
};

// k-feasible cut。
//   帶 generation：cut 裡的 Node 同樣依附於某一版 AIG，
//   跨 rebuild 使用與 stale SigRef 是同一類錯誤。
struct Cut {
    Node              root;
    std::vector<Node> leaves;
    uint32_t          generation = kInvalidGeneration;
    bool              tainted    = false;   // 由 root 繼承
};

struct NpnClass {
    TruthTable           canonical;
    uint32_t             num_vars = 0;
    uint32_t             phase    = 0;
    std::vector<uint8_t> perm;
};

enum class SatResult  : uint8_t { Unsat = 0, Sat = 1, Unknown = 2 };
enum class EquivResult: uint8_t { Equal = 0, NotEqual = 1, Unknown = 2 };

inline const char* to_string(EquivResult r) {
    switch (r) {
        case EquivResult::Equal:    return "equal";
        case EquivResult::NotEqual: return "not_equal";
        default:                    return "unknown";
    }
}

// 等價類報告。
// 常數類與等價類分開回報，語意不同：
//     「這些 net 恆為 0」  ≠  「這些 net 彼此等價」
//   而且 FRAIG sweep 後常數類通常大到會淹沒真正有意思的等價類。
struct EquivClassReport {
    // 彼此功能等價的 net 分組（不含常數）。
    // 一個 net 與其反相會落在不同組（各自等價於 f 與 !f）。
    std::vector<std::vector<std::string>> equivalence_classes;

    // 恆為 0 / 恆為 1 的 net。
    std::vector<std::string> constant_zero;
    std::vector<std::string> constant_one;

    // 因建模不可信而被排除的 net 數（不列名字，避免大電路撐爆報告）。
    // > 0 時代表這份報告不完整。
    std::size_t excluded_untrusted = 0;

    // Phase A = 只反映結構共享；Phase B sweep 後才是完整的功能等價類。
    bool is_complete = false;
};

} // namespace eqeng
