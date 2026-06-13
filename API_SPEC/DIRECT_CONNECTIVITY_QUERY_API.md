# Direct Connectivity Query API 整理

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
```

---

### 3.3 Gate Inputs / Output

```cpp
std::vector<int> getGateInputNetIds(const std::string& gateInstName) const;
std::vector<std::string> getGateInputNetNames(const std::string& gateInstName) const;
std::string getGateOutputNetName(const std::string& gateInstName) const;
```

語意：

```text
取得 gate 直接連接的 input nets / output net。
若 gate input 有 -1 disconnected slot，會被略過。
```

---

### 3.4 Gate Immediate Fanin / Fanout

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
```

---

### 3.5 Direct Connection Check

```cpp
bool isGateDirectlyConnectedToNet(const std::string& gateInstName,
                                  const std::string& netName) const;

bool isNetDirectlyConnectedToGate(const std::string& netName,
                                  const std::string& gateInstName) const;
```

語意：

```text
若 net 是 gate 的任一 input 或 output，回傳 true。
兩個 API 語意相同，只是參數順序不同。
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
| `NetDriver` | 查某個 net / bus 的直接 driver gate |
| `NetLoads` | 查某個 net / bus 直接 load 到哪些 gates |
| `GateInputs` | 查某個 gate 的直接 input nets |
| `GateOutput` | 查某個 gate 的 output net |
| `GateFanin` | 查直接驅動某 gate inputs 的上一層 gates |
| `GateFanout` | 查某 gate output 直接 fanout 到哪些 gates |
| `DirectlyConnected` | 判斷指定 gate 與指定 net 是否直接相連 |

核心資料結構：

```cpp
struct DirectConnectivityQuery {
    DirectConnectivityQueryType type;
    std::string gateName;
    std::string netName;
    bool includeIds = true;
    bool includeNames = true;
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

    std::vector<int> gateIds;
    std::vector<int> netIds;
    std::vector<std::string> gateNames;
    std::vector<std::string> netNames;
};
```

---

## 5. Prompt 對應方向

| Prompt 類型 | 高階 query |
|---|---|
| Which gate drives net n1? | `NetDriver` |
| Report every gate connected to net n1. | `NetLoads` |
| Report every gate connected to the output of g0. | `GateFanout` |
| What are the input nets of g1? | `GateInputs` |
| What is the output net of g1? | `GateOutput` |
| Which gates feed g1? | `GateFanin` |
| Is gate g1 directly connected to net n2? | `DirectlyConnected` |

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
Gate input/output helper
Gate immediate fanin/fanout helper
Direct connection check
DirectConnectivityQuery / DirectConnectivityReport 高階 API
```

測試狀態：

```text
mini test/tester.cpp 已覆蓋 runDirectConnectivityQuery() 的 NetDriver / NetLoads / GateInputs / GateFanout / DirectlyConnected。
目前 regression 結果：Summary: 45 passed, 0 failed.
```

後續可補：

```text
1. 增加 pin-level report，例如 gate input pin index/name 與 net 的對應。
2. 增加 whole-design fanout constraint report，例如列出 fanout > N 的 nets。
3. 增加 clock/reset-aware direct connectivity classification。
```
