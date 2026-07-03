#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// =========================================================================
// Core Netlist Data Types
//
// 這個 header 只放 netlist graph 的純資料型別。
// 不放 parser、analysis、mutation、optimization API。
// 其他 header 若只需要 Gate/Net/Port/ConeResult 型別，應優先 include 本檔。
// =========================================================================

// Defines the supported types of logic gates and sequential elements.
enum class GateType {
    AND, OR, NAND, NOR, NOT, BUF, XOR, XNOR, DFF, UNKNOWN
};

// Represents a physical wire (net) connecting different components in the circuit.
struct Net {
    int id;                       // Unique index in the Netlist's nets vector.
    std::string name;             // The name of the wire, e.g. "n1" or "clk".

    int driverGateId;             // The gate driving this net; -1 if PI or undriven.
    std::vector<int> loadGateIds; // Gate IDs that receive this net as input.

    bool isPI = false;            // Whether this net is a primary input.
    bool isPO = false;            // Whether this net is a primary output.
    bool isConst = false;         // Whether this net is a constant.
    bool isRemoved = false;       // Tombstone used by cleanup passes without shifting net IDs.
    int constVal = -1;            // Constant value 0/1; -1 means not a constant.

    Net(int _id, const std::string& _name)
        : id(_id), name(_name), driverGateId(-1) {
    }
};

// Represents an I/O port declaration, which can be scalar or bus.
struct Port {
    std::string name;         // Base name of the port.
    int msb;                  // Most significant bit index; -1 for scalar.
    int lsb;                  // Least significant bit index; -1 for scalar.

    std::vector<int> netIds;  // Physical net IDs associated with this port.

    bool isBus() const { return msb != -1 && lsb != -1; }

    Port(const std::string& _name, int _msb = -1, int _lsb = -1)
        : name(_name), msb(_msb), lsb(_lsb) {
    }
};

// Represents an instantiated logic gate or sequential element, e.g. DFF.
struct Gate {
    int id;                              // Unique index in the Netlist's gates vector.
    std::string instName;                // Instance name.
    GateType type;                       // Functional gate type.

    std::vector<int> inputNetIds;        // Net IDs connected to input pins.
    std::vector<std::string> inputPinNames; // Named input pins, mainly for DFF.
    int outputNetId;                     // Output net ID; -1 if unconnected.

    Gate(int _id, const std::string& _name, GateType _type)
        : id(_id), instName(_name), type(_type), outputNetId(-1) {
    }
};

// Cone query result.
//
// netIds:
//   All net IDs inside the cone.
// children:
//   Net-to-net traversal edges. children[A] = {B, C} means B/C are next-level nets.
// rootNetIds:
//   One or more root net IDs; bus queries may have multiple roots.
struct ConeResult {
    std::unordered_set<int> netIds;
    std::unordered_map<int, std::vector<int>> children;
    std::vector<int> rootNetIds;
};
