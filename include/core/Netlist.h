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

    // 用於標記常數線的 API 
    void setNetConst(int netId, bool isConst);

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

    // --- E2：找到符合條件的最短組合邏輯路徑 ---

    // 找到 startNet 到 endNet 的最短組合路徑；不存在時回傳空路徑。
    CombinationalPath findShortestCombinationalPath(
        const std::string& startNet,
        const std::string& endNet) const;

    // 找到避開全部 avoidedNodes 的最短組合路徑。
    CombinationalPath findShortestCombinationalPathAvoiding(
        const std::string& startNet,
        const std::string& endNet,
        const std::vector<PathNode>& avoidedNodes) const;

    // 找到經過全部 requiredNodes 的最短組合路徑。
    CombinationalPath findShortestCombinationalPathThrough(
        const std::string& startNet,
        const std::string& endNet,
        const std::vector<PathNode>& requiredNodes) const;

    // 找到經過全部 requiredNodes 且避開全部 avoidedNodes 的最短組合路徑。
    CombinationalPath findShortestCombinationalPathThroughAvoiding(
        const std::string& startNet,
        const std::string& endNet,
        const std::vector<PathNode>& requiredNodes,
        const std::vector<PathNode>& avoidedNodes) const;

    // --- Sequential Boundary Path Query ---

    // 取得指定 DFF instance 的 named input pin 所連接的 net ID，例如 "D" 或 "CK"。
    // 若 gateId 無效、該 gate 不是 DFF、pinName 不存在，或該 pin 未連線，回傳 -1。
    int getDffInputNetId(int dffGateId, const std::string& pinName) const;

    // 掃描所有 Primary Input 到所有 DFF D-pin 的組合路徑，回傳最大邏輯深度與對應路徑。
    // 若設計中沒有 DFF，或不存在任何 PI -> DFF.D 組合路徑，回傳 {-1, emptyPath}。
    std::pair<int, CombinationalPath> getMaximumLogicDepthFromPiToDffD() const;

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

    // 建立連線：將指定 Gate 的輸入端連接到指定的 Net。
    bool connectGateInput(const std::string& gateName, const std::string& netName, int pinIndex = -1);

    bool disconnectAllPins(int gateId);
    // 斷開連線 + 設為 UNKNOWN
    bool removeGate(int gateId);

    // 將整個 netlist 重新建構成只使用 AND 和 NOT gates
    // 使用 De Morgan 定理替換 OR/NAND/NOR/XOR/XNOR/BUF
    // DFF 保留不動
    // 回傳新增的 gate 數量
    int reconstructToAndNot();

    // =================================================
    // Netlist Optimization
    // =================================================

    // 移除所有不影響任何 PO 的 gate 和 net
    // 回傳移除的 gate 數量
    int trimDeadLogic();
 
    // 找出所有連續兩個 NOT gate，把它們消除，直接連線
    // 回傳移除的 inverter pair 數量
    int collapseBackToBackInverters();
 
    // 對 fanout > maxFanout 的 net 插入 buffer
    // 讓每個 gate 的 fanout ≤ maxFanout，預設 maxFanout = 4
    // 回傳插入的 buffer 數量
    int insertBuffersForFanout(int maxFanout = 4);

    // 合併結構等價的 gate（相同 type + 相同 input net 集合）
    // 回傳合併的 gate 數量
    int mergeEquivalentGates();

    // =========================================================================
    // Depth Analysis API
    //
    // 第一階段只做 depth / level / critical path 分析，不直接修改 netlist。
    // 共通語意：
    // 1. 使用 unit-delay gate model：每個 combinational gate 的 depth cost = 1。
    // 2. PI、DFF.Q、constant net 的 level 視為 0。
    // 3. DFF 是 sequential boundary，不從 DFF input 穿越到 Q。
    // 4. 回傳 -1 代表找不到、無法計算、或遇到不支援的情況。
    // =========================================================================

    // 標記 DepthReport 的 endpoint 類型，方便回覆 prompt 與後續 optimization selector 使用。
    enum class DepthEndpointType {
        Unknown,
        SpecificNet,
        PrimaryOutput,
        DffD
    };

    // 保存單一 endpoint 的 depth 分析結果。
    struct DepthReport {
        DepthEndpointType endpointType = DepthEndpointType::Unknown; // endpoint 的來源類型
        std::string endpointName;        // 可讀名稱，例如 y、n10、ff1.D
        int endpointNetId = -1;          // 被分析的 endpoint net ID
        int depth = -1;                  // 到該 endpoint 的最大 combinational depth
        CombinationalPath criticalPath;  // 到該 endpoint 的一條 critical path
    };

    // 計算每個 net 的 combinational level；vector index 對應 net ID。
    std::vector<int> computeNetLevels() const;

    // 計算每個 gate output 的 combinational level；vector index 對應 gate ID。
    std::vector<int> computeGateLevels() const;

    // 查詢指定 net 的最大 fanin depth；找不到或無法計算時回傳 -1。
    int getMaxDepthToNet(const std::string& netName) const;

    // 找到到指定 net 的一條 critical path；不存在時回傳空路徑。
    CombinationalPath findCriticalPathToNet(const std::string& netName) const;

    // 分析指定 net 的 depth、endpoint net ID 與 critical path。
    DepthReport analyzeDepthToNet(const std::string& netName) const;

    // 分析所有 primary outputs 的 depth。
    std::vector<DepthReport> analyzePrimaryOutputDepths() const;

    // 列出所有 depth 大於 limit 的 primary output 名稱。
    std::vector<std::string> getPrimaryOutputsWithDepthGreaterThan(int limit) const;

    // 分析所有 DFF D-pin 的 depth。
    std::vector<DepthReport> analyzeDffDDepths() const;

    // 列出所有 D-pin depth 大於 limit 的 DFF instance name。
    std::vector<std::string> getDffsWithDDepthGreaterThan(int limit) const;

    // 在所有 primary output 與 DFF D-pin endpoints 中，找出 depth 最大的 critical path。
    DepthReport findGlobalCriticalPath() const;

    // 找出所有 depth 大於 maxDepth 的 timing endpoints；endpoint 範圍包含 PO 與 DFF.D。
    std::vector<DepthReport> findEndpointsExceedingDepth(int maxDepth) const;

    // =========================================================================
    // Optimization Result API
    //
    // 這一層定義 optimization pass 的共用回傳格式。
    // 之後每個真正會修改 netlist 的 pass 都應該用這個結果描述：
    // 有沒有修改、depth 是否改善、是否做過等價檢查、是否接受或 rollback。
    // =========================================================================

    // 保存單次 optimization pass 嘗試的結果。
    struct OptimizationResult {
        bool changed = false;            // netlist 是否真的被修改
        bool depthImproved = false;      // 修改後 depth 是否變小
        bool equivalenceChecked = false; // 是否已做等價檢查
        bool equivalent = false;         // 等價檢查是否通過
        int oldDepth = -1;               // 修改前 endpoint depth
        int newDepth = -1;               // 修改後 endpoint depth
        std::string passName;            // pass 名稱
        std::string message;             // 給 debug / LLM response 的說明
    };

    // =========================================================================
    // Optimization Candidate API
    //
    // 這一層只負責把 depth analysis 的結果整理成「準備最佳化的目標」。
    // 不直接修改 netlist，也不執行 rewrite。
    // =========================================================================

    // 保存單一 depth optimization candidate。
    struct OptimizationCandidate {
        DepthReport endpoint;            // 要被最佳化的 endpoint 分析結果
        int targetDepth = -1;            // 希望最佳化後達到的 depth 上限
        ConeResult faninCone;            // endpoint 的 transitive fanin cone
        CombinationalPath criticalPath;  // endpoint 目前的一條 critical path
    };

    // 根據單一 endpoint report 建立 optimization candidate。
    OptimizationCandidate buildOptimizationCandidate(const DepthReport& endpoint,
                                                     int targetDepth) const;

    // 找出所有 depth 大於 targetDepth 的 timing endpoints，並轉成 optimization candidates。
    std::vector<OptimizationCandidate> findOptimizationCandidatesExceedingDepth(
        int targetDepth) const;

    // =========================================================================
    // Optimization Target Selection API
    //
    // 這一層只負責從 candidate / critical path 裡找出「可能可以處理的 gate」。
    // 不直接修改 netlist，也不做等價檢查。
    // =========================================================================

    // 找出 candidate critical path 上所有 BUF gate；只偵測，不判斷是否安全可移除。
    std::vector<int> findBufferGatesOnCriticalPath(
        const OptimizationCandidate& candidate) const;

    // 找出 candidate critical path 上第一版可安全 bypass 的 BUF gate；只偵測，不修改 netlist。
    std::vector<int> findRemovableBufferGatesOnCriticalPath(
        const OptimizationCandidate& candidate) const;

    // =========================================================================
    // Optimization Rewrite Pass API
    //
    // 這一層未來會放真正修改 netlist 的 pass。
    // 所有 pass 都應該搭配 structural / function validation，失敗時 rollback。
    // =========================================================================

    // TODO: 實作 buffer cleanup pass，移除可安全 bypass 的 BUF gate。
    // OptimizationResult cleanupBufferChain(const OptimizationCandidate& candidate);

    // =========================================================================
    // Optimization Validation / Rollback API
    //
    // 這一層未來會放結構驗證、限制驗證、功能等價檢查，以及 rollback 流程。
    // =========================================================================

    // TODO: 檢查 gate/net graph 是否一致，例如 driver/load 關係是否同步。
    // bool validateStructure() const;

    // TODO: 檢查 transformation 後是否仍符合 Problem A 的 restricted primitive netlist 規則。
    // bool validateProblemAConstraints() const;

    struct PathEndpoint;

    // =========================================================================
    // Startpoint-to-Endpoint Resolver Helpers
    //
    // 這些 helper 負責把常見的抽象起點/終點轉成實際 net ID。
    // 它們是未來統一 PathQuery API 的底層 building blocks。
    // =========================================================================

    // 取得所有 Primary Input 對應的 net ID；bus 會展開成每一個 bit net。
    std::vector<int> getPrimaryInputNetIds() const;

    // 取得所有 Primary Output 對應的 net ID；bus 會展開成每一個 bit net。
    std::vector<int> getPrimaryOutputNetIds() const;

    // 取得指定 DFF 的 Q/output net ID；若 gateId 無效、不是 DFF 或未連線，回傳 -1。
    int getDffOutputNetId(int dffGateId) const;

    // 取得指定 gate 的 output net ID；若 gateId 無效或 output 未連線，回傳 -1。
    int getGateOutputNetId(int gateId) const;

    // 依 positional input index 取得指定 gate 的 input net ID；若 index 無效回傳 -1。
    int getGateInputNetId(int gateId, int pinIndex) const;

    // 依 named input pin 取得指定 gate 的 input net ID；若 pinName 不存在回傳 -1。
    int getGateInputNetId(int gateId, const std::string& pinName) const;

    // 將單一抽象 PathEndpoint 解析成實際 net ID 陣列；解析失敗時回傳空陣列。
    std::vector<int> resolvePathEndpoint(const PathEndpoint& endpoint) const;

    // 將多個抽象 PathEndpoint 解析成實際 net ID 陣列；會自動移除重複 net ID。
    std::vector<int> resolvePathEndpoints(const std::vector<PathEndpoint>& endpoints) const;




//高階API放置區------------------------------------------------------------------------------------------------------------------------------
    //以下為高階API，之後給LLM用的。

    // =========================================================================
    // 未來統一 Startpoint-to-Endpoint Path Query API 草稿
    //
    // 這個區塊先獨立放在最下面，作為後續逐步整理 path analysis 的規格草稿。
    // 目前既有功能仍使用上方 A/B/C/D/E/F 類 API；這裡先不急著替換既有實作。
    //
    // 設計目標：
    // 1. 把 PI、PO、DFF.Q、DFF.D、Gate output、Gate input、Specific net
    //    這些不同語意的起點/終點統一描述。
    // 2. 之後由 resolver helper 將 PathEndpoint 轉成實際 net ID。
    // 3. 再依 PathQueryMode 決定要做 exists、find any、enumerate、min/max depth
    //    或 every-path 類查詢。
    // =========================================================================

    // 表示 path query 的抽象起點或終點類型。
    enum class PathEndpointType {
        SpecificNet,     // 指定某條 net，例如 "n16"
        PrimaryInput,    // primary input，可表示單一 PI 或整個 PI bus
        PrimaryOutput,   // primary output，可表示單一 PO 或整個 PO bus
        DffQ,            // DFF 的 Q 輸出 pin，通常作為 register-to-* 的 startpoint
        DffD,            // DFF 的 D 輸入 pin，通常作為 *-to-register 的 endpoint
        DffClock,        // DFF clock pin，例如 CK；偏 control path
        DffReset,        // DFF reset/set pin，例如 RN/SN；偏 control path
        GateOutput,      // 指定 gate 的 output net
        GateInput        // 指定 gate 的某個 input net
    };

    // 描述一個抽象 path 起點或終點；實際查詢前要先 resolve 成 net ID。
    struct PathEndpoint {
        PathEndpointType type;
        std::string name;       // net name、port name、gate instance name 或 DFF instance name
        std::string pinName;    // named pin 使用，例如 "D"、"Q"、"CK"、"RN"
        int pinIndex = -1;      // positional gate input 使用，例如 input 0 / input 1

        // 建立一個 path endpoint；不同 type 會使用不同欄位。
        PathEndpoint(PathEndpointType _type,
                     const std::string& _name,
                     const std::string& _pinName = "",
                     int _pinIndex = -1)
            : type(_type), name(_name), pinName(_pinName), pinIndex(_pinIndex) {
        }
    };

    // 表示同一組 startpoints/endpoints 要執行哪一種 path query。
    enum class PathQueryMode {
        Exists,             // 是否至少存在一條符合條件的路徑
        FindAny,            // 回傳任意一條符合條件的路徑
        EnumerateAll,       // 回傳所有符合條件的路徑
        MinDepth,           // 回傳最短邏輯深度路徑
        MaxDepth,           // 回傳最長邏輯深度路徑
        EveryPathThrough,   // 判斷所有路徑是否都經過 requiredNodes
        EveryPathAvoids     // 判斷所有路徑是否都避開 avoidedNodes
    };

    // 描述一個完整的 startpoint-to-endpoint path query。
    struct PathQuery {
        std::vector<PathEndpoint> startpoints;  // 一個或多個抽象起點
        std::vector<PathEndpoint> endpoints;    // 一個或多個抽象終點
        std::vector<PathNode> requiredNodes;    // 每條符合條件的路徑必須經過的 net/gate
        std::vector<PathNode> avoidedNodes;     // 每條符合條件的路徑必須避開的 net/gate
        PathQueryMode mode = PathQueryMode::Exists;
        bool combinationalOnly = true;          // true 時遇到 DFF 視為 sequential boundary
    };

    // 保存統一 path query 的結果；不同 mode 會使用不同欄位。
    struct PathQueryResult {
        bool exists = false;                    // exists/every-path 類查詢的主要結果
        int depth = -1;                         // min/max depth 類查詢的邏輯深度
        CombinationalPath path;                 // find any/min/max depth 的代表路徑
        std::vector<CombinationalPath> paths;   // enumerate all 的所有路徑
    };

    // 執行統一 path query；目前支援 Exists、FindAny、EnumerateAll、MinDepth、MaxDepth、
    // EveryPathThrough、EveryPathAvoids。
    // 若 mode 尚未支援、端點解析失敗，或 combinationalOnly=false，回傳預設空結果。
    PathQueryResult runPathQuery(const PathQuery& query) const;

//-----------------------------------------------------------------------------------------------------------------------------------
};
