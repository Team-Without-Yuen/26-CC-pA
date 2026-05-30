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

// 將 GateType 轉成大寫字串，主要給報告與 debug 輸出使用
std::string gateTypeToString(GateType type);

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
    int rootNetId = -1;
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

    // return which gate input pins are connected to a wire (Wire/PI/PO) (support for multi-bit signals)
    std::vector<int> getWireLoads(const std::string& wireName) const;

    // return which gate input pins are connected to a gate output
    std::vector<int> getGateFanout(const std::string& gateInstName) const;

    // Count the number of specific types of logic gates
    size_t getGateCountByType(GateType type) const;

    // 將字串展開成照 index 排序的 Net ID 陣列 (處理單一 bit 或 Bus)
    std::vector<int> expandNetToBits(const std::string& name) const;

    // --- Logic Equivalence Checking (LEC) ---
    // 檢查兩個訊號（支援多位寬）是否在所有輸入情況下功能完全相同
    bool checkEquivalence(const std::string& netA, const std::string& netB) const;

    /* 
        future work...
    */
   std::pair<int, std::vector<std::string>> getLongestPath(const std::string& startNet, const std::string& endNet) const;

   // --- Cone Analysis ---
    // Transitive Fanin Cone：從 net 往回追到所有 PI（DFF 不穿越）
    ConeResult getTransitiveFaninCone(const std::string& netName) const;
 
    // Transitive Fanout Cone：從 net 往前追到所有 PO（DFF 不穿越）
    ConeResult getTransitiveFanoutCone(const std::string& netName) const;
     
    // 第三種 / 第六種：找所有路徑（DFF 不穿越）
    // requirePItoPort = true  → 第六種（驗證起點是 PI、終點是 PO）
    // requirePItoPort = false → 第三種（不限定）
    std::vector<std::vector<std::string>> getAllPaths(
        const std::string& startNet,
        const std::string& endNet,
        const std::vector<std::string>& avoidNets = std::vector<std::string>(),
        bool requirePItoPort = false
    ) const;

};
