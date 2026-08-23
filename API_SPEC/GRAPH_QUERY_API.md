# Graph Query Internal/Legacy API

> Status: Reference (internal/legacy adapter)
>
> Public owner: `PathQuery::IsSeparator`、`PathQuery::FindMandatoryNodes`

本文件只說明 directed combinational graph 的 cut/articulation backend、舊 C++ 型別與
相容入口。它回答 graph connectivity 問題，不判斷 Boolean functional dependence，也不
執行 netlist 修改。LLM、`tools.cpp` 新功能與其他 public caller 不應直接選擇
`GraphQuery`。

公開契約與用法請看：

- [`PATH_QUERY_API.md`](PATH_QUERY_API.md)
- [`PATH_QUERY_USAGE.md`](PATH_QUERY_USAGE.md)
- [`TOOLS_SPEC/PATH_QUERY_TOOL.md`](../TOOLS_SPEC/PATH_QUERY_TOOL.md)

---

## 1. Graph Semantics

每個 active net 是一個 graph node；每個 active combinational gate 產生 `input net -> output net` directed edges。`UNKNOWN` gate、removed net 與 DFF D-to-Q edge 不納入 graph，因此 DFF 是 sequential boundary。

`ArticulationPointsBetween` 在本專案中的精確語意是 source-to-target directed vertex separator：移除該 internal net 後，source 到 target 不再有 directed combinational path。source 與 target 本身不列入結果。

`IsCutNetBetweenPiPo` 的語意是：是否至少存在一組 PI bit / PO bit，原本有完整 directed path 經過 candidate，而移除 candidate 後該 PI-to-PO pair 的所有 path 都被切斷。

## 2. Query Types

```cpp
enum class GraphQueryType {
    IsCutNetBetweenPiPo,
    ArticulationPointsBetween
};

struct GraphQuery {
    GraphQueryType type;
    std::string netName;
    std::string sourceNetName;
    std::string targetNetName;
};
```

| Type | Required input | Main result |
|---|---|---|
| `IsCutNetBetweenPiPo` | `netName` internal net | `isCut`, `pathExists`, witness PI/PO |
| `ArticulationPointsBetween` | `sourceNetName`, `targetNetName` | `pathExists`, articulation net IDs/names |

## 3. Report Contract

```cpp
struct GraphReport {
    bool ok;
    bool exists;
    bool unsupported;
    bool combinationalCycleDetected;
    std::string message;
    std::string status;

    std::string candidateNetName;
    int candidateNetId;
    std::string sourceNetName;
    int sourceNetId;
    std::string targetNetName;
    int targetNetId;

    bool pathExists;
    bool isCut;
    size_t checkedPrimaryInputCount;
    size_t checkedPrimaryOutputCount;
    std::string witnessPrimaryInput;
    std::string witnessPrimaryOutput;

    std::vector<int> articulationNetIds;
    std::vector<std::string> articulationNetNames;
};
```

`exists` 對 cut query 等同 `isCut`；對 articulation query 表示 list 是否非空。`pathExists` 必須另外讀取，因為「沒有 path」與「有 path 但沒有 internal articulation point」是不同結果。

常見 status：

| Status | Meaning |
|---|---|
| `CUT_NET` | candidate 是至少一組 PI-to-PO pair 的 cut |
| `NOT_CUT_NET` | candidate 不是任何 PI-to-PO pair 的 cut |
| `ARTICULATION_POINTS_FOUND` | 已回傳 source-to-target articulation nets |
| `NO_ARTICULATION_POINTS` | source/target connected，但沒有 internal articulation net |
| `NO_PATH` | source 到 target 沒有 directed combinational path |
| `NET_NOT_FOUND` / `SOURCE_NOT_FOUND` / `TARGET_NOT_FOUND` | 名稱不存在或物件已 removed |
| `INTERNAL_NET_REQUIRED` | cut candidate 是 PI、PO 或 constant |
| `COMBINATIONAL_CYCLE` | graph 非 DAG，這版 dominator analysis 不支援 |

## 4. Algorithm

實作位於 `src/analysis/GraphAnalysis.cpp`。系統先建立 active net DAG 與 topological order，再以 immediate-dominator tree 求出所有 directed mandatory nodes。

`ArticulationPointsBetween` 建立一次 source-rooted dominator tree，沿 target 的 immediate-dominator chain 回收結果。`IsCutNetBetweenPiPo` 對每個 PI bit 建立 dominator tree，尋找 candidate 支配的 reachable PO bit，並回傳第一組 witness。

此流程不列舉所有 paths。對固定 PI 數量，時間接近 graph size 的線性倍數，適合大型 testcase。

## 5. Internal/Compatibility Entry Point

```cpp
Netlist::GraphReport report = netlist.runGraphQuery(query);
```

`runGraphQuery()` 由 `runPathQuery()` 內部呼叫，或供舊程式相容使用。`tools.cpp` 目前仍
保留隱藏的 `graph_query` compatibility parser，但 help/schema 不把它列為 LLM-facing
command。公開入口應呼叫 `runPathQuery()`，不要直接操作 adjacency、topological order
或 dominator state。

## 6. Current Limits

1. 只分析 active combinational DAG；遇到 combinational cycle 會回 `unsupported`。
2. DFF 不建立 D-to-Q edge，不做跨 cycle articulation。
3. Articulation result 是 net-level directed separator，不列 gate instance。
4. PI-to-PO cut 採「至少一組 PI/PO pair 被切斷」語意，並提供 witness pair。
