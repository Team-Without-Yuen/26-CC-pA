# Depth Optimization Strategy

這份文件整理針對 testcase 中 depth optimization / restructuring 類 prompt 的整體策略。重點是：如何在 preserve functional equivalence、basis constraint、cost function 都存在的情況下，安全地做 critical path depth reduction。

---

## 1. 目標 Prompt 類型

代表 prompt：

```text
Reduce the critical path depth through restructuring.
Make sure nothing changes functionally.
The cost function is the maximum logic depth of the final design; smaller is better.
```

```text
Reduce critical path depth through logic restructuring,
ensuring the cone of n10 maintains only NOR and NOT gates.
Preserve functional equivalence.
Report original if already optimal.
The cost function is the maximum logic depth of the final design; smaller is better.
```

```text
Optimize the logic to minimize maximum path depth,
ensuring the cone of n15 contains only AND, OR, and NOT gates.
Make sure nothing changes functionally.
The cost function is the maximum logic depth of the final design; smaller is better.
```

```text
Minimize the maximum logic depth of the design,
ensuring the netlist remains AND and NOT only.
Make sure nothing changes functionally.
The cost function is the maximum logic depth of the final design; smaller is better.
```

---

## 2. 核心策略：Target-Basis-First

如果 prompt 有 gate basis constraint，採用：

```text
先轉成題目要求的 target basis，
再在 target basis 裡做 depth optimization。
```

也就是：

```text
Original netlist
  -> map scope to target basis
  -> validate basis constraint
  -> validate equivalence
  -> optimize depth using target-basis-preserving rewrites
  -> validate basis constraint again
  -> validate equivalence again
  -> report final depth
```

不建議單純採用：

```text
Original netlist
  -> convert to AIG
  -> optimize AIG depth
  -> convert to required basis at the end
```

原因是 AIG depth 變小，不代表轉成 NAND/NOT、NOR/NOT、AND/OR/NOT 後 final depth 也會變小。題目評分看的是 final design 的 maximum logic depth，所以 optimization 過程中的 cost model 應盡量和 final representation 一致。

---

## 3. 為什麼先轉 Target Basis

### 3.1 Cost Function 一致

題目 cost function 通常是：

```text
maximum logic depth of the final design
```

如果先在 AIG 或其他 internal representation 裡最佳化，最後再 mapping 到 target basis，depth 可能改變。這會讓 optimization 階段看到的 cost 和題目真正評分的 cost 不一致。

Target-basis-first 的好處是：

```text
評估 rewrite candidate 時看到的 depth，
就是最後會輸出的 gate basis depth。
```

### 3.2 Hard Requirement 比較清楚

例如：

```text
ensuring the cone of n10 maintains only NOR and NOT gates
```

如果先轉 NOR/NOT，再只做 NOR/NOT-preserving rewrite，每一步都能維持 hard requirement。

反之，如果中間轉成 AIG 或其他 basis，就需要額外證明：

```text
最後 mapping 回來後 target cone 仍滿足 requirement。
```

雖然可以做到，但 report 和 validation 都比較複雜。

### 3.3 Report 比較好解釋

Target-basis-first 的 report 可以清楚寫成：

```text
original depth = X
after target-basis mapping depth = Y
after target-basis-preserving optimization depth = Z
basis constraint valid = true
functional equivalence = true
```

這比「先在 AIG 裡優化，再轉回 target basis」更容易跟 professor / judge 解釋。

---

## 4. Prompt 分類與策略

### 4.1 無 Basis Constraint

Prompt:

```text
Reduce the critical path depth through restructuring.
```

策略：

```text
scope = whole design
target_basis = unrestricted
cost = global maximum logic depth
```

可使用較自由的 internal representation 或 existing netlist basis 產生 rewrite candidate。最後只要求：

```text
functional equivalence preserved
final max depth improved or no worse
```

第一版可先做 conservative rewrites：

```text
cleanup buffers
collapse double inverters
constant propagation
same-input simplification
structural duplicate merge
local simplification fixpoint
```

### 4.2 NOR/NOT Cone Constraint

Prompt:

```text
cone of n10 maintains only NOR and NOT gates
```

策略：

```text
scope = cone(n10)
target_basis = NOR_NOT
cost = global maximum logic depth 或 cone depth，依 prompt 決定
```

流程：

```text
1. map cone(n10) to NOR/NOT
2. validate cone(n10) only contains NOR/NOT
3. validate functional equivalence
4. run NOR/NOT-preserving depth rewrites
5. re-check cone basis and equivalence
```

### 4.3 AND/OR/NOT Cone Constraint

Prompt:

```text
cone of n15 contains only AND, OR, and NOT gates
```

策略：

```text
scope = cone(n15)
target_basis = AND_OR_NOT
cost = global maximum logic depth 或 cone depth，依 prompt 決定
```

流程：

```text
1. map cone(n15) to AND/OR/NOT
2. validate cone(n15) only contains AND/OR/NOT
3. validate functional equivalence
4. run AND/OR/NOT-preserving depth rewrites
5. re-check cone basis and equivalence
```

### 4.4 AND/NOT Whole Design Constraint

Prompt:

```text
netlist remains AND and NOT only
```

策略：

```text
scope = whole design
target_basis = AND_NOT
cost = global maximum logic depth
```

AND/NOT 和 AIG-like representation 很接近，但對外 report 建議仍稱為：

```text
target basis = AND_NOT
```

而不是直接說 AIG，因為題目要求的是 gate-level netlist。

---

## 5. 建議 Flow

完整流程：

```text
1. Parse optimization prompt
2. Snapshot original netlist
3. Determine scope
4. Determine target basis
5. Determine cost function
6. If needed, run TechnologyApply to target basis
7. Validate basis constraint
8. Validate functional equivalence
9. Run DepthQuery before optimization
10. Build optimization candidates from critical path / critical cone
11. Generate target-basis-preserving rewrite candidates
12. Evaluate each candidate by final-basis depth
13. Accept only equivalent and improving candidate
14. Iterate until no improvement or limit reached
15. Run final validation
16. Return optimization report
```

以 pseudo flow 表示：

```text
OptApplyRequest
  -> parse scope / basis / cost
  -> clone original
  -> TechnologyApply(scope, targetBasis)
  -> BasisConstraintValidator(scope, targetBasis)
  -> WholeDesignEquivalence or local equivalence certificate
  -> DepthQuery(GlobalCriticalPath or scope depth)
  -> CandidateBuilder
  -> TargetBasisRewriteEngine
  -> CandidateEvaluator
  -> accept / rollback
  -> NetlistEditReport or OptApplyReport
```

---

## 6. 需要的 API 元件

### 6.1 TechnologyApply

把現有 TechMapper wrapper 包成 public apply API。

建議 request：

```text
scope = whole_design / cone
scopeName = net/output name
targetBasis = AND_NOT / OR_NOT / NAND_NOT / NOR_NOT / AND_OR_NOT
sourceGateFilter = all / XOR / XNOR / OR2 / NAND2_CONST1
validateEquivalence = true
rollbackOnFailure = true
```

回傳：

```text
NetlistEditReport
mappingDelta
changedGateNames
changedNetNames
basisConstraintResult
equivalence result
```

### 6.2 BasisConstraintValidator

用途：

```text
確認 whole design 或指定 cone 是否只包含允許 gate types。
```

例子：

```text
validateBasis(scope = cone(n10), allowed = {NOR, NOT})
validateBasis(scope = whole_design, allowed = {AND, NOT})
```

回傳應包含：

```text
valid
scope
allowedGateTypes
violatingGateNames
violatingGateTypes
```

### 6.3 WholeDesignEquivalence

目前 `NetlistEditReport` 已預留：

```text
EquivalenceCheckMethod::WholeDesignSat
```

但還沒有正式接入 original/current design equivalence。

Depth optimization 類 prompt 幾乎都要求：

```text
Make sure nothing changes functionally.
Preserve functional equivalence.
```

因此後續需要：

```text
checkCurrentEquivalentToOriginal()
checkBeforeAfterEquivalent()
```

### 6.4 DepthOptimizationApply

建議不要一開始承諾 global optimal。第一版可以是：

```text
conservative depth improvement flow
```

回傳應包含：

```text
beforeDepth
afterMappingDepth
afterOptimizationDepth
improved
candidateTriedCount
candidateAcceptedCount
basisConstraintValid
equivalenceChecked
functionallyEquivalent
message
```

---

## 7. Rewrite Candidate 原則

每個 basis 都應有自己的 rewrite rule set。

### 7.1 AND/NOT

可做：

```text
double negation elimination
constant propagation
same-input simplification
AND tree balancing
common subexpression structural merge
De Morgan normalization
```

### 7.2 NOR/NOT

可做：

```text
double negation elimination
constant propagation
NOR tree balancing where legal
merge structurally equivalent NOR cones
local De Morgan rewrites that end in NOR/NOT
```

### 7.3 NAND/NOT

可做：

```text
double negation elimination
constant propagation
NAND tree balancing where legal
merge structurally equivalent NAND cones
local De Morgan rewrites that end in NAND/NOT
```

### 7.4 AND/OR/NOT

可做：

```text
AND/OR tree balancing
constant propagation
same-input simplification
De Morgan push / pull when depth improves
structural duplicate merge
```

---

## 8. Candidate Evaluation

每個 candidate 都要通過：

```text
1. structure valid
2. Problem A constraints valid
3. basis constraint valid
4. functional equivalence valid
5. depth no worse, preferably improved
```

評分：

```text
primary cost = maximum logic depth of final design
secondary cost = gate count
third cost = changed object count / rewrite size
```

接受規則第一版可保守：

```text
accept if:
  functionallyEquivalent == true
  basisConstraintValid == true
  afterDepth < beforeDepth

otherwise rollback candidate
```

若 prompt 允許 no-worse optimization，可放寬為：

```text
afterDepth <= beforeDepth && gateCount improves
```

但目前 testcase 的 cost function 幾乎都是 depth smaller is better，所以第一版以 depth improvement 為主。

---

## 9. Report Original If Already Optimal

這句很難嚴格證明：

```text
Report original if already optimal.
```

除非：

```text
1. cone 很小，可以 exhaustive search；
2. 或有 formal lower bound proof；
3. 或演算法能完整枚舉 target basis 下所有等價實作。
```

第一版建議不要聲稱：

```text
globally optimal
```

而是回：

```text
No improving equivalent rewrite was found under the current strategy.
The original/current implementation is kept.
```

若要在報告中寫得保守：

```text
若目前策略找不到更小 depth 的等價 candidate，系統會保留原設計，
並回報 no improving candidate found，而不宣稱已形式化證明 global optimal。
```

---

## 10. 和現有 API 的關係

目前已可直接使用：

```text
DepthQuery
EditApply safe cleanup / simplification
NetlistEditReport
TechMapper WithReport wrappers
FunctionQuery signal equivalence
```

目前缺少：

```text
public TechnologyApply API
BasisConstraintValidator
whole-design equivalence API
DepthOptimizationApply / OptApply 強化版
candidate-specific rollback / report
```

因此建議實作順序：

```text
1. TechnologyApply
2. BasisConstraintValidator
3. WholeDesignEquivalence
4. Conservative DepthOptimizationApply
5. Target-basis-specific rewrite rules
6. More complete optimality / search strategy
```

---

## 11. 對四個代表 Prompt 的處理

### Prompt A

```text
Reduce the critical path depth through restructuring.
```

流程：

```text
DepthQuery::GlobalCriticalPath
-> conservative rewrites
-> DepthQuery again
-> equivalence validation
-> report before/after depth
```

### Prompt B

```text
Reduce critical path depth through logic restructuring,
ensuring the cone of n10 maintains only NOR and NOT gates.
```

流程：

```text
TechnologyApply(scope = cone(n10), targetBasis = NOR_NOT)
-> validate cone basis
-> equivalence validation
-> DepthQuery
-> NOR/NOT-preserving rewrites
-> validate cone basis again
-> equivalence validation
-> report final max depth
```

### Prompt C

```text
Optimize the logic to minimize maximum path depth,
ensuring the cone of n15 contains only AND, OR, and NOT gates.
```

流程：

```text
TechnologyApply(scope = cone(n15), targetBasis = AND_OR_NOT)
-> validate cone basis
-> equivalence validation
-> AND/OR/NOT-preserving rewrites
-> DepthQuery
-> report before/after depth
```

### Prompt D

```text
Minimize the maximum logic depth of the design,
ensuring the netlist remains AND and NOT only.
```

流程：

```text
TechnologyApply(scope = whole_design, targetBasis = AND_NOT)
-> validate whole design basis
-> equivalence validation
-> AND/NOT-preserving depth optimization
-> DepthQuery::GlobalCriticalPath
-> report final depth
```

---

## 12. 總結

建議採用：

```text
Target-Basis-First Optimization Flow
```

核心原則：

```text
有 target basis constraint 時，先轉到 target basis。
所有 rewrite candidate 都必須維持 target basis。
所有 cost evaluation 都用 final target basis depth。
所有 apply 都必須通過 equivalence validation。
沒有 formal proof 時，不聲稱 global optimal，只回報 no improving candidate found。
```
