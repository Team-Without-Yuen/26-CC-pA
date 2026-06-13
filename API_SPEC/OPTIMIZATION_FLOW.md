# Optimization Flow

這份文件整理 Problem A backend engine 後續的 optimization 流程。

目前策略：

```text
先做本地安全最佳化。
再做本地 depth-oriented 最佳化。
最後才考慮外部工具。
```

---

## 1. 總體原則

最佳化不應該是直接改 netlist 後就結束，而應該包在一個可驗證流程裡：

```text
1. 建立 snapshot / rollback point
2. 選擇 optimization candidate
3. 套用一個 pass
4. structural validation
5. constraint validation
6. function validation
7. 重新計算 depth / cost
8. accept 或 rollback
```

也就是：

```text
rollback / verification framework
```

不是單一 pass，而是所有 transformation pass 的外層 wrapper。

---

## 2. Stage A：本地安全化簡

這一階段的目標是：

```text
保守、可控、容易驗證。
先讓 netlist 變乾淨，不追求複雜重寫。
```

建議 pass：

| 順序 | Pass | 目前建議 | 說明 |
|---:|---|---|---|
| 1 | dangling logic removal | 延後一點 | 需要確認哪些 gate 不影響 PO / DFF.D。 |
| 2 | buffer simplification | 先做 | 最適合作為第一個 rewrite pass。 |
| 3 | double buffer removal | 合併到 buffer cleanup | 可視為 buffer simplification 的特例。 |
| 4 | double inverter removal | 第二批做 | 規則簡單，但要小心 PO / fanout。 |
| 5 | constant propagation | 第二批做 | 對 primitive gates 很有用。 |
| 6 | same-input simplification | 第二批做 | 例如 and(a,a)=a、xor(a,a)=0。 |
| 7 | structural hashing | 第三批做 | 可以合併重複邏輯，但要有穩定 key。 |

第一個建議實作：

```text
buffer cleanup
```

原因：

```text
規則簡單。
容易測試。
容易看出 depth 是否下降。
適合把 rewrite -> validate -> accept / rollback 流程跑起來。
```

---

## 3. Stage B：本地 Depth-Oriented 最佳化

這一階段才真正針對：

```text
Reduce critical path depth
target depth 4
optimize cone
```

建議 pass：

| 順序 | Pass | 目前建議 | 說明 |
|---:|---|---|---|
| 8 | local algebraic rewrite | 等 Stage A 穩定後 | 例如 De Morgan 或局部等價轉換。 |
| 9 | associative chain balancing | 重要 | AND / OR / XOR chain balancing 可直接降 depth。 |
| 10 | critical-path driven rewrite | 重要 | 只針對 candidate critical path 附近重寫。 |
| 13 | cone gate-count minimization | 延後 | 可能和 depth 目標衝突。 |
| 14 | cone depth minimization | 延後但重要 | 可能需要更完整的 cone rewriting。 |

注意：

```text
NAND / NOR / XNOR 有 inversion polarity 問題。
不能直接當作 AND / OR / XOR 任意重排。
```

---

## 4. Stage C：目前先延後的 Buffer / Physical 類 Pass

這幾項目前不建議太早做：

| Pass | 建議 | 原因 |
|---|---|---|
| fanout buffer tree | 延後 | 題目目前沒有 real load / capacitance / slew model。 |
| depth balancing buffer insertion | 延後 | 可能增加 gate count，且不一定改善 unit-depth critical path。 |

這些比較像 physical-aware 或 timing-library-aware 的 optimization。
Problem A 第一版沒有 real delay model 時，先不放前面。

---

## 5. Stage D：外部工具整合

外部工具目前先保留為後續強化選項。

可能工具：

| 工具 | 適合用途 | 代價 |
|---|---|---|
| ABC | AIG rewrite / balance / refactor | 要做 cone 匯出與匯入。 |
| mockturtle | C++ logic network optimization | 要轉成它的 network model。 |
| Yosys | Verilog synthesis flow | 較重，且會和自訂 parser / engine 重疊。 |

建議時機：

```text
本地 safe pass 和 validation framework 穩定後，
再接 external small-cone resynthesis。
```

外部工具相關流程：

```text
18. external small-cone resynthesis
19. external equivalence check
20. accept / rollback decision
```

---

## 6. 驗證流程

每個 transformation pass 後都應該檢查：

| 驗證 | 目的 |
|---|---|
| structural validation | 檢查 net / gate graph 是否一致。 |
| constraint validation | 檢查是否違反題目限制，例如 DFF boundary、primitive gates。 |
| function validation | 用 LEC / SAT 確認功能等價。 |

第一版可以先做：

```text
structural validation
function validation
```

constraint validation 可以先以保守 rewrite 規則代替，後續再獨立成 API。

---

## 7. 第一版建議實作順序

目前已完成：

```text
DepthAnalysis
OptimizationCandidate
findBufferGatesOnCriticalPath()
findRemovableBufferGatesOnCriticalPath()
validateStructure()
validateProblemAConstraints()
cloneForRollback()
replaceAllLoadsOfNet()
bypassBufferGate()
findAllRemovableBufferGates()
cleanupAllRemovableBuffers()
```

下一步：

```text
1. 設計統一 NetlistEditReport / OptimizationEditReport。
2. 補 cleanupAllRemovableBuffers() 的 regression test。
3. 包 candidate-driven cleanupBufferChain() dry-run / apply wrapper。
4. 統一 structural validation + function validation + rollback flow。
5. 將 accept / rollback decision 寫成固定流程，而不是散落在 pass 裡。
```

第一版 buffer cleanup 的保守條件：

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

因為：

```text
g_buf output = n_buf，不是 PO，可先視為 removable。
g_y output = y，是 PO，第一版先保守跳過。
```

目前測試狀態：

```text
mini test/tester.cpp 已覆蓋 analysis / path / depth / function API 與少量 mutation primitive。
cleanup / optimization pass 尚需補專門 regression test。
目前 regression：Summary: 45 passed, 0 failed.
```

---

## 8. 128GB 記憶體下的設計提示

題目環境提供 128GB memory，因此可以合理使用空間換時間：

```text
net level cache
gate level cache
fanin / fanout cone cache
path query memo
optimization candidate cache
bitset / vector-based membership table
```

但 transformation 之後要注意：

```text
cache 必須 invalidate。
不能拿修改前的 depth / cone / path 結果判斷修改後的 netlist。
```
