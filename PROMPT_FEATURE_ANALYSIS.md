# Prompt Feature Analysis

這份文件根據 `testcase/test01` 到 `testcase/test40` 的 `prompt.txt`，整理 Problem A 可能需要的 EDA engine 功能。

## 1. 基礎 I/O 與統計

出現 request：

```text
load design
write current design
count all gates broken down by gate type
compute total gate count
number of primary inputs and outputs
list primary inputs / outputs with bit widths
what type of gate is g0 and pin connections
list all XOR / NAND gates
list gates with constant inputs
```

需要 API：

```cpp
loadVerilog(path)
writeVerilog(path)
countGatesByType()
getTotalGateCount()
getPrimaryInputCount()
getPrimaryOutputCount()
listPrimaryInputsWithWidth()
listPrimaryOutputsWithWidth()
getGateInfo(gateName)
listGatesByType(type)
findGatesWithConstInput(type, constValue optional)
```

現有 code 支援度：

```text
load/write: 已有基本功能
gate count: 已新增 countGatesByType()
PI/PO count: 已有 primaryInputs / primaryOutputs
gate info: 已新增 getGateId() / findGate()
constant input scan: 已新增 findGatesWithConstInput()
```

建議優先做，因為最容易、很多 testcase 都會問。

目前已新增基礎查詢 API：

```cpp
std::string gateTypeToString(GateType type);

int Netlist::getGateId(const std::string& gateInstName) const;
int Netlist::getNetId(const std::string& netName) const;
const Gate* Netlist::findGate(const std::string& gateInstName) const;
const Net* Netlist::findNet(const std::string& netName) const;
std::map<GateType, int> Netlist::countGatesByType() const;
std::vector<int> Netlist::getGatesByType(GateType type) const;
std::vector<int> Netlist::findGatesWithConstInput(
    GateType type = GateType::UNKNOWN,
    int constValue = -1) const;
```

實作位置：

```text
src/core/Netlist.cpp
  核心資料結構操作與 name/id lookup

src/analysis/NetlistAnalysis.cpp
  不改變 netlist 的分析查詢，例如 gate count、依 type 列 gates、constant-input gates
```

`main.cpp` 目前會在 parse 後輸出 gate type breakdown，方便測試：

```text
[Basic Query] Gate type breakdown:
         -> AND: ...
         -> OR: ...
         -> NOT: ...
         -> NAND: ...
         -> NOR: ...
         -> XOR: ...
         -> XNOR: ...
         -> BUF: ...
         -> DFF: ...
```

## 2. Fanout / Fanin 基礎查詢

出現 request：

```text
fanout of primary input n5 / n0
list gates a signal drives directly
number of gates driven by g0
immediate successors of gate g0
every gate connected to output of g0
transitive fanout cone of input n0
transitive fanin cone of output n3 / n12 / ...
logic cone of output n12
gates shared between fanin cones
largest fanin cone
```

需要 API：

```cpp
getDirectFanoutGatesByNet(netName)
getDirectFanoutGatesByGate(gateName)
getGateFanout(gateName)
getTransitiveFanoutGatesFromNet(netName)
getTransitiveFaninGatesToNet(netName)
getFaninConeSize(outputNet)
getSharedFaninCone(outputA, outputB)
findOutputWithLargestFaninCone()
```

現有 code 支援度：

```text
Net::loadGateIds 已有 direct fanout。
Gate::outputNetId + Net::loadGateIds 可做 gate successors。
Net::driverGateId 可往 fanin 方向走。
還需要把 name lookup API 公開，並實作 DFS/BFS。
```

這是第二優先，因為後面 path/depth/optimization 都依賴它。

## 3. Path Existence / Path Enumeration / Cut

出現 request：

```text
determine whether path from A to B exists avoiding node X
does every path from input A to output B pass through gate/wire X
list every path from PI to PO
complete enumeration of paths between A and B
find paths of length 0 direct PI-to-PO
wire X is a cut between any PI and any PO
articulation points in combinational graph
```

需要 API：

```cpp
bool pathExists(srcNet, dstNet, avoidNode optional)
bool everyPathPassesThrough(srcNet, dstNet, requiredNode)
vector<Path> enumeratePaths(srcNet, dstNet, limit optional)
vector<pair<Net,Net>> findZeroLengthPaths()
bool isCutBetweenAnyPIAndPO(nodeName)
vector<Node> articulationPointsBetween(srcNet, dstNet)
```

注意：

```text
完整 enumerate paths 可能爆炸。
需要設定保護，例如 path count limit / depth limit / testcase-specific enough。
Prompt 會要求 list every path，所以至少 sample 可能控制在可列舉大小。
```

現有 code 支援度：

```text
direct graph relation 已有，但需要封裝 graph traversal。
DFF 要當 sequential boundary，combinational path 不應穿過 DFF。
```

## 4. Logic Depth / Critical Path

出現 request：

```text
maximum logic depth from input A to output B
critical path depth between A and B
maximum logic depth of fanin cone
maximum combinational depth in whole design
outputs with depth > 4
deepest output bit
maximum depth from PI to any DFF D-pin
maximum register-to-register path depth
gate g0 lies on any maximum-depth path
```

需要 API：

```cpp
int maxDepth(srcNet, dstNet)
int maxDepthToOutput(outputNet)
int maxDepthWholeDesign()
vector<OutputDepth> outputsWithDepthGreaterThan(k)
Output findDeepestOutput()
int maxDepthPIToDffD()
int maxRegisterToRegisterDepth()
bool gateOnAnyMaxDepthPath(gateName)
```

定義建議：

```text
depth = traversed combinational gate count
PI / DFF Q are startpoints
PO / DFF D are endpoints
DFF itself is sequential boundary，不算 combinational gate
```

現有 code 支援度：

```text
需要 topological DFS/DP。
如果 graph 保證 acyclic between sequential boundaries，DP 可做。
若有 cycle，需要 cycle guard。
```

這是高優先，因為 test04 開始就出現，後續大量出現。

## 5. Functional Equivalence / Boolean Function / Symmetry

出現 request：

```text
signals A and B functionally equivalent
current design equivalent to original
output n16 always 0
derive Boolean equation for output
write logic expression for n30
whether function is symmetric w.r.t. inputs
NAND(a,b) equivalent to n25
```

可能實作方向：

```text
小 cone: truth table simulation
中大型 cone: structural hashing / AIG / BDD / SAT
contest first-pass: bounded truth table for small fanin cone + structural equivalence fallback
```

需要 API：

```cpp
buildCone(outputNet)
collectConePIs(cone)
simulateCone(cone, assignment)
truthTable(signal)
bool equivalent(signalA, signalB)
bool outputConstant(outputNet, value)
string deriveExpression(signal, maxSize optional)
bool symmetric(signal, inputA, inputB)
```

注意：

```text
這類需求難度高。
可以先做 structural equivalence / constant propagation / small fanin truth table。
需要保留 original netlist snapshot 以做 transformed design equivalence。
```

## 6. Cleanup / Dangling / Floating

出現 request：

```text
trim unused wires and gates
remove dangling gates not contributing to PO
floating inputs / unconnected output ports
redundant gates removable
```

需要 API：

```cpp
markReachableFromPO()
removeUnreachableGates()
removeUnusedNets()
findFloatingInputs()
findUnconnectedOutputs()
```

現有 code 支援度：

```text
Net::driverGateId / Gate::outputNetId / loadGateIds 足夠做 reachable sweep。
但目前 Netlist 沒有 delete/update API，需要新增可修改 graph 的方法。
```

## 7. Rename / Reconnect / Buffer Insertion

出現 request：

```text
rename wire n1214 to renamed_wire
rename gate g0 to renamed_gate
reconnect input pin A of gate g0 to signal n24[0]
insert BUF on signal n2 so each load driven through dedicated buffer
insert buffers so no gate drives more than 4 loads
insert buffers on clock/reset to reduce fanout
```

需要 API：

```cpp
renameNet(oldName, newName)
renameGate(oldName, newName)
reconnectGateInput(gateName, pinIndexOrName, newNetName)
insertBufferOnNet(netName)
bufferEachLoad(netName)
limitFanout(maxFanout)
maxFanout(signal optional)
```

現有 code 支援度：

```text
目前只有 add/connect，沒有 disconnect、rename、remove。
要先補 graph mutation API：
  disconnectGateInput
  replaceGateInputNet
  createUniqueNetName
  createUniqueGateName
  update name maps
```

這類 transform 是中高優先，test21 起大量出現。

## 8. Constant Propagation / Local Simplification

出現 request：

```text
NAND with constant input
AND with constant 0 input
OR with constant 1 input
NOR with constant inputs
simplify reported gates
replace NAND with const 1 by inverter
```

需要 API：

```cpp
findGatesWithConstInput(type, value optional)
simplifyGateWithConstant(gateId)
replaceGateWithBufferOrInverterOrConst(gateId)
removeGateAndReconnect(gateId, replacementNet)
```

Basic rules：

```text
AND(x,0)=0
AND(x,1)=x
OR(x,1)=1
OR(x,0)=x
NAND(x,1)=NOT(x)
NAND(x,0)=1
NOR(x,0)=NOT(x)
NOR(x,1)=0
```

需要 mutation API 才能安全做。

## 9. Gate Decomposition / Remapping

出現 request：

```text
XNOR -> NOR-only
XOR -> 4 NAND
reconstruct entire netlist using AND/NOT
remap entire design to NAND/NOT
logic cone using NAND/NOT or NOR/NOT
```

需要 API：

```cpp
replaceXorWithNandNetwork(gateId)
replaceXnorWithNorNetwork(gateId)
remapGateToAndNot(gateId)
remapCone(outputNet, targetLib)
remapWholeDesign(targetLib)
```

注意：

```text
這屬於 transformation。
需要 create gates/nets、remove old gate、rewire fanouts、保持 PO/DFF pins。
```

## 10. Structural Duplicate / Back-to-back Inverter

出現 request：

```text
find back-to-back inverter pairs and collapse them
merge structurally duplicate gates
merge functionally equivalent gates
```

需要 API：

```cpp
findBackToBackNotPairs()
collapseInverterPair(pair)
structuralHashGate(gateId)
mergeEquivalentDrivers(keepGate, removeGate)
```

注意：

```text
structural duplicate 比 functional equivalent 簡單很多。
建議先做 structural duplicate：
  same type + same input net IDs，commutative gates sort inputs
```

## 11. Sequential / DFF-specific

出現 request：

```text
list all flip-flops driven by clock n0
maximum depth from PI to any DFF D-pin
register-to-register paths
D input logic enable/hold structures
```

需要 API：

```cpp
getDffClockNet(gateId)
getDffDNet(gateId)
getDffQNet(gateId)
listDffsByClock(clockNet)
listRegisterToRegisterPaths()
detectDffEnableHoldStructure(dffId)
```

現有 code 重要觀察：

```text
Sample DFF 使用 named pins:
  .RN, .SN, .CK, .D, .Q
現有 Reader 會把 Q 當 output，其餘 inputPinNames 依原順序存。
後續要查 D pin / CK pin，需要用 inputPinNames 找 "D" / "CK"。
```

## 12. 建議實作順序

### Phase 1: 查詢基礎

```text
countGatesByType
getGateInfo
listGatesByType
direct fanout of net/gate
immediate successors
PI/PO count and bit widths
```

### Phase 2: 圖 traversal

```text
transitive fanin / fanout
pathExists with avoid node
everyPathPassesThrough
fanin cone size
shared fanin cone
```

目前已新增 direct fanout / immediate successor API：

```cpp
std::vector<int> Netlist::getDirectFanoutGatesOfNet(const std::string& netName) const;
std::vector<int> Netlist::getDirectFanoutGatesOfGate(const std::string& gateInstName) const;
std::vector<int> Netlist::getImmediateSuccessors(const std::string& gateInstName) const;
```

可支援的 prompt 類型：

```text
What is the fanout of primary input n0?
List every gate that n0 drives directly.
Determine the number of gates driven by g0.
Enumerate the immediate successors of gate g0.
Report every gate connected to the output of g0.
```

### Phase 3: depth

```text
maxDepth(src,dst)
maxDepthToOutput
maxDepthWholeDesign
outputsDepthGreaterThan(k)
PI-to-DFF-D depth
register-to-register depth
```

### Phase 4: simple transforms

```text
rename net/gate
buffer insertion for a specific net
fanout limit buffering
dangling sweep
constant propagation
back-to-back inverter collapse
```

### Phase 5: advanced logic

```text
structural duplicate merge
gate remapping
truth-table based equivalence for small cones
Boolean expression generation
symmetry checking
```
