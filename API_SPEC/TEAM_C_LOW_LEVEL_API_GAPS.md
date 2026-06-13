# Team C Low-Level API Gaps

這份文件整理 **Team C 應負責的 optimization / mapping strategy API**。

Team C 的邊界是：

```text
決定要最佳化哪裡。
建立 fanout / mapping / depth optimization plan。
選擇 mapping rule。
選擇 rewrite pattern。
呼叫 Team B 的 mutation primitive 來落地。
呼叫 Team A 的 checker / report 來驗證結果。
```

Team C 不應該重做一般 analysis。  
fanout report、scope gate collection、type counting、equivalence、depth comparison 這些應該由 Team A 提供。

所有會實際修改 netlist 的 Team C flow，建議統一回傳 Team A 定義的：

```cpp
NetlistEditReport
```

也就是說，fanout buffering、technology mapping、depth optimization 不要各自定義不同的結果格式。  
差異資訊透過 `NetlistEditReport::fanoutChange`、`mappingDelta`、`depthChange` 等欄位表示。

## 1. 目前已經有的 Team C API

Fanout insertion execution：

```cpp
int insertBuffersForFanout(int maxFanout = 4);
int insertBuffersForSpecificNet(const std::string& wireName, int maxFanout = 4);
int insertBuffersOnEachLoad(const std::string& wireName);
int insertBufferAtDriver(const std::string& wireName);
int insertBufferBeforeGate(const std::string& wireName, const std::string& targetGateName);
int insertBuffersByGateType(GateType type, bool bufferInputs = true, bool bufferOutputs = true);
```

Optimization candidate：

```cpp
OptimizationCandidate buildOptimizationCandidate(const DepthReport& endpoint, int targetDepth) const;
std::vector<OptimizationCandidate> findOptimizationCandidatesExceedingDepth(int targetDepth) const;
std::vector<int> findBufferGatesOnCriticalPath(const OptimizationCandidate& candidate) const;
std::vector<int> findRemovableBufferGatesOnCriticalPath(const OptimizationCandidate& candidate) const;
```

TechMapper：

```cpp
int mapTechnology(...);
int mapTechnologyForCone(...);
int convertToBasis(...);
int convertToAndNot(...);
int convertToOrNot(...);
int convertToNand(...);
int convertToNor(...);
int customMapTechnology(...);
std::vector<TechMapRule> getValidRules(GateType targetGate, const std::vector<GateType>& allowedTypes);
```

目前主要實作檔案：

```text
src/analysis/OptimizationAnalysis.cpp
src/transformation/NetlistTransformation.cpp
include/core/TechMapper.h
```

目前已經有 depth-driven candidate selector，但還缺 plan / apply / validation report 的完整 flow。

## 2. C-1 缺 fanout buffer planning API

目前 insertion 會直接改 netlist，缺修改前 planning。

Team A 應提供：

```cpp
getNetsExceedingFanout()
checkMaxFanoutConstraint()
analyzeControlFanout()
```

Team C 根據 Team A 的分析結果建立 buffer plan。

仍需要補：

```cpp
struct FanoutBufferPlan {
    int sourceNetId;
    int maxFanout;
    int originalLoadCount;
    int expectedBufferCount;
    std::vector<std::vector<int>> loadGroups;
};

FanoutBufferPlan buildFanoutBufferPlan(int netId, int maxFanout) const;
std::vector<FanoutBufferPlan> buildGlobalFanoutBufferPlans(int maxFanout) const;

bool validateFanoutBufferPlan(const FanoutBufferPlan& plan) const;
```

用途：

- 修改前估新增 BUF 數。
- 分配每顆 buffer drive 哪些 loads。
- debug fanout tree。
- 回答 How many BUF gates were added。

## 3. C-2 缺 control-aware fanout buffering strategy

Team A 負責判斷某 net 是否是 clock/reset-like。  
Team C 負責決定 clock/reset 要如何插 buffer。

仍需要補：

```cpp
FanoutBufferPlan buildClockFanoutBufferPlan(int clockNetId, int maxFanout) const;
FanoutBufferPlan buildResetFanoutBufferPlan(int resetNetId, int maxFanout) const;

int applyFanoutBufferPlan(const FanoutBufferPlan& plan);
```

注意：

```text
DFF CK/RN/SN pin 不能混淆。
clock/reset fanout buffer insertion 應避免破壞 DFF pin name 對應。
```

## 4. C-3 缺 mapping rule availability wrapper

目前 TechMapper 已經有底層：

```cpp
getValidRules(targetGate, allowedTypes)
```

但缺對外更直覺的 rule selection API。

仍需要補：

```cpp
bool hasMappingRule(
    GateType targetType,
    const std::vector<GateType>& allowedTypes
) const;

TechMapRule getBestMappingRule(
    GateType targetType,
    const std::vector<GateType>& allowedTypes
) const;

int estimateMappedGateCount(
    GateType targetType,
    const std::vector<GateType>& allowedTypes
) const;
```

用途：

- 確認 XOR -> NAND-only 是否有 rule。
- 確認 XNOR -> NOR-only 是否有 rule。
- 預估新增 gate 數。

## 5. C-4 缺 scope-aware mapping plan

Team A 負責收集 scope gates。  
Team C 負責建立 mapping plan。

仍需要補：

```cpp
struct MappingPlan {
    std::unordered_set<int> scopeGateIds;
    std::vector<int> targetGateIds;
    std::vector<GateType> allowedTypes;
    bool oneToMany;
    int estimatedAddedGates;
};

MappingPlan buildMappingPlan(
    const std::unordered_set<int>& scopeGateIds,
    const std::vector<GateType>& targetTypes,
    const std::vector<GateType>& allowedTypes,
    bool oneToMany
) const;

bool validateMappingPlan(const MappingPlan& plan) const;
```

用途：

- Convert OR gates in cone of n10 to NAND/NOT。
- Replace all XOR gates with NAND gates。
- 預估 mapping 後 gate count。

## 6. C-5 缺 mapping apply / accept wrapper

TechMapper 可以改 netlist，但缺一個完整流程：

```text
snapshot
apply mapping
Team A 檢查 allowed types / equivalence / delta
成功 accept，失敗 rollback
```

仍需要補：

```cpp
NetlistEditReport applyMappingPlanWithValidation(
    Netlist& netlist,
    const MappingPlan& plan
);
```

這個 wrapper 會呼叫：

```text
Team B snapshot / restore
Team C TechMapper
Team A equivalence / basis compliance / NetlistEditReport building blocks
```

## 7. C-6 缺 depth optimization primitive selector

目前能找到 critical path，但缺「找可安全重排的結構」。

仍需要補：

```cpp
struct AssociativeChain {
    GateType type;
    std::vector<int> gateIds;
    std::vector<int> leafNetIds;
    int outputNetId;
};

std::vector<AssociativeChain> findAssociativeChainsInCone(
    const ConeResult& cone,
    GateType type
) const;

std::vector<AssociativeChain> findAssociativeChainsOnCriticalPath(
    const CombinationalPath& path,
    GateType type
) const;

bool isAssociativeChainSafeToRewrite(
    const AssociativeChain& chain
) const;
```

第一版只支援：

```text
AND
OR
XOR
```

不要先支援：

```text
NAND
NOR
XNOR
```

原因是 inversion polarity 會讓重排不安全。

## 8. C-7 缺 balanced tree construction strategy

找到 associative chain 後，要能建立平衡樹。

仍需要補：

```cpp
struct BalancedTreePlan {
    GateType type;
    std::vector<int> leafNetIds;
    int outputNetId;
    std::string namePrefix;
    int expectedDepth;
    int expectedGateCount;
};

BalancedTreePlan buildBalancedTreePlan(
    const AssociativeChain& chain
) const;

int buildBalancedTree(
    GateType type,
    const std::vector<int>& leafNetIds,
    int outputNetId,
    const std::string& namePrefix
);

bool replaceAssociativeChainWithBalancedTree(
    const AssociativeChain& chain
);
```

安全條件：

- chain 中間 net 沒有 cone 外 fanout。
- output net 保留。
- leaf nets 不改。
- old chain gates 移除。

這是第一版能真正降低 unit-depth 的 rewrite primitive。

## 9. C-8 缺 critical-path driven optimization flow

目前有 candidate，但缺完整 optimization loop。

仍需要補：

```cpp
NetlistEditReport optimizeEndpointDepth(
    Netlist& netlist,
    const std::string& endpointNetName,
    int targetDepth
);

NetlistEditReport optimizeAllEndpointsExceedingDepth(
    Netlist& netlist,
    int targetDepth
);
```

流程：

```text
Team A 找 endpoints exceeding depth。
Team C 找 critical path / associative chain。
Team C 建 balanced tree plan。
Team B 執行 rewrite。
Team A 檢查 depth change / equivalence。
失敗則 Team B rollback。
```

## 10. C-9 後期 external tool boundary API

如果後面要接 ABC / mockturtle，需要：

```cpp
bool exportConeToAig(
    const ConeResult& cone,
    const std::string& path
) const;

bool importConeReplacement(
    const std::string& path,
    int originalOutputNetId
);

std::vector<int> getConeBoundaryInputNets(const ConeResult& cone) const;
std::vector<int> getConeOutputNets(const ConeResult& cone) const;
```

這不是第一優先，但若要做真正 resynthesis 會需要。

## 11. Team C 優先順序

P0：

1. `buildFanoutBufferPlan()`
2. `applyFanoutBufferPlan()`
3. `hasMappingRule()` / `getBestMappingRule()`
4. `buildMappingPlan()`
5. `validateMappingPlan()`

P1：

1. `applyMappingPlanWithValidation()`
2. `findAssociativeChainsInCone()`
3. `isAssociativeChainSafeToRewrite()`
4. `buildBalancedTreePlan()`
5. optimization / mapping flow 回傳 `NetlistEditReport`

P2：

1. `buildBalancedTree()`
2. `replaceAssociativeChainWithBalancedTree()`
3. `optimizeEndpointDepth()`
4. `optimizeAllEndpointsExceedingDepth()`

P3：

1. functionally equivalent candidate grouping。
2. NAND pair search candidate generation。
3. cone export/import。
4. ABC / mockturtle bridge。

## 12. Team C 和 Team A/B 的邊界

| 問題 | 負責 |
| --- | --- |
| 找 fanout 違規 net | Team A |
| 設計 fanout buffer grouping | Team C |
| 實際插 buffer 並改線 | Team B / existing transformation |
| 找 cone scope gates | Team A |
| 選 mapping rule | Team C |
| 實際套 mapping | Team C + Team B primitive |
| 檢查 only NAND/NOT remain | Team A |
| 檢查 mapping 後等價 | Team A |
| 統一修改 / 最佳化結果 report 格式 | Team A |
| mapping / optimization flow 填入 report | Team C |
| rollback mapping fail | Team B |
| 找可 balance 的 associative chain | Team C |
| 真正改接 tree 的 net/gate | Team B primitive + Team C strategy |

## 13. 目前測試狀態

```text
mini test/tester.cpp 目前覆蓋 depth/path/function 等 Team C 會依賴的 analysis 基礎。
fanout buffer planning、mapping plan、depth optimization flow 尚未納入 regression tester。
目前 regression：Summary: 45 passed, 0 failed.
```
