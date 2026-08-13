#pragma once
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include "include/core/NetlistTypes.h"
#include "include/core/NetlistQueries.h"
#include "include/core/PathTypes.h"
#include "include/core/OptimizationTypes.h"
#include "include/core/EditFlow.h"
#include "include/core/OptimizationFlow.h"
#include "include/core/NetlistEditReport.h"
#include "TransformationReport.h"
#include "include/SATEngine/SatTime.h"

namespace eqeng {
class Primitives;
}

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

    // Bulk rewrite passes may defer pin-level load-list maintenance and rebuild
    // it once at the end. This avoids repeated O(fanout) vector erases while
    // remaining invisible outside a single synchronous mutation call.
    bool deferLoadListMaintenance = false;

    // revision 是 cache freshness 的正確性依據；dirty 只保留為共享診斷狀態。
    // 初值為 true：新建的 Netlist 還沒有對應的 AIG。
    mutable bool dirty_ = true;
    mutable uint64_t revision_ = 0;   // 每次 mutation 遞增；供 snapshot 檢查用

    // One Netlist owns at most one Boolean-analysis cache.  It is lazy and is
    // never copied or moved because Primitives is bound to this exact object.
    mutable std::unique_ptr<eqeng::Primitives> primitives_;

    // Internal backend entry point for Netlist's high-level API implementations.
    // SigRef/AIG objects must never escape into public reports.
    eqeng::Primitives& booleanPrimitives() const;

public:
    Netlist();
    ~Netlist();
    Netlist(const Netlist& other);
    Netlist& operator=(const Netlist& other);
    Netlist(Netlist&& other) noexcept;
    Netlist& operator=(Netlist&& other) noexcept;

    // 所有 mutation 的唯一標記入口。
    // public：TechMapper / DepthOptimizer / MockturtleConverter 等外部 class 或
    // free function 直接改 gates/nets 時也需要呼叫。
    // LLM 與高階 API 不需要知道這存在，也不該主動呼叫。
    void markDirty() { dirty_ = true; ++revision_; }

    // 供 Primitives::ensure_fresh() 使用。
    // LLM 與 高階 API 都不需要知道這些存在，也不該直接呼叫。
    bool     isDirty()  const { return dirty_; }
    uint64_t revision() const { return revision_; }

    // 只有「AIG 已重建完成」時才可呼叫。除了 Primitives 不該有人碰。
    void clearDirty() const { dirty_ = false; }

    // =========================================================================
    // Netlist.h API 分類索引
    //
    // 0. Construction / Writer Access
    //    - parser 建立 netlist、writer 讀 port list 時使用。
    //
    // 1. Low-Level Analysis Helpers
    //    - Basic object query、direct connectivity、function analysis、
    //      cone analysis、path/depth analysis。
    //
    // 2. Low-Level Mutation / Cleanup Helpers
    //    - rename / reconnect / remove / buffer insertion / cleanup execution。
    //
    // 3. Optimization Candidate / Strategy Helpers
    //    - 從 analysis 結果整理 optimization candidate。
    //
    // 4. Unified High-Level Query APIs
    //    - BasicQuery、DirectConnectivityQuery、FunctionQuery（含 symmetry）、FunctionSearchQuery、
    //      ConeQuery、PathQuery。這些是之後給 LLM / prompt dispatch 使用的穩定入口。
    //
    // 分類規則：
    // - const 且只回傳資訊者，優先放 Analysis。
    // - 會修改 gates/nets/connectivity 者，放 Mutation / Cleanup。
    // - 只選目標或建立 plan，不直接修改者，放 Optimization Candidate / Strategy。
    // - 給 LLM 使用的統一入口，放 Unified High-Level Query APIs。
    // =========================================================================
    
    // =========================================================================
    // 0. Construction / Writer Access API
    //
    // 這一層只負責建立 netlist 基本物件，以及讓 writer 讀回 port declaration。
    // Parser / Reader 會使用 add/connect 系列 API 建立圖結構。
    // Writer 會使用 getPrimaryInputs/getPrimaryOutputs 保留原始 port 順序。
    // =========================================================================

    // 取得 primary input declarations；主要給 VerilogWriter 使用。
    const std::vector<Port>& getPrimaryInputs() const { return primaryInputs; }

    // 取得 primary output declarations；主要給 VerilogWriter 使用。
    const std::vector<Port>& getPrimaryOutputs() const { return primaryOutputs; }

    // 新增 primary input port；bus 會建立對應 bit nets。
    void addPrimaryInput(const std::string& portName, int msb = -1, int lsb = -1);

    // 新增 primary output port；bus 會建立對應 bit nets。
    void addPrimaryOutput(const std::string& portName, int msb = -1, int lsb = -1);

    // 新增 gate / DFF instance，回傳 gate ID。
    int addGate(const std::string& name, GateType type);

    // 新增 net，回傳 net ID。
    int addNet(const std::string& name);

    // 標記 net 是否為常數；parser 建立 1'b0 / 1'b1 常數 net 時使用。
    void setNetConst(int netId, bool isConst, int val = -1);

    // 將 net 接到 gate input；DFF 可透過 pinName 保存 D/CK/RN/SN 等 named pin。
    void connectGateInput(int gateId, int netId, const std::string& pinName = "");

    // 將 gate output 接到 net。
    void connectGateOutput(int gateId, int netId);

    // =========================================================================
    // 1. Low-Level Analysis API
    //
    // 這一大區塊只放 read-only analysis helper。
    // 它們負責查詢、traversal、function/depth/path 分析，不直接修改 netlist。
    // 會改變 gates/nets/connectivity 的 API 應放到後面的 Mutation / Cleanup 區塊。
    // =========================================================================

    // =========================================================================
    // 1.1 Basic Netlist / ID / Type Query API
    //
    // 這一層只處理最基本的 netlist 物件查詢：
    // gate/net/port 數量、ID/name lookup、gate type、PI/PO/DFF/constant 判斷、
    // 以及列出所有 gate/net/port names。
    // 不做 cone traversal、path search、depth analysis 或 optimization。
    // =========================================================================

    // 依 ID 取得 Gate；呼叫者需先確認 id 合法。
    const Gate& getGate(int id) const { return gates[id]; } 
    Gate& getGateMutable(int id) { markDirty(); return gates[id]; }

    // 依 ID 取得 Net；呼叫者需先確認 id 合法。
    const Net& getNet(int id) const { return nets[id]; } 
    Net&  getNetMutable(int id)  { markDirty(); return nets[id]; }

    // 取得 gates vector 的 slot 數；包含 edit 後保留 ID 的 tombstone。
    size_t getGateCount() const { return gates.size(); }

    // 取得 nets vector 的 slot 數；包含 edit 後保留 ID 的 tombstone。
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

    // 依照 gates vector 順序列出所有 active gate instance names。
    std::vector<std::string> getAllGateNames() const;

    // 依照 nets vector 順序列出所有 active net names。
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

    // 將 scalar net 或 bus name 展開成照 index 排序的 net ID 陣列。
    std::vector<int> expandNetToBits(const std::string& name) const;

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

    // 統計指定 gate type 的 gate 數量。
    size_t getGateCountByType(GateType type) const;

    // 列出指定 gate type 的所有 gate ID
    std::vector<int> getGatesByType(GateType type) const;

    // 找出含有 constant input 的 gates；type=UNKNOWN 表示不限定 gate type，constValue=-1 表示不限定 0/1
    std::vector<int> findGatesWithConstInput(
        GateType type = GateType::UNKNOWN,
        int constValue = -1,
        int inputCount = -1) const;

    // 找出含有 constant input 的 gates，並回傳它們的實例名稱 (Instance Name)
    std::vector<std::string> getGateNamesWithConstInput(
        GateType type = GateType::UNKNOWN,
        int constValue = -1,
        int inputCount = -1) const;

    // 計算含有 constant input 的 gates 的數量
    size_t countGatesWithConstInput(
        GateType type = GateType::UNKNOWN,
        int constValue = -1,
        int inputCount = -1) const;

    // 給定特定的 Gate Instance Name，回傳包含其類型與 I/O 連線狀態的格式化字串
    std::string getGateInfo(const std::string& instName) const;

    // =========================================================================
    // 1.2 Direct Connectivity Low-Level Query API
    //
    // 這一層只查「直接相連」的 gate/net 關係，不做 transitive cone 或 path traversal。
    // 之後會作為高階 DirectConnectivityQuery 的底層 helper。
    // =========================================================================
    
    // 取得指定 net 的 driver gate ID；若 net 不存在、無 driver，或 bus 有多個 driver，回傳 -1。
    int getNetDriverGateId(const std::string& netName) const;

    // getNetDriverGateId 的 ID 版本 (多載)
    int getNetDriverGateId(int netId) const;

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

    // 依照 Problem A QA 的 fanout load 定義，回報指定 scalar net 的所有直接負載。
    // 這個 API 不只看 gate input，也會把 primary output connection 算入。
    // DFF input 會依 named pin 分成 D / CK / RN-SN / other。
    FanoutLoadReport getFanoutLoadReport(int netId) const;

    // 依照 Problem A QA 的 fanout load 定義，回報指定 net / bus 的所有直接負載。
    // 若傳入 bus name，會 aggregate 每個 bit 的 fanout load。
    FanoutLoadReport getFanoutLoadReport(const std::string& netName) const;

    // 依照 Problem A QA 的 fanout load 定義，取得指定 net / bus 的總負載數。
    // 注意：這和 getNetLoadGateCount() 不同；本 API 會把 primary output connection 算入。
    size_t getFanoutLoadCount(const std::string& netName) const;

    // 依照 Problem A QA 的 fanout load 定義掃描全設計或所有 primary inputs。
    // maxFanoutLimit >= 0 時會填 violatingReports；primaryInputsOnly=true 時只檢查 PI nets。
    GlobalFanoutReport getGlobalFanoutReport(int maxFanoutLimit = -1,
                                             bool primaryInputsOnly = false,
                                             bool includeZeroFanout = false) const;

    // 判斷全設計是否符合指定 fanout limit；使用 Problem A QA fanout load 定義。
    bool satisfiesFanoutLimit(int maxFanoutLimit) const;

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

    // 取得指定 wire / port / bus 直接 load 到的 gate IDs。
    std::vector<int> getWireLoads(const std::string& wireName) const;

    // 取得指定線路直接驅動的下一層邏輯閘名稱列表
    std::vector<std::string> getWireLoadNames(const std::string& wireName) const;

    // 計算指定線路直接驅動的下一層邏輯閘總數
    size_t getWireLoadCount(const std::string& wireName) const;

    // 取得指定 gate output 直接 fanout 到的 gate IDs。
    std::vector<int> getGateFanout(const std::string& gateInstName) const;

    // 取得指定邏輯閘輸出端直接驅動的下一層邏輯閘名稱列表
    std::vector<std::string> getGateFanoutNames(const std::string& gateInstName) const;

    // 計算指定邏輯閘輸出端直接驅動的下一層邏輯閘總數
    size_t getGateFanoutCount(const std::string& gateInstName) const;

    // =========================================================================
    // 1.3 Boolean / Function Analysis API
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

    void encodeGateToCNF(CaDiCaL::Solver& solver, const Gate& gate) const;

    // 底層 LEC helper：檢查兩個訊號（支援多位寬）是否在所有輸入情況下功能完全相同。
    bool checkEquivalence(const std::string& netA, const std::string& netB) const;

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

    // =========================================================================
    // 1.4 Cone Analysis API
    //
    // 這一層回答 transitive fanin / fanout cone 問題。
    // DFF 是 sequential boundary，cone traversal 不會穿越 DFF。
    // =========================================================================

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
    // 1.5 組合邏輯路徑分析 API (Combinational Path Analysis)
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
    // 7. Path resolver、A 至 F 類 API 與 runPathQuery() 的實作集中於 src/analysis/PathAnalysis.cpp。
    // =========================================================================

    // 型別定義已拆到 PathTypes.h；using alias 保留 Netlist::Xxx 既有寫法。
    using PathNodeType = ::PathNodeType;
    using PathNode = ::PathNode;
    using CombinationalPath = ::CombinationalPath;
    using PathEndpointType = ::PathEndpointType;
    using PathEndpoint = ::PathEndpoint;
    using PathQueryMode = ::PathQueryMode;
    using PathQuery = ::PathQuery;
    using PathQueryResult = ::PathQueryResult;
    using RegisterPathQueryMode = ::RegisterPathQueryMode;
    using RegisterPathQuery = ::RegisterPathQuery;
    using RegisterPathReport = ::RegisterPathReport;

    // --- Path Endpoint Resolver Helpers ---

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

    // =========================================================================
    // 1.6 Depth Analysis API
    //
    // 第一階段只做 depth / level / critical path 分析，不直接修改 netlist。
    // 共通語意：
    // 1. 使用 unit-delay gate model：每個 combinational gate 的 depth cost = 1。
    // 2. PI、DFF.Q、constant net 的 level 視為 0。
    // 3. DFF 是 sequential boundary，不從 DFF input 穿越到 Q。
    // 4. 回傳 -1 代表找不到、無法計算、或遇到不支援的情況。
    // =========================================================================

    // 型別定義已拆到 PathTypes.h；using alias 保留 Netlist::DepthReport 既有寫法。
    using DepthEndpointType = ::DepthEndpointType;
    using DepthReport = ::DepthReport;

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

    // 型別定義已拆到 PathTypes.h；using alias 保留 Netlist::DepthQuery 既有寫法。
    using DepthQueryType = ::DepthQueryType;
    using DepthQuery = ::DepthQuery;
    using DepthReportSet = ::DepthReportSet;

    // 執行統一 DepthQuery；內部只 dispatch 到既有 DepthAnalysis helper。
    DepthReportSet runDepthQuery(const DepthQuery& query) const;

    // =========================================================================
    // 2. Low-Level Mutation / Cleanup API
    //
    // 這一大區塊放會直接修改 netlist graph 的 API，以及與 cleanup pass 緊密相連的
    // candidate finder / checker。
    //
    // 分類規則：
    // - Mutation Primitive：單一步驟改圖，例如 rename、disconnect、remove、rewire。
    // - Transformation Pass：一次套用一批修改，例如 trimDeadLogic、cleanupAllRemovableBuffers。
    // - Candidate Finder：只找可修改目標，名稱通常是 find*/get*，本身不改圖。
    // - Validation / Rollback：修改後檢查或建立 rollback snapshot。
    //
    // 使用會改圖的 API 後，建議搭配 validateStructure() / validateProblemAConstraints()
    // 或後續統一 NetlistEditReport 流程檢查結果。
    // =========================================================================

    // =========================================================================
    // 2.1 Graph Mutation Primitives
    //
    // 這一層是最底層的 ECO 操作，只做單一種 graph 修改。
    // 後續 cleanup / optimization pass 應優先組合這些 primitive，而不是直接手動改 vector。
    // =========================================================================

    // 重新命名 Gate。如果舊名字找不到，則回傳 false。
    bool renameGate(const std::string& oldName, const std::string& newName);
    NetlistEditReport renameGateWithReport(const std::string& oldName, const std::string& newName);

    // 重新命名 Net。如果舊名字找不到，則回傳 false。
    bool renameNet(const std::string& oldName, const std::string& newName);
    NetlistEditReport renameNetWithReport(const std::string& oldName, const std::string& newName);

    // 斷開連線：將指定 Gate 的輸入端與指定的 Net 斷開。
    bool disconnectGateInput(const std::string& gateName, const std::string& netName);
    NetlistEditReport disconnectGateInputWithReport(const std::string& gateName, const std::string& netName);

    // 建立連線：將指定 Gate 的輸入端連接到指定的 Net。
    bool connectGateInput(const std::string& gateName, const std::string& netName, int pinIndex = -1);
    NetlistEditReport connectGateInputWithReport(const std::string& gateName, const std::string& netName, int pinIndex = -1);

    // 斷開連線：將指定 Gate 的輸入與輸出斷開
    bool disconnectAllPins(int gateId);
    // 斷開連線 + 設為 UNKNOWN
    bool removeGate(int gateId);
    NetlistEditReport removeGateWithReport(int gateId);

    // 查詢 gate 是否已被標記為 removed（type == UNKNOWN）。
    bool isGateRemoved(int gateId) const;
 
    // 把 gate 標記為 removed（type = UNKNOWN），同時清理雙向連線。
    bool markGateRemoved(int gateId);
 
    // 把所有 UNKNOWN gate 從 gates vector 中清除，並修正所有 ID。
    // 回傳移除的 gate 數量。
    int compactRemovedGates();

    // 把所有接到 oldNetId 的 gate 改接到 newNetId，同時更新 loadGateIds。
    bool replaceAllLoadsOfNet(int oldNetId, int newNetId);
    NetlistEditReport replaceAllLoadsOfNetWithReport(int oldNetId, int newNetId);

    // ----------------------並非給LLM的API------------------------------------------
    // 移除舊的 PO 中的一個 bit (oldNetId)，改為使用 newNetId
    bool swapPrimaryOutputNet(int oldNetId, int newNetId);

    // 若把 oldNetId 併進 newNetId 會造出組合迴路,回傳 true。
    // 不會修改任何東西,可安全地在決策前呼叫。
    bool wouldCreateCombLoop(int oldNetId, int newNetId) const;

    // 兩條 net 已知功能等價時,回傳一個安全的合併方向 {old, new}。
    // 照這個順序呼叫 mergeNets 保證不會造出組合迴路,也不會毀掉 port 名稱。
    // 兩個方向都不安全時回傳 {-1, -1}。
    std::pair<int, int> safeMergeDirection(int netA, int netB) const;

    // 說明：將 oldNetId 完全短接到 newNetId。
    //       oldNetId 的所有負載 (Load Gates) 都會改接到 newNetId。
    //       如果 oldNetId 是 Primary Output，newNetId 將會繼承其 PO 身份與名稱。
    bool mergeNets(int oldNetId, int newNetId);
    // -----------------------------------------------------------------------------

    // =========================================================================
    // 2.2 Buffer Insertion Transformation API
    //
    // 這一層會插入新的 BUF gate / net，主要用於 fanout constraint 或局部 rewiring。
    // 回傳值通常是成功插入的 buffer 數量。
    // =========================================================================

    // 對 fanout > maxFanout 的 net 插入 buffer
    // 讓每個 gate 的 fanout ≤ maxFanout，預設 maxFanout = 4
    BufferInsertionReport insertBuffersForFanout(int maxFanout = 4);
    NetlistEditReport insertBuffersForFanoutWithReport(int maxFanout = 4);

    // 針對特定的 Net (支援 Bus) 限制其 Fanout，插入 Cascaded Buffer
    BufferInsertionReport insertBuffersForSpecificNet(const std::string& wireName, int maxFanout = 4);
    NetlistEditReport insertBuffersForSpecificNetWithReport(const std::string& wireName, int maxFanout = 4);

    //  解決 D-FF 的 Clock 與 Reset High-Fanout 問題，自動尋找控制線路並建立 Cascaded Buffer Tree
    BufferInsertionReport insertBuffersForDffControl(int maxFanout, bool processClock, bool processReset);
    NetlistEditReport insertBuffersForDffControlWithReport(int maxFanout, bool processClock, bool processReset);

    // 為每個負載加上獨立 Buffer
    BufferInsertionReport insertBuffersOnEachLoad(const std::string& wireName);
    NetlistEditReport insertBuffersOnEachLoadWithReport(const std::string& wireName);

    // 在訊號的驅動端加上單一 Buffer
    BufferInsertionReport insertBufferAtDriver(const std::string& wireName);
    NetlistEditReport insertBufferAtDriverWithReport(const std::string& wireName);

    // 在特定的 Gate 前面增加 Buffer (只阻斷指定的 wire 到該 Gate 的連線)
    BufferInsertionReport insertBufferBeforeGate(const std::string& wireName, const std::string& targetGateName);
    NetlistEditReport insertBufferBeforeGateWithReport(const std::string& wireName, const std::string& targetGateName);

    // 針對某種類型的 Gate，讓它的輸入或輸出都接上 Buffer
    BufferInsertionReport insertBuffersByGateType(GateType type, bool bufferInputs = true, bool bufferOutputs = true);
    NetlistEditReport insertBuffersByGateTypeWithReport(GateType type, bool bufferInputs = true, bool bufferOutputs = true);

    // =========================================================================
    // 2.3 Validation / Rollback Helpers
    //
    // 這一層不應改變原 netlist；validate* 只檢查，cloneForRollback() 回傳 snapshot。
    // 之後若導入 NetlistEditReport / accept-rollback flow，會依賴這一層。
    // =========================================================================
 
    // 檢查 gate/net graph 的雙向指標是否一致。
    bool validateStructure() const;
 
    // 檢查是否符合 Problem A 的限制（PO 有 driver、DFF.Q 有效等）。
    bool validateProblemAConstraints() const;
 
    // 回傳 Netlist 的深拷貝，供 rollback 使用。
    Netlist cloneForRollback() const;

    // 收集修改型 report 共用的設計統計快照。
    NetlistStats collectNetlistStats() const;

    // 計算 before / after 統計差異，供 NetlistEditReport 使用。
    static NetlistDiff diffStats(const NetlistStats& before, const NetlistStats& after);

    // 驗證修改後 netlist；目前只做 structure / Problem A constraint。
    static EditValidationResult validateEditResult(const Netlist& before, const Netlist& after);

    // 建立修改前後的 global critical depth comparison；targetDepth < 0 表示不檢查 target。
    static DepthChange buildDepthChangeReport(
        const Netlist& before,
        const Netlist& after,
        const std::string& endpointName = "",
        int targetDepth = -1);

    // 建立修改型操作的統一 report。read-only query 不使用此格式。
    static NetlistEditReport buildEditReport(
        const Netlist& before,
        const Netlist& after,
        const std::string& operationName,
        NetlistEditOperationKind kind = NetlistEditOperationKind::Unknown);

    // Mark a report as equivalence-certified by a known safe local rule.
    // This is lighter than whole-design SAT and should only be used by wrappers
    // whose implementation has an operation-specific equivalence argument.
    static void certifyEquivalence(
        NetlistEditReport& report,
        EquivalenceCheckMethod method,
        const std::string& message);

    // =========================================================================
    // 2.4 Legacy Cleanup Execution Passes
    //
    // 這些是會直接修改 netlist 的既有一鍵 cleanup / simplification pass。
    // 目前保留作為 legacy pass；新增 pass 時應盡量拆成：
    // - Team A: find candidate / checker
    // - Team B: mutation executor
    // - Team A: NetlistEditReport building blocks
    // =========================================================================
 
    int trimDeadLogic();
    NetlistEditReport trimDeadLogicWithReport();
    int collapseBackToBackInverters();
    NetlistEditReport collapseBackToBackInvertersWithReport();
    // Legacy/internal structural merge alias。名稱不代表 SAT functional equivalence，
    // 對外請使用 mergeStructurallyEquivalentGates()。
    int mergeEquivalentGates();
    NetlistEditReport mergeEquivalentGatesWithReport();

    // =========================================================================
    // 2.5 Buffer Cleanup Building Blocks
    //
    // findAllRemovableBufferGates() 是 analysis/candidate finder；
    // bypassBufferGate() / cleanupAllRemovableBuffers() 會修改 netlist。
    // =========================================================================
 
    // bypass 一個 BUF gate：in -> BUF -> out -> loads  =>  in -> loads
    bool bypassBufferGate(int bufGateId);
 
    // 找出全局所有可以安全 bypass 的 BUF gate ID 列表
    std::vector<int> findAllRemovableBufferGates() const;
 
    // bypass 全局所有可移除的 BUF gate，回傳移除數量
    int cleanupAllRemovableBuffers();
    NetlistEditReport cleanupAllRemovableBuffersWithReport();
 
    // =========================================================================
    // 2.6 Double Inverter Removal Building Blocks
    //
    // findDoubleInverterPairs() 是 analysis/candidate finder；
    // bypassDoubleInverter() 會修改 netlist。
    // =========================================================================
 
    // 找出所有合法的 NOT -> NOT gate 對，回傳 (g1_id, g2_id)
    std::vector<std::pair<int,int>> findDoubleInverterPairs() const;
 
    // bypass NOT(g1) -> NOT(g2)，讓 g1 的 input 直接接到 g2 的 loads
    bool bypassDoubleInverter(int g1id, int g2id);
 
    // =========================================================================
    // 2.7 Constant Propagation Building Blocks
    //
    // isConst* / getConst* 是 analysis helper；
    // simplifyGateWithConstant() 會修改 netlist。
    // =========================================================================
 
    bool isConst0Net(int netId) const;
    bool isConst1Net(int netId) const;
    int getConst0NetId() const;
    int getConst1NetId() const;
 
    // 對單一 gate 做 constant folding；回傳 true 若有簡化
    bool simplifyGateWithConstant(int gateId);
 
    // =========================================================================
    // 2.8 Same-Input Simplification Building Blocks
    //
    // findSameInputGates() 是 analysis/candidate finder；
    // simplifySameInputGate() 會修改 netlist。
    // =========================================================================
 
    // 找出所有兩個 input 相同的 gate（and(a,a) / xor(a,a) 等）
    std::vector<int> findSameInputGates() const;
 
    // 套用 same-input 化簡規則到單一 gate；回傳 true 若有簡化
    bool simplifySameInputGate(int gateId);
 
    // =========================================================================
    // 2.9 Dangling Logic Removal Building Blocks
    //
    // getEssentialGateIdsForTimingEndpoints() / findDanglingGateIds() 是 analysis；
    // removeDanglingLogic() 會修改 netlist。
    // =========================================================================
 
    // 從所有 PO 和 DFF input pin 往回 BFS，回傳所有有用的 gate ID 集合
    std::unordered_set<int> getEssentialGateIdsForTimingEndpoints() const;
 
    // 回傳所有 dangling gate 的 ID（不影響任何 PO 或 DFF input 的 gate）
    std::vector<int> findDanglingGateIds() const;
 
    // 移除所有 dangling gate，回傳移除數量
    int removeDanglingLogic();
    NetlistEditReport removeDanglingLogicWithReport();
 
    // =========================================================================
    // 2.10 Structural Hashing Building Blocks
    //
    // makeStructuralKey() / findStructurallyEquivalentGateGroups() 是 analysis；
    // mergeStructurallyEquivalentGates() 會修改 netlist。
    // =========================================================================
 
    // 為 gate 產生 structural hash key（commutative gate 的 input 先排序）
    std::string makeStructuralKey(int gateId) const;
 
    // 回傳所有結構等價的 gate 群組（每個 group 至少 2 個，group[0] 是 canonical）
    std::vector<std::vector<int>> findStructurallyEquivalentGateGroups() const;
 
    // 合併所有結構等價的 gate 群組，回傳合併數量
    int mergeStructurallyEquivalentGates();
    NetlistEditReport mergeStructurallyEquivalentGatesWithReport();

    // 以 FunctionSearch SAT 等價類合併跨結構但 output function 相同的 gates。
    NetlistEditReport mergeFunctionallyEquivalentGatesWithReport(
        TargetScope scope = TargetScope::WHOLE_NETLIST,
        const std::string& scopeName = "",
        GateType gateTypeFilter = GateType::UNKNOWN,
        size_t simulationPatternCount = 256,
        double timeLimitSeconds = 30.0);

    // =========================================================================
    // B-5: Unique name generator
    // =========================================================================

    // 產生不會和現有 gate 名稱衝突的唯一名稱
    std::string makeUniqueGateName(const std::string& prefix) const;
    // 產生不會和現有 net 名稱衝突的唯一名稱
    std::string makeUniqueNetName(const std::string& prefix) const;
    // 用唯一名稱新增 gate
    int addGateWithUniqueName(const std::string& prefix, GateType type);
    // 用唯一名稱新增 net
    int addNetWithUniqueName(const std::string& prefix);

    // =========================================================================
    // B-4: Gate replacement primitives
    // =========================================================================

    // 把 gate 的所有 load 改接到 sourceNet，gate 本身消失
    bool replaceGateWithNet(int gateId, int sourceNetId);
    NetlistEditReport replaceGateWithNetWithReport(int gateId, int sourceNetId);
    // 把 gate 替換成常數 net
    bool replaceGateWithConstant(int gateId, int constNetId);
    NetlistEditReport replaceGateWithConstantWithReport(int gateId, int constNetId);
    // 把 gate 改寫成 NOT(sourceNet)
    bool replaceGateWithNotOfNet(int gateId, int sourceNetId);
    NetlistEditReport replaceGateWithNotOfNetWithReport(int gateId, int sourceNetId);
    // 新增一個 gate 並讓它驅動 outputNet，回傳新 gate ID
    int createGateDrivingNet(GateType type, const std::vector<int>& inputNetIds,
                            int outputNetId, const std::string& nameHint = "");
    // 把 gate 所有 pin 斷開並標記為 UNKNOWN
    bool removeGateAndDetachPins(int gateId);

    // =========================================================================
    // B-1: Pin-level 精準改線 primitive
    // =========================================================================

    // 斷開指定 gate 的第 pinIndex 個 input pin
    bool disconnectGateInputPin(int gateId, int pinIndex);
    // 把指定 gate 的第 pinIndex 個 input pin 接到 netId
    bool connectGateInputPin(int gateId, int pinIndex, int netId);
    // 把指定 gate 的第 pinIndex 個 input pin 改接到 newNetId
    bool reconnectGateInputPin(int gateId, int pinIndex, int newNetId);
    // 用名稱把指定 gate 的 pinName pin 改接到 newNetName（支援 DFF D/CK/RN/SN）
    bool reconnectGateInputPinByName(const std::string& gateName,
                                    const std::string& pinName,
                                    const std::string& newNetName);

    // =========================================================================
    // B-2: PO-safe output rewrite primitive
    // =========================================================================

    // 把 targetNet 的 driver 換成 newDriverGate（保留 net 名稱與 PO flag）
    bool replaceDriverOfNet(int targetNetId, int newDriverGateId);
    NetlistEditReport replaceDriverOfNetWithReport(int targetNetId, int newDriverGateId);
    // 把 gate 的 output 改接到 newOutputNetId
    bool rewireGateOutputToExistingNet(int gateId, int newOutputNetId);
    NetlistEditReport rewireGateOutputToExistingNetWithReport(int gateId, int newOutputNetId);
    // 保留 PO net，插入新 gate 驅動它
    bool preservePortNetAndReplaceDriver(int poNetId, GateType newGateType,
                                        const std::vector<int>& inputNetIds);
    // 把 targetNet 的功能換成 sourceNet，保留 targetNet 名稱
    bool replaceNetFunctionWithNetKeepingName(int targetNetId, int sourceNetId);
    NetlistEditReport replaceNetFunctionWithNetKeepingNameWithReport(int targetNetId, int sourceNetId);

    // =========================================================================
    // B-3: Net merge / net bypass primitive
    // =========================================================================

    // 把 fromNet 完全合併到 toNet
    bool mergeNetIntoNet(int fromNetId, int toNetId);
    NetlistEditReport mergeNetIntoNetWithReport(int fromNetId, int toNetId);
    // bypass removedNet，讓所有 load 改接到 replacementNet（保留 PO 語意）
    bool bypassNetKeepingPortSemantics(int removedNetId, int replacementNetId);
    NetlistEditReport bypassNetKeepingPortSemanticsWithReport(int removedNetId, int replacementNetId);
    // 把 oldNet 的所有 load 改接到 newNet
    bool redirectAllLoads(int oldNetId, int newNetId, bool allowDuplicateLoads = false);
    NetlistEditReport redirectAllLoadsWithReport(int oldNetId, int newNetId, bool allowDuplicateLoads = false);
    // 如果 net 沒有任何 load 且非 PO/PI/const，移除它
    bool removeNetIfUnused(int netId);
    NetlistEditReport removeNetIfUnusedWithReport(int netId);
    // 掃描所有 net，移除所有未使用的 net
    int removeUnusedNets();
    NetlistEditReport removeUnusedNetsWithReport();

    // =========================================================================
    // B-6: Transaction / rollback primitive
    // =========================================================================
   
    bool restoreFrom(const Netlist& backup);
    bool validateAfterMutation() const;

    // =========================================================================
    // B-7: Cleanup fixpoint runner
    // =========================================================================

    int simplifyAllGatesWithConstants();
    NetlistEditReport simplifyAllGatesWithConstantsWithReport();
    int simplifyGatesWithConstants(
        GateType type,
        int constValue = -1,
        int inputCount = -1);
    NetlistEditReport simplifyGatesWithConstantsWithReport(
        GateType type,
        int constValue = -1,
        int inputCount = -1);
    int simplifyAllSameInputGates();
    NetlistEditReport simplifyAllSameInputGatesWithReport();
    int runLocalSimplificationFixpoint();
    NetlistEditReport runLocalSimplificationFixpointWithReport();
    int runSafeCleanupFixpoint();
    NetlistEditReport runSafeCleanupFixpointWithReport();

    // =========================================================================
    // B-8: compactRemovedGatesWithIdMap
    // =========================================================================

    struct CompactResult {
        int removedGateCount = 0;
        std::unordered_map<int, int> oldToNewGateId;
        std::unordered_map<int, int> newToOldGateId;
    };
    CompactResult compactRemovedGatesWithIdMap();

    // =========================================================================
    // Boolean Expression Extraction API
    //
    // Symbolic traversal：從指定 net 往回展開 fanin cone，
    // 產生人類可讀的 Boolean expression 字串。
    //
    // 共通規則：
    // 1. PI net / constant net / DFF.Q 視為 leaf，直接用 net name。
    // 2. DFF 是 sequential boundary，不穿越。
    // 3. fanout sharing（同一條 net 被多個地方用到）會被 memo 正確處理，
    //    不會重複展開，但字串中會重複出現同一個 net name。
    // 4. expression 格式：AND(a, b)、OR(a, NOT(b))、NAND(x, y) 等。
    // =========================================================================

    // 回傳指定 net 的完整 Boolean expression（對大電路可能很長）。
    // 內部有 depth cap 與輸出字串大小上限（見 Booleanexpression.cpp），一旦觸發，
    // 深於上限的節點會直接以 net name 表示；wasTruncated（若非 nullptr）會被
    // 設成 true，讓呼叫端知道這不是完整、忠實的展開。
    std::string getBooleanExpression(const std::string& netName, bool* wasTruncated = nullptr) const;

    // 用 net ID 版本
    std::string getBooleanExpressionOfNet(int netId) const;

    // 限制展開深度：超過 maxDepth 的節點直接用 net name 替代
    // 適合大電路的部分展開
    std::string getSimplifiedBooleanExpression(
        const std::string& netName, int maxDepth = 10) const;

    // 回傳 netName fanin cone 中所有 PI net 的名稱（排序去重）
    // 用於回答「n12 depends on which primary inputs」
    // 注意：這是 getPrimaryInputSupportBreakdown() 三個桶（real PI / DFF.Q
    // pseudo-PI / undriven leaf）的聯集，不區分種類；要分開來看請改用
    // getPrimaryInputSupportBreakdown()。
    std::vector<std::string> getPrimaryInputsOfNet(const std::string& netName) const;

    // getPrimaryInputsOfNet() 只回傳一個攤平、不分類的 leaf 名單，呼叫端沒辦法
    // 分辨「這是真正的 top-level primary input」還是「這其實是某顆 DFF 的 Q」還是
    // 「這條線根本沒有 driver、只是剛好不是 PI」——這三種在 Problem A 語意下代表
    // 完全不同的事情（真正外部輸入 / 跨 clock cycle 的暫存狀態 / 電路本身可能有
    // 缺陷的懸空訊號）。這個 struct 把同一次 fanin BFS 的結果分成三桶，讓呼叫端
    // 不用另外再查一次 BasicQuery 就能分辨。
    struct PrimaryInputSupport {
        std::vector<std::string> all;               // 三桶的聯集，內容等同 getPrimaryInputsOfNet()
        std::vector<std::string> realPrimaryInputs;  // 真正宣告的 top-level primary input
        std::vector<std::string> dffPseudoInputs;    // DFF.Q pseudo primary input（跨 sequential boundary）
        std::vector<std::string> undrivenLeaves;     // 沒有 driver 且不是 PI（懸空/floating fanin）
    };

    // 跟 getPrimaryInputsOfNet() 走同一次 BFS，但把 leaf 分成三類回傳。
    PrimaryInputSupport getPrimaryInputSupportBreakdown(const std::string& netName) const;


    // =========================================================================
    // 3. Optimization Candidate / Strategy API
    //
    // 這一大區塊只負責 optimization planning / target selection。
    // 它可以讀取 depth / cone / path analysis 結果，整理出 candidate 或可處理目標。
    // 除非明確標成 rewrite pass，這裡的 API 不應直接修改 netlist。
    // =========================================================================

    // =========================================================================
    // 3.1 Optimization Result / Candidate Types
    //
    // 型別定義已拆到 OptimizationTypes.h；using alias 保留 Netlist::Xxx 既有寫法。
    // OptimizationResult 目前是 legacy/transitional format，後續應逐步改成 NetlistEditReport。
    // =========================================================================

    using OptimizationResult = ::OptimizationResult;
    using OptimizationCandidate = ::OptimizationCandidate;
    using OptPassKind = ::OptPassKind;
    using OptQueryRequest = ::OptQueryRequest;
    using OptCandidate = ::OptCandidate;
    using OptQueryReport = ::OptQueryReport;
    using OptApplyRequest = ::OptApplyRequest;
    using EditCommandKind = ::EditCommandKind;
    using EditApplyRequest = ::EditApplyRequest;
    using NetlistStats = ::NetlistStats;
    using NetlistDiff = ::NetlistDiff;
    using EditValidationResult = ::EditValidationResult;
    using NetlistEditReport = ::NetlistEditReport;
    using FunctionalGateMergeRecord = ::FunctionalGateMergeRecord;
    using FunctionalMergeSummary = ::FunctionalMergeSummary;
    using WholeDesignEquivalenceReport = ::WholeDesignEquivalenceReport;
    using NetlistEditOperationKind = ::NetlistEditOperationKind;
    using EquivalenceCheckMethod = ::EquivalenceCheckMethod;

    // =========================================================================
    // 3.2 Depth-Driven Candidate Builder
    //
    // 這一層只負責把 depth analysis 的結果整理成「準備最佳化的目標」。
    // 不直接修改 netlist，也不執行 rewrite。
    // =========================================================================

    // 根據單一 endpoint report 建立 optimization candidate。
    OptimizationCandidate buildOptimizationCandidate(const DepthReport& endpoint,
                                                     int targetDepth) const;

    // 找出所有 depth 大於 targetDepth 的 timing endpoints，並轉成 optimization candidates。
    std::vector<OptimizationCandidate> findOptimizationCandidatesExceedingDepth(
        int targetDepth) const;

    // =========================================================================
    // 3.3 Optimization Target Selection API
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
    // 3.4 High-Level OPT Flow Skeleton
    //
    // 第一版：query 只列候選，不修改 netlist；apply 套用整個 pass 並回傳
    // NetlistEditReport。candidate-specific apply 留給下一版。
    // =========================================================================

    OptQueryReport runOptQuery(const OptQueryRequest& request) const;
    NetlistEditReport runOptApply(const OptApplyRequest& request);
    NetlistEditReport runEditApply(const EditApplyRequest& request);

    // =========================================================================
    // 3.5 Future Optimization Rewrite Pass API
    //
    // 這一層未來會放真正修改 netlist 的 pass。
    // 所有 pass 都應該搭配 structural / function validation，失敗時 rollback。
    // =========================================================================

    // TODO: 實作 buffer cleanup pass，移除可安全 bypass 的 BUF gate。
    // OptimizationResult cleanupBufferChain(const OptimizationCandidate& candidate);

    // =========================================================================
    // 3.6 Future Optimization Validation / Rollback API
    //
    // 結構驗證、Problem A 限制檢查與 rollback snapshot 目前放在 2.3。
    // 未來若加入 optimization-specific accept / rollback flow，應組合：
    // - cloneForRollback()
    // - validateStructure()
    // - validateProblemAConstraints()
    // - checkEquivalence() / future whole-design equivalence checker
    // - NetlistEditReport
    // =========================================================================

    // =========================================================================
    // 4. Unified High-Level Query APIs
    //
    // 以下是之後給 LLM / prompt dispatch 使用的高階穩定入口。
    // 原則：
    // - query struct 描述輸入條件。
    // - report struct 統一保存回傳結果。
    // - run*Query() 只 dispatch 到底層 helper，不直接修改 netlist。
    // =========================================================================

    // =========================================================================
    // 統一 Basic Query API
    //
    // 這一層把 Basic Netlist / ID / Type Query 的低階 helper 包成單一入口。
    // 適合回答 design summary、列出 gate/net/port、查 gate/net/port 基本資訊、
    // gate type 統計、constant input gates、structural issue 等問題。
    // 不做 direct connectivity、cone traversal、path search、depth analysis。
    // =========================================================================

    // 型別定義已拆到 NetlistQueries.h；using alias 保留 Netlist::BasicQuery 既有寫法。
    using BasicQueryType = ::BasicQueryType;
    using BasicQuery = ::BasicQuery;
    using BasicReport = ::BasicReport;

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

    // 型別定義已拆到 NetlistQueries.h；using alias 保留 Netlist::DirectConnectivityQuery 既有寫法。
    using DirectConnectivityQueryType = ::DirectConnectivityQueryType;
    using DirectConnectivityQuery = ::DirectConnectivityQuery;
    using DirectConnectivityReport = ::DirectConnectivityReport;
    using FanoutLoadReport = ::FanoutLoadReport;
    using GlobalFanoutReport = ::GlobalFanoutReport;

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

    // 型別定義已拆到 NetlistQueries.h；using alias 保留 Netlist::FunctionQuery 既有寫法。
    using FunctionQueryType = ::FunctionQueryType;
    using FunctionQuery = ::FunctionQuery;
    using FunctionReport = ::FunctionReport;

    // 執行統一 FunctionQuery；內部只呼叫 Boolean / Function Analysis 底層 helper。
    FunctionReport runFunctionQuery(const FunctionQuery& query) const;

    // =========================================================================
    // 統一 Function Search Query API
    //
    // FunctionQuery 驗證已知 target；FunctionSearchQuery 則在指定 scope 內自行產生
    // signal candidates，再以 simulation 做必要條件篩選、SAT 做最終證明。
    // 本 API 為 read-only，不修改 netlist。
    // =========================================================================

    using FunctionSearchQueryType = ::FunctionSearchQueryType;
    using FunctionSearchMode = ::FunctionSearchMode;
    using FunctionSearchScope = ::FunctionSearchScope;
    using FunctionSearchQuery = ::FunctionSearchQuery;
    using FunctionSearchMatch = ::FunctionSearchMatch;
    using FunctionSearchEquivalenceClass = ::FunctionSearchEquivalenceClass;
    using FunctionSearchReport = ::FunctionSearchReport;

    FunctionSearchReport runFunctionSearchQuery(const FunctionSearchQuery& query) const;

    // =========================================================================
    // 統一 Sequential Pattern Query API
    //
    // 這一層組合既有 DFF pin、cone 與 function helper，推導 D-input 的 register
    // control semantics。辨識 canonical / functional feedback-MUX enable/hold
    // pattern；AND-only data gating 只回 non-match diagnostic。
    // =========================================================================

    using SequentialPatternQueryType = ::SequentialPatternQueryType;
    using DffInputPatternKind = ::DffInputPatternKind;
    using SequentialPatternDetectionMethod = ::SequentialPatternDetectionMethod;
    using SequentialPatternQuery = ::SequentialPatternQuery;
    using DffInputPattern = ::DffInputPattern;
    using DffInputPatternReport = ::DffInputPatternReport;
    using SequentialPatternReportSet = ::SequentialPatternReportSet;

    SequentialPatternReportSet runSequentialPatternQuery(
        const SequentialPatternQuery& query) const;

    // Directed combinational graph query；DFF 作為 boundary，不產生 D-to-Q edge。
    using GraphQueryType = ::GraphQueryType;
    using GraphQuery = ::GraphQuery;
    using GraphReport = ::GraphReport;
    GraphReport runGraphQuery(const GraphQuery& query) const;

    // 比較 this(current) 與 original 的同名 PI、PO 與 DFF.D endpoint Boolean function。
    // 使用 combinational SAT miter；DFF Q 視為 sequential boundary leaf。
    WholeDesignEquivalenceReport checkWholeDesignEquivalence(
        const Netlist& original,
        double totalTimeBudgetSeconds = 240.0) const;

    // =========================================================================
    // 統一 Cone Query API
    //
    // 這一層回答跨多層 reachable / transitive fanin / transitive fanout 問題。
    // 它會回傳 cone 內的 net/gate 集合與數量；可選擇附帶 cone 內長短路徑。
    // DFF 是 sequential boundary，cone traversal 不穿越 DFF。
    // =========================================================================

    // 型別定義已拆到 NetlistQueries.h；using alias 保留 Netlist::ConeQuery 既有寫法。
    using ConeQueryType = ::ConeQueryType;
    using ConeQuery = ::ConeQuery;
    using ConeReport = ::ConeReport;

    // 執行統一 ConeQuery；內部只呼叫 cone helper，不做 startpoint-to-endpoint path query。
    ConeReport runConeQuery(const ConeQuery& query) const;

    // =========================================================================
    // 統一 Startpoint-to-Endpoint Path Query API
    //
    // 這一層把 PI、PO、DFF.Q、DFF.D、Gate output、Gate input、Specific net
    // 這些不同語意的起點/終點統一描述，再由 1.5 的 resolver helper 轉成 net ID。
    // PathQueryMode 決定要做 exists、find any、enumerate、min/max depth、
    // every-path，或 mandatory-node/separator 類查詢。
    // =========================================================================

    // 執行統一 path query；目前支援一般 path、register endpoint preset、
    // mandatory nodes 與 directed separator/cut。
    // 若 mode 尚未支援、端點解析失敗，或 combinationalOnly=false，回傳預設空結果。
    PathQueryResult runPathQuery(const PathQuery& query) const;

    // Legacy/internal register-to-register convenience wrapper。
    // 公開工具應使用 runPathQuery() 的 DffQ/DffD endpoint preset；本函式保留相容性。
    RegisterPathReport runRegisterPathQuery(const RegisterPathQuery& query) const;
};
