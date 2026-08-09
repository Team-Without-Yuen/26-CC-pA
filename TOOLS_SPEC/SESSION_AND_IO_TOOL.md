# Session And I/O Tool

## 1. 責任

`read` 建立目前設計與 original snapshot；`write` 輸出 current design；`help` 顯示 public grammar；`quit`/`exit` 結束 session。所有 query、edit、report 與 equivalence command 都依賴同一個 session。

完整性規則：除 prompt 明確限制筆數外，不自行截斷結果；支援 file output 時寫入完整檔案，
正式答案只回必要摘要。時間限制依題目指定。詳見 [`LLM_NOTES.md`](LLM_NOTES.md)。

## 2. 選擇條件

每個 testcase 的 load prompt 先使用 `read`，最後的 output prompt 使用 `write`。所有其他
public query/edit 都要求同一 session 已有成功載入的 current design。不要因 prompt 同時要求
「load and report」就只執行 `read`；load 後仍需呼叫負責 report 的 query。

`help` 只用於確認 public grammar，不回答 circuit prompt。`quit`/`exit` 只在該 testcase
全部 prompt 與 final write 完成後使用。

## 3. Commands

```text
read <verilog_file>
write <verilog_file>
help
quit
exit
```

路徑可使用單引號或雙引號包含空白。`read` 或 `write` 會把 command 後剩餘整行視為路徑。

## 4. Read

成功 `read` 後：

- current design 替換為新設計。
- original snapshot 建立為同一份設計。
- previous-edit baseline 與 cached edit report 清空。
- `design_revision` 歸零。

主要 data：

| 欄位 | 用途 |
|---|---|
| `loaded_file` | 實際載入路徑 |
| `gate_count` | current active gate count |
| `net_count` | current active net count |
| `primary_input_count` | PI port 數量 |
| `primary_output_count` | PO port 數量 |
| `original_snapshot_available` | 是否已建立 original baseline |

讀取失敗時原本 session 不會被新 temporary design 取代。

## 5. Write

`write` 只輸出 current design。主要 data 為 `output_file` 與 `written`。尚未成功 `read` 時會回 `NO_DESIGN_LOADED`。

## 6. 輸出判讀

| Command | 成功條件 | 主要 data | 常見失敗 |
|---|---|---|---|
| `read` | `status:ok`, `complete:true`, `original_snapshot_available:true` | loaded file、active gate/net、PI/PO count | file/parser error；舊 session 保留 |
| `write` | `status:ok`, `complete:true`, `written:true` | `output_file` | `NO_DESIGN_LOADED` 或 writer error |
| `help` | `status:ok`, `complete:true` | public grammar text | 無 design 也可使用 |
| `quit`/`exit` | `status:ok`, `complete:true` | 無必要 data | session 結束後不可繼續 query |

`read` 回傳的 gate/net/PI/PO count 只用於確認載入與基本 size；需要 gate-type breakdown、
bus width 或 structural issue 時，必須再呼叫 `structure_query`。

## 7. Prompt Examples

```text
Prompt: Load the input design and report its basic size.
Commands:
  read input.v
  structure_query summary
```

```text
Prompt: Save the modified design as result.v.
Command:
  write result.v
```

## 8. 限制

`write` 不會自動執行 equivalence。題目要求功能不變時，先使用 `equiv_query` 確認，再輸出設計。

跨 prompt 的 hard constraint 由 LLM 從對話上下文維護，不會由 session 自動推論或保存。
若後續 prompt 說 `preserve previous constraints` 或 `maintain existing constraint`，應先查看
前題工具呼叫，並在新的 edit/optimization command 重新傳入限制；final write 前再以對應
query/report 驗證。
