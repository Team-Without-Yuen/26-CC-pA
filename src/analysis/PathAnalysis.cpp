#include "include/core/Netlist.h"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <queue>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

// 保存已由名稱解析成 ID 的條件節點，供搜尋時以整數快速比對。
struct ResolvedPathNode {
    Netlist::PathNodeType type;
    int id;
};

// 保存一筆 BFS 搜尋狀態；passedRequired[i] 表示是否已經過第 i 個 required node。
struct SearchState {
    int netId;
    std::vector<unsigned char> passedRequired;
    int previousStateIndex;
    int previousGateId;
};

class LiteralPathArtifactWriter;

struct PathEnumerationOptions {
    double timeLimitSeconds = 0.0;
    bool countOnly = false;
    bool storePaths = true;
    size_t maxStoredPaths = 0;
    LiteralPathArtifactWriter* pathWriter = nullptr;
};

struct PathEnumerationState {
    size_t pathCount = 0;
    bool complete = true;
    bool timedOut = false;
    bool pathLimitReached = false;
    bool outputFailed = false;
    size_t periodicWorkCount = 0;
    std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();
};

struct PathCountMemo {
    std::vector<size_t> counts;
    std::vector<unsigned char> ready;
    std::vector<unsigned char> visiting;
    bool cycleDetected = false;
};

struct LongestPathMemo {
    std::vector<int> depths;
    std::vector<Netlist::CombinationalPath> paths;
    std::vector<unsigned char> ready;
    std::vector<unsigned char> visiting;
    bool cycleDetected = false;
};

bool shouldStopEnumeration(const PathEnumerationOptions& options,
                           PathEnumerationState& state) {
    if (state.outputFailed) {
        state.complete = false;
        return true;
    }
    if (options.timeLimitSeconds > 0.0) {
        const auto now = std::chrono::steady_clock::now();
        const std::chrono::duration<double> elapsed = now - state.startTime;
        if (elapsed.count() >= options.timeLimitSeconds) {
            state.complete = false;
            state.timedOut = true;
            return true;
        }
    }

    return false;
}

bool shouldStopEnumerationPeriodically(const PathEnumerationOptions& options,
                                       PathEnumerationState& state) {
    if (state.outputFailed) {
        state.complete = false;
        return true;
    }
    ++state.periodicWorkCount;
    if ((state.periodicWorkCount & 0x3fffU) != 0) {
        return false;
    }
    return shouldStopEnumeration(options, state);
}

constexpr std::streamoff kPathCountHeaderOffset =
    static_cast<std::streamoff>(sizeof("Total paths: ") - 1);
constexpr int kPathCountHeaderWidth = 20;

bool rewritePathFileCount(std::fstream& file, size_t pathCount) {
    file.flush();
    file.seekp(kPathCountHeaderOffset, std::ios::beg);
    if (!file) {
        return false;
    }
    file << std::setw(kPathCountHeaderWidth) << pathCount;
    file.flush();
    file.seekp(0, std::ios::end);
    return static_cast<bool>(file);
}

// 逐條輸出具名 net/gate sequence，讓 artifact 不需額外解碼即可直接作答。
class LiteralPathArtifactWriter {
public:
    LiteralPathArtifactWriter(std::ostream& output,
                              const Netlist& netlist,
                              bool expectedCountKnown,
                              size_t expectedPathCount)
        : output_(output), netlist_(netlist), expectedCountKnown_(expectedCountKnown) {
        buffer_.reserve(kBufferCapacity);
        appendText("Total paths: ");
        if (expectedCountKnown_) {
            appendUnsigned(expectedPathCount);
        } else {
            appendRepeated(' ', kPathCountHeaderWidth - 1);
            appendText("0");
        }
        appendText("\nFormat: LITERAL_PATH_V1\n");
        appendText("Each record is a complete start-to-end sequence.\n");
        appendText("Record syntax: Path <index>: <net> -> <gate>(<type>) -> <net> ...\n");
        if (expectedCountKnown_) {
            appendText("Expected paths: ");
            appendUnsigned(expectedPathCount);
            appendText("\n");
        }
        appendText("\n[paths]\n");
    }

    bool writePath(const Netlist::CombinationalPath& path, size_t pathIndex) {
        if (failed_ || path.netIds.empty()) {
            failed_ = true;
            return false;
        }

        if (path.netIds.size() != path.gateIds.size() + 1) {
            failed_ = true;
            return false;
        }

        appendText("Path ");
        appendUnsigned(pathIndex);
        appendText(": ");
        for (size_t i = 0; i < path.netIds.size(); ++i) {
            const int netId = path.netIds[i];
            if (!netlist_.isValidNetId(netId)) {
                failed_ = true;
                return false;
            }
            appendText(netlist_.getNet(netId).name);
            if (i < path.gateIds.size()) {
                const int gateId = path.gateIds[i];
                if (!netlist_.isValidGateId(gateId)) {
                    failed_ = true;
                    return false;
                }
                const Gate& gate = netlist_.getGate(gateId);
                appendText(" -> ");
                appendText(gate.instName);
                appendText("(");
                appendText(netlist_.gateTypeToString(gate.type));
                appendText(") -> ");
            }
        }
        appendText("\n");
        return !failed_;
    }

    bool finish(size_t writtenPathCount, bool complete, bool timedOut) {
        appendText("\n[summary]\nWritten paths: ");
        appendUnsigned(writtenPathCount);
        appendText("\nComplete: ");
        appendText(complete ? "yes" : "no");
        appendText("\nTimed out: ");
        appendText(timedOut ? "yes" : "no");
        appendText("\n");
        return flush();
    }

    bool expectedCountKnown() const { return expectedCountKnown_; }

private:
    static constexpr size_t kBufferCapacity = 32U * 1024U * 1024U;

    void appendText(std::string_view text) {
        if (buffer_.size() + text.size() > kBufferCapacity && !flush()) {
            return;
        }
        buffer_.append(text.data(), text.size());
    }

    void appendRepeated(char value, size_t count) {
        if (buffer_.size() + count > kBufferCapacity && !flush()) {
            return;
        }
        buffer_.append(count, value);
    }

    template <typename Integer>
    void appendInteger(Integer value) {
        char digits[32];
        const auto converted = std::to_chars(digits, digits + sizeof(digits), value);
        if (converted.ec != std::errc()) {
            failed_ = true;
            return;
        }
        if (buffer_.size() + static_cast<size_t>(converted.ptr - digits) >
                kBufferCapacity &&
            !flush()) {
            return;
        }
        buffer_.append(digits, converted.ptr);
    }

    void appendUnsigned(size_t value) { appendInteger(value); }
    bool flush() {
        if (failed_) {
            return false;
        }
        if (!buffer_.empty()) {
            output_.write(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
            buffer_.clear();
        }
        if (!output_) {
            failed_ = true;
        }
        return !failed_;
    }

    std::ostream& output_;
    const Netlist& netlist_;
    std::string buffer_;
    bool expectedCountKnown_ = false;
    bool failed_ = false;
};

// 將一組 PathNode 名稱轉成 ID；任一名稱不存在代表查詢條件無效。
bool resolvePathNodes(const Netlist& netlist,
                      const std::vector<Netlist::PathNode>& nodes,
                      std::vector<ResolvedPathNode>& resolvedNodes) {
    for (const Netlist::PathNode& node : nodes) {
        ResolvedPathNode resolved;
        resolved.type = node.type;
        if (node.type == Netlist::PathNodeType::Net) {
            resolved.id = netlist.getNetId(node.name);
        } else {
            resolved.id = netlist.getGateId(node.name);
        }
        if (resolved.id < 0) {
            return false;
        }
        resolvedNodes.push_back(resolved);
    }
    return true;
}

std::string describePathEndpoint(const Netlist::PathEndpoint& endpoint) {
    std::string typeName;
    switch (endpoint.type) {
    case Netlist::PathEndpointType::SpecificNet: typeName = "net"; break;
    case Netlist::PathEndpointType::PrimaryInput: typeName = "pi"; break;
    case Netlist::PathEndpointType::PrimaryOutput: typeName = "po"; break;
    case Netlist::PathEndpointType::DffQ: typeName = "dff_q"; break;
    case Netlist::PathEndpointType::DffD: typeName = "dff_d"; break;
    case Netlist::PathEndpointType::DffClock: typeName = "dff_clock"; break;
    case Netlist::PathEndpointType::DffReset: typeName = "dff_reset"; break;
    case Netlist::PathEndpointType::GateOutput: typeName = "gate_out"; break;
    case Netlist::PathEndpointType::GateInput: typeName = "gate_in"; break;
    }

    std::string description = typeName + ":" + endpoint.name;
    if (!endpoint.pinName.empty()) {
        description += ":" + endpoint.pinName;
    } else if (endpoint.pinIndex >= 0) {
        description += ":" + std::to_string(endpoint.pinIndex);
    }
    return description;
}

std::string describePathNode(const Netlist::PathNode& node) {
    return std::string(node.type == Netlist::PathNodeType::Net ? "net:" : "gate:") +
           node.name;
}

// 將 RegisterPathQueryMode 對應到底層 PathQueryMode。
Netlist::PathQueryMode toPathQueryMode(Netlist::RegisterPathQueryMode mode) {
    switch (mode) {
    case Netlist::RegisterPathQueryMode::Exists:
        return Netlist::PathQueryMode::Exists;
    case Netlist::RegisterPathQueryMode::FindAny:
        return Netlist::PathQueryMode::FindAny;
    case Netlist::RegisterPathQueryMode::EnumerateAll:
        return Netlist::PathQueryMode::EnumerateAll;
    case Netlist::RegisterPathQueryMode::MinDepth:
        return Netlist::PathQueryMode::MinDepth;
    case Netlist::RegisterPathQueryMode::MaxDepth:
        return Netlist::PathQueryMode::MaxDepth;
    }
    return Netlist::PathQueryMode::Exists;
}

// 解析 register path query 的 DFF 名稱；空清單代表使用設計中所有 DFF。
bool resolveDffNameList(const Netlist& netlist,
                        const std::vector<std::string>& requestedNames,
                        std::vector<std::string>& resolvedNames,
                        std::string& message) {
    resolvedNames = requestedNames.empty() ? netlist.getDffNames() : requestedNames;
    if (resolvedNames.empty()) {
        message = "No DFF instances are available";
        return false;
    }

    std::unordered_set<std::string> seen;
    std::vector<std::string> uniqueNames;
    uniqueNames.reserve(resolvedNames.size());
    for (const std::string& dffName : resolvedNames) {
        const int gateId = netlist.getGateId(dffName);
        if (!netlist.isDffGate(gateId)) {
            message = "Unknown or non-DFF instance: " + dffName;
            return false;
        }
        if (seen.insert(dffName).second) {
            uniqueNames.push_back(dffName);
        }
    }

    resolvedNames = std::move(uniqueNames);
    return true;
}

// 判斷指定 net 是否存在於一組條件節點中；gate 類型條件不會命中。
bool containsNet(const std::vector<ResolvedPathNode>& nodes, int netId) {
    for (const ResolvedPathNode& node : nodes) {
        if (node.type == Netlist::PathNodeType::Net && node.id == netId) {
            return true;
        }
    }
    return false;
}

// 判斷指定 gate 是否存在於一組條件節點中；net 類型條件不會命中。
bool containsGate(const std::vector<ResolvedPathNode>& nodes, int gateId) {
    for (const ResolvedPathNode& node : nodes) {
        if (node.type == Netlist::PathNodeType::Gate && node.id == gateId) {
            return true;
        }
    }
    return false;
}

// 在走到指定 net 時，更新哪些 required net 條件已被滿足。
void markRequiredNet(const std::vector<ResolvedPathNode>& requiredNodes,
                     int netId,
                     std::vector<unsigned char>& passedRequired) {
    for (size_t i = 0; i < requiredNodes.size(); ++i) {
        if (requiredNodes[i].type == Netlist::PathNodeType::Net &&
            requiredNodes[i].id == netId) {
            passedRequired[i] = 1;
        }
    }
}

// 在穿越指定 gate 時，更新哪些 required gate 條件已被滿足。
void markRequiredGate(const std::vector<ResolvedPathNode>& requiredNodes,
                      int gateId,
                      std::vector<unsigned char>& passedRequired) {
    for (size_t i = 0; i < requiredNodes.size(); ++i) {
        if (requiredNodes[i].type == Netlist::PathNodeType::Gate &&
            requiredNodes[i].id == gateId) {
            passedRequired[i] = 1;
        }
    }
}

// 判斷 requiredNodes 的所有條件是否都已經被目前路徑滿足。
bool allRequiredPassed(const std::vector<unsigned char>& passedRequired) {
    for (unsigned char passed : passedRequired) {
        if (passed == 0) {
            return false;
        }
    }
    return true;
}

// 從 end net 反向標記所有能在 combinational boundary 內到達 end 的 nets。
// DFS enumerate all 會用這個 bitmap 剪掉不可能到終點的分支。
std::vector<unsigned char> computeReverseReachableNets(
    const Netlist& netlist,
    int endNetId,
    const std::vector<ResolvedPathNode>& avoidedNodes) {
    std::vector<unsigned char> canReach(netlist.getNetCount(), 0);
    if (!netlist.isValidNetId(endNetId) || containsNet(avoidedNodes, endNetId)) {
        return canReach;
    }

    std::queue<int> pending;
    canReach[static_cast<size_t>(endNetId)] = 1;
    pending.push(endNetId);

    while (!pending.empty()) {
        const int currentNetId = pending.front();
        pending.pop();

        const Net& currentNet = netlist.getNet(currentNetId);
        const int driverGateId = currentNet.driverGateId;
        if (driverGateId < 0 || containsGate(avoidedNodes, driverGateId)) {
            continue;
        }

        const Gate& driverGate = netlist.getGate(driverGateId);
        if (driverGate.type == GateType::DFF) {
            continue;
        }

        for (int inputNetId : driverGate.inputNetIds) {
            if (!netlist.isValidNetId(inputNetId) ||
                containsNet(avoidedNodes, inputNetId) ||
                canReach[static_cast<size_t>(inputNetId)] != 0) {
                continue;
            }
            canReach[static_cast<size_t>(inputNetId)] = 1;
            pending.push(inputNetId);
        }
    }

    return canReach;
}

size_t saturatingAdd(size_t lhs, size_t rhs) {
    const size_t maxValue = std::numeric_limits<size_t>::max();
    if (maxValue - lhs < rhs) {
        return maxValue;
    }
    return lhs + rhs;
}

// Count-only fast path: memoize the number of combinational paths from each net
// to the fixed endpoint. This is exact for DAG-like combinational logic. If a
// cycle is observed, the caller must fall back to the explicit DFS enumerator.
size_t countPathsToEndDepthFirst(
    const Netlist& netlist,
    int currentNetId,
    int endNetId,
    const std::vector<ResolvedPathNode>& avoidedNodes,
    const std::vector<unsigned char>& canReachEnd,
    const PathEnumerationOptions& options,
    PathEnumerationState& state,
    PathCountMemo& memo) {
    if (shouldStopEnumeration(options, state) || memo.cycleDetected) {
        return 0;
    }
    if (!netlist.isValidNetId(currentNetId) ||
        static_cast<size_t>(currentNetId) >= canReachEnd.size() ||
        canReachEnd[static_cast<size_t>(currentNetId)] == 0) {
        return 0;
    }
    if (currentNetId == endNetId) {
        return 1;
    }

    const size_t currentIndex = static_cast<size_t>(currentNetId);
    if (memo.ready[currentIndex] != 0) {
        return memo.counts[currentIndex];
    }
    if (memo.visiting[currentIndex] != 0) {
        memo.cycleDetected = true;
        return 0;
    }

    memo.visiting[currentIndex] = 1;
    size_t count = 0;

    const Net& currentNet = netlist.getNet(currentNetId);
    for (int gateId : currentNet.loadGateIds) {
        if (shouldStopEnumeration(options, state) || memo.cycleDetected) {
            break;
        }

        const Gate& gate = netlist.getGate(gateId);
        if (gate.type == GateType::DFF ||
            gate.outputNetId < 0 ||
            containsGate(avoidedNodes, gateId)) {
            continue;
        }

        const int nextNetId = gate.outputNetId;
        if (containsNet(avoidedNodes, nextNetId) ||
            static_cast<size_t>(nextNetId) >= canReachEnd.size() ||
            canReachEnd[static_cast<size_t>(nextNetId)] == 0) {
            continue;
        }

        count = saturatingAdd(
            count,
            countPathsToEndDepthFirst(netlist, nextNetId, endNetId,
                                      avoidedNodes, canReachEnd,
                                      options, state, memo));
    }

    memo.visiting[currentIndex] = 0;
    if (!memo.cycleDetected && !state.timedOut) {
        memo.counts[currentIndex] = count;
        memo.ready[currentIndex] = 1;
    }
    return count;
}

// 將 BFS 狀態轉成 visited key；相同 net 但通過條件不同，必須視為不同狀態。
std::string makeStateKey(int netId,
                         const std::vector<unsigned char>& passedRequired) {
    std::string key = std::to_string(netId);
    key.push_back(':');
    for (unsigned char passed : passedRequired) {
        key.push_back(passed == 0 ? '0' : '1');
    }
    return key;
}

// 使用 BFS 找到任意一條經過全部 requiredNodes 且避開全部 avoidedNodes 的路徑。
// requiredNodes 為空代表沒有必經限制，avoidedNodes 為空代表沒有禁止限制。
Netlist::CombinationalPath findPathMatching(
    const Netlist& netlist,
    const std::string& startNet,
    const std::string& endNet,
    const std::vector<ResolvedPathNode>& requiredNodes,
    const std::vector<ResolvedPathNode>& avoidedNodes) {
    Netlist::CombinationalPath path;
    const int startNetId = netlist.getNetId(startNet);
    const int endNetId = netlist.getNetId(endNet);
    if (startNetId < 0 || endNetId < 0 ||
        containsNet(avoidedNodes, startNetId) ||
        containsNet(avoidedNodes, endNetId)) {
        return path;
    }

    SearchState startState;
    startState.netId = startNetId;
    startState.passedRequired.assign(requiredNodes.size(), 0);
    startState.previousStateIndex = -1;
    startState.previousGateId = -1;
    markRequiredNet(requiredNodes, startNetId, startState.passedRequired);

    if (startNetId == endNetId) {
        if (allRequiredPassed(startState.passedRequired)) {
            path.netIds.push_back(startNetId);
        }
        return path;
    }

    std::vector<SearchState> states;
    std::queue<int> pendingStateIndices;
    std::unordered_set<std::string> visitedStates;
    states.push_back(startState);
    pendingStateIndices.push(0);
    visitedStates.insert(makeStateKey(startNetId, startState.passedRequired));

    int foundStateIndex = -1;
    while (!pendingStateIndices.empty() && foundStateIndex < 0) {
        const int currentStateIndex = pendingStateIndices.front();
        pendingStateIndices.pop();
        const SearchState currentState = states[static_cast<size_t>(currentStateIndex)];

        const Net& currentNet = netlist.getNet(currentState.netId);
        for (int gateId : currentNet.loadGateIds) {
            const Gate& gate = netlist.getGate(gateId);
            if (gate.type == GateType::DFF ||
                gate.outputNetId < 0 ||
                containsGate(avoidedNodes, gateId)) {
                continue;
            }

            const int nextNetId = gate.outputNetId;
            if (containsNet(avoidedNodes, nextNetId)) {
                continue;
            }

            SearchState nextState;
            nextState.netId = nextNetId;
            nextState.passedRequired = currentState.passedRequired;
            nextState.previousStateIndex = currentStateIndex;
            nextState.previousGateId = gateId;
            markRequiredGate(requiredNodes, gateId, nextState.passedRequired);
            markRequiredNet(requiredNodes, nextNetId, nextState.passedRequired);

            const std::string stateKey =
                makeStateKey(nextNetId, nextState.passedRequired);
            if (!visitedStates.insert(stateKey).second) {
                continue;
            }

            states.push_back(nextState);
            const int nextStateIndex = static_cast<int>(states.size() - 1);
            if (nextNetId == endNetId &&
                allRequiredPassed(nextState.passedRequired)) {
                foundStateIndex = nextStateIndex;
                break;
            }
            pendingStateIndices.push(nextStateIndex);
        }
    }

    if (foundStateIndex < 0) {
        return path;
    }

    int currentStateIndex = foundStateIndex;
    while (currentStateIndex >= 0) {
        const SearchState& state = states[static_cast<size_t>(currentStateIndex)];
        path.netIds.push_back(state.netId);
        if (state.previousGateId >= 0) {
            path.gateIds.push_back(state.previousGateId);
        }
        currentStateIndex = state.previousStateIndex;
    }
    std::reverse(path.netIds.begin(), path.netIds.end());
    std::reverse(path.gateIds.begin(), path.gateIds.end());
    return path;
}

// 由目前 net 向下游列舉路徑；目前分支的 cycle guard 可避免遞迴迴圈且保留匯合路徑。
void enumeratePathsDepthFirst(
    const Netlist& netlist,
    int currentNetId,
    int endNetId,
    const std::vector<ResolvedPathNode>& requiredNodes,
    const std::vector<ResolvedPathNode>& avoidedNodes,
    const std::vector<unsigned char>& alreadyPassedRequired,
    Netlist::CombinationalPath& currentPath,
    std::unordered_set<int>& netsInCurrentPath,
    std::vector<Netlist::CombinationalPath>& results,
    const PathEnumerationOptions& options,
    PathEnumerationState& state,
    const std::vector<unsigned char>* canReachEnd) {
    if (shouldStopEnumeration(options, state)) {
        return;
    }

    const Net& currentNet = netlist.getNet(currentNetId);
    for (int gateId : currentNet.loadGateIds) {
        if (shouldStopEnumeration(options, state)) {
            return;
        }

        const Gate& gate = netlist.getGate(gateId);
        if (gate.type == GateType::DFF ||
            gate.outputNetId < 0 ||
            containsGate(avoidedNodes, gateId)) {
            continue;
        }

        const int nextNetId = gate.outputNetId;
        if (containsNet(avoidedNodes, nextNetId) ||
            (canReachEnd != nullptr &&
             (nextNetId < 0 ||
              static_cast<size_t>(nextNetId) >= canReachEnd->size() ||
              (*canReachEnd)[static_cast<size_t>(nextNetId)] == 0)) ||
            netsInCurrentPath.count(nextNetId) != 0) {
            continue;
        }

        std::vector<unsigned char> passedRequired = alreadyPassedRequired;
        markRequiredGate(requiredNodes, gateId, passedRequired);
        markRequiredNet(requiredNodes, nextNetId, passedRequired);
        currentPath.gateIds.push_back(gateId);
        currentPath.netIds.push_back(nextNetId);

        if (nextNetId == endNetId) {
            if (allRequiredPassed(passedRequired)) {
                const size_t pathIndex = state.pathCount;
                state.pathCount++;
                if (options.pathWriter != nullptr &&
                    !options.pathWriter->writePath(currentPath, pathIndex)) {
                    state.outputFailed = true;
                    state.complete = false;
                }
                if (!options.countOnly &&
                    options.storePaths &&
                    (options.maxStoredPaths == 0 ||
                     results.size() < options.maxStoredPaths)) {
                    results.push_back(currentPath);
                }
            }
        } else {
            netsInCurrentPath.insert(nextNetId);
            enumeratePathsDepthFirst(netlist, nextNetId, endNetId,
                                     requiredNodes, avoidedNodes, passedRequired,
                                     currentPath, netsInCurrentPath, results,
                                     options, state, canReachEnd);
            netsInCurrentPath.erase(nextNetId);
        }

        currentPath.netIds.pop_back();
        currentPath.gateIds.pop_back();
    }
}

// 無 required/avoided constraints 的官方 DAG fast path。省略空條件 vector 複製與
// per-branch hash-set cycle guard；reverse reachability 仍會剪除不可能抵達 endpoint 的分支。
void enumerateUnconstrainedPathsDepthFirst(
    const Netlist& netlist,
    int currentNetId,
    int endNetId,
    Netlist::CombinationalPath& currentPath,
    std::vector<Netlist::CombinationalPath>& results,
    const PathEnumerationOptions& options,
    PathEnumerationState& state,
    const std::vector<unsigned char>& canReachEnd) {
    const Net& currentNet = netlist.getNet(currentNetId);
    for (int gateId : currentNet.loadGateIds) {
        if (shouldStopEnumerationPeriodically(options, state)) {
            return;
        }

        const Gate& gate = netlist.getGate(gateId);
        if (gate.type == GateType::DFF || gate.outputNetId < 0) {
            continue;
        }

        const int nextNetId = gate.outputNetId;
        if (static_cast<size_t>(nextNetId) >= canReachEnd.size() ||
            canReachEnd[static_cast<size_t>(nextNetId)] == 0) {
            continue;
        }

        currentPath.gateIds.push_back(gateId);
        currentPath.netIds.push_back(nextNetId);

        if (nextNetId == endNetId) {
            const size_t pathIndex = state.pathCount++;
            if (options.pathWriter != nullptr &&
                !options.pathWriter->writePath(currentPath, pathIndex)) {
                state.outputFailed = true;
                state.complete = false;
            }
            if (!options.countOnly && options.storePaths &&
                (options.maxStoredPaths == 0 ||
                 results.size() < options.maxStoredPaths)) {
                results.push_back(currentPath);
            }
        } else {
            enumerateUnconstrainedPathsDepthFirst(
                netlist, nextNetId, endNetId, currentPath, results,
                options, state, canReachEnd);
        }

        currentPath.netIds.pop_back();
        currentPath.gateIds.pop_back();
        if (state.outputFailed || state.timedOut) {
            return;
        }
    }
}

// 以 DFS 列舉所有經過全部 requiredNodes 且避開全部 avoidedNodes 的路徑。
std::vector<Netlist::CombinationalPath> enumeratePathsMatching(
    const Netlist& netlist,
    const std::string& startNet,
    const std::string& endNet,
    const std::vector<ResolvedPathNode>& requiredNodes,
    const std::vector<ResolvedPathNode>& avoidedNodes,
    const PathEnumerationOptions& options,
    PathEnumerationState& state,
    const std::vector<unsigned char>* precomputedCanReachEnd = nullptr) {
    std::vector<Netlist::CombinationalPath> results;
    const int startNetId = netlist.getNetId(startNet);
    const int endNetId = netlist.getNetId(endNet);
    if (startNetId < 0 || endNetId < 0 ||
        containsNet(avoidedNodes, startNetId) ||
        containsNet(avoidedNodes, endNetId)) {
        return results;
    }

    std::vector<unsigned char> ownedCanReachEnd;
    const std::vector<unsigned char>* canReachEnd = precomputedCanReachEnd;
    if (canReachEnd == nullptr) {
        ownedCanReachEnd = computeReverseReachableNets(netlist, endNetId, avoidedNodes);
        canReachEnd = &ownedCanReachEnd;
    }
    if (static_cast<size_t>(startNetId) >= canReachEnd->size() ||
        (*canReachEnd)[static_cast<size_t>(startNetId)] == 0) {
        return results;
    }

    std::vector<unsigned char> passedRequired(requiredNodes.size(), 0);
    markRequiredNet(requiredNodes, startNetId, passedRequired);
    Netlist::CombinationalPath currentPath;
    currentPath.netIds.push_back(startNetId);
    if (startNetId == endNetId) {
        if (allRequiredPassed(passedRequired)) {
            const size_t pathIndex = state.pathCount;
            state.pathCount++;
            if (options.pathWriter != nullptr &&
                !options.pathWriter->writePath(currentPath, pathIndex)) {
                state.outputFailed = true;
                state.complete = false;
            }
            if (!options.countOnly &&
                options.storePaths &&
                (options.maxStoredPaths == 0 ||
                 results.size() < options.maxStoredPaths)) {
                results.push_back(currentPath);
            }
        }
        return results;
    }

    if (requiredNodes.empty() && avoidedNodes.empty()) {
        enumerateUnconstrainedPathsDepthFirst(
            netlist, startNetId, endNetId, currentPath, results,
            options, state, *canReachEnd);
        return results;
    }

    std::unordered_set<int> netsInCurrentPath;
    netsInCurrentPath.insert(startNetId);
    enumeratePathsDepthFirst(netlist, startNetId, endNetId,
                             requiredNodes, avoidedNodes, passedRequired,
                             currentPath, netsInCurrentPath, results,
                             options, state, canReachEnd);
    return results;
}

// ─────────────────────────────────────────────────────────────────────────────
//  使用 DFS + 動態規劃 (Memoization) 尋找最長路徑
//  回傳值：std::pair<深度, 路徑>。如果深度為 -1 代表此路不通或無法滿足條件。
// ─────────────────────────────────────────────────────────────────────────────
std::pair<int, Netlist::CombinationalPath> findLongestPathDepthFirst(
    const Netlist& netlist,
    int currentNetId,
    int endNetId,
    const std::vector<ResolvedPathNode>& requiredNodes,
    const std::vector<ResolvedPathNode>& avoidedNodes,
    std::vector<unsigned char> passedRequired,
    std::unordered_map<std::string, std::pair<int, Netlist::CombinationalPath>>& memo,
    std::unordered_set<std::string>& inStack) {
    
    // 產生當前狀態的唯一 Key (結合 Net ID 與狀態)
    std::string stateKey = makeStateKey(currentNetId, passedRequired);

    // 抵達終點的終止條件
    if (currentNetId == endNetId) {
        if (allRequiredPassed(passedRequired)) {
            Netlist::CombinationalPath basePath;
            basePath.netIds.push_back(currentNetId);
            return {0, basePath}; // 深度 0，路徑只含終點
        }
        return {-1, {}}; // 雖然到了終點，但必經條件沒滿足，算死路
    }

    // 查表 (Memoization)：確認這個狀態
    auto it = memo.find(stateKey);
    if (it != memo.end()) {
        return it->second;
    }

    // 防無窮迴圈 (Combinational Loop)
    if (inStack.count(stateKey)) {
        return {-1, {}};
    }

    inStack.insert(stateKey);

    const Net& currentNet = netlist.getNet(currentNetId);
    std::pair<int, Netlist::CombinationalPath> bestResult = {-1, {}};

    // 探索所有下游路徑
    for (int gateId : currentNet.loadGateIds) {
        const Gate& gate = netlist.getGate(gateId);
        
        // 避開 DFF, 輸出懸空, 或黑名單 Gate
        if (gate.type == GateType::DFF ||
            gate.outputNetId < 0 ||
            containsGate(avoidedNodes, gateId)) {
            continue;
        }

        const int nextNetId = gate.outputNetId;
        
        // 避開黑名單 Net
        if (containsNet(avoidedNodes, nextNetId)) {
            continue;
        }

        // 拷貝並更新下一步的打勾狀態
        std::vector<unsigned char> nextPassed = passedRequired;
        markRequiredGate(requiredNodes, gateId, nextPassed);
        markRequiredNet(requiredNodes, nextNetId, nextPassed);

        // 往下遞迴尋找最長路徑
        auto result = findLongestPathDepthFirst(
            netlist, nextNetId, endNetId, requiredNodes, avoidedNodes, 
            nextPassed, memo, inStack
        );

        // 如果這條岔路走得通，比對看看是不是目前最長的
        if (result.first >= 0) {
            if (result.first + 1 > bestResult.first) {
                bestResult.first = result.first + 1;
                bestResult.second = result.second; // 拷貝下游回傳的最佳路徑
                
                // 用 push_back，避免 O(N)
                bestResult.second.gateIds.push_back(gateId);
                bestResult.second.netIds.push_back(currentNetId);
            }
        }
    }

    // 清理並存檔
    inStack.erase(stateKey);
    memo[stateKey] = bestResult;
    return bestResult;
}

//  尋找符合 required/avoided 條件的「最長」組合邏輯路徑
Netlist::CombinationalPath findLongestPathMatching(
    const Netlist& netlist,
    const std::string& startNet,
    const std::string& endNet,
    const std::vector<ResolvedPathNode>& requiredNodes,
    const std::vector<ResolvedPathNode>& avoidedNodes) {
    
    Netlist::CombinationalPath path;
    const int startNetId = netlist.getNetId(startNet);
    const int endNetId = netlist.getNetId(endNet);
    
    // 防呆檢查
    if (startNetId < 0 || endNetId < 0 ||
        containsNet(avoidedNodes, startNetId) ||
        containsNet(avoidedNodes, endNetId)) {
        return path;
    }

    // 初始化狀態
    std::vector<unsigned char> passedRequired(requiredNodes.size(), 0);
    markRequiredNet(requiredNodes, startNetId, passedRequired);

    // 準備記憶體與防呆堆疊
    std::unordered_map<std::string, std::pair<int, Netlist::CombinationalPath>> memo;
    std::unordered_set<std::string> inStack;

    // 啟動引擎
    auto result = findLongestPathDepthFirst(
        netlist, startNetId, endNetId, requiredNodes, avoidedNodes,
        passedRequired, memo, inStack
    );

    // 深度 < 0 代表沒找到合法路徑，回傳空的 path
    if (result.first >= 0) {
        path = result.second;
        // 因為 DFS 是由後往前 push_back，這裡做最後一次翻轉
        std::reverse(path.netIds.begin(), path.netIds.end());
        std::reverse(path.gateIds.begin(), path.gateIds.end());
    }

    return path;
}

// Multi-source/multi-end max-depth fast path for unconstrained queries.
// This avoids running one longest-path DFS per start/end pair.
Netlist::CombinationalPath findLongestPathFromAnyStartToNet(
    const Netlist& netlist,
    int currentNetId,
    const std::vector<unsigned char>& isStartNet,
    LongestPathMemo& memo) {
    Netlist::CombinationalPath emptyPath;
    if (!netlist.isValidNetId(currentNetId)) {
        return emptyPath;
    }

    const size_t currentIndex = static_cast<size_t>(currentNetId);
    if (currentIndex >= memo.ready.size()) {
        return emptyPath;
    }
    if (memo.ready[currentIndex] != 0) {
        return memo.paths[currentIndex];
    }
    if (memo.visiting[currentIndex] != 0) {
        memo.cycleDetected = true;
        return emptyPath;
    }

    memo.visiting[currentIndex] = 1;

    Netlist::CombinationalPath bestPath;
    int bestDepth = -1;
    const Net& currentNet = netlist.getNet(currentNetId);
    if (!currentNet.isRemoved &&
        currentIndex < isStartNet.size() &&
        isStartNet[currentIndex] != 0) {
        bestPath.netIds.push_back(currentNetId);
        bestDepth = 0;
    }

    if (!currentNet.isRemoved && currentNet.driverGateId >= 0) {
        const Gate& driverGate = netlist.getGate(currentNet.driverGateId);
        if (driverGate.type != GateType::DFF && driverGate.outputNetId == currentNetId) {
            for (int inputNetId : driverGate.inputNetIds) {
                if (!netlist.isValidNetId(inputNetId)) {
                    continue;
                }

                Netlist::CombinationalPath inputPath =
                    findLongestPathFromAnyStartToNet(
                        netlist, inputNetId, isStartNet, memo);
                if (!inputPath.exists()) {
                    continue;
                }

                const int candidateDepth = inputPath.depth() + 1;
                if (candidateDepth > bestDepth) {
                    bestDepth = candidateDepth;
                    bestPath = std::move(inputPath);
                    bestPath.gateIds.push_back(currentNet.driverGateId);
                    bestPath.netIds.push_back(currentNetId);
                }
            }
        }
    }

    memo.visiting[currentIndex] = 0;
    memo.depths[currentIndex] = bestDepth;
    memo.paths[currentIndex] = bestPath;
    memo.ready[currentIndex] = 1;
    return memo.paths[currentIndex];
}

Netlist::CombinationalPath findLongestPathBetweenEndpointSets(
    const Netlist& netlist,
    const std::vector<int>& startNetIds,
    const std::vector<int>& endNetIds,
    bool& cycleDetected) {
    cycleDetected = false;

    std::vector<unsigned char> isStartNet(netlist.getNetCount(), 0);
    for (int startNetId : startNetIds) {
        if (netlist.isValidNetId(startNetId) &&
            !netlist.getNet(startNetId).isRemoved) {
            isStartNet[static_cast<size_t>(startNetId)] = 1;
        }
    }

    LongestPathMemo memo;
    memo.depths.assign(netlist.getNetCount(), -1);
    memo.paths.resize(netlist.getNetCount());
    memo.ready.assign(netlist.getNetCount(), 0);
    memo.visiting.assign(netlist.getNetCount(), 0);

    Netlist::CombinationalPath bestPath;
    for (int endNetId : endNetIds) {
        if (!netlist.isValidNetId(endNetId) ||
            netlist.getNet(endNetId).isRemoved) {
            continue;
        }

        Netlist::CombinationalPath candidate =
            findLongestPathFromAnyStartToNet(
                netlist, endNetId, isStartNet, memo);
        if (memo.cycleDetected) {
            cycleDetected = true;
            return Netlist::CombinationalPath();
        }
        if (candidate.exists() &&
            (!bestPath.exists() || candidate.depth() > bestPath.depth())) {
            bestPath = std::move(candidate);
        }
    }

    return bestPath;
}

} // namespace

// 判斷指定的節點 (Net 或 Gate) 是否為終點。
// combinationalOnly = true 時，如果下游只接 DFF，也視為組合路徑終點。
// combinationalOnly = false 時，必須真的沒有任何 load 才算拓樸終點。
bool Netlist::isEndpoint(const PathNode& node, bool combinationalOnly) const {
    int targetNetId = -1;

    if (node.type == PathNodeType::Net) {
        targetNetId = getNetId(node.name);
    } else {
        const int gateId = getGateId(node.name);
        if (gateId < 0) {
            return false;
        }
        targetNetId = gates[gateId].outputNetId;
    }

    if (targetNetId < 0) {
        return true;
    }

    const Net& net = nets[targetNetId];
    if (net.loadGateIds.empty()) {
        return true;
    }

    if (combinationalOnly) {
        for (int gateId : net.loadGateIds) {
            if (gateId < 0 || gateId >= static_cast<int>(gates.size())) {
                return false;
            }
            if (gates[gateId].type != GateType::DFF) {
                return false;
            }
        }
        return true;
    }

    return false;
}

// 判斷兩條 net 之間是否至少存在一條組合路徑，且不跨越 DFF。
bool Netlist::hasCombinationalPath(const std::string& startNet,
                                   const std::string& endNet) const {
    return findAnyCombinationalPath(startNet, endNet).exists();
}

// 判斷兩條 net 之間是否至少存在一條避開全部指定節點的組合路徑。
bool Netlist::hasCombinationalPathAvoiding(
    const std::string& startNet,
    const std::string& endNet,
    const std::vector<PathNode>& avoidedNodes) const {
    return findAnyCombinationalPathAvoiding(
        startNet, endNet, avoidedNodes).exists();
}

// 判斷兩條 net 之間是否至少存在一條經過全部指定節點的組合路徑。
bool Netlist::hasCombinationalPathThrough(
    const std::string& startNet,
    const std::string& endNet,
    const std::vector<PathNode>& requiredNodes) const {
    return findAnyCombinationalPathThrough(
        startNet, endNet, requiredNodes).exists();
}

// 判斷是否存在一條經過全部 requiredNodes 且避開全部 avoidedNodes 的組合路徑。
bool Netlist::hasCombinationalPathThroughAvoiding(
    const std::string& startNet,
    const std::string& endNet,
    const std::vector<PathNode>& requiredNodes,
    const std::vector<PathNode>& avoidedNodes) const {
    return findAnyCombinationalPathThroughAvoiding(
        startNet, endNet, requiredNodes, avoidedNodes).exists();
}

// 使用 BFS 找到任意一條兩條 net 之間的組合邏輯路徑。
Netlist::CombinationalPath Netlist::findAnyCombinationalPath(
    const std::string& startNet,
    const std::string& endNet) const {
    return findPathMatching(*this, startNet, endNet, {}, {});
}

// 使用 BFS 找到任意一條避開全部指定節點的組合邏輯路徑。
Netlist::CombinationalPath Netlist::findAnyCombinationalPathAvoiding(
    const std::string& startNet,
    const std::string& endNet,
    const std::vector<PathNode>& avoidedNodes) const {
    std::vector<ResolvedPathNode> avoided;
    if (!resolvePathNodes(*this, avoidedNodes, avoided)) {
        return CombinationalPath();
    }
    return findPathMatching(*this, startNet, endNet, {}, avoided);
}

// 使用 BFS 找到任意一條經過全部指定節點的組合邏輯路徑。
Netlist::CombinationalPath Netlist::findAnyCombinationalPathThrough(
    const std::string& startNet,
    const std::string& endNet,
    const std::vector<PathNode>& requiredNodes) const {
    std::vector<ResolvedPathNode> required;
    if (!resolvePathNodes(*this, requiredNodes, required)) {
        return CombinationalPath();
    }
    return findPathMatching(*this, startNet, endNet, required, {});
}

// 使用 BFS 找到任意一條經過全部 requiredNodes 且避開全部 avoidedNodes 的組合路徑。
Netlist::CombinationalPath Netlist::findAnyCombinationalPathThroughAvoiding(
    const std::string& startNet,
    const std::string& endNet,
    const std::vector<PathNode>& requiredNodes,
    const std::vector<PathNode>& avoidedNodes) const {
    std::vector<ResolvedPathNode> required;
    std::vector<ResolvedPathNode> avoided;
    if (!resolvePathNodes(*this, requiredNodes, required) ||
        !resolvePathNodes(*this, avoidedNodes, avoided)) {
        return CombinationalPath();
    }
    return findPathMatching(*this, startNet, endNet, required, avoided);
}

// 以 DFS backtracking 列出兩條 net 之間的所有組合邏輯路徑。
std::vector<Netlist::CombinationalPath> Netlist::enumerateCombinationalPaths(
    const std::string& startNet,
    const std::string& endNet) const {
    PathEnumerationOptions options;
    PathEnumerationState state;
    return enumeratePathsMatching(*this, startNet, endNet, {}, {}, options, state);
}

// 以 DFS backtracking 列出所有避開全部指定節點的組合邏輯路徑。
std::vector<Netlist::CombinationalPath>
Netlist::enumerateCombinationalPathsAvoiding(
    const std::string& startNet,
    const std::string& endNet,
    const std::vector<PathNode>& avoidedNodes) const {
    std::vector<ResolvedPathNode> avoided;
    if (!resolvePathNodes(*this, avoidedNodes, avoided)) {
        return std::vector<CombinationalPath>();
    }
    PathEnumerationOptions options;
    PathEnumerationState state;
    return enumeratePathsMatching(*this, startNet, endNet, {}, avoided, options, state);
}

// 以 DFS backtracking 列出所有經過全部指定節點的組合邏輯路徑。
std::vector<Netlist::CombinationalPath>
Netlist::enumerateCombinationalPathsThrough(
    const std::string& startNet,
    const std::string& endNet,
    const std::vector<PathNode>& requiredNodes) const {
    std::vector<ResolvedPathNode> required;
    if (!resolvePathNodes(*this, requiredNodes, required)) {
        return std::vector<CombinationalPath>();
    }
    PathEnumerationOptions options;
    PathEnumerationState state;
    return enumeratePathsMatching(*this, startNet, endNet, required, {}, options, state);
}

// 以 DFS 列出所有經過全部 requiredNodes 且避開全部 avoidedNodes 的組合路徑。
std::vector<Netlist::CombinationalPath>
Netlist::enumerateCombinationalPathsThroughAvoiding(
    const std::string& startNet,
    const std::string& endNet,
    const std::vector<PathNode>& requiredNodes,
    const std::vector<PathNode>& avoidedNodes) const {
    std::vector<ResolvedPathNode> required;
    std::vector<ResolvedPathNode> avoided;
    if (!resolvePathNodes(*this, requiredNodes, required) ||
        !resolvePathNodes(*this, avoidedNodes, avoided)) {
        return std::vector<CombinationalPath>();
    }
    PathEnumerationOptions options;
    PathEnumerationState state;
    return enumeratePathsMatching(*this, startNet, endNet, required, avoided, options, state);
}

// 判斷所有既有路徑是否都經過 requiredNodes 中每一個節點；無原始路徑時回傳 false。
bool Netlist::everyPathPassesThrough(
    const std::string& startNet,
    const std::string& endNet,
    const std::vector<PathNode>& requiredNodes) const {
    std::vector<ResolvedPathNode> required;
    if (!resolvePathNodes(*this, requiredNodes, required) ||
        !hasCombinationalPath(startNet, endNet)) {
        return false;
    }
    for (const PathNode& requiredNode : requiredNodes) {
        if (hasCombinationalPathAvoiding(startNet, endNet, {requiredNode})) {
            return false;
        }
    }
    return true;
}

// 判斷所有既有路徑是否都避開 avoidedNodes 中每一個節點；無原始路徑時回傳 false。
bool Netlist::everyPathAvoids(
    const std::string& startNet,
    const std::string& endNet,
    const std::vector<PathNode>& avoidedNodes) const {
    std::vector<ResolvedPathNode> avoided;
    if (!resolvePathNodes(*this, avoidedNodes, avoided) ||
        !hasCombinationalPath(startNet, endNet)) {
        return false;
    }
    for (const PathNode& avoidedNode : avoidedNodes) {
        if (hasCombinationalPathThrough(startNet, endNet, {avoidedNode})) {
            return false;
        }
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  最長組合邏輯路徑 (Longest Combinational Path) API 
// ─────────────────────────────────────────────────────────────────────────────

//  最長路徑的布林判斷 (借用findAnyCombinationalPath，有路徑 = 有最長路徑)

// 找到兩條 net 之間的最長組合邏輯路徑 (無特殊條件)。
Netlist::CombinationalPath Netlist::findLongestCombinationalPath(
    const std::string& startNet,
    const std::string& endNet) const {
    
    // 直接呼叫我們寫好的底層引擎，傳入空的條件陣列 {}
    return findLongestPathMatching(*this, startNet, endNet, {}, {});
}

// 找到兩條 net 之間，且「避開」全部指定節點的最長組合邏輯路徑。
Netlist::CombinationalPath Netlist::findLongestCombinationalPathAvoiding(
    const std::string& startNet,
    const std::string& endNet,
    const std::vector<PathNode>& avoidedNodes) const {
    
    std::vector<ResolvedPathNode> avoided;
    // 將外部的字串名稱轉換為內部的高速比對 ID
    if (!resolvePathNodes(*this, avoidedNodes, avoided)) {
        return CombinationalPath(); // 轉換失敗 (找不到節點) 直接回傳空路徑
    }
    return findLongestPathMatching(*this, startNet, endNet, {}, avoided);
}

// 找到兩條 net 之間，且「經過」全部指定節點的最長組合邏輯路徑。
Netlist::CombinationalPath Netlist::findLongestCombinationalPathThrough(
    const std::string& startNet,
    const std::string& endNet,
    const std::vector<PathNode>& requiredNodes) const {
    
    std::vector<ResolvedPathNode> required;
    if (!resolvePathNodes(*this, requiredNodes, required)) {
        return CombinationalPath();
    }
    return findLongestPathMatching(*this, startNet, endNet, required, {});
}

// 找到兩條 net 之間，「經過」全部 requiredNodes 且「避開」全部 avoidedNodes 的最長路徑。
Netlist::CombinationalPath Netlist::findLongestCombinationalPathThroughAvoiding(
    const std::string& startNet,
    const std::string& endNet,
    const std::vector<PathNode>& requiredNodes,
    const std::vector<PathNode>& avoidedNodes) const {
    
    std::vector<ResolvedPathNode> required;
    std::vector<ResolvedPathNode> avoided;
    
    // 必須 required 和 avoided 兩者都成功解析才繼續
    if (!resolvePathNodes(*this, requiredNodes, required) ||
        !resolvePathNodes(*this, avoidedNodes, avoided)) {
        return CombinationalPath();
    }
    return findLongestPathMatching(*this, startNet, endNet, required, avoided);
}

// ─────────────────────────────────────────────────────────────────────────────
//  最短組合邏輯路徑 (Shortest Combinational Path) API
// ─────────────────────────────────────────────────────────────────────────────

// 找到兩條 net 之間的最短組合邏輯路徑；底層 BFS 第一個合法結果即為最短路徑。
Netlist::CombinationalPath Netlist::findShortestCombinationalPath(
    const std::string& startNet,
    const std::string& endNet) const {
    return findPathMatching(*this, startNet, endNet, {}, {});
}

// 找到兩條 net 之間，且避開全部指定節點的最短組合邏輯路徑。
Netlist::CombinationalPath Netlist::findShortestCombinationalPathAvoiding(
    const std::string& startNet,
    const std::string& endNet,
    const std::vector<PathNode>& avoidedNodes) const {
    std::vector<ResolvedPathNode> avoided;
    if (!resolvePathNodes(*this, avoidedNodes, avoided)) {
        return CombinationalPath();
    }
    return findPathMatching(*this, startNet, endNet, {}, avoided);
}

// 找到兩條 net 之間，且經過全部指定節點的最短組合邏輯路徑。
Netlist::CombinationalPath Netlist::findShortestCombinationalPathThrough(
    const std::string& startNet,
    const std::string& endNet,
    const std::vector<PathNode>& requiredNodes) const {
    std::vector<ResolvedPathNode> required;
    if (!resolvePathNodes(*this, requiredNodes, required)) {
        return CombinationalPath();
    }
    return findPathMatching(*this, startNet, endNet, required, {});
}

// 找到兩條 net 之間，且經過全部 requiredNodes 並避開全部 avoidedNodes 的最短路徑。
Netlist::CombinationalPath Netlist::findShortestCombinationalPathThroughAvoiding(
    const std::string& startNet,
    const std::string& endNet,
    const std::vector<PathNode>& requiredNodes,
    const std::vector<PathNode>& avoidedNodes) const {
    std::vector<ResolvedPathNode> required;
    std::vector<ResolvedPathNode> avoided;
    if (!resolvePathNodes(*this, requiredNodes, required) ||
        !resolvePathNodes(*this, avoidedNodes, avoided)) {
        return CombinationalPath();
    }
    return findPathMatching(*this, startNet, endNet, required, avoided);
}

// 取得指定 DFF 的 named input pin 所連接的 net ID；用於 DFF.D / DFF.CK 等 sequential 查詢。
int Netlist::getDffInputNetId(int dffGateId, const std::string& pinName) const {
    if (dffGateId < 0 || dffGateId >= static_cast<int>(gates.size())) {
        return -1;
    }

    const Gate& dff = gates[dffGateId];
    if (dff.type != GateType::DFF) {
        return -1;
    }

    for (size_t i = 0; i < dff.inputPinNames.size() && i < dff.inputNetIds.size(); ++i) {
        if (dff.inputPinNames[i] == pinName) {
            return dff.inputNetIds[i];
        }
    }
    return -1;
}

// 取得所有 Primary Input port 展開後的 net ID；bus port 會保留每一個 bit net。
std::vector<int> Netlist::getPrimaryInputNetIds() const {
    std::vector<int> netIds;
    for (const Port& port : primaryInputs) {
        for (int netId : port.netIds) {
            if (netId >= 0 && netId < static_cast<int>(nets.size())) {
                netIds.push_back(netId);
            }
        }
    }
    return netIds;
}

// 取得所有 Primary Output port 展開後的 net ID；bus port 會保留每一個 bit net。
std::vector<int> Netlist::getPrimaryOutputNetIds() const {
    std::vector<int> netIds;
    for (const Port& port : primaryOutputs) {
        for (int netId : port.netIds) {
            if (netId >= 0 && netId < static_cast<int>(nets.size())) {
                netIds.push_back(netId);
            }
        }
    }
    return netIds;
}

// 取得指定 DFF 的 Q/output net ID；DFF 的 outputNetId 目前代表 Q pin。
int Netlist::getDffOutputNetId(int dffGateId) const {
    if (dffGateId < 0 || dffGateId >= static_cast<int>(gates.size())) {
        return -1;
    }

    const Gate& dff = gates[dffGateId];
    if (dff.type != GateType::DFF ||
        dff.outputNetId < 0 ||
        dff.outputNetId >= static_cast<int>(nets.size())) {
        return -1;
    }
    return dff.outputNetId;
}

// 取得指定 gate 的 output net ID；primitive gate 與 DFF 都可用。
int Netlist::getGateOutputNetId(int gateId) const {
    if (gateId < 0 || gateId >= static_cast<int>(gates.size())) {
        return -1;
    }

    const int outputNetId = gates[gateId].outputNetId;
    if (outputNetId < 0 || outputNetId >= static_cast<int>(nets.size())) {
        return -1;
    }
    return outputNetId;
}

// 依 input pin 的位置取得指定 gate 的 input net ID；primitive gate 通常使用這個版本。
int Netlist::getGateInputNetId(int gateId, int pinIndex) const {
    if (gateId < 0 || gateId >= static_cast<int>(gates.size()) || pinIndex < 0) {
        return -1;
    }

    const Gate& gate = gates[gateId];
    if (pinIndex >= static_cast<int>(gate.inputNetIds.size())) {
        return -1;
    }

    const int inputNetId = gate.inputNetIds[static_cast<size_t>(pinIndex)];
    if (inputNetId < 0 || inputNetId >= static_cast<int>(nets.size())) {
        return -1;
    }
    return inputNetId;
}

// 依 named input pin 取得指定 gate 的 input net ID；DFF 的 D/CK/RN/SN 會使用這個版本。
int Netlist::getGateInputNetId(int gateId, const std::string& pinName) const {
    if (gateId < 0 || gateId >= static_cast<int>(gates.size())) {
        return -1;
    }

    const Gate& gate = gates[gateId];
    for (size_t i = 0; i < gate.inputPinNames.size() && i < gate.inputNetIds.size(); ++i) {
        if (gate.inputPinNames[i] == pinName) {
            const int inputNetId = gate.inputNetIds[i];
            if (inputNetId >= 0 && inputNetId < static_cast<int>(nets.size())) {
                return inputNetId;
            }
            return -1;
        }
    }
    return -1;
}

// 將單一抽象 PathEndpoint 解析成實際 net ID 陣列；空結果代表解析失敗或沒有對應 net。
std::vector<int> Netlist::resolvePathEndpoint(const PathEndpoint& endpoint) const {
    std::vector<int> netIds;

    auto appendValidNet = [&](int netId) {
        if (netId >= 0 && netId < static_cast<int>(nets.size())) {
            netIds.push_back(netId);
        }
    };

    auto appendPortNets = [&](const std::vector<Port>& ports) {
        for (const Port& port : ports) {
            if (!endpoint.name.empty() && port.name != endpoint.name) {
                continue;
            }
            for (int netId : port.netIds) {
                appendValidNet(netId);
            }
        }
    };

    auto endpointRequestsAllDffs = [&]() {
        return endpoint.name.empty() || endpoint.name == "*";
    };

    switch (endpoint.type) {
    case PathEndpointType::SpecificNet:
        appendValidNet(getNetId(endpoint.name));
        break;
    case PathEndpointType::PrimaryInput:
        appendPortNets(primaryInputs);
        break;
    case PathEndpointType::PrimaryOutput:
        appendPortNets(primaryOutputs);
        break;
    case PathEndpointType::DffQ:
        if (endpointRequestsAllDffs()) {
            for (int gateId : getGatesByType(GateType::DFF)) {
                appendValidNet(getDffOutputNetId(gateId));
            }
        } else {
            appendValidNet(getDffOutputNetId(getGateId(endpoint.name)));
        }
        break;
    case PathEndpointType::DffD:
        if (endpointRequestsAllDffs()) {
            for (int gateId : getGatesByType(GateType::DFF)) {
                appendValidNet(getGateInputNetId(gateId, "D"));
            }
        } else {
            appendValidNet(getGateInputNetId(getGateId(endpoint.name), "D"));
        }
        break;
    case PathEndpointType::DffClock:
        appendValidNet(getGateInputNetId(getGateId(endpoint.name),
                                         endpoint.pinName.empty() ? "CK" : endpoint.pinName));
        break;
    case PathEndpointType::DffReset:
        if (!endpoint.pinName.empty()) {
            appendValidNet(getGateInputNetId(getGateId(endpoint.name), endpoint.pinName));
        } else {
            appendValidNet(getGateInputNetId(getGateId(endpoint.name), "RN"));
            appendValidNet(getGateInputNetId(getGateId(endpoint.name), "SN"));
        }
        break;
    case PathEndpointType::GateOutput:
        appendValidNet(getGateOutputNetId(getGateId(endpoint.name)));
        break;
    case PathEndpointType::GateInput: {
        const int gateId = getGateId(endpoint.name);
        if (!endpoint.pinName.empty()) {
            appendValidNet(getGateInputNetId(gateId, endpoint.pinName));
        } else {
            appendValidNet(getGateInputNetId(gateId, endpoint.pinIndex));
        }
        break;
    }
    }

    return netIds;
}

// 將多個抽象 PathEndpoint 解析成 net ID，並保留首次出現順序、移除重複 net。
std::vector<int> Netlist::resolvePathEndpoints(
    const std::vector<PathEndpoint>& endpoints) const {
    std::vector<int> resolvedNetIds;
    std::unordered_set<int> seenNetIds;

    for (const PathEndpoint& endpoint : endpoints) {
        const std::vector<int> endpointNetIds = resolvePathEndpoint(endpoint);
        for (int netId : endpointNetIds) {
            if (seenNetIds.insert(netId).second) {
                resolvedNetIds.push_back(netId);
            }
        }
    }

    return resolvedNetIds;
}

// 執行統一 endpoint connectivity/path query。
Netlist::PathQueryResult Netlist::runPathQuery(const PathQuery& query) const {
    PathQueryResult result;
    auto importGraphReport = [&result](const GraphReport& graphReport) {
        result.ok = graphReport.ok;
        result.unsupported = graphReport.unsupported;
        result.message = graphReport.message;
        result.status = graphReport.status;
        result.exists = graphReport.exists;
        result.pathExists = graphReport.pathExists;
        result.isSeparator = graphReport.isCut;
        result.combinationalCycleDetected = graphReport.combinationalCycleDetected;
        result.separatorCandidateNetName = graphReport.candidateNetName;
        result.separatorCandidateNetId = graphReport.candidateNetId;
        result.mandatoryNetIds = graphReport.articulationNetIds;
        result.mandatoryNetNames = graphReport.articulationNetNames;
        result.witnessStartpoint = graphReport.witnessPrimaryInput;
        result.witnessEndpoint = graphReport.witnessPrimaryOutput;
        result.checkedStartpointCount = graphReport.checkedPrimaryInputCount;
        result.checkedEndpointCount = graphReport.checkedPrimaryOutputCount;
    };

    if (query.mode == PathQueryMode::EnumerateAll &&
        (!std::isfinite(query.enumerationTimeLimitSeconds) ||
         query.enumerationTimeLimitSeconds <= 0.0)) {
        result.status = "INVALID_ARGUMENT";
        result.message =
            "EnumerateAll requires a finite, positive enumerationTimeLimitSeconds.";
        return result;
    }

    if (!query.combinationalOnly) {
        result.unsupported = true;
        result.message = "Path query currently supports combinationalOnly=true only";
        return result;
    }

    // 無 endpoints 的 IsSeparator 保留原 PI-to-PO cut 語意；底層重用 dominator engine。
    if (query.mode == PathQueryMode::IsSeparator &&
        query.startpoints.empty() && query.endpoints.empty()) {
        if (query.separatorCandidateNetName.empty() ||
            !query.requiredNodes.empty() || !query.avoidedNodes.empty()) {
            result.message =
                "PI-to-PO IsSeparator requires separatorCandidateNetName and no path constraints";
            return result;
        }
        GraphQuery graphQuery;
        graphQuery.type = GraphQueryType::IsCutNetBetweenPiPo;
        graphQuery.netName = query.separatorCandidateNetName;
        importGraphReport(runGraphQuery(graphQuery));
        return result;
    }

    if (query.mode == PathQueryMode::DirectPiPoConnections) {
        if (!query.startpoints.empty() || !query.endpoints.empty() ||
            !query.requiredNodes.empty() || !query.avoidedNodes.empty()) {
            result.message =
                "DirectPiPoConnections does not accept endpoints or path constraints";
            return result;
        }
        const std::vector<int> primaryInputNetIds = getPrimaryInputNetIds();
        const std::vector<int> primaryOutputNetIds = getPrimaryOutputNetIds();
        const std::unordered_set<int> outputNetSet(primaryOutputNetIds.begin(),
                                                   primaryOutputNetIds.end());
        std::unordered_set<int> seenNetIds;
        for (int netId : primaryInputNetIds) {
            if (!isValidNetId(netId) || outputNetSet.count(netId) == 0 ||
                !seenNetIds.insert(netId).second) {
                continue;
            }
            CombinationalPath path;
            path.netIds.push_back(netId);
            result.paths.push_back(path);
        }
        result.ok = true;
        result.message = "Direct PI-to-PO connection query completed";
        result.pathCount = result.paths.size();
        result.exists = result.pathCount > 0;
        if (result.exists) {
            result.path = result.paths.front();
            result.depth = 0;
        }
        return result;
    }

    std::vector<int> startNetIds;
    std::vector<int> endNetIds;
    std::unordered_set<int> seenStartNetIds;
    std::unordered_set<int> seenEndNetIds;

    if (query.startpoints.empty()) {
        result.unresolvedStartpoints.push_back("<missing>");
    }
    for (const PathEndpoint& endpoint : query.startpoints) {
        const std::vector<int> endpointNetIds = resolvePathEndpoint(endpoint);
        if (endpointNetIds.empty()) {
            result.unresolvedStartpoints.push_back(describePathEndpoint(endpoint));
            continue;
        }
        for (int netId : endpointNetIds) {
            if (seenStartNetIds.insert(netId).second) {
                startNetIds.push_back(netId);
            }
        }
    }

    if (query.endpoints.empty()) {
        result.unresolvedEndpoints.push_back("<missing>");
    }
    for (const PathEndpoint& endpoint : query.endpoints) {
        const std::vector<int> endpointNetIds = resolvePathEndpoint(endpoint);
        if (endpointNetIds.empty()) {
            result.unresolvedEndpoints.push_back(describePathEndpoint(endpoint));
            continue;
        }
        for (int netId : endpointNetIds) {
            if (seenEndNetIds.insert(netId).second) {
                endNetIds.push_back(netId);
            }
        }
    }

    auto collectUnresolvedNodes = [&](const std::vector<PathNode>& nodes,
                                      std::vector<std::string>& unresolved) {
        for (const PathNode& node : nodes) {
            std::vector<ResolvedPathNode> resolved;
            if (!resolvePathNodes(*this, {node}, resolved)) {
                unresolved.push_back(describePathNode(node));
            }
        }
    };
    collectUnresolvedNodes(query.requiredNodes, result.unresolvedRequiredNodes);
    collectUnresolvedNodes(query.avoidedNodes, result.unresolvedAvoidedNodes);

    if (!result.unresolvedStartpoints.empty() ||
        !result.unresolvedEndpoints.empty() ||
        !result.unresolvedRequiredNodes.empty() ||
        !result.unresolvedAvoidedNodes.empty()) {
        result.message = "Path query contains unresolved endpoints or constraint nodes";
        return result;
    }

    if (query.mode == PathQueryMode::FindMandatoryNodes ||
        query.mode == PathQueryMode::IsSeparator) {
        if (!query.requiredNodes.empty() || !query.avoidedNodes.empty()) {
            result.message =
                "Mandatory-node and separator modes do not accept required/avoided constraints";
            return result;
        }
        if (startNetIds.size() != 1 || endNetIds.size() != 1) {
            result.message =
                "Mandatory-node and separator modes require exactly one resolved start and endpoint";
            return result;
        }

        if (query.mode == PathQueryMode::IsSeparator) {
            result.separatorCandidateNetName = query.separatorCandidateNetName;
            result.separatorCandidateNetId = getNetId(query.separatorCandidateNetName);
            if (!isValidNetId(result.separatorCandidateNetId) ||
                getNet(result.separatorCandidateNetId).isRemoved) {
                result.ok = false;
                result.exists = false;
                result.isSeparator = false;
                result.status = "SEPARATOR_NOT_FOUND";
                result.message = "Separator candidate net not found: " +
                                 query.separatorCandidateNetName;
                return result;
            }
        }

        GraphQuery graphQuery;
        graphQuery.type = GraphQueryType::ArticulationPointsBetween;
        graphQuery.sourceNetName = nets[startNetIds.front()].name;
        graphQuery.targetNetName = nets[endNetIds.front()].name;
        const GraphReport graphReport = runGraphQuery(graphQuery);
        importGraphReport(graphReport);
        if (query.mode == PathQueryMode::FindMandatoryNodes || !graphReport.ok ||
            !graphReport.pathExists) {
            return result;
        }

        result.separatorCandidateNetName = query.separatorCandidateNetName;
        result.separatorCandidateNetId = getNetId(query.separatorCandidateNetName);
        result.isSeparator = std::find(result.mandatoryNetIds.begin(),
                                       result.mandatoryNetIds.end(),
                                       result.separatorCandidateNetId) !=
                             result.mandatoryNetIds.end();
        result.exists = result.isSeparator;
        result.status = result.isSeparator ? "SEPARATOR" : "NOT_SEPARATOR";
        result.message = result.isSeparator
            ? "Candidate is a directed separator between the selected endpoints."
            : "Candidate is not a directed separator between the selected endpoints.";
        return result;
    }

    result.ok = true;
    result.message = "Path query completed";

    switch (query.mode) {
    case PathQueryMode::Exists:
        for (int startNetId : startNetIds) {
            for (int endNetId : endNetIds) {
                if (hasCombinationalPathThroughAvoiding(
                        nets[startNetId].name,
                        nets[endNetId].name,
                        query.requiredNodes,
                        query.avoidedNodes)) {
                    result.exists = true;
                    return result;
                }
            }
        }
        return result;

    case PathQueryMode::FindAny:
        for (int startNetId : startNetIds) {
            for (int endNetId : endNetIds) {
                CombinationalPath path = findAnyCombinationalPathThroughAvoiding(
                    nets[startNetId].name,
                    nets[endNetId].name,
                    query.requiredNodes,
                    query.avoidedNodes);
                if (path.exists()) {
                    result.exists = true;
                    result.depth = path.depth();
                    result.path = path;
                    return result;
                }
            }
        }
        return result;

    case PathQueryMode::EnumerateAll:
    {
        std::vector<ResolvedPathNode> required;
        std::vector<ResolvedPathNode> avoided;
        if (!resolvePathNodes(*this, query.requiredNodes, required) ||
            !resolvePathNodes(*this, query.avoidedNodes, avoided)) {
            return result;
        }

        PathEnumerationOptions options;
        options.timeLimitSeconds = query.enumerationTimeLimitSeconds;
        options.countOnly = query.countOnly;
        options.storePaths = !query.countOnly;
        options.maxStoredPaths = 0;
        PathEnumerationState state;
        std::fstream pathOutput;
        std::unique_ptr<LiteralPathArtifactWriter> pathWriter;
        bool expectedPathCountKnown = false;
        size_t expectedPathCount = 0;

        // 無 required-node 時可用既有 memoized count-only DP 先取得 exact total。
        // 計數結果同時用於 artifact header 與完成後的完整性核對。
        if (query.writePathsToFile && !query.countOnly && required.empty()) {
            PathQuery countQuery = query;
            countQuery.countOnly = true;
            countQuery.writePathsToFile = false;
            countQuery.outputFilePath.clear();
            countQuery.maxPrintedPaths = 0;
            const PathQueryResult countResult = runPathQuery(countQuery);
            if (countResult.ok && countResult.completeEnumeration &&
                !countResult.enumerationTimedOut) {
                expectedPathCountKnown = true;
                expectedPathCount = countResult.pathCount;
            }
        }

        if (query.writePathsToFile && !query.countOnly) {
            result.outputFilePath = query.outputFilePath.empty()
                                        ? "path_enumeration_output.txt"
                                        : query.outputFilePath;
            pathOutput.open(result.outputFilePath,
                            std::ios::out | std::ios::trunc | std::ios::in);
            if (!pathOutput) {
                result.ok = false;
                result.message = "Failed to open path enumeration output file: " +
                                 result.outputFilePath;
                return result;
            }
            pathWriter = std::make_unique<LiteralPathArtifactWriter>(
                pathOutput, *this, expectedPathCountKnown, expectedPathCount);
            options.pathWriter = pathWriter.get();
            options.storePaths = query.maxPrintedPaths > 0;
            options.maxStoredPaths = query.maxPrintedPaths;
            result.wrotePathsToFile = true;
        }

        bool stopEnumeration = false;
        std::unordered_map<int, std::vector<unsigned char>> reachabilityByEndNet;
        std::unordered_map<int, PathCountMemo> pathCountMemoByEndNet;
        reachabilityByEndNet.reserve(endNetIds.size());
        pathCountMemoByEndNet.reserve(endNetIds.size());
        for (int startNetId : startNetIds) {
            if (stopEnumeration) {
                break;
            }
            for (int endNetId : endNetIds) {
                if (shouldStopEnumeration(options, state)) {
                    stopEnumeration = true;
                    break;
                }

                auto reachabilityIt = reachabilityByEndNet.find(endNetId);
                if (reachabilityIt == reachabilityByEndNet.end()) {
                    reachabilityIt = reachabilityByEndNet
                        .emplace(endNetId,
                                 computeReverseReachableNets(*this, endNetId, avoided))
                        .first;
                }
                const std::vector<unsigned char>& canReachEnd =
                    reachabilityIt->second;
                if (static_cast<size_t>(startNetId) >= canReachEnd.size() ||
                    canReachEnd[static_cast<size_t>(startNetId)] == 0) {
                    continue;
                }

                if (options.countOnly && required.empty()) {
                    auto memoIt = pathCountMemoByEndNet.find(endNetId);
                    if (memoIt == pathCountMemoByEndNet.end()) {
                        PathCountMemo memo;
                        memo.counts.assign(nets.size(), 0);
                        memo.ready.assign(nets.size(), 0);
                        memo.visiting.assign(nets.size(), 0);
                        memoIt = pathCountMemoByEndNet.emplace(endNetId, std::move(memo)).first;
                    }

                    PathCountMemo& countMemo = memoIt->second;
                    if (!countMemo.cycleDetected) {
                        const size_t pairPathCount =
                            countPathsToEndDepthFirst(*this, startNetId, endNetId,
                                                      avoided, canReachEnd,
                                                      options, state, countMemo);
                        if (shouldStopEnumeration(options, state)) {
                            stopEnumeration = true;
                            break;
                        }
                        if (!countMemo.cycleDetected) {
                            state.pathCount =
                                saturatingAdd(state.pathCount, pairPathCount);
                            continue;
                        }
                    }
                }

                std::vector<CombinationalPath> paths =
                    enumeratePathsMatching(
                        *this,
                        nets[startNetId].name,
                        nets[endNetId].name,
                        required,
                        avoided,
                        options,
                        state,
                        &canReachEnd);
                result.paths.insert(result.paths.end(), paths.begin(), paths.end());
                if (shouldStopEnumeration(options, state)) {
                    stopEnumeration = true;
                    break;
                }
            }
        }
        result.pathCount = state.pathCount;
        result.exists = result.pathCount > 0;
        if (expectedPathCountKnown && !state.timedOut && !state.outputFailed &&
            state.pathCount != expectedPathCount) {
            state.complete = false;
            result.ok = false;
            result.message = "Path enumeration count did not match the exact pre-count.";
        }
        result.completeEnumeration = state.complete;
        result.enumerationTimedOut = state.timedOut;
        result.enumerationPathLimitReached = state.pathLimitReached;
        result.countOnly = query.countOnly;
        if (state.timedOut) {
            result.enumerationStopReason = "Enumeration stopped by time limit.";
        } else if (state.outputFailed) {
            result.enumerationStopReason = "Enumeration stopped because artifact output failed.";
        } else if (state.pathLimitReached) {
            result.enumerationStopReason = "Enumeration stopped by maxEnumeratedPaths limit.";
        }
        if (!result.paths.empty()) {
            result.path = result.paths.front();
            result.depth = result.path.depth();
        }
        if (result.wrotePathsToFile) {
            const bool writerFinished = pathWriter != nullptr &&
                pathWriter->finish(result.pathCount,
                                   result.completeEnumeration,
                                   result.enumerationTimedOut);
            const bool countFinalized = writerFinished &&
                (pathWriter->expectedCountKnown() ||
                 rewritePathFileCount(pathOutput, result.pathCount));
            if (!countFinalized) {
                result.ok = false;
                result.completeEnumeration = false;
                result.message = "Failed to finalize path enumeration output file: " +
                                 result.outputFilePath;
                return result;
            }
        }
        return result;
    }

    case PathQueryMode::MinDepth:
        for (int startNetId : startNetIds) {
            for (int endNetId : endNetIds) {
                CombinationalPath path = findShortestCombinationalPathThroughAvoiding(
                    nets[startNetId].name,
                    nets[endNetId].name,
                    query.requiredNodes,
                    query.avoidedNodes);
                if (path.exists() && (!result.exists || path.depth() < result.depth)) {
                    result.exists = true;
                    result.depth = path.depth();
                    result.path = path;
                }
            }
        }
        return result;

    case PathQueryMode::MaxDepth:
        if (query.requiredNodes.empty() && query.avoidedNodes.empty()) {
            bool cycleDetected = false;
            CombinationalPath path = findLongestPathBetweenEndpointSets(
                *this, startNetIds, endNetIds, cycleDetected);
            if (!cycleDetected) {
                if (path.exists()) {
                    result.exists = true;
                    result.depth = path.depth();
                    result.path = std::move(path);
                }
                return result;
            }
        }

        for (int startNetId : startNetIds) {
            for (int endNetId : endNetIds) {
                CombinationalPath path = findLongestCombinationalPathThroughAvoiding(
                    nets[startNetId].name,
                    nets[endNetId].name,
                    query.requiredNodes,
                    query.avoidedNodes);
                if (path.exists() && path.depth() > result.depth) {
                    result.exists = true;
                    result.depth = path.depth();
                    result.path = path;
                }
            }
        }
        return result;

    case PathQueryMode::EveryPathThrough: {
        if (query.requiredNodes.empty()) {
            result.ok = false;
            result.message = "EveryPathThrough requires at least one required node";
            return result;
        }

        bool sawAnyPath = false;
        for (int startNetId : startNetIds) {
            for (int endNetId : endNetIds) {
                const std::string& startName = nets[startNetId].name;
                const std::string& endName = nets[endNetId].name;
                if (!hasCombinationalPath(startName, endName)) {
                    continue;
                }
                sawAnyPath = true;
                if (!everyPathPassesThrough(startName, endName, query.requiredNodes)) {
                    result.exists = false;
                    return result;
                }
            }
        }
        result.exists = sawAnyPath;
        return result;
    }

    case PathQueryMode::EveryPathAvoids: {
        if (query.avoidedNodes.empty()) {
            result.ok = false;
            result.message = "EveryPathAvoids requires at least one avoided node";
            return result;
        }

        bool sawAnyPath = false;
        for (int startNetId : startNetIds) {
            for (int endNetId : endNetIds) {
                const std::string& startName = nets[startNetId].name;
                const std::string& endName = nets[endNetId].name;
                if (!hasCombinationalPath(startName, endName)) {
                    continue;
                }
                sawAnyPath = true;
                if (!everyPathAvoids(startName, endName, query.avoidedNodes)) {
                    result.exists = false;
                    return result;
                }
            }
        }
        result.exists = sawAnyPath;
        return result;
    }

    case PathQueryMode::FindMandatoryNodes:
    case PathQueryMode::IsSeparator:
        return result;

    case PathQueryMode::DirectPiPoConnections:
        return result;

    }

    return result;
}

// 執行 register-to-register path query。
// 這一層不重寫 traversal，而是把 DFF.Q / DFF.D 自動展開成 PathQuery endpoint。
Netlist::RegisterPathReport
Netlist::runRegisterPathQuery(const RegisterPathQuery& query) const {
    RegisterPathReport report;
    report.mode = query.mode;

    if (!query.combinationalOnly) {
        report.message = "Register path query currently supports combinationalOnly=true only";
        return report;
    }

    if (!resolveDffNameList(*this, query.startDffNames, report.startDffNames, report.message) ||
        !resolveDffNameList(*this, query.endDffNames, report.endDffNames, report.message)) {
        return report;
    }

    PathQuery pathQuery;
    pathQuery.mode = toPathQueryMode(query.mode);
    pathQuery.requiredNodes = query.requiredNodes;
    pathQuery.avoidedNodes = query.avoidedNodes;
    pathQuery.combinationalOnly = query.combinationalOnly;
    pathQuery.maxPrintedPaths = query.maxPrintedPaths;
    pathQuery.maxEnumeratedPaths = query.maxEnumeratedPaths;
    pathQuery.enumerationTimeLimitSeconds = query.enumerationTimeLimitSeconds;
    pathQuery.countOnly = query.countOnly;

    if (query.mode == RegisterPathQueryMode::EnumerateAll) {
        pathQuery.writePathsToFile = true;
        pathQuery.outputFilePath = query.outputFilePath.empty()
                                       ? "register_path_enumeration_output.txt"
                                       : query.outputFilePath;
    }

    std::unordered_map<int, std::string> startDffByNet;
    std::unordered_map<int, std::string> endDffByNet;

    for (const std::string& dffName : report.startDffNames) {
        const int qNetId = getDffOutputNetId(getGateId(dffName));
        if (!isValidNetId(qNetId)) {
            report.message = "DFF has no valid Q/output net: " + dffName;
            return report;
        }
        pathQuery.startpoints.push_back(PathEndpoint(PathEndpointType::DffQ, dffName));
        startDffByNet.emplace(qNetId, dffName);
    }

    for (const std::string& dffName : report.endDffNames) {
        const int dNetId = getGateInputNetId(getGateId(dffName), "D");
        if (!isValidNetId(dNetId)) {
            report.message = "DFF has no valid D input net: " + dffName;
            return report;
        }
        pathQuery.endpoints.push_back(PathEndpoint(PathEndpointType::DffD, dffName));
        endDffByNet.emplace(dNetId, dffName);
    }

    report.pathResult = runPathQuery(pathQuery);
    if (!report.pathResult.ok) {
        report.message = report.pathResult.message;
        return report;
    }
    report.ok = true;
    report.exists = report.pathResult.exists;
    report.depth = report.pathResult.depth;
    report.message = report.exists ? "Register-to-register path query succeeded"
                                   : "No register-to-register path found";

    const CombinationalPath& representativePath = report.pathResult.path;
    if (representativePath.exists()) {
        const auto startIt = startDffByNet.find(representativePath.netIds.front());
        if (startIt != startDffByNet.end()) {
            report.startDffName = startIt->second;
        }

        const auto endIt = endDffByNet.find(representativePath.netIds.back());
        if (endIt != endDffByNet.end()) {
            report.endDffName = endIt->second;
        }
    }

    return report;
}

// 掃描所有 PI bit 到所有 DFF D-pin 的組合路徑，找出最大的 logic depth 與 witness path。
std::pair<int, Netlist::CombinationalPath>
Netlist::getMaximumLogicDepthFromPiToDffD() const {
    int bestDepth = -1;
    CombinationalPath bestPath;
    const std::vector<int> dffGateIds = getGatesByType(GateType::DFF);

    for (const Port& inputPort : primaryInputs) {
        for (int piNetId : inputPort.netIds) {
            if (piNetId < 0 || piNetId >= static_cast<int>(nets.size())) {
                continue;
            }

            const std::string& piNetName = nets[piNetId].name;
            for (int dffGateId : dffGateIds) {
                const int dNetId = getDffInputNetId(dffGateId, "D");
                if (dNetId < 0 || dNetId >= static_cast<int>(nets.size())) {
                    continue;
                }

                const std::string& dNetName = nets[dNetId].name;
                CombinationalPath path =
                    findLongestCombinationalPath(piNetName, dNetName);
                if (path.exists() && path.depth() > bestDepth) {
                    bestDepth = path.depth();
                    bestPath = path;
                }
            }
        }
    }

    return {bestDepth, bestPath};
}
