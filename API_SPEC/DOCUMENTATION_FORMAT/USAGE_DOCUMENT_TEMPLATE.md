# <Feature Name> 使用說明

這份文件只負責說明 `<C++ API>` 與對應公開 command 的使用方式。

設計背景與內部 contract 請看：

```text
API_SPEC/<FEATURE>_API.md
```

---

## 1. 基本概念與入口

列出最小呼叫形式、公開 CLI，以及標準使用流程。

## 2. Mode / Command 總表

| Mode | 必要輸入 | 主要輸出 | 語意 |
|---|---|---|---|
| `<mode>` | `<field>` | `<report field>` | 說明 |

## 3. 輸入欄位

列出型別、預設值、必要條件與互斥規則。

## 4. 回傳欄位與狀態判讀

說明成功、明確 false、空集合、invalid input、partial、timeout 與 unsupported。

## 5. 各 Mode 使用範例

每個 mode 至少提供一個 C++ 或 CLI 範例，並指出應讀取的 report 欄位。

## 6. Prompt 對應

| Prompt 語意 | Mode / Command | 主要讀取 |
|---|---|---|
| `<prompt>` | `<mode>` | `<field>` |

## 7. 何時不要使用

列出相似問題應改用的其他高階 API。

## 8. 實作與測試狀態

列出目前可用範圍、相關測試與已知限制。
