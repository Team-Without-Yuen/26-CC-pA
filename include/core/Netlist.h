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

    // =========================================================================
    // Basic Netlist / ID / Type Query API
    //
    // 這一層只處理最基本的 netlist 物件查詢：
    // gate/net/port 數量、ID/name lookup、gate type、PI/PO/DFF/constant 判斷、
    // 以及列出所有 gate/net/port names。
    // 不做 cone traversal、path search、depth analysis 或 optimization。
    // =========================================================================

    // 依 ID 取得 Gate；呼叫者需先確認 id 合法。
    const Gate& getGate(int id) const { return gates[id]; } 

    // 依 ID 取得 Net；呼叫者需先確認 id 合法。
    const Net& getNet(int id) const { return nets[id]; } 

    // 取得 gate 總數；包含 combinational gates 與 DFF。
    size_t getGateCount() const { return gates.size(); }

    // 取得 net 總數；包含 PI/PO/internal/constant net。
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

    // 判斷 gate ID 是否落在 gates vector 的合法範圍內。
    bool isValidGateId(int gateId) const;

    // 判斷 net ID 是否落在 nets vector 的合法範圍內。
    bool isValidNetId(int netId) const;

    // 判斷指定 gate 是否為 DFF；gateId 無效時回傳 false。
    bool isDffGate(int gateId) const;

    // 判斷指定 gate 是否為 combinational gate；DFF/UNKNOWN/無效 ID 回傳 false。
    bool isCombinationalGate(int gateId) const;

    // 判斷指定 net 是否為 Primary Input；netId 無效時回傳 false。
    bool isPrimaryInputNet(int netId) const;

    // 判斷指定 net 是否為 Primary Output；netId 無效時回傳 false。
    bool isPrimaryOutputNet(int netId) const;

    // 判斷指定 net 是否為 constant net；netId 無效時回傳 false。
    bool isConstantNet(int netId) const;

    // 依照 gates vector 順序列出所有 gate instance names。
    std::vector<std::string> getAllGateNames() const;

    // 依照 nets vector 順序列出所有 net names。
    std::vector<std::string> getAllNetNames() const;

    // 依照 port declaration 順序列出 primary input port names。
    std::vector<std::string> getPrimaryInputNames() const;

    // 依照 port declaration 順序列出 primary output port names。
    std::vector<std::string> getPrimaryOutputNames() const;

    // 列出所有 DFF instance names。
    std::vector<std::string> getDffNames() const;

    // 列出所有 combinational gate instance names；不包含 DFF。
    std::vector<std::string> getCombinationalGateNames() const;

    // 取得指定 PI/PO port 的 bit width；找不到 port 時回傳 -1。
    int getPortWidth(const std::string& portName) const;

    // 判斷指定 PI/PO port 是否為 bus；找不到 port 時回傳 false。
    bool isBusPort(const std::string& portName) const;

    // 依照 port bit 順序列出指定 PI/PO port 對應的 net names；找不到時回傳空陣列。
    std::vector<std::string> getPortBitNames(const std::string& portName) const;

    // 列出非 PI、非 constant，且沒有合法 driver gate 的 net names。
    std::vector<std::string> getUndrivenNetNames() const;

    // 列出非 PO、非 constant，且沒有任何 load gate 的 net names。
    std::vector<std::string> getNoLoadNetNames() const;

    // 列出有 undriven 或 no-load 結構問題的 net names；會移除重複。
    std::vector<std::string> getFloatingNetNames() const;

    // 列出 input 或 output 存在無效 / unconnected net ID 的 gate instance names。
    std::vector<std::string> getUnconnectedGateNames() const;

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

    // =========================================================================
    // Direct Connectivity Low-level Query API
    //
    // 這一層只查「直接相連」的 gate/net 關係，不做 transitive cone 或 path traversal。
    // 之後會作為高階 DirectConnectivityQuery 的底層 helper。
    // =========================================================================

    // 取得指定 net 的 driver gate ID；若 net 不存在、無 driver，或 bus 有多個 driver，回傳 -1。
    int getNetDriverGateId(const std::string& netName) const;

    // 取得指定 net / bus 每個 bit 的 driver gate IDs；會移除重複，無 driver 的 bit 會略過。
    std::vector<int> getNetDriverGateIds(const std::string& netName) const;

    // 取得指定 net / bus 的 driver gate names；會移除重複，無 driver 時回傳空陣列。
    std::vector<std::string> getNetDriverGateNames(const std::string& netName) const;

    // 取得指定 scalar net 的 driver gate name；若沒有唯一 driver，回傳空字串。
    std::string getNetDriverGateName(const std::string& netName) const;

    // 取得指定 net / bus 直接 load 到的 gate IDs；語意等同 getWireLoads，但命名改用 Net。
    std::vector<int> getNetLoadGateIds(const std::string& netName) const;

    // 取得指定 net / bus 直接 load 到的 gate names；語意等同 getWireLoadNames。
    std::vector<std::string> getNetLoadGateNames(const std::string& netName) const;

    // 取得指定 net / bus 直接 load 到的 gate 數量；語意等同 getWireLoadCount。
    size_t getNetLoadGateCount(const std::string& netName) const;

    // 取得指定 gate 的所有有效 input net IDs；未連接 input 會被略過。
    std::vector<int> getGateInputNetIds(const std::string& gateInstName) const;

    // 取得指定 gate 的所有有效 input net names；未連接 input 會被略過。
    std::vector<std::string> getGateInputNetNames(const std::string& gateInstName) const;

    // 取得指定 gate 的 output net name；若 gate 不存在或 output 未連接，回傳空字串。
    std::string getGateOutputNetName(const std::string& gateInstName) const;

    // 取得直接驅動指定 gate inputs 的上一層 gate IDs；PI/constant input 沒有 driver 會被略過。
    std::vector<int> getGateFaninGateIds(const std::string& gateInstName) const;

    // 取得直接驅動指定 gate inputs 的上一層 gate names。
    std::vector<std::string> getGateFaninGateNames(const std::string& gateInstName) const;

    // 取得直接驅動指定 gate inputs 的上一層 gate 數量。
    size_t getGateFaninGateCount(const std::string& gateInstName) const;

    // 判斷指定 gate 的任一 input 或 output 是否直接連到指定 net。
    bool isGateDirectlyConnectedToNet(const std::string& gateInstName,
                                      const std::string& netName) const;

    // 判斷指定 net 是否直接連到指定 gate；語意與 isGateDirectlyConnectedToNet 相同但參數順序相反。
    bool isNetDirectlyConnectedToGate(const std::string& netName,
                                      const std::string& gateInstName) const;

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

    // =========================================================================
    // Boolean / Function Analysis API
    //
    // 這一層回答「訊號功能」類問題，例如：
    // 1. 兩條 net / bus 是否功能等價。
    // 2. 某條 scalar net 是否可能為 0 或 1。
    // 3. 某條 scalar net 是否在所有輸入組合下恆為 0 或恆為 1。
    //
    // 共通規則：
    // - 目前以 combinational fanin cone 建 SAT model。
    // - DFF output 視為 pseudo primary input，不穿越 DFF 回到 D pin。
    // - canNetBeValue / isNetConstantFunction 第一版只支援 scalar net。
    // =========================================================================

    // 語意化 wrapper：檢查兩個 net / bus 是否在所有輸入組合下功能等價。
    bool areNetsEquivalent(const std::string& netA, const std::string& netB) const;

    // 使用 SAT 判斷 scalar net 是否存在某組輸入 assignment 可以變成 value；value 只能是 0 或 1。
    bool canNetBeValue(const std::string& netName, int value) const;

    // 使用 SAT 判斷 scalar net 是否在所有輸入 assignment 下都等於 constValue；constValue 只能是 0 或 1。
    bool isNetConstantFunction(const std::string& netName, int constValue) const;

    // 判斷 scalar net 是否在所有輸入組合下都為 0。
    bool isNetAlwaysZero(const std::string& netName) const;

    // 判斷 scalar net 是否在所有輸入組合下都為 1。
    bool isNetAlwaysOne(const std::string& netName) const;

   // --- Cone Analysis ---
    // Transitive Fanin Cone：從 net 往回追到所有 PI（DFF 不穿越）
    ConeResult getTransitiveFaninCone(const std::string& netName) const;
 
    // Transitive Fanout Cone：從 net 往前追到所有 PO（DFF 不穿越）
    ConeResult getTransitiveFanoutCone(const std::string& netName) const;

    // 取得指定 Gate 的 Fanin 邏輯錐
    ConeResult getGateTransitiveFaninCone(const std::string& gateName) const;
    
    // 取得指定 Gate 的 Fanout 邏輯錐
    ConeResult getGateTransitiveFanoutCone(const std::string& gateName) const;

    // 將 ConeResult 內的 net IDs 轉成依 ID 排序的陣列，方便高階 ConeQuery 回傳穩定結果。
    std::vector<int> getConeNetIds(const ConeResult& cone) const;

    // 將 ConeResult 內的 net IDs 轉成 net names；順序與 getConeNetIds() 一致。
    std::vector<std::string> getConeNetNames(const ConeResult& cone) const;

    // 計算 ConeResult 內 net 數量。
    size_t getConeNetCount(const ConeResult& cone) const;

    // 從 ConeResult 內的 net-to-net 邊推回實際參與 cone 的 gate IDs；DFF 邊界不會被算入。
    std::vector<int> getConeGateIds(const ConeResult& cone) const;

    // 將 ConeResult 內參與 cone 的 gate IDs 轉成 gate names；順序與 getConeGateIds() 一致。
    std::vector<std::string> getConeGateNames(const ConeResult& cone) const;

    // 計算 ConeResult 內參與 cone 的 gate 數量。
    size_t getConeGateCount(const ConeResult& cone) const;

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

    // 表示 DepthQuery 要執行哪一種 depth/timing 查詢。
    enum class DepthQueryType {
        SpecificNet,              // 分析單一 net 的最大 depth 與 critical path
        PrimaryOutputs,           // 分析所有 primary output endpoints
        DffD,                     // 分析所有 DFF D-pin endpoints
        GlobalCriticalPath,       // 找出 PO 與 DFF.D 中最深的 timing endpoint
        EndpointsExceedingDepth   // 找出所有 depth 大於 threshold 的 timing endpoints
    };

    // 描述一個 Depth Query；這一層只處理 depth/timing，不處理 function 或 through/avoid path 條件。
    struct DepthQuery {
        DepthQueryType type = DepthQueryType::SpecificNet;
        std::string netName;             // SpecificNet 使用的 endpoint net name
        int threshold = -1;              // EndpointsExceedingDepth 使用的 depth 門檻
        bool includeCriticalPath = true; // false 時回傳 report 會清空 criticalPath，降低資料量
    };

    // 保存 DepthQuery 的統一回傳結果。
    struct DepthReportSet {
        bool ok = false;                 // query 是否成功
        std::string message;             // 給 debug / LLM response 的簡短訊息
        DepthQueryType type = DepthQueryType::SpecificNet;

        std::vector<DepthReport> reports; // 查詢得到的一個或多個 endpoint report
        DepthReport worst;                // reports 中 depth 最大者；GlobalCriticalPath 的主要結果

        int threshold = -1;               // query 使用的 depth 門檻；未使用時為 -1
        size_t count = 0;                 // reports 數量
    };

    // 執行統一 DepthQuery；內部只 dispatch 到既有 DepthAnalysis helper。
    DepthReportSet runDepthQuery(const DepthQuery& query) const;

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
    // 統一 Basic Query API
    //
    // 這一層把 Basic Netlist / ID / Type Query 的低階 helper 包成單一入口。
    // 適合回答 design summary、列出 gate/net/port、查 gate/net/port 基本資訊、
    // gate type 統計、constant input gates、structural issue 等問題。
    // 不做 direct connectivity、cone traversal、path search、depth analysis。
    // =========================================================================

    // 表示 BasicQuery 要執行哪一種基礎查詢。
    enum class BasicQueryType {
        Summary,                 // 回傳 design 規模與 gate type 統計
        ListGates,               // 列出所有 gate names
        ListNets,                // 列出所有 net names
        ListPrimaryInputs,       // 列出所有 primary input port names
        ListPrimaryOutputs,      // 列出所有 primary output port names
        ListDffs,                // 列出所有 DFF instance names
        ListCombinationalGates,  // 列出所有 combinational gate names
        GateInfo,                // 查單一 gate 的基本資訊
        NetInfo,                 // 查單一 net 的基本資訊
        PortInfo,                // 查單一 PI/PO port 的基本資訊
        CountByGateType,         // 統計指定 gate type 數量；UNKNOWN 表示回傳全部統計
        GatesByType,             // 列出指定 gate type 的 gates
        GatesWithConstantInput,  // 列出 input 接 constant 的 gates
        StructuralIssues         // 回報 undriven/no-load/floating/unconnected
    };

    // 描述一個 Basic Query；不同 type 會使用不同欄位。
    struct BasicQuery {
        BasicQueryType type = BasicQueryType::Summary;
        std::string name;                       // GateInfo/NetInfo/PortInfo 使用的物件名稱
        GateType gateType = GateType::UNKNOWN;  // gate type filter；UNKNOWN 表示不限制或列全部
        int constValue = -1;                    // constant input filter：-1 不限制，0 表示 1'b0，1 表示 1'b1
        bool includeIds = true;                 // 回傳 report 時是否填 gateIds/netIds
        bool includeNames = true;               // 回傳 report 時是否填 gateNames/netNames/portNames
    };

    // 保存 BasicQuery 的統一回傳結果；不同 query type 會填不同欄位。
    struct BasicReport {
        bool ok = false;                        // 查詢是否成功；名稱不存在或 type 不合法時為 false
        std::string message;                    // 給 debug / LLM response 使用的簡短訊息

        size_t gateCount = 0;                   // design 或篩選後 gate 數量
        size_t netCount = 0;                    // design 或篩選後 net 數量
        size_t logicalWireCount = 0;            // Verilog declaration 層級的 wire 數量
        size_t primaryInputCount = 0;           // primary input port 數量
        size_t primaryOutputCount = 0;          // primary output port 數量

        int objectId = -1;                      // GateInfo/NetInfo 的 ID
        std::string objectName;                 // GateInfo/NetInfo/PortInfo 的名稱
        std::string typeName;                   // gate type 或 net/port 類型描述
        std::string formattedInfo;              // getGateInfo() 這類人類可讀格式

        bool exists = false;                    // 指定 name 是否存在
        bool isDff = false;                     // GateInfo 使用
        bool isCombinational = false;           // GateInfo 使用
        bool isPrimaryInput = false;            // NetInfo 使用
        bool isPrimaryOutput = false;           // NetInfo 使用
        bool isConstant = false;                // NetInfo 使用
        bool isBus = false;                     // PortInfo 使用
        int portWidth = -1;                     // PortInfo 使用

        std::vector<int> gateIds;               // 查詢得到的 gate IDs
        std::vector<int> netIds;                // 查詢得到的 net IDs
        std::vector<std::string> gateNames;     // 查詢得到的 gate names
        std::vector<std::string> netNames;      // 查詢得到的 net names
        std::vector<std::string> portNames;     // 查詢得到的 port names

        std::map<GateType, int> gateTypeCounts; // 各 gate type 統計

        std::vector<std::string> undrivenNets;      // structural issue：無 driver 的 nets
        std::vector<std::string> noLoadNets;        // structural issue：無 load 的 nets
        std::vector<std::string> floatingNets;      // structural issue：undriven/no-load union
        std::vector<std::string> unconnectedGates;  // structural issue：有未連接 pin 的 gates
    };

    // 執行統一 BasicQuery；內部只呼叫 Basic helper，不做跨層 traversal。
    BasicReport runBasicQuery(const BasicQuery& query) const;

    // =========================================================================
    // 統一 Direct Connectivity Query API
    //
    // 這一層回答 gate/net 的「直接相連」問題：
    // net driver、net loads、gate inputs、gate output、gate immediate fanin/fanout、
    // 以及 gate 和 net 是否直接相連。
    // 不做 transitive cone traversal、path search 或 depth analysis。
    // =========================================================================

    // 表示 DirectConnectivityQuery 要執行哪一種直接連線查詢。
    enum class DirectConnectivityQueryType {
        NetDriver,          // 查某個 net / bus 的直接 driver gate
        NetLoads,           // 查某個 net / bus 直接 load 到哪些 gates
        GateInputs,         // 查某個 gate 的直接 input nets
        GateOutput,         // 查某個 gate 的 output net
        GateFanin,          // 查直接驅動某 gate inputs 的上一層 gates
        GateFanout,         // 查某 gate output 直接 fanout 到哪些 gates
        DirectlyConnected   // 判斷指定 gate 與指定 net 是否直接相連
    };

    // 描述一個 Direct Connectivity Query；不同 type 會使用 gateName / netName。
    struct DirectConnectivityQuery {
        DirectConnectivityQueryType type = DirectConnectivityQueryType::NetDriver;
        std::string gateName;     // GateInputs/GateOutput/GateFanin/GateFanout/DirectlyConnected 使用
        std::string netName;      // NetDriver/NetLoads/DirectlyConnected 使用
        bool includeIds = true;   // 是否填 gateIds/netIds
        bool includeNames = true; // 是否填 gateNames/netNames
    };

    // 保存 DirectConnectivityQuery 的統一回傳結果。
    struct DirectConnectivityReport {
        bool ok = false;                    // query 是否成功；名稱不存在或必要欄位缺失時為 false
        bool exists = false;                // 指定 gate/net 是否存在
        bool connected = false;             // DirectlyConnected 的主要結果
        std::string message;                // 給 debug / LLM response 的簡短訊息

        std::string gateName;               // 查詢指定或解析出的 gate name
        std::string netName;                // 查詢指定或解析出的 net name
        int gateId = -1;                    // 單一 gate ID 結果；沒有唯一 gate 時為 -1
        int netId = -1;                     // 單一 net ID 結果；沒有唯一 net 時為 -1
        size_t count = 0;                   // 結果數量

        std::vector<int> gateIds;           // 查詢結果中的 gate IDs
        std::vector<int> netIds;            // 查詢結果中的 net IDs
        std::vector<std::string> gateNames; // 查詢結果中的 gate names
        std::vector<std::string> netNames;  // 查詢結果中的 net names
    };

    // 執行統一 DirectConnectivityQuery；內部只呼叫 direct connectivity helper。
    DirectConnectivityReport runDirectConnectivityQuery(
        const DirectConnectivityQuery& query) const;

    // =========================================================================
    // 統一 Function Analysis Query API
    //
    // 這一層回答 Boolean function 類問題：
    // 兩個 net / bus 是否功能等價、某個 scalar net 是否可能為 0/1、
    // 或某個 scalar net 是否恆為 0/1。
    // DFF.Q 會被視為 pseudo primary input，不會穿越 DFF 回到 D pin。
    // =========================================================================

    // 表示 FunctionQuery 要執行哪一種功能分析。
    enum class FunctionQueryType {
        Equivalence,       // 檢查 netNameA 與 netNameB 是否功能等價
        CanBeValue,        // 檢查 netNameA 是否存在某組輸入可等於 constValue
        ConstantFunction,  // 檢查 netNameA 是否恆等於 constValue
        AlwaysZero,        // 檢查 netNameA 是否永遠為 0
        AlwaysOne,         // 檢查 netNameA 是否永遠為 1
        TruthStatus        // 回報 netNameA 是 always 0、always 1，或 non-constant
    };

    // 描述一個 Function Analysis Query；不同 type 會使用不同欄位。
    struct FunctionQuery {
        FunctionQueryType type = FunctionQueryType::TruthStatus;
        std::string netNameA;   // 主要分析目標；Equivalence 時代表第一個 net / bus
        std::string netNameB;   // Equivalence 使用的第二個 net / bus
        int constValue = -1;    // CanBeValue / ConstantFunction 使用；只能是 0 或 1
    };

    // 保存 FunctionQuery 的統一回傳結果。
    struct FunctionReport {
        bool ok = false;              // query 是否成功；名稱不存在或參數不合法時為 false
        bool exists = false;          // 對 yes/no 問題的主要布林答案
        std::string message;          // 給 debug / LLM response 的簡短訊息

        std::string netNameA;         // 查詢目標 A
        std::string netNameB;         // 查詢目標 B
        int netIdA = -1;              // scalar net A 的 ID；bus 或不存在時可為 -1
        int netIdB = -1;              // scalar net B 的 ID；bus 或不存在時可為 -1
        int constValue = -1;          // query 指定或推導出的常數值；unknown 時為 -1

        bool equivalent = false;      // Equivalence 結果
        bool canBeZero = false;       // TruthStatus / CanBeValue 輔助資訊
        bool canBeOne = false;        // TruthStatus / CanBeValue 輔助資訊
        bool isConstant = false;      // TruthStatus 結果；always 0 或 always 1 時為 true
        std::string status;           // "ALWAYS_ZERO"、"ALWAYS_ONE"、"NON_CONSTANT" 或錯誤描述
    };

    // 執行統一 FunctionQuery；內部只呼叫 Boolean / Function Analysis 底層 helper。
    FunctionReport runFunctionQuery(const FunctionQuery& query) const;

    // =========================================================================
    // 統一 Cone Query API
    //
    // 這一層回答跨多層 reachable / transitive fanin / transitive fanout 問題。
    // 它會回傳 cone 內的 net/gate 集合與數量；可選擇附帶 cone 內長短路徑。
    // DFF 是 sequential boundary，cone traversal 不穿越 DFF。
    // =========================================================================

    // 表示 ConeQuery 要從哪種物件與方向建立 transitive cone。
    enum class ConeQueryType {
        NetTransitiveFanin,   // 從指定 net 往 fanin 方向追溯
        NetTransitiveFanout,  // 從指定 net 往 fanout 方向展開
        GateTransitiveFanin,  // 從指定 gate output net 往 fanin 方向追溯
        GateTransitiveFanout  // 從指定 gate output net 往 fanout 方向展開
    };

    // 描述一個 Cone Query；net 類 query 使用 netName，gate 類 query 使用 gateName。
    struct ConeQuery {
        ConeQueryType type = ConeQueryType::NetTransitiveFanin;
        std::string netName;       // NetTransitiveFanin / NetTransitiveFanout 使用
        std::string gateName;      // GateTransitiveFanin / GateTransitiveFanout 使用
        bool includeIds = true;    // 是否填 netIds/gateIds/rootNetIds
        bool includeNames = true;  // 是否填 netNames/gateNames/rootNetNames
        bool includeLocalPaths = false; // 是否附帶 cone 內 longest/shortest net path
    };

    // 保存 ConeQuery 的統一回傳結果。
    struct ConeReport {
        bool ok = false;                       // query 是否成功
        bool exists = false;                   // 指定 net/gate 是否存在並成功建立 cone
        std::string message;                   // 給 debug / LLM response 的簡短訊息

        ConeQueryType type = ConeQueryType::NetTransitiveFanin;
        std::string sourceName;                // 使用者指定的來源名稱
        int sourceId = -1;                     // 來源 net ID 或 gate ID
        ConeResult cone;                       // 原始 cone 結果，供底層演算法繼續使用

        size_t netCount = 0;                   // cone 內有效 net 數量
        size_t gateCount = 0;                  // cone 內有效 combinational gate 數量
        std::vector<int> rootNetIds;           // cone roots，支援 bus 多 root
        std::vector<std::string> rootNetNames; // root net names
        std::vector<int> netIds;               // cone 內 net IDs
        std::vector<int> gateIds;              // cone 內 gate IDs
        std::vector<std::string> netNames;     // cone 內 net names
        std::vector<std::string> gateNames;    // cone 內 gate names

        int longestDepth = -1;                 // cone 內 longest net path depth
        int shortestDepth = -1;                // cone 內 shortest net path depth
        std::vector<int> longestPathNetIds;    // cone 內 longest path 的 net IDs
        std::vector<int> shortestPathNetIds;   // cone 內 shortest path 的 net IDs
        std::vector<std::string> longestPathNetNames;  // cone 內 longest path 的 net names
        std::vector<std::string> shortestPathNetNames; // cone 內 shortest path 的 net names
    };

    // 執行統一 ConeQuery；內部只呼叫 cone helper，不做 startpoint-to-endpoint path query。
    ConeReport runConeQuery(const ConeQuery& query) const;

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
