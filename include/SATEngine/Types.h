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

    // 取反：只翻 complement bit，generation 不變。
    SigRef operator!() const { return SigRef(!sig_, gen_); }

    // 同一版之內才有比較意義；跨版比較一律回 false（而不是假裝相等）。
    bool operator==(const SigRef& o) const {
        return gen_ == o.gen_ && gen_ != kInvalidGeneration && sig_ == o.sig_;
    }
    bool operator!=(const SigRef& o) const { return !(*this == o); }

private:
    friend class Primitives;
    SigRef(Sig s, uint32_t gen) : sig_(s), gen_(gen) {}

    Sig      sig_{};
    uint32_t gen_ = kInvalidGeneration;
};

// k-feasible cut。
//   帶 generation：cut 裡的 Node 同樣依附於某一版 AIG，
//   跨 rebuild 使用與 stale SigRef 是同一類錯誤。
struct Cut {
    Node              root;
    std::vector<Node> leaves;
    uint32_t          generation = kInvalidGeneration;
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

} // namespace eqeng