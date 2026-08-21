# tools.cpp / TOOLS_SPEC 待更新表

本文件只記錄尚未完成的 `tools.cpp` parser/help/envelope printer 或 `TOOLS_SPEC/*.md`
同步事項。API facade、Netlist/engine、optimizer core 與底層 regression 請放在
`API_CORE_BACKEND_TODO.md`。

## 已確認待同步

目前沒有已確認但尚未同步的 path artifact 項目。`LITERAL_PATH_V1` 已同步至
`tools.cpp` 對應 backend、`TOOLS_SPEC/PATH_QUERY_TOOL.md` 與 Path API 文件。

## 完整輸出待盤查

下列功能目前能計算完整結果，但大型 detail 仍可能直接進 terminal。先確認 hidden prompt
規模與既有 generic artifact 能否重用，再決定 parser/schema；未確認前不直接增加 LLM 參數。

| 優先級 | Tool / mode | 需確認事項 |
|---:|---|---|
| Deferred / team-owned | `edit_apply` / `report_query last_edit` | changed names、merge records 與 validation messages 的大型輸出；等待其他成員合併 |
| Deferred / team-owned | `equiv_query` | matched/skipped boundary 與 mismatch detail 的大型輸出；等待其他成員合併 |

工具內部的輸出門檻只能決定 terminal 或 artifact，不得限制實際計算筆數。artifact 模式的
response 必須包含 count、complete 與 `output_file`。

## 等待 API 完成後同步

目前沒有等待 Cone Query API 完成後再同步的項目；TOOL-002 已完成 CLI grammar、help、
printer、artifact 與 Tool 文件同步。

## 後期 Boolean 重構

`func_query boolean_expression` 的 stack-safe large-output/artifact policy 等 Boolean backend
契約確定後再同步。此表不預先設計另一套 CLI schema。

## 不放在本表

- 已完成的 tools regression 或歷史同步紀錄。
- optimizer runtime、SAT/AIG engine、sequential matcher 與 rollback transaction。
- LLM 自然語言 routing、答案猜測或跨 prompt constraint memory。
