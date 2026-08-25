# Function Search API 整理

這份文件整理 `FunctionSearchQuery` 的設計、責任與內部 contract。

使用方式與 prompt 範例請看：

```text
API_SPEC/FUNCTION_SEARCH_USAGE.md
```

---

## 1. 核心定位

`FunctionSearchQuery` 處理「候選名稱未知，必須掃描設計才能找到答案」的 Boolean search。目前公開支援：

```text
FunctionalPatternOperands
NandEquivalentInputPairs (compatibility alias)
EquivalentGatePairs
FunctionalConstantSignals
ComplementaryPairs
```

`FunctionalPatternOperands` 搜尋使指定 BUF/NOT/AND/NAND/OR/NOR/XOR/XNOR
function 等價於 target 的既有 signals；`NandEquivalentInputPairs` 保留原有 NAND 呼叫相容性。
`EquivalentGatePairs` 搜尋 output function 完全相同的 active combinational gate pairs，並整理成等價類。
`FunctionalConstantSignals` 在候選名稱未知時，批次搜尋 functionally constant 0/1 signals；
literal constant net 不列入「derived functional constant」結果。
`ComplementaryPairs` 搜尋 `f(a)=!f(b)` 的 named signals 或 active combinational gate outputs，
不要求兩者具有固定 NOT/NAND/NOR 結構。

本 API 是 read-only analysis，不會 merge gate、rewire 或修改 netlist。修改必須交給 `EditApply`，並依題意執行修改後等價驗證。

---

## 2. 資料模型與邊界

搜尋以 current netlist 的 scalar nets 與 active combinational gates為基礎；PI 與 DFF.Q 是正式
function boundary。目前沒有有效 driver 的 scalar net 也會被 simulation/SAT 視為 unconstrained
Boolean leaf，不會自動當成 constant 0。這能保守分析 floating net，但不代表 floating 狀態已被
修復或轉成 PI。

`EquivalentGatePairs` 比較的是兩顆 gate 的 output function：

```text
gateA.output XOR gateB.output
```

若 miter 在所有 PI / DFF.Q assignments 下皆 UNSAT，兩者才會進入同一個等價類。simulation signature 只做候選分桶，不構成功能等價證明。

scope 可限制在 whole design 或既有 cone：

```text
WholeDesign
NetFanin / NetFanout
GateFanin / GateFanout
```

scope 的 gate 集合沿用 `ConeQuery` 語意；fanin 包含驅動 root net 的 combinational gate，fanout 從 root net 往下游搜尋。

---

## 3. Query 型別

```cpp
enum class FunctionSearchQueryType {
    NandEquivalentInputPairs,
    FunctionalPatternOperands,
    EquivalentGatePairs,
    FunctionalConstantSignals,
    ComplementaryPairs
};

enum class FunctionSearchConstantFilter {
    Zero,
    One,
    Either
};

enum class FunctionSearchCandidateDomain {
    Signals,
    CombinationalGateOutputs
};

enum class FunctionSearchMode {
    FindAny,
    FindAll
};

enum class FunctionSearchScope {
    WholeDesign,
    NetFanin,
    NetFanout,
    GateFanin,
    GateFanout
};
```

共同限制：

| 欄位 | 預設值 | 語意 |
|---|---:|---|
| `mode` | `FindAny` | 找第一組 witness，或完整分類 |
| `maxResults` | `0` | `0` 表示不限制；正值只用於題意明確要求前 N 筆時 |
| `maxStoredMatches` | `256` | `matches` 在記憶體中保留的 sample 數；`0` 表示不保留 |
| `simulationPatternCount` | `256` | deterministic bit-parallel patterns，合法範圍 1..4096 |
| `timeLimitSeconds` | `290.0` | 必須為有限正數；simulation、分類與 SAT 共用總時間，保留 10 秒供 request 收尾 |
| `expandEquivalentPairs` | `true` | false 時只回 equivalence classes，不展開平方級 pair records；functional merge 內部使用 |
| `writeMatchesToFile` | `false` | 是否以 streaming 將 pair records 寫到檔案 |
| `outputFilePath` | 空字串 | 指定 artifact 路徑；公開 CLI 會自動提供不覆寫的檔名 |

`FunctionalConstantSignals` 使用：

| 欄位 | 語意 |
|---|---|
| `constantFilter` | `Zero`、`One` 或 `Either` |
| `internalSignalsOnly` | true 時搜尋 active named driven non-port nets，包含 internal DFF.Q；false 時額外納入 PI/PO |
| `scope`, `scopeName` | whole design 或指定 fanin/fanout cone |

此 mode 排除 removed、unnamed、literal constant 與 floating/undriven nets。DFF.Q 是合法的
current-state Boolean source；若同時是 PO，仍依 port policy 決定是否納入。

`FunctionalPatternOperands` 與 `NandEquivalentInputPairs` 使用：

| 欄位 | 語意 |
|---|---|
| `targetNetName` | existing scalar target |
| `patternGateType` | 通用 mode 必填；支援 BUF/NOT/AND/NAND/OR/NOR/XOR/XNOR；legacy NAND mode 固定為 NAND |
| `internalSignalsOnly` | true 時排除 PI、PO、constant、undriven 與 target；保留有有效 driver 的 internal nets，包含 internal DFF.Q。DFF.Q 若同時為 PO，依 PO policy 排除 |
| `allowSameSignalPair` | 是否允許 `(a,a)` |
| `scope`, `scopeName` | 可限制 operand 候選於 whole design 或指定 fanin/fanout cone |

`internalSignalsOnly=false` 時會納入 PI，以及有有效 driver 的 PO/internal nets；因此也會納入
同時是 PO 的 DFF.Q。constant、target
本身與非 PI undriven nets 仍排除。Pair 為 unordered，不會同時回 `(a,b)` 與 `(b,a)`；只有
`allowSameSignalPair=true` 才加入 `(a,a)`。

BUF/NOT 為一元搜尋，`operandArity=1`，match 只填 `operandNetIds[0]`；AND/NAND/OR/NOR/
XOR/XNOR 為二元搜尋，`operandArity=2`。MUX 不走本 mode，避免 O(n^3) 枚舉。

`EquivalentGatePairs` 使用：

| 欄位 | 語意 |
|---|---|
| `scope` | 搜尋範圍 |
| `scopeName` | 非 whole scope 的 net/gate 名稱 |
| `gateTypeFilter` | `UNKNOWN` 代表全部 combinational types；也可限制單一 type |

`ComplementaryPairs` 使用：

| 欄位 | 語意 |
|---|---|
| `candidateDomain` | `Signals` 或 `CombinationalGateOutputs` |
| `scope`, `scopeName` | whole design 或指定 fanin/fanout cone，只限制候選集合 |
| `internalSignalsOnly` | signal domain 預設排除 PI/PO，保留 internal DFF.Q；false 時納入 PI/PO |
| `gateTypeFilter` | 只適用 gate domain；`UNKNOWN` 代表全部 combinational types |

兩個 domain 都排除 removed、unnamed、literal constant 與 floating/undriven candidates。gate domain
不包含 DFF；signal domain 的 internal DFF.Q 是合法 current-state Boolean source。scope 不會縮小 proof
所需的 PI/DFF.Q assignment 空間，因此 scoped 結果仍是完整 Boolean relation，而非局部假設。

---

## 4. Report Contract

共同完整性欄位：

| 欄位 | 語意 |
|---|---|
| `ok` | request 合法，且目前答案可安全使用 |
| `found` | 至少找到一組 SAT-proven match |
| `complete` | FindAny 已回答 exists，或 FindAll 已完成 |
| `allCandidatesExamined` | 所有候選均已完成分類 |
| `timedOut` | 已超過共用時間上限 |
| `truncated` | pair 展開達到 `maxResults` |
| `unsupported` | 部分 function cone 無法編碼或分析 |
| `status`, `message` | machine-readable 狀態與說明 |

結果保存與輸出欄位：

| 欄位 | 語意 |
|---|---|
| `matchCount` | 搜尋目前找到的 match 數；完整 FindAll 時為完整總數 |
| `matches` | 最多 `maxStoredMatches` 筆記憶體 sample，不代表完整總數 |
| `wroteMatchesToFile` | 是否成功建立 streaming artifact |
| `outputFilePath` | 完整或受明確 `maxResults` 限制的 records 檔案路徑 |
| `patternGateType`, `patternTypeName` | 實際要求的 Boolean pattern |
| `operandArity` | 一元為 1，二元為 2；equivalent-gate mode 為 0 |
| `constantFilter` | constant search 的 0/1/either filter；其他 mode 不適用 |
| `constantSignals` | 記憶體中的 proven constant records；FindAll streaming 時可為空 |
| `constantZeroCount`, `constantOneCount` | 已證明的 constant-0/1 結果數 |
| `candidateSignalsRejectedBySimulation` | 已由 concrete simulation assignment 排除的候選數 |
| `provenNonConstantSignalCount` | 已確實證明同時可為 0 與 1 的候選數；specific filter 的反方向 constant 不會誤算 |
| `inconclusiveSignalCount` | unknown/unsupported、不能安全分類的候選數 |
| `candidateDomain` | complementary search 的 signals/gates domain |
| `complementaryClasses` | 正負 phase members；每個 positive member 與每個 negative member 互補 |
| `complementaryClassCount` | 至少各有一個正/負 member 的 proven phase class 數 |
| `complementaryPairCount` | 所有 class 的 `positive.size * negative.size` 總和 |
| `inconclusiveCandidateCount` | complementary search 中未能完整分類的候選數 |

### 4.1 Streaming Artifact Contract

FindAll streaming 使用 `FUNCTION_SEARCH_ARTIFACT_V2`。檔案 header 必須讓 reader 不依賴
terminal envelope 也能還原 query：`query_type`、`search_mode`、target/pattern、operand arity、
scope/scope name、gate-type filter、candidate domain、boundary/same-signal policy，以及
`result_policy`/`max_results`。不適用欄位固定寫 `not_applicable`，不可用空字串表示。
Pattern/NAND artifact 另明確寫出 `include_pi_po_port_signals`、
`internal_dff_q_signals_included` 與 `dff_q_output_port_requires_boundary_opt_in`，避免把
timing boundary 與 port opt-in 混為同一概念。

既有 `Match N`、`Equivalence class N` records 與 `Total matches`/`Complete`/`Status` footer
保持相容。V2 另以 `records_begin`/`records_end` 劃分 records，footer 補上 candidate、SAT、
unknown、class/pair 與完整性統計。artifact 是 query output，不是可重播的 mutation request，
也不代表後續 command 會自動引用上一份結果。

constant search 使用 `Constant signal N` records，包含 net ID/name、driver gate ID/name/type、
`constant_value`、`proof_method` 與 `solver_status`。header 另含 `constant_filter`、
`constant_signals_excluded:false` 與 `literal_constant_nets_excluded:true`；footer 含 0/1、
non-constant 與 inconclusive counts。

complementary search 使用 `Complementary class N` records。每筆 class 明確寫出
`positive_members x negative_members` 的重建規則、兩側完整 net/gate identity、`pair_count` 與 proof
method；因此大型結果不需要逐 pair 展開，也沒有隱藏數量截斷。footer 另含
`complementary_class_count`、`complementary_pair_count` 與 `inconclusive_candidate_count`。
若 caller 明確設定 `maxResults`，artifact 改寫前 N 筆 literal complementary pair records，header
標記 `literal_pairs_explicit_limit`，且 report 保持 `truncated=true`；不會用完整 class 偷渡超出
prompt 限制的 pairs。

`EquivalentGatePairs` 主要統計：

```text
candidateGateCount
candidateSignalCount
simulationEligibleSignalCount
simulationBucketCount
candidatePairsConsidered
candidatePairsRejectedBySimulation
satChecks
satUnknownCount
unsupportedSignalCount
equivalenceClassCount
equivalentPairCount
simulationPatternCount
elapsedSeconds
```

`equivalenceClasses` 保存 SAT-proven class 的 gate/net IDs 與 names。`equivalentPairCount` 是所有已證明 class 可展開的 pair 總數；`matchCount` 是 report 的結果數，而 `matches.size()` 只是 sample 數。啟用 streaming 時，完整 pair records 位於 `outputFilePath`，不會為了輸出而全部堆在記憶體。

每筆 equivalent-gate match 包含：

```text
gateIdA / gateNameA / netIdA / netNameA
gateIdB / gateNameB / netIdB / netNameB
provenEquivalent = true
proofMethod = SAT_EQUIVALENCE_CLASS
solverStatus = UNSAT
```

---

## 5. 高階入口與執行流程

```cpp
FunctionSearchReport runFunctionSearchQuery(
    const FunctionSearchQuery& query) const;
```

`EquivalentGatePairs` 執行流程：

```text
解析 scope 與 gate-type filter
  -> 收集 active combinational gate outputs
  -> deterministic bit-parallel simulation
  -> 依完整 signature 分桶
  -> 每桶以 representative Phase B incremental SAT comparison 建立等價類
  -> FindAny 回第一組 witness
  -> FindAll 展開 class pairs，逐筆計數、選擇性保存 sample、選擇性 streaming 寫檔
```

不同 simulation bucket 的 outputs 已有 concrete simulation assignment 證明不同，因此不需 SAT。相同 bucket 仍逐步做 SAT，避免把 simulation collision 當成 proof。

`FunctionalConstantSignals` 執行流程：

```text
解析 scope 與 candidate policy
  -> 對 current named netlist 做一次 deterministic bit-parallel simulation
  -> 依 zero/one/either filter 保留全 0 或全 1 signature
  -> 其他 signature 由 concrete assignment 證明不符合 filter
  -> survivors 以共用 Primitives 做 literal equality 或 incremental SAT constant proof
  -> 只有 Equal/UNSAT 結果可進 report/artifact
```

simulation 的全 0/全 1 signature 只代表候選，不能單獨列為 constant。SAT Unknown、unsupported
或 deadline 到期會使 FindAll `complete=false`；不得降格成 NO_MATCH。

`ComplementaryPairs` 執行流程：

```text
解析 candidate domain、scope 與 filter
  -> 一次 deterministic bit-parallel simulation
  -> 以 min(signature, complement(signature)) 建立 phase-aware buckets
  -> 不同 bucket 由 concrete assignment 安全排除
  -> 同 bucket 以 AIG literal phase 或 equiv_checked(candidate, !representative) 建立 proven classes
  -> FindAny 回第一組 witness；FindAll 回完整正負 phase classes 與 exact Cartesian pair count
```

simulation 互補只用於分桶，不是 positive proof。演算法避免 whole-design naive O(N^2) SAT；正式
proof 數量通常由 signature collision bucket 與 class representatives 決定。Unknown、unsupported 或
deadline 會保留已證明 classes，但使 `complete=false`。
若短 signature 產生超過 32 個 candidates 的 collision bucket，backend 會自動以 4096 patterns
重做一次線性 simulation refinement，再進 proof；這不改變候選、答案或 proof 標準。

Phase B 替換 `EquivalentGatePairs` 的 class proof backend，並用於 NAND pair
`FindAny` 與通用 pattern surviving-candidate proof。NAND `FindAll` 仍使用 legacy cone-miter
SAT，其他 pattern 使用共用 engine。scope、candidate 順序、
simulation、timeout 與 report assembly 均沿用原本流程；同一個 `Netlist`
使用 private lazy `Primitives` owner，caller 不提供 backend 選項。

---

## 6. 與其他 API 的責任界線

| 問題 | API |
|---|---|
| 已知 `n1`、`n2`，判斷是否等價 | `FunctionQuery::Equivalence` |
| 已知 condition，判斷兩 net 是否條件等價 | `FunctionQuery::ConditionalEquivalence` |
| 搜尋 `NAND(a,b)==target` 的未知 pair | `FunctionSearchQuery::NandEquivalentInputPairs` |
| 搜尋 BUF/NOT/AND/NAND/OR/NOR/XOR/XNOR 的未知 operands | `FunctionSearchQuery::FunctionalPatternOperands` |
| 搜尋任意 output function 相同的 gate pairs | `FunctionSearchQuery::EquivalentGatePairs` |
| 已知 net，判斷它是否 constant 0/1 | `FunctionQuery::AlwaysZero/AlwaysOne/ConstantValue` |
| 未知名稱，列出 functionally constant 0/1 signals | `FunctionSearchQuery::FunctionalConstantSignals` |
| 搜尋未知名稱的互補 signal/gate-output pairs | `FunctionSearchQuery::ComplementaryPairs` |
| 找結構完全相同的 duplicate gates | `EditApply::MergeStructurallyEquivalentGates` |
| 實際合併、rewire、清理 gate | `EditApply` |
| 修改前後 whole-design equivalence | `WholeDesignEquivalence` |

gate output 等價不等於該 gate 在 observability 上 redundant；本 API 不做任意 redundant-gate removal。

---

## 7. 限制與安全規則

- 目前 simulation/SAT 支援 AND、OR、NAND、NOR、NOT、BUF、XOR、XNOR combinational logic。
- 官方 netlist 只使用題目規定的 gate types；prompt 中的 MUX 等語意由合法 gates 組成的 Boolean function 表示。Direct MUX/unknown primitive 不屬競賽輸入 contract，因此不納入本 API 的 unsupported coverage。
- DFF 不作為候選 gate；internal DFF.Q 預設可作為 Boolean operand，且分析在 Q 停止、不跨 cycle。DFF.Q 若同時是 PO，依 port policy 由 `internalSignalsOnly=false` 納入。
- gate type filter 不接受 DFF 或 UNKNOWN 字串；C++ 的 `UNKNOWN` 值代表不過濾。
- `NO_MATCH` 只有在 `ok && complete && allCandidatesExamined` 時才是完整否定答案。
- TIMEOUT、SOLVER_UNKNOWN、unsupported 或 result truncation 都不得解讀為「不存在」。
- `FindAll` 預設不設數量上限；只有 caller 或 prompt 明確指定正值 `maxResults` 時才會回 `RESULT_LIMIT_REACHED`。
- CLI parser 拒絕重複 option、`--all`/`--find-any` 衝突，以及 FindAny 搭配
  `--max-results`；canonical option 與相容 alias 仍視為同一個 option。
- 公開 CLI 的 FindAll 一律自動 streaming 至 `<design>_function_search_<sequence>.txt`，terminal envelope 只回 summary 與路徑。
- 自動檔名會跳過既有檔案，不覆寫同目錄中的舊 artifact。
- `FindAll` 的 worst case 仍可能需要大量 SAT checks；scope、gate filter 與題目允許的 time limit 是主要規模控制。
- constant search 的 simulation 只做 sound rejection；只有 AIG literal equality 或 SAT proof 可產生 positive result。
- complementary search 的 simulation phase 也只做 sound rejection；只有 AIG phase identity 或 incremental SAT proof 可產生 positive pair/class。
- complementary signal/gate domain 的選擇屬 prompt 語意，不可由 backend 自動混用；`--gate-type` 只適用 gates，PI/PO opt-in 只適用 signals。
- 搜尋結果只證明 output functions 相同，不會自動套用 merge。

---

## 8. 實作與測試狀態

| 項目 | 路徑 |
|---|---|
| Query / Report 型別 | `include/core/NetlistQueries.h` |
| Public aliases / entry | `include/core/Netlist.h` |
| simulation、SAT、分類 | `src/analysis/FunctionAnalysis.cpp` |
| public CLI | `tools.cpp` |
| 通用 pattern 與 legacy NAND API regression（26/26） | `mini test/test23` |
| 通用 pattern 與 legacy NAND CLI regression（18/18） | `mini test/test24` |
| equivalent-gate API/CLI regression | `mini test/test26` |
| reference/Phase B differential 與大型 random DAG | `mini test/test44` |
| functional constant batch CLI regression（12/12） | `mini test/test63` |
| complementary phase-class CLI regression（12/12） | `mini test/test64` |

`test24` 覆蓋 NAND FindAny/FindAll、完整 artifact、明確 result limit、no-match、invalid input、timeout，以及衝突/重複 option 的 strict parser。`test26` 覆蓋跨 gate type 等價、多個 equivalence classes、FindAny/FindAll、完整 artifact、scope、gate filter、result limit、直接 timeout、functional merge 與 rollback。`test44` 以 reference/engine 雙路徑做官方、synthetic 與最大 2048-gate random DAG 比對；EquivalentGatePairs 的 reference 為獨立 legacy proof，NAND reference 為 production adapter。

`test63` 覆蓋 constant 0/1/either、FindAny/FindAll、PI/PO opt-in、DFF.Q boundary、cone
scope、完整 artifact、明確 result limit 與 strict parser。NewTestCase/test53 另以既有
`always_zero n16` 對照 batch zero search，兩者一致判定 n16 非 constant-zero；batch 完整分類
5,577 個 eligible signals，Unknown/timeout 均為 0。

`test64` 覆蓋 direct NOT、AND/NAND、OR/NOR、XOR/XNOR、De Morgan、同 phase 多成員、
signals/gates domain、NAND filter、whole/cone scope、internal DFF.Q、PI/PO opt-in、FindAny/FindAll、
explicit result limit、strict parser 與 self-contained Cartesian-product artifact。
NewTestCase/test53 的 complementary signal FindAll 在 refinement 前於 290 秒 timeout；refinement 後
約 36.4 秒完成 5,252 candidates、474 classes、526 pairs，Unknown/unsupported/timeout 均為 0。

NewTestCase production differential 實測（2026-08-20）：

| Case | Command | 結果 | Elapsed |
|---|---|---|---:|
| test29 原始設計 | `func_search equivalent_pairs whole --all` | 7/7 records；complete；約 6.27x | 約 0.44 秒 |
| test30 原始設計 | `func_search equivalent_pairs whole --all` | 1/1 record；complete；約 6.07x | 約 0.04 秒 |
| test70 | `func_search nand_pair n25` | 1 SAT-proven witness；complete；FindAny 使用 AIG incremental SAT | 約 0.99 秒（搜尋） |
| test70 | `func_search pattern OR n25` | direct target operands 優先後第一組即證明；AIG literal equality | 約 0.55 秒（含 read/query session） |

test29/test30 的 artifact record 數與 `match_count` 完全一致；test70 是存在性問題，FindAny 直接在 envelope 回 witness。test70 differential 的 legacy/AIG 結果同為 1 組、complete、Unknown=0，時間約 7.16/2.21 秒。FindAll 則同為 512 組、complete、Unknown=0，legacy/AIG 約 59.29/121.49 秒，因此保留 legacy。

通用 FindAny 會先檢查 target 直接 driver 的 eligible inputs，再保留原始完整候選順序作 fallback。
此排序不刪除候選、不影響 FindAll；test70 OR probe 的 simulation pair 數由 257,412,964 降為 1。

`mini test/test45` 另重播 test29/test30 在 functional merge 前的官方順序：AND/NOT basis conversion、dead/dangling cleanup、double-inverter collapse，再建立只讀 oracle 並呼叫 `EditApply::MergeFunctionallyEquivalentGates`。此時分別完整合併 361/494 顆；oracle class size、merge records、`mergedGateCount` 與 active-gate delta 完全一致，mandatory CEC、獨立 CEC、write/readback CEC 皆通過。
