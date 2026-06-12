# Cleanup / Simplification Notes

這份文件整理 Stage A「本地安全清理 / 簡化」會遇到的問題，以及實作時需要用到或新增的 function。

Stage A 的目標：

```text
先做保守、容易驗證、容易 rollback 的 local optimization。
不要一開始就做複雜 Boolean rewriting。
```

---

## 1. 共通風險

### 1.1 DFF boundary

DFF 是 sequential boundary。

清理 / 簡化 pass 不應該：

```text
穿越 DFF
改掉 DFF.Q 的語意
把 DFF.D 直接接到 DFF.Q
把 CK / RN / SN 當成一般 data path 重寫
```

目前建議：

```text
PI / DFF.Q / constants  當 combinational startpoint
PO / DFF.D              當 combinational endpoint
CK / RN / SN            先不納入資料路徑最佳化
```

相關既有 function：

```cpp
getGatesByType(GateType::DFF)
getDffOutputNetId(int dffGateId)
getGateInputNetId(int gateId, const std::string& pinName)
analyzeDffDDepths()
```

---

### 1.2 Primary Output net 不能隨便移除

如果 gate output 是 PO，例如：

```verilog
buf g_y(y, n_buf);
```

第一版不要直接刪掉 `y` 或把 `y` 改名。

原因：

```text
PO name 是外部 interface。
VerilogWriter 需要保留 module output。
LEC / prompt 回覆也依賴 output name。
```

目前策略：

```text
output net 是 PO 的 BUF / NOT，第一版先跳過。
```

相關既有欄位：

```cpp
Net::isPO
```

---

### 1.3 fanout sharing

清理某個 gate output net 時，可能影響多個 downstream gates。

例子：

```text
n_buf 同時接到 g_y 和 ff1.D
```

如果 bypass `g_buf`：

```text
n_buf 的所有 loads 都要改接到 input net。
driver/load graph 也要同步更新。
```

需要特別確認：

```text
output net 的 loadGateIds 是否完整更新
input net 的 loadGateIds 是否補上新 loads
gate inputNetIds 是否更新
DFF inputPinNames 與 inputNetIds 順序不能被破壞
```

相關既有 function：

```cpp
disconnectGateInput(...)
connectGateInput(...)
getGateInputNetId(...)
```

可能需要新增：

```cpp
bool replaceGateInputNet(int gateId, int oldNetId, int newNetId);
bool replaceAllLoadsOfNet(int oldNetId, int newNetId);
```

---

### 1.4 unconnected / floating net

題目 Q&A 說可能有 floating 或 unconnected ports。

因此所有 pass 都要防：

```text
netId == -1
gateId == -1
outputNetId == -1
inputNetIds 裡有 -1
```

相關既有修正：

```text
DFF disconnect 時保留 -1，不 erase input slot。
VerilogWriter 已避免 getNet(-1)。
```

---

### 1.5 removed gate 策略

如果真的移除 gate，有兩種做法：

```text
1. erase gates vector 裡的 gate
2. 加 removed flag，保留 gate ID
```

第一版建議：

```text
先避免 erase gates vector。
如果要大量移除，最好加 Gate::removed 或做 compaction pass。
```

原因：

```text
gate ID 被大量 API 使用。
直接 erase 會造成 ID 遞補，容易破壞 path / cone / tests。
```

可能需要新增：

```cpp
bool isGateRemoved(int gateId) const;
bool markGateRemoved(int gateId);
bool compactRemovedGates();
```

---

### 1.6 cache invalidation

題目環境有 128GB memory，可以使用 cache / memo。

但 transformation 後必須 invalidate：

```text
depth cache
cone cache
path cache
candidate cache
structural hash table
```

目前還沒有正式 cache 層，因此第一版每次修改後直接重新計算即可。

---

### 1.7 validation / rollback

每個 rewrite pass 後都應該走：

```text
structural validation
constraint validation
function validation
accept / rollback
```

目前既有 function：

```cpp
checkEquivalence(...)
```

可能需要新增：

```cpp
bool validateStructure() const;
bool validateProblemAConstraints() const;
Netlist cloneForRollback() const; // 或直接 copy Netlist
```

---

## 2. Pass-by-Pass 問題與需要 function

### 2.1 Buffer simplification / buffer cleanup

目標：

```text
in -> BUF -> out -> loads
改成
in -> loads
```

目前已完成：

```cpp
findBufferGatesOnCriticalPath(const OptimizationCandidate& candidate)
findRemovableBufferGatesOnCriticalPath(const OptimizationCandidate& candidate)
```

第一版安全條件：

```text
1. gate type 是 BUF
2. BUF 剛好 1 input
3. input net / output net 都有效
4. output net 不是 Primary Output
5. output net 不是 constant
6. input net != output net
7. output net 至少有一個 load gate
```

建議下一批新增：

```cpp
std::vector<int> findAllRemovableBufferGates() const;
OptimizationResult cleanupBufferChain(const OptimizationCandidate& candidate);
OptimizationResult cleanupAllRemovableBuffers();
```

內部可能需要：

```cpp
bool bypassBufferGate(int bufferGateId);
bool replaceAllLoadsOfNet(int oldNetId, int newNetId);
```

---

### 2.2 Double buffer removal

模式：

```text
a -> BUF -> n1 -> BUF -> n2
```

可簡化成：

```text
a -> BUF -> n2
```

或搭配 buffer cleanup 直接 bypass 中間 buffer。

可以視為：

```text
buffer cleanup 的特例
```

需要 function：

```cpp
std::vector<int> findDoubleBufferPatterns() const;
```

但第一版不一定要獨立做，先讓 global buffer cleanup 處理即可。

---

### 2.3 Double inverter removal

模式：

```text
a -> NOT -> n1 -> NOT -> n2
```

可簡化成：

```text
a -> n2
```

主要風險：

```text
第二個 NOT output 是 PO 時先不要動。
中間 net 有其他 fanout 時不能簡單刪。
```

需要 function：

```cpp
std::vector<std::pair<int, int>> findDoubleInverterPairs() const;
bool bypassDoubleInverter(int firstNotGateId, int secondNotGateId);
```

---

### 2.4 Constant propagation

常見規則：

```text
and(a, 1) = a
and(a, 0) = 0
or(a, 0)  = a
or(a, 1)  = 1
xor(a, 0) = a
xor(a, 1) = not(a)
nand / nor / xnor 要注意 inversion polarity
```

主要風險：

```text
產生常數 net 時要重用 1'b0 / 1'b1。
若需要新增 NOT gate，要產生唯一 instance name / net name。
PO output 仍要保留。
```

既有 function：

```cpp
findGatesWithConstInput(...)
countGatesWithConstInput(...)
```

需要新增：

```cpp
bool isConst0Net(int netId) const;
bool isConst1Net(int netId) const;
int getConst0NetId() const;
int getConst1NetId() const;
bool simplifyGateWithConstant(int gateId);
```

---

### 2.5 Same-input simplification

常見規則：

```text
and(a, a)  = a
or(a, a)   = a
xor(a, a)  = 0
xnor(a, a) = 1
nand(a,a)  = not(a)
nor(a,a)   = not(a)
```

主要風險：

```text
nand / nor 可能需要新增 NOT gate。
PO output 先保守處理。
```

需要新增：

```cpp
std::vector<int> findSameInputGates() const;
bool simplifySameInputGate(int gateId);
```

---

### 2.6 Dangling logic removal

目標：

```text
移除不影響任何 PO / DFF.D 的 gate。
```

基本做法：

```text
從所有 timing endpoints 反向做 transitive fanin。
沒有被標記到的 combinational gates 就是 dangling。
```

timing endpoints：

```text
Primary Output
DFF.D
```

既有 function：

```cpp
getPrimaryOutputNetIds()
analyzeDffDDepths()
getTransitiveFaninCone(...)
```

需要新增：

```cpp
std::unordered_set<int> getEssentialGateIdsForTimingEndpoints() const;
std::vector<int> findDanglingGateIds() const;
OptimizationResult removeDanglingLogic();
```

主要風險：

```text
DFF CK/RN/SN 的來源是否算 essential，要依題目語意決定。
第一版建議保守：DFF 的所有 input source cone 都先保留，或至少不要刪 clock/reset cone。
```

---

### 2.7 Structural hashing

目標：

```text
合併功能與輸入完全相同的 gates。
```

例子：

```text
and g1(n1, a, b);
and g2(n2, b, a);
```

`and` 是 commutative，因此可視為同一個 structural key：

```text
AND(min(a,b), max(a,b))
```

主要風險：

```text
只有 commutative gate 可以排序 input。
NOT / BUF 不可套二輸入 key。
NAND / NOR / XNOR 要保留 type，不可和 AND / OR / XOR 混。
合併後 output net fanouts 要改接到 canonical output net。
```

需要新增：

```cpp
std::string makeStructuralKey(int gateId) const;
std::vector<std::vector<int>> findStructurallyEquivalentGateGroups() const;
OptimizationResult mergeStructurallyEquivalentGates();
```

---

## 3. 目前既有可重用 function

查詢與 graph：

```cpp
getGateId(...)
getNetId(...)
getGate(...)
getNet(...)
getGatesByType(...)
getGateOutputNetId(...)
getGateInputNetId(...)
getPrimaryOutputNetIds()
getDffOutputNetId(...)
getTransitiveFaninCone(...)
getTransitiveFanoutCone(...)
```

Depth / candidate：

```cpp
computeNetLevels()
computeGateLevels()
findCriticalPathToNet(...)
findGlobalCriticalPath()
findEndpointsExceedingDepth(...)
buildOptimizationCandidate(...)
findOptimizationCandidatesExceedingDepth(...)
```

目前已做的 optimization target selector：

```cpp
findBufferGatesOnCriticalPath(...)
findRemovableBufferGatesOnCriticalPath(...)
```

Transformation：

```cpp
renameGate(...)
renameNet(...)
disconnectGateInput(...)
connectGateInput(...)
```

Validation：

```cpp
checkEquivalence(...)
```

---

## 4. 建議下一步

目前已完成第一批 cleanup building blocks：

```cpp
std::vector<int> findAllRemovableBufferGates() const;
bool replaceAllLoadsOfNet(int oldNetId, int newNetId);
bool bypassBufferGate(int bufferGateId);
int cleanupAllRemovableBuffers();
std::vector<std::pair<int,int>> findDoubleInverterPairs() const;
bool bypassDoubleInverter(int g1id, int g2id);
bool simplifyGateWithConstant(int gateId);
std::vector<int> findSameInputGates() const;
bool simplifySameInputGate(int gateId);
std::vector<int> findDanglingGateIds() const;
int removeDanglingLogic();
std::vector<std::vector<int>> findStructurallyEquivalentGateGroups() const;
int mergeStructurallyEquivalentGates();
```

下一步應先補：

```text
1. cleanup / simplification 專用 mini regression test。
2. 統一修改結果 report，例如 NetlistEditReport。
3. 將 cleanupAllRemovableBuffers() 接上 validateStructure() / rollback flow。
4. 再逐步把 double inverter / constant propagation / same-input / dangling / structural hashing 包成固定 pass runner。
```

目前測試狀態：

```text
mini test/tester.cpp 目前主要覆蓋 analysis 高階 API 與少量 mutation primitive。
cleanup pass 尚未完整納入 tester。
目前 regression：Summary: 45 passed, 0 failed.
```
