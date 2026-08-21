# Sequential Query Tool

## 責任

`sequential_query` 只用於從 DFF D-input Boolean function 辨識同顆 DFF 的 enable/hold
語意。一般 DFF 列表用 `structure_query list_dffs`；clock/reset 直接驅動查詢用
Direct Connectivity。

## Command

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

## Routing

| Prompt | Command |
|---|---|
| 有多少 DFF 具 enable/hold | `sequential_query enable_hold all --summary-only` |
| 列出/報告所有 D-input enable/hold logic | `sequential_query enable_hold all` |
| 查指定 DFF | `sequential_query enable_hold <dff_name>` |

預設已同時支援 canonical 與功能等價 restructuring，不要為了「開啟功能判定」
自動加 `--functional-fallback`。這個 flag 只為舊 caller 與明確 mapping 調整參數保留。

## 輸出判讀

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

## 大型輸出

- `all --summary-only` 只回完整統計。
- `all` 會把完整 DFF records 寫到唯一 artifact，response 回 `output_file`。
- 不要因為輸出大而自行加 `--limit`；只有 prompt 明確要求視窗時才用 offset/limit。
- 回答時簡述數量與結論，完整明細提供 artifact 路徑。

## 狀態規則

- `complete:true`：所有目標 DFF 的 Boolean classification 完成。
- `complete:false` / `timed_out:true`：已確認 matches 仍有效，但不得將其餘 DFF 回答為確定 non-match。
- `DFF_NOT_FOUND`：名稱錯誤，不要猜測其他 DFF。
- `INVALID_DFF_PINS` / `UNSUPPORTED`：Boolean model 或 D/Q boundary 無法可靠建立。

## 相容參數

`--verify-sat`、`--functional-fallback` 與 simulation options 仍保留 parser 相容性。正式
classification 無法被這些 flag 關閉。只有 prompt 明確要求特定限制或除錯時，
才應調整 candidate/per-DFF/time 參數。
