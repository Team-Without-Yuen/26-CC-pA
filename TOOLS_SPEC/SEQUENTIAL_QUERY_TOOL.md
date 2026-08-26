# Sequential Query Tool

## 1. 責任

`sequential_query` 只用於從 DFF D-input Boolean function 辨識同顆 DFF 的 enable/hold
語意。一般 DFF 列表用 `structure_query list_dffs`；clock/reset 直接驅動查詢用
Direct Connectivity。

## 2. 選擇條件

當 proof object 是「同一顆 DFF 的 D-input Boolean function 是否包含以該顆 Q 為 feedback 的
enable/hold decomposition」時使用本工具。它分析的是 current-cycle sequential pattern，不是
一般 DFF inventory、clock/reset adjacency，也不跨 time frame 展開 state。

先決定 target scope 與 output shape：

| Semantic request | Command |
|---|---|
| 所有 DFF 的 matched/non-matched 統計，不需要 identities | `sequential_query enable_hold all --summary-only` |
| 所有 DFF 的完整 classification/role records | `sequential_query enable_hold all` |
| 已知 DFF instance 的 enable/hold property 與 roles | `sequential_query enable_hold <dff_name>` |

一般 DFF 名單、數量或 pins 使用 `structure_query`；某 clock/reset net 的直接 DFF loads 使用
Direct Connectivity；已知一般 signal 的 Boolean property 使用 `func_query`。不能因 prompt 出現
`DFF`、`enable` 或 `feedback` 單字就選本工具，必須確認要求的是 D-input/Q-feedback 的功能分類。

## 3. Command Grammar

```text
sequential_query enable_hold <all|dff_name> [--summary-only]
                 [--confirmed-only] [--include-no-pattern]
                 [--offset n] [--limit n] [--verify-sat]
                 [--functional-fallback]
                 [--max-functional-candidates n]
                 [--max-functional-matches n] [--functional-find-any]
                 [--resolve-functional-data|--no-resolve-functional-data]
                 [--max-functional-data-candidates n]
                 [--no-functional-simulation-filter]
                 [--functional-simulation-patterns 1..4096]
                 [--functional-per-dff-time-limit seconds]
                 [--functional-time-limit seconds]
```

## 4. Modes

| Mode | Target | 用途 | 主要輸出 |
|---|---|---|---|
| `enable_hold` | `all` | 分類所有 DFF；summary 或完整 records | `matched_dff_count`, `exists`, `output_file` |
| `enable_hold` | `<dff_name>` | 分類指定 DFF 並解析 enable/data/feedback roles | `matched`, `confirmed`, role/proof fields |

預設已同時支援 canonical 與功能等價 restructuring，不要為了「開啟功能判定」自動加
`--functional-fallback`。這個 flag 只為舊 caller 與明確 mapping 調整參數保留。

## 5. 輸出判讀

| 需求 | 讀取 |
|---|---|
| 數量 | `matched_dff_count` |
| 是否有任一 match | `exists` |
| 結果是否完整 | `complete`, `timed_out` |
| 單顆 DFF 是否 match | `matched`，並確認 pattern `confirmed:true` |
| enable/data/feedback | `enable_net_name`, `data_net_name`, `feedback_net_name` |
| polarity | `active_level_name`, `hold_level` |
| proof | `detection_method`, `solver_status`, `hold_functionally_proven` |

`enable_net_name` 或 `data_net_name` 空白可能是正常結果：代表 role 是 Q-free
Boolean function，但沒有單一具名 net。只要 `confirmed:true`，仍要算入
`matched_dff_count`。

`DATA_GATING_WITHOUT_HOLD_FEEDBACK` 代表像 `D=EN&DATA` 的 data gating，因沒有
同顆 Q feedback，不是 enable/hold match。

candidate/examined/unexamined 數字是 proof 後的具名 role mapping 診斷。
`functional_candidate_limit_reached:true` 不等於 Boolean 結果 partial；仍以頂層
`complete` 與 `timed_out` 判斷。

### 5.1 大型輸出

- `all --summary-only` 只回完整統計。
- `all` 會把完整 DFF records 寫到唯一 artifact，response 回 `output_file`。
- 不要因為輸出大而自行加 `--limit`；只有 prompt 明確要求視窗時才用 offset/limit。
- 回答時簡述數量與結論，完整明細提供 artifact 路徑。

### 5.2 狀態規則

- `complete:true`：所有目標 DFF 的 Boolean classification 完成。
- `complete:false` / `timed_out:true`：已確認 matches 仍有效，但不得將其餘 DFF 回答為確定 non-match。
- `DFF_NOT_FOUND`：名稱錯誤，不要猜測其他 DFF。
- `INVALID_DFF_PINS` / `UNSUPPORTED`：Boolean model 或 D/Q boundary 無法可靠建立。

## 6. Prompt Examples

```text
Prompt: How many flip-flops implement enable/hold behavior?
Command: sequential_query enable_hold all --summary-only
Read: matched_dff_count；先確認 complete=true
```

```text
Prompt: List every DFF with enable/hold behavior and report its roles.
Command: sequential_query enable_hold all
Read: matched_dff_count、complete、output_file
```

```text
Prompt: Does flip-flop ff3 implement enable/hold, and what are its enable and data roles?
Command: sequential_query enable_hold ff3
Read: matched、confirmed、enable/data/feedback fields
```

## 7. 組合流程

若同一題還要求 DFF inventory 或 clock/reset direct loads，分別呼叫 `structure_query list_dffs`
或對應 connectivity mode，再依同一具名 DFF/net 組合結果；不得用 enable/hold records 代替
一般 structural pin 查詢。

## 8. 限制與相容參數

`--verify-sat`、`--functional-fallback` 與 simulation options 仍保留 parser 相容性。正式
classification 無法被這些 flag 關閉。只有 prompt 明確要求特定限制或除錯時，
才應調整 candidate/per-DFF/time 參數。
