# Blup API 盤查區

本資料夾用於盤查下列三個高階 API 的底層實作、功能涵蓋、效能與 testcase 實測結果：

1. Path Query API
2. Function Search API
3. Sequential Pattern Query API

合併階段也會依序盤查其他成員的 API；目前新增：

4. Basic Query API

## 工作原則

- 優先維持既有 public API 與 CLI 參數，不因內部修正任意改動輸入格式。
- 修改程式碼前，先記錄問題、重現方式、影響範圍與建議方案，並與使用者討論。
- 每項修正都必須使用官方 `testcase` 內容驗證；必要時可增加 mini test，但不能只靠 synthetic case。
- Boolean function、SAT、cofactor、functional decomposition 等問題先標記為 `Boolean-Deferred`，留待後續 AIG functional index 重構處理。
- 問題與修改統一記錄於對應 API 的 `*_ISSUES_AND_CHANGES.md`。

## 目前階段

Path Query、Function Search 與 Sequential Pattern Query 的本階段盤查均已完成。總結請先看：

- `THREE_API_FINAL_COVERAGE.md`：三個 API 的責任邊界、實測 coverage 與剩餘風險。
- `PATH_QUERY_ISSUES_AND_CHANGES.md`：Path Query 問題、修正與效能證據。
- `FUNCTION_SEARCH_ISSUES_AND_CHANGES.md`：Function Search 問題、修正與測試證據。
- `SEQUENTIAL_PATTERN_ISSUES_AND_CHANGES.md`：Sequential Pattern 問題、修正與測試證據。
- `BASIC_QUERY_ISSUES_AND_CHANGES.md`：Basic Query merge review、問題與 regression。
- `OFFICIAL_QUESTIONS.md`：尚待官方回答的精確 path count 上限。

目前只保留兩類後續工作：已明確標記的 CLI/數值規格問題，以及後期以 AIG functional index
統一處理的 Boolean 分析重構。新的問題仍需先記錄、討論，再修改正式程式碼。
