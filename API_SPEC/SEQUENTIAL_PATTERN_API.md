# Sequential Pattern Query API 整理

這份文件整理 `SequentialPatternQuery` 的 register-control semantics、report contract 與分析限制。

使用方式與 prompt 範例請看：

```text
API_SPEC/SEQUENTIAL_PATTERN_USAGE.md
```

---

## 1. 功能範圍

`SequentialPatternQuery` 負責處理需要組合多種既有分析能力，才能推導出的暫存器控制語意。第一個公開模式是 `DffEnableHold`，對應以下題型：

```text
Report D-input enable/hold structures implemented through multiplexers or AND gates.
How many flip-flops have enable or hold structures?
```

這個 API 不會取代既有 API 的責任：

| 資訊 | 既有負責 API |
| --- | --- |
| DFF 清單 | `BasicQuery::ListDffs` |
| D/CK/RN/SN 與 Q 連線 | Direct Connectivity／DFF pin helper |
| D-input fanin cone | `ConeQuery` |
| Q-to-D path | `PathQuery` |
| Boolean／SAT 證明 | `FunctionQuery` |
| DFF.D depth | `DepthQuery` |

Sequential Pattern API 是一層精簡的語意整合 wrapper，不會重寫 graph traversal 或 SAT encoding。

## 2. 公開入口

```cpp
SequentialPatternReportSet runSequentialPatternQuery(
    const SequentialPatternQuery& query) const;
```

```cpp
enum class SequentialPatternQueryType {
    DffEnableHold
};

struct SequentialPatternQuery {
    SequentialPatternQueryType type;
    std::string dffName;  // 空字串表示所有 active DFF
    bool includeAndGatedCandidates;
    bool verifyCanonicalMatchesWithSat;
};
```

## 3. 第一版辨識語意

第一版會辨識下列功能等價的兩層 canonical feedback MUX：

```text
OR(AND(S, A), AND(!S, B))
NAND(NAND(S, A), NAND(!S, B))
AND(OR(S, A), OR(!S, B))
NOR(NOR(S, A), NOR(!S, B))
```

其中一個 data branch 必須包含目前分析的 DFF 自己的 Q net，另一個 branch 則視為載入新資料的 data branch。辨識時會正規化 BUF chain，以及具有反相語意的 `NOT(x)`、`NAND(x,x)`、`NOR(x,x)`。成對的反相 wrapper 可被跨越，因此能辨識 technology mapping 後產生的 NAND/NOT 或 NOR/NOT canonical form。

這些形式可涵蓋 canonical AND/OR/NOT feedback MUX，以及 basis conversion 後產生的 NAND/NOR 形式。Canonical 結構本身已能對任意 upstream function 建立精確的代數證明；使用者也可以選擇額外執行 SAT 驗證：

```text
D | enable=inactive == Q
D | enable=active   == data
```

若 D-input 只是直接由 AND gate 驅動，目前只會回傳 `AndGatedDataCandidate`。在官方說明 `D = EN & DATA` 是否算 enable/hold 之前，不會把這種結構計入 confirmed hold structure。

## 4. Report 語意

`DffInputPatternReport` 對每一顆被分析的 DFF 回傳一筆紀錄。`patterns` 可以包含 confirmed `MuxHold`，或仍等待官方定義的 candidate。

主要欄位：

| 欄位 | 語意 |
| --- | --- |
| `dffName`, `dNetName`, `qNetName` | DFF instance 與 D/Q pin nets |
| `qFeedbackObserved` | canonical local pattern 中有觀察到該 DFF 自己的 Q feedback |
| `enableNetName` | 正規化後的 select／control net |
| `dataNetName` | active branch 的原始 data net |
| `dataBranchNetName` | 實際接到 branch gate 的訊號；data literal 反相時會與 `dataNetName` 不同 |
| `activeLevel` | `1` 表示 active-high、`0` 表示 active-low、`-1` 表示未知 |
| `structuralMatch` | 是否符合 canonical gate structure |
| `holdFunctionallyProven` | inactive cofactor 是否已證明等於 Q |
| `loadFunctionallyProven` | active cofactor 是否已證明等於 data |
| `confirmed` | 是否計入 enable/hold 統計 |
| `semanticsPending` | 是否仍需要官方定義才能決定 |

`matchedDffCount` 會依照 DFF instance 去重。同一顆 DFF 即使產生多筆 pattern，也不會重複增加 flip-flop count。

## 5. Status 規則

| Status | 語意 |
| --- | --- |
| `OK` | 要求的 DFF 已完成分析 |
| `NO_DFF` | 設計中沒有 active DFF |
| `DFF_NOT_FOUND` | 指定 instance 不存在或不是 DFF |
| `INVALID_DFF_PINS` | D 或 Q pin 缺失 |
| `PARTIAL` | 部分 DFF 無法分析，或 SAT 沒有完整結論 |
| `UNSUPPORTED_QUERY` | 不支援的 query type |

成功但沒有找到 pattern 不算錯誤：

```text
ok = true
matchedDffCount = 0
status = OK
```

## 6. 尚待官方確認的事項

目前仍需保留以下限制：

1. 除了 canonical local forms，是否還必須辨識任意 Boolean-equivalent MUX 實作。
2. 沒有 Q feedback 的直接 AND-gated D-input 是否算 enable/hold。
3. D-input cone 中有巢狀 pattern 時，應只回報最外層控制結構，還是列出所有 nested patterns。

## 7. 實作與效能驗證

目前實作位於：

```text
src/analysis/SequentialPatternAnalysis.cpp
```

已完成的 focused verification：

```text
mini test/test19：15 passed, 0 failed
mini test/test20：13 passed, 0 failed；包含 edit sequence、PO + DFF.D whole-design equivalence 與 mapped pattern SAT 驗證
mini test/test21：12 passed, 0 failed；包含 CLI summary/detail/pagination/SAT/error contract
NewTestCase/test40 原始設計：分析 2585 顆 DFF，找到 1583 顆 canonical matches
NewTestCase/test40 完成 NAND/NOT mapping 與 cleanup 後：找到 1590 顆 confirmed matches、1975 顆 candidates
post-edit test40 sequential query：約 0.050 秒；完整 mapping/edit/equivalence/query flow 約 33.7 秒
test40 CLI summary：約 0.32 秒、1101 characters；all detail 預設限制 50 records
```

以上仍是 canonical pattern 掃描，不代表任意 Boolean-equivalent pattern 已完整覆蓋。mapping 後數量可能增加，因為原本非 canonical 外觀會被轉成可直接辨識的 basis form。
