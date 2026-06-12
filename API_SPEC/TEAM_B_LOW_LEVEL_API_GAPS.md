# Team B Low-Level API Gaps

這份文件整理 **Team B 應負責的 netlist 修改類 API**。

Team B 的邊界是：

```text
安全修改 netlist。
提供 rollback / restore。
提供 cleanup / simplification 的執行 pass。
保證修改後 netlist 結構仍合法。
```

Team B 不負責決定「要不要改」或「哪裡值得改」。  
那些 analysis / candidate / report 應該由 Team A 提供。  
Team C 則負責 optimization / mapping strategy，並呼叫 Team B 的 mutation primitive 來落地。

修改完成後，Team B 的 cleanup / transformation wrapper 應回傳 Team A 定義的：

```cpp
NetlistEditReport
```

不要另外為 cleanup、rename、rewire 各自定義不同 report 格式。

## 1. 目前已經有的 Team B API

基本修改：

```cpp
bool renameGate(oldName, newName);
bool renameNet(oldName, newName);
bool disconnectGateInput(gateName, netName);
bool connectGateInput(gateName, netName, pinIndex);
bool disconnectAllPins(gateId);
bool removeGate(gateId);
```

removed gate / compaction：

```cpp
bool isGateRemoved(gateId);
bool markGateRemoved(gateId);
int compactRemovedGates();
```

validation / rollback snapshot：

```cpp
bool validateStructure() const;
bool validateProblemAConstraints() const;
Netlist cloneForRollback() const;
```

cleanup / simplification execution：

```cpp
bool replaceAllLoadsOfNet(oldNetId, newNetId);
bool bypassBufferGate(bufGateId);
int cleanupAllRemovableBuffers();
bool bypassDoubleInverter(g1id, g2id);
bool simplifyGateWithConstant(gateId);
bool simplifySameInputGate(gateId);
int removeDanglingLogic();
int mergeStructurallyEquivalentGates();
```

目前主要實作檔案：

```text
src/transformation/NetlistTransformation.cpp
src/optimization/NetlistOptimization.cpp
```

所以 Team B 不是缺全部，而是缺「讓 rewrite 可以安全落地」的更精準 mutation primitive、transaction wrapper，以及統一 report。

## 2. B-1 缺 pin-level 精準改線 primitive

Analysis 端可以查 pin，但 Team B 需要真正能改指定 pin。

目前只有：

```cpp
disconnectGateInput(gateName, netName);
connectGateInput(gateName, netName, pinIndex);
```

仍需要補：

```cpp
bool disconnectGateInputPin(int gateId, int pinIndex);
bool connectGateInputPin(int gateId, int pinIndex, int netId);
bool reconnectGateInputPin(int gateId, int pinIndex, int newNetId);

bool reconnectGateInputPinByName(
    const std::string& gateName,
    const std::string& pinName,
    const std::string& newNetName
);
```

會卡住的題型：

- reconnect input pin A of gate g0。
- DFF D / CK / RN / SN pin 安全改接。
- clock/reset buffer insertion 後重新接到指定 DFF pin。

## 3. B-2 缺 PO-safe output rewrite primitive

目前多個 simplification 遇到 output net 是 PO 會跳過。這很安全，但會讓 output cone 不能被簡化。

仍需要補：

```cpp
bool replaceDriverOfNet(int targetNetId, int newDriverGateId);

bool rewireGateOutputToExistingNet(int gateId, int newOutputNetId);

bool preservePortNetAndReplaceDriver(
    int poNetId,
    GateType newGateType,
    const std::vector<int>& inputNetIds
);

bool replaceNetFunctionWithNetKeepingName(
    int targetNetId,
    int sourceNetId
);
```

核心語意：

```text
如果 y 是 PO，不能直接把所有 y 的 load 改成 a。
應該保留 y 這條 port net，改變 y 的 driver 或插入等價 driver。
```

會卡住的題型：

- constant propagation 到 PO。
- same-input simplification 到 PO。
- double inverter collapse 到 PO。
- output cone remap / optimization。

## 4. B-3 缺完整 net merge / net bypass primitive

目前有：

```cpp
replaceAllLoadsOfNet(oldNetId, newNetId);
```

但完整 net merge 還要處理 PI / PO / constant / driver / loads / bus bit。

仍需要補：

```cpp
bool mergeNetIntoNet(int fromNetId, int toNetId);

bool bypassNetKeepingPortSemantics(
    int removedNetId,
    int replacementNetId
);

bool redirectAllLoads(
    int oldNetId,
    int newNetId,
    bool allowDuplicateLoads = false
);

bool removeNetIfUnused(int netId);
int removeUnusedNets();
```

會卡住的題型：

- double inverter collapse。
- double buffer collapse。
- structural duplicate merge。
- dangling net removal。
- constant propagation 中 gate output bypass。

## 5. B-4 缺 gate replacement primitive

目前 replacement 邏輯散在多個 function 裡，容易重複出錯。

仍需要補：

```cpp
bool replaceGateWithNet(int gateId, int sourceNetId);
bool replaceGateWithConstant(int gateId, int constNetId);
bool replaceGateWithNotOfNet(int gateId, int sourceNetId);

int createGateDrivingNet(
    GateType type,
    const std::vector<int>& inputNetIds,
    int outputNetId,
    const std::string& nameHint
);

bool removeGateAndDetachPins(int gateId);
```

會卡住的規則：

```text
AND(x,1)   -> x
OR(x,0)    -> x
NAND(x,1) -> NOT(x)
NOR(x,0)  -> NOT(x)
XOR(x,1)  -> NOT(x)
XNOR(x,0) -> NOT(x)
```

這組 API 是 constant propagation、same-input、mapping、depth rewrite 的共同底層。

## 6. B-5 缺 unique name generator

目前很多地方手動組字串命名，重複 pass 後可能撞名。

仍需要補：

```cpp
std::string makeUniqueGateName(const std::string& prefix) const;
std::string makeUniqueNetName(const std::string& prefix) const;

int addGateWithUniqueName(const std::string& prefix, GateType type);
int addNetWithUniqueName(const std::string& prefix);
```

會卡住的題型：

- fanout buffer insertion。
- constant propagation 新增 NOT。
- technology mapping。
- repeated optimization pass。

## 7. B-6 缺 transaction / rollback primitive

目前有：

```cpp
Netlist cloneForRollback() const;
```

但缺正式 restore API。

仍需要補：

```cpp
struct NetlistSnapshot {
    Netlist netlist;
};

NetlistSnapshot createSnapshot() const;
void restoreSnapshot(const NetlistSnapshot& snapshot);

bool restoreFrom(const Netlist& backup);
bool validateAfterMutation() const;
```

會卡住的題型：

- 所有 transformation。
- mapping / fanout / depth optimization 失敗 rollback。
- Team C 試做 rewrite 後，若 A 的 equivalence/depth checker 失敗，要能還原。

## 8. B-7 缺 cleanup fixpoint runner

目前有單一 gate simplification，但缺全域 fixpoint。

仍需要補：

```cpp
int simplifyAllGatesWithConstants();
int simplifyAllSameInputGates();
int runLocalSimplificationFixpoint();

NetlistEditReport runLocalSimplificationFixpointWithReport();
```

原因：

```text
constant propagation 後可能產生新的 constant propagation 機會。
只掃一輪會簡化不完全。
```

## 9. B-8 缺 old-to-new ID remap after compaction

目前 `compactRemovedGates()` 會讓後面的 gate ID 遞補。  
如果外部已保存 gate ID，compaction 後需要 map 才能追蹤。

仍需要補：

```cpp
struct CompactResult {
    int removedGateCount;
    std::unordered_map<int, int> oldToNewGateId;
    std::unordered_map<int, int> newToOldGateId;
};

CompactResult compactRemovedGatesWithIdMap();
```

用途：

- 幫 `NetlistEditReport` 追蹤 compaction 前後的 changed gate。
- Team C 保存 candidate gate ID 後還能追蹤。
- 避免 DFF / gate ID 在 compaction 後對不上。

## 10. Team B 優先順序

P0：

1. `makeUniqueGateName()` / `makeUniqueNetName()`
2. `replaceGateWithNet()` / `replaceGateWithNotOfNet()` / `replaceGateWithConstant()`
3. `disconnectGateInputPin()` / `reconnectGateInputPin()`
4. `createSnapshot()` / `restoreSnapshot()`

P1：

1. PO-safe rewrite APIs。
2. `mergeNetIntoNet()` / `removeUnusedNets()`。
3. `simplifyAllGatesWithConstants()`。
4. `simplifyAllSameInputGates()`。
5. `runLocalSimplificationFixpoint()`。

P2：

1. `compactRemovedGatesWithIdMap()`。
2. stricter mutation validation hooks。
3. cleanup / mutation wrapper 回傳 `NetlistEditReport`。

## 11. Team B 和 Team A/C 的邊界

| 問題 | 負責 |
| --- | --- |
| 找哪些 gate 可以化簡 | Team A |
| 實際把 gate 化簡掉 | Team B |
| 判斷修改後是否等價 | Team A |
| 統一修改結果 report 格式 | Team A |
| cleanup / mutation 後填入 report | Team B |
| 修改失敗後 rollback | Team B |
| 決定哪個 cone 要 optimize | Team C |
| 真正改接 cone 裡的 net/gate | Team B |
| 計算修改前後 gate 數 | Team A |
| 產生 unique gate/net name | Team B |

## 12. 目前測試狀態

```text
mini test/tester.cpp 目前覆蓋 renameGate、disconnectGateInput、connectGateInput 與 VerilogWriter。
cleanup / simplification pass 尚未完整納入 regression tester。
目前 regression：Summary: 45 passed, 0 failed.
```
