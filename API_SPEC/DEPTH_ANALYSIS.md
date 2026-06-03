# Depth Analysis API 設計整理

這份文件定義後續要新增的 `DepthAnalysis` API。  
目標是支援 Problem A 中 timing / critical path / depth optimization 類 prompt。

---

## 1. 背景與目標

題目與 sample prompt 可能出現這類需求：

```text
Reduce critical path depth.
Perform depth optimization.
Try to restructure n10 target depth 4.
For each output depth > 4 optimize cone.
What is the maximum logic depth from input in0 to output out3?
```

這類問題的第一步不是馬上改電路，而是先分析：

```text
某個 net / output / DFF.D 的目前 depth 是多少？
最長路徑 critical path 經過哪些 gates？
哪些 endpoints 超過 target depth？
要最佳化的 fanin cone 是哪一塊？
```

因此 `DepthAnalysis` 的目的：

```text
提供穩定、可重用的 depth / level / critical path 查詢 API，
作為後續 depth optimization / cone restructuring 的基礎。
```

---

## 2. Depth 定義

目前採用簡化的 unit-delay gate model：

```text
每個 combinational gate 的 delay / depth cost = 1
```

因此一條路徑的 depth：

```text
路徑中穿越的 combinational gate 數量。
```

例子：

```text
a -> g1 -> n1 -> g2 -> y
```

depth 為：

```text
2
```

因為經過：

```text
g1, g2
```

---

## 3. Boundary 規則

Depth analysis 預設只分析 combinational region。

起點 depth = 0 的節點：

```text
Primary Input
DFF.Q
constant 1'b0 / 1'b1
```

終點可以是：

```text
Primary Output
DFF.D
Specific Net
Gate Input
Gate Output
```

DFF 規則：

```text
DFF 是 sequential boundary。
不從 DFF input 穿越到 DFF Q。
```

如果要分析 register-to-register depth：

```text
startpoint = DFF.Q
endpoint   = DFF.D
```

---

## 4. Level 與 Depth 的差異

在這份文件中：

```text
net level  = 從最近的 combinational startpoint 到該 net 的最大 gate 數
path depth = 某一條 startpoint-to-endpoint path 的 gate 數
```

若有多條 fanin path 到同一個 net：

```text
net level = max(所有 fanin path depth)
```

例子：

```text
a -> g1 -> n1
b -> g2 -> n2 -> g3 -> n1
```

則：

```text
level(n1) = 2
```

---

## 5. 第一階段建議 API

第一階段只做分析，不做 transformation。

### 5.1 computeNetLevels

```cpp
std::vector<int> computeNetLevels() const;
```

用途：

```text
計算每個 net 的 combinational level。
```

回傳：

```text
vector index = netId
value        = net level
```

建議語意：

```text
PI / DFF.Q / constant net level = 0
無法計算或不在 combinational region 內 = -1
```

用途例子：

```text
快速查每條 net 的最大 fanin depth。
找出 depth 過大的 output。
為 critical path reconstruction 做準備。
```

---

### 5.2 computeGateLevels

```cpp
std::vector<int> computeGateLevels() const;
```

用途：

```text
計算每個 gate output 的 level。
```

回傳：

```text
vector index = gateId
value        = gate output level
```

建議語意：

```text
combinational gate output level = max(input net levels) + 1
DFF gate level = -1，因為 DFF 不屬於 combinational traversal
input net 無法計算時，gate level = -1
```

---

### 5.3 getMaxDepthToNet

```cpp
int getMaxDepthToNet(const std::string& netName) const;
```

用途：

```text
查某條 net 的最大 fanin depth。
```

回傳：

```text
成功：depth >= 0
失敗或找不到 net：-1
```

例子：

```text
getMaxDepthToNet("n10")
```

可回答：

```text
What is the logic depth of n10?
```

---

### 5.4 findCriticalPathToNet

```cpp
CombinationalPath findCriticalPathToNet(const std::string& netName) const;
```

用途：

```text
找出到指定 net 的一條最長 combinational path。
```

回傳：

```text
CombinationalPath
```

語意：

```text
若多條 path 同樣最長，回傳任意一條即可。
```

可回答：

```text
Report the critical path to n10.
```

---

### 5.5 analyzeDepthToNet

```cpp
DepthReport analyzeDepthToNet(const std::string& netName) const;
```

建議新增資料結構：

```cpp
enum class DepthEndpointType {
    Unknown,
    SpecificNet,
    PrimaryOutput,
    DffD
};

struct DepthReport {
    DepthEndpointType endpointType = DepthEndpointType::Unknown;
    std::string endpointName;
    int endpointNetId = -1;
    int depth = -1;
    CombinationalPath criticalPath;
};
```

欄位語意：

```text
endpointType  : endpoint 來源類型，例如 PrimaryOutput 或 DffD。
endpointName  : 給人看的名稱，例如 y、n10、ff1.D。
endpointNetId : 實際被分析的 net ID。
depth         : 到該 endpoint 的最大 combinational depth。
criticalPath  : 到該 endpoint 的一條 critical path。
```

用途：

```text
一次取得 endpoint net、depth、critical path。
```

適合給高階 API / LLM response 使用。

---

### 5.6 analyzePrimaryOutputDepths

```cpp
std::vector<DepthReport> analyzePrimaryOutputDepths() const;
```

用途：

```text
分析所有 primary outputs 的 depth。
```

可回答：

```text
Report all primary outputs whose logic cone contains depth greater than 4.
What is the maximum output depth in this design?
```

---

### 5.7 getPrimaryOutputsWithDepthGreaterThan

```cpp
std::vector<std::string> getPrimaryOutputsWithDepthGreaterThan(int limit) const;
```

用途：

```text
列出所有 depth > limit 的 primary output names。
```

可回答：

```text
For each output depth > 4, optimize cone.
```

這個 API 可以作為 optimization 前的 endpoint selector。

---

### 5.8 analyzeDffDDepths

```cpp
std::vector<DepthReport> analyzeDffDDepths() const;
```

用途：

```text
分析所有 DFF D-pin 的 combinational depth。
```

原因：

Timing path 常見 endpoint 不只有 PO，也包含 register D pin。

---

### 5.9 getDffsWithDDepthGreaterThan

```cpp
std::vector<std::string> getDffsWithDDepthGreaterThan(int limit) const;
```

用途：

```text
列出所有 D-pin depth > limit 的 DFF instance names。
```

可回答：

```text
Find registers whose D input has depth greater than 5.
```

---

## 6. 第二階段可加的 API

第二階段才開始跟 optimization 接軌。

### 6.1 findGlobalCriticalPath

```cpp
DepthReport findGlobalCriticalPath() const;
```

用途：

```text
在所有 PO 與 DFF.D endpoints 中找出最大 depth 的 critical path。
```

可回答：

```text
Reduce critical path depth.
```

如果 prompt 沒指定 target net，這個 API 可先找全設計最嚴重的路徑。

---

### 6.2 findEndpointsExceedingDepth

```cpp
std::vector<DepthReport> findEndpointsExceedingDepth(int maxDepth) const;
```

用途：

```text
找出所有 depth > maxDepth 的 endpoints。
```

endpoint 可包含：

```text
Primary Output
DFF.D
```

---

### 6.3 getFaninConeDepthReport

```cpp
DepthReport getFaninConeDepthReport(const std::string& targetNet) const;
```

用途：

```text
針對某個 target net 的 fanin cone 建立 depth report。
```

可接到：

```text
optimize cone
restructure target net
```

---

## 7. 和 PathQuery 的關係

`PathQuery` 已經可以做：

```text
startpoint-to-endpoint MinDepth / MaxDepth
```

但 `DepthAnalysis` 的定位不同：

```text
PathQuery:
  使用者已指定 startpoint / endpoint。

DepthAnalysis:
  幫助系統自動分析整個設計或某個 cone 哪裡太深。
```

例如：

```text
What is the max depth from input a to output y?
```

適合：

```text
PathQuery MaxDepth
```

但：

```text
For each output depth > 4 optimize cone.
```

適合：

```text
DepthAnalysis 找出 outputs
再交給 Optimization API
```

---

## 8. 和 Optimization 的關係

Depth optimization 的高階流程應該是：

```text
1. DepthAnalysis 找出超標 endpoint
2. 取得 endpoint fanin cone
3. 找出 critical path
4. 套用保守 rewrite / balance
5. 重新計算 depth
6. 用 LEC 或 SAT 檢查功能等價
7. 若不符合 hard constraint 或功能錯誤，rollback
```

因此 DepthAnalysis 是 optimization 的前置條件。

---

## 9. 後續 Optimization 可能用到的 Transformation

第一批保守 transformation：

```text
buffer chain cleanup
inverter-buffer merge
constant propagation
duplicate gate sharing
simple algebraic simplification
```

第二批 depth-oriented transformation：

```text
AND tree balancing
OR tree balancing
XOR tree balancing
critical path local rewriting
fanin cone restructuring
```

注意：

```text
NAND / NOR / XNOR 有 inversion polarity 問題，不應直接當 AND / OR / XOR 任意平衡。
```

---

## 10. 實作注意事項

### 10.1 DFF 不穿越

DepthAnalysis 應與 PathQuery 一致：

```text
DFF 是 sequential boundary。
```

DFF.Q 可當 startpoint，DFF.D 可當 endpoint。

---

### 10.2 unconnected net / pin

官方 Q&A 說 netlist 可能有 floating 或 unconnected ports。

因此：

```text
遇到 -1 netId 要跳過。
不要呼叫 getNet(-1)。
```

---

### 10.3 combinational loop

理論上 gate-level netlist 應多為 combinational DAG + DFF boundary。

但仍建議保護：

```text
若遇到 combinational loop，level 計算應回報 failure 或跳過。
```

可用：

```text
topological sort
DFS inStack cycle detection
```

目前狀態：

```text
findLongestPathInCone() 已加入 DFS visiting/inStack 保護。
若 cone 分支遇到 cycle，該分支視為無效。
若整個 cone 只有 cycle 而沒有合法出口，回傳 {-1, empty path}。
```

---

### 10.4 多 fanin reconvergence

Depth 取最大 fanin depth：

```text
gate output level = max(input levels) + 1
```

這是 timing critical path 常用的簡化模型。

---

## 11. 建議實作順序

目前狀態：

```text
第一階段 API 已先在 include/core/Netlist.h 中完成宣告。
computeNetLevels() / computeGateLevels() / getMaxDepthToNet() 已在
src/analysis/DepthAnalysis.cpp 完成實作與 mini tester 驗證。
findCriticalPathToNet() / analyzeDepthToNet() 也已完成實作與 mini tester 驗證。
analyzePrimaryOutputDepths() / getPrimaryOutputsWithDepthGreaterThan() 已完成實作與 mini tester 驗證。
analyzeDffDDepths() / getDffsWithDDepthGreaterThan() 已完成實作與 mini tester 驗證。
findGlobalCriticalPath() / findEndpointsExceedingDepth() 已完成實作與 mini tester 驗證。
後續編譯或連結測試時，要記得把 src/analysis/DepthAnalysis.cpp 加入編譯命令。
```

第一步：

```cpp
std::vector<int> computeNetLevels() const;
std::vector<int> computeGateLevels() const;
int getMaxDepthToNet(const std::string& netName) const;
```

第二步：

```cpp
CombinationalPath findCriticalPathToNet(const std::string& netName) const;
DepthReport analyzeDepthToNet(const std::string& netName) const;
```

第三步：

```cpp
std::vector<DepthReport> analyzePrimaryOutputDepths() const;
std::vector<std::string> getPrimaryOutputsWithDepthGreaterThan(int limit) const;
```

第四步：

```cpp
std::vector<DepthReport> analyzeDffDDepths() const;
std::vector<std::string> getDffsWithDDepthGreaterThan(int limit) const;
```

第五步：

```cpp
DepthReport findGlobalCriticalPath() const;
std::vector<DepthReport> findEndpointsExceedingDepth(int maxDepth) const;
```

---

## 12. 第一版暫不處理

第一版 DepthAnalysis 先不做：

```text
real delay / library timing
cell area
load capacitance
slew
arrival / required time
slack
multi-cycle path
false path
clock domain timing constraints
```

因為 Problem A 目前 gate-level primitive model 較簡化，先採 unit-depth model 較適合。
