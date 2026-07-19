# Session And I/O Tool

## 1. 責任

`read` 建立目前設計與 original snapshot；`write` 輸出 current design；`help` 顯示 public grammar；`quit`/`exit` 結束 session。所有 query、edit、report 與 equivalence command 都依賴同一個 session。

## 2. Commands

```text
read <verilog_file>
write <verilog_file>
help
quit
exit
```

路徑可使用單引號或雙引號包含空白。`read` 或 `write` 會把 command 後剩餘整行視為路徑。

## 3. Read

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

## 4. Write

`write` 只輸出 current design。主要 data 為 `output_file` 與 `written`。尚未成功 `read` 時會回 `NO_DESIGN_LOADED`。

## 5. Prompt Examples

```text
Prompt: Load testcase31 and report its basic size.
Commands:
  read NewTestCase/test31/test31.v
  structure_query summary
```

```text
Prompt: Save the modified design as result.v.
Command:
  write result.v
```

## 6. 限制

`write` 不會自動執行 equivalence。題目要求功能不變時，先使用 `equiv_query` 確認，再輸出設計。
