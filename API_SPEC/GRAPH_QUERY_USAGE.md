# Graph Query 使用說明

`GraphQuery` 不再是 LLM/tools 的公開入口。原本的 cut/articulation 功能已合併到 `PathQuery`；`GraphAnalysis.cpp` 仍保留 dominator engine，`runGraphQuery()` 只作為 `PathQuery` 的內部 adapter 與舊程式相容層。

內部 contract 與 dominator 演算法請看：

```text
API_SPEC/GRAPH_QUERY_API.md
```

---

## 1. 公開替代方式

### 1.1 PI-to-PO Cut

```cpp
Netlist::PathQuery query;
query.mode = Netlist::PathQueryMode::IsSeparator;
query.separatorCandidateNetName = "n55104";

const auto report = netlist.runPathQuery(query);
```

```text
path_query pi_po_cut n55104
```

主要讀取 `ok`、`pathExists`、`isSeparator`、`witnessStartpoint`、`witnessEndpoint` 與 `status`。

### 1.2 Source-to-Target Mandatory Nodes

```cpp
Netlist::PathQuery query;
query.mode = Netlist::PathQueryMode::FindMandatoryNodes;
query.startpoints.push_back(
    Netlist::PathEndpoint(Netlist::PathEndpointType::SpecificNet, "n2"));
query.endpoints.push_back(
    Netlist::PathEndpoint(Netlist::PathEndpointType::SpecificNet, "n14"));

const auto report = netlist.runPathQuery(query);
```

```text
path_query mandatory_nodes net:n2 net:n14
```

主要讀取 `pathExists`、`mandatoryNetIds`、`mandatoryNetNames` 與 `status`。

### 1.3 Specific Separator

```text
path_query is_separator net:n2 net:n14 n10
```

`isSeparator=true` 表示候選 net 位於選定 endpoints 間所有 directed combinational paths。底層仍使用 dominator，不列舉所有 paths。

## 2. Legacy 相容性

以下舊入口目前仍可執行，但不出現在 public help/schema：

```text
graph_query pi_po_cut <internal_net>
graph_query articulation_between <source_net> <target_net>
```

新程式與 LLM routing 不得再新增對 `graph_query` 的依賴。
