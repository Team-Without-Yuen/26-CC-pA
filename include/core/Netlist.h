#pragma once
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <unordered_set>

// Defines the supported types of logic gates and sequential elements
enum class GateType {
    AND, OR, NAND, NOR, NOT, BUF, XOR, XNOR, DFF, UNKNOWN
};

// Represents a physical wire (net) connecting different components in the circuit
struct Net {
    int id;                      // Unique index in the Netlist's 'nets' vector
    std::string name;            // The name of the wire (e.g., "n1", "clk")

    int driverGateId;            // The ID of the gate driving this net (-1 if it's a Primary Input)
    std::vector<int> loadGateIds;// A list of gate IDs that receive this net as an input

    bool isPI = false;           // Flag indicating if this net is a Primary Input
    bool isPO = false;           // Flag indicating if this net is a Primary Output
    bool isConst = false;        // Flag indicating if this net is a constant

    Net(int _id, const std::string& _name) : id(_id), name(_name), driverGateId(-1) {}
};

// Represents an I/O port declaration, which can be a single wire or a multi-bit bus
struct Port {
    std::string name;            // The base name of the port
    int msb;                     // Most Significant Bit index (-1 if single-bit)
    int lsb;                     // Least Significant Bit index (-1 if single-bit)

    std::vector<int> netIds;     // The underlying physical Net IDs associated with this port

    // Helper function to check if this port is a multi-bit bus
    bool isBus() const { return msb != -1 && lsb != -1; }

    Port(const std::string& _name, int _msb = -1, int _lsb = -1)
        : name(_name), msb(_msb), lsb(_lsb) {
    }
};

// Represents an instantiated logic gate or sequential element (e.g., D-Flip-Flop)
struct Gate {
    int id;                      // Unique index in the Netlist's 'gates' vector
    std::string instName;        // The instance name
    GateType type;               // The functional logic type of the gate

    std::vector<int> inputNetIds;// The Net IDs connected to the input pins of this gate
    std::vector<std::string> inputPinNames;  // for dff
    int outputNetId;             // The Net ID connected to the output pin (-1 if unconnected)

    Gate(int _id, const std::string& _name, GateType _type)
        : id(_id), instName(_name), type(_type), outputNetId(-1) {}
};

// --- Cone 查詢結果 ---
// netIds    : cone 內所有 net 的 ID（flat set，適合快速查詢）
// children  : 樹狀結構，children[A] = {B, C} 表示 A 的下一層是 B 和 C
// rootNetId : 起點 net ID
struct ConeResult {
    std::unordered_set<int> netIds;
    std::unordered_map<int, std::vector<int>> children;
    std::vector<int> rootNetIds; // 支援多個 roots (支援 Bus)
};

// The core data structure representing the entire circuit graph
class Netlist {
private:
    // Contiguous memory storage for elements (improves CPU cache locality and performance)
    std::vector<Gate> gates;
    std::vector<Net> nets;

    // Hash maps for fast name-to-ID lookups, crucial for the parsing phase
    std::unordered_map<std::string, int> gateNameToId;
    std::unordered_map<std::string, int> netNameToId;

    // Stores the I/O ports while maintaining their original declared order
    std::vector<Port> primaryInputs;
    std::vector<Port> primaryOutputs;

public:
    Netlist() = default;
    
    // --- APIs for retrieving I/O definitions (used by Writer) ---
    const std::vector<Port>& getPrimaryInputs() const { return primaryInputs; }
    const std::vector<Port>& getPrimaryOutputs() const { return primaryOutputs; }

    // --- APIs for Adding I/O Ports ---
    void addPrimaryInput(const std::string& portName, int msb = -1, int lsb = -1);
    void addPrimaryOutput(const std::string& portName, int msb = -1, int lsb = -1);

    // --- APIs for Adding Components ---
    int addGate(const std::string& name, GateType type);
    int addNet(const std::string& name);

    // --- APIs for Establishing Connections ---
    void connectGateInput(int gateId, int netId, const std::string& pinName = "");
    void connectGateOutput(int gateId, int netId);

    // --- APIs for Querying Data ---
    const Gate& getGate(int id) const { return gates[id]; } 
    const Net& getNet(int id) const { return nets[id]; } 
    // Compute the total gate count of the design.
    size_t getGateCount() const { return gates.size(); }
    size_t getNetCount() const { return nets.size(); }

    // 取得邏輯 Wire 的總數（多位寬展開的 bit 如 "data[0]", "data[1]" 會被視為同一個 "data"）
    size_t getLogicalWireCount() const;

    // 將 GateType 轉成大寫字串
    std::string gateTypeToString(GateType type) const;
    // 將大寫字串轉成 GateType 
    GateType stringToGateType(std::string str) const;

    // 依 gate instance name 查詢 gate ID；找不到回傳 -1
    int getGateId(const std::string& gateInstName) const;

    // 依 net name 查詢 net ID；找不到回傳 -1
    int getNetId(const std::string& netName) const;

    // 依 gate instance name 取得 Gate 指標；找不到回傳 nullptr
    const Gate* findGate(const std::string& gateInstName) const;

    // 依 net name 取得 Net 指標；找不到回傳 nullptr
    const Net* findNet(const std::string& netName) const;

    // 統計所有 gate type 的數量，包含 AND/OR/NOT/NAND/NOR/XOR/XNOR/BUF/DFF
    std::map<GateType, int> countGatesByType() const;

    // 列出指定 gate type 的所有 gate ID
    std::vector<int> getGatesByType(GateType type) const;

    // 找出含有 constant input 的 gates；type=UNKNOWN 表示不限定 gate type，constValue=-1 表示不限定 0/1
    std::vector<int> findGatesWithConstInput(GateType type = GateType::UNKNOWN, int constValue = -1) const;

    // 找出含有 constant input 的 gates，並回傳它們的實例名稱 (Instance Name)
    std::vector<std::string> getGateNamesWithConstInput(GateType type = GateType::UNKNOWN, int constValue = -1) const;

    // 計算含有 constant input 的 gates 的數量
    size_t countGatesWithConstInput(GateType type = GateType::UNKNOWN, int constValue = -1) const;

    // 給定特定的 Gate Instance Name，回傳包含其類型與 I/O 連線狀態的格式化字串
    std::string getGateInfo(const std::string& instName) const;

    // return which gate input pins are connected to a wire (Wire/PI/PO) (support for multi-bit signals)
    std::vector<int> getWireLoads(const std::string& wireName) const;

    // 取得指定線路直接驅動的下一層邏輯閘名稱列表
    std::vector<std::string> getWireLoadNames(const std::string& wireName) const;

    // 計算指定線路直接驅動的下一層邏輯閘總數
    size_t getWireLoadCount(const std::string& wireName) const;

    // return which gate input pins are connected to a gate output
    std::vector<int> getGateFanout(const std::string& gateInstName) const;

    // 取得指定邏輯閘輸出端直接驅動的下一層邏輯閘名稱列表
    std::vector<std::string> getGateFanoutNames(const std::string& gateInstName) const;

    // 計算指定邏輯閘輸出端直接驅動的下一層邏輯閘總數
    size_t getGateFanoutCount(const std::string& gateInstName) const;

    // Count the number of specific types of logic gates
    size_t getGateCountByType(GateType type) const;

    // 將字串展開成照 index 排序的 Net ID 陣列 (處理單一 bit 或 Bus)
    std::vector<int> expandNetToBits(const std::string& name) const;

    // --- Logic Equivalence Checking (LEC) ---
    // 檢查兩個訊號（支援多位寬）是否在所有輸入情況下功能完全相同
    bool checkEquivalence(const std::string& netA, const std::string& netB) const;

   // --- Cone Analysis ---
    // Transitive Fanin Cone：從 net 往回追到所有 PI（DFF 不穿越）
    ConeResult getTransitiveFaninCone(const std::string& netName) const;
 
    // Transitive Fanout Cone：從 net 往前追到所有 PO（DFF 不穿越）
    ConeResult getTransitiveFanoutCone(const std::string& netName) const;

    // 取得指定 Gate 的 Fanin 邏輯錐
    ConeResult getGateTransitiveFaninCone(const std::string& gateName) const;
    
    // 取得指定 Gate 的 Fanout 邏輯錐
    ConeResult getGateTransitiveFanoutCone(const std::string& gateName) const;

    // 邏輯錐高階查詢 API (Logic Cone Analysis Wrappers)  
    std::vector<std::string> getTransitiveFaninConeGateNames(const std::string& netName) const;
    size_t getTransitiveFaninConeGateCount(const std::string& netName) const;
    std::vector<std::string> getTransitiveFanoutConeGateNames(const std::string& netName) const;
    size_t getTransitiveFanoutConeGateCount(const std::string& netName) const;
    std::vector<std::string> getGateTransitiveFaninConeGateNames(const std::string& gateName) const;
    size_t getGateTransitiveFaninConeGateCount(const std::string& gateName) const;
    std::vector<std::string> getGateTransitiveFanoutConeGateNames(const std::string& gateName) const;
    size_t getGateTransitiveFanoutConeGateCount(const std::string& gateName) const;

    //  分析邏輯錐：尋找錐體內的最長路徑 (Local Critical Path)
    std::pair<int, std::vector<int>> findLongestPathInCone(const ConeResult& cone) const;

    //  分析邏輯錐：尋找錐體內的最短路徑 (Min-Path / Hold Time Check)
    std::pair<int, std::vector<int>> findShortestPathInCone(const ConeResult& cone) const;

    // 針對 Net 的 Fanin Cone 的時序與路徑分析
    std::pair<int, std::vector<std::string>> getTransitiveFaninConeLongestPath(const std::string& netName) const;
    std::pair<int, std::vector<std::string>> getTransitiveFaninConeShortestPath(const std::string& netName) const;

    // 針對 Net 的 Fanout Cone 的時序與路徑分析
    std::pair<int, std::vector<std::string>> getTransitiveFanoutConeLongestPath(const std::string& netName) const;
    std::pair<int, std::vector<std::string>> getTransitiveFanoutConeShortestPath(const std::string& netName) const;

    // 針對 Gate 的 Fanin Cone 的時序與路徑分析
    std::pair<int, std::vector<std::string>> getGateTransitiveFaninConeLongestPath(const std::string& gateName) const;
    std::pair<int, std::vector<std::string>> getGateTransitiveFaninConeShortestPath(const std::string& gateName) const;

    // 針對 Gate 的 Fanout Cone 的時序與路徑分析
    std::pair<int, std::vector<std::string>> getGateTransitiveFanoutConeLongestPath(const std::string& gateName) const;
    std::pair<int, std::vector<std::string>> getGateTransitiveFanoutConeShortestPath(const std::string& gateName) const;

    // =========================================================================
    // 組合邏輯路徑分析 API (Combinational Path Analysis)
    //
    // 共通規則：
    // 1. 起點與終點固定以 net name 表示；一條路徑形式為 Net -> Gate -> Net -> ...。
    // 2. 路徑條件中的每個節點可為 Net 或 Gate，以 PathNode 統一表示。
    // 3. depth 定義為路徑中經過的 combinational gate 數量。
    // 4. DFF 仍會以 GateType::DFF 保存在 netlist 中，但它是 sequential boundary；
    //    組合路徑遇到 DFF 時不從 input 端穿越到 Q。
    // 5. requiredNodes 表示必須全部經過的節點；avoidedNodes 表示必須全部避開的節點。
    //    換句話說，路徑碰到 avoidedNodes 中任一節點即不符合條件。
    // 6. 若名稱不存在、條件矛盾，或起點到終點原本沒有路徑，布林查詢回傳 false。
    // 7. A 至 E 類 API 的實作集中於 src/analysis/PathAnalysis.cpp。
    // =========================================================================

    // 表示限制條件指定的節點種類；可用相同 API 查詢 net 或 gate。
    enum class PathNodeType {
        Net,
        Gate
    };

    // 表示路徑條件中的一個具名節點；多個條件以 std::vector<PathNode> 傳入。
    struct PathNode {
        PathNodeType type;
        std::string name;

        // 建立一個路徑限制節點，type 決定 name 要在 nets 或 gates 中查找。
        PathNode(PathNodeType _type, const std::string& _name)
            : type(_type), name(_name) {
        }
    };

    // 保存一條組合邏輯路徑；netIds 與 gateIds 都依照起點到終點排列。
    struct CombinationalPath {
        std::vector<int> netIds;   // 路徑經過的 nets，包含 startNet 與 endNet
        std::vector<int> gateIds;  // 路徑中真正穿越的 combinational gates

        // 回傳此路徑的邏輯深度，也就是經過的 combinational gate 數量。
        int depth() const {
            return static_cast<int>(gateIds.size());
        }

        // 判斷是否保存了一條有效路徑；沒有找到路徑時 netIds 為空。
        bool exists() const {
            return !netIds.empty();
        }
    };

    // 判斷指定的節點 (Net 或 Gate) 是否為終點。
    bool isEndpoint(const PathNode& node, bool combinationalOnly = true) const;

    // --- A：判斷是否至少存在一條符合條件的路徑 ---

    // 判斷 startNet 到 endNet 是否存在不穿越 DFF 的組合邏輯路徑。
    bool hasCombinationalPath(const std::string& startNet,
                              const std::string& endNet) const;

    // 判斷是否存在至少一條避開 avoidedNodes 中全部節點的組合邏輯路徑。
    bool hasCombinationalPathAvoiding(const std::string& startNet,
                                      const std::string& endNet,
                                      const std::vector<PathNode>& avoidedNodes) const;

    // 判斷是否存在至少一條經過 requiredNodes 中全部節點的組合邏輯路徑。
    bool hasCombinationalPathThrough(const std::string& startNet,
                                     const std::string& endNet,
                                     const std::vector<PathNode>& requiredNodes) const;

    // 判斷是否存在至少一條經過全部 requiredNodes 且避開全部 avoidedNodes 的路徑。
    bool hasCombinationalPathThroughAvoiding(
        const std::string& startNet,
        const std::string& endNet,
        const std::vector<PathNode>& requiredNodes,
        const std::vector<PathNode>& avoidedNodes) const;

    // --- B：回傳任意一條符合條件的實際路徑 ---

    // 找到任意一條 startNet 到 endNet 的組合路徑；不存在時回傳空路徑。
    CombinationalPath findAnyCombinationalPath(const std::string& startNet,
                                               const std::string& endNet) const;

    // 找到任意一條避開 avoidedNodes 中全部節點的組合路徑；不存在時回傳空路徑。
    CombinationalPath findAnyCombinationalPathAvoiding(
        const std::string& startNet,
        const std::string& endNet,
        const std::vector<PathNode>& avoidedNodes) const;

    // 找到任意一條經過 requiredNodes 中全部節點的組合路徑；不存在時回傳空路徑。
    CombinationalPath findAnyCombinationalPathThrough(
        const std::string& startNet,
        const std::string& endNet,
        const std::vector<PathNode>& requiredNodes) const;

    // 找到任意一條經過全部 requiredNodes 且避開全部 avoidedNodes 的組合路徑。
    CombinationalPath findAnyCombinationalPathThroughAvoiding(
        const std::string& startNet,
        const std::string& endNet,
        const std::vector<PathNode>& requiredNodes,
        const std::vector<PathNode>& avoidedNodes) const;

    // --- C：列出全部符合條件的實際路徑 ---

    // 列出 startNet 到 endNet 的所有組合路徑；大型電路可能產生大量結果。
    std::vector<CombinationalPath> enumerateCombinationalPaths(
        const std::string& startNet,
        const std::string& endNet) const;

    // 列出所有避開 avoidedNodes 中全部節點的組合路徑。
    std::vector<CombinationalPath> enumerateCombinationalPathsAvoiding(
        const std::string& startNet,
        const std::string& endNet,
        const std::vector<PathNode>& avoidedNodes) const;

    // 列出所有經過 requiredNodes 中全部節點的組合路徑。
    std::vector<CombinationalPath> enumerateCombinationalPathsThrough(
        const std::string& startNet,
        const std::string& endNet,
        const std::vector<PathNode>& requiredNodes) const;

    // 列出所有經過全部 requiredNodes 且避開全部 avoidedNodes 的組合路徑。
    std::vector<CombinationalPath> enumerateCombinationalPathsThroughAvoiding(
        const std::string& startNet,
        const std::string& endNet,
        const std::vector<PathNode>& requiredNodes,
        const std::vector<PathNode>& avoidedNodes) const;

    // --- D：判斷所有原有路徑是否都滿足多節點條件 ---

    // 判斷 startNet 到 endNet 的每一條組合路徑是否都經過所有 requiredNodes。
    // 若兩點原本沒有路徑，不採用 vacuous truth，而是回傳 false。
    bool everyPathPassesThrough(const std::string& startNet,
                                const std::string& endNet,
                                const std::vector<PathNode>& requiredNodes) const;

    // 判斷 startNet 到 endNet 的每一條組合路徑是否都避開所有 avoidedNodes。
    // 若兩點原本沒有路徑，不採用 vacuous truth，而是回傳 false。
    bool everyPathAvoids(const std::string& startNet,
                         const std::string& endNet,
                         const std::vector<PathNode>& avoidedNodes) const;

    // --- E：找到符合條件的最長組合邏輯路徑 ---

    //  最長路徑的布林判斷 (借用findAnyCombinationalPath，有路徑 = 有最長路徑)
    
    CombinationalPath findLongestCombinationalPath(
        const std::string& startNet,
        const std::string& endNet) const;

    CombinationalPath findLongestCombinationalPathAvoiding(
        const std::string& startNet,
        const std::string& endNet,
        const std::vector<PathNode>& avoidedNodes) const;

    CombinationalPath findLongestCombinationalPathThrough(
        const std::string& startNet,
        const std::string& endNet,
        const std::vector<PathNode>& requiredNodes) const;

    CombinationalPath findLongestCombinationalPathThroughAvoiding(
        const std::string& startNet,
        const std::string& endNet,
        const std::vector<PathNode>& requiredNodes,
        const std::vector<PathNode>& avoidedNodes) const;

    // --- F：確保 endNet 為終點 (Endpoint) 的路徑搜尋 API ---
    // --- 第一組：回傳 Bool 的存在性檢查 ---
    bool hasCombinationalPathToEndpoint(
        const std::string& startNet, const std::string& endNet) const {
        if (!isEndpoint(PathNode(PathNodeType::Net, endNet))) return false;
        return hasCombinationalPath(startNet, endNet);
    }

    bool hasCombinationalPathAvoidingToEndpoint(
        const std::string& startNet, const std::string& endNet, const std::vector<PathNode>& avoidedNodes) const {
        if (!isEndpoint(PathNode(PathNodeType::Net, endNet))) return false;
        return hasCombinationalPathAvoiding(startNet, endNet, avoidedNodes);
    }

    bool hasCombinationalPathThroughToEndpoint(
        const std::string& startNet, const std::string& endNet, const std::vector<PathNode>& requiredNodes) const {
        if (!isEndpoint(PathNode(PathNodeType::Net, endNet))) return false;
        return hasCombinationalPathThrough(startNet, endNet, requiredNodes);
    }

    bool hasCombinationalPathThroughAvoidingToEndpoint(
        const std::string& startNet, const std::string& endNet, 
        const std::vector<PathNode>& requiredNodes, const std::vector<PathNode>& avoidedNodes) const {
        if (!isEndpoint(PathNode(PathNodeType::Net, endNet))) return false;
        return hasCombinationalPathThroughAvoiding(startNet, endNet, requiredNodes, avoidedNodes);
    }

    // --- 第二組：使用 BFS 尋找單一捷徑 ---

    CombinationalPath findAnyCombinationalPathToEndpoint(
        const std::string& startNet, const std::string& endNet) const {
        if (!isEndpoint(PathNode(PathNodeType::Net, endNet))) return CombinationalPath();
        return findAnyCombinationalPath(startNet, endNet);
    }

    CombinationalPath findAnyCombinationalPathAvoidingToEndpoint(
        const std::string& startNet, const std::string& endNet, const std::vector<PathNode>& avoidedNodes) const {
        if (!isEndpoint(PathNode(PathNodeType::Net, endNet))) return CombinationalPath();
        return findAnyCombinationalPathAvoiding(startNet, endNet, avoidedNodes);
    }

    CombinationalPath findAnyCombinationalPathThroughToEndpoint(
        const std::string& startNet, const std::string& endNet, const std::vector<PathNode>& requiredNodes) const {
        if (!isEndpoint(PathNode(PathNodeType::Net, endNet))) return CombinationalPath();
        return findAnyCombinationalPathThrough(startNet, endNet, requiredNodes);
    }

    CombinationalPath findAnyCombinationalPathThroughAvoidingToEndpoint(
        const std::string& startNet, const std::string& endNet, 
        const std::vector<PathNode>& requiredNodes, const std::vector<PathNode>& avoidedNodes) const {
        if (!isEndpoint(PathNode(PathNodeType::Net, endNet))) return CombinationalPath();
        return findAnyCombinationalPathThroughAvoiding(startNet, endNet, requiredNodes, avoidedNodes);
    }

    // --- 第三組：使用 DFS 窮舉所有路徑 ---

    std::vector<CombinationalPath> enumerateCombinationalPathsToEndpoint(
        const std::string& startNet, const std::string& endNet) const {
        if (!isEndpoint(PathNode(PathNodeType::Net, endNet))) return {};
        return enumerateCombinationalPaths(startNet, endNet);
    }

    std::vector<CombinationalPath> enumerateCombinationalPathsAvoidingToEndpoint(
        const std::string& startNet, const std::string& endNet, const std::vector<PathNode>& avoidedNodes) const {
        if (!isEndpoint(PathNode(PathNodeType::Net, endNet))) return {};
        return enumerateCombinationalPathsAvoiding(startNet, endNet, avoidedNodes);
    }

    std::vector<CombinationalPath> enumerateCombinationalPathsThroughToEndpoint(
        const std::string& startNet, const std::string& endNet, const std::vector<PathNode>& requiredNodes) const {
        if (!isEndpoint(PathNode(PathNodeType::Net, endNet))) return {};
        return enumerateCombinationalPathsThrough(startNet, endNet, requiredNodes);
    }

    std::vector<CombinationalPath> enumerateCombinationalPathsThroughAvoidingToEndpoint(
        const std::string& startNet, const std::string& endNet, 
        const std::vector<PathNode>& requiredNodes, const std::vector<PathNode>& avoidedNodes) const {
        if (!isEndpoint(PathNode(PathNodeType::Net, endNet))) return {};
        return enumerateCombinationalPathsThroughAvoiding(startNet, endNet, requiredNodes, avoidedNodes);
    }

    // --- 第四組：反證法邏輯判斷 ---

    bool everyPathPassesThroughToEndpoint(
        const std::string& startNet, const std::string& endNet, const std::vector<PathNode>& requiredNodes) const {
        if (!isEndpoint(PathNode(PathNodeType::Net, endNet))) return false;
        return everyPathPassesThrough(startNet, endNet, requiredNodes);
    }

    bool everyPathAvoidsToEndpoint(
        const std::string& startNet, const std::string& endNet, const std::vector<PathNode>& avoidedNodes) const {
        if (!isEndpoint(PathNode(PathNodeType::Net, endNet))) return false;
        return everyPathAvoids(startNet, endNet, avoidedNodes);
    }

    // --- 第五組：最長路徑搜尋 ---

    CombinationalPath findLongestCombinationalPathToEndpoint(
        const std::string& startNet, const std::string& endNet) const {
        if (!isEndpoint(PathNode(PathNodeType::Net, endNet))) return CombinationalPath();
        return findLongestCombinationalPath(startNet, endNet);
    }

    CombinationalPath findLongestCombinationalPathAvoidingToEndpoint(
        const std::string& startNet, const std::string& endNet, const std::vector<PathNode>& avoidedNodes) const {
        if (!isEndpoint(PathNode(PathNodeType::Net, endNet))) return CombinationalPath();
        return findLongestCombinationalPathAvoiding(startNet, endNet, avoidedNodes);
    }

    CombinationalPath findLongestCombinationalPathThroughToEndpoint(
        const std::string& startNet, const std::string& endNet, const std::vector<PathNode>& requiredNodes) const {
        if (!isEndpoint(PathNode(PathNodeType::Net, endNet))) return CombinationalPath();
        return findLongestCombinationalPathThrough(startNet, endNet, requiredNodes);
    }

    CombinationalPath findLongestCombinationalPathThroughAvoidingToEndpoint(
        const std::string& startNet, const std::string& endNet, 
        const std::vector<PathNode>& requiredNodes, const std::vector<PathNode>& avoidedNodes) const {
        if (!isEndpoint(PathNode(PathNodeType::Net, endNet))) return CombinationalPath();
        return findLongestCombinationalPathThroughAvoiding(startNet, endNet, requiredNodes, avoidedNodes);
    }

    // =================================================
    // Netlist Mutation / ECO(Engineering Change Order)
    // =================================================

    // 重新命名 Gate。如果舊名字找不到，則回傳 false。
    bool renameGate(const std::string& oldName, const std::string& newName);

    // 重新命名 Net。如果舊名字找不到，則回傳 false。
    bool renameNet(const std::string& oldName, const std::string& newName);

    // 斷開連線：將指定 Gate 的輸入端與指定的 Net 斷開。
    bool disconnectGateInput(const std::string& gateName, const std::string& netName);

    // --- Netlist Transformation (B類) ---
 
    // B1 + B2: 移除所有不影響任何 PO 的 gate 和 net
    // 回傳移除的 gate 數量
    int trimDeadLogic();
 
    // B3: 找出所有連續兩個 NOT gate，把它們消除，直接連線
    // 回傳移除的 inverter pair 數量
    int collapseBackToBackInverters();
 
    // B4: 對 fanout > maxFanout 的 net 插入 buffer
    // 讓每個 gate 的 fanout ≤ maxFanout，預設 maxFanout = 4
    // 回傳插入的 buffer 數量
    int insertBuffersForFanout(int maxFanout = 4);

    // --- Netlist Transformation (C類) ---
 
    // C1: 將整個 netlist 重新建構成只使用 AND 和 NOT gates
    // 使用 De Morgan 定理替換 OR/NAND/NOR/XOR/XNOR/BUF
    // DFF 保留不動
    // 回傳新增的 gate 數量
    int reconstructToAndNot();
 
    // C2: 合併結構等價的 gate（相同 type + 相同 input net 集合）
    // 回傳合併的 gate 數量
    int mergeEquivalentGates();

};
