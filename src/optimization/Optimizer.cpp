#include "include/core/Optimizer.h"
#include "include/SATEngine/SatTime.h"
#include "include/SATEngine/Primitives.h"
#include <limits>
#include <string>
#include <iostream>
#include <functional>
#include <algorithm>
#include <vector>
#include <queue>
#include <unordered_set>
#include <unordered_map>
#include <chrono>
#include <type_traits>

namespace {

bool samePorts(const std::vector<Port>& lhs, const std::vector<Port>& rhs) {
    if (lhs.size() != rhs.size()) return false;
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (lhs[i].name != rhs[i].name ||
            lhs[i].msb != rhs[i].msb ||
            lhs[i].lsb != rhs[i].lsb ||
            lhs[i].netIds != rhs[i].netIds) {
            return false;
        }
    }
    return true;
}

bool sameNetlistGraph(const Netlist& lhs, const Netlist& rhs) {
    if (lhs.getGateCount() != rhs.getGateCount() ||
        lhs.getNetCount() != rhs.getNetCount() ||
        !samePorts(lhs.getPrimaryInputs(), rhs.getPrimaryInputs()) ||
        !samePorts(lhs.getPrimaryOutputs(), rhs.getPrimaryOutputs())) {
        return false;
    }

    for (size_t i = 0; i < lhs.getGateCount(); ++i) {
        const Gate& a = lhs.getGate(static_cast<int>(i));
        const Gate& b = rhs.getGate(static_cast<int>(i));
        if (a.instName != b.instName ||
            a.type != b.type ||
            a.inputNetIds != b.inputNetIds ||
            a.inputPinNames != b.inputPinNames ||
            a.outputNetId != b.outputNetId) {
            return false;
        }
    }

    for (size_t i = 0; i < lhs.getNetCount(); ++i) {
        const Net& a = lhs.getNet(static_cast<int>(i));
        const Net& b = rhs.getNet(static_cast<int>(i));
        if (a.name != b.name ||
            a.driverGateId != b.driverGateId ||
            a.loadGateIds != b.loadGateIds ||
            a.isPI != b.isPI ||
            a.isPO != b.isPO ||
            a.isConst != b.isConst ||
            a.isRemoved != b.isRemoved ||
            a.constVal != b.constVal) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// 對「進入本 pass 前的快照」做一次組合等價驗證。
//
// 比較點靠名字對齊:真實 PI/PO 用 net 名,DFF 用 "$Q:"/"$D:" + instance 名。
// GenericLowering 保留了 PI/PO port 宣告與 DFF instance 名,所以兩側對得上;
// 內部 net 名被重新產生不影響比對。
// ---------------------------------------------------------------------------
struct EquivalenceOutcome {
    bool ran          = false;   // 驗證真的跑了
    bool equivalent   = false;   // 證明等價
    bool inconclusive = false;   // 跑了但下不了結論(逾時 / 污染 / 介面不符)
    bool skipped      = false;   // 根本沒開始(沒預算)—— 與「跑了但沒結論」分開
    // 只驗了乾淨子集合。equivalent 仍為 true,但涵蓋率不是 100%。
    bool partial      = false;
    std::size_t comparedPoints  = 0;
    std::size_t untrustedPoints = 0;
    std::string message;
};

EquivalenceOutcome verifyAgainstSnapshot(const Netlist& before,
                                         const Netlist& after,
                                         double budgetSeconds,
                                         bool verbose) {
    EquivalenceOutcome out;

    if (budgetSeconds <= 0.0) {
        out.skipped = true;                     // 不是 inconclusive
        out.message = "skipped: no time budget left for equivalence checking";
        return out;
    }

    const auto started = std::chrono::steady_clock::now();
    auto elapsed = [&] {
        return std::chrono::duration<double>(
                   std::chrono::steady_clock::now() - started).count();
    };

    try {
        // Primitives 預設 verbose_rebuild=true,會把 rebuild / remap / fraig
        //   每一行都印到 stderr,跟呼叫端的 verbose 完全脫鉤。
        eqeng::Primitives::Config pcfg;
        pcfg.verbose_rebuild = verbose;

        // 兩側各自建模。AigSnapshot 是自足的(比較點的 cone 重建進自己的 Ntk、
        // 名字以字串保存),所以 before 的快照可以拿到 after 的 Primitives 上比對。
        // primBefore 不需要 Phase B:snapshot() 只做 collect_interface +
        // copy_cone,完全不碰 SAT 或 FRAIG。
        eqeng::Primitives primBefore(before, {}, pcfg);
        eqeng::AigSnapshot snap = primBefore.snapshot();
        if (!snap.valid()) {
            out.inconclusive = true;
            out.message = "could not build a reference snapshot of the pre-optimization netlist";
            return out;
        }

        eqeng::Primitives primAfter(after, {}, pcfg);
        primAfter.enable_phase_b(true);

        {
            const auto ptsB = primBefore.comparison_points(true);
            const auto ptsA = primAfter.comparison_points(true);
            const std::unordered_set<std::string> setB(ptsB.begin(), ptsB.end());

            std::size_t common = 0;
            for (const auto& n : ptsA) if (setB.count(n)) ++common;

            if (common == 0 && !ptsB.empty()) {
                out.inconclusive = true;
                out.message = "comparison points do not overlap at all (" +
                              std::to_string(ptsB.size()) + " before, " +
                              std::to_string(ptsA.size()) + " after). "
                              "The optimizer most likely regenerated DFF instance names -- "
                              "equivalence checking needs them preserved.";
                return out;
            }
            if (verbose) {
                std::cout << "  comparison points: " << common << " common, "
                          << ptsB.size() << " before, " << ptsA.size() << " after\n";
            }
        }

        // 污染來源判別。
        //   before 髒、after 乾淨 → 原始電路本來就有問題,最佳化沒有讓它變糟
        //   after 比 before 髒     → 修改引入了新污染,這時絕不能給確定答案
        const uint32_t taintBefore = primBefore.num_tainted_nets();
        const uint32_t taintAfter  = primAfter.num_tainted_nets();
        const bool modificationIntroducedTaint = (taintAfter > taintBefore);

        if (verbose && (taintBefore || taintAfter)) {
            std::cout << "  tainted nets: " << taintBefore << " before, "
                      << taintAfter << " after"
                      << (modificationIntroducedTaint ? "  [the edit made it worse]"
                                                      : "  [pre-existing]") << "\n";
        }

        eqeng::CecOptions opt;
        opt.require_identical_outputs = true;
        opt.include_dff_next_state    = true;   // 只改到 DFF 輸入的錯誤不能漏掉
        opt.identify_mismatches       = true;
        opt.untrusted_is_failure      = true;
        // 「結構被大幅改寫但功能不變」正是 FRAIG 的適用情境:
        // 兩側功能相同的內部節點合併後,頂端的 XOR 直接塌成常數 0,SAT 幾乎不用做事。
        opt.fraig_miter               = true;
        // 修改引入新污染才嚴格。原始電路本來就髒的話,比乾淨子集合並回報涵蓋率。
        opt.untrusted_is_failure      = modificationIntroducedTaint;

        // 預算自保。
        //   time_limit_seconds 只管 CEC 內部的求解,不含建兩顆 AIG、
        //   兩次 copy_cone、建 miter —— 那些在 112k gate 上約 0.3~0.5 秒。
        //   而且 fraig_miter=true 時,miter sweep 拿 total_time_budget 一份,
        //   之後求解又重新武裝一份,最壞是兩倍。這裡先扣已耗時間再折半。
        const double left = budgetSeconds - elapsed();
        if (left <= 0.0) {
            out.inconclusive = true;
            out.message = "budget consumed while building the models; no time left to solve";
            return out;
        }
        opt.time_limit_seconds = left;

        const eqeng::CecResult r = primAfter.equiv_to_snapshot(snap, opt);
        out.ran             = true;
        out.comparedPoints  = r.compared_outputs.size();
        out.untrustedPoints = r.untrusted_outputs.size();

        if (r.status == eqeng::EquivResult::Equal && !r.interface_mismatch) {
            out.equivalent = true;
            out.partial    = !r.untrusted_outputs.empty();

            out.message = "verified equivalent over " +
                          std::to_string(r.compared_outputs.size()) + " comparison points (" +
                          std::to_string(r.resolved_by_strash) + " resolved structurally)";

            if (out.partial) {
                // 涵蓋率一定要講出來。「驗過了」和「驗過 88% 」對使用者
                //   是不同的結論,而 LLM 只會照字面轉述。
                out.message += "; " + std::to_string(r.untrusted_outputs.size()) +
                               " further point(s) could NOT be compared because the "
                               "ORIGINAL netlist is not a faithful model there (" +
                               std::to_string(taintBefore) + " tainted nets before the edit, "
                               "first unverifiable point: " + r.untrusted_outputs.front() + ")";
            }
            return out;
        }

        if (r.status == eqeng::EquivResult::NotEqual) {
            out.message = "NOT equivalent";
            if (!r.mismatched_outputs.empty()) {
                out.message += "; first mismatching point: " + r.mismatched_outputs.front();
                if (r.mismatched_outputs.size() > 1)
                    out.message += " (+" + std::to_string(r.mismatched_outputs.size() - 1) + " more)";
            }
            return out;
        }

        out.inconclusive = true;
        out.message = "inconclusive: " +
            (r.message.empty() ? std::string("solver returned unknown") : r.message);
        if (r.interface_mismatch) out.message += " [interface mismatch]";
        if (!r.untrusted_outputs.empty())
            out.message += " [" + std::to_string(r.untrusted_outputs.size()) + " untrusted points]";
        return out;

    } catch (const std::exception& e) {
        out.inconclusive = true;
        out.message = std::string("equivalence check threw: ") + e.what();
        return out;
    }
}

} // namespace

Optimizer::Optimizer(const OptimizerConfig& config)
    : config(config) {} // 使用初始化列表進行高效賦值

bool isGateAllowed(GateType type, 
                   const std::vector<GateType>& allowedTypes, 
                   const std::vector<GateType>& bannedTypes) {
    
    // 1. 如果在 bannedTypes 裡，絕對不允許 (黑名單優先)
    if (std::find(bannedTypes.begin(), bannedTypes.end(), type) != bannedTypes.end()) {
        return false;
    }

    // 2. 如果 allowedTypes 有內容，且該 type 不在裡面，則不允許 (白名單過濾)
    if (!allowedTypes.empty()) {
        if (std::find(allowedTypes.begin(), allowedTypes.end(), type) == allowedTypes.end()) {
            return false;
        }
    }

    // 通過以上檢查，代表是 Allowed
    return true;
}

// 最佳化主控流程
OptimizationResult Optimizer::executeOptimization(
        Netlist& netlist,
        TechMapper& techMapper,
        const opt::OptimizationRequest& request,
        bool verbose,
        const request_time_budget::RequestDeadline* requestDeadline) {

    using namespace opt;

    OptimizationResult result;
    // metric 決定這次是深度還是面積最佳化；passName 讓 log 與 report 分得出來。
    result.passName = isDepthMetric(request.cost.metric) ? "Opt_CP" : "Opt_Area";

    // =====================================================================
    // 階段 0：Request 驗證（在碰 netlist 之前，失敗就零副作用返回）
    // =====================================================================
    const RequestValidation validation = validateRequest(request);
    if (!validation.ok) {
        result.status  = OptimizationStatus::ERROR_INVALID_REQUEST;
        result.message = "Invalid optimization request: " + validation.message;
        return result;
    }

        // basisConstraints 最多兩組：沒有 scope 的那組 = cone 外（無 cone 時就是全域），
    // 有 scope 的那組 = 該 cone 內。兩者並存 = 異質 basis 題。
    static const BasisConstraint kUnconstrained{};
    const BasisConstraint* outsidePtr = nullptr;
    const BasisConstraint* conePtr    = nullptr;
    for (const auto& b : request.basisConstraints) {
        if (b.isWholeNetlist()) outsidePtr = &b;
        else                    conePtr    = &b;
    }

    const BasisConstraint& outsideBasis = outsidePtr ? *outsidePtr : kUnconstrained;
    const bool hasLocalBasisScope = (conePtr != nullptr);

    // legacy path 只認得單一組約束。cone 題給 cone 的那組（維持舊行為），
    // 否則給全域那組。
    const BasisConstraint& legacyBasis = hasLocalBasisScope ? *conePtr : outsideBasis;
    const std::vector<GateType>& allowedTypes = legacyBasis.allowed;
    const std::vector<GateType>& bannedTypes  = legacyBasis.banned;

    // 兩區都受限時 legacy path 無法表達：Stage 4 的全域 convertToBasis
    // 會把 cone 內剛強制好的 basis 一起改掉。這種題目只能走 lowering。
    const bool requiresLowering = hasLocalBasisScope && outsideBasis.constrains();

    const Netlist originalSnapshot = netlist.cloneForRollback();

    // =====================================================================
    // 量測優化前的 cost
    // =====================================================================
    result.costMetricName = describeCost(request.cost);

    const CostMeasurement before = measureCost(netlist, request.cost);
    if (!before.ok) {
        result.status  = OptimizationStatus::ERROR_INVALID_REQUEST;
        result.message = "Cannot evaluate the cost function: " + before.message;
        return result;
    }
    result.oldDepth       = before.primary(request.cost.metric);
    result.oldGlobalDepth = before.globalDepth;
    result.oldConeDepth   = before.coneDepth;
    result.oldGateCount   = before.gateCount;

    auto requestTimedOut = [&]() {
        return requestDeadline != nullptr && requestDeadline->expired();
    };
    auto remainingSeconds = [&]() -> double {
        return requestDeadline != nullptr ? requestDeadline->remainingSeconds()
                                          : std::numeric_limits<double>::max();
    };

    auto returnFailure = [&](OptimizationStatus status, const std::string& message) {
        netlist.restoreFrom(originalSnapshot);
        result.status         = status;
        result.changed        = false;
        result.depthImproved  = false;
        result.newDepth       = result.oldDepth;
        result.newGlobalDepth = result.oldGlobalDepth;
        result.newConeDepth   = result.oldConeDepth;
        result.newGateCount   = result.oldGateCount;
        result.areaDelta      = 0;

        // 已經驗出結果的話要保留 —— 尤其 Refuted，那是回滾的原因本身。
        // 其餘失敗路徑（timeout / constraint）在階段 7 之前就返回了，
        // 此時 equivalenceStatus 仍是初值 NotChecked，不需要特別處理。
        result.equivalenceChecked = (result.equivalenceStatus != EquivalenceStatus::NotChecked);
        result.equivalent         = (result.equivalenceStatus == EquivalenceStatus::Verified);

        result.message = message;
        return result;
    };
    auto returnTimeout = [&](const std::string& message) {
        return returnFailure(OptimizationStatus::TIMEOUT, message);
    };

    if (requestTimedOut()) {
        return returnTimeout("Depth optimization could not start because the request deadline expired.");
    }

    if (verbose) {
        std::cout << "\n=================================================\n";
        std::cout << "[Flow Start] cost metric: " << result.costMetricName << "\n";
        std::cout << "             basis scope: ";
        if (hasLocalBasisScope) {
            std::cout << "cone '" << conePtr->scope->sourceName << "'"
                      << (outsideBasis.constrains() ? " + constrained outside"
                                                    : " (outside unconstrained)");
        } else {
            std::cout << (outsideBasis.constrains() ? "whole netlist" : "unconstrained");
        }
        std::cout << "\n";
        std::cout << "             old cost: "    << result.oldDepth
                  << " | global depth: "          << result.oldGlobalDepth
                  << " | gate count: "            << result.oldGateCount << "\n";
        std::cout << "             time budget: " << remainingSeconds() << "s\n";
    }

    // =====================================================================
    // 階段 1：Lowering 計畫 + 約束分類
    // =====================================================================
    lowering::LoweringSpec loweringSpec;
    bool   loweringUsable = false;
    std::string loweringSkipReason;

    {
        auto scopeIsFaninCone = [](ConeQueryType t) {
            return t == ConeQueryType::NetTransitiveFanin  ||
                   t == ConeQueryType::GateTransitiveFanin ||
                   t == ConeQueryType::LargestOutputCone;
        };

        // cone 外（或無 cone 時的全域）basis。
        // outsideBasis 兩個清單都空時 fromLists 會允許全部閘型，
        // 等同舊版「cone 外不受限」的行為。
        loweringSpec.defaultBasis =
            lowering::LoweringBasis::fromLists(outsideBasis.allowed, outsideBasis.banned);

        if (!loweringSpec.defaultBasis.complete()) {
            loweringSkipReason = "the outside-cone basis {" +
                loweringSpec.defaultBasis.describe() + "} is not functionally complete";

        } else if (!hasLocalBasisScope) {
            loweringUsable = true;

        } else if (!scopeIsFaninCone(conePtr->scope->type)) {
            loweringSkipReason = "basis scope is a fanout cone; lowering only supports fanin cones";

        } else {
            const ConeResolution cr = resolveConeGates(netlist, *conePtr->scope);
            if (!cr.ok) {
                loweringSkipReason = "cannot resolve the basis cone root: " + cr.message;
            } else if (cr.rootNetName.empty()) {
                loweringSkipReason = "basis cone root has no resolvable net name";
            } else {
                loweringSpec.coneBasis =
                    lowering::LoweringBasis::fromLists(conePtr->allowed, conePtr->banned);
                loweringSpec.coneRootName = cr.rootNetName;

                if (loweringSpec.coneBasis->complete()) {
                    loweringUsable = true;
                } else {
                    loweringSkipReason = "the in-cone basis {" +
                        loweringSpec.coneBasis->describe() + "} is not functionally complete";
                    loweringSpec.coneBasis.reset();
                    loweringSpec.coneRootName.clear();
                }
            }
        }
    }

    // 沒有 legacy 退路的題目，與其產出違反約束的電路不如明確失敗。
    if (requiresLowering && !loweringUsable) {
        return returnFailure(
            OptimizationStatus::ERROR_CONSTRAINT_UNSATISFIED,
            "This request constrains both the inside and the outside of a cone, which "
            "requires the direct-lowering path, but lowering is unavailable: "
            + loweringSkipReason + ".");
    }

    // lowering 的 DP 用哪個字典序，與 Stage 2 的 pass 組合對齊。
    loweringSpec.objective = isDepthMetric(request.cost.metric)
                           ? lowering::LoweringObjective::MinDepth
                           : lowering::LoweringObjective::MinArea;

    if (verbose) {
        if (loweringUsable)
            std::cout << "             lowering  : enabled ("
                      << (loweringSpec.objective == lowering::LoweringObjective::MinDepth
                            ? "min-depth" : "min-area")
                      << "), outside {" << loweringSpec.defaultBasis.describe() << "}"
                      << (loweringSpec.hasCone()
                            ? (", cone '" + loweringSpec.coneRootName + "' {" +
                               loweringSpec.coneBasis->describe() + "}")
                            : "")
                      << "\n";
        else
            std::cout << "             lowering  : disabled (" << loweringSkipReason
                      << "); falling back to the legacy basis-enforcement path\n";
    }

    const bool canAbsorbInverters =
        isGateAllowed(GateType::NAND, allowedTypes, bannedTypes) ||
        isGateAllowed(GateType::NOR,  allowedTypes, bannedTypes) ||
        isGateAllowed(GateType::XNOR, allowedTypes, bannedTypes);

    // =====================================================================
    // 階段 2：全域深度壓縮（AIG / XAG 雙路徑 + warm-start）
    // =====================================================================
    constexpr double kStage2TimeLimitSeconds = 200.0;
    const double stage2BudgetSeconds = requestDeadline != nullptr
        ? requestDeadline->boundedStageSeconds(kStage2TimeLimitSeconds)
        : kStage2TimeLimitSeconds;
    const auto stage2Start = std::chrono::steady_clock::now();
    auto stage2Elapsed = [&]() {
        return std::chrono::duration<double>(
            std::chrono::steady_clock::now() - stage2Start).count();
    };

    const double finalizeReserve = std::max(3.0, result.oldGateCount * 0.00009);
    const double equivReserve    = config.verifyEquivalence
                                 ? config.equivalenceReserveSeconds : 0.0;
    const double totalReserve    = finalizeReserve + equivReserve;
    const double firstIterEstimate = 6.0 + result.oldGateCount * 0.00013;

    if (verbose) {
        std::cout << "             reserve   : finalize " << finalizeReserve
                  << "s + equiv " << equivReserve << "s\n";
    }
    if (stage2BudgetSeconds <= 0.0) {
        return returnTimeout("Depth optimization could not enter Stage 2 because "
                             "the request deadline expired.");
    }

    bool loweringActive = loweringUsable;

    Stage2Context ctx;
    ctx.templateNetlist   = &originalSnapshot;
    ctx.request           = &request;
    ctx.loweringSpec      = &loweringSpec;
    ctx.loweringActive    = &loweringActive;
    ctx.policy            = config.stage2Policy;
    ctx.deadline          = requestDeadline;
    ctx.reserveSeconds    = totalReserve;
    ctx.firstIterEstimate = firstIterEstimate;
    ctx.verbose           = verbose;
    // 深度目標跑 balancing + preserve_depth；面積目標跑 rewriting + resub +
    // refactoring。runStage2Path 的 runPasses 依這個欄位分岔。
    ctx.goal              = goalOf(request.cost.metric);

    auto stage2Left = [&]() -> double {
        const double byStage2  = stage2BudgetSeconds - stage2Elapsed();
        const double byRequest = (requestDeadline != nullptr)
            ? requestDeadline->remainingSeconds() - totalReserve
            : std::numeric_limits<double>::max();
        return std::max(0.0, std::min(byStage2, byRequest));
    };
    auto worthRunning = [&](double budget) {
        return budget > firstIterEstimate * 2.0;
    };

    // 先跑哪一條：目標 basis 做不出 XOR 時，esop_rebalancing / xag_npn 製造的
    // XOR 節點在 lowering 時要付代價（深度 3 層、面積 4 顆閘），AIG 沒有這筆帳。
    // cone basis 題的 cone 外不受限，XOR 可用，先跑 XAG。
    const bool xorAvailable =
        isGateAllowed(GateType::XOR,  outsideBasis.allowed, outsideBasis.banned) ||
        isGateAllowed(GateType::XNOR, outsideBasis.allowed, outsideBasis.banned);
    const bool aigFirst = !xorAvailable;

    if (verbose) {
        std::cout << "[Step 2] Dual-path "
                  << (ctx.goal == OptimizationGoal::DEPTH ? "depth" : "area")
                  << " optimization (first: " << (aigFirst ? "AIG" : "XAG")
                  << ", target basis " << (xorAvailable ? "has" : "lacks")
                  << " XOR/XNOR) budget=" << stage2Left() << "s\n";
    }

    // ---- 1. 兩條路徑的保底預算 ----
    Stage2PathResult first = aigFirst
        ? runStage2Path<mockturtle::aig_network>(ctx, "AIG", stage2Left() * 0.45)
        : runStage2Path<mockturtle::xag_network>(ctx, "XAG", stage2Left() * 0.45);

    Stage2PathResult second;
    if (worthRunning(stage2Left() * 0.8)) {
        second = aigFirst
            ? runStage2Path<mockturtle::xag_network>(ctx, "XAG", stage2Left() * 0.8)
            : runStage2Path<mockturtle::aig_network>(ctx, "AIG", stage2Left() * 0.8);
    } else if (verbose) {
        std::cout << "  -> skipping the second path: only " << stage2Left()
                  << "s left\n";
    }

    // ---- 2. 挑贏家 ----
    Stage2PathResult best;
    if (first.ok) best = std::move(first);
    if (second.ok &&
        (!best.ok || second.cost.betterThan(best.cost, request.cost.metric))) {
        if (verbose && best.ok)
            std::cout << "  -> " << second.name << " wins ("
                      << second.cost.globalDepth << " vs "
                      << best.cost.globalDepth << ")\n";
        best = std::move(second);
    } else if (verbose && second.ok && best.ok) {
        std::cout << "  -> " << second.name << " did not improve on " << best.name
                  << " (" << second.cost.globalDepth << " vs "
                  << best.cost.globalDepth << ")\n";
    }

    // ---- 3. 剩餘時間給贏家續跑 ----
    //
    // 保底預算切完之後通常還剩不少。
    // 全部餵給表現最好的那條軌跡，從它的 best 繼續探索。
    if (best.ok && best.resumable() && worthRunning(stage2Left())) {
        const double resumeBudget = stage2Left();
        if (verbose)
            std::cout << "  -> resuming " << best.name << " with the remaining "
                      << resumeBudget << "s\n";

        Stage2PathResult resumed;
        if (best.aigState.has_value()) {
            resumed = runStage2Path<mockturtle::aig_network>(
                ctx, (best.name + "+").c_str(), resumeBudget, &*best.aigState);
        } else if (best.xagState.has_value()) {
            resumed = runStage2Path<mockturtle::xag_network>(
                ctx, (best.name + "+").c_str(), resumeBudget, &*best.xagState);
        }

        if (resumed.ok && resumed.cost.betterThan(best.cost, request.cost.metric)) {
            if (verbose)
                std::cout << "  -> resume improved " << best.cost.globalDepth
                          << " -> " << resumed.cost.globalDepth << "\n";
            best = std::move(resumed);
        } else if (verbose && resumed.ok) {
            std::cout << "  -> resume did not improve (" << resumed.cost.globalDepth
                      << " vs " << best.cost.globalDepth << ")\n";
        }
    }

    bool haveCandidate = false;
    if (best.ok) {
        netlist = std::move(best.netlist);
        haveCandidate = true;
        if (verbose)
            std::cout << "  -> Stage 2 winner: " << best.name
                      << " global=" << best.cost.globalDepth
                      << " cone="   << best.cost.coneDepth
                      << " gates="  << best.cost.gateCount
                      << " (" << best.iterationsRun << " iterations, "
                      << best.elapsedSeconds << "s)"
                      << " | stage2 used " << stage2Elapsed() << "s of "
                      << stage2BudgetSeconds << "s\n";
    }

    netlist.trimDeadLogic();

    const bool loweringApplied = loweringActive && haveCandidate;

    // lowering 在 Stage 2 執行期失敗（loweringActive 被關掉）的話，
    // 異質 basis 題就沒有任何路徑能滿足約束了。
    if (requiresLowering && !loweringApplied) {
        return returnFailure(
            OptimizationStatus::ERROR_CONSTRAINT_UNSATISFIED,
            "Lowering failed during optimization and no fallback path can satisfy "
            "both the in-cone and outside-cone gate constraints.");
    }

    // -----------------------------------------------------------------
    // 超時處理:lowering 產出的候選 by construction 就滿足目標 basis,
    // 所以任何時間點交出它都合法 —— 不需要回滾。
    // 沒有 lowering 的話電路還沒合規,只能回滾。
    // -----------------------------------------------------------------
    bool budgetExhausted = requestTimedOut();
    if (budgetExhausted && !loweringApplied) {
        return returnTimeout("Depth optimization exhausted the request time budget during Stage 2 "
                             "before the netlist could be brought into the target basis.");
    }
    if (budgetExhausted && verbose) {
        std::cout << "  [note] request budget exhausted; returning the best compliant "
                     "candidate found so far.\n";
    }

    if (loweringApplied && verbose) {
        std::cout << "[Step 3-5] skipped: lowering already enforced the basis "
                     "and absorbed inverters.\n";
    }

    // =====================================================================
    // 階段 3：局部基底約束（legacy path）
    // =====================================================================
    if (!loweringApplied && hasLocalBasisScope) {
        const ConeRef& basisCone = *conePtr->scope;

        if (requestTimedOut()) {
            return returnTimeout("Depth optimization exhausted the request time budget before local cone enforcement.");
        }
        if (verbose)
            std::cout << "[Step 3] Local basis enforcement on cone '"
                      << basisCone.sourceName << "'...\n";

        auto refreshCone = [&]() { return resolveConeGates(netlist, basisCone); };

        ConeResolution cone = refreshCone();
        if (!cone.ok) {
            return returnFailure(OptimizationStatus::ERROR_INVALID_REQUEST,
                                 "Cannot resolve the basis scope cone: " + cone.message);
        }
        if (cone.gateIds.empty()) {
            return returnFailure(OptimizationStatus::ERROR_INVALID_REQUEST,
                                 "Basis scope cone contains no combinational gates; "
                                 "cannot enforce a local basis.");
        }
        if (verbose) std::cout << "  -> cone size: " << cone.gateIds.size() << " gates\n";

        if (verbose) std::cout << "[Step a] Cone-internal basis enforcement\n";
        TechMapReport coneRep = techMapper.convertToBasisOnGateSet(
            netlist, cone.gateIds, allowedTypes, bannedTypes, verbose);

        if (coneRep.status != TechMapStatus::SUCCESS) {
            if (coneRep.status == TechMapStatus::TIMEOUT) {
                return returnTimeout("Cone basis enforcement timed out: " + coneRep.message);
            }
            return returnFailure(OptimizationStatus::ERROR_CONSTRAINT_UNSATISFIED,
                                 "Cone basis enforcement failed: " + coneRep.message);
        }
        eliminateDoubleInverters(netlist);
        netlist.trimDeadLogic();

        if (verbose) std::cout << "[Step b] Cone-internal inverter absorption\n";
        cone = refreshCone();
        if (!cone.ok) {
            return returnFailure(OptimizationStatus::ERROR_INVALID_REQUEST,
                                 "Basis scope cone became unresolvable after enforcement: " + cone.message);
        }
        techMapper.absorbInvertersOnGateSet(netlist, cone.gateIds,
                                            allowedTypes, bannedTypes, verbose);
        if (requestTimedOut()) {
            return returnTimeout("Depth optimization exhausted the request time budget during cone inverter absorption.");
        }
        eliminateDoubleInverters(netlist);
        netlist.trimDeadLogic();

        if (verbose) std::cout << "[Step c] Outside-cone inverter absorption\n";
        cone = refreshCone();
        if (!cone.ok) {
            return returnFailure(OptimizationStatus::ERROR_INVALID_REQUEST,
                                 "Basis scope cone became unresolvable after absorption: " + cone.message);
        }

        std::unordered_set<int> outsideGates;
        for (int g = 0; g < (int)netlist.getGateCount(); ++g) {
            if (netlist.isGateRemoved(g)) continue;
            const GateType t = netlist.getGate(g).type;
            if (t == GateType::UNKNOWN || t == GateType::DFF) continue;
            if (cone.gateIds.count(g)) continue;
            outsideGates.insert(g);
        }

        static const std::vector<GateType> kAllGates = {
            GateType::AND, GateType::OR, GateType::NAND, GateType::NOR,
            GateType::NOT, GateType::XOR, GateType::XNOR };

        techMapper.absorbInvertersOnGateSet(netlist, outsideGates, kAllGates, {}, verbose);
        if (requestTimedOut()) {
            return returnTimeout("Depth optimization exhausted the request time budget during outside-cone inverter absorption.");
        }
        eliminateDoubleInverters(netlist);
        netlist.trimDeadLogic();
    }

    // =====================================================================
    // 階段 4：全域基底強制（legacy path）
    // =====================================================================
    bool needBasisEnforce = false;
    if (!loweringApplied && !hasLocalBasisScope && outsideBasis.constrains()) {
        static const std::vector<GateType> kAllComb = {
            GateType::AND, GateType::OR, GateType::NAND, GateType::NOR,
            GateType::NOT, GateType::BUF, GateType::XOR, GateType::XNOR };
        for (GateType t : kAllComb) {
            if (!isGateAllowed(t, allowedTypes, bannedTypes) &&
                netlist.getGateCountByType(t) > 0) { needBasisEnforce = true; break; }
        }
    }

    if (needBasisEnforce) {
        if (requestTimedOut()) {
            return returnTimeout("Depth optimization exhausted the request time budget before basis enforcement.");
        }
        if (verbose) std::cout << "[Step 4] Global basis enforcement...\n";

        TechMapReport rep = techMapper.convertToBasis(
            netlist, TargetScope::WHOLE_NETLIST, "", allowedTypes, bannedTypes, verbose);

        if (rep.status != TechMapStatus::SUCCESS) {
            if (rep.status == TechMapStatus::TIMEOUT) {
                return returnTimeout("Basis enforcement timed out: " + rep.message);
            }
            return returnFailure(OptimizationStatus::ERROR_CONSTRAINT_UNSATISFIED,
                                 "Basis enforcement failed: " + rep.message);
        }

        eliminateDoubleInverters(netlist);
        netlist.trimDeadLogic();
        if (verbose)
            std::cout << "  -> after basis enforcement, global depth: "
                      << netlist.findGlobalCriticalPath().depth << "\n";
    }

    // =====================================================================
    // 階段 5：全域反相吸收（legacy path）
    // =====================================================================
    if (!loweringApplied && canAbsorbInverters && !hasLocalBasisScope) {
        if (requestTimedOut()) {
            return returnTimeout("Depth optimization exhausted the request time budget before inverter absorption.");
        }
        if (verbose) std::cout << "[Step 5] Inverter absorption...\n";
        techMapper.absorbInverters(netlist, allowedTypes, bannedTypes, verbose);
        eliminateDoubleInverters(netlist);
        netlist.trimDeadLogic();
        if (verbose)
            std::cout << "  -> after absorption, global depth: "
                      << netlist.findGlobalCriticalPath().depth << "\n";
    }

    // =====================================================================
    // 階段 6：結算
    // =====================================================================
    netlist.trimDeadLogic();

    const CostMeasurement after = measureCost(netlist, request.cost);
    if (!after.ok) {
        return returnFailure(OptimizationStatus::ERROR_INVALID_REQUEST,
                             "Cost function became unmeasurable after optimization: " + after.message);
    }
    result.newDepth       = after.primary(request.cost.metric);
    result.newGlobalDepth = after.globalDepth;
    result.newConeDepth   = after.coneDepth;
    result.newGateCount   = after.gateCount;
    result.depthImproved  = (result.newDepth < result.oldDepth);
    result.areaDelta      = result.newGateCount - result.oldGateCount;
    result.changed        = !sameNetlistGraph(originalSnapshot, netlist);

    // =====================================================================
    // 階段 7：等價驗證
    //   GenericLowering 繞過了所有已驗證的 rewriting 路徑,極性算錯會靜默
    //   產生「深度漂亮但功能錯誤」的電路。這一步是唯一的防線。
    // =====================================================================
    result.equivalenceStatus  = EquivalenceStatus::NotChecked;
    result.equivalenceChecked = false;
    result.equivalent         = false;
    std::string equivNote;

    if (config.verifyEquivalence && result.changed) {
        const double left = std::max(0.0, remainingSeconds() - 3.0);
        const double budget = (requestDeadline != nullptr)
            ? std::min(left, std::max(config.equivalenceReserveSeconds, left * 0.4))
            : config.equivalenceReserveSeconds;

        if (verbose) std::cout << "[Step 7] Equivalence check (budget " << budget << "s)...\n";
        const EquivalenceOutcome eq =
            verifyAgainstSnapshot(originalSnapshot, netlist, budget, verbose);

        equivNote = eq.message;
        result.equivalencePartial         = eq.partial;
        result.equivalenceComparedPoints  = eq.comparedPoints;
        result.equivalenceUntrustedPoints = eq.untrustedPoints;

        // EquivalenceOutcome 的四種狀態映射：
        //   equivalent            -> Verified
        //   ran && !equiv && !inc -> Refuted（唯一會回滾的）
        //   inconclusive          -> Inconclusive
        //   skipped               -> NotChecked（沒預算，根本沒開始）
        if (eq.equivalent) {
            result.equivalenceStatus = EquivalenceStatus::Verified;
        } else if (eq.ran && !eq.inconclusive) {
            result.equivalenceStatus = EquivalenceStatus::Refuted;
        } else if (eq.inconclusive) {
            result.equivalenceStatus = EquivalenceStatus::Inconclusive;
        } else {
            result.equivalenceStatus = EquivalenceStatus::NotChecked;   // skipped
        }

        result.equivalenceChecked = (result.equivalenceStatus != EquivalenceStatus::NotChecked);
        result.equivalent         = (result.equivalenceStatus == EquivalenceStatus::Verified);

        if (verbose) std::cout << "  -> " << eq.message << "\n";

        if (result.equivalenceStatus == EquivalenceStatus::Refuted) {
            return returnFailure(OptimizationStatus::ERROR_NOT_EQUIVALENT,
                "Depth optimization produced a non-equivalent netlist and was rolled back: "
                + eq.message);
        }

        if ((eq.inconclusive || eq.skipped) && config.rollbackOnInconclusiveEquivalence) {
            return returnFailure(OptimizationStatus::ERROR_NOT_EQUIVALENT,
                "Equivalence could not be established and the result was rolled back: "
                + eq.message);
        }

        if (eq.partial && verbose) {
            std::cout << "  [warn] equivalence verified on the clean subset only ("
                      << eq.comparedPoints << " of "
                      << (eq.comparedPoints + eq.untrustedPoints) << " points)\n";
        }
    }

    // =====================================================================
    // 收尾
    // =====================================================================
    result.status = result.changed ? OptimizationStatus::SUCCESS
                                   : OptimizationStatus::NO_IMPROVEMENT;

    if (result.changed) {
        result.message = "Generated an optimization candidate measured by "
                       + result.costMetricName + " using the "
                       + (loweringApplied ? "direct-lowering" : "legacy basis-enforcement")
                       + " path";
        if (budgetExhausted)
            result.message += "; the request time budget ran out, so this is the best "
                              "compliant candidate found before the deadline";

        // 依三態產生敘述。最重要的是 NotChecked 不能被讀成「不等價」——
        // 那是呼叫端最容易犯的錯，而兩者對使用者的意義完全相反。
        switch (result.equivalenceStatus) {
            case EquivalenceStatus::Verified:
                result.message += "; equivalence verified";
                if (result.equivalencePartial)
                    result.message += " on the trustworthy subset only ("
                                    + std::to_string(result.equivalenceComparedPoints) + " of "
                                    + std::to_string(result.equivalenceComparedPoints +
                                                     result.equivalenceUntrustedPoints)
                                    + " comparison points)";
                break;
            case EquivalenceStatus::Inconclusive:
                result.message += "; equivalence checking found no counterexample but "
                                  "could not complete within the time budget";
                break;
            case EquivalenceStatus::NotChecked:
            default:
                result.message += "; this pass did not run an equivalence check";
                break;
        }
        result.message += ".";
    } else {
        result.message = "Optimization completed without a measurable depth, area, "
                         "or gate-type change.";
    }

    if (verbose) {
        std::cout << "[Done] " << result.costMetricName << ": "
                  << result.oldDepth << " -> " << result.newDepth
                  << " | global depth: " << result.oldGlobalDepth
                  << " -> " << result.newGlobalDepth
                  << " | gates: " << result.oldGateCount
                  << " -> " << result.newGateCount
                  << " | equivalent: " << (result.equivalent ? "yes"
                                          : (result.equivalenceChecked ? "UNPROVEN" : "unchecked"))
                  << "\n";
        std::cout << "=================================================\n";
    }
    return result;
}

template <typename Ntk>
Optimizer::Stage2PathResult Optimizer::runStage2Path(
        const Stage2Context& ctx,
        const char* pathName,
        double pathBudgetSeconds,
        const Ntk* warmStart) {

    using namespace opt;
    constexpr bool kIsXag = std::is_same_v<Ntk, mockturtle::xag_network>;

    Stage2PathResult out;
    out.name = pathName;

    const auto pathStart = std::chrono::steady_clock::now();
    auto pathElapsed = [&]() {
        return std::chrono::duration<double>(
            std::chrono::steady_clock::now() - pathStart).count();
    };
    auto remainingSeconds = [&]() -> double {
        return ctx.deadline != nullptr ? ctx.deadline->remainingSeconds()
                                       : std::numeric_limits<double>::max();
    };

    // ---- 型別分派 ----
    auto lowerNtk = [&](const Ntk& n, const Netlist& t) {
        if constexpr (kIsXag) return lowering::LowerXag(n, t, *ctx.loweringSpec);
        else                  return lowering::LowerAig(n, t, *ctx.loweringSpec);
    };
    auto legacyToNetlist = [&](const Ntk& n, const Netlist& t) -> Netlist {
        if constexpr (kIsXag) return XagToNetlist(n, t);
        else                  return AigToNetlist(n, t);
    };

    // 兩條路徑共用同一個 lowering spec，所以候選的分數直接可比。
    auto toNetlist = [&](const Ntk& n, const Netlist& t) -> Netlist {
        if (*ctx.loweringActive) {
            lowering::LoweringResult r = lowerNtk(n, t);
            if (r.ok) {
                if (ctx.verbose && !r.unpreservedDffNetNames.empty()) {
                    std::cout << "  [lowering] " << r.unpreservedDffNetNames.size()
                            << " DFF D-pin net name(s) could not be preserved, first: "
                            << r.unpreservedDffNetNames.front() << "\n";
                }
                return std::move(r.netlist);
            }
            *ctx.loweringActive = false;
            if (ctx.verbose)
                std::cout << "  [warn] lowering failed at runtime: " << r.message << "\n";
        }
        return legacyToNetlist(n, t);
    };

    auto buildNtk = [&]() -> Ntk {
        if constexpr (kIsXag)
            return mockturtle::cleanup_dangling(NetlistToXag(*ctx.templateNetlist));
        else
            return mockturtle::cleanup_dangling(NetlistToAig(*ctx.templateNetlist));
    };

    // 一輪的最佳化 pass。深度與面積的差別集中在這裡。
    //
    // 深度組合：balancing 壓深度 → cut_rewriting 在不變深的前提下省面積
    //           → resubstitution 同樣不得變深
    // 面積組合：不跑 balancing（它是用面積換深度）→ cut_rewriting 只接受
    //           真的變小的改寫 → resubstitution 是主力 → refactoring 收尾
    auto runPasses = [&](Ntk& cand) {
        const bool depthGoal = (ctx.goal == OptimizationGoal::DEPTH);

        // 1) balancing —— 唯一真正在壓深度的 pass。
        //    面積目標下跑它只會把電路吹大，直接跳過。
        if (depthGoal) {
            mockturtle::balancing_params bps;
            bps.cut_enumeration_ps.cut_size = 6u;
            if constexpr (kIsXag) {
                mockturtle::esop_rebalancing<Ntk> reb;
                cand = mockturtle::cleanup_dangling(mockturtle::balancing(cand, {reb}, bps));
            } else {
                mockturtle::sop_rebalancing<Ntk> reb;
                cand = mockturtle::cleanup_dangling(mockturtle::balancing(cand, {reb}, bps));
            }
        }

        // 2) cut rewriting —— 兩個目標都跑，參數相反。
        //    allow_zero_gain 在深度目標下開著，是為了讓結構「攤開」給下一輪
        //    balancing 用；面積目標沒有這個下游，打平的改寫只是浪費時間。
        {
            mockturtle::cut_rewriting_params cr;
            cr.cut_enumeration_ps.cut_size = 4;
            cr.preserve_depth  = depthGoal;
            cr.allow_zero_gain = depthGoal;
            mockturtle::xag_npn_resynthesis<Ntk> resyn;
            cand = mockturtle::cleanup_dangling(mockturtle::cut_rewriting(cand, resyn, cr));
        }

        // 3) resubstitution —— 面積目標的主力。
        //    深度目標下必須 preserve_depth，否則它會把 balancing 壓下來的
        //    深度換回去（test39 的 iter 3-5 深度回升就是這樣來的）。
        {
            mockturtle::resubstitution_params rp;
            rp.preserve_depth = depthGoal;
            mockturtle::fanout_view fv{cand};
            mockturtle::depth_view  dv{fv};
            if constexpr (kIsXag) {
                mockturtle::xag_resubstitution(dv, rp);
            } else {
                // 深度版原本只對 XAG 跑 resub；面積目標下 AIG 也需要，
                // 因為 resub 是這條路徑最主要的縮小手段。
                mockturtle::aig_resubstitution(dv, rp);
            }
            cand = mockturtle::cleanup_dangling(cand);
        }

        // 4) refactoring —— 只有面積目標。
        //    它會把 MFFC 整塊重新合成，是純面積導向，深度目標下會變深。
        //    注意：resynthesis 必須用 sop_factoring 而非 xag_npn_resynthesis ——
        //    後者是 4-input NPN 資料庫，max_pis > 4 時會在 kitty 內部斷言失敗。
        if (!depthGoal) {
            mockturtle::refactoring_params rf;
            rf.max_pis = 6;
            rf.allow_zero_gain = false;
            mockturtle::sop_factoring<Ntk> resyn;
            mockturtle::refactoring(cand, resyn, rf);
            cand = mockturtle::cleanup_dangling(cand);
        }
    };

    // ---- 起點 ----
    BestCandidateTracker<Ntk> tracker(ctx.request->cost.metric, ctx.policy);

    const bool isWarmStart = (warmStart != nullptr);
    Ntk cand = isWarmStart ? *warmStart : buildNtk();

    {
        const CostMeasurement m0 =
            evaluateCandidate(cand, *ctx.templateNetlist, toNetlist, ctx.request->cost);
        tracker.consider(cand, m0);
        if (ctx.verbose) {
            std::cout << "  [" << pathName << "] "
                      << (isWarmStart ? "resuming from" : "round-trip only")
                      << ": global=" << m0.globalDepth
                      << " cone="    << m0.coneDepth
                      << " gates="   << m0.gateCount << "\n";
        }
    }

    // ---- 迭代 ----
    double lastIterSeconds = 0.0;
    bool   hitHardPatience = false;
    out.stopReason = Stage2StopReason::IterationCap;

    for (int iter = 0; iter < ctx.policy.maxIterations; ++iter) {
        const double predicted =
            (lastIterSeconds > 0.0) ? lastIterSeconds * 1.3 : ctx.firstIterEstimate;
        const double byPath    = pathBudgetSeconds - pathElapsed();
        const double byRequest = remainingSeconds() - ctx.reserveSeconds;
        const double available = std::min(byPath, byRequest);

        if (predicted > available) {
            out.stopReason = Stage2StopReason::BudgetOut;
            if (ctx.verbose)
                std::cout << "  [" << pathName << "] stopping at iteration " << iter
                          << ": needs ~" << predicted << "s, only " << available
                          << "s available\n";
            break;
        }

        const auto iterStart = std::chrono::steady_clock::now();
        runPasses(cand);
        const CostMeasurement m =
            evaluateCandidate(cand, *ctx.templateNetlist, toNetlist, ctx.request->cost);
        const bool improved = tracker.consider(cand, m);
        lastIterSeconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - iterStart).count();

        if (improved) out.lastImprovedIteration = out.iterationsRun;
        ++out.iterationsRun;

        if (ctx.verbose) {
            std::cout << "  [" << pathName << "] iter " << iter
                      << ": global=" << m.globalDepth
                      << " cone="    << m.coneDepth
                      << " gates="   << m.gateCount
                      << " (" << lastIterSeconds << "s)"
                      << (improved ? "  [best]" : "") << "\n";
        }

        // ---- 停止判斷 ----
        // 注意：這裡刻意「不」把 cand 重置回 tracker.best()。
        // mockturtle 的 pass 是決定性的，從 best 重跑會產生完全相同的
        // 結果，變成無窮迴圈。從漂移後的 cand 繼續才有新狀態可探索。
        if (tracker.shouldStop()) {
            if (tracker.stagnantRounds() >= ctx.policy.hardPatience) {
                hitHardPatience = true;
                out.stopReason = Stage2StopReason::Converged;
                if (ctx.verbose)
                    std::cout << "  [" << pathName << "] no improvement for "
                              << tracker.stagnantRounds()
                              << " rounds (hard cap), stopping\n";
                break;
            }

            const double nextPredicted = lastIterSeconds * 1.3;
            const double slackNeeded   = nextPredicted * ctx.policy.explorationSlack;
            const double stillHave     = std::min(pathBudgetSeconds - pathElapsed(),
                                                  remainingSeconds() - ctx.reserveSeconds);
            if (stillHave < slackNeeded) {
                out.stopReason = Stage2StopReason::Converged;
                if (ctx.verbose)
                    std::cout << "  [" << pathName << "] no improvement for "
                              << tracker.stagnantRounds() << " rounds and only "
                              << stillHave << "s left, stopping\n";
                break;
            }

            if (ctx.verbose)
                std::cout << "  [" << pathName << "] no improvement for "
                          << tracker.stagnantRounds() << " rounds but "
                          << stillHave << "s left, continuing to explore\n";
        }
    }

    // ---- 收尾 ----
    if (tracker.hasBest()) {
        // 打到 hardPatience 代表這條軌跡已經死了，續跑沒有意義
        if (!hitHardPatience) {
            if constexpr (kIsXag) out.xagState = tracker.best();
            else                  out.aigState = tracker.best();
        }

        out.netlist = toNetlist(tracker.best(), *ctx.templateNetlist);
        eliminateDoubleInverters(out.netlist);
        out.netlist.trimDeadLogic();
        out.cost = measureCost(out.netlist, ctx.request->cost);
        out.ok   = out.cost.ok;
    }
    out.elapsedSeconds = pathElapsed();
    return out;
}

template Optimizer::Stage2PathResult
Optimizer::runStage2Path<mockturtle::aig_network>(
    const Stage2Context&, const char*, double, const mockturtle::aig_network*);
template Optimizer::Stage2PathResult
Optimizer::runStage2Path<mockturtle::xag_network>(
    const Stage2Context&, const char*, double, const mockturtle::xag_network*);

// 輔助函式：給定 Root 與 Cut 邊界，從 Netlist 走訪並建立 PatternNode (AST)，同時收集 TargetCone
std::shared_ptr<PatternNode> Optimizer::extractLhsFromCut(Netlist& netlist, 
                                                               int rootGateId, 
                                                               const KCut& cut, 
                                                               std::unordered_set<int>& outTargetCone,
                                                               int& outActualN) { // 用來回傳真實的 N
    outTargetCone.clear();
    
    // 將 Cut 的 Leaf Nets 轉為查表，方便 O(1) 判斷
    std::unordered_map<int, int> leafNetToIndex;
    for (int i = 0; i < cut.size; ++i) {
        leafNetToIndex[cut.leafNetIds[i]] = i; // 記錄這個 Net 對應到第幾個 PI
    }

    // 遞迴建構 Pattern 的 Lambda
    std::unordered_map<int, std::shared_ptr<PatternNode>> visitedGates;

    std::function<std::shared_ptr<PatternNode>(int)> buildAst = [&](int gateId) -> std::shared_ptr<PatternNode> {
        if (visitedGates.count(gateId)) return visitedGates[gateId];

        const Gate& g = netlist.getGate(gateId);
        outTargetCone.insert(gateId); // 將走訪到的 Gate 加入 TargetCone

        std::vector<std::shared_ptr<PatternNode>> childNodes;

        for (int inNetId : g.inputNetIds) {
            // 情況 A：如果這條輸入線是 Cut 的邊界 (Leaf)，停止往下，建立 PI 節點
            if (leafNetToIndex.count(inNetId)) {
                childNodes.push_back(PatternNode::makePI(leafNetToIndex[inNetId]));
            } 
            // 情況 B：常數線
            else if (netlist.getNet(inNetId).isConst) {
                int val = netlist.getNet(inNetId).constVal;
                childNodes.push_back(val == 1 ? PatternNode::Const1() : PatternNode::Const0());
            } 
            // 情況 C：內部線，繼續往下遞迴尋找 Driver Gate
            else {
                int driverId = netlist.getNetDriverGateId(inNetId);
                bool treatAsPI = false;

                if (driverId != -1 && netlist.isValidGateId(driverId)) {
                    // 檢查上一級的 GateType 
                    GateType driverType = netlist.getGate(driverId).type;
                    
                    if (driverType == GateType::DFF) {
                        treatAsPI = true; // 遇到 DFF，強制踩煞車，視為組合邏輯的邊界
                    } else {
                        // 是一般的組合邏輯閘，安全繼續往下長
                        childNodes.push_back(buildAst(driverId));
                    }
                } else {
                    treatAsPI = true; // 找不到 Driver (如系統 PI 或死線)，強制視為邊界
                }

                // 如果判定這條線是新的邊界 (PI)
                if (treatAsPI) {
                    // 安全檢查：如果這個 inNetId 剛剛還沒被註冊過，才給它新的 index
                    if (leafNetToIndex.find(inNetId) == leafNetToIndex.end()) {
                        leafNetToIndex[inNetId] = leafNetToIndex.size();
                    }
                    childNodes.push_back(PatternNode::makePI(leafNetToIndex[inNetId])); 
                }
            }
        }

        // 建立當前的 Gate 節點
        auto node = std::make_shared<PatternNode>(PatternNode{NodeType::GATE, g.type, -1, childNodes});
        visitedGates[gateId] = node;
        return node;
    };

    // 1. 先執行 AST 樹的建立。在這過程中，leafNetToIndex 可能會被動態加入新的變數。
    auto astRoot = buildAst(rootGateId);

    // 2. 將最終實際被使用的 PI 總數寫入 outActualN，回傳給外層！
    outActualN = leafNetToIndex.size();

    return astRoot;
}

// 共用的 Cut 剪枝過濾器
std::vector<KCut> pruneAndLimitCuts(std::vector<KCut>& cuts) {
    if (cuts.empty()) return cuts;

    // 1. 排序：優先考慮 Size 小的，若 Size 相同則依據 Signature 與內容排序
    std::sort(cuts.begin(), cuts.end(), [](const KCut& a, const KCut& b) {
        if (a.size != b.size) return a.size < b.size;
        if (a.signature != b.signature) return a.signature < b.signature;
        
        // 確保完全相同的 Cut 會排在一起
        for (int i = 0; i < a.size; ++i) {
            if (a.leafNetIds[i] != b.leafNetIds[i]) {
                return a.leafNetIds[i] < b.leafNetIds[i];
            }
        }
        return false;
    });

    // 2. 去重 (O(N))：將連續完全相同的元素剔除
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());

    // 3. 子集剪枝 (Subsumption Pruning)
    std::vector<KCut> filtered;
    filtered.reserve(std::min((size_t)MAX_CUTS_PER_NODE, cuts.size()));

    for (const auto& cut : cuts) {
        bool isSubsumed = false;
        for (const auto& keptCut : filtered) {
            // 利用 Signature 進行 O(1) 超集檢查
            if ((keptCut.signature & cut.signature) == keptCut.signature) {
                
                // 確認是真的子集，而非 Hash 碰撞
                bool allMatch = true;
                for (int i = 0; i < keptCut.size; ++i) {
                    bool found = false;
                    for (int j = 0; j < cut.size; ++j) {
                        if (keptCut.leafNetIds[i] == cut.leafNetIds[j]) { 
                            found = true; break; 
                        }
                    }
                    if (!found) { allMatch = false; break; }
                }
                
                if (allMatch) {
                    isSubsumed = true;
                    break; // 被包含，直接丟棄
                }
            }
        }
        
        if (!isSubsumed) {
            filtered.push_back(cut);
            // 剪枝的同時做數量限制，確保留下來的一定是互相獨立的精華
            if (filtered.size() >= MAX_CUTS_PER_NODE) {
                break; 
            }
        }
    }
    
    return filtered;
}

// 輔助函式：聯集兩個 Cut 列表 (DP Merge)
std::vector<KCut> mergeCuts(const std::vector<KCut>& leftCuts, const std::vector<KCut>& rightCuts) {
    if (leftCuts.empty()) return rightCuts;
    if (rightCuts.empty()) return leftCuts;

    std::vector<KCut> result;
    // 預先配置大約的空間，減少 push_back 時的重新分配
    result.reserve(leftCuts.size() * rightCuts.size());

    for (const auto& cl : leftCuts) {
        for (const auto& cr : rightCuts) {
            
            uint64_t mergedSig = cl.signature | cr.signature;
            if (__builtin_popcountll(mergedSig) > MAX_K) {
                continue; 
            }

            KCut merged;
            merged.signature = mergedSig; 

            int i = 0, j = 0;
            bool exceed = false;

            while (i < cl.size || j < cr.size) {
                if (merged.size == MAX_K) {
                    exceed = true;
                    break; 
                }

                if (i < cl.size && j < cr.size) {
                    if (cl.leafNetIds[i] < cr.leafNetIds[j]) {
                        merged.leafNetIds[merged.size++] = cl.leafNetIds[i++];
                    } else if (cl.leafNetIds[i] > cr.leafNetIds[j]) {
                        merged.leafNetIds[merged.size++] = cr.leafNetIds[j++];
                    } else {
                        merged.leafNetIds[merged.size++] = cl.leafNetIds[i];
                        i++; j++;
                    }
                } else if (i < cl.size) {
                    merged.leafNetIds[merged.size++] = cl.leafNetIds[i++];
                } else {
                    merged.leafNetIds[merged.size++] = cr.leafNetIds[j++];
                }
            }

            if (!exceed) {
                result.push_back(merged);
            }
        }
    }

    // 不直接 resize，而是透過嚴格剪枝後再限制數量
    return pruneAndLimitCuts(result);
}

// 在乾淨的 Cone 裡面找出最佳的 K-feasible Cut
KCut Optimizer::extractBestKFeasibleCut(Netlist& netlist, const OptimizationCandidate& candidate, OptimizationGoal goal) {
    // 1. 取得剛剛被我們清理過、現在非常乾淨的 coneGateSet
    std::vector<int> coneGateList = netlist.getConeGateIds(candidate.faninCone);
    std::unordered_set<int> coneGateSet(coneGateList.begin(), coneGateList.end());

    // 如果 Endpoint 本身已經被優化拔掉了，回傳空
    int rootGateId = netlist.getNetDriverGateId(candidate.endpoint.endpointNetId);
    if (!coneGateSet.count(rootGateId)) return KCut(); // 初始化空結構回傳

    // 2. 拓樸排序 (Topological Sort)
    std::unordered_map<int, int> inDegree;
    for (int gid : coneGateSet) inDegree[gid] = 0;

    for (int gid : coneGateSet) {
        const Gate& g = netlist.getGate(gid);
        for (int inNetId : g.inputNetIds) {
            int driverId = netlist.getNetDriverGateId(inNetId);
            // 如果輸入來源也是 Cone 內部的節點，則 inDegree + 1
            if (coneGateSet.count(driverId)) {
                inDegree[gid]++;
            }
        }
    }

    std::queue<int> q;
    std::vector<int> topoOrder;
    for (int gid : coneGateSet) {
        if (inDegree[gid] == 0) q.push(gid);
    }

    while (!q.empty()) {
        int curr = q.front(); q.pop();
        topoOrder.push_back(curr);

        const Gate& g = netlist.getGate(curr);
        if (g.outputNetId != -1) {
            for (int lgid : netlist.getNet(g.outputNetId).loadGateIds) {
                if (coneGateSet.count(lgid)) {
                    if (--inDegree[lgid] == 0) q.push(lgid);
                }
            }
        }
    }

    // 3. DP 狀態計算 (Cut Enumeration)
    std::unordered_map<int, std::vector<KCut>> nodeCuts;

    for (int currGateId : topoOrder) {
        const Gate& g = netlist.getGate(currGateId);
        std::vector<KCut> currentGateCuts;

        // Trivial Cut: 把自己當作邊界 (Cut 包含自己輸出的那一條線)
        if (g.outputNetId != -1) {
            KCut trivialCut;
            trivialCut.leafNetIds[0] = g.outputNetId;
            trivialCut.size = 1;
            trivialCut.normalize(); // 計算 Signature
            currentGateCuts.push_back(trivialCut);
        }

        // DP Merge: 根據每一個輸入腳位，把前一級的 Cuts 聯集起來
        std::vector<KCut> mergedCuts;
        bool isFirstInput = true;

        for (int inNetId : g.inputNetIds) {
            
            // 如果這條線是常數 0/1，直接跳過
            // 常數不佔用 MAX_K 額度，也不應該成為 Cut 的 Leaf 邊界
            const Net& inNet = netlist.getNet(inNetId);
            if (inNet.isConst) {
                continue; 
            }

            int driverId = inNet.driverGateId;
            std::vector<KCut> branchCuts;

            if (coneGateSet.count(driverId)) {
                branchCuts = nodeCuts[driverId];
            } else {
                KCut boundaryCut;
                boundaryCut.leafNetIds[0] = inNetId;
                boundaryCut.size = 1;
                boundaryCut.normalize();
                branchCuts.push_back(boundaryCut);
            }

            if (isFirstInput) {
                mergedCuts = std::move(branchCuts);
                isFirstInput = false;
            } else {
                mergedCuts = mergeCuts(mergedCuts, branchCuts);
            }
        }

        for (const auto& mc : mergedCuts) {
            currentGateCuts.push_back(mc);
        }

        nodeCuts[currGateId] = pruneAndLimitCuts(currentGateCuts);
    }

    // 4. 評估與挑選最佳 Cut (根據 Goal 分流)
    const auto& rootCuts = nodeCuts[rootGateId];
    KCut bestCut;
    CutScore bestScore = {-1, -1, -999}; // 初始化為極小值
    bool foundValidCut = false;

    // 取得 Root Gate 的輸出線 ID
    int rootOutNetId = netlist.getGate(rootGateId).outputNetId;

    std::unordered_set<int> criticalGateSet;
    if (goal == OptimizationGoal::DEPTH) {
        criticalGateSet.insert(candidate.criticalPath.gateIds.begin(), candidate.criticalPath.gateIds.end());
    }

    for (const KCut& cut : rootCuts) {

        // 絕不能選 Root 自己的 Trivial Cut！
        // 否則 extractLhsFromCut 會直接貫穿整個電路！
        if (cut.size == 1 && cut.leafNetIds[0] == rootOutNetId) {
            continue; 
        }

        CutScore currentScore;
        if (goal == OptimizationGoal::AREA) {
            currentScore = evaluateAreaCut(netlist, cut, rootGateId);
        } else {
            // DEPTH 模式
            currentScore = evaluateDepthCut(netlist, cut, rootGateId, criticalGateSet);
        }

        // 利用重載的 operator< 直接比較
        if (bestScore < currentScore) {
            bestScore = currentScore;
            bestCut = cut;
            foundValidCut = true;
        }
    }

    // 防呆：如果真的找不到任何合法的 Cut (例如全部都超過 K 限制)
    if (!foundValidCut) {
        return KCut(); // 回傳空，讓外層跳過這個 Endpoint
    }

    // 將最佳結果回傳 (這就是我們要送給 SAT 去算 Truth Table 的邊界)
    return bestCut;
}

// 輔助函式：AREA 模式下的 Cut 評分機制
CutScore Optimizer::evaluateAreaCut(Netlist& netlist, const KCut& cut, int rootGateId) {
    CutScore score;

    // 1. 建立邊界查表 (使用 Lambda 搭配小陣列走訪，速度極快)
    auto isLeafNet = [&](int netId) {
        for (int i = 0; i < cut.size; ++i) {
            if (cut.leafNetIds[i] == netId) return true;
        }
        return false;
    };

    // 2. 模擬解除參考 (Simulated Dereferencing)
    std::unordered_map<int, int> simulatedFanout;
    std::queue<int> derefQueue;
    int mffcVolume = 0;

    // Root Gate 一定會被換掉
    derefQueue.push(rootGateId);
    mffcVolume++; 

    while (!derefQueue.empty()) {
        int currGateId = derefQueue.front();
        derefQueue.pop();

        if (!netlist.isValidGateId(currGateId)) continue;
        const Gate& currGate = netlist.getGate(currGateId);

        for (int inNetId : currGate.inputNetIds) {
            // 如果追溯到了 Cut 邊界 (Leaf)，就停止往下找
            if (isLeafNet(inNetId)) continue;

            // 找驅動這條線的上一級 Gate
            int driverId = netlist.getNetDriverGateId(inNetId);
            if (driverId == -1 || !netlist.isValidGateId(driverId)) continue;

            // 初始化 Fanout 計數 (如果還沒被登記過)
            if (simulatedFanout.find(driverId) == simulatedFanout.end()) {
                const Gate& driverGate = netlist.getGate(driverId);
                int outNetId = driverGate.outputNetId;
                
                if (outNetId != -1) {
                    const Net& outNet = netlist.getNet(outNetId);
                    int actualFanout = outNet.loadGateIds.size();
                    
                    // 如果這條線是 PO，給它一個天文數字的假負載，保證它絕對不會被歸零拔除
                    if (outNet.isPO) {
                        actualFanout += 10000; 
                    }
                    simulatedFanout[driverId] = actualFanout;
                } else {
                    simulatedFanout[driverId] = 0;
                }
            }

            // 模擬拔除：因為 currGate 被我們假裝拔掉了，所以 driverId 少了一個聽眾
            if (simulatedFanout[driverId] > 0) {
                simulatedFanout[driverId]--;
                
                // 如果聽眾歸零，代表它完全包覆在 Cut 裡面，沒有外部扇出了！
                if (simulatedFanout[driverId] == 0) {
                    derefQueue.push(driverId);
                    mffcVolume++; // MFFC 體積 +1
                }
            }
        }
    }

    // 3. 設定 CutScore (利用 operator< 自動比較)
    
    // 首要指標：能拔除的 Gate 數量越多越好
    score.primaryScore = mffcVolume;      
    
    // 次要指標：AREA 模式比較少用次要指標，預設給 0 即可
    score.secondaryScore = 0;             
    
    // 懲罰指標：Leaf 數量 (輸入腳位) 越少越好。
    // 因為在 operator< 裡面，penalty 是從小比到大，我們希望 size 越小的贏，
    // 所以我們存入負的 size，這樣 size=4 的 penalty (-4) 就會大於 size=7 的 (-7)！
    score.penalty = -cut.size;            

    return score;
}

// 輔助函式：DEPTH 模式下的 Cut 評分機制
CutScore Optimizer::evaluateDepthCut(Netlist& netlist, 
                                          const KCut& cut, 
                                          int rootGateId, 
                                          const std::unordered_set<int>& criticalGateSet) { // <--- 直接接收 Set
    CutScore score;

    // 1. 建立邊界查表 (使用 Lambda 搭配小陣列走訪)
    auto isLeafNet = [&](int netId) {
        for (int i = 0; i < cut.size; ++i) {
            if (cut.leafNetIds[i] == netId) return true;
        }
        return false;
    };

    // 2. 局部 BFS 走訪 (計算 Critical Volume)
    std::queue<int> q;
    std::unordered_set<int> visited;
    int criticalVolume = 0;

    q.push(rootGateId);
    visited.insert(rootGateId);

    while (!q.empty()) {
        int currGateId = q.front();
        q.pop();

        // 直接使用外部傳入的 criticalGateSet 進行查詢
        if (criticalGateSet.count(currGateId)) {
            criticalVolume++;
        }

        if (!netlist.isValidGateId(currGateId)) continue;
        const Gate& currGate = netlist.getGate(currGateId);

        for (int inNetId : currGate.inputNetIds) {
            if (isLeafNet(inNetId)) continue;

            int driverId = netlist.getNetDriverGateId(inNetId);
            
            if (driverId != -1 && netlist.isValidGateId(driverId)) {
                if (visited.find(driverId) == visited.end()) {
                    visited.insert(driverId);
                    q.push(driverId);
                }
            }
        }
    }

    // 3. 設定 CutScore
    score.primaryScore = criticalVolume; 
    score.penalty = -cut.size;
    score.secondaryScore = evaluateAreaCut(netlist, cut, rootGateId).primaryScore;

    return score;
}

// 全 netlist 的 NOT(NOT x) → x 消除。DepthOptimizer 专用，直接砍關鍵路徑深度。
// 回傳消除的 NOT 對數。
//
// 用 worklist 取代「只要這一輪有任何 collapse 就整個 gate 陣列重掃一次」的作法：
// 後者對長串 NOT chain（例如 mockturtle basis enforcement/absorption 疊出來的
// 一長串 NOT）在 gate id 順序與邏輯鏈方向不一致時，會是 O(gate 數 * chain 長度)；
// 這個函式在單一 executeCriticalPathOptimization() 呼叫中會被呼叫十幾次，足以
// 在百萬閘電路上造成明顯的 timeout 風險。這裡只保留「第二顆 NOT」被拔除、
// 「第一顆 NOT」留給後續 cleanup pass 的既有語意；唯一會因為一次 collapse 而
// 改變資格的候選，是 outNet 原本的下游負載——它們的 input 被 redirect 到
// srcNet，若它們本身也是 NOT gate，「自己的 driver 是不是 NOT」這個判斷條件
// 就可能因此改變，需要重新入列檢查。
int Optimizer::eliminateDoubleInverters(Netlist& netlist) {
    int removed = 0;

    std::queue<int> worklist;
    std::unordered_set<int> queued;
    auto enqueue = [&](int gateId) {
        if (gateId < 0 || gateId >= (int)netlist.getGateCount()) return;
        if (netlist.getGate(gateId).type != GateType::NOT) return;
        if (queued.insert(gateId).second) worklist.push(gateId);
    };
    for (int g = 0; g < (int)netlist.getGateCount(); ++g) enqueue(g);

    while (!worklist.empty()) {
        const int g = worklist.front();
        worklist.pop();
        queued.erase(g);

        Gate& gate = netlist.getGateMutable(g);
        if (gate.type != GateType::NOT) continue;
        if (gate.inputNetIds.empty() || gate.inputNetIds[0] < 0) continue;

        int midNet = gate.inputNetIds[0];

        // 讀取驅動 midNet 的閘 (drv)
        int drv = netlist.getNet(midNet).driverGateId;
        if (drv < 0 || drv >= (int)netlist.getGateCount()) continue;
        if (netlist.getGate(drv).type != GateType::NOT) continue;          // 上游也是 NOT
        if (netlist.getGate(drv).inputNetIds.empty()) continue;

        int srcNet = netlist.getGate(drv).inputNetIds[0];                  // 第一顆 NOT 的輸入
        int outNet = gate.outputNetId;                                     // 第二顆 NOT 的輸出
        if (srcNet < 0 || outNet < 0) continue;

        // PO 保護：outNet 是 PO 就不動（避免 PO 失去 driver / 語意錯亂）
        if (netlist.getNet(outNet).isPO) continue;

        // redirectNetLoads 會清空 outNet.loadGateIds，要在那之前先記下這批
        // gate id，才能在 redirect 完成後重新檢查它們是否新符合 collapse 條件。
        const std::vector<int> rewiredLoads = netlist.getNet(outNet).loadGateIds;

        // 把 outNet 的所有負載改吃 srcNet（= NOT(NOT x) = x）
        redirectNetLoads(netlist, outNet, srcNet);   // 呼叫 Optimizer 內部的 helper
        detachGate(netlist, g);                       // 刪掉第二顆 NOT（第一顆留給後續的清理 pass 或邏輯最佳化）
        ++removed;

        for (int loadGateId : rewiredLoads) enqueue(loadGateId);
    }
    return removed;
}

template <typename NtkT, typename ToNetlistFn>
opt::CostMeasurement Optimizer::evaluateCandidate(
        const NtkT& ntk,
        const Netlist& templateNetlist,
        ToNetlistFn&& toNetlist,
        const opt::CostTarget& cost) {
    Netlist probe = toNetlist(ntk, templateNetlist);
    eliminateDoubleInverters(probe);
    probe.trimDeadLogic();          // 讓 gateCount tie-break 有意義
    return opt::measureCost(probe, cost);
}
