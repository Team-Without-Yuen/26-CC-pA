# QA High-Level API Compliance Issues

這份文件整理目前高階 API 和最新版 ICCAD Problem A Q&A 之間的落差。

目的不是重新描述所有 API，而是建立一份可以逐項修正的問題表：

- 哪些部分已經符合 QA
- 哪些部分只部分符合 QA
- 哪些部分會導致 testcase prompt 無法完整回答
- 修正時應該優先處理哪些項目

---

## 1. 目前整體狀態

目前已完成的高階 API 主幹：

| 高階 API | 主要用途 | 目前狀態 |
|---|---|---|
| `BasicQuery` | gate / net / PI / PO / DFF 基本資訊查詢 | 大致可用 |
| `DirectConnectivityQuery` | driver / load / immediate fanin / fanout | 部分符合 QA |
| `ConeQuery` | transitive fanin / fanout cone | 大致符合 QA |
| `PathQuery` | startpoint-to-endpoint path analysis | 部分符合 QA |
| `DepthQuery` | logic depth / critical path / DFF.D endpoint depth | 大致符合 QA |
| `FunctionQuery` | SAT-based equivalence / constant status | 部分符合 QA |
| transformation / optimization reports | 修改、最佳化、驗證結果整理 | 尚未形成統一高階 API |

目前 tester 狀態：

```text
57 passed, 0 failed
```

代表現有 mini test 覆蓋的功能正常，但不代表已完整覆蓋新版 QA 的所有語意要求。

---

## 2. QA Compliance 問題表

| ID | 問題 | 涉及 API / 模組 | QA 要求 | 目前狀態 | 風險 | 優先度 | 建議修正 |
|---|---|---|---|---|---|---|---|
| QAC-001 | fanout load 定義不完整 | `DirectConnectivityQuery`, fanout optimization, constraint checker | fanout load 要包含 primitive gate input、DFF D、DFF CK、DFF RN/SN、primary output connection | 已新增 `FanoutLoadReport` 與 `GlobalFanoutReport`，可分類單一 net loads、查全域 max fanout、PI max fanout、fanout limit violations | fanout buffer insertion / optimization 若仍直接看 `loadGateIds.size()`，可能少算 PO load | P0 | 下一步把 fanout buffer insertion / optimization 改成依賴 `FanoutLoadReport` |
| QAC-002 | complete path enumeration 缺寫檔模式 | `PathQuery::EnumerateAll`, CLI tools | 若要求列出所有 paths，必須 literal listing；大型結果可寫到 file 並回報 path | `EnumerateAll` 預設自動寫檔，已新增 `outputFilePath` / `pathCount`，CLI 支援 `path_query enumerate ... -out file` 覆蓋檔名；目前仍保留 in-memory enumerate | 大型 testcase path 數量極端爆炸時仍可能需要 streaming DFS，但測試環境 128GB RAM 下第一版可接受 | P0 | 後續若實測 memory 壓力過大，再新增 streaming callback 版 enumerate |
| QAC-003 | 缺 original vs current design equivalence | `FunctionQuery`, transformation validation | transformation 後要能驗證 current design 和 original loaded netlist 功能等價 | 目前只能比較同一份 netlist 內兩個 net / bus | 修改後無法正式回答「是否和 original equivalent」 | P0 | 建立 original snapshot，新增 whole-design combinational equivalence report |
| QAC-004 | DFF initial state = 0 尚未建模 | `FunctionQuery`, constant analysis | QA 說 DFF initial state is 0，X ignored | 目前 DFF.Q 在 combinational analysis 中視為 pseudo PI | 若 prompt 涉及初始狀態，always 0/1 可能回答不完整 | P1 | 先在文件標註目前只做 combinational function；若要支援 initial-state query，新增 sequential-initial query mode |
| QAC-005 | 缺 DFF clock/reset fanout classification 高階 API | `DirectConnectivityQuery`, DFF helpers | CK / RN / SN 都可能是 fanout 類問題的一部分 | 底層可查 DFF named pin，但沒有直接 report 某 net 驅動哪些 CK/RN/SN | clock/reset fanout prompt 需要手動組合，容易漏 | P1 | 在 fanout report 中加入 DFF pin role 分類 |
| QAC-006 | 缺 all register-to-register path 支援 | `PathQuery`, `DepthQuery` | register-to-register path 可由 DFF.Q 到 DFF.D 表示 | `PathQuery` 已支援 `all_dff_q` / `all_dff_d` endpoint，可查 all DFF.Q -> all DFF.D 的 exists、find_any、enumerate、min/max depth；`RegisterPathQuery` 僅保留為便利 wrapper | 目前仍沿用 in-memory enumerate；極大量 path 時未做 streaming | P1 | 後續若遇到 path 數爆炸，再補 streaming / count-only mode |
| QAC-007 | BasicQuery 對被刪除 gate 的狀態可能不穩 | `BasicQuery`, cleanup / simplification | transformation 後 count 應反映有效 design | 某些 API 可能直接看 `gates.size()`，若有 `GateType::UNKNOWN` tombstone 可能被算入 | cleanup 後 gate count / list gates 可能錯 | P1 | 定義 active gate 規則；BasicQuery count/list 預設排除 `UNKNOWN` |
| QAC-008 | 缺統一 transformation result report | transformation / optimization | transformation prompt 通常需要回報 before/after、是否達標、是否 equivalent | 現在有零散 report，但沒有統一高階格式 | 不同 transformation 回答格式不一致，LLM 容易漏檢查 | P1 | 建立 `NetlistEditReport` 或統一 `TransformationQueryResult` |
| QAC-009 | function query 缺 Boolean equation export | `FunctionQuery` | testcase 有可能要求 derive Boolean equation | 目前有 SAT/equivalence/constant status，但沒有輸出 symbolic equation | Boolean expression 類 prompt 無法直接回答 | P2 | 新增 bounded cone expression builder，DFF.Q/PI/constant 當 leaf |
| QAC-010 | 缺 symmetry / cut / articulation 類高階 query | analysis API | testcase prompt 可能問 symmetry、cut point、關鍵節點 | 目前 path/cone 可組合部分答案，但沒有專用 API | 特殊 analysis prompt 覆蓋不足 | P2 | 後續新增 graph analysis API：dominator / articulation / cut / symmetry |
| QAC-011 | path query 缺 count-only safe mode | `PathQuery` | 有些 prompt 可能只問 path count | 目前 count 只能 enumerate 後 `paths.size()` | 大型 path count 會爆 | P2 | 新增 DP-based count 或 bounded count，並偵測 combinational DAG / loop guard |
| QAC-012 | CLI / tools 是否完整包裝所有高階 API 尚未確認 | `tools.cpp`, `tools_high.cpp` | 最終系統要能由自然語言映射到 engine command | 有些 API 已有 CLI，有些仍可能只在 C++ API | LLM agent 可能無法直接呼叫某些功能 | P2 | 對照 `TOOLS_CLI_USAGE.md` 與實際 tools command，補缺口 |

---

## 3. 建議修正順序

### Step 1：先修 fanout load 定義

原因：

- 新版 QA 明確提到 fanout load 包含 primary output connection。
- fanout query、fanout constraint、fanout buffer insertion 都會用到。
- 這是底層語意問題，應優先統一。

目標：

- 已新增一個統一 fanout load report。
- 不再讓各個 API 自己解讀 `loadGateIds`。
- 明確把 PO load 和 DFF named pins 納入 report。

目前第一階段完成內容：

- 新增 `FanoutLoadReport`。
- 新增 `getFanoutLoadReport(int netId)`。
- 新增 `getFanoutLoadReport(const std::string& netName)`。
- 新增 `getFanoutLoadCount(const std::string& netName)`。
- 新增 `getGlobalFanoutReport(...)`。
- 新增 `satisfiesFanoutLimit(int maxFanoutLimit)`。
- 新增 `DirectConnectivityQueryType::FanoutLoadReport`。
- 新增 `DirectConnectivityQueryType::GlobalFanoutReport`。
- 新增 CLI command：`conn_query fanout_load <net>` / `conn_query fanout_report <net>`。
- 新增 CLI command：`conn_query global_fanout [limit]` / `conn_query pi_fanout [limit]` / `conn_query fanout_violations <limit>`。
- tester 已覆蓋：
  - primitive gate input load
  - DFF `.D` load
  - DFF `.CK` load
  - DFF `.RN/.SN` 分別計為兩個 reset/set loads
  - primary output connection load
  - global max fanout
  - primary-input max fanout
  - fanout limit violation
  - fanout buffer insertion respecting primary output load
  - fanout buffer insertion respecting DFF RN/SN sink pins

後續仍需接上的部分：

- 其他尚未改用 pin-level sink 的非 fanout-limit transformation / optimization pass

---

### Step 2：修 PathQuery complete enumeration

原因：

- QA 明確說 complete enumeration 必須列出每一條 path。
- 大型 testcase 可能有數十萬條 paths。
- 目前全部放 memory 不夠安全。

目標：

- `EnumerateAll` 預設自動寫檔。
- result 回傳：
  - `pathCount`
  - `outputFilePath`
  - 是否截斷或完整輸出

---

### Step 3：補 original vs current equivalence

原因：

- transformation / optimization prompt 會要求驗證修改後與原始設計等價。
- 目前 `FunctionQuery` 只能查同一份 netlist 的 net equivalence。

目標：

- 保存 original netlist snapshot。
- 對所有 PO 與 DFF.D endpoint 做 combinational equivalence。
- 產生統一 equivalence report。

---

### Step 4：補 DFF clock/reset classification

原因：

- QA 指出 DFF clock / reset / set pin 都算 fanout load。
- 後面 fanout buffer tree、clock/reset query 都會用到。

目標：

- 能回答：
  - 哪些 DFF 的 CK 由 net n0 驅動？
  - 哪些 DFF 的 RN/SN 由 net n1 驅動？
  - 某 net 的 fanout load 分類統計是多少？

---

### Step 5：整理 transformation 統一 report

原因：

- cleanup、simplification、buffer insertion、mapping、optimization 都需要類似報告。
- 如果沒有統一 report，LLM 很容易只執行修改，忘記驗證。

目標：

- 統一 report 欄位：
  - before / after gate count
  - before / after depth
  - fanout violation count
  - changed gates / nets
  - equivalence check result
  - accepted / rejected / rollback reason

---

## 4. 已符合 QA 的重要規則

以下部分目前方向正確，修正時要保留：

| 規則 | 目前狀態 |
|---|---|
| DFF 是 primitive gate | parser 已支援 |
| DFF 使用 named port `.RN/.SN/.CK/.D/.Q` | parser 已支援 |
| `.SN` 是 active-low set | 文件已記錄 |
| DFF 是 sequential boundary | path / cone / depth / function analysis 已採用 |
| DFF.Q 視為 pseudo PI | depth / function 已採用 |
| DFF.D 可作為 timing endpoint | depth / path 已支援 |
| DFF input pin order 不能因 disconnect 被 erase | transformation 目前已有保留 `-1` 的設計 |
| no RTL / flattened gate-level netlist | parser 與文件方向正確 |
| functional constant 需要 SAT/logic reasoning | `FunctionQuery` 已有 SAT-based constant check |

---

## 5. 下一個實作目標

建議下一步從 `QAC-001 fanout load 定義不完整` 開始。

這一項會影響：

- DirectConnectivityQuery
- fanout count
- maximum fanout prompt
- fanout buffer insertion
- clock/reset/set load classification
- transformation constraint validation

第一版可以先新增 read-only API，不急著改 transformation：

```cpp
struct FanoutLoadReport {
    int netId = -1;
    std::string netName;

    std::vector<int> combinationalGateLoads;
    std::vector<int> dffDataLoads;
    std::vector<int> dffClockLoads;
    std::vector<int> dffResetSetLoads;
    bool drivesPrimaryOutput = false;

    int totalLoadCount = 0;
};
```

然後所有 fanout 類 query 都改成依賴這份 report。
