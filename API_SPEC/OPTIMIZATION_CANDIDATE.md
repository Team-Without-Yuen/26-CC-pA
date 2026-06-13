# Optimization Candidate API

這份文件記錄 depth optimization 前的 candidate selector。

目前這一層只整理資料，不直接修改 netlist。

---

## 1. 目的

DepthAnalysis 可以回答：

```text
哪些 endpoint depth 超過 target？
critical path 是哪一條？
```

OptimizationCandidate 則把這些資訊整理成後續 rewrite pass 可直接使用的格式：

```text
endpoint
targetDepth
faninCone
criticalPath
```

---

## 2. Result / Candidate 資料結構

```cpp
struct OptimizationCandidate {
    DepthReport endpoint;
    int targetDepth = -1;
    ConeResult faninCone;
    CombinationalPath criticalPath;
};

struct OptimizationResult {
    bool changed = false;
    bool depthImproved = false;
    bool equivalenceChecked = false;
    bool equivalent = false;
    int oldDepth = -1;
    int newDepth = -1;
    std::string passName;
    std::string message;
};
```

欄位用途：

```text
endpoint     : 要最佳化的 endpoint，例如 y 或 ff1.D。
targetDepth  : 希望最佳化後達到的 depth 上限。
faninCone    : endpoint 的 transitive fanin cone，是 rewrite 的主要搜尋範圍。
criticalPath : endpoint 目前的一條最長路徑，是優先處理的方向。
```

`OptimizationResult` 目前先作為後續 rewrite pass 的共用回傳格式，現在尚未用來修改 netlist。

---

## 3. Candidate API

### 3.1 buildOptimizationCandidate

```cpp
OptimizationCandidate buildOptimizationCandidate(
    const DepthReport& endpoint,
    int targetDepth) const;
```

用途：

```text
把單一 DepthReport 轉成 OptimizationCandidate。
```

這個 function 會：

```text
1. 保留 endpoint metadata。
2. 設定 targetDepth。
3. 取得 endpoint fanin cone。
4. 保留或重新取得 critical path。
```

---

### 3.2 findOptimizationCandidatesExceedingDepth

```cpp
std::vector<OptimizationCandidate>
findOptimizationCandidatesExceedingDepth(int targetDepth) const;
```

用途：

```text
找出所有 depth > targetDepth 的 timing endpoints，並轉成 optimization candidates。
```

endpoint 範圍目前包含：

```text
Primary Output
DFF.D
```

---

## 4. Target Selection API

這一層只負責找出「可能可以處理的 gate」，不修改 netlist。

---

### 4.1 findBufferGatesOnCriticalPath

```cpp
std::vector<int> findBufferGatesOnCriticalPath(
    const OptimizationCandidate& candidate) const;
```

用途：

```text
找出 candidate critical path 上的 BUF gate。
```

注意：

```text
這個 function 只偵測，不修改 netlist。
後續 buffer cleanup pass 可以用它決定優先處理哪些 gate。
```

---

### 4.2 findRemovableBufferGatesOnCriticalPath

```cpp
std::vector<int> findRemovableBufferGatesOnCriticalPath(
    const OptimizationCandidate& candidate) const;
```

用途：

```text
從 critical path 上的 BUF gate 中，篩出第一版可安全 bypass 的 BUF gate。
```

第一版安全條件：

```text
1. gate type 必須是 BUF
2. BUF 必須剛好 1 input
3. input net / output net 都有效
4. output net 不是 Primary Output
5. output net 不是 constant
6. input net != output net
7. output net 至少有一個 load gate
```

mini circuit 預期：

```text
findBufferGatesOnCriticalPath(yCandidate)
=> g_buf, g_y

findRemovableBufferGatesOnCriticalPath(yCandidate)
=> g_buf
```

---

## 5. Rewrite / Validation API 規劃

目前 rewrite / validation 的底層 building blocks 已部分存在：

```cpp
bool validateStructure() const;
bool validateProblemAConstraints() const;
Netlist cloneForRollback() const;
bool replaceAllLoadsOfNet(int oldNetId, int newNetId);
bool bypassBufferGate(int bufGateId);
std::vector<int> findAllRemovableBufferGates() const;
int cleanupAllRemovableBuffers();
```

仍缺的是更高階、帶 report 的 candidate-driven rewrite wrapper，例如：

```cpp
// NetlistEditReport cleanupBufferChain(const OptimizationCandidate& candidate);
```

---

## 6. 後續接法

後續 optimization pass 可以照這個流程使用：

```text
1. candidates = findOptimizationCandidatesExceedingDepth(targetDepth)
2. 對每個 candidate 檢查 faninCone / criticalPath
3. 套用保守 rewrite
4. 重新計算 depth
5. 跑 LEC / SAT equivalence check
6. 成功則保留，失敗則 rollback
```

---

## 7. 目前暫不處理

第一版 candidate selector 先不做：

```text
多個 candidate 的衝突處理
rewrite 成本估計
area / gate count tradeoff
candidate 排序策略
rollback transaction
```

---

## 8. 目前實作與測試狀態

```text
實作檔案：src/analysis/OptimizationAnalysis.cpp
型別檔案：include/core/OptimizationTypes.h
相關 cleanup 實作：src/optimization/NetlistOptimization.cpp
tester：mini test/tester.cpp
目前 regression：Summary: 45 passed, 0 failed.
```

目前 tester 已間接覆蓋 depth / critical path / optimization candidate 的輸入條件，但尚未完整覆蓋所有 cleanup pass。
