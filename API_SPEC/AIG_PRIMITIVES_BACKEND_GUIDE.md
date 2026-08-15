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
src/SATEngine/SatEngine.cpp
src/SATEngine/Fraig.cpp
```

`SatEngine` 與 `Fraig` 已有 Phase B 實作，`Netlist` 也保留 private lazy owner。
scalar Function Query 曾完成試接與 correctness/performance benchmark，但官方 workload
以大型電路、少量且非重複 query 為主，cold build 成本無法攤平，因此 production
`runFunctionQuery()` 已切回 legacy backend。Function Search 的 `EquivalentGatePairs`
已局部使用 private owner 做 batch class proof；NAND FindAny/FindAll 與其他 Function
Query 仍維持 legacy。Phase B 不是獨立 public facade。

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

## 3.1 Phase B 能力

- lazy incremental AIG-to-CNF encoding 與共用 CaDiCaL session
- unconditional / assumption-based equivalence 與 constant proof
- counterexample 保存與 FRAIG 反例回收
- simulation-only candidate filtering
- SAT-verified equivalence class、constant class 與 representative lookup
- AIG grow 後 lazy sync，Netlist revision 改變後整組 engine rebuild
- simulation memory、單次 SAT time/conflict 與整次 sweep budget

`Equal / NotEqual / Unknown` 仍是唯一 proof 語意。simulation collision 不可當成
等價證明；SAT Unknown、budget exhaustion 或 untrusted signal 都不得合併。

## 4. 建構與生命週期

```cpp
Netlist current;
// High-level Netlist member implementations obtain the private lazy owner.
// Callers do not construct or retain a second production Primitives instance.
```

`Netlist` 私有保存一個 lazy `std::unique_ptr<eqeng::Primitives>`。第一次內部 Boolean
backend 呼叫才建立；同一個正式 `Netlist` 不會由各個高階 API 各建一份 AIG cache。

`Primitives` 只以 `const Netlist&` 讀取 named design。Netlist copy、move、assignment、
clone 與 restore 都不複製或搬移 cache；新物件在下次需要時重新 lazy build。這避免
cache 仍引用舊地址，也避免 rollback 後沿用舊 AIG。

## 5. Freshness 與 revision

所有 Netlist mutation 必須呼叫 `markDirty()` 並增加 revision。每個 `Primitives` 自行保存 `builtRevision`：

```text
builtRevision == netlist.revision
  -> model fresh

builtRevision != netlist.revision
  -> lazy rebuild
```

freshness 以每個 cache 的 `builtRevision` 為準；`dirty` 只保留為共享診斷狀態，不能
單獨作為 cache 是否有效的依據。正式高階 API 一律使用 Netlist 的唯一 owner。

`restoreFrom()` 先丟棄 owner，再將 revision 設為 current/backup 兩者最大值加一，保持
單調增加。一般 copy/move assignment 同樣丟棄 owner，因此不可能保留指向舊設計的 cache。

## 6. Gate arity 與建模

- AND/OR/NAND/NOR/XOR/XNOR 接受至少一個 input，並按 `inputNetIds` 順序折疊全部輸入。
- NAND/NOR/XNOR 是對完整 fold 的結果反相，不是逐級串接同類反相 gate。
- NOT/BUF 必須恰好一個 input；零個或多個都將 model 標為 `Invalid`。
- 拓撲排序以完整 distinct input set 計算 dependency，支援非相鄰 tied input。

## 7. Generation 與 cache

AIG rebuild 後：

- generation 遞增
- 舊 `SigRef` 與 `Cut` 必須被拒絕
- cofactor/cone cache 清空
- cofactor cache 使用完整 `(function, variable, value)` key 與 equality，不能把 hash 當唯一識別

每次高階分析都應從名稱重新 resolve，不能跨 edit 保存 `SigRef`。

## 8. Proof 安全語意

- `Invalid` model 不得產生已證明的答案。
- `Unknown` 與 timeout 不得當作 `Equal` 或普通 false。
- 正式路徑使用 `StaleSigPolicy::Throw`。
- `UnknownPolicy::AsEqual`、`StaleSigPolicy::Ignore` 與 `RebuildAndWarn` 只可用於低階診斷，不得用於競賽答案或 edit acceptance。
- high-level API 必須將 engine 結果映射回既有 report contract。

## 9. 高階 API 接入規則

接入時只替換內部 proof function：

```text
existing high-level implementation
  -> internal proof call
     legacy SAT or Primitives
  -> existing report assembly
```

禁止建立 `runXWithAigBackend()` 形式的第二套 public API，禁止在 AIG adapter 內複製整份 report 邏輯。正式切換前必須以相同 testcase 比較答案、Unknown/timeout、counterexample、完整性與時間。

試接期間已驗證 scalar adapter 可讓 lazy AIG rebuild 與 incremental SAT 共用
request deadline；該 adapter 目前不在 production 路徑。private owner 仍關閉自動
FRAIG sweep；日後接入 batch search/report 時，必須把同一 deadline 傳入整次 sweep，
不得直接沿用 FRAIG 的無總上限預設。

## 10. 驗證

```text
mini test/test36/test36.cpp
mini test/test37/test37.cpp
mini test/test40/test40.cpp
mini test/test41/test41.cpp
mini test/test42/test42.cpp
mini test/test43/test43.cpp
mini test/test44/test44.cpp
```

- test36：model health、floating、invalid arity/model、taint complement 與局部 CEC（22 checks）。
- test37：proof、cofactor cache、revision freshness、stale SigRef、deadline、任意 fan-in 與 Netlist value lifecycle（22 checks）。
- test40：production legacy Function Query 與 Phase A AIG golden differential。
- test41：官方 60/300 秒 request 與各高階 API 預設 budget。
- test42：Phase B assumptions、proof ledger、counterexample、mutation、FRAIG、memory-limit termination、untrusted report 與固定亂數 DAG（22 checks）。
- test43：以官方 NewTestCase 直接比較獨立 Phase B runner 與 production legacy Function Analysis SAT 的 cold、warm、mutation rebuild 與同 testcase session 時間。
- test44：以獨立 legacy scalar proof 比較 production EquivalentGatePairs Phase B；另保留 NAND prototype benchmark，不提供 public backend 開關。

Function Query、Function Search 等高階 API 的 regression 應繼續呼叫唯一既有入口，不透過獨立 AIG facade。

production differential 位於 `mini test/test40/test40.cpp`，維持 legacy Function Query
對 Phase A AIG golden。Phase B 的低階 correctness 由 test42 驗證，cold/warm backend
比較則集中在 test43；不再以 production 高階入口暗中切換 backend。

### 10.1 NewTestCase Phase B / legacy 效能結論

`mini test/test43/test43.cpp` 使用 NewTestCase 17/18/19/20/31/33/35，直接建立獨立
Phase B runner 與 legacy 對照，不改正式 `FunctionQuery` / `FunctionReport`。
constant family 的 legacy 對照同樣計算 can-be-0 與 can-be-1，確保雙方產生完整
report 所需資訊，避免工作量不對等。

```text
單筆 query：19/19 語意一致
Phase B cold wins：0/19
Phase B warm wins：19/19
mutation rebuild wins：0/19

官方 prompt session：7/7 語意一致
Phase B cold-session wins：1/7
Phase B warm-session wins：7/7
```

代表性 run 的單筆 cold speedup 中位數約為 `0.4x`，代表 Phase B 首次查詢約為
legacy 的 `2.5x` 耗時；warm speedup 中位數約為 `8.7x`，觀測範圍約 `4.8x` 至
`48x`。mutation 後重建的中位 speedup 約為 `0.4x`，首次 owner build/rebuild
成本仍明顯。微秒級數字會受系統排程影響，backend 決策以勝率、量級及多次中位數為主。

因此 production scalar Function Query 已回切 legacy，第二批 query type 也暫停接入。
只有當 batch/session 能明確大量重用 owner，或 AIG cold build 成本下降後，才重新評估
Phase B；正式選擇前仍須驗證 timeout/Unknown 契約，不能只依時間切換後得到不同
proof 語意。

詳細數據：

```text
mini test/test43/newtestcase_phase_b_benchmark.tsv
mini test/test43/newtestcase_phase_b_session_benchmark.tsv
```

### 10.2 Function Search batch 結論

test44 在 NewTestCase 29/30 的 EquivalentGatePairs FindAll 與 test35 NAND FindAny
得到相同完整結果，Phase B 約加速 `4.4x` 至 `7.8x`。這些 query 能反覆比較既有 AIG
signals，符合 incremental SAT 的使用方式。

test35 NAND FindAll 則不同：每個 surviving pair 都要建立新的 `NAND(a,b)` candidate。
重複 55 秒測試分別得到 512/512 complete 與 timeout 506/512，Phase B 約
54.46–55.76 秒；放寬到官方 290 秒後可穩定得到完整 512/512、Unknown=0，但仍約
55.85 秒，legacy 約 28.13 秒。production 暫不接入；NAND FindAll 需先有不持續擴張
常駐 AIG/SAT state 的 ephemeral 或 chunked proof backend。另以 mixed-gate synthetic
case 驗證跨 AND/NAND/NOR/XOR 重構，equivalent 4/4、NAND 10/10 pairs 一致。

另以 8 組固定 seed、48 至 2048 個 mixed-gate random DAG 執行 16 個 differential
cases，set/completion/Unknown 與植入 oracle 全部一致。EquivalentGatePairs FindAll
穩定加速約 3.18x–8.98x；NAND FindAny 在只需一次 proof 時多數為 0.17x–0.94x。
因此 Phase B 的適用條件是「可攤平 owner build 的重複 proof」，不能只依 query kind
判斷。EquivalentGatePairs 已完成 production 局部接入：candidate/simulation/report
保持原樣，只替換 bucket representative proof。最終 test44 為 22/22、Unknown=0、
mismatch=0；test26 C++ 28/28、CLI 18/18，test24 9/9。NAND modes 仍維持 legacy。

詳細數據：

```text
mini test/test44/function_search_phase_b_batch_benchmark.tsv
mini test/test44/function_search_phase_b_batch_benchmark_random.tsv
mini test/test44/function_search_phase_b_batch_benchmark_test35_nand_all_290.tsv
```
