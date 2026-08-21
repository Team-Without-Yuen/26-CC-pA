# API / Core Backend 待辦

本文件只記錄尚未完成的 public C++ API、Netlist/engine、optimizer core、sequential
pattern engine 與底層 regression。已完成事項必須寫入對應 API/Usage 文件後從本表移除。

`tools.cpp` parser/help/printer 與 `TOOLS_SPEC` 同步事項請放在 `TOOLS待更新表.md`。

## P1：Critical Path Optimization runtime blocker

大型 scoped optimization 仍可能在 core 內超時：

```text
- 單次 mockturtle primitive 尚缺 cooperative cancellation。
- ScopedFaninCone 不應先做全設計 XAG；需要 cone-isolated resynthesis。
```

已知結果：test22、test26 與 NewTestCase/test40 bounded smoke 可完成；NewTestCase/test33
曾在 120 秒外層 timeout。此問題屬 optimizer core，不由 tools parser 修正。

## P1：OptApply equivalence report 三態化

目前 changed candidate 必定嘗試 whole-design SAT，但 UNKNOWN/inconclusive 且沒有找到
mismatch 時，可能依 mockturtle function-preserving 假設接受。既有欄位
`functionallyEquivalent`、`wholeDesignEquivalent` 與 `EquivalenceMethod::WholeDesignSat`
無法單獨區分 SAT-proven 與 trusted-but-unproven；暫時只能由 warning
`This has not been proven by SAT` 判讀。

後續應新增明確 proof status（例如 `ProvenEquivalent / ProvenMismatch /
InconclusiveAccepted`），並讓 tools envelope 的 `complete` 與 `status` 不再混用 transaction
完成、candidate commit 與 proof completeness。文件已先依現行行為校正，不得再把
UNKNOWN 描述成已證明等價。

## P1：Function Analysis expression regression

現有 expression 已有 size/depth/incomplete 契約，但仍需補：

```text
- 極深 BUF/NOT chain 的 stack safety。
- size budget 與 depth limit 各自觸發時的 report flag。
- deadline、artifact footer 與 partial/complete 一致性。
```

不得為測試新增 hidden output cap，也不改既有 public 參數。

## P1：AIG deadline 與資源安全

尚未完成：

```text
- snapshot CEC、cofactor/equiv_under() 與首次 lazy AIG rebuild 接收同一個 cooperative deadline。
- Phase B budget、simulate-only、auto-sweep threshold 與大電路 memory regression。
```

AIG 只能作為既有高階 API 的 private Boolean backend；named netlist 仍是唯一 design state，
不得建立平行 public query/report facade。

## P2：Function Search backend

目前 `EquivalentGatePairs` 與 NAND FindAny 已使用 Phase B/共用 pattern engine。
尚未完成的候選：

```text
- NAND FindAll：需要 ephemeral/chunked proof 或可控制 solver growth 的設計。
```

test70 FindAny 的 legacy/engine 約 7.16/2.21 秒，結果一致且 Unknown=0；
FindAll 的 legacy/engine 約 59.29/121.49 秒，兩者均完整找到 512 組。
因此 production 依 mode 固定選擇 backend，不得只因 AIG 已存在就全面切換。

## P2：Function Search pattern type 擴充

目前 public operand search 只開放 `NAND(a,b)==target`。後續保留現有
`nand_pair` 相容入口，再新增統一 pattern search，覆蓋：

```text
BUF/NOT：單 operand search。
AND/NAND/OR/NOR/XOR/XNOR：雙 operand search。
MUX：不做 O(n^3) 直接窮舉，需獨立 functional decomposition/candidate reduction。
```

底層已有通用 `FunctionalPatternEngine` pattern builder/proof，但 public candidate
generation、simulation prefilter、report 與 CLI 尚未擴充。NAND benchmark 可作為
mode-aware backend 設計的基準，不能直接代表其他 gate type 的效能；正式接入前
至少要對每個 arity/type family 做 correctness differential，並對代表性大電路比較
FindAny/FindAll 的 surviving candidate 數與 runtime。

## P2：Sequential enable/hold report 與 role mapping

目前 Boolean enable/hold match 與具名 EN/DATA role mapping 共用部分 `complete`/candidate
診斷，容易讓呼叫端把「功能已證明」誤讀成「所有具名 role 都已完整搜尋」。處理順序固定為：

1. 先修 report 契約，分開 `patternDetectionComplete` 與 `roleMappingComplete`；
   `matchedDffCount` 只計功能已證明的 DFF。
2. 再以 D 對同顆 Q 的 AIG cofactor 建立 canonical enable/data function，並透過
   literal-to-name index 對照 named net；找不到單一 net 時仍保留 derived function。

第二階段不得先於 report 契約落地，且不改現有 public CLI 參數。

## 後期 Boolean/AIG 重構

下列能力集中處理，不在各 API 零散加入 heuristic：

```text
- arbitrary Q-free enable/hold decomposition。
- conditional/bus/batch equivalence、constant、dependence 與 symmetry proof。
- consistent witness 與 complement-equivalence report。
- Boolean expression stack-safe DAG/artifact contract。
- functionally-equivalent cleanup/edit 的 scalable proof。
```

正式接入時保留既有 query/report schema；Unknown 不得當成 NotEqual，edit/opt 也不得把
Unknown 當成已證明等價。

## 不列入本表

- `tools.cpp` parser/help/envelope 或 `TOOLS_SPEC` 文件同步。
- 已完成的 regression、QA 結論與歷史 benchmark。
- 官方保證不會出現在 netlist 的未知 primitive robustness。
- 由 LLM/testcase driver 管理的跨 prompt persistent constraints。
