# API 文件格式規範

本資料夾定義 `API_SPEC` 中正式高階 API 文件的命名、內容分工與檢查方式。新增或重寫 API 文件時，應先複製對應模板，再填入實際內容。

## 1. 檔名規則

每個正式高階 API 使用同一個 basename，固定建立一組文件：

```text
<FEATURE>_API.md
<FEATURE>_USAGE.md
```

例如：

```text
BASIC_QUERY_API.md
BASIC_QUERY_USAGE.md
PATH_QUERY_API.md
PATH_QUERY_USAGE.md
EDIT_APPLY_API.md
EDIT_APPLY_USAGE.md
```

不得使用下列混合形式：

```text
<FEATURE>_ANALYSIS.md
<FEATURE>_QUERY_USAGE.md（除非 FEATURE 本身就是 QUERY）
<FEATURE>_API_USAGE.md
```

## 2. 兩份文件的責任

| 文件 | 主要讀者 | 必須回答 |
|---|---|---|
| `_API.md` | API 維護者、底層實作者 | 為什麼存在、責任邊界、資料模型、型別、演算法、限制、實作位置 |
| `_USAGE.md` | tools/LLM 串接者、API 使用者 | 如何建立 request、輸入什麼、讀哪些 report 欄位、錯誤怎麼判讀、各 mode 範例 |

API 文件可以提到一個最小呼叫形式，但不應塞入所有逐 mode 範例。Usage 文件可以簡述責任，但不應重新推導底層演算法。

## 3. 交叉連結

兩份文件開頭必須互相連結：

```text
API_SPEC/<FEATURE>_USAGE.md
API_SPEC/<FEATURE>_API.md
```

若 API 已成為 legacy/internal engine，仍保留成對格式，但必須在兩份文件開頭明確標示公開替代入口。

## 4. 不需要成對的文件

下列文件不是單一 API contract，不強迫使用 `_API.md` / `_USAGE.md`：

```text
*_COVERAGE.md
*_FLOW.md
*_NOTES.md
*_INVENTORY.md
*_SUMMARY.md
*_GAPS.md
TOOLS_*_SPEC.md
PUBLIC_API_CONSOLIDATION.md
```

## 5. 完成檢查

```text
[ ] API 與 Usage basename 相同
[ ] H1 能清楚辨識 API 名稱與文件角色
[ ] API/Usage 開頭互相連結
[ ] API 文件包含定位、型別/入口、report、邊界、限制、實作狀態
[ ] Usage 文件包含基本流程、mode、input、output、範例、錯誤語意、prompt routing
[ ] 公開 CLI 名稱與 tools help/schema 一致
[ ] legacy 入口不會被寫成新的公開用法
[ ] 文件中的欄位名稱已和 header/source 核對
[ ] 修改後執行文件連結檢查與相關 regression test
```

模板：

```text
API_SPEC/DOCUMENTATION_FORMAT/API_DOCUMENT_TEMPLATE.md
API_SPEC/DOCUMENTATION_FORMAT/USAGE_DOCUMENT_TEMPLATE.md
```
