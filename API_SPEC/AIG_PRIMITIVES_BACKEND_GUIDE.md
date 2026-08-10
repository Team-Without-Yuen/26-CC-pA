# AIG Primitives Backend 使用指南

本文件提供後端與高階 API 開發者使用 `eqeng::Primitives`。它不是 LLM command 規格，
也不要求修改 `tools.cpp`。

實作位置：

```text
include/SATEngine/AigBuilder.h
include/SATEngine/Primitives.h
src/SATEngine/AigBuilder.cpp
src/SATEngine/Primitives.cpp
src/SATEngine/Session.cpp
```

## 1. 定位與所有權

current named `Netlist` 是唯一正式設計；AIG 只是衍生的 Boolean functional index。
gate/net 名稱、gate type、depth、path、fanin/fanout 與輸出 Verilog 仍以 named netlist 為準。

一份 current `Netlist` 最終只能由 backend analysis context 持有一個共用 `Primitives`。
Function Search、Function Analysis、Sequential Pattern 與 CEC 必須共用同一個 instance，
不得各自在長生命週期物件中建立另一份 AIG cache。

目前高階 API 尚未接入這個 context；接入前可在 backend regression 中直接建構：

```cpp
#include "include/SATEngine/Primitives.h"

eqeng::Primitives::Config config;
config.verbose_rebuild = false;
eqeng::Primitives primitives(netlist, {}, config);
```

## 2. Lazy rebuild 與物件生命週期

建構 `Primitives` 時不建立 AIG。第一個需要模型的操作才 lazy build；named netlist mutation
必須經過會呼叫 `markDirty()` 的 API。下一個 query 會整體重建 AIG，generation 遞增。

所有舊 `SigRef` 與 `Cut` 在 rebuild 後失效：

```cpp
auto old = primitives.resolve("n1");
netlist.addNet("new_net");

// 重新取得；不可沿用 old。
auto current = primitives.resolve("n1");
```

跨修改應保存 net/gate 名稱，不應保存 `SigRef`、raw AIG signal 或 `Cut`。

## 3. ModelHealth

每次 build 都會產生 model health：

| 狀態 | 意義 | Boolean proof |
|---|---|---|
| `Sound` | 所有必要訊號均被忠實建模 | 允許 |
| `Conservative` | floating/undriven signal 已建成具名 free PI | 允許 |
| `Invalid` | 必要輸入、PO、DFF D、driver 或拓撲無法忠實建模 | 禁止 |

查詢方式：

```cpp
const auto health = primitives.model_health();
const auto reason = primitives.model_health_message();
```

PO-only floating net 也必須建立成 free PI，不得綁成 constant 0。未接 DFF RN/SN 依 cell
定義視為 inactive，屬合法情況；DFF D 缺失或已連接的 control 無法解析則為 `Invalid`。

## 4. Invalid model 的固定語意

Invalid model 絕對不能產生 proven Boolean answer。

下列 checked API 回 `EquivResult::Unknown`：

```text
equiv_checked
is_const_checked
equiv_under
is_const_under
depends_on_checked
equiv_to_snapshot / except / only
```

下列無法用回傳型別表達 Unknown 的功能會丟 `UnsoundModel`：

```text
equiv / is_const / is_const0 / is_const1
is_free_var / is_constant / is_complemented / raw / wrap
cofactor / functional_support / is_unate / is_symmetric
make_and / make_or / make_xor / make_mux
enumerate_cuts / truth_of / npn_matches / cut access
equivalence_classes
```

高階 API 接入後應捕捉 `UnsoundModel`，轉成自己的統一 report，不得讓例外穿透至 CLI。
`UnknownPolicy::AsEqual` 只處理 solver resource-limit Unknown，不能把 Invalid model 轉成 true。

## 5. 可在 Invalid model 上使用的診斷入口

下列入口不宣稱 Boolean proof，可用來診斷建模問題：

```text
model / model_health / model_health_message
resolve / try_resolve / names_of
comparison_points
stats
```

`model()`、`aig()` 與 `names()` 不應出現在一般高階 API；它們主要供 backend debugging
與 regression 使用。`raw()` / `wrap()` 仍受 model-health gate 保護，不可用來繞過 Invalid。

## 6. Phase A 能力邊界

目前正式可用的是 Phase A。Phase 切換介面保留給後續開發，本輪不修改其行為；現階段
不得把 Phase B SatEngine/Fraig stub 當成可用 backend。

Phase A 的重要限制：

```text
- equivalence_classes 只有 structural sharing，不是完整 functional equivalence classes。
- equiv_under / is_const_under 仍會物化 cofactor。
- functional_support、symmetry、unate 可能建立大量 cofactor。
- enumerate_cuts 每次可能掃描整顆 AIG。
```

因此導入既有 API 前必須以 testcase 比較答案、時間與記憶體，不能只因介面存在就替換。

## 7. CEC

```cpp
auto before = primitives.snapshot();
apply_named_netlist_edit(netlist);
const auto result = primitives.equiv_to_snapshot(before);
```

snapshot 是 move-only，並以名稱對齊 PI、PO、DFF Q 與 DFF next-state。若 before 或 after
模型 Invalid，CEC 回 `Unknown`，不得將它解讀成 equivalent 或 not-equivalent。

## 8. 測試

專用 regression：

```text
mini test/test36/test36.cpp
```

涵蓋 Sound DAG、gate-input floating、PO-only floating、missing gate input、DFF D、合法未接
RN/SN、組合迴路、Invalid CEC，以及 `UnknownPolicy::AsEqual` 不得繞過 model-health gate。
