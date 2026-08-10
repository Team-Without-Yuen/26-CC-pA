# AIG Primitives 內部工具指南

本文件描述 `eqeng::Primitives` 作為高階 API 內部 Boolean proof 工具的使用方式。它不是獨立高階 API、LLM command、tools routing 或另一套 report facade。

## 1. 責任邊界

```text
唯一高階 API
  -> 共用參數驗證、名稱/bus、timeout、report
     -> 內部 Boolean proof helper
        -> eqeng::Primitives
```

`Primitives` 只應回傳 Boolean proof 所需的低階結果，例如：

- `Equal / NotEqual / Unknown`
- timeout / unsupported
- counterexample 或 functional class 等 engine 資訊

它不得：

- 解析 LLM prompt 或 CLI 參數
- 複製高階 API 的參數檢查
- 組裝 `FunctionReport`、`EditReport` 或 tools envelope
- 取代 named netlist 的 gate/name/path/depth/connectivity 語意
- 對外暴露 `SigRef`、AIG node 或 CNF variable

## 2. 核心檔案

```text
include/SATEngine/AigBuilder.h
include/SATEngine/NameMap.h
include/SATEngine/Primitives.h
include/SATEngine/SatEngine.h
include/SATEngine/Fraig.h
include/SATEngine/Types.h
src/SATEngine/AigBuilder.cpp
src/SATEngine/NameMap.cpp
src/SATEngine/Primitives.cpp
src/SATEngine/Session.cpp
src/SATEngine/EngineStubs.cpp
```

`SatEngine` 與 `Fraig` 目前仍為 Phase B 介面與 stub，不可啟用。

## 3. Phase A 能力

- named netlist 轉 AIG，支援題目 gate types
- net name/ID 與 AIG signal mapping
- floating/undriven signal 以 free PI 保留
- DFF.Q pseudo-PI 與 DFF next-state comparison point
- model health：`Sound / Conservative / Invalid`
- equivalence 與 constant proof
- deadline-aware Phase A proof
- cofactor、support、dependence、unateness、symmetry
- cut、truth table、NPN classification
- snapshot CEC 基礎

Phase A 每次非 trivial proof 仍可能重新建立 solver/CNF，因此只能視為正確性基準，不代表大量 query 會加速。

## 4. 建構與生命週期

```cpp
Netlist current;
eqeng::Primitives primitives(current);
```

`Primitives` 保存 `current` 的 reference，因此其生命週期不得超過 Netlist，也不能在 Netlist move 到新地址後繼續使用。

高階 session 可以保存一個內部 `Primitives` 以重用 AIG cache，但該 session 不應提供另一套高階 query facade。新設計整份取代 `Netlist` 時，應銷毀並重建 `Primitives`。

## 5. Freshness 與 revision

所有 Netlist mutation 必須呼叫 `markDirty()` 並增加 revision。每個 `Primitives` 自行保存 `builtRevision`：

```text
builtRevision == netlist.revision
  -> model fresh

builtRevision != netlist.revision
  -> lazy rebuild
```

這使多個暫時性 Primitives 不會因其中一個呼叫 `clearDirty()`，讓另一個誤用舊 AIG。正式設計仍應避免長期建立多份 engine，以免重複耗用記憶體。

`restoreFrom()` 必須保持 revision 單調增加。一般 Netlist assignment 若可能複製較舊 revision，呼叫端應重建 Primitives。

## 6. Generation 與 cache

AIG rebuild 後：

- generation 遞增
- 舊 `SigRef` 與 `Cut` 必須被拒絕
- cofactor/cone cache 清空
- cofactor cache 使用完整 `(function, variable, value)` key 與 equality，不能把 hash 當唯一識別

每次高階分析都應從名稱重新 resolve，不能跨 edit 保存 `SigRef`。

## 7. Proof 安全語意

- `Invalid` model 不得產生已證明的答案。
- `Unknown` 與 timeout 不得當作 `Equal` 或普通 false。
- 正式路徑使用 `StaleSigPolicy::Throw`。
- `UnknownPolicy::AsEqual`、`StaleSigPolicy::Ignore` 與 `RebuildAndWarn` 只可用於低階診斷，不得用於競賽答案或 edit acceptance。
- high-level API 必須將 engine 結果映射回既有 report contract。

## 8. 高階 API 未來接入規則

接入時只替換內部 proof function：

```text
existing high-level implementation
  -> internal proof call
     legacy SAT or Primitives
  -> existing report assembly
```

禁止建立 `runXWithAigBackend()` 形式的第二套 public API，禁止在 AIG adapter 內複製整份 report 邏輯。正式切換前必須以相同 testcase 比較答案、Unknown/timeout、counterexample、完整性與時間。

## 9. 驗證

```text
mini test/test36/test36.cpp
mini test/test37/test37.cpp
```

- test36：model health、floating、invalid model 與安全拒絕。
- test37：直接測 Primitives proof、cofactor cache、per-instance revision freshness、stale SigRef 與 deadline。

Function Query、Function Search 等高階 API 的 regression 應繼續呼叫唯一既有入口，不透過獨立 AIG facade。
