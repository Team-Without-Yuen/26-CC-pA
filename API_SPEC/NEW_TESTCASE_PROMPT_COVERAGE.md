# NewTestCase Prompt Coverage Notes

這份文件用來對照 `NewTestCase/testXX/prompt.txt` 中出現的原文需求，確認目前 `API_SPEC` 中定義的高階 API 是否能處理。

分類方式：

```text
OK       : 目前已有明確高階 API / usage 可以處理。
Partial  : 現有 API 可組合出答案，但缺少更直接的 mode / wrapper / usage 規範。
Missing  : 目前缺正式高階 API，或只有底層實作但尚未 public 化。
Future   : 屬於真正 optimization / mapping / equivalence flow，需後續補完整流程。
```

---

## 1. 已可直接處理的 Prompt 類型

這一類 prompt 已經可以由目前的 `StructureQuery` facade、`ConeQuery`、`PathQuery`、`DepthQuery`、`FunctionQuery` 或 `EditApply` 處理。`StructureQuery` 內部 dispatch 到 Basic/DirectConnectivity；`PathQuery` 內部可使用 Graph dominator engine。

### 1.1 Design Summary / Gate Count

Prompt 原文：

```text
Please count all the gates in this design and report the total count broken down by gate type (AND, OR, NOT, NAND, NOR, XOR, XNOR, BUF, DFF).
```

出現位置：

```text
test02 ~ test40 大多數 testcase
```

目前對應 API：

```cpp
BasicQuery query;
query.type = BasicQueryType::Summary;
BasicReport report = netlist.runBasicQuery(query);
```

或：

```cpp
BasicQuery query;
query.type = BasicQueryType::CountByGateType;
query.gateType = GateType::UNKNOWN;
BasicReport report = netlist.runBasicQuery(query);
```

Coverage：

```text
OK
```

主要讀取：

```text
report.gateCount
report.gateTypeCounts
```

---

### 1.2 Total Gate Count

Prompt 原文：

```text
Compute the total gate count of the design.
```

出現位置：

```text
test20
```

目前對應 API：

```cpp
BasicQuery query;
query.type = BasicQueryType::Summary;
BasicReport report = netlist.runBasicQuery(query);
```

Coverage：

```text
OK
```

主要讀取：

```text
report.gateCount
```

---

### 1.3 Primary Input / Primary Output Count

Prompt 原文：

```text
Determine the number of primary inputs and outputs.
```

出現位置：

```text
test32
```

Prompt 原文：

```text
How many primary inputs and primary outputs does this design have?
```

出現位置：

```text
test37
```

目前對應 API：

```cpp
BasicQuery query;
query.type = BasicQueryType::Summary;
BasicReport report = netlist.runBasicQuery(query);
```

Coverage：

```text
OK
```

主要讀取：

```text
report.primaryInputCount
report.primaryOutputCount
```

---

### 1.4 List Gates By Type

Prompt 原文：

```text
List all XOR gates in this design.
```

出現位置：

```text
test39, test40
```

Prompt 原文：

```text
List all NAND gates in this design with their input and output signals.
```

出現位置：

```text
test35
```

目前對應 API：

```cpp
BasicQuery query;
query.type = BasicQueryType::GatesByType;
query.gateType = GateType::XOR; // or NAND
BasicReport report = netlist.runBasicQuery(query);
```

若需要 input/output pin connections：

```cpp
BasicQuery info;
info.type = BasicQueryType::GateInfo;
info.name = gateName;
BasicReport gateInfo = netlist.runBasicQuery(info);
```

Coverage：

```text
OK
```

注意：

```text
「with their input and output signals」需要對 GatesByType 回傳的每個 gate 再呼叫 GateInfo。
這屬於可接受的批次查詢，不一定要新增 API。
```

---

### 1.5 Gate Type And Pin Connections

Prompt 原文：

```text
What type of gate is g0? Report its gate type and pin connections.
```

出現位置：

```text
test31, test36, test40
```

目前對應 API：

```cpp
BasicQuery query;
query.type = BasicQueryType::GateInfo;
query.name = "g0";
BasicReport report = netlist.runBasicQuery(query);
```

Coverage：

```text
OK
```

主要讀取：

```text
report.typeName
report.formattedInfo
```

---

### 1.6 Constant Input Gates

Prompt 原文：

```text
Report any NAND gates with constant inputs (0 or 1) in this design.
```

出現位置：

```text
test32, test38
```

Prompt 原文：

```text
Report any OR gates with a constant 1 input in this design.
```

出現位置：

```text
test39
```

Prompt 原文：

```text
Report any AND gates with a constant 0 input in this design.
```

出現位置：

```text
test36
```

Prompt 原文：

```text
List all gates with one or more inputs tied to 1'b1.
```

出現位置：

```text
test37
```

目前對應 API：

```cpp
BasicQuery query;
query.type = BasicQueryType::GatesWithConstantInput;
query.gateType = GateType::NAND; // OR / AND / UNKNOWN
query.constValue = -1;           // 0, 1, or -1 for either
BasicReport report = netlist.runBasicQuery(query);
```

Coverage：

```text
OK
```

---

### 1.7 Floating / Structural Issues

Prompt 原文：

```text
Check if there are any floating inputs or unconnected output ports in this design.
```

出現位置：

```text
test37
```

Prompt 原文：

```text
How many floating signals were found?
```

出現位置：

```text
test37
```

目前對應 API：

```cpp
BasicQuery query;
query.type = BasicQueryType::StructuralIssues;
BasicReport report = netlist.runBasicQuery(query);
```

Coverage：

```text
OK
```

主要讀取：

```text
report.floatingNets
report.unconnectedGates
```

注意：

```text
若 prompt 嚴格區分 floating input / unconnected output port，目前 report 命名還偏 general structural issue。
但 API 已能給出主要資料。
```

---

### 1.8 Direct Fanout / Immediate Successors

Prompt 原文：

```text
What is the fanout of primary input n5? List all gates that n5 drives directly.
```

出現位置：

```text
test05
```

Prompt 原文：

```text
What is the fanout of primary input n0? List every gate that n0 drives directly.
```

出現位置：

```text
test31, test34, test36
```

Prompt 原文：

```text
Determine the number of gates driven by g0.
```

出現位置：

```text
test12 ~ test20
```

Prompt 原文：

```text
Enumerate the immediate successors of gate g0.
```

出現位置：

```text
test14 ~ test20
```

Prompt 原文：

```text
Report every gate connected to the output of g0.
```

出現位置：

```text
test31, test35, test37
```

目前對應 API：

```cpp
DirectConnectivityQuery query;
query.type = DirectConnectivityQueryType::NetLoadGates;
query.netName = "n5";
DirectConnectivityReport report = netlist.runDirectConnectivityQuery(query);
```

或：

```cpp
DirectConnectivityQuery query;
query.type = DirectConnectivityQueryType::GateFanout;
query.gateName = "g0";
DirectConnectivityReport report = netlist.runDirectConnectivityQuery(query);
```

Coverage：

```text
OK
```

主要讀取：

```text
report.count
report.gateNames
```

---

### 1.9 Transitive Fanin / Fanout Cone

Prompt 原文：

```text
Compute the transitive fanin cone of output n3.
```

出現位置：

```text
test15
```

Prompt 原文：

```text
Compute the transitive fanout cone of input n0[0].
```

出現位置：

```text
test15
```

Prompt 原文：

```text
Determine all gates reachable from n2.
```

出現位置：

```text
test31
```

Prompt 原文：

```text
What is the transitive fanout of primary input n0? List all gates reachable from n0.
```

出現位置：

```text
test32
```

Prompt 原文：

```text
Compute the fanin logic cone of output n14 and list all gates that contribute to this output.
```

出現位置：

```text
test38
```

目前對應 API：

```cpp
ConeQuery query;
query.type = ConeQueryType::NetTransitiveFanin; // or NetTransitiveFanout
query.netName = "n14";
ConeReport report = netlist.runConeQuery(query);
```

Coverage：

```text
OK
```

主要讀取：

```text
report.gateCount
report.gateNames
report.netNames
```

---

### 1.10 Gate Count In Fanin Cone

Prompt 原文：

```text
How many gates are in the fanin cone of primary output n15?
```

出現位置：

```text
test03
```

Prompt 原文：

```text
How many gates are in the logic cone of output n12?
```

出現位置：

```text
test32
```

目前對應 API：

```cpp
ConeQuery query;
query.type = ConeQueryType::NetTransitiveFanin;
query.netName = "n15";
ConeReport report = netlist.runConeQuery(query);
```

Coverage：

```text
OK
```

主要讀取：

```text
report.gateCount
```

---

### 1.11 Path Existence With Avoid Node

Prompt 原文：

```text
Determine whether a combinational path from n0[0] to n4 exists that does not traverse node n719.
```

出現位置：

```text
test06
```

Prompt 原文：

```text
Verify whether a path connecting input n0[1] to output n6 exists while avoiding n94.
```

出現位置：

```text
test07
```

Prompt 原文：

```text
Does a combinational path from n2 to n30 exist that avoids n4552?
```

出現位置：

```text
test34
```

目前對應 API：

```cpp
PathQuery query;
query.mode = PathQueryMode::Exists;
query.startpoints = { PathEndpoint(PathEndpointType::SpecificNet, "n0[0]") };
query.endpoints = { PathEndpoint(PathEndpointType::SpecificNet, "n4") };
query.avoidedNodes = { PathNode(PathNodeType::Net, "n719") };
PathQueryResult result = netlist.runPathQuery(query);
```

Coverage：

```text
OK
```

主要讀取：

```text
result.exists
```

---

### 1.12 Path Enumeration

Prompt 原文：

```text
List every path originating at primary input n0[1] and terminating at primary output n10.
```

出現位置：

```text
test08
```

Prompt 原文：

```text
Provide a complete enumeration of paths between n2 and n117[0].
```

出現位置：

```text
test09
```

Prompt 原文：

```text
Find all combinational paths from primary input n2 to primary output n12 and list each path showing the gates traversed.
```

出現位置：

```text
test32
```

目前對應 API：

```cpp
PathQuery query;
query.mode = PathQueryMode::EnumerateAll;
query.startpoints = { PathEndpoint(PathEndpointType::SpecificNet, "n2") };
query.endpoints = { PathEndpoint(PathEndpointType::SpecificNet, "n12") };
PathQueryResult result = netlist.runPathQuery(query);
```

Coverage：

```text
OK
```

注意：

```text
大型 testcase 可能路徑數爆炸。API_SPEC 目前已有 writePathsToFile / maxPrintedPaths。
若官方 testcase 很大，後續可補 count-only / streaming mode。
```

---

### 1.13 Startpoint-To-Endpoint Max / Longest / Critical Path Depth

Prompt 原文：

```text
Compute the maximum logic depth from input n12 to output n25[0].
```

出現位置：

```text
test10
```

Prompt 原文：

```text
Determine the longest combinational path depth from n30 to n31[1].
```

出現位置：

```text
test11
```

Prompt 原文：

```text
Calculate the critical path depth between n4 and n5[0].
```

出現位置：

```text
test13
```

目前對應 API：

```cpp
PathQuery query;
query.mode = PathQueryMode::MaxDepth;
query.startpoints = { PathEndpoint(PathEndpointType::SpecificNet, "n12") };
query.endpoints = { PathEndpoint(PathEndpointType::SpecificNet, "n25[0]") };
PathQueryResult result = netlist.runPathQuery(query);
```

Coverage：

```text
OK
```

主要讀取：

```text
result.depth
result.path
```

---

### 1.14 Output / Cone / Global Depth

Prompt 原文：

```text
Compute the maximum logic depth of the fanin cone of output n3.
```

出現位置：

```text
test04
```

Prompt 原文：

```text
What is the maximum combinational logic depth in the design now?
```

出現位置：

```text
test35, test38, test39, test40
```

Prompt 原文：

```text
What is the maximum logic depth from any primary input to any DFF D-pin in this design?
```

出現位置：

```text
test31, test36
```

目前對應 API：

```cpp
DepthQuery query;
query.type = DepthQueryType::SpecificNet;
query.netName = "n3";
DepthReportSet report = netlist.runDepthQuery(query);
```

或：

```cpp
DepthQuery query;
query.type = DepthQueryType::GlobalCriticalPath;
DepthReportSet report = netlist.runDepthQuery(query);
```

或：

```cpp
DepthQuery query;
query.type = DepthQueryType::DffD;
DepthReportSet report = netlist.runDepthQuery(query);
```

Coverage：

```text
OK
```

注意：

```text
「maximum combinational logic depth in the design」目前可用 GlobalCriticalPath，
語意是 PO 與 DFF.D timing endpoints 中最大 depth。
```

---

### 1.15 Outputs With Depth Greater Than Threshold

Prompt 原文：

```text
How many outputs have a logic depth greater than 4?
```

出現位置：

```text
test31, test39
```

目前對應 API：

```cpp
DepthQuery query;
query.type = DepthQueryType::PrimaryOutputsExceedingDepth;
query.threshold = 4;
DepthReportSet report = netlist.runDepthQuery(query);
```

Coverage：

```text
OK
```

注意：

```text
DepthQuery::EndpointsExceedingDepth 目前包含 PO 與 DFF.D。
若 prompt 明確說 outputs，使用 PrimaryOutputsExceedingDepth，其 count 可直接作答。
```

---

### 1.16 Register-To-Register Paths

Prompt 原文：

```text
List all register-to-register paths in this design through combinational logic.
```

出現位置：

```text
test32, test37
```

Prompt 原文：

```text
What is the maximum combinational depth on any register-to-register path in this design?
```

出現位置：

```text
test40
```

目前對應 API：

```cpp
RegisterPathQuery query;
query.mode = RegisterPathQueryMode::EnumerateAll; // or MaxDepth
RegisterPathReport report = netlist.runRegisterPathQuery(query);
```

Coverage：

```text
OK
```

---

### 1.17 Signal Equivalence

Prompt 原文：

```text
Determine whether signals n2122 and n2116 are functionally equivalent.
```

出現位置：

```text
test17
```

Prompt 原文：

```text
Verify that n1039 and n1046 produce identical logic values for all inputs.
```

出現位置：

```text
test18
```

Prompt 原文：

```text
Check functional equivalence between internal signals n1287 and n2404.
```

出現位置：

```text
test31
```

目前對應 API：

```cpp
FunctionQuery query;
query.type = FunctionQueryType::Equivalence;
query.netNameA = "n2122";
query.netNameB = "n2116";
FunctionReport report = netlist.runFunctionQuery(query);
```

Coverage：

```text
OK
```

主要讀取：

```text
report.equivalent
```

---

### 1.18 Always-Zero / Truth Status

Prompt 原文：

```text
Is output n16 always 0 regardless of all inputs? Report yes or no.
```

出現位置：

```text
test31
```

目前對應 API：

```cpp
FunctionQuery query;
query.type = FunctionQueryType::AlwaysZero;
query.netNameA = "n16";
FunctionReport report = netlist.runFunctionQuery(query);
```

Coverage：

```text
OK
```

---

### 1.19 Rename Gate / Wire

Prompt 原文：

```text
Rename gate g0 to renamed_gate. Ensure functional equivalence is preserved.
```

出現位置：

```text
test24
```

Prompt 原文：

```text
Rename wire n1214 to renamed_wire. Ensure functional equivalence is preserved.
```

出現位置：

```text
test32
```

Prompt 原文：

```text
Try to rename internal signal n1289 to renamed_sig and update all references to it. Ensure the design functionality does not change.
```

出現位置：

```text
test31
```

目前對應 API：

```cpp
EditApplyRequest request;
request.kind = EditCommandKind::RenameNet; // or RenameGate
request.oldName = "n1214";
request.newName = "renamed_wire";
NetlistEditReport report = netlist.runEditApply(request);
```

Coverage：

```text
OK
```

主要讀取：

```text
report.success
report.changed
report.validation.equivalenceMethod
```

---

### 1.20 Cleanup / Simplification / Buffer Insertion

Prompt 原文：

```text
Find all back-to-back inverter pairs and collapse them into a wire. Ensure functional equivalence is preserved.
```

出現位置：

```text
test26, test27, test28, test29, test30, test31, test35, test38, test39, test40
```

Prompt 原文：

```text
Check if there are any dangling gates in this design that do not contribute to any primary output. Remove them if found.
```

出現位置：

```text
test32, test33
```

Prompt 原文：

```text
Simplify the reported NAND gates by propagating their constant inputs. Ensure the design functionality does not change.
```

出現位置：

```text
test32, test38
```

Prompt 原文：

```text
Insert buffers wherever needed so that no gate drives more than 4 loads. Make sure nothing changes functionally.
```

出現位置：

```text
test21
```

Prompt 原文：

```text
Please insert a BUF gate on signal n2 so that each load of n2 is driven through a dedicated buffer. Ensure the design functionality does not change.
```

出現位置：

```text
test31, test39
```

目前對應 API：

```cpp
EditApplyRequest request;
request.kind = EditCommandKind::CollapseDoubleInverter;
NetlistEditReport report = netlist.runEditApply(request);
```

或：

```cpp
request.kind = EditCommandKind::SafeCleanupFixpoint;
request.kind = EditCommandKind::RemoveDanglingLogic;
request.kind = EditCommandKind::SimplifyConstants;
request.kind = EditCommandKind::InsertBuffersForFanout;
request.kind = EditCommandKind::InsertBuffersOnEachLoad;
```

Routing 原則：

```text
精準指定 back-to-back inverter collapse 時，用 CollapseDoubleInverter。
精準指定 dangling gates 時，可用 RemoveDanglingLogic。
若 prompt 寫成 cleanup / prune / trim / floating nodes 這類較廣語意，優先用 SafeCleanupFixpoint。
official test38 的 redundant-gate prompt 已由電路實測確認是 structural duplicates，固定使用 MergeStructurallyEquivalentGates，並由 active gate delta 回答移除數量。
```

Coverage：

```text
OK
```

注意：

```text
「How many X gates were eliminated / added」要從 report.diff.gateTypeCountDelta 或 changedGateNames 讀。
建議後續在 usage 中更明確規範計數讀法。
```

---

## 2. 可處理但建議補更清楚 Usage / Wrapper 的 Prompt

### 2.1 Cone 內 Gate Type Breakdown

Prompt 原文：

```text
Report the number of each gate type in the cone of n8.
```

出現位置：

```text
test37
```

Prompt 原文：

```text
Report the number of each gate type in the cone of n14.
```

出現位置：

```text
test38
```

目前對應 API：

```text
ConeQuery::NetTransitiveFanin
ConeReport::gateTypeCounts
```

Coverage：

```text
OK
```

`gateTypeCounts` 已由 ConeReport 直接回傳，不需要 LLM 對 gate list 自行統計。

---

### 2.2 Shared Gates Between Fanin Cones

Prompt 原文：

```text
Report all gates shared between the fanin cones of n16 and n17.
```

出現位置：

```text
test31
```

目前對應 API：

```text
ConeQuery::SharedFaninGates
query.netName = "n16"
query.secondNetName = "n17"
```

Coverage：

```text
OK
```

共有 gate names、count 與 type counts 由單一 report 回傳。

---

### 2.3 Largest / Deepest Fanin Cone Output

Prompt 原文：

```text
Which output bit has the deepest fanin logic cone?
```

出現位置：

```text
test33, test34, test35
```

Prompt 原文：

```text
Which output has the largest fanin cone?
```

出現位置：

```text
test40
```

目前可行作法：

```text
deepest:
  DepthQuery::PrimaryOutputs，取 depth 最大的 output

largest:
  對每個 PO 執行 ConeQuery::NetTransitiveFanin，取 gateCount 最大的 output
```

Coverage：

```text
Partial
```

缺口：

```text
deepest 可以由 DepthQuery 處理。
largest fanin cone 需要 batch ConeQuery，目前沒有 wrapper。
建議補 ConeQuery usage 或新增 ConeQueryType::LargestFaninConeOutput。
```

---

### 2.4 DFF Clock Loads

Prompt 原文：

```text
List all flip-flops driven by clock n0.
```

出現位置：

```text
test35, test36
```

目前對應 API：

```cpp
DirectConnectivityQuery query;
query.type = DirectConnectivityQueryType::FanoutLoadReport;
query.netName = "n0";
DirectConnectivityReport report = netlist.runDirectConnectivityQuery(query);
```

主要讀取：

```text
report.fanoutLoadReport.dffClockLoads
```

Coverage：

```text
OK / Partial
```

原因：

```text
API 已有 dffClockLoads，但 usage 文件要明確寫「List all flip-flops driven by clock n0」就是 FanoutLoadReport。
若要直接輸出 DFF names，tools.cpp 需要把 IDs 轉成 names。
```

---

### 2.5 Highest Fanout Primary Input

Prompt 原文：

```text
Which primary input has the highest fanout in this design?
```

出現位置：

```text
test36, test38
```

目前可行作法：

```cpp
DirectConnectivityQuery query;
query.type = DirectConnectivityQueryType::GlobalFanoutReport;
query.primaryInputsOnly = true;
DirectConnectivityReport report = netlist.runDirectConnectivityQuery(query);
```

Coverage：

```text
OK / Partial
```

原因：

```text
GlobalFanoutReport 已有 primaryInputsOnly 與 maxFanoutReports。
但 usage 文件與 tools.cpp 輸出需明確支援「最高 fanout PI」。
```

---

### 2.6 Boolean Expression / Equation

Prompt 原文：

```text
Derive the Boolean equation for output n16 in terms of its primary inputs.
```

出現位置：

```text
test31
```

Prompt 原文：

```text
Write the logic expression for n30 using only the primary input names.
```

出現位置：

```text
test34
```

Prompt 原文：

```text
What Boolean function does output n25 compute? Express it in terms of the primary inputs.
```

出現位置：

```text
test35
```

目前狀態：

```text
Netlist.h / Booleanexpression.cpp 已有：
getBooleanExpression()
getSimplifiedBooleanExpression()
getPrimaryInputsOfNet()
```

Coverage：

```text
Partial
```

缺口：

```text
API_SPEC/FUNCTION_ANALYSIS_USAGE.md 仍把 Boolean equation 寫成 future。
需要把它正式納入 FunctionQuery，或新增 FunctionQueryType::BooleanExpression。
```

---

## 3. Technology Mapping / Basis Conversion Prompt

### 3.1 Technology Mapping / Basis Conversion

Prompt 原文：

```text
Convert the logic cone of n10 to use only NOR and NOT gates while preserving functional equivalence.
```

出現位置：

```text
test26
```

Prompt 原文：

```text
Decompose all XOR gates in the fanin cone of n15 into AND, OR, and NOT gates without changing functionality.
```

出現位置：

```text
test27
```

Prompt 原文：

```text
Reconstruct the entire netlist using only AND and NOT gates while preserving functional equivalence.
```

出現位置：

```text
test28, test29, test30, test34
```

Prompt 原文：

```text
Convert every XNOR gate in this design to an equivalent NOR-only circuit. Ensure the design functionality does not change.
```

出現位置：

```text
test33
```

Prompt 原文：

```text
Convert every XOR gate in this design to an equivalent 4-NAND circuit. Ensure the design functionality does not change.
```

出現位置：

```text
test39
```

Prompt 原文：

```text
Remap the entire design to use only NAND and NOT gates. Make sure nothing changes functionally.
```

出現位置：

```text
test40
```

目前狀態：

```text
TechMapper 底層已有 convertToBasis(), customMapTechnology(), mapTechnologyWithReport()。
目前已補成 public edit_apply 子功能：
- EditCommandKind::ConvertToBasis
- EditCommandKind::ReplaceGateType
```

Coverage：

```text
Covered by runEditApply()
```

對應方式：

```text
Convert the logic cone of n10 to use only NOR and NOT gates
  -> ConvertToBasis(scope = NET_FANIN, scopeName = "n10", allowedTypes = {NOR, NOT})

Decompose all XOR gates in the fanin cone of n15 into AND, OR, and NOT gates
  -> ReplaceGateType(scope = NET_FANIN, scopeName = "n15", targetGateType = XOR, allowedTypes = {AND, OR, NOT})

Reconstruct the entire netlist using only AND and NOT gates
  -> ConvertToBasis(scope = WHOLE_NETLIST, allowedTypes = {AND, NOT})

Convert every XNOR gate in this design to an equivalent NOR-only circuit
  -> ReplaceGateType(scope = WHOLE_NETLIST, targetGateType = XNOR, allowedTypes = {NOR})

Convert every XOR gate in this design to an equivalent 4-NAND circuit
  -> ReplaceGateType(scope = WHOLE_NETLIST, targetGateType = XOR, allowedTypes = {NAND})

Remap the entire design to use only NAND and NOT gates
  -> ConvertToBasis(scope = WHOLE_NETLIST, allowedTypes = {NAND, NOT})
```

回傳：

```text
NetlistEditReport
mappingDelta.removedCountByType
mappingDelta.addedCountByType
mappingDelta.finalGateCountByType
validation.equivalenceMethod
```

---

### 3.2 Mapping 後新增 Gate 數量

Prompt 原文：

```text
How many NOR gates were added by replacing the XNOR gates?
```

出現位置：

```text
test35
```

Prompt 原文：

```text
How many NAND gates were added by replacing the XOR gates?
```

出現位置：

```text
test35
```

Prompt 原文：

```text
How many NAND gates are now in the design after the XOR-to-NAND conversion?
```

出現位置：

```text
test39
```

Coverage：

```text
Covered by runEditApply() technology mapping report
```

讀取方式：

```text
How many NOR gates were added by replacing the XNOR gates?
  -> previous ReplaceGateType report.mappingDelta.addedCountByType[NOR]

How many NAND gates were added by replacing the XOR gates?
  -> previous ReplaceGateType report.mappingDelta.addedCountByType[NAND]

How many NAND gates are now in the design after the XOR-to-NAND conversion?
  -> previous ReplaceGateType report.mappingDelta.finalGateCountByType[NAND]
```

---

## 4. 特殊分析 Prompt 狀態

### 4.1 Original vs Current Whole-Design Equivalence

Prompt 原文：

```text
Prove that the transformed design is equivalent to the pre-transformation netlist.
```

出現位置：

```text
test31, test35
```

Prompt 原文：

```text
Verify functional equivalence between the current design and the original loaded netlist.
```

出現位置：

```text
test32, test36, test40
```

Prompt 原文：

```text
Check whether the current netlist is functionally equivalent to the netlist as last loaded from disk.
```

出現位置：

```text
test33, test34, test37
```

Coverage：

```text
Covered
```

目前對應 API：

```text
current.checkWholeDesignEquivalence(original, totalTimeBudgetSeconds)
```

比較邊界與 report：

```text
- 依名稱檢查 PI、PO 與 DFF instance interface。
- 用 batched SAT miter 比較所有 PO 與 DFF.D next-state functions。
- DFF.Q 依 instance name 作為共享 sequential boundary leaf。
- 回傳 matched/mismatched/skipped PO 與 DFF.D names、timeout 與 unsupported reason。
```

驗證：

```text
mini test/test3: 9/9 passed
mini test/test20: 可偵測只改變 DFF.D、但所有 PO 保持相同的錯誤修改
NewTestCase/test40 post-edit: 134 PO + 2585 DFF.D 全部等價，約 3.73 秒
```

---

### 4.2 Symmetry Check

Prompt 原文：

```text
Check whether the function at n11 is symmetric with respect to inputs n3 and n9[0].
```

出現位置：

```text
test36
```

Prompt 原文：

```text
Check whether the function at n8 is symmetric with respect to inputs n3 and n4[0].
```

出現位置：

```text
test37
```

Coverage：

```text
Covered
```

實作：

```text
FunctionQueryType::Symmetry
func_query symmetry <target_net_or_bus> <input_a> <input_b>
使用 swapped-cofactor dual-cone SAT miter；SAT 回傳非對稱反例，UNSAT 證明 symmetric。
```

驗證結果：

```text
test36: func_query symmetry n11 n3 n9[0] -> SYMMETRIC / NOT_NEEDED
test37: func_query symmetry n8 n3 n4[0] -> SYMMETRIC / NOT_NEEDED
兩題指定的 inputs 均不在 target structural support，因此不需啟動 SAT solver。
mini test/test22: 14/14 passed，含 scalar/bus target、DFF.Q、反例與錯誤邊界。
```

---

### 4.3 Cut / Articulation Point

Prompt 原文：

```text
Determine whether wire n55104 is a cut between any primary input and any primary output. Report yes or no.
```

出現位置：

```text
test33
```

Prompt 原文：

```text
Determine whether wire n10239 is a cut between any primary input and any primary output. Report yes or no.
```

出現位置：

```text
test36
```

Prompt 原文：

```text
Find all articulation points in the combinational graph between n2 and n14.
```

出現位置：

```text
test38
```

Coverage：

```text
Ready
```

已完成：

```text
PathQueryMode::IsSeparator
PathQueryMode::FindMandatoryNodes
path_query pi_po_cut <internal_net>
path_query mandatory_nodes <source_net> <target_net>
```

底層仍使用 `GraphQueryType::IsCutNetBetweenPiPo` / `ArticulationPointsBetween`，但它們是 PathQuery 內部 engine，不再是獨立公開 command。

---

### 4.4 Existing Pair Search For Equivalent NAND(a,b)

Prompt 原文：

```text
Does there exist any pair of internal signals (a, b) already in the netlist such that NAND(a, b) is equivalent to n25?
```

出現位置：

```text
test35
```

Coverage：

```text
OK（high-level C++ API 與 `func_search nand_pairs` tools CLI 均已串接）
```

目前對應 API：

```cpp
FunctionSearchQuery query;
query.type = FunctionSearchQueryType::NandEquivalentInputPairs;
query.mode = FunctionSearchMode::FindAny;
query.targetNetName = "n25";
query.internalSignalsOnly = true;

FunctionSearchReport report = netlist.runFunctionSearchQuery(query);
```

公開 tools command：

```text
func_search nand_pair n25
```

command 會回傳 `complete`、`found`、candidate/SAT 統計，以及只包含 SAT-proven UNSAT miter 的 witness pairs。timeout、result limit 或 unsupported 不可解讀成不存在。

主要讀取：

```text
report.ok / found / complete / timedOut
report.matches[*].netNameA / netNameB
report.matches[*].proofMethod / solverStatus
```

實作使用 deterministic bit-parallel simulation 淘汰不可能的 pair，再以 SAT miter 驗證剩餘 candidates。simulation 不作為最終 proof；只有 UNSAT pair 會進入 `matches`。

test35/n25 實測約 6.0 秒找到 `(n26080, n6359)`，proof 為 `SAT_UNSAT_MITER / UNSAT`。

---

### 4.5 DFF Enable / Hold Structure Detection

Prompt 原文：

```text
Report the D input logic of the flip-flops to report any existing enable or hold structures implemented through multiplexers or AND gates.
```

出現位置：

```text
test40
```

Prompt 原文：

```text
How many flip-flops were found to have enable or hold structures in their D input logic?
```

出現位置：

```text
test40
```

Coverage：

```text
C++ Canonical + Functional API Covered / CLI Functional Mode Covered
```

目前處理方式：

```text
SequentialPatternQuery::DffEnableHold
sequential_query enable_hold all
sequential_query enable_hold all --summary-only

- 支援 OR/AND、NAND/NAND、AND/OR、NOR/NOR canonical feedback-MUX。
- 正規化 NOT、NAND(x,x)、NOR(x,x) 與成對反相 wrapper，可分析 basis mapping 後的 D-input logic。
- C++ API 與 `sequential_query --functional-fallback` 可 opt-in 啟用 SAT cofactor fallback，辨識 XOR restructuring 或 internal selector 等非 canonical、但功能等價的 MUX-hold。
- fallback 會驗證 control 的 0/1 可達性，並回傳 candidate/time/completeness report；CLI 預設關閉，且公開 candidate、simulation、per-DFF 與 query-wide time budget。
- 回傳 DFF、D/Q、enable、data、feedback、active level 與 evidence gates。
- matchedDffCount 依 DFF instance 去重。
- AND-only D-input 目前只列為 semantics-pending candidate，不計入 confirmed count。
```

驗證：

```text
mini test/test19: 15/15 passed
mini test/test20: 13/13 passed
mini test/test21 CLI: 18/18 passed（canonical + opt-in functional）
mini test/test28 functional pattern: 19/19 passed
NewTestCase/test40 original design: 2585 DFF analyzed, 1583 canonical matches
NewTestCase/test40 after NAND/NOT mapping and cleanup: 1590 confirmed matches, 1975 candidates
post-edit query: about 0.050 seconds; complete pre-query flow: about 33.7 seconds
CLI summary output: 1101 characters, complete aggregate; detail defaults to 50-record pages
test40 functional FindAny + simulation-aware ranking + safe-Reject quota + hybrid SAT session benchmark: 12 顆抽樣連續執行皆 12/12 complete。quota 64 的 2 秒執行約開始 43 顆 fallback DFF、找到 27 筆額外 proven matches；quota 4/8 的 2 秒執行約找到 90–92 筆。quota 4 在 10 秒找到 193 筆；20 秒 budget 的執行實際約 13.4 秒走完全部 1002 顆 fallback DFF，找到 263 筆，matched DFF 1583 -> 1846。後者沒有 timeout/unknown，但 760 顆仍有 unexamined searchable candidates，因此仍為 PARTIAL，所有數字只能視為 proven lower bound
```

---

## 5. 已完成：真正 Optimization 類 Prompt

### 5.1 Critical Path / Max Depth Optimization

Prompt 原文：

```text
Reduce the critical path depth through restructuring. Make sure nothing changes functionally. The cost function is the maximum logic depth of the final design; smaller is better.
```

出現位置：

```text
test22, test23
```

Prompt 原文：

```text
Optimize the logic to minimize maximum path depth. Ensure functional equivalence is preserved.
```

出現位置：

```text
test24
```

Prompt 原文：

```text
Minimize the critical path depth of the design, ensuring the netlist remains AND and NOT only.
```

出現位置：

```text
test29, test30
```

Coverage：

```text
Covered by C++ OptApply and tools.cpp；heuristic best-effort
```

目前狀態：

```text
OptApply::CriticalPathDepth 已完成：
- working-copy candidate + transactional commit
- Problem A depth metric（NOT/BUF 各算一層）
- global/scoped depth objective
- whole-netlist allowed/banned basis enforcement
- no-improvement / target / invalid candidate rollback
- mandatory PO + DFF.D whole-design SAT
- NetlistEditReport.depthChange / depthOptimization

test22 實測 global depth 41 -> 20。
tools.cpp 已 expose `opt_query/opt_apply critical_path_depth`，並輸出完整
depth change、constraint、candidate、whole-design SAT 與 rollback 狀態。
LLM-facing 契約見 `TOOLS_SPEC/OPTIMIZATION_TOOL.md`。
```

建議：

```text
放在 technology mapping + whole-design equivalence 之後。
先能保證安全 mapping / cleanup，再做 depth optimization。
```

---

### 5.2 Cone-Restricted Depth Optimization With Basis Constraint

Prompt 原文：

```text
Reduce critical path depth through logic restructuring, ensuring the cone of n10 maintains only NOR and NOT gates.
```

出現位置：

```text
test26
```

Prompt 原文：

```text
Optimize the logic to minimize maximum path depth, ensuring the cone of n15 contains only AND, OR, and NOT gates.
```

出現位置：

```text
test27
```

Prompt 原文：

```text
Optimize the depth of the cone of n8 while ensuring the cone of n8 maintains only NAND and NOT gates.
```

出現位置：

```text
test33
```

Prompt 原文：

```text
Optimize the logic cone of output n14 while ensuring the netlist remains NAND and NOT only.
```

出現位置：

```text
test40
```

Coverage：

```text
Public command covered；large scoped-cone runtime remains partial
```

目前狀態：

```text
DFF.Q fanin target 會由 resolveRewriteScope() 轉成 D-pin data cone。
local allowed/banned basis、scope validation、whole-design SAT 與 rollback 已整合。

test26 實測：
- requested scope n10
- resolved D-pin root n1113
- original cone 195 gates
- final global depth 58 -> 30
- final resolved cone 648 gates
- NOR/NOT compliance PASS

mini test/test30/test31 已覆蓋 malformed scope、DFF.Q scope、basis compliance、
target rollback 與 whole-design SAT；mini test/test32 另覆蓋 tools help/parser、
全域改善、scoped hard constraint、report cache、timeout envelope、rollback 與
保守 lower-bound proof。

Official bounded smoke：
- test40：完整前置流程後 n14 為 `NOT(NAND(n514,n412))`，NAND/NOT depth 2
  可證明已最優，約 3.6 秒回 original。
- test33：n8 解析為 depth 327 的大型 D-pin cone；前置 edit 完成，但 global
  XAG primitive 在 120 秒 outer timeout 內未完成。
- `--time-limit` 尚不能中止單次 mockturtle primitive，因此 test33 仍是
  optimizer-core runtime blocker。
```

---

## 6. 建議補 API 優先順序

### P0：先補會直接阻擋大量 testcase 的 public flow

```text
1. WholeDesignEquivalence API
   - 對應 original/current equivalence prompts。
   - 每次 read design 時保存 original snapshot。

已補：technology mapping / basis conversion 目前已整合進 `edit_apply`：
   - ConvertToBasis
   - ReplaceGateType
   - 回傳 NetlistEditReport + mappingDelta。
```

### P1：補現有 query API 的小缺口

```text
3. FunctionQuery::BooleanExpression
   - 程式已有底層，補正式 query/report/usage。

4. ConeQuery summary extension
   - cone gate type counts
   - shared fanin cone gates
   - largest fanin cone output

5. DirectConnectivity usage / tools output
   - DFF clock loads driven by net
   - highest fanout primary input
```

### P2：特殊分析

```text
6. GraphAnalysisQuery
   - cut net
   - articulation points

7. FunctionQuery::Symmetry（已完成）
   - SAT-based input-swap equivalence、bus target、counterexample report。

8. FunctionSearchQuery（NAND pair 與 equivalent gate-pair modes 已完成）
   - existing pair search，例如 NAND(a,b) == target。
   - whole/cone scope 內的 SAT-proven functional-equivalent gate pairs 與 gate-type filter。
   - 實際 functional merge 由 `EditApply::MergeFunctionallyEquivalentGates` 負責，已完成 cycle-safe apply、whole-design SAT 與 rollback。
   - official test38 已確認為 structural duplicates，可由 `MergeStructurallyEquivalentGates` 完整移除 14 gates；general observability-only redundancy search/removal 仍屬 hidden-case hardening。

9. SequentialPatternQuery
   - DFF enable / hold structure detection。
```

### P3：真正最佳化（public flow 完成，large-cone runtime partial）

```text
10. DepthOptimizationApply
    - C++ OptApply::CriticalPathDepth 已完成
    - global/scoped depth + whole/local basis 已完成
    - NetlistEditReport、whole-design SAT、accept/rollback 已完成
    - tools.cpp / TOOL_SPEC / mini test32 已完成
    - test40 official bounded smoke PASS
    - test33 仍需 cone-isolated resynthesis 或 cooperative timeout
```
