# <Feature Name> API 整理

這份文件整理 `<C++ API>` 的設計、責任與內部 contract。

使用方式與 prompt 範例請看：

```text
API_SPEC/<FEATURE>_USAGE.md
```

---

## 1. 核心定位

說明 API 負責回答什麼，以及明確不負責什麼。

## 2. 資料模型與邊界

說明 graph、timing、sequential、SAT 或 mutation boundary。

## 3. Query / Request 型別

列出 enum、request struct 與各 mode 的必要輸入。

## 4. Report Contract

列出共用狀態、主要結果、完整性與錯誤欄位，說明 false、unknown、partial、timeout 的差別。

## 5. 高階入口與執行流程

列出公開 C++ entry point 與主要 dispatch/algorithm 流程，不放大量逐題使用範例。

## 6. 與其他 API 的責任界線

用表格說明容易混淆的問題應交給哪個 API。

## 7. 限制與安全規則

說明 unsupported case、規模限制、時間限制、等價性或 completeness 規則。

## 8. 實作與測試狀態

列出 header、source、對應 regression test 與尚未完成事項。
