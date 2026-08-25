# Direct Connectivity Query API 整理

> 公開 CLI 統一由 `structure_query` 提供；`DirectConnectivityQuery` 保留為內部 C++ 的一層連線查詢 API。舊 `conn_query` 命令僅供相容，不列入新的公開 tool schema。

這份文件整理 `Netlist` 的直接連線查詢 API，也就是 Basic Query 和 Cone / Path Query 中間的一層。

使用範例與 prompt 對應請看：

```text
API_SPEC/DIRECT_CONNECTIVITY_QUERY_USAGE.md
```

---

## 1. 核心定位

Direct Connectivity Query 負責回答「gate 和 net 之間是否直接相連」。

典型問題：

```text
哪個 gate drive net n1？
net n1 直接 load 到哪些 gates？
gate g1 的 input nets 是哪些？
gate g1 的 output net 是什麼？
哪些 gates 直接餵進 gate g1？
gate g1 的 output 直接 fanout 到哪些 gates？
net n1 依照 Problem A QA 定義有多少 fanout loads？
net clk drive 哪些 DFF clock pins？
gate g1 是否直接連到 net n2？
```

這一層不做：

```text
transitive fanin / fanout cone traversal
startpoint-to-endpoint path search
critical path / depth analysis
optimization rewrite
```

---

## 2. 圖模型

目前 netlist graph 以 `Net -> Gate -> Net` 表示。

直接連線關係：

```text
Net.driverGateId  -> 直接驅動這條 net 的 gate
Net.loadGateIds   -> 直接使用這條 net 當 input 的 gates
Gate.inputNetIds  -> gate 直接讀取的 input nets
Gate.outputNetId  -> gate 直接驅動的 output net
```

Active object 規則：

```text
Direct Connectivity Query 只回報 active gate / active net。
已移除 gate（GateType::UNKNOWN）與已移除 net（isRemoved=true）視為不存在。
helper 不只檢查 ID 範圍，也會檢查 cached driver/load 是否與 gate 端 output/input pin 雙向一致。
```

這個規則的原因：

```text
cleanup / optimization 會使用 tombstone 保留 ID 穩定性。
如果 query 只看 vector index 或 net.loadGateIds / net.driverGateId cache，可能把已刪除或 stale edge 誤報成有效直接連線。
```

Immediate fanin / fanout 定義：

```text
Gate immediate fanin  = 驅動該 gate input nets 的上一層 gates
Gate immediate fanout = 該 gate output net 直接 load 到的下一層 gates
```

注意：

```text
這裡的 fanin/fanout 都是 immediate，不是 transitive cone。
若要跨多層找 reachable gates，應使用 Cone Query。
```

Problem A QA fanout load 定義：

```text
fanout load =
  primitive gate input pin
+ DFF D pin
+ DFF CK pin
+ DFF RN/SN pin
+ primary output connection
```

因此：

```text
NetLoadGates / GateFanout 只回答 gate instance adjacency。
FanoutLoadReport 回答 QA 定義的 pin-level fanout load。
```

同一顆 gate 多個 input pin 接同一條 net 時：

```text
NetLoadGates / GateFanout 只列一次 gate instance。
FanoutLoadReport 依 pin-level load 計數，會把每個 input pin 分別計入。
```

---

## 3. 低階 Helper

### 3.1 Net Driver

```cpp
int getNetDriverGateId(const std::string& netName) const;
std::vector<int> getNetDriverGateIds(const std::string& netName) const;
std::string getNetDriverGateName(const std::string& netName) const;
std::vector<std::string> getNetDriverGateNames(const std::string& netName) const;
```

語意：

```text
getNetDriverGateId() 適合 scalar net，沒有 driver 時回傳 -1。
getNetDriverGateIds() 支援 bus，會展開 bit nets 並去重。
removed scalar net 視為不存在；bus 只有至少一個 active bit net 時才存在。
driver 必須同時滿足 net.driverGateId == gateId 且 gate.outputNetId == netId。
```

---

### 3.2 Net Loads

```cpp
std::vector<int> getNetLoadGateIds(const std::string& netName) const;
std::vector<std::string> getNetLoadGateNames(const std::string& netName) const;
size_t getNetLoadGateCount(const std::string& netName) const;
```

語意：

```text
取得指定 net / bus 直接 load 到的 gates。
這組 API 是 getWireLoads() / getWireLoadNames() / getWireLoadCount() 的 Net 命名 wrapper。
removed scalar net 視為不存在；bus 只回報 active bit nets 的 loads。
load gate 必須實際在 inputNetIds 中使用該 net，stale loadGateIds cache 不會被回報。
```

---

### 3.3 Problem A Fanout Load Report

```cpp
FanoutLoadReport getFanoutLoadReport(int netId) const;
FanoutLoadReport getFanoutLoadReport(const std::string& netName) const;
size_t getFanoutLoadCount(const std::string& netName) const;
```

語意：

```text
依照 Problem A QA 的 fanout load 定義，回報 net 的 pin-level loads。
它會把 primary output connection 算入，也會把 DFF input 依 named pin 分類。
```

| 欄位 | 意思 |
|---|---|
| `combinationalGateLoads` | primitive gate input pin loads |
| `dffDataLoads` | DFF `.D` pin loads |
| `dffClockLoads` | DFF `.CK` pin loads |
| `dffResetSetLoads` | DFF `.RN` / `.SN` pin loads |
| `dffOtherLoads` | 其他 DFF input pin loads |
| `allGateLoadIds` | 所有 gate/DFF pin loads；同一 gate 多 pin 時可重複 |
| `distinctGateLoadIds` | 直接 load gate instances，依首次 pin 出現順序去重 |
| `combinationalGateLoadCount` | primitive gate input pin load 數；大型 list artifact 化後仍可直接讀取 |
| `dffDataLoadCount` | DFF `.D` pin load 數 |
| `dffClockLoadCount` | DFF `.CK` pin load 數 |
| `dffResetSetLoadCount` | DFF `.RN` / `.SN` pin load 數 |
| `dffOtherLoadCount` | 其他 DFF input pin load 數 |
| `distinctGateLoadCount` | 不重複的直接 load gate instance 數 |
| `drivesPrimaryOutput` | 是否直接連到 primary output |
| `primaryOutputLoadCount` | primary output connection 數 |
| `totalLoadCount` | 符合 QA 定義的總 fanout load 數 |

注意：

```text
同一顆 DFF 如果 .RN(net) 和 .SN(net) 都接同一條 net，會算成兩個 sink pin loads。
五個 category count 是對應 vector 的完整 `size()`，不受 terminal list 或 artifact 切換影響；
`totalLoadCount` 等於五個 category counts 加上 `primaryOutputLoadCount`。
`distinctGateLoadCount` 與 `distinctGateLoadIds` 不包含 PO connection；它們回答直接被此 net
驅動的 gate instances，並不改變 `totalLoadCount` 的 pin-level QA 語意。
```

---

### 3.4 Gate Inputs / Output

```cpp
std::vector<int> getGateInputNetIds(const std::string& gateInstName) const;
std::vector<std::string> getGateInputNetNames(const std::string& gateInstName) const;
std::string getGateOutputNetName(const std::string& gateInstName) const;
```

語意：

```text
取得 gate 直接連接的 input nets / output net。
若 gate input 有 -1 disconnected slot，會被略過。
GateOutput 對 active gate 若沒有有效且雙向一致的 active output net，會成功回報 gate 存在，
但 `count=0`、`netId=-1`，且不填入 `netName` / `netIds` / `netNames` payload。
```

---

### 3.5 Gate Immediate Fanin / Fanout

```cpp
std::vector<int> getGateFaninGateIds(const std::string& gateInstName) const;
std::vector<std::string> getGateFaninGateNames(const std::string& gateInstName) const;
size_t getGateFaninGateCount(const std::string& gateInstName) const;

std::vector<int> getGateFanout(const std::string& gateInstName) const;
std::vector<std::string> getGateFanoutNames(const std::string& gateInstName) const;
size_t getGateFanoutCount(const std::string& gateInstName) const;
```

語意：

```text
GateFanin 只看直接驅動 gate input nets 的上一層 gates。
GateFanout 只看 gate output net 直接 load 到的下一層 gates。
兩者都只回報雙向一致的 active edges。
```

---

### 3.6 Direct Connection Check

```cpp
bool isGateDirectlyConnectedToNet(const std::string& gateInstName,
                                  const std::string& netName) const;

bool isNetDirectlyConnectedToGate(const std::string& netName,
                                  const std::string& gateInstName) const;
```

語意：

```text
若 scalar net 是 gate 的任一 input 或 output，回傳 true。
若 `netName` 是 bus base name，會展開所有 active bits；任一 bit 與 gate 直接相連即回傳 true。
兩個 API 語意相同，只是參數順序不同。
removed gate/net 會回 false；高階 DirectlyConnected report 則回 ok=false、exists=false。
```

---

## 4. 高階 DirectConnectivityQuery

目前已提供統一高階入口：

```cpp
DirectConnectivityReport runDirectConnectivityQuery(
    const DirectConnectivityQuery& query) const;
```

支援的 query type：

| Query type | 用途 |
|---|---|
| `NetDriverGates` | 查某個 net / bus 的直接 driver gates |
| `NetLoadGates` | 查某個 net / bus 直接 load 到哪些 gates |
| `FanoutLoadReport` | 依 Problem A QA fanout load 定義回報 pin-level loads |
| `GlobalFanoutReport` | 掃描全設計或所有 PI 的 fanout loads / max / violations / predicate matches |
| `FanoutRankingReport` | 依 all/PI/PO/internal/gate-output scope 做 distinct fanout ranking，完整保留 ties |
| `GateInputs` | 查某個 gate 的直接 input nets |
| `GateOutput` | 查某個 gate 的 output net |
| `GateFanin` | 查直接驅動某 gate inputs 的上一層 gates |
| `GateFanout` | 查某 gate output 直接 fanout 到哪些 gates |
| `DirectlyConnected` | 判斷指定 gate 與 scalar net / bus 的 active bits 是否直接相連 |

核心資料結構：

```cpp
struct DirectConnectivityQuery {
    DirectConnectivityQueryType type;
    std::string gateName;
    std::string netName;
    int fanoutLimit = -1;
    bool primaryInputsOnly = false;
    bool includeZeroFanout = false;
    FanoutPredicate fanoutPredicate = FanoutPredicate::None;
    size_t fanoutValue = 0;
    size_t fanoutUpperValue = 0;
    FanoutScope fanoutScope = FanoutScope::All;
    FanoutRankMode fanoutRankMode = FanoutRankMode::Highest;
    size_t fanoutRankValue = 1;
    bool includeIds = true;
    bool includeNames = true;
    bool includePinDetails = false;
};

struct DirectConnectivityReport {
    bool ok = false;
    bool exists = false;
    bool connected = false;
    std::string message;

    std::string gateName;
    std::string netName;
    int gateId = -1;
    int netId = -1;
    size_t count = 0;
    bool pinDetailsIncluded = false;
    size_t pinConnectionCount = 0;

    std::vector<int> gateIds;
    std::vector<int> netIds;
    std::vector<std::string> gateNames;
    std::vector<std::string> netNames;
    std::vector<ConnectivityPinRecord> pinConnections;
};
```

`NetDriverGates` / `NetLoadGates` 設定 `includePinDetails=true` 時會填入逐 pin 的
`ConnectivityPinRecord`。每筆包含 scalar net ID/name、gate ID/name/type、pin index/name、
`Driver/Load` direction 與 `Output/CombinationalInput/DffData/DffClock/DffResetSet/DffOther` role。
`count` 維持去重 gate instance 數；`pinConnectionCount` 是實際 pin edge 數，兩者不可混用。

---

## 5. Prompt 對應方向

| Prompt 類型 | 高階 query |
|---|---|
| Which gate drives net n1? | `NetDriverGates` |
| Which exact output pin drives net n1? | `NetDriverGates` with `includePinDetails=true` |
| Report every gate connected to net n1. | `NetLoadGates` |
| Report every gate pin connected to net n1. | `NetLoadGates` with `includePinDetails=true` |
| How many fanout loads does net n1 have? | `FanoutLoadReport` |
| What is n1's fanout, and which gates does it directly drive? | `FanoutLoadReport`；同時讀 `totalLoadCount` 與 `distinctGateLoadIds` |
| List DFF clock/reset loads driven by n1. | `FanoutLoadReport` |
| Which primary input has the highest fanout? | `GlobalFanoutReport` with `primaryInputsOnly = true` |
| Does every signal satisfy max fanout 16? | `GlobalFanoutReport` with `fanoutLimit = 16` |
| How many PI nets have fanout exactly 4? | `GlobalFanoutReport` with `primaryInputsOnly = true`, `fanoutPredicate = Equal`, `fanoutValue = 4` |
| List nets whose fanout is between 4 and 8. | `GlobalFanoutReport` with `fanoutPredicate = BetweenInclusive`, bounds 4 and 8 |
| Which nets have the second-highest fanout? | `FanoutRankingReport` with `NthHighest`, rank 2 |
| List the top 5 PI fanout levels, retaining ties. | `FanoutRankingReport` with PI scope, `Top`, value 5 |
| Which DFF output nets have the lowest fanout? | `FanoutRankingReport` with DFF-output scope, `Lowest` |
| Report every gate connected to the output of g0. | `GateFanout` |
| What are the input nets of g1? | `GateInputs` |
| What is the output net of g1? | `GateOutput` |
| Which gates feed g1? | `GateFanin` |
| Is gate g1 directly connected to net n2? | `DirectlyConnected` |
| Is gate g1 connected to any bit of bus data? | `DirectlyConnected`；`count/netNames` 回實際相連 bits |

`GlobalFanoutReport` 的 extrema candidate 是 scope 內全部 active nets。`includeZeroFanout=false`
只會從完整 `netReports` 明細省略零值，不會在 maximum 比較前排除它們；因此 all-zero scope
仍會回 `maxFanout=0` 以及完整 `maxFanoutReports` ties。`checkedNetCount` 是實際參與
extrema/limit 計算的 candidate 數，不等於省略零值後的 `netReports.size()`。

`fanoutPredicate` 支援 `Equal/NotEqual/GreaterThan/GreaterOrEqual/LessThan/LessOrEqual/
BetweenInclusive`。篩選不改變上述 extrema、limit 或 `DirectConnectivityReport.count` 契約；
命中數與完整清單分別讀 `matchedNetCount`、`matchedReports`。`BetweenInclusive` 的上下界均包含。

`FanoutRankingReport` 的 rank 依 distinct fanout value 計算，第一名從 1 開始。同一 fanout 的
nets 屬同一 rank，依 net name/net ID 穩定排序且完整保留。`Top/Bottom K` 的 K 是 fanout
levels，不是強制切成 K 條 nets。scope 支援 `All/PrimaryInputs/PrimaryOutputs/Internal/
GateOutputs/CombinationalOutputs/DffOutputs`；gate-output scopes 僅接受 active 且 driver edge
雙向一致的 nets。rank 不存在或 scope 為空是成功的空結果，由 `requestedRankExists=false`
表示；removed tombstone 不參與。

---

## 6. 目前完成狀態

實作位置：

```text
include/core/NetlistQueries.h
src/analysis/ConnectivityAnalysis.cpp
```

已完成：

```text
Net driver helper
Net load helper
Problem A fanout load report helper
Gate input/output helper
Gate immediate fanin/fanout helper
Direct connection check
DirectConnectivityQuery / DirectConnectivityReport 高階 API
Fanout ranking scope / distinct-rank report
```

測試狀態：

```text
mini test/tester.cpp 已覆蓋 runDirectConnectivityQuery() 的 NetDriverGates / NetLoadGates / FanoutLoadReport / GlobalFanoutReport / GateInputs / GateFanout / DirectlyConnected。
mini test/test55 另覆蓋 DirectlyConnected、fanout predicate/ranking、全部 ranking scopes、ties、
rank 不存在、空 scope，以及所有公開 connectivity mode 的 strict parser；新增 ranking 部分待手動驗證。
```

逐 pin driver/load detail 已由 `ConnectivityPinRecord` 完成。其他非 fanout-limit transformation /
optimization pass 若有 fanout 判斷，仍應逐步改用 pin-level sink。
