# 待詢問官方的問題

## Path count 的數值上限

### 問題背景

組合路徑數可能隨電路結構呈指數成長。現有 `PathQuery` 使用 64-bit `size_t` 保存
路徑數；測試中的 196-gate DAG 已能產生 `2^65` 條路徑，超過 unsigned 64-bit
可表示的最大值。因此需要確認正式測資是否可能要求輸出超過 `2^64 - 1` 的精確數量，
以決定是否導入 arbitrary-precision integer。

### 可直接寄出的英文信件

```text
Subject: Clarification on the Maximum Expected Count in Path-Counting Prompts

Dear CAD Contest Committee,

We would like to clarify the expected numeric range for counting-related prompts, particularly prompts asking for the total number of combinational paths or register-to-register paths.

Because the number of distinct paths in a netlist may grow exponentially, even a relatively small circuit can contain more than 2^64 - 1 paths.

Should contestants expect the exact answer to any counting prompt to exceed the range of a 64-bit unsigned integer?

If so, are implementations required to support arbitrary-precision integer counting and print the complete exact decimal value?

For example, if a circuit contains 2^65 distinct combinational paths, should the expected answer be:

36893488147419103232

Or can we assume that all official test cases will have exact counts within the range of an unsigned 64-bit integer?

Thank you.
```

### 回覆後的處理方式

- 若官方保證所有 count 都不超過 `2^64 - 1`：保留現有型別，將 `PATH-003` 標記為不影響正式測資。
- 若官方不保證上限：規劃 arbitrary-precision path count，並同步 report、CLI printer、API 文件與 regression。

