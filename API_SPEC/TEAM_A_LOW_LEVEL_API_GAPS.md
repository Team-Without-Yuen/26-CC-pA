# Team A Low-Level API Gaps

這份文件整理 **Team A 應負責的分析類 API**。

Team A 的邊界是：

```text
只讀 netlist，不修改 netlist。
負責 query / classification / checker / report / before-after analysis。
```

也就是說，只要 API 的主要功能是「找出資訊、判斷條件、產生報告」，就應該優先放在 Team A。

Team B 會使用 Team A 的分析結果來安全修改 netlist。  
Team C 會使用 Team A 的分析結果來決定 optimization / mapping 策略。

## 1. 目前已經有的 Team A API

Basic query：

```cpp
int getGateCount() const;
int getNetCount() const;
int getGateId(const std::string& gateName) const;
int getNetId(const std::string& netName) const;
const Gate* findGate(const std::string& gateName) const;
const Net* findNet(const std::string& netName) const;
bool isValidGateId(int gateId) const;
bool isValidNetId(int netId) const;
bool isDffGate(int gateId) const;
bool isCombinationalGate(int gateId) const;
bool isPrimaryInputNet(int netId) const;
bool isPrimaryOutputNet(int netId) const;
bool isConstantNet(int netId) const;
std::vector<std::string> getAllGateNames() const;
std::vector<std::string> getAllNetNames() const;
std::vector<std::string> getPrimaryInputNames() const;
std::vector<std::string> getPrimaryOutputNames() const;
std::vector<std::string> getDffNames() const;
std::vector<int> getGatesByType(GateType type) const;
std::map<GateType, int> countGatesByType() const;
size_t getGateCountByType(GateType type) const;
```

Direct connectivity：

```cpp
std::vector<int> getNetDriverGateIds(const std::string& netName) const;
std::vector<int> getNetLoadGateIds(const std::string& netName) const;
size_t getNetLoadGateCount(const std::string& netName) const;
std::vector<int> getGateFanout(const std::string& gateInstName) const;
size_t getGateFanoutCount(const std::string& gateInstName) const;
std::vector<int> getGateInputNetIds(const std::string& gateInstName) const;
int getGateOutputNetId(int gateId) const;
int getGateInputNetId(int gateId, int pinIndex) const;
int getGateInputNetId(int gateId, const std::string& pinName) const;
int getDffInputNetId(int dffGateId, const std::string& pinName) const;
int getDffOutputNetId(int dffGateId) const;
```

Cone / path / depth：

```cpp
ConeResult getTransitiveFaninCone(const std::string& netName) const;
ConeResult getTransitiveFanoutCone(const std::string& netName) const;
ConeResult getGateTransitiveFaninCone(const std::string& gateName) const;
ConeResult getGateTransitiveFanoutCone(const std::string& gateName) const;
std::vector<int> getConeGateIds(const ConeResult& cone) const;

PathQueryResult runPathQuery(const PathQuery& query) const;

std::vector<int> computeNetLevels() const;
std::vector<int> computeGateLevels() const;
int getMaxDepthToNet(const std::string& netName) const;
DepthReportSet runDepthQuery(const DepthQuery& query) const;
```

Function analysis：

```cpp
bool canNetBeValue(const std::string& netName, int value) const;
bool isNetConstantFunction(const std::string& netName, int constValue) const;
bool isNetAlwaysZero(const std::string& netName) const;
bool isNetAlwaysOne(const std::string& netName) const;
FunctionReport runFunctionQuery(const FunctionQuery& query) const;
```

目前 analysis API 主要分檔：

```text
src/analysis/BasicAnalysis.cpp
src/analysis/ConnectivityAnalysis.cpp
src/analysis/FunctionAnalysis.cpp
src/analysis/ConeAnalysis.cpp
src/analysis/PathAnalysis.cpp
src/analysis/DepthAnalysis.cpp
src/analysis/OptimizationAnalysis.cpp
```

## 2. A-1 缺全域 fanout constraint / report API

這一項不是完全從零開始。  
目前已經可以查單一 net/gate fanout：

```cpp
size_t getNetLoadGateCount(const std::string& netName) const;
size_t getGateFanoutCount(const std::string& gateInstName) const;
```

真正缺的是「掃整個設計」的 global checker / report wrapper。

需要補：

```cpp
int getMaxFanout() const;
int getMaxFanoutOfPrimaryInputs() const;

std::vector<int> getNetsExceedingFanout(int maxFanout) const;
std::vector<std::string> getNetNamesExceedingFanout(int maxFanout) const;

bool checkMaxFanoutConstraint(int maxFanout) const;

std::pair<std::string, int> getPrimaryInputWithHighestFanout() const;
```

用途：

- 回答 maximum fanout。
- 找出 fanout 違規 net。
- 驗證 fanout buffer insertion 是否達成限制。
- 回答 Which primary input has highest fanout?

## 3. A-2 缺 clock/reset-aware fanout classification API

這一項不是完全從零開始。  
目前已經有兩類接近功能：

```cpp
int getDffInputNetId(int dffGateId, const std::string& pinName) const;
int getGateInputNetId(int gateId, const std::string& pinName) const;
std::vector<int> getGatesByType(GateType::DFF) const;
std::vector<int> getNetLoadGateIds(const std::string& netName) const;
```

PathQuery 也已經支援：

```cpp
PathEndpointType::DffClock
PathEndpointType::DffReset
```

所以目前已經能做到：

```text
給定某顆 DFF，解析它的 CK / RN / SN 接到哪條 net。
給定某條 net，查出它直接 load 到哪些 gate。
在 path query 裡把 DFF clock/reset pin 當 startpoint 或 endpoint。
```

真正缺的是「反向分類 wrapper」：

```text
給定某條 net，直接回報它驅動了哪些 DFF 的 CK pin、哪些 DFF 的 RN/SN pin、哪些是一般 combinational load。
```

需要補：

```cpp
std::vector<int> getDffsDrivenByClockNet(int netId) const;
std::vector<int> getDffsDrivenByResetNet(int netId) const;

bool isNetClockLike(int netId) const;
bool isNetResetLike(int netId) const;

struct ControlFanoutInfo {
    int netId;
    std::vector<int> dffClockLoads;
    std::vector<int> dffResetLoads;
    std::vector<int> combinationalLoads;
};

ControlFanoutInfo analyzeControlFanout(int netId) const;
```

用途：

- list all DFFs driven by clock n0。
- list all DFFs driven by reset n1。
- 區分 clock/reset fanout 與一般 combinational fanout。
- 讓 Team C 做 clock/reset buffer planning 時不混淆 DFF pins。

## 4. A-3 缺 gate pin metadata query

目前已經能查 gate input/output 接到哪個 net，但缺「pin name / pin index」之間的查詢 API。

需要補：

```cpp
int getGateInputPinIndex(int gateId, const std::string& pinName) const;
std::string getGateInputPinName(int gateId, int pinIndex) const;

std::vector<std::string> getGateInputPinNames(int gateId) const;
```

用途：

- reconnect input pin A of gate g0 前，先解析 pin A 對應到哪個 index。
- DFF D / CK / RN / SN pin alignment 檢查。
- Team B 做 pin-level rewire 前的 read-only 查詢。

## 5. A-4 缺 scope-aware analysis API

這一項不是完全從零開始。  
目前 cone API 已經可以得到 cone gate IDs：

```cpp
ConeResult getTransitiveFaninCone(const std::string& netName) const;
ConeResult getTransitiveFanoutCone(const std::string& netName) const;
ConeResult getGateTransitiveFaninCone(const std::string& gateName) const;
ConeResult getGateTransitiveFanoutCone(const std::string& gateName) const;
std::vector<int> getConeGateIds(const ConeResult& cone) const;
```

真正缺的是把 `TechMapper::TargetScope + name` 統一解析成 scope gate set 的正式 wrapper。

需要補：

```cpp
std::unordered_set<int> collectGateIdsInScope(
    TechMapper::TargetScope scope,
    const std::string& name
) const;

bool isGateInsideScope(
    int gateId,
    TechMapper::TargetScope scope,
    const std::string& name
) const;
```

用途：

- cone of n10。
- fanin cone of output n8。
- OR gates in cone of n11[0]。
- 提供 Team C mapping / optimization 的合法操作範圍。

## 6. A-5 缺 scope-aware gate type checker

這一項不是完全從零開始。  
目前有全域 gate type query：

```cpp
std::vector<int> getGatesByType(GateType type) const;
std::map<GateType, int> countGatesByType() const;
size_t getGateCountByType(GateType type) const;
```

真正缺的是「限定在 scope gate set 裡」的 type filter / count wrapper。

需要補：

```cpp
std::vector<int> getGateIdsByTypeInScope(
    GateType targetType,
    const std::unordered_set<int>& scopeGateIds
) const;

std::vector<int> getGateIdsByTypesInScope(
    const std::vector<GateType>& targetTypes,
    const std::unordered_set<int>& scopeGateIds
) const;

bool hasGateTypeInScope(
    GateType targetType,
    const std::unordered_set<int>& scopeGateIds
) const;

std::map<GateType, int> countGateTypesInScope(
    const std::unordered_set<int>& scopeGateIds
) const;
```

用途：

- list all XOR gates in scope。
- count NAND gates in cone。
- 找出需要被 mapping 的 gate type。

## 7. A-6 缺 basis compliance checker

這一項是 A-5 的延伸，不是完全從零開始。  
只要有 scope gate set 與 gate type query，就能檢查 scope 內是否只剩允許的 gate type。

真正缺的是正式的 allowed-type compliance wrapper。

需要補：

```cpp
bool checkAllowedGateTypesInScope(
    const std::unordered_set<int>& scopeGateIds,
    const std::vector<GateType>& allowedTypes
) const;

std::vector<int> findDisallowedGatesInScope(
    const std::unordered_set<int>& scopeGateIds,
    const std::vector<GateType>& allowedTypes
) const;
```

用途：

- 確認 scope 內只使用 AND/NOT。
- 確認 XOR-to-NAND 後沒有 XOR。
- 確認 NOR-only conversion 後沒有非 NOR gate。

## 8. A-7 NetlistEditReport building block：stats / diff

這不是另一套獨立 report，而是 `NetlistEditReport` 需要用到的基礎統計資料。

這一項不是完全從零開始。  
目前有基本 count：

```cpp
int getGateCount() const;
int getNetCount() const;
std::map<GateType, int> countGatesByType() const;
```

真正缺的是把這些 count 包成 `NetlistStats`，以及計算 before/after 差異的 `NetlistDiff`。

需要補：

```cpp
struct NetlistStats {
    int gateCount;
    int netCount;
    std::map<GateType, int> gateTypeCount;
};

NetlistStats collectNetlistStats() const;

struct NetlistDiff {
    int addedGates;
    int removedGates;
    int addedNets;
    int removedNets;
    std::map<GateType, int> gateTypeDelta;
};

NetlistDiff diffStats(
    const NetlistStats& before,
    const NetlistStats& after
);
```

用途：

- How many gates were removed?
- How many BUF gates were added?
- How many NAND gates are now in the cone?
- 填入 `NetlistEditReport::beforeStats`、`afterStats`、`diff`。

## 9. A-8 NetlistEditReport building block：mapping delta

這不是另一套獨立 mapping report，而是 `NetlistEditReport::mappingDelta` 的資料來源。

這一項不是完全從零開始。  
它可以建立在 A-4 scope wrapper、A-5 scope type count、A-7 stats/diff 上。

真正缺的是 mapping 專用的 scope-aware delta 結構。

需要補：

```cpp
struct MappingDelta {
    int removedTargetGates;
    int addedGates;
    std::map<GateType, int> beforeTypeCount;
    std::map<GateType, int> afterTypeCount;
    std::map<GateType, int> typeDelta;
};

MappingDelta computeMappingDelta(
    const Netlist& before,
    const Netlist& after,
    const std::unordered_set<int>& originalScope
);
```

用途：

- mapping 前後 gate type count。
- 回答新增多少 NAND / NOR。
- Team C 評估 mapping 是否符合 prompt。
- 填入 `NetlistEditReport::mappingDelta`。

## 10. A-9 NetlistEditReport building block：depth change

這不是另一套獨立 depth report，而是 `NetlistEditReport::depthChange` 的資料來源。

這一項不是完全從零開始。  
目前已經有 depth 計算：

```cpp
std::vector<int> computeNetLevels() const;
std::vector<int> computeGateLevels() const;
int getMaxDepthToNet(const std::string& netName) const;
DepthReportSet runDepthQuery(const DepthQuery& query) const;
```

真正缺的是 before/after depth comparison wrapper。

需要補：

```cpp
struct DepthChange {
    int beforeDepth;
    int afterDepth;
    bool improved;
    bool meetsTarget;
};

DepthChange evaluateDepthChange(
    const Netlist& before,
    const Netlist& after,
    const std::string& endpointNetName,
    int targetDepth
);

bool isDepthNoWorse(
    const Netlist& before,
    const Netlist& after,
    const std::string& endpointNetName
);
```

用途：

- 判斷 depth optimization 是否真的改善。
- 判斷是否達到 target depth。
- Team C 決定 accept / rollback。
- 填入 `NetlistEditReport::depthChange`。

## 11. A-10 缺 whole-design equivalence checker

這一項不是完全從零開始。  
目前已有 signal-level SAT-based function analysis：

```cpp
bool areNetsEquivalent(const std::string& netA, const std::string& netB) const;
FunctionReport runFunctionQuery(const FunctionQuery& query) const;
```

真正缺的是 whole-design wrapper：

```text
逐一比較所有 PO。
必要時比較所有 DFF.D sequential boundary endpoint。
彙整成 transformation 後的 equivalence validation。
```

需要補：

```cpp
bool checkPrimaryOutputEquivalence(
    const Netlist& golden,
    const Netlist& revised
);

bool checkSequentialBoundaryEquivalence(
    const Netlist& golden,
    const Netlist& revised
);

bool checkWholeDesignEquivalence(
    const Netlist& golden,
    const Netlist& revised
);
```

第一版語意：

```text
比較所有 PO function。
DFF.Q 視為 pseudo PI。
DFF.D 視為 combinational endpoint。
不跨 clock cycle。
```

用途：

- transformation 後驗證 functional equivalence。
- Team B/C 修改後的 accept / rollback 依據。
- 填入 `NetlistEditReport::validation.functionallyEquivalent`。

## 12. A-11 缺 writable / live object classification API

這一項不是完全從零開始。  
目前已有 removed gate / structural validation 相關能力：

```cpp
bool isGateRemoved(int gateId) const;
bool validateStructure() const;
bool validateProblemAConstraints() const;
```

真正缺的是 writer 使用的 live/writable object classification wrapper。

需要補：

```cpp
bool isNetWritable(int netId) const;
bool isGateWritable(int gateId) const;
std::vector<int> getWritableGateIds() const;
std::vector<int> getWritableNetIds() const;
```

用途：

- 避免 writer 輸出 removed gate。
- 避免 invalid / unused net 造成不合法 Verilog。
- 幫 Team B/C 檢查修改後狀態。

## 13. A-12 缺 cleanup candidate finder 的整理

這一項大部分已經有了。  
目前已有一些 finder，但它們散在 optimization / cleanup 區塊。建議明確歸類為 A 的 analysis candidate API。

已經有：

```cpp
std::vector<int> findAllRemovableBufferGates() const;
std::vector<std::pair<int,int>> findDoubleInverterPairs() const;
std::vector<int> findSameInputGates() const;
std::vector<int> findDanglingGateIds() const;
std::vector<std::vector<int>> findStructurallyEquivalentGateGroups() const;
```

建議補齊或整理：

```cpp
std::vector<int> findGatesSimplifiableWithConstants() const;
std::vector<int> findDoubleBufferPairs() const;
```

用途：

- Team A 找候選。
- Team B 執行實際 cleanup。

## 14. A-13 缺統一修改 / 最佳化結果 report

目前 cleanup、mapping、fanout buffer insertion、depth optimization 都可能需要回報：

```text
是否成功
是否真的有修改
是否 rollback
修改前後 gate/net 數量
修改前後 gate type delta
修改後 structural / Problem A constraint 是否合法
修改後是否 functionally equivalent
depth / fanout / mapping 是否達成 prompt 要求
```

這一項比較接近真正新的共同資料結構。  
目前已有很多 report building blocks，但還沒有一個統一承載所有修改 / 最佳化結果的共同格式。

如果 B/C 各自定義 report，LLM 呼叫與回答會變得很分散。建議由 Team A 定義共同資料格式，並提供填 report 需要的 before/after analysis helper。

前面的 stats / diff、mapping delta、depth change、fanout change 都是這份 report 的 building blocks，不應視為獨立 report 系統。

需要補：

```cpp
struct FanoutChange {
    int beforeMaxFanout = -1;
    int afterMaxFanout = -1;
    int targetFanout = -1;
    bool meetsConstraint = false;
    std::vector<std::string> violatingNetNames;
};

struct EditValidationResult {
    bool structureValid = false;
    bool problemAConstraintValid = false;
    bool functionallyEquivalent = false;
};

struct NetlistEditReport {
    bool success = false;
    bool changed = false;
    bool rolledBack = false;

    std::string operationName;
    std::string message;

    NetlistStats beforeStats;
    NetlistStats afterStats;
    NetlistDiff diff;

    EditValidationResult validation;

    std::optional<DepthChange> depthChange;
    std::optional<FanoutChange> fanoutChange;
    std::optional<MappingDelta> mappingDelta;

    std::vector<std::string> changedGateNames;
    std::vector<std::string> changedNetNames;
    std::vector<std::string> warnings;
};

NetlistEditReport buildEditReport(
    const Netlist& before,
    const Netlist& after,
    const std::string& operationName
);

EditValidationResult validateEditResult(
    const Netlist& before,
    const Netlist& after
);
```

分工語意：

```text
Team A：定義 NetlistEditReport，負責計算 stats / diff / equivalence / fanout / depth / mapping delta。
Team B：執行 mutation / cleanup，最後回傳或填入 NetlistEditReport。
Team C：執行 optimization / mapping flow，最後回傳或填入 NetlistEditReport。
```

用途：

- cleanup 做了什麼。
- mapping 新增 / 移除了多少 gate。
- fanout buffer insertion 是否滿足 max fanout。
- depth optimization 是否真的改善。
- transformation 後是否 functionally equivalent。
- 失敗時是否 rollback。

## 15. Team A 優先順序

P0：

1. `checkWholeDesignEquivalence()`
2. `NetlistEditReport` / `buildEditReport()`
3. `collectNetlistStats()` / `diffStats()`
4. `getMaxFanout()` / `getNetsExceedingFanout()` / `checkMaxFanoutConstraint()`
5. `collectGateIdsInScope()`
6. `getGateIdsByTypeInScope()`

P1：

1. `getGateInputPinIndex()` / `getGateInputPinName()`
2. `checkAllowedGateTypesInScope()`
3. `computeMappingDelta()`
4. `evaluateDepthChange()`
5. `analyzeControlFanout()`

P2：

1. writable object classification。
2. cleanup candidate finder 整理。
3. more detailed report formatting。

## 16. 目前測試狀態

```text
mini test/tester.cpp 已覆蓋主要 high-level analysis API。
目前 regression：Summary: 45 passed, 0 failed.
```
