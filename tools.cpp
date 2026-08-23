#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "include/core/Netlist.h"
#include "include/io/VerilogReader.h"
#include "include/io/VerilogWriter.h"

namespace {

enum class ToolStatus {
    Ok,
    NoChange,
    Partial,
    Timeout,
    Unsupported,
    Error
};

struct ToolSession {
    Netlist current;
    std::optional<Netlist> original;
    std::optional<Netlist> lastEditBaseline;
    std::optional<Netlist::NetlistEditReport> lastEditReport;
    VerilogReader reader;
    VerilogWriter writer;
    std::string loadedFilePath;
    size_t designRevision = 0;
    size_t pathArtifactSequence = 0;
    size_t functionExpressionArtifactSequence = 0;
    size_t functionSearchArtifactSequence = 0;
    size_t sequentialArtifactSequence = 0;
    size_t listArtifactSequence = 0;
    bool designLoaded = false;
};

struct ToolResponse {
    bool ok = false;
    ToolStatus status = ToolStatus::Error;
    std::string command;
    std::string mode;
    std::string message;
    bool complete = false;
};

constexpr size_t kMaximumResponseTokenCount = 4096;
constexpr size_t kResponseEnvelopeTokenReserve = 256;
constexpr size_t kConservativeCharactersPerToken = 3;
constexpr size_t kConservativeListEntryTokenOverhead = 2;

size_t saturatingAdd(size_t value, size_t increment) {
    if (increment > std::numeric_limits<size_t>::max() - value) {
        return std::numeric_limits<size_t>::max();
    }
    return value + increment;
}

struct ListArtifactSection {
    std::string title;
    std::vector<std::string> entries;
    size_t generatedEntryCount = 0;
    std::function<std::string(size_t)> generateEntry;

    size_t size() const {
        return generateEntry ? generatedEntryCount : entries.size();
    }
};

struct ListArtifactContent {
    std::vector<std::pair<std::string, std::string>> fields;
    std::vector<ListArtifactSection> sections;

    size_t entryCount() const {
        size_t count = 0;
        for (const ListArtifactSection& section : sections) {
            count = saturatingAdd(count, section.size());
        }
        return count;
    }

    size_t estimatedSerializedCharacters() const {
        size_t characters = 128;
        for (const auto& field : fields) {
            characters = saturatingAdd(characters, field.first.size());
            characters = saturatingAdd(characters, field.second.size());
            characters = saturatingAdd(characters, 6);
        }
        for (const ListArtifactSection& section : sections) {
            characters = saturatingAdd(characters, section.title.size());
            characters = saturatingAdd(characters, 32);
            if (section.generateEntry) {
                for (size_t i = 0; i < section.generatedEntryCount; ++i) {
                    const std::string entry = section.generateEntry(i);
                    characters = saturatingAdd(characters, entry.size());
                    characters = saturatingAdd(characters, 3);
                }
            } else {
                for (const std::string& entry : section.entries) {
                    characters = saturatingAdd(characters, entry.size());
                    characters = saturatingAdd(characters, 3);
                }
            }
        }
        return characters;
    }
};

struct ListArtifactResult {
    bool attempted = false;
    bool complete = false;
    bool wroteFile = false;
    size_t entryCount = 0;
    size_t estimatedCharacterCount = 0;
    size_t estimatedTokenCount = 0;
    size_t responseTokenLimit = kMaximumResponseTokenCount;
    bool triggeredByTokenEstimate = false;
    std::string format = "QUERY_LIST_ARTIFACT_V1";
    std::string outputFilePath;
    std::string message;
};

std::string toolStatusName(ToolStatus status) {
    switch (status) {
        case ToolStatus::Ok: return "ok";
        case ToolStatus::NoChange: return "no_change";
        case ToolStatus::Partial: return "partial";
        case ToolStatus::Timeout: return "timeout";
        case ToolStatus::Unsupported: return "unsupported";
        case ToolStatus::Error:
        default:
            return "error";
    }
}

void emitToolResponse(const ToolSession& session,
                      const ToolResponse& response,
                      const std::function<void()>& writeData = {}) {
    std::cout << "TOOL_RESULT_BEGIN\n";
    std::cout << "ok: " << (response.ok ? "true" : "false") << "\n";
    std::cout << "status: " << toolStatusName(response.status) << "\n";
    std::cout << "command: " << response.command << "\n";
    if (!response.mode.empty()) {
        std::cout << "mode: " << response.mode << "\n";
    }
    std::cout << "message: " << response.message << "\n";
    std::cout << "design_revision: " << session.designRevision << "\n";
    std::cout << "complete: " << (response.complete ? "true" : "false") << "\n";
    if (writeData) {
        std::cout << "data:\n";
        writeData();
    }
    std::cout << "TOOL_RESULT_END\n";
}

void emitToolError(const ToolSession& session,
                   const std::string& command,
                   const std::string& mode,
                   const std::string& message) {
    ToolResponse response;
    response.command = command;
    response.mode = mode;
    response.message = message;
    emitToolResponse(session, response);
}

// 將字串轉小寫，讓 CLI mode/type 可以接受大小寫混用。
std::string toLower(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

// 判斷 token 是否為整數，用於 gate_in:<gate>:<index>。
bool isIntegerToken(const std::string& text) {
    if (text.empty()) {
        return false;
    }
    size_t start = (text[0] == '-' || text[0] == '+') ? 1 : 0;
    if (start == text.size()) {
        return false;
    }
    for (size_t i = start; i < text.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) {
            return false;
        }
    }
    return true;
}

bool isOptionToken(const std::string& text) {
    return !text.empty() && text.front() == '-';
}

bool parseNonNegativeSize(const std::string& text, size_t& value) {
    if (text.empty()) {
        return false;
    }
    for (char ch : text) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) {
            return false;
        }
    }
    try {
        size_t parsedCharacters = 0;
        const unsigned long long parsed = std::stoull(text, &parsedCharacters, 10);
        if (parsedCharacters != text.size() ||
            parsed > static_cast<unsigned long long>(std::numeric_limits<size_t>::max())) {
            return false;
        }
        value = static_cast<size_t>(parsed);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool parsePositiveFiniteSeconds(const std::string& text, double& value) {
    try {
        size_t parsedCharacters = 0;
        const double parsed = std::stod(text, &parsedCharacters);
        if (parsedCharacters != text.size() || !std::isfinite(parsed) || parsed <= 0.0) {
            return false;
        }
        value = parsed;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

// 依 delimiter 切字串；這裡只用於解析 CLI endpoint/node token。
std::vector<std::string> split(const std::string& text, char delimiter) {
    std::vector<std::string> parts;
    std::string current;
    std::istringstream iss(text);
    while (std::getline(iss, current, delimiter)) {
        parts.push_back(current);
    }
    if (!text.empty() && text.back() == delimiter) {
        parts.emplace_back();
    }
    return parts;
}

// 清掉 CLI 參數前後空白。
std::string trim(std::string text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.erase(text.begin());
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.pop_back();
    }
    return text;
}

// read/write 檔名可能包含空白；吃掉整行剩餘內容並移除外層引號。
std::string readRestPath(std::istringstream& iss) {
    std::string path;
    std::getline(iss >> std::ws, path);
    path = trim(path);
    if (path.size() >= 2 &&
        ((path.front() == '"' && path.back() == '"') ||
         (path.front() == '\'' && path.back() == '\''))) {
        path = path.substr(1, path.size() - 2);
    }
    return path;
}

// 建立不覆寫既有檔案的 Path Query artifact 名稱；輸出位置為目前工作目錄。
std::string makeAutomaticPathOutputPath(ToolSession& session) {
    namespace fs = std::filesystem;

    std::string designStem = session.loadedFilePath.empty()
        ? "design"
        : fs::path(session.loadedFilePath).stem().string();
    for (char& ch : designStem) {
        const unsigned char value = static_cast<unsigned char>(ch);
        if (!std::isalnum(value) && ch != '_' && ch != '-') {
            ch = '_';
        }
    }
    if (designStem.empty()) {
        designStem = "design";
    }

    while (true) {
        ++session.pathArtifactSequence;
        const std::string candidate = designStem + "_path_query_" +
                                      std::to_string(session.pathArtifactSequence) +
                                      ".txt";
        std::error_code error;
        const bool exists = fs::exists(candidate, error);
        if (error || !exists) {
            return candidate;
        }
    }
}

// 建立不覆寫既有檔案的 Function Search artifact 名稱。
std::string makeAutomaticFunctionSearchOutputPath(ToolSession& session) {
    namespace fs = std::filesystem;

    std::string designStem = session.loadedFilePath.empty()
        ? "design"
        : fs::path(session.loadedFilePath).stem().string();
    for (char& ch : designStem) {
        const unsigned char value = static_cast<unsigned char>(ch);
        if (!std::isalnum(value) && ch != '_' && ch != '-') {
            ch = '_';
        }
    }
    if (designStem.empty()) {
        designStem = "design";
    }

    while (true) {
        ++session.functionSearchArtifactSequence;
        const std::string candidate = designStem + "_function_search_" +
                                      std::to_string(
                                          session.functionSearchArtifactSequence) +
                                      ".txt";
        std::error_code error;
        const bool exists = fs::exists(candidate, error);
        if (error || !exists) {
            return candidate;
        }
    }
}

// 建立不覆寫既有檔案的 Sequential Pattern artifact 名稱。
std::string makeAutomaticSequentialOutputPath(ToolSession& session) {
    namespace fs = std::filesystem;

    std::string designStem = session.loadedFilePath.empty()
        ? "design"
        : fs::path(session.loadedFilePath).stem().string();
    for (char& ch : designStem) {
        const unsigned char value = static_cast<unsigned char>(ch);
        if (!std::isalnum(value) && ch != '_' && ch != '-') {
            ch = '_';
        }
    }
    if (designStem.empty()) {
        designStem = "design";
    }

    while (true) {
        ++session.sequentialArtifactSequence;
        const std::string candidate = designStem + "_sequential_query_" +
                                      std::to_string(
                                          session.sequentialArtifactSequence) +
                                      ".txt";
        std::error_code error;
        const bool exists = fs::exists(candidate, error);
        if (error || !exists) {
            return candidate;
        }
    }
}

// 印出 string 陣列；所有高階 query report 的 name list 都共用這個輸出。
void printStringList(const std::string& title,
                     const std::vector<std::string>& values) {
    std::cout << title << " (" << values.size() << "):\n";
    for (const std::string& value : values) {
        std::cout << "  " << value << "\n";
    }
}

// 印出 gate ID 陣列對應的 instance names；fanout pin-level report 會用到。
void printGateIdList(const Netlist& netlist,
                     const std::string& title,
                     const std::vector<int>& gateIds) {
    std::cout << title << " (" << gateIds.size() << "):\n";
    for (int gateId : gateIds) {
        if (netlist.isValidGateId(gateId)) {
            std::cout << "  " << netlist.getGate(gateId).instName << "\n";
        }
    }
}

// 印出 fanout report 中的 net name 清單。
void printFanoutNetList(const std::string& title,
                        const std::vector<Netlist::FanoutLoadReport>& reports) {
    std::cout << title << " (" << reports.size() << "):\n";
    for (const Netlist::FanoutLoadReport& report : reports) {
        std::cout << "  " << report.netName
                  << " fanout=" << report.totalLoadCount << "\n";
    }
}

// 印出 path 的 net/gate 序列，供 path/depth 類 query 共用。
void printPath(const Netlist& netlist, const Netlist::CombinationalPath& path) {
    if (!path.exists()) {
        std::cout << "  (no path)\n";
        return;
    }

    std::cout << "  Depth: " << path.depth() << "\n";
    std::cout << "  Nets:\n";
    for (int netId : path.netIds) {
        std::cout << "    " << netlist.getNet(netId).name << "\n";
    }
    std::cout << "  Gates:\n";
    for (int gateId : path.gateIds) {
        std::cout << "    " << netlist.getGate(gateId).instName << "\n";
    }
}

// 將 CLI node token 轉成 PathNode。
// 語法：gate:<name>、net:<name>，沒有前綴時預設視為 net。
Netlist::PathNode parsePathNode(const std::string& token) {
    if (token.rfind("gate:", 0) == 0) {
        return Netlist::PathNode(Netlist::PathNodeType::Gate, token.substr(5));
    }
    if (token.rfind("net:", 0) == 0) {
        return Netlist::PathNode(Netlist::PathNodeType::Net, token.substr(4));
    }
    return Netlist::PathNode(Netlist::PathNodeType::Net, token);
}

// 將 CLI endpoint token 轉成 PathEndpoint。
// 支援 net/pi/po/dff_q/dff_d/dff_clk/dff_reset/gate_out/gate_in。
Netlist::PathEndpoint parseEndpoint(const std::string& token) {
    const std::string loweredToken = toLower(token);
    if (loweredToken == "all_pi" || loweredToken == "pi:*") {
        return Netlist::PathEndpoint(Netlist::PathEndpointType::PrimaryInput, "");
    }
    if (loweredToken == "all_po" || loweredToken == "po:*") {
        return Netlist::PathEndpoint(Netlist::PathEndpointType::PrimaryOutput, "");
    }
    if (loweredToken == "all_dff_q" || loweredToken == "dff_q:*") {
        return Netlist::PathEndpoint(Netlist::PathEndpointType::DffQ, "");
    }
    if (loweredToken == "all_dff_d" || loweredToken == "dff_d:*") {
        return Netlist::PathEndpoint(Netlist::PathEndpointType::DffD, "");
    }

    std::vector<std::string> parts = split(token, ':');
    if (parts.size() < 2) {
        return Netlist::PathEndpoint(Netlist::PathEndpointType::SpecificNet, token);
    }

    const std::string prefix = toLower(parts[0]);
    const std::string name = parts[1];
    const std::string pin = parts.size() >= 3 ? parts[2] : "";

    if (prefix == "net") {
        return Netlist::PathEndpoint(Netlist::PathEndpointType::SpecificNet, name);
    }
    if (prefix == "pi") {
        return Netlist::PathEndpoint(Netlist::PathEndpointType::PrimaryInput, name);
    }
    if (prefix == "po") {
        return Netlist::PathEndpoint(Netlist::PathEndpointType::PrimaryOutput, name);
    }
    if (prefix == "dff_q") {
        return Netlist::PathEndpoint(Netlist::PathEndpointType::DffQ, name);
    }
    if (prefix == "dff_d") {
        return Netlist::PathEndpoint(Netlist::PathEndpointType::DffD, name);
    }
    if (prefix == "dff_clk" || prefix == "dff_clock") {
        return Netlist::PathEndpoint(Netlist::PathEndpointType::DffClock, name, pin);
    }
    if (prefix == "dff_reset" || prefix == "dff_rst") {
        return Netlist::PathEndpoint(Netlist::PathEndpointType::DffReset, name, pin);
    }
    if (prefix == "gate_out") {
        return Netlist::PathEndpoint(Netlist::PathEndpointType::GateOutput, name);
    }
    if (prefix == "gate_in") {
        if (isIntegerToken(pin)) {
            return Netlist::PathEndpoint(Netlist::PathEndpointType::GateInput,
                                         name, "", std::stoi(pin));
        }
        return Netlist::PathEndpoint(Netlist::PathEndpointType::GateInput,
                                     name, pin, pin.empty() ? 0 : -1);
    }

    return Netlist::PathEndpoint(Netlist::PathEndpointType::SpecificNet, token);
}

void addListSection(ListArtifactContent& content,
                    const std::string& title,
                    const std::vector<std::string>& entries) {
    if (!entries.empty()) content.sections.push_back({title, entries});
}

void addGeneratedListSection(
    ListArtifactContent& content,
    const std::string& title,
    size_t entryCount,
    std::function<std::string(size_t)> generateEntry) {
    if (entryCount == 0) return;
    ListArtifactSection section;
    section.title = title;
    section.generatedEntryCount = entryCount;
    section.generateEntry = std::move(generateEntry);
    content.sections.push_back(std::move(section));
}

std::vector<std::string> gateNamesFromIds(
    const Netlist& netlist,
    const std::vector<int>& gateIds);
std::vector<std::string> fanoutReportEntries(
    const std::vector<Netlist::FanoutLoadReport>& reports);
std::string fanoutPredicateName(Netlist::FanoutPredicate predicate);
std::string fanoutScopeName(Netlist::FanoutScope scope);
std::string fanoutRankModeName(Netlist::FanoutRankMode mode);
std::vector<std::string> fanoutRankEntries(
    const std::vector<Netlist::FanoutRankEntry>& entries);
std::string coneRankMetricName(Netlist::ConeRankMetric metric);
std::string coneRankModeName(Netlist::ConeRankMode mode);
std::vector<std::string> coneRankEntries(
    const std::vector<Netlist::ConeRankEntry>& entries);
std::string coneMetricPredicateName(
    Netlist::ConeMetricPredicate predicate);
std::vector<std::string> coneFilterEntries(
    const std::vector<Netlist::ConeFilterEntry>& entries);

std::string formatPinConnection(const PinConnectionSummary& pin) {
    std::string result = pin.pinName + "=";
    if (!pin.connected) return result + "<unconnected>";
    if (pin.isConstant && (pin.constantValue == 0 || pin.constantValue == 1)) {
        result += "1'b" + std::to_string(pin.constantValue);
    } else {
        result += pin.netName;
    }
    if (pin.isPrimaryInput) result += "(PI)";
    if (pin.isPrimaryOutput) result += "(PO)";
    return result;
}

std::string formatGateConnection(const GateConnectionSummary& gate) {
    std::string result = "gate=" + gate.gateName + " type=" + gate.typeName +
                         " inputs=[";
    for (size_t i = 0; i < gate.inputs.size(); ++i) {
        if (i != 0) result += ", ";
        result += formatPinConnection(gate.inputs[i]);
    }
    result += "] output=" + formatPinConnection(gate.output);
    return result;
}

std::string unconnectedPinReasonName(UnconnectedPinReason reason) {
    switch (reason) {
    case UnconnectedPinReason::Unconnected: return "UNCONNECTED";
    case UnconnectedPinReason::InvalidNetId: return "INVALID_NET_ID";
    case UnconnectedPinReason::RemovedNet: return "REMOVED_NET";
    }
    return "UNKNOWN";
}

std::string formatUnconnectedPin(const Netlist& netlist,
                                 const UnconnectedPinSummary& pin) {
    const bool gateValid = netlist.isValidGateId(pin.gateId);
    const Gate* gate = gateValid ? &netlist.getGate(pin.gateId) : nullptr;
    const bool isInput = pin.direction == PinDirection::Input;

    std::string pinName;
    if (isInput) {
        if (gate != nullptr && pin.pinIndex >= 0 &&
            pin.pinIndex < static_cast<int>(gate->inputPinNames.size()) &&
            !gate->inputPinNames[pin.pinIndex].empty()) {
            pinName = gate->inputPinNames[pin.pinIndex];
        } else {
            pinName = "IN" + std::to_string(pin.pinIndex + 1);
        }
    } else {
        pinName = gate != nullptr && gate->type == GateType::DFF ? "Q" : "OUT";
    }

    std::string netName = "<unconnected>";
    if (netlist.isValidNetId(pin.netId)) {
        netName = netlist.getNet(pin.netId).name;
    } else if (pin.netId >= 0) {
        netName = "<invalid-net:" + std::to_string(pin.netId) + ">";
    }

    return "gate_id=" + std::to_string(pin.gateId) +
           " gate=" + (gate != nullptr ? gate->instName : "<invalid-gate>") +
           " type=" + (gate != nullptr ? netlist.gateTypeToString(gate->type)
                                         : "UNKNOWN") +
           " direction=" + (isInput ? "input" : "output") +
           " pin=" + pinName +
           " pin_index=" + std::to_string(pin.pinIndex) +
           " net_id=" + std::to_string(pin.netId) +
           " net=" + netName +
           " reason=" + unconnectedPinReasonName(pin.reason);
}

std::string connectivityPinRoleName(ConnectivityPinRole role) {
    switch (role) {
    case ConnectivityPinRole::Output: return "output";
    case ConnectivityPinRole::CombinationalInput: return "combinational_input";
    case ConnectivityPinRole::DffData: return "data";
    case ConnectivityPinRole::DffClock: return "clock";
    case ConnectivityPinRole::DffResetSet: return "reset_set";
    case ConnectivityPinRole::DffOther: return "dff_other";
    }
    return "unknown";
}

std::string formatConnectivityPinRecord(const ConnectivityPinRecord& record) {
    return "net=" + record.netName +
           " direction=" +
           (record.direction == ConnectivityPinDirection::Driver ? "driver" : "load") +
           " gate=" + record.gateName +
           " type=" + record.gateTypeName +
           " pin=" + record.pinName +
           " role=" + connectivityPinRoleName(record.role);
}

std::string formatGateTypeList(const Netlist& netlist,
                               const std::vector<GateType>& types,
                               const std::string& emptyText) {
    if (types.empty()) return emptyText;
    std::string result;
    for (GateType type : types) {
        if (!result.empty()) result += ", ";
        result += netlist.gateTypeToString(type);
    }
    return result;
}

ListArtifactContent makeBasicListArtifactContent(
    const Netlist& netlist,
    const Netlist::BasicReport& report) {
    ListArtifactContent content;
    content.fields.push_back({"message", report.message});
    if (report.hasGateCount) {
        content.fields.push_back({"gate count", std::to_string(report.gateCount)});
    }
    if (report.hasNetCount) {
        content.fields.push_back({"net count", std::to_string(report.netCount)});
    }
    if (report.hasLogicalWireCount) {
        content.fields.push_back(
            {"logical wire count", std::to_string(report.logicalWireCount)});
    }
    if (report.hasPrimaryInputCount) {
        content.fields.push_back(
            {"primary input count", std::to_string(report.primaryInputCount)});
    }
    if (report.hasPrimaryOutputCount) {
        content.fields.push_back(
            {"primary output count", std::to_string(report.primaryOutputCount)});
    }
    if (report.hasPrimaryInputBitCount) {
        content.fields.push_back(
            {"primary input bit count", std::to_string(report.primaryInputBitCount)});
    }
    if (report.hasPrimaryOutputBitCount) {
        content.fields.push_back(
            {"primary output bit count", std::to_string(report.primaryOutputBitCount)});
    }
    if (report.netClassification.valid) {
        const NetClassificationSummary& summary = report.netClassification;
        content.fields.push_back(
            {"active net count", std::to_string(summary.activeNetCount)});
        content.fields.push_back(
            {"primary-input net count",
             std::to_string(summary.primaryInputNetCount)});
        content.fields.push_back(
            {"primary-output net count",
             std::to_string(summary.primaryOutputNetCount)});
        content.fields.push_back(
            {"primary-input/output net count",
             std::to_string(summary.primaryInputOutputNetCount)});
        content.fields.push_back(
            {"constant net count", std::to_string(summary.constantNetCount)});
        content.fields.push_back(
            {"internal net count", std::to_string(summary.internalNetCount)});
    }
    if (!report.objectName.empty()) {
        content.fields.push_back({"object", report.objectName});
        if (report.objectId >= 0) {
            content.fields.push_back({"object id", std::to_string(report.objectId)});
        }
        if (!report.typeName.empty()) {
            content.fields.push_back({"object type", report.typeName});
        }
    }
    if (report.gateTypeFilterApplied || report.gateTypeExclusionApplied) {
        content.fields.push_back(
            {"scope gate count", std::to_string(report.scopeGateCount)});
        content.fields.push_back(
            {"filtered gate count", std::to_string(report.gateCount)});
        content.fields.push_back({"gate type filter applied",
                                  report.gateTypeFilterApplied ? "yes" : "no"});
        content.fields.push_back({"gate type filters",
            formatGateTypeList(netlist, report.appliedGateTypeFilters, "all")});
        content.fields.push_back({"gate type exclusion applied",
                                  report.gateTypeExclusionApplied ? "yes" : "no"});
        content.fields.push_back({"excluded gate type filters",
            formatGateTypeList(netlist, report.appliedExcludedGateTypeFilters, "none")});
    }
    if (report.gateDetailsIncluded) {
        content.fields.push_back({"gate details included", "yes"});
        content.fields.push_back(
            {"gate detail count", std::to_string(report.gateConnections.size())});
    }

    std::vector<std::string> gateTypeCounts;
    for (const auto& item : report.gateTypeCounts) {
        gateTypeCounts.push_back(netlist.gateTypeToString(item.first) + " : " +
                                 std::to_string(item.second));
    }
    addListSection(content, "Gate type counts", gateTypeCounts);
    std::vector<std::string> gateConnections;
    gateConnections.reserve(report.gateConnections.size());
    for (const GateConnectionSummary& gate : report.gateConnections) {
        gateConnections.push_back(formatGateConnection(gate));
    }
    if (gateConnections.empty()) {
        addListSection(content, "Gate names", report.gateNames);
    } else {
        addListSection(content, "Gate connection details", gateConnections);
    }
    addListSection(content, "Net names", report.netNames);
    if (report.netClassification.valid) {
        const NetClassificationSummary& summary = report.netClassification;
        addListSection(content, "Primary-input nets",
                       summary.primaryInputNetNames);
        addListSection(content, "Primary-output nets",
                       summary.primaryOutputNetNames);
        addListSection(content, "Primary-input/output nets",
                       summary.primaryInputOutputNetNames);
        addListSection(content, "Constant nets", summary.constantNetNames);
        addListSection(content, "Internal nets", summary.internalNetNames);
    }
    if (report.hasUnconnectedPinCounts) {
        content.fields.push_back(
            {"unconnected gate count",
             std::to_string(report.unconnectedGates.size())});
        content.fields.push_back(
            {"unconnected input pin count",
             std::to_string(report.unconnectedInputPinCount)});
        content.fields.push_back(
            {"unconnected output pin count",
             std::to_string(report.unconnectedOutputPinCount)});
    }
    addListSection(content, "Port names", report.portNames);

    std::vector<std::string> ports;
    ports.reserve(report.ports.size());
    for (const PortSummary& port : report.ports) {
        const std::string direction = port.isInput
            ? "input"
            : (port.isOutput ? "output" : "unknown");
        ports.push_back("name=" + port.name + " direction=" + direction +
                        " width=" + std::to_string(port.width) +
                        " is_bus=" + (port.isBus ? "true" : "false") +
                        " msb=" + std::to_string(port.msb) +
                        " lsb=" + std::to_string(port.lsb));
    }
    addListSection(content, "Port summaries", ports);
    addListSection(content, "Undriven nets", report.undrivenNets);
    addListSection(content, "No-load nets", report.noLoadNets);
    addListSection(content, "Floating nets", report.floatingNets);
    addListSection(content, "Unconnected gates", report.unconnectedGates);
    addGeneratedListSection(
        content,
        "Unconnected pin details",
        report.unconnectedPins.size(),
        [&netlist, &report](size_t index) {
            return formatUnconnectedPin(netlist, report.unconnectedPins[index]);
        });
    addListSection(content, "Floating primary-input nets",
                   report.floatingPrimaryInputNets);
    addListSection(content, "Unconnected primary-output nets",
                   report.unconnectedPrimaryOutputNets);
    return content;
}

ListArtifactContent makeConnectivityListArtifactContent(
    const Netlist& netlist,
    const Netlist::DirectConnectivityReport& report) {
    ListArtifactContent content;
    content.fields = {
        {"message", report.message},
        {"gate", report.gateName},
        {"net", report.netName},
        {"count", std::to_string(report.count)}
    };

    if (report.fanoutLoadReport.ok) {
        const Netlist::FanoutLoadReport& fanout = report.fanoutLoadReport;
        content.fields.push_back(
            {"fanout load count", std::to_string(fanout.totalLoadCount)});
        content.fields.push_back(
            {"drives primary output", fanout.drivesPrimaryOutput ? "yes" : "no"});
        content.fields.push_back(
            {"primary output load count",
             std::to_string(fanout.primaryOutputLoadCount)});
        addListSection(content, "Combinational gate input loads",
                       gateNamesFromIds(netlist, fanout.combinationalGateLoads));
        addListSection(content, "DFF D-pin loads",
                       gateNamesFromIds(netlist, fanout.dffDataLoads));
        addListSection(content, "DFF clock-pin loads",
                       gateNamesFromIds(netlist, fanout.dffClockLoads));
        addListSection(content, "DFF reset/set-pin loads",
                       gateNamesFromIds(netlist, fanout.dffResetSetLoads));
        addListSection(content, "DFF other-pin loads",
                       gateNamesFromIds(netlist, fanout.dffOtherLoads));
    }
    if (report.globalFanoutReport.ok) {
        const Netlist::GlobalFanoutReport& global = report.globalFanoutReport;
        content.fields.push_back(
            {"checked nets", std::to_string(global.checkedNetCount)});
        content.fields.push_back(
            {"max fanout", std::to_string(global.maxFanout)});
        if (global.fanoutLimit >= 0) {
            content.fields.push_back(
                {"fanout limit", std::to_string(global.fanoutLimit)});
            content.fields.push_back(
                {"satisfies limit", global.satisfiesLimit ? "yes" : "no"});
        }
        if (global.fanoutFilterApplied) {
            content.fields.push_back(
                {"fanout filter scope",
                 global.primaryInputsOnly ? "pi" : "all"});
            content.fields.push_back(
                {"fanout predicate", fanoutPredicateName(global.fanoutPredicate)});
            content.fields.push_back(
                {"fanout value", std::to_string(global.fanoutValue)});
            if (global.fanoutPredicate ==
                Netlist::FanoutPredicate::BetweenInclusive) {
                content.fields.push_back(
                    {"fanout upper value",
                     std::to_string(global.fanoutUpperValue)});
            }
            content.fields.push_back(
                {"matched net count", std::to_string(global.matchedNetCount)});
        }
        addListSection(content, "Max-fanout nets",
                       fanoutReportEntries(global.maxFanoutReports));
        addListSection(content, "Violating nets",
                       fanoutReportEntries(global.violatingReports));
        if (global.fanoutFilterApplied) {
            addListSection(content, "Matched nets",
                           fanoutReportEntries(global.matchedReports));
        }
    }
    if (report.fanoutRankingReport.ok) {
        const Netlist::FanoutRankingReport& ranking =
            report.fanoutRankingReport;
        content.fields.push_back(
            {"fanout ranking scope", fanoutScopeName(ranking.scope)});
        content.fields.push_back(
            {"fanout ranking mode", fanoutRankModeName(ranking.mode)});
        content.fields.push_back(
            {"requested rank or count",
             std::to_string(ranking.requestedRankOrCount)});
        content.fields.push_back(
            {"checked nets", std::to_string(ranking.checkedNetCount)});
        content.fields.push_back(
            {"distinct fanout levels",
             std::to_string(ranking.distinctFanoutLevelCount)});
        content.fields.push_back(
            {"selected fanout levels",
             std::to_string(ranking.selectedFanoutLevelCount)});
        content.fields.push_back(
            {"result net count", std::to_string(ranking.resultNetCount)});
        content.fields.push_back(
            {"requested rank exists",
             ranking.requestedRankExists ? "yes" : "no"});
        addListSection(content, "Ranked nets",
                       fanoutRankEntries(ranking.rankedReports));
    }
    if (report.pinDetailsIncluded) {
        content.fields.push_back(
            {"pin connection count", std::to_string(report.pinConnectionCount)});
        std::vector<std::string> pinConnections;
        pinConnections.reserve(report.pinConnections.size());
        for (const ConnectivityPinRecord& record : report.pinConnections) {
            pinConnections.push_back(formatConnectivityPinRecord(record));
        }
        addListSection(content, "Pin connection details", pinConnections);
    }
    if (!report.fanoutLoadReport.ok && !report.pinDetailsIncluded) {
        addListSection(content, "Gate names", report.gateNames);
    }
    if (!report.fanoutRankingReport.ok) {
        addListSection(content, "Net names", report.netNames);
    }
    return content;
}

ListArtifactContent makeConeListArtifactContent(
    const Netlist& netlist,
    const Netlist::ConeReport& report) {
    ListArtifactContent content;
    std::string appliedGateTypes = "all";
    if (!report.appliedGateTypeFilters.empty()) {
        appliedGateTypes.clear();
        for (GateType type : report.appliedGateTypeFilters) {
            if (!appliedGateTypes.empty()) appliedGateTypes += ", ";
            appliedGateTypes += netlist.gateTypeToString(type);
        }
    }
    if (report.filterReport.ok) {
        const Netlist::ConeFilterReport& filter = report.filterReport;
        content.fields = {
            {"message", report.message},
            {"output cone filter metric", coneRankMetricName(filter.metric)},
            {"output cone filter predicate",
             coneMetricPredicateName(filter.predicate)},
            {"filter value", std::to_string(filter.value)},
            {"gate type filter applied",
             report.gateTypeFilterApplied ? "yes" : "no"},
            {"gate type filters", appliedGateTypes},
            {"checked primary outputs",
             std::to_string(filter.checkedOutputCount)},
            {"matched output count",
             std::to_string(filter.matchedOutputCount)}
        };
        if (filter.predicate ==
            Netlist::ConeMetricPredicate::BetweenInclusive) {
            content.fields.push_back(
                {"filter upper value", std::to_string(filter.upperValue)});
        }
        addListSection(content, "Matched output cones",
                       coneFilterEntries(filter.matchedOutputs));
        return content;
    }
    content.fields = {
        {"message", report.message},
        {"source", report.sourceName},
        {"second source", report.secondSourceName},
        {"scope gate count", std::to_string(report.scopeGateCount)},
        {"filtered gate count", std::to_string(report.gateCount)},
        {"gate type filter applied", report.gateTypeFilterApplied ? "yes" : "no"},
        {"gate type filters", appliedGateTypes},
        {"gate details included", report.gateDetailsIncluded ? "yes" : "no"},
        {"gate detail count", std::to_string(report.gateConnections.size())},
        {"net count", std::to_string(report.netCount)},
        {"checked primary outputs", std::to_string(report.checkedOutputCount)},
        {"longest local path depth", std::to_string(report.longestDepth)},
        {"shortest local path depth", std::to_string(report.shortestDepth)}
    };

    if (report.rankingReport.ok) {
        const Netlist::ConeRankingReport& ranking = report.rankingReport;
        content.fields.push_back(
            {"output cone ranking metric", coneRankMetricName(ranking.metric)});
        content.fields.push_back(
            {"output cone ranking mode", coneRankModeName(ranking.mode)});
        content.fields.push_back(
            {"requested rank or count",
             std::to_string(ranking.requestedRankOrCount)});
        content.fields.push_back(
            {"distinct cone metric levels",
             std::to_string(ranking.distinctMetricLevelCount)});
        content.fields.push_back(
            {"selected cone metric levels",
             std::to_string(ranking.selectedMetricLevelCount)});
        content.fields.push_back(
            {"result output count",
             std::to_string(ranking.resultOutputCount)});
        content.fields.push_back(
            {"requested rank exists",
             ranking.requestedRankExists ? "yes" : "no"});
        addListSection(content, "Ranked output cones",
                       coneRankEntries(ranking.rankedOutputs));
    }

    std::vector<std::string> gateTypeCounts;
    for (const auto& item : report.gateTypeCounts) {
        gateTypeCounts.push_back(netlist.gateTypeToString(item.first) + " : " +
                                 std::to_string(item.second));
    }
    addListSection(content, "Gate type counts", gateTypeCounts);
    addListSection(content, "Root nets", report.rootNetNames);
    if (report.gateDetailsIncluded) {
        std::vector<std::string> gateConnections;
        gateConnections.reserve(report.gateConnections.size());
        for (const GateConnectionSummary& gate : report.gateConnections) {
            gateConnections.push_back(formatGateConnection(gate));
        }
        addListSection(content, "Gate connection details", gateConnections);
    } else {
        addListSection(content, "Cone gates", report.gateNames);
    }
    addListSection(content, "Cone nets", report.netNames);
    addListSection(content, "Longest local path nets",
                   report.longestPathNetNames);
    addListSection(content, "Shortest local path nets",
                   report.shortestPathNetNames);
    return content;
}

ListArtifactContent makePathListArtifactContent(
    const Netlist& netlist,
    const Netlist::PathQuery& query,
    const Netlist::PathQueryResult& result) {
    ListArtifactContent content;
    if (query.mode == Netlist::PathQueryMode::DirectPiPoConnections) {
        content.fields.emplace_back("total_direct_connections",
                                    std::to_string(result.pathCount));
        content.fields.emplace_back("logic_depth", "0");
        std::vector<std::string> connections;
        connections.reserve(result.paths.size());
        for (size_t index = 0; index < result.paths.size(); ++index) {
            const Netlist::CombinationalPath& path = result.paths[index];
            std::string entry = "connection " + std::to_string(index + 1) + ": ";
            if (path.netIds.empty()) {
                entry += "<empty>";
            } else {
                for (size_t netIndex = 0; netIndex < path.netIds.size(); ++netIndex) {
                    if (netIndex != 0) entry += " -> ";
                    const int netId = path.netIds[netIndex];
                    entry += netlist.isValidNetId(netId)
                        ? netlist.getNet(netId).name
                        : "<invalid-net:" + std::to_string(netId) + ">";
                }
            }
            connections.push_back(std::move(entry));
        }
        addListSection(content, "Direct PI-to-PO connections", connections);
        return content;
    }

    if (query.mode == Netlist::PathQueryMode::FindMandatoryNodes) {
        content.fields.emplace_back("path_exists", result.pathExists ? "yes" : "no");
        content.fields.emplace_back("mandatory_internal_net_count",
                                    std::to_string(result.mandatoryNetNames.size()));
        addListSection(content, "Mandatory internal nets", result.mandatoryNetNames);
        return content;
    }

    if (result.path.exists()) {
        content.fields.emplace_back("depth", std::to_string(result.path.depth()));
        std::vector<std::string> netNames;
        netNames.reserve(result.path.netIds.size());
        for (int netId : result.path.netIds) {
            netNames.push_back(netlist.isValidNetId(netId)
                ? netlist.getNet(netId).name
                : "<invalid-net:" + std::to_string(netId) + ">");
        }
        addListSection(content, "Path nets in start-to-end order", netNames);
        std::vector<std::string> gateNames;
        gateNames.reserve(result.path.gateIds.size());
        for (int gateId : result.path.gateIds) {
            gateNames.push_back(netlist.isValidGateId(gateId)
                ? netlist.getGate(gateId).instName
                : "<invalid-gate:" + std::to_string(gateId) + ">");
        }
        addListSection(content, "Path gates in start-to-end order", gateNames);
    }
    return content;
}

ListArtifactContent makeDepthListArtifactContent(
    const Netlist& netlist,
    const Netlist::DepthReportSet& report) {
    ListArtifactContent content;
    content.fields.emplace_back("report_count", std::to_string(report.count));
    if (report.threshold >= 0) {
        content.fields.emplace_back("threshold", std::to_string(report.threshold));
    }
    if (!report.gateName.empty()) {
        content.fields.emplace_back("gate", report.gateName);
        content.fields.emplace_back("gate_on_critical_path",
                                    report.gateOnCriticalPath ? "yes" : "no");
    }
    if (report.worst.depth >= 0) {
        content.fields.emplace_back("worst_endpoint", report.worst.endpointName);
        content.fields.emplace_back("worst_depth", std::to_string(report.worst.depth));
    }

    std::vector<std::string> endpoints;
    endpoints.reserve(report.reports.size());
    for (const DepthReport& item : report.reports) {
        endpoints.push_back(item.endpointName + " depth=" + std::to_string(item.depth));
    }
    addListSection(content, "Endpoint depth reports", endpoints);

    if (report.worst.criticalPath.exists()) {
        std::vector<std::string> netNames;
        netNames.reserve(report.worst.criticalPath.netIds.size());
        for (int netId : report.worst.criticalPath.netIds) {
            netNames.push_back(netlist.isValidNetId(netId)
                ? netlist.getNet(netId).name
                : "<invalid-net:" + std::to_string(netId) + ">");
        }
        std::vector<std::string> gateNames;
        gateNames.reserve(report.worst.criticalPath.gateIds.size());
        for (int gateId : report.worst.criticalPath.gateIds) {
            gateNames.push_back(netlist.isValidGateId(gateId)
                ? netlist.getGate(gateId).instName
                : "<invalid-gate:" + std::to_string(gateId) + ">");
        }
        addListSection(content, "Worst path nets in start-to-end order", netNames);
        addListSection(content, "Worst path gates in start-to-end order", gateNames);
    }
    return content;
}

ListArtifactContent makeFunctionListArtifactContent(
    const Netlist::FunctionReport& report) {
    ListArtifactContent content;
    content.fields = {
        {"message", report.message},
        {"status", report.status},
        {"target net", report.netNameA},
        {"support leaf count", std::to_string(report.supportPrimaryInputs.size())},
        {"real primary input count",
         std::to_string(report.supportRealPrimaryInputs.size())},
        {"DFF.Q boundary count",
         std::to_string(report.supportDffPseudoInputs.size())},
        {"undriven boundary count",
         std::to_string(report.supportUndrivenLeaves.size())}
    };
    if (!report.symmetryInputNameA.empty() || !report.symmetryInputNameB.empty()) {
        content.fields.emplace_back("symmetry input A", report.symmetryInputNameA);
        content.fields.emplace_back("symmetry input B", report.symmetryInputNameB);
        content.fields.emplace_back("symmetric", report.symmetric ? "yes" : "no");
        content.fields.emplace_back(
            "counterexample found", report.counterexampleFound ? "yes" : "no");
        content.fields.emplace_back(
            "mismatched target bit count",
            std::to_string(report.mismatchedTargetBitNames.size()));
        content.fields.emplace_back(
            "counterexample assignment count",
            std::to_string(report.counterexampleAssignments.size()));
    }

    addListSection(content, "Support leaves", report.supportPrimaryInputs);
    addListSection(content, "Real primary inputs", report.supportRealPrimaryInputs);
    addListSection(content, "DFF.Q pseudo primary inputs",
                   report.supportDffPseudoInputs);
    std::vector<std::string> stateBoundaryMappings;
    stateBoundaryMappings.reserve(report.supportDffStateBoundaries.size());
    for (const auto& boundary : report.supportDffStateBoundaries) {
        stateBoundaryMappings.push_back(
            boundary.stateVariableName + " = " + boundary.dffName + "." +
            boundary.pinName + " (net " + boundary.netName + ")");
    }
    addListSection(content, "DFF.Q current-state variable mappings",
                   stateBoundaryMappings);
    addListSection(content, "Undriven leaves", report.supportUndrivenLeaves);
    addListSection(content, "Mismatched target bits",
                   report.mismatchedTargetBitNames);

    const auto assignmentEntries = [](const auto& assignments) {
        std::vector<std::string> entries;
        entries.reserve(assignments.size());
        for (const auto& item : assignments) {
            entries.push_back(item.first + " = " + std::to_string(item.second));
        }
        std::sort(entries.begin(), entries.end());
        return entries;
    };
    addListSection(content, "Counterexample assignments",
                   assignmentEntries(report.counterexampleAssignments));
    addListSection(content, "Target values before swap (A=0, B=1)",
                   assignmentEntries(report.outputValuesBeforeSwap));
    addListSection(content, "Target values after swap (A=1, B=0)",
                   assignmentEntries(report.outputValuesAfterSwap));
    return content;
}

// 印出 BasicQuery 的統一 report。
void printBasicReport(const Netlist& netlist,
                      const Netlist::BasicReport& report,
                      bool suppressLists = false) {
    if (!report.ok) {
        std::cout << "Error: " << report.message << "\n";
        return;
    }

    std::cout << "OK: " << report.message << "\n";
    if (report.hasGateCount) {
        std::cout << "  gates: " << report.gateCount << "\n";
    }
    if (report.hasNetCount) {
        std::cout << "  nets: " << report.netCount << "\n";
    }
    if (report.hasLogicalWireCount) {
        std::cout << "  logical wires: " << report.logicalWireCount << "\n";
    }
    if (report.hasPrimaryInputCount) {
        std::cout << "  primary inputs: " << report.primaryInputCount << "\n";
    }
    if (report.hasPrimaryOutputCount) {
        std::cout << "  primary outputs: " << report.primaryOutputCount << "\n";
    }
    if (report.hasPrimaryInputBitCount) {
        std::cout << "  primary input bits: " << report.primaryInputBitCount << "\n";
    }
    if (report.hasPrimaryOutputBitCount) {
        std::cout << "  primary output bits: " << report.primaryOutputBitCount << "\n";
    }
    if (report.netClassification.valid) {
        const NetClassificationSummary& summary = report.netClassification;
        std::cout << "  active nets: " << summary.activeNetCount << "\n";
        std::cout << "  primary-input nets: "
                  << summary.primaryInputNetCount << "\n";
        std::cout << "  primary-output nets: "
                  << summary.primaryOutputNetCount << "\n";
        std::cout << "  primary-input/output nets: "
                  << summary.primaryInputOutputNetCount << "\n";
        std::cout << "  constant nets: " << summary.constantNetCount << "\n";
        std::cout << "  internal nets: " << summary.internalNetCount << "\n";
    }
    if (report.hasUnconnectedPinCounts) {
        std::cout << "  unconnected gates: "
                  << report.unconnectedGates.size() << "\n";
        std::cout << "  unconnected input pins: "
                  << report.unconnectedInputPinCount << "\n";
        std::cout << "  unconnected output pins: "
                  << report.unconnectedOutputPinCount << "\n";
    }
    if (report.gateTypeFilterApplied || report.gateTypeExclusionApplied) {
        std::cout << "  scope gates: " << report.scopeGateCount << "\n";
        std::cout << "  filtered gates: " << report.gateCount << "\n";
        std::cout << "  gate type filter applied: "
                  << (report.gateTypeFilterApplied ? "yes" : "no") << "\n";
        std::cout << "  gate type filters: "
                  << formatGateTypeList(
                         netlist, report.appliedGateTypeFilters, "all") << "\n";
        std::cout << "  gate type exclusion applied: "
                  << (report.gateTypeExclusionApplied ? "yes" : "no") << "\n";
        std::cout << "  excluded gate type filters: "
                  << formatGateTypeList(
                         netlist, report.appliedExcludedGateTypeFilters, "none") << "\n";
    }
    if (report.gateDetailsIncluded) {
        std::cout << "  gate details included: yes\n";
        std::cout << "  gate detail count: " << report.gateConnections.size() << "\n";
    }
    if (!report.objectName.empty()) {
        std::cout << "  object: " << report.objectName << "\n";
        if (report.objectId >= 0) {
            std::cout << "  id: " << report.objectId << "\n";
        }
        if (!report.typeName.empty()) {
            std::cout << "  type: " << report.typeName << "\n";
        }
        if (report.portWidth >= 0) {
            std::cout << "  width: " << report.portWidth << "\n";
            std::cout << "  is_bus: " << (report.isBus ? "true" : "false") << "\n";
            std::cout << "  is_primary_input: "
                      << (report.isPrimaryInput ? "true" : "false") << "\n";
            std::cout << "  is_primary_output: "
                      << (report.isPrimaryOutput ? "true" : "false") << "\n";
        }
        if (!report.formattedInfo.empty() && report.gateConnections.empty()) {
            std::cout << report.formattedInfo << "\n";
        }
    }
    if (!report.gateTypeCounts.empty()) {
        std::cout << "Gate type counts:\n";
        for (const auto& item : report.gateTypeCounts) {
            std::cout << "  " << netlist.gateTypeToString(item.first)
                      << " : " << item.second << "\n";
        }
    }
    if (!suppressLists && !report.gateNames.empty() &&
        report.gateConnections.empty()) {
        printStringList("Gate names", report.gateNames);
    }
    if (!suppressLists && !report.gateConnections.empty()) {
        std::cout << "Gate connection details ("
                  << report.gateConnections.size() << "):\n";
        for (const GateConnectionSummary& gate : report.gateConnections) {
            std::cout << "  " << formatGateConnection(gate) << "\n";
        }
    }
    if (!suppressLists && !report.netNames.empty()) {
        printStringList("Net names", report.netNames);
    }
    if (!suppressLists && report.netClassification.valid) {
        const NetClassificationSummary& summary = report.netClassification;
        printStringList("Primary-input nets", summary.primaryInputNetNames);
        printStringList("Primary-output nets", summary.primaryOutputNetNames);
        printStringList("Primary-input/output nets",
                        summary.primaryInputOutputNetNames);
        printStringList("Constant nets", summary.constantNetNames);
        printStringList("Internal nets", summary.internalNetNames);
    }
    if (!suppressLists && !report.portNames.empty()) {
        printStringList("Port names", report.portNames);
    }
    if (!suppressLists && !report.ports.empty()) {
        std::cout << "Port summaries (" << report.ports.size() << "):\n";
        for (const PortSummary& port : report.ports) {
            std::cout << "  - name: " << port.name << "\n";
            std::cout << "    direction: "
                      << (port.isInput ? "input" : port.isOutput ? "output" : "unknown")
                      << "\n";
            std::cout << "    width: " << port.width << "\n";
            std::cout << "    is_bus: " << (port.isBus ? "true" : "false") << "\n";
            std::cout << "    msb: " << port.msb << "\n";
            std::cout << "    lsb: " << port.lsb << "\n";
        }
    }
    if (!suppressLists && !report.undrivenNets.empty()) {
        printStringList("Undriven nets", report.undrivenNets);
    }
    if (!suppressLists && !report.noLoadNets.empty()) {
        printStringList("No-load nets", report.noLoadNets);
    }
    if (!suppressLists && !report.floatingNets.empty()) {
        printStringList("Floating nets", report.floatingNets);
    }
    if (!suppressLists && !report.unconnectedGates.empty()) {
        printStringList("Unconnected gates", report.unconnectedGates);
    }
    if (!suppressLists && !report.unconnectedPins.empty()) {
        std::cout << "Unconnected pin details ("
                  << report.unconnectedPins.size() << "):\n";
        for (const UnconnectedPinSummary& pin : report.unconnectedPins) {
            std::cout << "  " << formatUnconnectedPin(netlist, pin) << "\n";
        }
    }
    if (!suppressLists && !report.floatingPrimaryInputNets.empty()) {
        printStringList(
            "Floating primary-input nets",
            report.floatingPrimaryInputNets);
    }
    if (!suppressLists && !report.unconnectedPrimaryOutputNets.empty()) {
        printStringList(
            "Unconnected primary-output nets",
            report.unconnectedPrimaryOutputNets);
    }
}

// 建立 Structure/Cone 大型清單的自動 artifact 名稱。
std::string makeAutomaticListOutputPath(ToolSession& session,
                                        const std::string& command) {
    namespace fs = std::filesystem;

    std::string designStem = session.loadedFilePath.empty()
        ? "design"
        : fs::path(session.loadedFilePath).stem().string();
    for (char& ch : designStem) {
        const unsigned char value = static_cast<unsigned char>(ch);
        if (!std::isalnum(value) && ch != '_' && ch != '-') ch = '_';
    }
    if (designStem.empty()) designStem = "design";

    std::string commandStem = command;
    for (char& ch : commandStem) {
        const unsigned char value = static_cast<unsigned char>(ch);
        if (!std::isalnum(value) && ch != '_' && ch != '-') ch = '_';
    }
    if (commandStem.empty()) commandStem = "query";

    while (true) {
        ++session.listArtifactSequence;
        const std::string candidate = designStem + "_" + commandStem +
            "_list_" + std::to_string(session.listArtifactSequence) + ".txt";
        std::error_code error;
        const bool exists = fs::exists(candidate, error);
        if (error || !exists) return candidate;
    }
}

ListArtifactResult writeAutomaticListArtifact(
    ToolSession& session,
    const std::string& command,
    const std::string& mode,
    const ListArtifactContent& content) {
    namespace fs = std::filesystem;

    ListArtifactResult result;
    result.entryCount = content.entryCount();
    result.estimatedCharacterCount = content.estimatedSerializedCharacters();
    size_t responseCharacterOverhead = command.size();
    responseCharacterOverhead = saturatingAdd(
        responseCharacterOverhead, mode.size());
    responseCharacterOverhead = saturatingAdd(
        responseCharacterOverhead, session.loadedFilePath.size());
    responseCharacterOverhead = saturatingAdd(responseCharacterOverhead, 128);
    result.estimatedCharacterCount = saturatingAdd(
        result.estimatedCharacterCount, responseCharacterOverhead);
    size_t entryTokenOverhead = 0;
    for (size_t i = 0; i < kConservativeListEntryTokenOverhead; ++i) {
        entryTokenOverhead = saturatingAdd(
            entryTokenOverhead, result.entryCount);
    }
    const size_t serializedTokenEstimate =
        saturatingAdd(result.estimatedCharacterCount,
                      kConservativeCharactersPerToken - 1) /
        kConservativeCharactersPerToken;
    result.estimatedTokenCount = saturatingAdd(
        kResponseEnvelopeTokenReserve,
        saturatingAdd(serializedTokenEstimate, entryTokenOverhead));
    result.triggeredByTokenEstimate =
        result.estimatedTokenCount >= kMaximumResponseTokenCount;
    if (!result.triggeredByTokenEstimate) {
        return result;
    }

    result.attempted = true;
    result.outputFilePath = makeAutomaticListOutputPath(session, command);
    const std::string temporaryPath = result.outputFilePath + ".tmp";
    std::ofstream output(temporaryPath, std::ios::out | std::ios::trunc);
    if (!output.is_open()) {
        result.message = "Failed to create list artifact; full list was printed to terminal.";
        return result;
    }

    output << "# Complete query list artifact\n"
           << "Format: " << result.format << "\n"
           << "Command: " << command << "\n"
           << "Mode: " << mode << "\n"
           << "Loaded design: " << session.loadedFilePath << "\n"
           << "Design revision: " << session.designRevision << "\n\n"
           << "Automatic artifact trigger:\n"
           << "  entry count: " << result.entryCount << "\n"
           << "  estimated serialized characters: "
           << result.estimatedCharacterCount << "\n"
           << "  estimated response tokens: "
           << result.estimatedTokenCount << "\n"
           << "  response token limit: " << result.responseTokenLimit << "\n"
           << "  triggered by token estimate: "
           << (result.triggeredByTokenEstimate ? "yes" : "no") << "\n\n"
           << "Fields:\n";
    for (const auto& field : content.fields) {
        output << "  " << field.first << ": " << field.second << "\n";
    }

    output << "\nLists:\n";
    for (const ListArtifactSection& section : content.sections) {
        output << section.title << " (" << section.size() << "):\n";
        if (section.generateEntry) {
            for (size_t i = 0; i < section.generatedEntryCount; ++i) {
                output << "  " << section.generateEntry(i) << "\n";
            }
        } else {
            for (const std::string& entry : section.entries) {
                output << "  " << entry << "\n";
            }
        }
    }
    output << "\nTotal list entries: " << result.entryCount << "\n"
           << "Complete: yes\n";
    output.flush();
    const bool writeSucceeded = output.good();
    output.close();

    if (!writeSucceeded) {
        std::error_code removeError;
        fs::remove(temporaryPath, removeError);
        result.message = "List artifact I/O failed; full list was printed to terminal.";
        return result;
    }

    std::error_code renameError;
    fs::rename(temporaryPath, result.outputFilePath, renameError);
    if (renameError) {
        std::error_code removeError;
        fs::remove(temporaryPath, removeError);
        result.message = "List artifact finalization failed; full list was printed to terminal.";
        return result;
    }

    result.complete = true;
    result.wroteFile = true;
    result.message = "Complete list data was written to an automatic artifact.";
    return result;
}

void printListArtifactMetadata(const ListArtifactResult& artifact) {
    if (!artifact.attempted) return;
    std::cout << "  list artifact format: " << artifact.format << "\n";
    std::cout << "  list artifact complete: "
              << (artifact.complete ? "yes" : "no") << "\n";
    std::cout << "  list entry count: " << artifact.entryCount << "\n";
    std::cout << "  estimated list characters: "
              << artifact.estimatedCharacterCount << "\n";
    std::cout << "  estimated response tokens: "
              << artifact.estimatedTokenCount << "\n";
    std::cout << "  response token limit: "
              << artifact.responseTokenLimit << "\n";
    std::cout << "  artifact triggered by token estimate: "
              << (artifact.triggeredByTokenEstimate ? "yes" : "no") << "\n";
    std::cout << "  wrote list to file: "
              << (artifact.wroteFile ? "yes" : "no") << "\n";
    if (artifact.wroteFile) {
        std::cout << "  output_file: " << artifact.outputFilePath << "\n";
    }
    std::cout << "  list artifact message: " << artifact.message << "\n";
}

std::vector<std::string> gateNamesFromIds(const Netlist& netlist,
                                          const std::vector<int>& gateIds) {
    std::vector<std::string> names;
    names.reserve(gateIds.size());
    for (int gateId : gateIds) {
        if (netlist.isValidGateId(gateId)) {
            names.push_back(netlist.getGate(gateId).instName);
        }
    }
    return names;
}

std::vector<std::string> fanoutReportEntries(
    const std::vector<Netlist::FanoutLoadReport>& reports) {
    std::vector<std::string> entries;
    entries.reserve(reports.size());
    for (const Netlist::FanoutLoadReport& report : reports) {
        entries.push_back(report.netName + " fanout=" +
                          std::to_string(report.totalLoadCount));
    }
    return entries;
}

std::string fanoutPredicateName(Netlist::FanoutPredicate predicate) {
    switch (predicate) {
    case Netlist::FanoutPredicate::None: return "none";
    case Netlist::FanoutPredicate::Equal: return "eq";
    case Netlist::FanoutPredicate::NotEqual: return "ne";
    case Netlist::FanoutPredicate::GreaterThan: return "gt";
    case Netlist::FanoutPredicate::GreaterOrEqual: return "ge";
    case Netlist::FanoutPredicate::LessThan: return "lt";
    case Netlist::FanoutPredicate::LessOrEqual: return "le";
    case Netlist::FanoutPredicate::BetweenInclusive: return "between";
    }
    return "unknown";
}

bool parseFanoutPredicate(const std::string& token,
                          Netlist::FanoutPredicate& predicate) {
    const std::string lowered = toLower(token);
    if (lowered == "eq") {
        predicate = Netlist::FanoutPredicate::Equal;
    } else if (lowered == "ne") {
        predicate = Netlist::FanoutPredicate::NotEqual;
    } else if (lowered == "gt") {
        predicate = Netlist::FanoutPredicate::GreaterThan;
    } else if (lowered == "ge") {
        predicate = Netlist::FanoutPredicate::GreaterOrEqual;
    } else if (lowered == "lt") {
        predicate = Netlist::FanoutPredicate::LessThan;
    } else if (lowered == "le") {
        predicate = Netlist::FanoutPredicate::LessOrEqual;
    } else if (lowered == "between") {
        predicate = Netlist::FanoutPredicate::BetweenInclusive;
    } else {
        return false;
    }
    return true;
}

std::string fanoutScopeName(Netlist::FanoutScope scope) {
    switch (scope) {
    case Netlist::FanoutScope::All: return "all";
    case Netlist::FanoutScope::PrimaryInputs: return "pi";
    case Netlist::FanoutScope::PrimaryOutputs: return "po";
    case Netlist::FanoutScope::Internal: return "internal";
    case Netlist::FanoutScope::GateOutputs: return "gate_output";
    case Netlist::FanoutScope::CombinationalOutputs: return "comb_output";
    case Netlist::FanoutScope::DffOutputs: return "dff_output";
    }
    return "unknown";
}

bool parseFanoutScope(const std::string& token, Netlist::FanoutScope& scope) {
    const std::string lowered = toLower(token);
    if (lowered == "all") scope = Netlist::FanoutScope::All;
    else if (lowered == "pi") scope = Netlist::FanoutScope::PrimaryInputs;
    else if (lowered == "po") scope = Netlist::FanoutScope::PrimaryOutputs;
    else if (lowered == "internal") scope = Netlist::FanoutScope::Internal;
    else if (lowered == "gate_output") scope = Netlist::FanoutScope::GateOutputs;
    else if (lowered == "comb_output") {
        scope = Netlist::FanoutScope::CombinationalOutputs;
    } else if (lowered == "dff_output") {
        scope = Netlist::FanoutScope::DffOutputs;
    } else return false;
    return true;
}

std::string fanoutRankModeName(Netlist::FanoutRankMode mode) {
    switch (mode) {
    case Netlist::FanoutRankMode::Highest: return "highest";
    case Netlist::FanoutRankMode::Lowest: return "lowest";
    case Netlist::FanoutRankMode::NthHighest: return "nth_highest";
    case Netlist::FanoutRankMode::NthLowest: return "nth_lowest";
    case Netlist::FanoutRankMode::Top: return "top";
    case Netlist::FanoutRankMode::Bottom: return "bottom";
    }
    return "unknown";
}

bool parseFanoutRankMode(const std::string& token,
                         Netlist::FanoutRankMode& mode) {
    const std::string lowered = toLower(token);
    if (lowered == "highest") mode = Netlist::FanoutRankMode::Highest;
    else if (lowered == "lowest") mode = Netlist::FanoutRankMode::Lowest;
    else if (lowered == "nth_highest") mode = Netlist::FanoutRankMode::NthHighest;
    else if (lowered == "nth_lowest") mode = Netlist::FanoutRankMode::NthLowest;
    else if (lowered == "top") mode = Netlist::FanoutRankMode::Top;
    else if (lowered == "bottom") mode = Netlist::FanoutRankMode::Bottom;
    else return false;
    return true;
}

std::vector<std::string> fanoutRankEntries(
    const std::vector<Netlist::FanoutRankEntry>& entries) {
    std::vector<std::string> result;
    result.reserve(entries.size());
    for (const Netlist::FanoutRankEntry& entry : entries) {
        result.push_back("rank=" + std::to_string(entry.rank) +
                         " net=" + entry.netName +
                         " fanout=" + std::to_string(entry.fanout));
    }
    return result;
}

std::string coneRankMetricName(Netlist::ConeRankMetric metric) {
    switch (metric) {
    case Netlist::ConeRankMetric::ScopeGateCount: return "scope_gates";
    case Netlist::ConeRankMetric::FilteredGateCount: return "filtered_gates";
    case Netlist::ConeRankMetric::NetCount: return "nets";
    }
    return "unknown";
}

bool parseConeRankMetric(const std::string& token,
                         Netlist::ConeRankMetric& metric) {
    const std::string lowered = toLower(token);
    if (lowered == "gates" || lowered == "scope_gates") {
        metric = Netlist::ConeRankMetric::ScopeGateCount;
    } else if (lowered == "filtered_gates") {
        metric = Netlist::ConeRankMetric::FilteredGateCount;
    } else if (lowered == "nets") {
        metric = Netlist::ConeRankMetric::NetCount;
    } else {
        return false;
    }
    return true;
}

std::string coneRankModeName(Netlist::ConeRankMode mode) {
    switch (mode) {
    case Netlist::ConeRankMode::Highest: return "highest";
    case Netlist::ConeRankMode::Lowest: return "lowest";
    case Netlist::ConeRankMode::NthHighest: return "nth_highest";
    case Netlist::ConeRankMode::NthLowest: return "nth_lowest";
    case Netlist::ConeRankMode::Top: return "top";
    case Netlist::ConeRankMode::Bottom: return "bottom";
    }
    return "unknown";
}

bool parseConeRankMode(const std::string& token,
                       Netlist::ConeRankMode& mode) {
    const std::string lowered = toLower(token);
    if (lowered == "highest") mode = Netlist::ConeRankMode::Highest;
    else if (lowered == "lowest") mode = Netlist::ConeRankMode::Lowest;
    else if (lowered == "nth_highest") {
        mode = Netlist::ConeRankMode::NthHighest;
    } else if (lowered == "nth_lowest") {
        mode = Netlist::ConeRankMode::NthLowest;
    } else if (lowered == "top") mode = Netlist::ConeRankMode::Top;
    else if (lowered == "bottom") mode = Netlist::ConeRankMode::Bottom;
    else return false;
    return true;
}

std::vector<std::string> coneRankEntries(
    const std::vector<Netlist::ConeRankEntry>& entries) {
    std::vector<std::string> result;
    result.reserve(entries.size());
    for (const Netlist::ConeRankEntry& entry : entries) {
        result.push_back(
            "rank=" + std::to_string(entry.rank) +
            " output=" + entry.outputNetName +
            " metric=" + std::to_string(entry.metricValue) +
            " scope_gates=" + std::to_string(entry.scopeGateCount) +
            " filtered_gates=" + std::to_string(entry.filteredGateCount) +
            " nets=" + std::to_string(entry.netCount));
    }
    return result;
}

std::string coneMetricPredicateName(
    Netlist::ConeMetricPredicate predicate) {
    switch (predicate) {
    case Netlist::ConeMetricPredicate::Equal: return "equal";
    case Netlist::ConeMetricPredicate::NotEqual: return "not_equal";
    case Netlist::ConeMetricPredicate::GreaterThan: return "greater_than";
    case Netlist::ConeMetricPredicate::GreaterOrEqual:
        return "greater_or_equal";
    case Netlist::ConeMetricPredicate::LessThan: return "less_than";
    case Netlist::ConeMetricPredicate::LessOrEqual: return "less_or_equal";
    case Netlist::ConeMetricPredicate::BetweenInclusive:
        return "between_inclusive";
    }
    return "unknown";
}

bool parseConeMetricPredicate(
    const std::string& token,
    Netlist::ConeMetricPredicate& predicate) {
    const std::string lowered = toLower(token);
    if (lowered == "eq") {
        predicate = Netlist::ConeMetricPredicate::Equal;
    } else if (lowered == "ne") {
        predicate = Netlist::ConeMetricPredicate::NotEqual;
    } else if (lowered == "gt") {
        predicate = Netlist::ConeMetricPredicate::GreaterThan;
    } else if (lowered == "ge") {
        predicate = Netlist::ConeMetricPredicate::GreaterOrEqual;
    } else if (lowered == "lt") {
        predicate = Netlist::ConeMetricPredicate::LessThan;
    } else if (lowered == "le") {
        predicate = Netlist::ConeMetricPredicate::LessOrEqual;
    } else if (lowered == "between") {
        predicate = Netlist::ConeMetricPredicate::BetweenInclusive;
    } else {
        return false;
    }
    return true;
}

std::vector<std::string> coneFilterEntries(
    const std::vector<Netlist::ConeFilterEntry>& entries) {
    std::vector<std::string> result;
    result.reserve(entries.size());
    for (const Netlist::ConeFilterEntry& entry : entries) {
        result.push_back(
            "output=" + entry.outputNetName +
            " metric=" + std::to_string(entry.metricValue) +
            " scope_gates=" + std::to_string(entry.scopeGateCount) +
            " filtered_gates=" + std::to_string(entry.filteredGateCount) +
            " nets=" + std::to_string(entry.netCount));
    }
    return result;
}

// 建立不覆寫既有檔案的 Boolean equation artifact 名稱。
std::string makeAutomaticFunctionExpressionOutputPath(ToolSession& session) {
    namespace fs = std::filesystem;

    std::string designStem = session.loadedFilePath.empty()
        ? "design"
        : fs::path(session.loadedFilePath).stem().string();
    for (char& ch : designStem) {
        const unsigned char value = static_cast<unsigned char>(ch);
        if (!std::isalnum(value) && ch != '_' && ch != '-') ch = '_';
    }
    if (designStem.empty()) designStem = "design";

    while (true) {
        ++session.functionExpressionArtifactSequence;
        const std::string candidate = designStem + "_boolean_equation_" +
            std::to_string(session.functionExpressionArtifactSequence) + ".txt";
        std::error_code error;
        const bool exists = fs::exists(candidate, error);
        if (error || !exists) return candidate;
    }
}

// 印出 DirectConnectivityQuery 的統一 report。
void printConnectivityReport(const Netlist& netlist,
                             const Netlist::DirectConnectivityQuery& query,
                             const Netlist::DirectConnectivityReport& report,
                             bool suppressLists = false) {
    if (!report.ok) {
        std::cout << "Error: " << report.message << "\n";
        return;
    }

    std::cout << "OK: " << report.message << "\n";
    if (!report.gateName.empty()) {
        std::cout << "  gate: " << report.gateName << "\n";
    }
    if (!report.netName.empty()) {
        std::cout << "  net: " << report.netName << "\n";
    }
    std::cout << "  count: " << report.count << "\n";
    if (report.pinDetailsIncluded) {
        std::cout << "  pin connection count: "
                  << report.pinConnectionCount << "\n";
    }
    if (query.type == Netlist::DirectConnectivityQueryType::DirectlyConnected) {
        std::cout << "  connected: " << (report.connected ? "yes" : "no") << "\n";
    }
    if (report.fanoutLoadReport.ok) {
        const Netlist::FanoutLoadReport& fanout = report.fanoutLoadReport;
        std::cout << "  fanout load count (QA definition): "
                  << fanout.totalLoadCount << "\n";
        std::cout << "  drives primary output: "
                  << (fanout.drivesPrimaryOutput ? "yes" : "no") << "\n";
        std::cout << "  primary output load count: "
                  << fanout.primaryOutputLoadCount << "\n";
        if (!suppressLists) {
            printGateIdList(netlist, "Combinational gate input loads",
                            fanout.combinationalGateLoads);
            printGateIdList(netlist, "DFF D-pin loads", fanout.dffDataLoads);
            printGateIdList(netlist, "DFF clock-pin loads", fanout.dffClockLoads);
            printGateIdList(netlist, "DFF reset/set-pin loads", fanout.dffResetSetLoads);
            printGateIdList(netlist, "DFF other-pin loads", fanout.dffOtherLoads);
        }
    }
    if (report.globalFanoutReport.ok) {
        const Netlist::GlobalFanoutReport& global = report.globalFanoutReport;
        std::cout << "  checked nets: " << global.checkedNetCount << "\n";
        std::cout << "  max fanout: " << global.maxFanout << "\n";
        if (global.fanoutLimit >= 0) {
            std::cout << "  fanout limit: " << global.fanoutLimit << "\n";
            std::cout << "  satisfies limit: "
                      << (global.satisfiesLimit ? "yes" : "no") << "\n";
        }
        if (global.fanoutFilterApplied) {
            std::cout << "  fanout filter scope: "
                      << (global.primaryInputsOnly ? "pi" : "all") << "\n";
            std::cout << "  fanout predicate: "
                      << fanoutPredicateName(global.fanoutPredicate) << "\n";
            std::cout << "  fanout value: " << global.fanoutValue << "\n";
            if (global.fanoutPredicate ==
                Netlist::FanoutPredicate::BetweenInclusive) {
                std::cout << "  fanout upper value: "
                          << global.fanoutUpperValue << "\n";
            }
            std::cout << "  matched net count: "
                      << global.matchedNetCount << "\n";
        }
        if (!suppressLists) {
            printFanoutNetList("Max-fanout nets", global.maxFanoutReports);
            if (!global.violatingReports.empty()) {
                printFanoutNetList("Violating nets", global.violatingReports);
            }
            if (global.fanoutFilterApplied) {
                printFanoutNetList("Matched nets", global.matchedReports);
            }
        }
    }
    if (report.fanoutRankingReport.ok) {
        const Netlist::FanoutRankingReport& ranking =
            report.fanoutRankingReport;
        std::cout << "  fanout ranking scope: "
                  << fanoutScopeName(ranking.scope) << "\n";
        std::cout << "  fanout ranking mode: "
                  << fanoutRankModeName(ranking.mode) << "\n";
        std::cout << "  requested rank or count: "
                  << ranking.requestedRankOrCount << "\n";
        std::cout << "  checked nets: " << ranking.checkedNetCount << "\n";
        std::cout << "  distinct fanout levels: "
                  << ranking.distinctFanoutLevelCount << "\n";
        std::cout << "  selected fanout levels: "
                  << ranking.selectedFanoutLevelCount << "\n";
        std::cout << "  result net count: " << ranking.resultNetCount << "\n";
        std::cout << "  requested rank exists: "
                  << (ranking.requestedRankExists ? "yes" : "no") << "\n";
        if (!suppressLists) {
            printStringList("Ranked nets",
                            fanoutRankEntries(ranking.rankedReports));
        }
    }
    if (!suppressLists && report.pinDetailsIncluded &&
        !report.pinConnections.empty()) {
        std::cout << "Pin connection details ("
                  << report.pinConnections.size() << "):\n";
        for (const ConnectivityPinRecord& record : report.pinConnections) {
            std::cout << "  " << formatConnectivityPinRecord(record) << "\n";
        }
    }
    if (!suppressLists && !report.fanoutLoadReport.ok &&
        !report.pinDetailsIncluded &&
        !report.gateNames.empty()) {
        printStringList("Gate names", report.gateNames);
    }
    if (!suppressLists && !report.fanoutRankingReport.ok &&
        !report.netNames.empty()) {
        printStringList("Net names", report.netNames);
    }
}

// 印出 ConeQuery 的統一 report。
void printConeReport(const Netlist& netlist,
                     const Netlist::ConeReport& report,
                     bool suppressLists = false) {
    if (!report.ok) {
        std::cout << "Error: " << report.message << "\n";
        return;
    }

    if (report.filterReport.ok) {
        const Netlist::ConeFilterReport& filter = report.filterReport;
        std::cout << "OK: " << report.message << "\n";
        std::cout << "  output cone filter metric: "
                  << coneRankMetricName(filter.metric) << "\n";
        std::cout << "  output cone filter predicate: "
                  << coneMetricPredicateName(filter.predicate) << "\n";
        std::cout << "  filter value: " << filter.value << "\n";
        if (filter.predicate ==
            Netlist::ConeMetricPredicate::BetweenInclusive) {
            std::cout << "  filter upper value: "
                      << filter.upperValue << "\n";
        }
        std::cout << "  gate type filter applied: "
                  << (report.gateTypeFilterApplied ? "yes" : "no") << "\n";
        std::cout << "  gate type filters: ";
        if (report.appliedGateTypeFilters.empty()) {
            std::cout << "all\n";
        } else {
            for (size_t i = 0; i < report.appliedGateTypeFilters.size(); ++i) {
                if (i != 0) std::cout << ", ";
                std::cout << netlist.gateTypeToString(
                    report.appliedGateTypeFilters[i]);
            }
            std::cout << "\n";
        }
        std::cout << "  checked primary outputs: "
                  << filter.checkedOutputCount << "\n";
        std::cout << "  matched output count: "
                  << filter.matchedOutputCount << "\n";
        if (!suppressLists && !filter.matchedOutputs.empty()) {
            printStringList("Matched output cones",
                            coneFilterEntries(filter.matchedOutputs));
        }
        return;
    }

    std::cout << "OK: " << report.message << "\n";
    std::cout << "  source: " << report.sourceName << "\n";
    if (!report.secondSourceName.empty()) {
        std::cout << "  second source: " << report.secondSourceName << "\n";
    }
    std::cout << "  gates: " << report.gateCount << "\n";
    std::cout << "  scope gates: " << report.scopeGateCount << "\n";
    std::cout << "  filtered gates: " << report.gateCount << "\n";
    std::cout << "  gate type filter applied: "
              << (report.gateTypeFilterApplied ? "yes" : "no") << "\n";
    std::cout << "  gate type filters: ";
    if (report.appliedGateTypeFilters.empty()) {
        std::cout << "all\n";
    } else {
        for (size_t i = 0; i < report.appliedGateTypeFilters.size(); ++i) {
            if (i != 0) std::cout << ", ";
            std::cout << netlist.gateTypeToString(report.appliedGateTypeFilters[i]);
        }
        std::cout << "\n";
    }
    std::cout << "  gate details included: "
              << (report.gateDetailsIncluded ? "yes" : "no") << "\n";
    std::cout << "  gate detail count: " << report.gateConnections.size() << "\n";
    std::cout << "  nets: " << report.netCount << "\n";
    if (!report.gateTypeCounts.empty()) {
        std::cout << "Gate type counts (" << report.gateTypeCounts.size() << "):\n";
        for (const auto& item : report.gateTypeCounts) {
            std::cout << "  " << netlist.gateTypeToString(item.first)
                      << " : " << item.second << "\n";
        }
    }
    if (report.checkedOutputCount > 0 || report.rankingReport.ok) {
        std::cout << "  checked primary outputs: " << report.checkedOutputCount << "\n";
    }
    if (report.rankingReport.ok) {
        const Netlist::ConeRankingReport& ranking = report.rankingReport;
        std::cout << "  output cone ranking metric: "
                  << coneRankMetricName(ranking.metric) << "\n";
        std::cout << "  output cone ranking mode: "
                  << coneRankModeName(ranking.mode) << "\n";
        std::cout << "  requested rank or count: "
                  << ranking.requestedRankOrCount << "\n";
        std::cout << "  distinct cone metric levels: "
                  << ranking.distinctMetricLevelCount << "\n";
        std::cout << "  selected cone metric levels: "
                  << ranking.selectedMetricLevelCount << "\n";
        std::cout << "  result output count: "
                  << ranking.resultOutputCount << "\n";
        std::cout << "  requested rank exists: "
                  << (ranking.requestedRankExists ? "yes" : "no") << "\n";
        if (!suppressLists && !ranking.rankedOutputs.empty()) {
            printStringList("Ranked output cones",
                            coneRankEntries(ranking.rankedOutputs));
        }
    }
    if (!suppressLists && !report.rootNetNames.empty()) {
        printStringList("Root nets", report.rootNetNames);
    }
    if (!suppressLists && report.gateDetailsIncluded &&
        !report.gateConnections.empty()) {
        std::cout << "Gate connection details ("
                  << report.gateConnections.size() << "):\n";
        for (const GateConnectionSummary& gate : report.gateConnections) {
            std::cout << "  " << formatGateConnection(gate) << "\n";
        }
    } else if (!suppressLists && !report.gateNames.empty()) {
        printStringList("Cone gates", report.gateNames);
    }
    if (!suppressLists && !report.netNames.empty()) {
        printStringList("Cone nets", report.netNames);
    }
    if (report.longestDepth >= 0) {
        std::cout << "  longest local path depth: " << report.longestDepth << "\n";
        if (!suppressLists) {
            for (int netId : report.longestPathNetIds) {
                std::cout << "    " << netlist.getNet(netId).name << "\n";
            }
        }
    }
    if (report.shortestDepth >= 0) {
        std::cout << "  shortest local path depth: " << report.shortestDepth << "\n";
    }
}

// 印出 PathQuery 的統一 result。
void printPathResult(const Netlist& netlist,
                     const Netlist::PathQuery& query,
                     const Netlist::PathQueryResult& result,
                     bool suppressLargeLists = false) {
    if (!result.ok) {
        std::cout << "Error: " << result.message << "\n";
        if (!result.unresolvedStartpoints.empty()) {
            printStringList("Unresolved startpoints", result.unresolvedStartpoints);
        }
        if (!result.unresolvedEndpoints.empty()) {
            printStringList("Unresolved endpoints", result.unresolvedEndpoints);
        }
        if (!result.unresolvedRequiredNodes.empty()) {
            printStringList("Unresolved required nodes", result.unresolvedRequiredNodes);
        }
        if (!result.unresolvedAvoidedNodes.empty()) {
            printStringList("Unresolved avoided nodes", result.unresolvedAvoidedNodes);
        }
        return;
    }

    if (query.mode == Netlist::PathQueryMode::DirectPiPoConnections) {
        std::cout << "Total direct PI-to-PO connections: " << result.pathCount << "\n";
        const size_t pathsToPrint = suppressLargeLists
            ? 0
            : std::min(query.maxPrintedPaths, result.paths.size());
        for (size_t i = 0; i < pathsToPrint; ++i) {
            std::cout << "Connection " << (i + 1) << ":\n";
            printPath(netlist, result.paths[i]);
        }
        if (pathsToPrint < result.paths.size()) {
            std::cout << "... omitted " << (result.paths.size() - pathsToPrint)
                      << " connections from terminal output\n";
        }
        return;
    }

    if (query.mode == Netlist::PathQueryMode::FindMandatoryNodes) {
        std::cout << "Status: " << result.status << "\n";
        std::cout << "Path exists: " << (result.pathExists ? "yes" : "no") << "\n";
        if (!suppressLargeLists) {
            printStringList("Mandatory internal nets", result.mandatoryNetNames);
        } else {
            std::cout << "Mandatory internal net count: "
                      << result.mandatoryNetNames.size() << "\n";
        }
        return;
    }

    if (query.mode == Netlist::PathQueryMode::IsSeparator) {
        std::cout << "Status: " << result.status << "\n";
        std::cout << "Candidate: " << result.separatorCandidateNetName << "\n";
        std::cout << "Path exists: " << (result.pathExists ? "yes" : "no") << "\n";
        std::cout << "Is separator: " << (result.isSeparator ? "yes" : "no") << "\n";
        if (!result.witnessStartpoint.empty() || !result.witnessEndpoint.empty()) {
            std::cout << "Witness startpoint: " << result.witnessStartpoint << "\n";
            std::cout << "Witness endpoint: " << result.witnessEndpoint << "\n";
        }
        return;
    }

    if (query.mode == Netlist::PathQueryMode::Exists ||
        query.mode == Netlist::PathQueryMode::EveryPathThrough ||
        query.mode == Netlist::PathQueryMode::EveryPathAvoids) {
        std::cout << (result.exists ? "Yes\n" : "No\n");
        return;
    }

    if (query.mode == Netlist::PathQueryMode::EnumerateAll) {
        std::cout << "Total paths: " << result.pathCount << "\n";
        std::cout << "Complete enumeration: "
                  << (result.completeEnumeration ? "yes" : "no") << "\n";
        std::cout << "Count only: " << (result.countOnly ? "yes" : "no") << "\n";
        std::cout << "Timed out: " << (result.enumerationTimedOut ? "yes" : "no") << "\n";
        std::cout << "Path limit reached: "
                  << (result.enumerationPathLimitReached ? "yes" : "no") << "\n";
        if (!result.enumerationStopReason.empty()) {
            std::cout << "Stop reason: " << result.enumerationStopReason << "\n";
        }
        if (result.wrotePathsToFile) {
            std::cout << "Wrote paths to file: yes\n";
            std::cout << "Output file: " << result.outputFilePath << "\n";
        }
        const size_t pathsToPrint = std::min(query.maxPrintedPaths, result.paths.size());
        for (size_t i = 0; i < pathsToPrint; ++i) {
            std::cout << "Path " << (i + 1) << ":\n";
            printPath(netlist, result.paths[i]);
        }
        if (!result.countOnly && pathsToPrint < result.pathCount) {
            std::cout << "... omitted " << (result.pathCount - pathsToPrint)
                      << " paths from terminal output";
            if (result.wrotePathsToFile) {
                std::cout << "; see " << result.outputFilePath;
            }
            std::cout << "\n";
        }
        return;
    }

    if (!result.path.exists()) {
        std::cout << "No path found.\n";
        return;
    }
    if (!suppressLargeLists) {
        printPath(netlist, result.path);
    } else {
        std::cout << "Depth: " << result.path.depth() << "\n";
    }
}

// 印出 RegisterPathQuery 的統一 report。
void printRegisterPathReport(const Netlist& netlist,
                             const Netlist::RegisterPathQuery& query,
                             const Netlist::RegisterPathReport& report) {
    if (!report.ok) {
        std::cout << "Error: " << report.message << "\n";
        return;
    }

    std::cout << "OK: " << report.message << "\n";
    std::cout << "Start DFFs: " << report.startDffNames.size() << "\n";
    std::cout << "End DFFs: " << report.endDffNames.size() << "\n";

    if (query.mode == Netlist::RegisterPathQueryMode::Exists) {
        std::cout << (report.exists ? "Yes\n" : "No\n");
        return;
    }

    if (query.mode == Netlist::RegisterPathQueryMode::EnumerateAll) {
        std::cout << "Total register paths: " << report.pathResult.pathCount << "\n";
        std::cout << "Complete enumeration: "
                  << (report.pathResult.completeEnumeration ? "yes" : "no") << "\n";
        std::cout << "Count only: "
                  << (report.pathResult.countOnly ? "yes" : "no") << "\n";
        std::cout << "Timed out: "
                  << (report.pathResult.enumerationTimedOut ? "yes" : "no") << "\n";
        std::cout << "Path limit reached: "
                  << (report.pathResult.enumerationPathLimitReached ? "yes" : "no") << "\n";
        if (!report.pathResult.enumerationStopReason.empty()) {
            std::cout << "Stop reason: " << report.pathResult.enumerationStopReason << "\n";
        }
        if (report.pathResult.wrotePathsToFile) {
            std::cout << "Wrote paths to file: yes\n";
            std::cout << "Output file: " << report.pathResult.outputFilePath << "\n";
        }
        const size_t pathsToPrint =
            std::min(query.maxPrintedPaths, report.pathResult.paths.size());
        for (size_t i = 0; i < pathsToPrint; ++i) {
            std::cout << "Path " << (i + 1) << ":\n";
            printPath(netlist, report.pathResult.paths[i]);
        }
        if (!report.pathResult.countOnly && pathsToPrint < report.pathResult.pathCount) {
            std::cout << "... omitted " << (report.pathResult.pathCount - pathsToPrint)
                      << " paths from terminal output";
            if (report.pathResult.wrotePathsToFile) {
                std::cout << "; see " << report.pathResult.outputFilePath;
            }
            std::cout << "\n";
        }
        return;
    }

    if (!report.exists) {
        std::cout << "No register-to-register path found.\n";
        return;
    }

    if (!report.startDffName.empty() || !report.endDffName.empty()) {
        std::cout << "Representative DFF pair: "
                  << (report.startDffName.empty() ? "(unknown)" : report.startDffName)
                  << " -> "
                  << (report.endDffName.empty() ? "(unknown)" : report.endDffName)
                  << "\n";
    }
    printPath(netlist, report.pathResult.path);
}

// 印出 DepthQuery 的統一 report。
void printDepthReportSet(const Netlist& netlist,
                         const Netlist::DepthReportSet& report,
                         bool suppressLargeLists = false) {
    if (!report.ok) {
        std::cout << "Error: " << report.message << "\n";
        return;
    }

    std::cout << "OK: " << report.message << "\n";
    if (!report.gateName.empty()) {
        std::cout << "Gate: " << report.gateName << "\n";
        std::cout << "Gate ID: " << report.gateId << "\n";
        std::cout << "Gate on critical path: "
                  << (report.gateOnCriticalPath ? "yes" : "no") << "\n";
    }
    std::cout << "Report count: " << report.count << "\n";
    if (!suppressLargeLists) {
        for (const DepthReport& item : report.reports) {
            std::cout << "  " << item.endpointName << " depth=" << item.depth << "\n";
        }
    }
    if (report.worst.depth >= 0) {
        std::cout << "Worst endpoint: " << report.worst.endpointName
                  << " depth=" << report.worst.depth << "\n";
        if (!suppressLargeLists) {
            printPath(netlist, report.worst.criticalPath);
        }
    }
}

// 印出 FunctionQuery 的統一 report。
void printFunctionReport(const Netlist::FunctionReport& report,
                         bool suppressLargeLists = false) {
    std::cout << (report.ok ? "OK: " : "Error: ") << report.message << "\n";
    if (!report.netNameA.empty()) {
        std::cout << "  net A: " << report.netNameA << "\n";
    }
    if (!report.netNameB.empty()) {
        std::cout << "  net B: " << report.netNameB << "\n";
    }
    if (!report.conditionNetName.empty()) {
        std::cout << "  condition net: " << report.conditionNetName << "\n";
        std::cout << "  condition value: " << report.conditionValue << "\n";
    }
    if (!report.status.empty()) {
        std::cout << "  status: " << report.status << "\n";
    }
    if (report.solverRan || report.solverUnknown || report.solverTimedOut ||
        report.unsupported || !report.solverStatus.empty()) {
        std::cout << "  solver ran: " << (report.solverRan ? "yes" : "no") << "\n";
        std::cout << "  solver status: " << report.solverStatus << "\n";
        std::cout << "  solver timed out: " << (report.solverTimedOut ? "yes" : "no") << "\n";
        std::cout << "  solver unknown: " << (report.solverUnknown ? "yes" : "no") << "\n";
        std::cout << "  unsupported: " << (report.unsupported ? "yes" : "no") << "\n";
    }
    if (!report.expression.empty()) {
        std::cout << "  expression: " << report.expression << "\n";
        std::cout << "  expression length: " << report.expressionLength << "\n";
        std::cout << "  expression depth limited: "
                  << (report.expressionDepthLimited ? "yes" : "no") << "\n";
        if (report.maxExpressionDepth >= 0) {
            std::cout << "  max expression depth: " << report.maxExpressionDepth << "\n";
        }
    }
    if (!report.expressionArtifactFormat.empty() ||
        !report.expressionOutputFilePath.empty()) {
        std::cout << "  expression artifact format: "
                  << report.expressionArtifactFormat << "\n";
        std::cout << "  expression artifact complete: "
                  << (report.expressionArtifactComplete ? "yes" : "no") << "\n";
        std::cout << "  expression artifact timed out: "
                  << (report.expressionArtifactTimedOut ? "yes" : "no") << "\n";
        std::cout << "  equation count: "
                  << report.expressionEquationCount << "\n";
        std::cout << "  boundary count: "
                  << report.expressionBoundaryCount << "\n";
        std::cout << "  wrote expression to file: "
                  << (report.wroteExpressionToFile ? "yes" : "no") << "\n";
        if (!report.expressionOutputFilePath.empty()) {
            std::cout << "  output_file: "
                      << report.expressionOutputFilePath << "\n";
        }
    }
    const bool expressionResult =
        report.status == "BOOLEAN_EXPRESSION" ||
        report.status == "BOOLEAN_EQUATION_ARTIFACT" ||
        report.status == "BOOLEAN_EQUATION_ARTIFACT_TIMEOUT" ||
        report.status == "BOOLEAN_EQUATION_ARTIFACT_INCOMPLETE" ||
        report.status == "SIMPLIFIED_BOOLEAN_EXPRESSION" ||
        report.status == "PRIMARY_INPUT_SUPPORT";
    const bool hasSupportBreakdown = expressionResult ||
        !report.supportPrimaryInputs.empty() ||
        !report.supportRealPrimaryInputs.empty() ||
        !report.supportDffPseudoInputs.empty() ||
        !report.supportUndrivenLeaves.empty();
    if (hasSupportBreakdown) {
        std::cout << "  support leaf count: "
                  << report.supportPrimaryInputs.size() << "\n";
        if (!suppressLargeLists) {
            printStringList("Support leaves", report.supportPrimaryInputs);
        }
        std::cout << "  real primary input count: "
                  << report.supportRealPrimaryInputs.size() << "\n";
        std::cout << "  DFF.Q boundary count: "
                  << report.supportDffPseudoInputs.size() << "\n";
        std::cout << "  undriven boundary count: "
                  << report.supportUndrivenLeaves.size() << "\n";
        std::cout << "  primary-input-only combinational expression available: "
                  << (report.supportDffPseudoInputs.empty() &&
                              report.supportUndrivenLeaves.empty()
                          ? "yes"
                          : "no")
                  << "\n";
    }
    if (!suppressLargeLists && !report.supportRealPrimaryInputs.empty()) {
        printStringList("  - real primary inputs", report.supportRealPrimaryInputs);
    }
    if (!suppressLargeLists && !report.supportDffPseudoInputs.empty()) {
        printStringList("  - DFF.Q pseudo primary inputs", report.supportDffPseudoInputs);
    }
    if (!suppressLargeLists && !report.supportDffStateBoundaries.empty()) {
        std::vector<std::string> mappings;
        mappings.reserve(report.supportDffStateBoundaries.size());
        for (const auto& boundary : report.supportDffStateBoundaries) {
            mappings.push_back(
                boundary.stateVariableName + " = " + boundary.dffName + "." +
                boundary.pinName + " (net " + boundary.netName + ")");
        }
        printStringList("  - DFF.Q current-state variable mappings", mappings);
    }
    if (!suppressLargeLists && !report.supportUndrivenLeaves.empty()) {
        printStringList("  - undriven leaves (no driver, not a primary input)", report.supportUndrivenLeaves);
    }
    if (report.status == "FUNCTIONALLY_DEPENDENT" ||
        report.status == "FUNCTIONALLY_INDEPENDENT") {
        std::cout << "  input in structural support: "
                  << (report.inputInStructuralSupport ? "yes" : "no") << "\n";
        std::cout << "  depends on input: "
                  << (report.dependsOnInput ? "yes" : "no") << "\n";
    }
    if (!report.symmetryInputNameA.empty() || !report.symmetryInputNameB.empty()) {
        std::cout << "  symmetry input A: " << report.symmetryInputNameA << "\n";
        std::cout << "  symmetry input B: " << report.symmetryInputNameB << "\n";
        std::cout << "  input A in structural support: "
                  << (report.symmetryInputAInStructuralSupport ? "yes" : "no") << "\n";
        std::cout << "  input B in structural support: "
                  << (report.symmetryInputBInStructuralSupport ? "yes" : "no") << "\n";
        std::cout << "  symmetric: " << (report.symmetric ? "yes" : "no") << "\n";
        std::cout << "  counterexample found: "
                  << (report.counterexampleFound ? "yes" : "no") << "\n";
        std::cout << "  mismatched target bit count: "
                  << report.mismatchedTargetBitNames.size() << "\n";
        std::cout << "  counterexample assignment count: "
                  << report.counterexampleAssignments.size() << "\n";
        std::cout << "  target value count before swap: "
                  << report.outputValuesBeforeSwap.size() << "\n";
        std::cout << "  target value count after swap: "
                  << report.outputValuesAfterSwap.size() << "\n";
        if (!suppressLargeLists && !report.mismatchedTargetBitNames.empty()) {
            printStringList("Mismatched target bits", report.mismatchedTargetBitNames);
        }
        if (!suppressLargeLists && !report.counterexampleAssignments.empty()) {
            std::cout << "Counterexample assignments ("
                      << report.counterexampleAssignments.size() << "):\n";
            for (const auto& item : report.counterexampleAssignments) {
                std::cout << "  " << item.first << " = " << item.second << "\n";
            }
        }
        if (!suppressLargeLists && !report.outputValuesBeforeSwap.empty()) {
            std::cout << "Target values before swap (A=0, B=1):\n";
            for (const auto& item : report.outputValuesBeforeSwap) {
                std::cout << "  " << item.first << " = " << item.second << "\n";
            }
        }
        if (!suppressLargeLists && !report.outputValuesAfterSwap.empty()) {
            std::cout << "Target values after swap (A=1, B=0):\n";
            for (const auto& item : report.outputValuesAfterSwap) {
                std::cout << "  " << item.first << " = " << item.second << "\n";
            }
        }
    }

    const bool symmetryResult =
        report.status == "SYMMETRIC" ||
        report.status == "NOT_SYMMETRIC" ||
        !report.symmetryInputNameA.empty() ||
        !report.symmetryInputNameB.empty();
    if (!expressionResult) {
        std::cout << "  answer: " << (report.exists ? "yes" : "no") << "\n";
        if (!symmetryResult) {
            std::cout << "  equivalent: " << (report.equivalent ? "yes" : "no") << "\n";
            std::cout << "  can be 0: " << (report.canBeZero ? "yes" : "no") << "\n";
            std::cout << "  can be 1: " << (report.canBeOne ? "yes" : "no") << "\n";
            std::cout << "  is constant: " << (report.isConstant ? "yes" : "no") << "\n";
        }
    }
}

// 印出 FunctionSearchQuery 的候選統計、完整性與 SAT-proven matches。
void printFunctionSearchReport(const Netlist& netlist,
                               const Netlist::FunctionSearchReport& report) {
    auto queryTypeName = [](Netlist::FunctionSearchQueryType type) {
        switch (type) {
        case Netlist::FunctionSearchQueryType::NandEquivalentInputPairs:
            return "NAND_EQUIVALENT_INPUT_PAIRS";
        case Netlist::FunctionSearchQueryType::FunctionalPatternOperands:
            return "FUNCTIONAL_PATTERN_OPERANDS";
        case Netlist::FunctionSearchQueryType::EquivalentGatePairs:
            return "EQUIVALENT_GATE_PAIRS";
        }
        return "UNKNOWN";
    };
    auto scopeName = [](Netlist::FunctionSearchScope scope) {
        switch (scope) {
        case Netlist::FunctionSearchScope::WholeDesign: return "WHOLE_DESIGN";
        case Netlist::FunctionSearchScope::NetFanin: return "NET_FANIN";
        case Netlist::FunctionSearchScope::NetFanout: return "NET_FANOUT";
        case Netlist::FunctionSearchScope::GateFanin: return "GATE_FANIN";
        case Netlist::FunctionSearchScope::GateFanout: return "GATE_FANOUT";
        }
        return "UNKNOWN";
    };

    std::cout << "  report_status: " << report.status << "\n";
    std::cout << "  search_type: " << queryTypeName(report.queryType) << "\n";
    std::cout << "  target_net: " << report.targetNetName << "\n";
    std::cout << "  target_net_id: " << report.targetNetId << "\n";
    std::cout << "  scope: " << scopeName(report.scope) << "\n";
    std::cout << "  scope_name: " << report.scopeName << "\n";
    std::cout << "  gate_type_filter: "
              << (report.gateTypeFilter == GateType::UNKNOWN
                      ? "ANY"
                      : netlist.gateTypeToString(report.gateTypeFilter))
              << "\n";
    std::cout << "  pattern_type: "
              << (report.patternTypeName.empty() ? "NONE" : report.patternTypeName)
              << "\n";
    std::cout << "  operand_arity: " << report.operandArity << "\n";
    std::cout << "  found: " << (report.found ? "true" : "false") << "\n";
    std::cout << "  complete: " << (report.complete ? "true" : "false") << "\n";
    std::cout << "  all_candidates_examined: "
              << (report.allCandidatesExamined ? "true" : "false") << "\n";
    std::cout << "  timed_out: " << (report.timedOut ? "true" : "false") << "\n";
    std::cout << "  truncated: " << (report.truncated ? "true" : "false") << "\n";
    std::cout << "  unsupported: " << (report.unsupported ? "true" : "false") << "\n";
    std::cout << "  candidate_signal_count: " << report.candidateSignalCount << "\n";
    std::cout << "  candidate_gate_count: " << report.candidateGateCount << "\n";
    std::cout << "  simulation_eligible_signal_count: "
              << report.simulationEligibleSignalCount << "\n";
    std::cout << "  simulation_bucket_count: "
              << report.simulationBucketCount << "\n";
    std::cout << "  candidate_pairs_considered: "
              << report.candidatePairsConsidered << "\n";
    std::cout << "  candidate_pairs_rejected_by_simulation: "
              << report.candidatePairsRejectedBySimulation << "\n";
    std::cout << "  sat_checks: " << report.satChecks << "\n";
    std::cout << "  sat_unknown_count: " << report.satUnknownCount << "\n";
    std::cout << "  unsupported_signal_count: "
              << report.unsupportedSignalCount << "\n";
    std::cout << "  equivalence_class_count: "
              << report.equivalenceClassCount << "\n";
    std::cout << "  equivalent_pair_count: "
              << report.equivalentPairCount << "\n";
    std::cout << "  simulation_pattern_count: "
              << report.simulationPatternCount << "\n";
    std::cout << "  elapsed_seconds: " << report.elapsedSeconds << "\n";
    std::cout << "  match_count: " << report.matchCount << "\n";
    std::cout << "  stored_match_count: " << report.matches.size() << "\n";
    std::cout << "  wrote_matches_to_file: "
              << (report.wroteMatchesToFile ? "true" : "false") << "\n";
    if (report.wroteMatchesToFile) {
        std::cout << "  output_file: " << report.outputFilePath << "\n";
    }
    if (!report.wroteMatchesToFile) {
        std::cout << "  matches:\n";
    }
    for (size_t index = 0;
         !report.wroteMatchesToFile && index < report.matches.size();
         ++index) {
        const Netlist::FunctionSearchMatch& match = report.matches[index];
        std::cout << "    match " << (index + 1) << ":\n";
        if (match.gateIdA >= 0 || match.gateIdB >= 0) {
            std::cout << "      gate_a: " << match.gateNameA << "\n";
            std::cout << "      gate_a_id: " << match.gateIdA << "\n";
            std::cout << "      gate_b: " << match.gateNameB << "\n";
            std::cout << "      gate_b_id: " << match.gateIdB << "\n";
        }
        std::cout << "      net_a: " << match.netNameA << "\n";
        std::cout << "      net_a_id: " << match.netIdA << "\n";
        std::cout << "      net_b: " << match.netNameB << "\n";
        std::cout << "      net_b_id: " << match.netIdB << "\n";
        std::cout << "      operand_count: " << match.operandNetIds.size() << "\n";
        for (size_t operand = 0; operand < match.operandNetIds.size(); ++operand) {
            std::cout << "      operand_" << (operand + 1) << ": "
                      << match.operandNetNames[operand] << "\n";
            std::cout << "      operand_" << (operand + 1) << "_id: "
                      << match.operandNetIds[operand] << "\n";
        }
        std::cout << "      proven_equivalent: "
                  << (match.provenEquivalent ? "true" : "false") << "\n";
        std::cout << "      proof_method: " << match.proofMethod << "\n";
        std::cout << "      solver_status: " << match.solverStatus << "\n";
    }
    if (!report.wroteMatchesToFile) {
        std::cout << "  equivalence_classes:\n";
    }
    for (size_t index = 0;
         !report.wroteMatchesToFile && index < report.equivalenceClasses.size();
         ++index) {
        const Netlist::FunctionSearchEquivalenceClass& equivalentClass =
            report.equivalenceClasses[index];
        std::cout << "    class " << (index + 1) << ":\n";
        std::cout << "      proven_equivalent: "
                  << (equivalentClass.provenEquivalent ? "true" : "false") << "\n";
        std::cout << "      proof_method: " << equivalentClass.proofMethod << "\n";
        std::cout << "      member_count: " << equivalentClass.gateIds.size() << "\n";
        for (size_t member = 0; member < equivalentClass.gateIds.size(); ++member) {
            std::cout << "      member " << (member + 1) << ": "
                      << equivalentClass.gateNames[member] << " (gate_id="
                      << equivalentClass.gateIds[member] << ", net="
                      << equivalentClass.netNames[member] << ", net_id="
                      << equivalentClass.netIds[member] << ")\n";
        }
    }
}

void printGraphReport(const Netlist::GraphReport& report) {
    std::cout << (report.ok ? "OK: " : "Error: ") << report.message << "\n";
    if (!report.status.empty()) {
        std::cout << "  status: " << report.status << "\n";
    }
    if (!report.candidateNetName.empty()) {
        std::cout << "  candidate net: " << report.candidateNetName << "\n";
        std::cout << "  candidate net id: " << report.candidateNetId << "\n";
    }
    if (!report.sourceNetName.empty()) {
        std::cout << "  source net: " << report.sourceNetName << "\n";
        std::cout << "  source net id: " << report.sourceNetId << "\n";
    }
    if (!report.targetNetName.empty()) {
        std::cout << "  target net: " << report.targetNetName << "\n";
        std::cout << "  target net id: " << report.targetNetId << "\n";
    }
    std::cout << "  path exists: " << (report.pathExists ? "yes" : "no") << "\n";
    std::cout << "  is cut: " << (report.isCut ? "yes" : "no") << "\n";
    if (report.checkedPrimaryInputCount || report.checkedPrimaryOutputCount) {
        std::cout << "  checked primary input bits: "
                  << report.checkedPrimaryInputCount << "\n";
        std::cout << "  checked primary output bits: "
                  << report.checkedPrimaryOutputCount << "\n";
    }
    if (!report.witnessPrimaryInput.empty()) {
        std::cout << "  witness primary input: " << report.witnessPrimaryInput << "\n";
        std::cout << "  witness primary output: " << report.witnessPrimaryOutput << "\n";
    }
    if (!report.articulationNetNames.empty()) {
        printStringList("Articulation net names", report.articulationNetNames);
    } else if (report.status == "NO_ARTICULATION_POINTS") {
        std::cout << "Articulation net names (0):\n";
    }
    std::cout << "  articulation count: " << report.articulationNetNames.size() << "\n";
    std::cout << "  combinational cycle detected: "
              << (report.combinationalCycleDetected ? "yes" : "no") << "\n";
}

std::string sequentialPatternKindName(DffInputPatternKind kind) {
    switch (kind) {
        case DffInputPatternKind::MuxHold: return "MuxHold";
        case DffInputPatternKind::DataGatingWithoutHoldFeedback:
            return "DataGatingWithoutHoldFeedback";
        default:
            return "Unknown";
    }
}

std::string sequentialDetectionMethodName(SequentialPatternDetectionMethod method) {
    switch (method) {
        case SequentialPatternDetectionMethod::StructuralCanonical:
            return "StructuralCanonical";
        case SequentialPatternDetectionMethod::StructuralCanonicalWithSat:
            return "StructuralCanonicalWithSat";
        case SequentialPatternDetectionMethod::FunctionalCofactorSat:
            return "FunctionalCofactorSat";
        default:
            return "Unknown";
    }
}

std::string activeLevelName(int activeLevel) {
    if (activeLevel == 1) return "active_high";
    if (activeLevel == 0) return "active_low";
    return "unknown";
}

struct SequentialPrintOptions {
    bool summaryOnly = false;
    bool includeNoPattern = false;
    bool recordWindowExplicit = false;
    size_t recordOffset = 0;
    size_t recordLimit = 50;
};

struct SequentialPageStats {
    size_t availableRecordCount = 0;
    size_t reportedRecordCount = 0;
    size_t omittedNoPatternCount = 0;
    size_t nextRecordOffset = 0;
    bool truncated = false;
};

SequentialPageStats getSequentialPageStats(
    const Netlist::SequentialPatternReportSet& report,
    const SequentialPrintOptions& options) {
    SequentialPageStats stats;
    const bool isSingleTarget = report.totalDffCount == 1;
    for (const DffInputPatternReport& dff : report.reports) {
        if (dff.patterns.empty()) {
            ++stats.omittedNoPatternCount;
        }
        if (options.includeNoPattern || isSingleTarget || !dff.patterns.empty()) {
            ++stats.availableRecordCount;
        }
    }

    if (options.summaryOnly || options.recordOffset >= stats.availableRecordCount) {
        return stats;
    }
    stats.reportedRecordCount = std::min(
        options.recordLimit,
        stats.availableRecordCount - options.recordOffset);
    stats.nextRecordOffset = options.recordOffset + stats.reportedRecordCount;
    stats.truncated = stats.nextRecordOffset < stats.availableRecordCount;
    return stats;
}

void printSequentialPatternReport(
    const Netlist::SequentialPatternReportSet& report,
    const SequentialPrintOptions& options) {
    std::cout << "  report_ok: " << (report.ok ? "true" : "false") << "\n";
    std::cout << "  report_status: " << report.status << "\n";
    std::cout << "  report_message: " << report.message << "\n";
    std::cout << "  exists: " << (report.exists ? "true" : "false") << "\n";
    std::cout << "  complete: " << (report.complete ? "true" : "false") << "\n";
    std::cout << "  timed_out: " << (report.timedOut ? "true" : "false") << "\n";
    std::cout << "  total_dff_count: " << report.totalDffCount << "\n";
    std::cout << "  analyzed_dff_count: " << report.analyzedDffCount << "\n";
    std::cout << "  matched_dff_count: " << report.matchedDffCount << "\n";
    std::cout << "  candidate_dff_count: " << report.candidateDffCount << "\n";
    std::cout << "  functional_candidate_count: "
              << report.functionalCandidateCount << "\n";
    std::cout << "  functional_searchable_candidate_count: "
              << report.functionalSearchableCandidateCount << "\n";
    std::cout << "  functional_candidates_examined: "
              << report.functionalCandidatesExamined << "\n";
    std::cout << "  functional_unexamined_candidate_count: "
              << report.functionalUnexaminedCandidateCount << "\n";
    std::cout << "  functional_inconclusive_candidate_count: "
              << report.functionalInconclusiveCandidateCount << "\n";
    std::cout << "  functional_simulation_pattern_count: "
              << report.functionalSimulationPatternCount << "\n";
    std::cout << "  functional_simulation_candidate_count: "
              << report.functionalSimulationCandidateCount << "\n";
    std::cout << "  functional_simulation_rejected_candidate_count: "
              << report.functionalSimulationRejectedCandidateCount << "\n";
    std::cout << "  functional_simulation_seconds: "
              << report.functionalSimulationSeconds << "\n";
    std::cout << "  functional_sat_check_count: "
              << report.functionalSatCheckCount << "\n";
    std::cout << "  functional_match_count: "
              << report.functionalMatchCount << "\n";
    std::cout << "  elapsed_seconds: " << report.elapsedSeconds << "\n";

    const SequentialPageStats page = getSequentialPageStats(report, options);
    std::cout << "  available_dff_record_count: "
              << page.availableRecordCount << "\n";
    std::cout << "  reported_dff_count: " << page.reportedRecordCount << "\n";
    std::cout << "  omitted_no_pattern_count: "
              << (options.includeNoPattern ? 0 : page.omittedNoPatternCount) << "\n";
    std::cout << "  record_offset: " << options.recordOffset << "\n";
    std::cout << "  record_limit: "
              << (options.summaryOnly ? 0 : options.recordLimit) << "\n";
    std::cout << "  records_truncated: "
              << (page.truncated ? "true" : "false") << "\n";
    std::cout << "  next_record_offset: " << page.nextRecordOffset << "\n";

    if (options.summaryOnly) {
        return;
    }

    size_t eligibleIndex = 0;
    size_t printedCount = 0;
    for (const DffInputPatternReport& dff : report.reports) {
        const bool isSingleTarget = report.totalDffCount == 1;
        if (!options.includeNoPattern && !isSingleTarget && dff.patterns.empty()) {
            continue;
        }
        if (eligibleIndex++ < options.recordOffset) {
            continue;
        }
        if (printedCount >= options.recordLimit) {
            break;
        }
        ++printedCount;

        std::cout << "  dff_record:\n";
        std::cout << "    dff_name: " << dff.dffName << "\n";
        std::cout << "    dff_gate_id: " << dff.dffGateId << "\n";
        std::cout << "    status: " << dff.status << "\n";
        std::cout << "    message: " << dff.message << "\n";
        std::cout << "    matched: " << (dff.matched ? "true" : "false") << "\n";
        std::cout << "    d_net_name: " << dff.dNetName << "\n";
        std::cout << "    d_net_id: " << dff.dNetId << "\n";
        std::cout << "    q_net_name: " << dff.qNetName << "\n";
        std::cout << "    q_net_id: " << dff.qNetId << "\n";
        std::cout << "    q_feedback_observed: "
                  << (dff.qFeedbackObserved ? "true" : "false") << "\n";
        std::cout << "    functional_fallback_attempted: "
                  << (dff.functionalFallbackAttempted ? "true" : "false") << "\n";
        std::cout << "    functional_fallback_complete: "
                  << (dff.functionalFallbackComplete ? "true" : "false") << "\n";
        std::cout << "    functional_fallback_timed_out: "
                  << (dff.functionalFallbackTimedOut ? "true" : "false") << "\n";
        std::cout << "    functional_candidate_limit_reached: "
                  << (dff.functionalCandidateLimitReached ? "true" : "false") << "\n";
        std::cout << "    functional_candidate_count: "
                  << dff.functionalCandidateCount << "\n";
        std::cout << "    functional_searchable_candidate_count: "
                  << dff.functionalSearchableCandidateCount << "\n";
        std::cout << "    functional_candidates_examined: "
                  << dff.functionalCandidatesExamined << "\n";
        std::cout << "    functional_unexamined_candidate_count: "
                  << dff.functionalUnexaminedCandidateCount << "\n";
        std::cout << "    functional_inconclusive_candidate_count: "
                  << dff.functionalInconclusiveCandidateCount << "\n";
        std::cout << "    functional_simulation_candidate_count: "
                  << dff.functionalSimulationCandidateCount << "\n";
        std::cout << "    functional_simulation_rejected_candidate_count: "
                  << dff.functionalSimulationRejectedCandidateCount << "\n";
        std::cout << "    functional_sat_check_count: "
                  << dff.functionalSatCheckCount << "\n";
        std::cout << "    pattern_count: " << dff.patterns.size() << "\n";

        for (const DffInputPattern& pattern : dff.patterns) {
            std::cout << "    pattern:\n";
            std::cout << "      kind: " << sequentialPatternKindName(pattern.kind) << "\n";
            std::cout << "      detection_method: "
                      << sequentialDetectionMethodName(pattern.detectionMethod) << "\n";
            std::cout << "      message: " << pattern.message << "\n";
            std::cout << "      confirmed: " << (pattern.confirmed ? "true" : "false") << "\n";
            std::cout << "      semantics_pending: "
                      << (pattern.semanticsPending ? "true" : "false") << "\n";
            std::cout << "      structural_match: "
                      << (pattern.structuralMatch ? "true" : "false") << "\n";
            std::cout << "      enable_net_name: " << pattern.enableNetName << "\n";
            std::cout << "      enable_net_id: " << pattern.enableNetId << "\n";
            std::cout << "      active_level: " << pattern.activeLevel << "\n";
            std::cout << "      active_level_name: "
                      << activeLevelName(pattern.activeLevel) << "\n";
            std::cout << "      hold_level: " << pattern.holdLevel << "\n";
            std::cout << "      data_net_name: " << pattern.dataNetName << "\n";
            std::cout << "      data_net_id: " << pattern.dataNetId << "\n";
            std::cout << "      data_branch_net_name: "
                      << pattern.dataBranchNetName << "\n";
            std::cout << "      data_branch_net_id: "
                      << pattern.dataBranchNetId << "\n";
            std::cout << "      data_inverted: "
                      << (pattern.dataInverted ? "true" : "false") << "\n";
            std::cout << "      data_function_resolved: "
                      << (pattern.dataFunctionResolved ? "true" : "false") << "\n";
            std::cout << "      data_search_attempted: "
                      << (pattern.dataSearchAttempted ? "true" : "false") << "\n";
            std::cout << "      data_search_complete: "
                      << (pattern.dataSearchComplete ? "true" : "false") << "\n";
            std::cout << "      data_search_timed_out: "
                      << (pattern.dataSearchTimedOut ? "true" : "false") << "\n";
            std::cout << "      data_candidate_count: "
                      << pattern.dataCandidateCount << "\n";
            std::cout << "      data_candidates_examined: "
                      << pattern.dataCandidatesExamined << "\n";
            std::cout << "      feedback_net_name: "
                      << pattern.feedbackNetName << "\n";
            std::cout << "      feedback_net_id: "
                      << pattern.feedbackNetId << "\n";
            std::cout << "      solver_ran: " << (pattern.solverRan ? "true" : "false") << "\n";
            std::cout << "      solver_status: " << pattern.solverStatus << "\n";
            std::cout << "      solver_timed_out: "
                      << (pattern.solverTimedOut ? "true" : "false") << "\n";
            std::cout << "      solver_unknown: "
                      << (pattern.solverUnknown ? "true" : "false") << "\n";
            std::cout << "      hold_functionally_proven: "
                      << (pattern.holdFunctionallyProven ? "true" : "false") << "\n";
            std::cout << "      load_functionally_proven: "
                      << (pattern.loadFunctionallyProven ? "true" : "false") << "\n";
            std::cout << "      evidence_gate_names ("
                      << pattern.evidenceGateNames.size() << "):\n";
            for (const std::string& name : pattern.evidenceGateNames) {
                std::cout << "        " << name << "\n";
            }
            std::cout << "      evidence_gate_ids ("
                      << pattern.evidenceGateIds.size() << "):\n";
            for (int gateId : pattern.evidenceGateIds) {
                std::cout << "        " << gateId << "\n";
            }
            std::cout << "      candidate_input_net_names ("
                      << pattern.candidateInputNetNames.size() << "):\n";
            for (const std::string& name : pattern.candidateInputNetNames) {
                std::cout << "        " << name << "\n";
            }
        }
    }
}

class ScopedCoutBufferRedirect {
public:
    explicit ScopedCoutBufferRedirect(std::streambuf* destination)
        : originalBuffer_(std::cout.rdbuf()), originalState_(std::cout.rdstate()) {
        std::cout.flush();
        std::cout.rdbuf(destination);
        std::cout.clear();
    }

    ~ScopedCoutBufferRedirect() {
        std::cout.flush();
        std::cout.rdbuf(originalBuffer_);
        std::cout.clear(originalState_);
    }

    ScopedCoutBufferRedirect(const ScopedCoutBufferRedirect&) = delete;
    ScopedCoutBufferRedirect& operator=(const ScopedCoutBufferRedirect&) = delete;

private:
    std::streambuf* originalBuffer_;
    std::ios::iostate originalState_;
};

bool writeSequentialPatternArtifact(
    const std::string& outputPath,
    const Netlist::SequentialPatternReportSet& report,
    const SequentialPrintOptions& sourceOptions) {
    std::ofstream output(outputPath, std::ios::out | std::ios::trunc);
    if (!output) {
        return false;
    }

    SequentialPrintOptions artifactOptions = sourceOptions;
    artifactOptions.summaryOnly = false;
    artifactOptions.recordWindowExplicit = false;
    artifactOptions.recordOffset = 0;
    artifactOptions.recordLimit = std::numeric_limits<size_t>::max();
    {
        ScopedCoutBufferRedirect redirect(output.rdbuf());
        printSequentialPatternReport(report, artifactOptions);
    }
    output.flush();
    return static_cast<bool>(output);
}

std::string equivalenceMethodName(EquivalenceCheckMethod method) {
    switch (method) {
        case EquivalenceCheckMethod::StructuralIdentity: return "StructuralIdentity";
        case EquivalenceCheckMethod::LocalRewriteRule: return "LocalRewriteRule";
        case EquivalenceCheckMethod::CertifiedRewrite: return "CertifiedRewrite";
        case EquivalenceCheckMethod::WholeDesignSat: return "WholeDesignSat";
        case EquivalenceCheckMethod::NotChecked:
        default:
            return "NotChecked";
    }
}

std::string editOperationKindName(NetlistEditOperationKind kind) {
    switch (kind) {
        case NetlistEditOperationKind::Cleanup: return "Cleanup";
        case NetlistEditOperationKind::Simplification: return "Simplification";
        case NetlistEditOperationKind::BufferInsertion: return "BufferInsertion";
        case NetlistEditOperationKind::TechnologyMapping: return "TechnologyMapping";
        case NetlistEditOperationKind::PrimitiveMutation: return "PrimitiveMutation";
        case NetlistEditOperationKind::DepthOptimization: return "DepthOptimization";
        case NetlistEditOperationKind::CustomRewrite: return "CustomRewrite";
        case NetlistEditOperationKind::Unknown:
        default:
            return "Unknown";
    }
}

std::string optPassKindName(OptPassKind kind) {
    switch (kind) {
        case OptPassKind::CleanupBufferChain: return "cleanup_buffer_chain";
        case OptPassKind::CollapseDoubleInverter: return "collapse_double_inverter";
        case OptPassKind::LocalSimplificationFixpoint:
            return "local_simplification_fixpoint";
        case OptPassKind::CriticalPathDepth: return "critical_path_depth";
        case OptPassKind::Unknown:
        default:
            return "unknown";
    }
}

void printGateTypeMap(const Netlist& netlist,
                      const std::string& title,
                      const std::map<GateType, int>& values) {
    std::cout << title << " (" << values.size() << "):\n";
    for (const auto& item : values) {
        std::cout << "  " << netlist.gateTypeToString(item.first)
                  << " : " << item.second << "\n";
    }
}

void printGateTypeList(const Netlist& netlist,
                       const std::string& title,
                       const std::vector<GateType>& values) {
    std::cout << title << " (" << values.size() << "):\n";
    for (GateType type : values) {
        std::cout << "  " << netlist.gateTypeToString(type) << "\n";
    }
}

void printIntList(const std::string& title, const std::vector<int>& values) {
    std::cout << title << " (" << values.size() << "):\n";
    for (int value : values) {
        std::cout << "  " << value << "\n";
    }
}

void printNetlistStats(const Netlist& netlist,
                       const std::string& title,
                       const NetlistStats& stats) {
    std::cout << title << ":\n";
    std::cout << "    gate_count: " << stats.gateCount << "\n";
    std::cout << "    active_gate_count: " << stats.activeGateCount << "\n";
    std::cout << "    removed_gate_count: " << stats.removedGateCount << "\n";
    std::cout << "    net_count: " << stats.netCount << "\n";
    std::cout << "    active_net_count: " << stats.activeNetCount << "\n";
    std::cout << "    removed_net_count: " << stats.removedNetCount << "\n";
    std::cout << "    primary_input_count: " << stats.primaryInputCount << "\n";
    std::cout << "    primary_output_count: " << stats.primaryOutputCount << "\n";
    std::cout << "    dff_count: " << stats.dffCount << "\n";
    std::cout << "    combinational_gate_count: " << stats.combinationalGateCount << "\n";
    printGateTypeMap(netlist, "    gate_type_counts", stats.gateTypeCounts);
}

void printEditReport(const Netlist& netlist, const Netlist::NetlistEditReport& report) {
    std::cout << "  report_success: " << (report.success ? "true" : "false") << "\n";
    std::cout << "  report_changed: " << (report.changed ? "true" : "false") << "\n";
    std::cout << "  rolled_back: " << (report.rolledBack ? "true" : "false") << "\n";
    std::cout << "  operation_kind: " << editOperationKindName(report.operationKind) << "\n";
    std::cout << "  operation_name: " << report.operationName << "\n";
    std::cout << "  report_message: " << report.message << "\n";

    printNetlistStats(netlist, "  before_stats", report.beforeStats);
    printNetlistStats(netlist, "  after_stats", report.afterStats);

    std::cout << "  diff:\n";
    std::cout << "    gate_count_delta: " << report.diff.gateCountDelta << "\n";
    std::cout << "    active_gate_count_delta: " << report.diff.activeGateCountDelta << "\n";
    std::cout << "    net_count_delta: " << report.diff.netCountDelta << "\n";
    std::cout << "    active_net_count_delta: " << report.diff.activeNetCountDelta << "\n";
    std::cout << "    dff_count_delta: " << report.diff.dffCountDelta << "\n";
    std::cout << "    combinational_gate_count_delta: "
              << report.diff.combinationalGateCountDelta << "\n";
    printGateTypeMap(netlist, "    gate_type_count_delta", report.diff.gateTypeCountDelta);

    std::cout << "  validation:\n";
    std::cout << "    structure_checked: "
              << (report.validation.structureChecked ? "true" : "false") << "\n";
    std::cout << "    structure_valid: "
              << (report.validation.structureValid ? "true" : "false") << "\n";
    std::cout << "    problem_a_constraints_checked: "
              << (report.validation.problemAConstraintsChecked ? "true" : "false") << "\n";
    std::cout << "    problem_a_constraints_baseline_valid: "
              << (report.validation.problemAConstraintsBaselineValid ? "true" : "false") << "\n";
    std::cout << "    problem_a_constraints_valid: "
              << (report.validation.problemAConstraintsValid ? "true" : "false") << "\n";
    std::cout << "    problem_a_constraints_regressed: "
              << (report.validation.problemAConstraintsRegressed ? "true" : "false") << "\n";
    std::cout << "    equivalence_checked: "
              << (report.validation.equivalenceChecked ? "true" : "false") << "\n";
    std::cout << "    functionally_equivalent: "
              << (report.validation.functionallyEquivalent ? "true" : "false") << "\n";
    std::cout << "    equivalence_method: "
              << equivalenceMethodName(report.validation.equivalenceMethod) << "\n";
    printStringList("    validation_messages", report.validation.messages);
    printStringList(
        "    new_problem_a_constraint_violations",
        report.validation.newProblemAConstraintViolations);

    if (report.depthChange) {
        std::cout << "  depth_change:\n";
        std::cout << "    endpoint_name: " << report.depthChange->endpointName << "\n";
        std::cout << "    before_depth: " << report.depthChange->beforeDepth << "\n";
        std::cout << "    after_depth: " << report.depthChange->afterDepth << "\n";
        std::cout << "    target_depth: " << report.depthChange->targetDepth << "\n";
        std::cout << "    improved: " << (report.depthChange->improved ? "true" : "false") << "\n";
        std::cout << "    meets_target: " << (report.depthChange->meetsTarget ? "true" : "false") << "\n";
    }

    if (report.fanoutChange) {
        std::cout << "  fanout_change:\n";
        std::cout << "    before_max_fanout: " << report.fanoutChange->beforeMaxFanout << "\n";
        std::cout << "    after_max_fanout: " << report.fanoutChange->afterMaxFanout << "\n";
        std::cout << "    target_fanout: " << report.fanoutChange->targetFanout << "\n";
        std::cout << "    improved: " << (report.fanoutChange->improved ? "true" : "false") << "\n";
        std::cout << "    meets_constraint: "
                  << (report.fanoutChange->meetsConstraint ? "true" : "false") << "\n";
        printStringList("    violating_net_names", report.fanoutChange->violatingNetNames);
    }

    if (report.mappingDelta) {
        std::cout << "  mapping_delta:\n";
        printGateTypeMap(netlist, "    removed_count_by_type", report.mappingDelta->removedCountByType);
        printGateTypeMap(netlist, "    added_count_by_type", report.mappingDelta->addedCountByType);
        printGateTypeMap(netlist, "    final_gate_count_by_type", report.mappingDelta->finalGateCountByType);
        printStringList("    modified_gate_names", report.mappingDelta->modifiedGateNames);
    }

    if (report.constantSimplification) {
        const auto& summary = *report.constantSimplification;
        std::cout << "  constant_simplification:\n";
        std::cout << "    target_gate_type: "
                  << (summary.targetGateType == GateType::UNKNOWN
                          ? "ALL"
                          : netlist.gateTypeToString(summary.targetGateType))
                  << "\n";
        std::cout << "    target_const_value: ";
        if (summary.targetConstValue < 0) std::cout << "ANY\n";
        else std::cout << summary.targetConstValue << "\n";
        std::cout << "    target_input_count: ";
        if (summary.targetInputCount < 0) std::cout << "ANY\n";
        else std::cout << summary.targetInputCount << "\n";
        std::cout << "    candidate_count: " << summary.candidateCount << "\n";
        std::cout << "    simplified_count: " << summary.simplifiedCount << "\n";
        std::cout << "    skipped_count: " << summary.skippedCount << "\n";
        std::cout << "    eliminated_target_gate_count: "
                  << summary.eliminatedTargetGateCount << "\n";
        printIntList("    candidate_gate_ids", summary.candidateGateIds);
        printIntList("    simplified_gate_ids", summary.simplifiedGateIds);
        printIntList("    skipped_gate_ids", summary.skippedGateIds);
        printStringList("    candidate_gate_names", summary.candidateGateNames);
        printStringList("    simplified_gate_names", summary.simplifiedGateNames);
        printStringList("    skipped_gate_names", summary.skippedGateNames);
    }

    if (report.deadLogic) {
        const auto& s = *report.deadLogic;
        std::cout << "  dead_logic:\n";
        std::cout << "    removed_gate_count: " << s.removedGateCount << "\n";
        std::cout << "    removed_net_count: " << s.removedNetCount << "\n";
        std::cout << "    removed_dff_count: " << s.removedDffCount << "\n";
        std::cout << "    include_sequential: "
                  << (s.includeSequential ? "true" : "false") << "\n";
        std::cout << "    timed_out: " << (s.timedOut ? "true" : "false") << "\n";
    }

    if (report.redundancyRemoval) {
        const auto& s = *report.redundancyRemoval;
        std::cout << "  redundancy_removal:\n";
        std::cout << "    removed_gate_count: " << s.removedGateCount << "\n";
        std::cout << "    removed_net_count: " << s.removedNetCount << "\n";
        std::cout << "    proven_redundant_pin_count: " << s.provenRedundantPinCount << "\n";
        std::cout << "    complete: " << (s.complete ? "true" : "false") << "\n";
        std::cout << "    timed_out: " << (s.timedOut ? "true" : "false") << "\n";
        std::cout << "    elapsed_seconds: " << s.elapsedSeconds << "\n";
    }

    if (report.functionalMerge) {
        const auto& summary = *report.functionalMerge;
        std::cout << "  functional_merge:\n";
        std::cout << "    scope: " << summary.scope << "\n";
        std::cout << "    scope_name: " << summary.scopeName << "\n";
        std::cout << "    gate_type_filter: "
                  << (summary.gateTypeFilter == GateType::UNKNOWN
                          ? "ANY"
                          : netlist.gateTypeToString(summary.gateTypeFilter))
                  << "\n";
        std::cout << "    search_status: " << summary.searchStatus << "\n";
        std::cout << "    search_complete: "
                  << (summary.searchComplete ? "true" : "false") << "\n";
        std::cout << "    search_timed_out: "
                  << (summary.searchTimedOut ? "true" : "false") << "\n";
        std::cout << "    whole_design_equivalence_checked: "
                  << (summary.wholeDesignEquivalenceChecked ? "true" : "false")
                  << "\n";
        std::cout << "    whole_design_equivalent: "
                  << (summary.wholeDesignEquivalent ? "true" : "false") << "\n";
        std::cout << "    whole_design_timed_out: "
                  << (summary.wholeDesignTimedOut ? "true" : "false") << "\n";
        std::cout << "    candidate_gate_count: " << summary.candidateGateCount << "\n";
        std::cout << "    equivalence_class_count: "
                  << summary.equivalenceClassCount << "\n";
        std::cout << "    equivalent_pair_count: "
                  << summary.equivalentPairCount << "\n";
        std::cout << "    sat_checks: " << summary.satChecks << "\n";
        std::cout << "    merged_gate_count: " << summary.mergedGateCount << "\n";
        std::cout << "    skipped_gate_count: " << summary.skippedGateCount << "\n";
        std::cout << "    search_elapsed_seconds: "
                  << summary.searchElapsedSeconds << "\n";
        std::cout << "    total_elapsed_seconds: "
                  << summary.totalElapsedSeconds << "\n";
        std::cout << "    merge_record_count: " << summary.records.size() << "\n";
        for (size_t index = 0; index < summary.records.size(); ++index) {
            const auto& record = summary.records[index];
            std::cout << "    merge_record " << (index + 1) << ":\n";
            std::cout << "      representative_gate: "
                      << record.representativeGateName << "\n";
            std::cout << "      representative_gate_id: "
                      << record.representativeGateId << "\n";
            std::cout << "      representative_net: "
                      << record.representativeNetName << "\n";
            std::cout << "      representative_net_id: "
                      << record.representativeNetId << "\n";
            std::cout << "      removed_gate: " << record.removedGateName << "\n";
            std::cout << "      removed_gate_id: " << record.removedGateId << "\n";
            std::cout << "      removed_net: " << record.removedNetName << "\n";
            std::cout << "      removed_net_id: " << record.removedNetId << "\n";
        }
        printStringList("    skipped_gate_names", summary.skippedGateNames);
    }

    if (report.depthOptimization) {
        const auto& summary = *report.depthOptimization;
        std::cout << "  depth_optimization:\n";
        std::cout << "    objective_metric: " << summary.objectiveMetric << "\n";
        std::cout << "    scope: " << summary.scope << "\n";
        std::cout << "    requested_scope_name: "
                  << summary.requestedScopeName << "\n";
        std::cout << "    basis_scope: " << summary.basisScope << "\n";
        std::cout << "    basis_scope_name: " << summary.basisScopeName << "\n";
        std::cout << "    resolved_root_net_name: "
                  << summary.resolvedRootNetName << "\n";
        std::cout << "    core_status: " << summary.coreStatus << "\n";
        std::cout << "    core_message: " << summary.coreMessage << "\n";
        printGateTypeList(
            netlist, "    allowed_gate_types", summary.allowedTypes);
        printGateTypeList(
            netlist, "    banned_gate_types", summary.bannedTypes);
        std::cout << "    baseline_constraints_satisfied: "
                  << (summary.baselineConstraintsSatisfied ? "true" : "false")
                  << "\n";
        std::cout << "    final_constraints_satisfied: "
                  << (summary.finalConstraintsSatisfied ? "true" : "false")
                  << "\n";
        std::cout << "    candidate_generated: "
                  << (summary.candidateGenerated ? "true" : "false") << "\n";
        std::cout << "    candidate_accepted: "
                  << (summary.candidateAccepted ? "true" : "false") << "\n";
        std::cout << "    whole_design_equivalence_checked: "
                  << (summary.wholeDesignEquivalenceChecked ? "true" : "false")
                  << "\n";
        std::cout << "    whole_design_equivalent: "
                  << (summary.wholeDesignEquivalent ? "true" : "false")
                  << "\n";
        std::cout << "    whole_design_timed_out: "
                  << (summary.wholeDesignTimedOut ? "true" : "false")
                  << "\n";
        std::cout << "    compared_output_count: "
                  << summary.comparedOutputCount << "\n";
        std::cout << "    compared_dff_d_count: "
                  << summary.comparedDffDCount << "\n";
        std::cout << "    time_budget_seconds: "
                  << summary.timeBudgetSeconds << "\n";
        std::cout << "    elapsed_seconds: " << summary.elapsedSeconds << "\n";
    }

    printIntList("  changed_gate_ids", report.changedGateIds);
    printIntList("  changed_net_ids", report.changedNetIds);
    printStringList("  changed_gate_names", report.changedGateNames);
    printStringList("  changed_net_names", report.changedNetNames);
    printStringList("  warnings", report.warnings);
}

void printOptQueryReport(const Netlist& netlist,
                         const Netlist::OptQueryReport& report) {
    std::cout << "  report_ok: " << (report.ok ? "true" : "false") << "\n";
    std::cout << "  pass_kind: " << optPassKindName(report.passKind) << "\n";
    std::cout << "  scope_name: " << report.scopeName << "\n";
    std::cout << "  report_message: " << report.message << "\n";
    std::cout << "  candidate_count: " << report.candidates.size() << "\n";
    for (const Netlist::OptCandidate& candidate : report.candidates) {
        std::cout << "  candidate:\n";
        std::cout << "    id: " << candidate.id << "\n";
        std::cout << "    pass_kind: "
                  << optPassKindName(candidate.passKind) << "\n";
        std::cout << "    reason: " << candidate.reason << "\n";
        std::cout << "    estimated_gate_delta: "
                  << candidate.estimatedGateDelta << "\n";
        std::cout << "    estimated_net_delta: "
                  << candidate.estimatedNetDelta << "\n";
        std::cout << "    requires_equivalence_check: "
                  << (candidate.requiresEquivalenceCheck ? "true" : "false")
                  << "\n";
        printIntList("    gate_ids", candidate.gateIds);
        printIntList("    net_ids", candidate.netIds);
        printStringList("    gate_names", candidate.gateNames);
        printStringList("    net_names", candidate.netNames);
    }
    printStringList("  warnings", report.warnings);
}

void printWholeDesignEquivalenceReport(
    const Netlist::WholeDesignEquivalenceReport& report) {
    std::cout << "  report_ok: " << (report.ok ? "true" : "false") << "\n";
    std::cout << "  equivalent: " << (report.equivalent ? "true" : "false") << "\n";
    std::cout << "  method: " << equivalenceMethodName(report.method) << "\n";
    std::cout << "  report_message: " << report.message << "\n";
    std::cout << "  time_budget_seconds: " << report.timeBudgetSeconds << "\n";
    std::cout << "  time_budget_exceeded: "
              << (report.timeBudgetExceeded ? "true" : "false") << "\n";
    std::cout << "  compared_output_count: " << report.comparedOutputCount << "\n";
    std::cout << "  skipped_output_count: " << report.skippedOutputCount << "\n";
    std::cout << "  compared_dff_d_count: " << report.comparedDffDCount << "\n";
    std::cout << "  skipped_dff_d_count: " << report.skippedDffDCount << "\n";
    printStringList("  matched_output_names", report.matchedOutputNames);
    printStringList("  mismatched_output_names", report.mismatchedOutputNames);
    printStringList("  skipped_output_names", report.skippedOutputNames);
    printStringList("  matched_dff_d_names", report.matchedDffDNames);
    printStringList("  mismatched_dff_d_names", report.mismatchedDffDNames);
    printStringList("  skipped_dff_d_names", report.skippedDffDNames);
    printStringList("  missing_input_names", report.missingInputNames);
    printStringList("  extra_input_names", report.extraInputNames);
    printStringList("  missing_output_names", report.missingOutputNames);
    printStringList("  extra_output_names", report.extraOutputNames);
    printStringList("  missing_dff_names", report.missingDffNames);
    printStringList("  extra_dff_names", report.extraDffNames);
    printStringList("  unsupported_reasons", report.unsupportedReasons);
    printStringList("  warnings", report.warnings);
}

bool parseStrictInteger(const std::string& token, int& value);
bool parseStrictDouble(const std::string& token, double& value);
bool parseConstantFilter(const std::string& token, int& value);

bool buildSequentialPatternQuery(
    std::istringstream& iss,
    const std::string& mode,
    Netlist::SequentialPatternQuery& query,
    SequentialPrintOptions& printOptions,
    std::string& error) {
    if (toLower(mode) != "enable_hold") {
        error = "Unknown sequential_query mode: " + mode;
        return false;
    }

    std::string target;
    if (!(iss >> target)) {
        error = "enable_hold requires target all or a DFF instance name.";
        return false;
    }
    if (toLower(target) != "all") {
        query.dffName = target;
    }

    bool functionalOptionsConfigured = false;
    std::string option;
    while (iss >> option) {
        const std::string lowered = toLower(option);
        if (lowered == "--summary-only" || lowered == "-summary_only") {
            printOptions.summaryOnly = true;
        } else if (lowered == "--include-no-pattern" || lowered == "-include_no_pattern") {
            printOptions.includeNoPattern = true;
        } else if (lowered == "--confirmed-only" || lowered == "-confirmed_only") {
            query.includeAndGatedCandidates = false;
        } else if (lowered == "--include-and-candidates" ||
                   lowered == "-include_and_candidates") {
            query.includeAndGatedCandidates = true;
        } else if (lowered == "--verify-sat" || lowered == "-verify_sat") {
            query.verifyCanonicalMatchesWithSat = true;
        } else if (lowered == "--functional-fallback" ||
                   lowered == "-functional_fallback") {
            query.enableFunctionalFallback = true;
        } else if (lowered == "--max-functional-candidates" ||
                   lowered == "-max_functional_candidates") {
            std::string valueToken;
            int value = -1;
            if (!(iss >> valueToken) || !parseStrictInteger(valueToken, value) || value < 1) {
                error = "--max-functional-candidates requires an integer greater than zero.";
                return false;
            }
            query.maxFunctionalCandidates = static_cast<size_t>(value);
            functionalOptionsConfigured = true;
        } else if (lowered == "--max-functional-matches" ||
                   lowered == "-max_functional_matches") {
            std::string valueToken;
            int value = -1;
            if (!(iss >> valueToken) || !parseStrictInteger(valueToken, value) || value < 1) {
                error = "--max-functional-matches requires an integer greater than zero.";
                return false;
            }
            query.maxFunctionalMatchesPerDff = static_cast<size_t>(value);
            functionalOptionsConfigured = true;
        } else if (lowered == "--functional-find-any" ||
                   lowered == "-functional_find_any") {
            query.findAllFunctionalMatches = false;
            functionalOptionsConfigured = true;
        } else if (lowered == "--resolve-functional-data" ||
                   lowered == "-resolve_functional_data") {
            query.resolveFunctionalDataNets = true;
            functionalOptionsConfigured = true;
        } else if (lowered == "--no-resolve-functional-data" ||
                   lowered == "-no_resolve_functional_data") {
            query.resolveFunctionalDataNets = false;
            functionalOptionsConfigured = true;
        } else if (lowered == "--max-functional-data-candidates" ||
                   lowered == "-max_functional_data_candidates") {
            std::string valueToken;
            int value = -1;
            if (!(iss >> valueToken) || !parseStrictInteger(valueToken, value) || value < 1) {
                error = "--max-functional-data-candidates requires an integer greater than zero.";
                return false;
            }
            query.maxFunctionalDataCandidatesPerMatch = static_cast<size_t>(value);
            functionalOptionsConfigured = true;
        } else if (lowered == "--no-functional-simulation-filter" ||
                   lowered == "-no_functional_simulation_filter") {
            query.enableFunctionalSimulationFilter = false;
            functionalOptionsConfigured = true;
        } else if (lowered == "--functional-simulation-patterns" ||
                   lowered == "-functional_simulation_patterns") {
            std::string valueToken;
            int value = -1;
            if (!(iss >> valueToken) || !parseStrictInteger(valueToken, value) ||
                value < 1 || value > 4096) {
                error = "--functional-simulation-patterns requires an integer from 1 to 4096.";
                return false;
            }
            query.functionalSimulationPatternCount = static_cast<size_t>(value);
            functionalOptionsConfigured = true;
        } else if (lowered == "--functional-per-dff-time-limit" ||
                   lowered == "-functional_per_dff_time_limit") {
            std::string valueToken;
            double value = 0.0;
            if (!(iss >> valueToken) || !parseStrictDouble(valueToken, value) || value <= 0.0) {
                error = "--functional-per-dff-time-limit requires a positive number of seconds.";
                return false;
            }
            query.functionalPerDffTimeLimitSeconds = value;
            functionalOptionsConfigured = true;
        } else if (lowered == "--functional-time-limit" ||
                   lowered == "-functional_time_limit") {
            std::string valueToken;
            double value = 0.0;
            if (!(iss >> valueToken) || !parseStrictDouble(valueToken, value) || value <= 0.0) {
                error = "--functional-time-limit requires a positive number of seconds.";
                return false;
            }
            query.functionalTimeLimitSeconds = value;
            functionalOptionsConfigured = true;
        } else if (lowered == "--offset" || lowered == "-offset") {
            std::string valueToken;
            int value = -1;
            if (!(iss >> valueToken) || !parseStrictInteger(valueToken, value) || value < 0) {
                error = "--offset requires a non-negative integer.";
                return false;
            }
            printOptions.recordOffset = static_cast<size_t>(value);
            printOptions.recordWindowExplicit = true;
        } else if (lowered == "--limit" || lowered == "-limit") {
            std::string valueToken;
            int value = -1;
            if (!(iss >> valueToken) || !parseStrictInteger(valueToken, value) || value < 1) {
                error = "--limit requires an integer greater than zero.";
                return false;
            }
            printOptions.recordLimit = static_cast<size_t>(value);
            printOptions.recordWindowExplicit = true;
        } else {
            error = "Unknown sequential_query option: " + option;
            return false;
        }
    }

    if (functionalOptionsConfigured && !query.enableFunctionalFallback) {
        error = "Functional search options require --functional-fallback.";
        return false;
    }
    if (query.dffName.empty() && query.verifyCanonicalMatchesWithSat) {
        error = "--verify-sat requires a specific DFF target; all-DFF SAT verification "
                "may exceed the testcase time limit.";
        return false;
    }
    if (printOptions.summaryOnly && printOptions.recordWindowExplicit) {
        error = "--summary-only cannot be combined with --offset or --limit.";
        return false;
    }
    return true;
}

bool parseBasicGateType(const Netlist& netlist,
                        const std::string& token,
                        GateType& type,
                        std::string& error) {
    type = netlist.stringToGateType(token);
    if (type != GateType::UNKNOWN) return true;
    error = "Unsupported gate type '" + token +
            "'. Supported types: AND OR NOT NAND NOR XOR XNOR BUF DFF";
    return false;
}

bool consumeBasicGateTypeList(const Netlist& netlist,
                              const std::vector<std::string>& tokens,
                              size_t& index,
                              const std::string& option,
                              std::vector<GateType>& destination,
                              std::string& error) {
    const size_t firstType = index;
    while (index < tokens.size() && !isOptionToken(tokens[index])) {
        GateType type = GateType::UNKNOWN;
        if (!parseBasicGateType(netlist, tokens[index], type, error)) return false;
        destination.push_back(type);
        ++index;
    }
    if (index == firstType) {
        error = option + " requires at least one gate type";
        return false;
    }
    return true;
}

bool parseBasicGateSelectionOptions(const Netlist& netlist,
                                    const std::vector<std::string>& tokens,
                                    bool allowWithPins,
                                    Netlist::BasicQuery& query,
                                    std::string& error) {
    size_t index = 0;
    bool concretePositionalType = false;
    if (index < tokens.size() && !isOptionToken(tokens[index])) {
        const std::string lowered = toLower(tokens[index]);
        if (lowered != "all" && lowered != "any") {
            if (!parseBasicGateType(netlist, tokens[index], query.gateType, error)) {
                return false;
            }
            concretePositionalType = true;
        }
        ++index;
    }

    bool sawGateTypes = false;
    bool sawExcludedGateTypes = false;
    bool sawWithPins = false;
    while (index < tokens.size()) {
        const std::string option = toLower(tokens[index++]);
        if (option == "--gate-types") {
            if (sawGateTypes) {
                error = "Duplicate structure_query option: --gate-types";
                return false;
            }
            if (concretePositionalType) {
                error = "Cannot combine a positional gate type with --gate-types";
                return false;
            }
            sawGateTypes = true;
            if (!consumeBasicGateTypeList(netlist, tokens, index, "--gate-types",
                                          query.gateTypeFilters, error)) {
                return false;
            }
            continue;
        }
        if (option == "--exclude-gate-types") {
            if (sawExcludedGateTypes) {
                error = "Duplicate structure_query option: --exclude-gate-types";
                return false;
            }
            sawExcludedGateTypes = true;
            if (!consumeBasicGateTypeList(netlist, tokens, index,
                                          "--exclude-gate-types",
                                          query.excludedGateTypeFilters, error)) {
                return false;
            }
            continue;
        }
        if (option == "--with-pins") {
            if (!allowWithPins) {
                error = "count_by_type does not support --with-pins";
                return false;
            }
            if (sawWithPins) {
                error = "Duplicate structure_query option: --with-pins";
                return false;
            }
            sawWithPins = true;
            query.includeConnectionDetails = true;
            continue;
        }
        error = isOptionToken(option)
            ? "Unknown structure_query option: " + option
            : "Unexpected structure_query argument: " + option;
        return false;
    }
    return true;
}

bool parseBasicConstantGateOptions(const Netlist& netlist,
                                   const std::vector<std::string>& tokens,
                                   Netlist::BasicQuery& query,
                                   std::string& error) {
    size_t index = 0;
    bool concretePositionalType = false;
    bool positionalConst = false;
    if (index < tokens.size() && !isOptionToken(tokens[index])) {
        const std::string lowered = toLower(tokens[index]);
        if (lowered != "all" && lowered != "any") {
            if (!parseBasicGateType(netlist, tokens[index], query.gateType, error)) {
                return false;
            }
            concretePositionalType = true;
        }
        ++index;
    }
    if (index < tokens.size() && !isOptionToken(tokens[index])) {
        if (!parseConstantFilter(tokens[index++], query.constValue)) {
            error = "Constant filter must be 0, 1, or any";
            return false;
        }
        positionalConst = true;
    }

    bool sawGateTypes = false;
    bool sawExcludedGateTypes = false;
    bool sawConst = false;
    bool sawInputs = false;
    bool sawWithPins = false;
    while (index < tokens.size()) {
        const std::string option = toLower(tokens[index++]);
        if (option == "--gate-types") {
            if (sawGateTypes) {
                error = "Duplicate structure_query option: --gate-types";
                return false;
            }
            if (concretePositionalType) {
                error = "Cannot combine a positional gate type with --gate-types";
                return false;
            }
            sawGateTypes = true;
            if (!consumeBasicGateTypeList(netlist, tokens, index, "--gate-types",
                                          query.gateTypeFilters, error)) {
                return false;
            }
            continue;
        }
        if (option == "--exclude-gate-types") {
            if (sawExcludedGateTypes) {
                error = "Duplicate structure_query option: --exclude-gate-types";
                return false;
            }
            sawExcludedGateTypes = true;
            if (!consumeBasicGateTypeList(netlist, tokens, index,
                                          "--exclude-gate-types",
                                          query.excludedGateTypeFilters, error)) {
                return false;
            }
            continue;
        }
        if (option == "--const") {
            if (sawConst || positionalConst) {
                error = "Constant filter was specified more than once";
                return false;
            }
            sawConst = true;
            if (index >= tokens.size() || isOptionToken(tokens[index]) ||
                !parseConstantFilter(tokens[index++], query.constValue)) {
                error = "--const requires 0, 1, or any";
                return false;
            }
            continue;
        }
        if (option == "--inputs" || option == "-inputs") {
            if (sawInputs || index >= tokens.size() ||
                !parseStrictInteger(tokens[index++], query.inputCount) ||
                query.inputCount < 1) {
                error = "--inputs requires one positive integer";
                return false;
            }
            sawInputs = true;
            continue;
        }
        if (option == "--with-pins") {
            if (sawWithPins) {
                error = "Duplicate structure_query option: --with-pins";
                return false;
            }
            sawWithPins = true;
            query.includeConnectionDetails = true;
            continue;
        }
        error = isOptionToken(option)
            ? "Unknown structure_query option: " + option
            : "Unexpected structure_query argument: " + option;
        return false;
    }
    return true;
}

bool requireNoBasicArguments(std::istringstream& iss,
                             const std::string& mode,
                             std::string& error) {
    std::string trailing;
    if (!(iss >> trailing)) return true;
    error = mode + " does not accept arguments; unexpected argument: " + trailing;
    return false;
}

bool requireOneBasicObjectArgument(std::istringstream& iss,
                                   const std::string& mode,
                                   const std::string& argumentName,
                                   std::string& value,
                                   std::string& error) {
    if (!(iss >> value) || isOptionToken(value)) {
        error = mode + " requires <" + argumentName + ">";
        return false;
    }
    std::string trailing;
    if (iss >> trailing) {
        error = "Unexpected structure_query argument: " + trailing;
        return false;
    }
    return true;
}

// 將 basic_query 的 mode 轉成 BasicQuery；recognized mode 的參數錯誤寫入 error。
bool buildBasicQuery(const Netlist& netlist,
                     std::istringstream& iss,
                     const std::string& mode,
                     Netlist::BasicQuery& query,
                     std::string& error) {
    const std::string m = toLower(mode);
    if (m == "summary") {
        query.type = Netlist::BasicQueryType::Summary;
        query.includeIds = false;
        query.includeNames = false;
        return requireNoBasicArguments(iss, m, error);
    } else if (m == "list_gates") {
        query.type = Netlist::BasicQueryType::ListGates;
        return requireNoBasicArguments(iss, m, error);
    } else if (m == "list_nets") {
        query.type = Netlist::BasicQueryType::ListNets;
        return requireNoBasicArguments(iss, m, error);
    } else if (m == "net_classes") {
        query.type = Netlist::BasicQueryType::NetClassification;
        return requireNoBasicArguments(iss, m, error);
    } else if (m == "list_pi") {
        query.type = Netlist::BasicQueryType::ListPrimaryInputs;
        return requireNoBasicArguments(iss, m, error);
    } else if (m == "list_po") {
        query.type = Netlist::BasicQueryType::ListPrimaryOutputs;
        return requireNoBasicArguments(iss, m, error);
    } else if (m == "list_dffs") {
        query.type = Netlist::BasicQueryType::ListDffs;
        return requireNoBasicArguments(iss, m, error);
    } else if (m == "list_comb" || m == "list_comb_gates") {
        query.type = Netlist::BasicQueryType::ListCombinationalGates;
        return requireNoBasicArguments(iss, m, error);
    } else if (m == "gate_info") {
        query.type = Netlist::BasicQueryType::GateInfo;
        return requireOneBasicObjectArgument(
            iss, m, "gate", query.name, error);
    } else if (m == "net_info") {
        query.type = Netlist::BasicQueryType::NetInfo;
        return requireOneBasicObjectArgument(
            iss, m, "net", query.name, error);
    } else if (m == "port_info") {
        query.type = Netlist::BasicQueryType::PortInfo;
        return requireOneBasicObjectArgument(
            iss, m, "port", query.name, error);
    } else if (m == "count_by_type") {
        query.type = Netlist::BasicQueryType::CountByGateType;
        std::vector<std::string> tokens;
        std::string token;
        while (iss >> token) tokens.push_back(token);
        return parseBasicGateSelectionOptions(netlist, tokens, false, query, error);
    } else if (m == "gates_by_type") {
        query.type = Netlist::BasicQueryType::GatesByType;
        std::vector<std::string> tokens;
        std::string token;
        while (iss >> token) tokens.push_back(token);
        return parseBasicGateSelectionOptions(netlist, tokens, true, query, error);
    } else if (m == "const_input_gates") {
        query.type = Netlist::BasicQueryType::GatesWithConstantInput;
        std::vector<std::string> tokens;
        std::string token;
        while (iss >> token) tokens.push_back(token);
        return parseBasicConstantGateOptions(netlist, tokens, query, error);
    } else if (m == "structural_issues") {
        query.type = Netlist::BasicQueryType::StructuralIssues;
        return requireNoBasicArguments(iss, m, error);
    } else {
        return false;
    }
    return true;
}

// 將 conn_query 的 mode 轉成 DirectConnectivityQuery。
bool buildConnectivityQuery(std::istringstream& iss,
                            const std::string& mode,
                            Netlist::DirectConnectivityQuery& query,
                            std::string& error) {
    const std::string m = toLower(mode);

    const auto requireOneName = [&](const std::string& argumentName,
                                    std::string& value) {
        if (!(iss >> value) || isOptionToken(value)) {
            error = m + " requires <" + argumentName + ">";
            return false;
        }
        std::string trailing;
        if (iss >> trailing) {
            error = "Unexpected structure_query argument: " + trailing;
            return false;
        }
        return true;
    };

    const auto parseOptionalFanoutLimit = [&]() {
        std::string token;
        if (!(iss >> token)) return true;
        if (!parseStrictInteger(token, query.fanoutLimit) || query.fanoutLimit < 0) {
            error = m + " accepts only a non-negative integer fanout limit";
            return false;
        }
        std::string trailing;
        if (iss >> trailing) {
            error = "Unexpected structure_query argument: " + trailing;
            return false;
        }
        return true;
    };

    if (m == "net_driver") {
        query.type = Netlist::DirectConnectivityQueryType::NetDriverGates;
        if (!(iss >> query.netName) || isOptionToken(query.netName)) {
            error = m + " requires <net>";
            return false;
        }
        std::string option;
        if (iss >> option) {
            if (option != "--with-pins") {
                error = isOptionToken(option)
                    ? "Unknown structure_query option: " + option
                    : "Unexpected structure_query argument: " + option;
                return false;
            }
            query.includePinDetails = true;
            if (iss >> option) {
                error = "Unexpected structure_query argument: " + option;
                return false;
            }
        }
    } else if (m == "net_loads") {
        query.type = Netlist::DirectConnectivityQueryType::NetLoadGates;
        if (!(iss >> query.netName) || isOptionToken(query.netName)) {
            error = m + " requires <net>";
            return false;
        }
        std::string option;
        if (iss >> option) {
            if (option != "--with-pins") {
                error = isOptionToken(option)
                    ? "Unknown structure_query option: " + option
                    : "Unexpected structure_query argument: " + option;
                return false;
            }
            query.includePinDetails = true;
            if (iss >> option) {
                error = "Unexpected structure_query argument: " + option;
                return false;
            }
        }
    } else if (m == "fanout_load" || m == "fanout_report") {
        query.type = Netlist::DirectConnectivityQueryType::FanoutLoadReport;
        return requireOneName("net", query.netName);
    } else if (m == "global_fanout") {
        query.type = Netlist::DirectConnectivityQueryType::GlobalFanoutReport;
        return parseOptionalFanoutLimit();
    } else if (m == "pi_fanout") {
        query.type = Netlist::DirectConnectivityQueryType::GlobalFanoutReport;
        query.primaryInputsOnly = true;
        return parseOptionalFanoutLimit();
    } else if (m == "fanout_violations") {
        query.type = Netlist::DirectConnectivityQueryType::GlobalFanoutReport;
        std::string token;
        if (!(iss >> token) || !parseStrictInteger(token, query.fanoutLimit) ||
            query.fanoutLimit < 0) {
            error = m + " requires a non-negative integer <limit>";
            return false;
        }
        std::string trailing;
        if (iss >> trailing) {
            error = "Unexpected structure_query argument: " + trailing;
            return false;
        }
    } else if (m == "fanout_filter") {
        query.type = Netlist::DirectConnectivityQueryType::GlobalFanoutReport;

        std::string scopeToken;
        std::string predicateToken;
        std::string valueToken;
        if (!(iss >> scopeToken >> predicateToken >> valueToken)) {
            error = "fanout_filter requires <all|pi> "
                    "<eq|ne|gt|ge|lt|le|between> <value> [upper]";
            return false;
        }

        const std::string scope = toLower(scopeToken);
        if (scope == "all") {
            query.primaryInputsOnly = false;
        } else if (scope == "pi") {
            query.primaryInputsOnly = true;
        } else {
            error = "fanout_filter scope must be all or pi";
            return false;
        }

        if (!parseFanoutPredicate(predicateToken, query.fanoutPredicate)) {
            error = "fanout_filter predicate must be "
                    "eq, ne, gt, ge, lt, le, or between";
            return false;
        }
        if (!parseNonNegativeSize(valueToken, query.fanoutValue)) {
            error = "fanout_filter value must be a non-negative integer";
            return false;
        }

        std::string trailing;
        if (query.fanoutPredicate ==
            Netlist::FanoutPredicate::BetweenInclusive) {
            if (!(iss >> valueToken) ||
                !parseNonNegativeSize(valueToken, query.fanoutUpperValue)) {
                error = "fanout_filter between requires a non-negative integer <upper>";
                return false;
            }
            if (query.fanoutValue > query.fanoutUpperValue) {
                error = "fanout_filter lower bound must not exceed upper bound";
                return false;
            }
            if (iss >> trailing) {
                error = "Unexpected structure_query argument: " + trailing;
                return false;
            }
        } else if (iss >> trailing) {
            error = "Unexpected structure_query argument: " + trailing;
            return false;
        }
    } else if (m == "fanout_rank") {
        query.type = Netlist::DirectConnectivityQueryType::FanoutRankingReport;

        std::string scopeToken;
        std::string modeToken;
        if (!(iss >> scopeToken >> modeToken)) {
            error = "fanout_rank requires <scope> <mode> [k]";
            return false;
        }
        if (!parseFanoutScope(scopeToken, query.fanoutScope)) {
            error = "fanout_rank scope must be all, pi, po, internal, "
                    "gate_output, comb_output, or dff_output";
            return false;
        }
        if (!parseFanoutRankMode(modeToken, query.fanoutRankMode)) {
            error = "fanout_rank mode must be highest, lowest, nth_highest, "
                    "nth_lowest, top, or bottom";
            return false;
        }

        const bool requiresValue =
            query.fanoutRankMode == Netlist::FanoutRankMode::NthHighest ||
            query.fanoutRankMode == Netlist::FanoutRankMode::NthLowest ||
            query.fanoutRankMode == Netlist::FanoutRankMode::Top ||
            query.fanoutRankMode == Netlist::FanoutRankMode::Bottom;
        std::string token;
        if (requiresValue) {
            if (!(iss >> token) ||
                !parseNonNegativeSize(token, query.fanoutRankValue) ||
                query.fanoutRankValue == 0) {
                error = "fanout_rank " + toLower(modeToken) +
                        " requires a positive integer <k>";
                return false;
            }
        } else {
            query.fanoutRankValue = 1;
        }
        if (iss >> token) {
            error = "Unexpected structure_query argument: " + token;
            return false;
        }
    } else if (m == "gate_inputs") {
        query.type = Netlist::DirectConnectivityQueryType::GateInputs;
        return requireOneName("gate", query.gateName);
    } else if (m == "gate_output") {
        query.type = Netlist::DirectConnectivityQueryType::GateOutput;
        return requireOneName("gate", query.gateName);
    } else if (m == "gate_fanin") {
        query.type = Netlist::DirectConnectivityQueryType::GateFanin;
        return requireOneName("gate", query.gateName);
    } else if (m == "gate_fanout") {
        query.type = Netlist::DirectConnectivityQueryType::GateFanout;
        return requireOneName("gate", query.gateName);
    } else if (m == "is_connected") {
        query.type = Netlist::DirectConnectivityQueryType::DirectlyConnected;
        if (!(iss >> query.gateName) || isOptionToken(query.gateName) ||
            !(iss >> query.netName) || isOptionToken(query.netName)) {
            error = m + " requires <gate> <net>";
            return false;
        }
        std::string trailing;
        if (iss >> trailing) {
            error = "Unexpected structure_query argument: " + trailing;
            return false;
        }
    } else {
        return false;
    }
    return true;
}

// 將 cone_query 的 mode 轉成 ConeQuery。
bool buildConeQuery(const Netlist& netlist,
                    std::istringstream& iss,
                    const std::string& mode,
                    Netlist::ConeQuery& query,
                    std::string& errorMessage) {
    const std::string m = toLower(mode);
    if (m == "net_fanin") {
        query.type = Netlist::ConeQueryType::NetTransitiveFanin;
        if (!(iss >> query.netName) || isOptionToken(query.netName)) {
            errorMessage = "net_fanin requires <net>";
            return false;
        }
    } else if (m == "net_fanout") {
        query.type = Netlist::ConeQueryType::NetTransitiveFanout;
        if (!(iss >> query.netName) || isOptionToken(query.netName)) {
            errorMessage = "net_fanout requires <net>";
            return false;
        }
    } else if (m == "gate_fanin") {
        query.type = Netlist::ConeQueryType::GateTransitiveFanin;
        if (!(iss >> query.gateName) || isOptionToken(query.gateName)) {
            errorMessage = "gate_fanin requires <gate>";
            return false;
        }
    } else if (m == "gate_fanout") {
        query.type = Netlist::ConeQueryType::GateTransitiveFanout;
        if (!(iss >> query.gateName) || isOptionToken(query.gateName)) {
            errorMessage = "gate_fanout requires <gate>";
            return false;
        }
    } else if (m == "largest_output") {
        query.type = Netlist::ConeQueryType::LargestOutputCone;
    } else if (m == "output_rank") {
        query.type = Netlist::ConeQueryType::OutputConeRanking;

        std::string metricToken;
        std::string modeToken;
        if (!(iss >> metricToken >> modeToken)) {
            errorMessage =
                "output_rank requires <metric> <mode> [k]";
            return false;
        }
        if (!parseConeRankMetric(metricToken, query.rankMetric)) {
            errorMessage =
                "output_rank metric must be gates, filtered_gates, or nets";
            return false;
        }
        if (!parseConeRankMode(modeToken, query.rankMode)) {
            errorMessage = "output_rank mode must be highest, lowest, "
                           "nth_highest, nth_lowest, top, or bottom";
            return false;
        }

        const bool requiresValue =
            query.rankMode == Netlist::ConeRankMode::NthHighest ||
            query.rankMode == Netlist::ConeRankMode::NthLowest ||
            query.rankMode == Netlist::ConeRankMode::Top ||
            query.rankMode == Netlist::ConeRankMode::Bottom;
        if (requiresValue) {
            std::string valueToken;
            if (!(iss >> valueToken) ||
                !parseNonNegativeSize(valueToken, query.rankValue) ||
                query.rankValue == 0) {
                errorMessage = "output_rank " + toLower(modeToken) +
                               " requires a positive integer <k>";
                return false;
            }
        } else {
            query.rankValue = 1;
        }
    } else if (m == "output_filter") {
        query.type = Netlist::ConeQueryType::OutputConeFilter;

        std::string metricToken;
        std::string predicateToken;
        std::string valueToken;
        if (!(iss >> metricToken >> predicateToken >> valueToken)) {
            errorMessage =
                "output_filter requires <metric> <predicate> <value> [upper]";
            return false;
        }
        if (!parseConeRankMetric(metricToken, query.rankMetric)) {
            errorMessage =
                "output_filter metric must be gates, filtered_gates, or nets";
            return false;
        }
        if (!parseConeMetricPredicate(predicateToken,
                                      query.metricPredicate)) {
            errorMessage = "output_filter predicate must be "
                           "eq, ne, gt, ge, lt, le, or between";
            return false;
        }
        if (!parseNonNegativeSize(valueToken, query.metricValue)) {
            errorMessage =
                "output_filter value must be a non-negative integer";
            return false;
        }
        if (query.metricPredicate ==
            Netlist::ConeMetricPredicate::BetweenInclusive) {
            if (!(iss >> valueToken) ||
                !parseNonNegativeSize(valueToken,
                                      query.metricUpperValue)) {
                errorMessage = "output_filter between requires a "
                               "non-negative integer <upper>";
                return false;
            }
            if (query.metricValue > query.metricUpperValue) {
                errorMessage = "output_filter lower bound must not exceed "
                               "upper bound";
                return false;
            }
        }
    } else if (m == "shared_fanin") {
        query.type = Netlist::ConeQueryType::SharedFaninGates;
        if (!(iss >> query.netName) || isOptionToken(query.netName) ||
            !(iss >> query.secondNetName) ||
            isOptionToken(query.secondNetName)) {
            errorMessage = "shared_fanin requires <net_a> <net_b>";
            return false;
        }
    } else {
        errorMessage = "Unknown cone_query mode: " + mode;
        return false;
    }

    std::vector<std::string> tokens;
    std::string token;
    while (iss >> token) tokens.push_back(token);

    bool sawPaths = false;
    bool sawPins = false;
    bool sawGateTypes = false;
    for (size_t i = 0; i < tokens.size();) {
        const std::string option = toLower(tokens[i]);
        if (option == "with_paths" || option == "--with-paths") {
            if (sawPaths) {
                errorMessage = "Duplicate cone_query path option";
                return false;
            }
            sawPaths = true;
            query.includeLocalPaths = true;
            ++i;
            continue;
        }
        if (option == "--with-pins") {
            if (sawPins) {
                errorMessage = "Duplicate cone_query option: --with-pins";
                return false;
            }
            sawPins = true;
            query.includeGateDetails = true;
            ++i;
            continue;
        }
        if (option == "--gate-types") {
            if (sawGateTypes) {
                errorMessage = "Duplicate cone_query option: --gate-types";
                return false;
            }
            sawGateTypes = true;
            ++i;
            const size_t firstType = i;
            while (i < tokens.size()) {
                const std::string candidate = toLower(tokens[i]);
                if (candidate.rfind("--", 0) == 0 || candidate == "with_paths") break;
                const GateType type = netlist.stringToGateType(tokens[i]);
                if (type == GateType::UNKNOWN) {
                    errorMessage = "Unsupported gate type '" + tokens[i] +
                        "'. Supported types: AND OR NOT NAND NOR XOR XNOR BUF DFF";
                    return false;
                }
                query.gateTypeFilters.push_back(type);
                ++i;
            }
            if (i == firstType) {
                errorMessage = "--gate-types requires at least one gate type";
                return false;
            }
            continue;
        }
        errorMessage = "Unknown cone_query option: " + tokens[i];
        return false;
    }
    if (query.type == Netlist::ConeQueryType::OutputConeFilter &&
        (query.includeLocalPaths || query.includeGateDetails)) {
        errorMessage = "output_filter is summary-only and does not support "
                       "--with-paths or --with-pins";
        return false;
    }
    return true;
}

// 將 depth_query 的 mode 轉成 DepthQuery。
bool buildDepthQuery(std::istringstream& iss,
                     const std::string& mode,
                     Netlist::DepthQuery& query,
                     std::string& error) {
    const std::string m = toLower(mode);
    auto requireValue = [&](const std::string& name, std::string& value) -> bool {
        if (!(iss >> value) || isOptionToken(value)) {
            error = name + " requires one value.";
            return false;
        }
        return true;
    };
    if (m == "net") {
        query.type = Netlist::DepthQueryType::SpecificNet;
        if (!requireValue("depth_query net", query.netName)) return false;
    } else if (m == "all_po") {
        query.type = Netlist::DepthQueryType::PrimaryOutputs;
    } else if (m == "all_dff_d") {
        query.type = Netlist::DepthQueryType::DffD;
    } else if (m == "global_critical") {
        query.type = Netlist::DepthQueryType::GlobalCriticalPath;
    } else if (m == "exceeding") {
        query.type = Netlist::DepthQueryType::EndpointsExceedingDepth;
        std::string value;
        size_t threshold = 0;
        if (!requireValue("depth_query exceeding", value) ||
            !parseNonNegativeSize(value, threshold) ||
            threshold > static_cast<size_t>(std::numeric_limits<int>::max())) {
            error = "depth_query exceeding requires a non-negative integer depth.";
            return false;
        }
        query.threshold = static_cast<int>(threshold);
        query.includeCriticalPath = false;
    } else if (m == "po_exceeding") {
        query.type = Netlist::DepthQueryType::PrimaryOutputsExceedingDepth;
        std::string value;
        size_t threshold = 0;
        if (!requireValue("depth_query po_exceeding", value) ||
            !parseNonNegativeSize(value, threshold) ||
            threshold > static_cast<size_t>(std::numeric_limits<int>::max())) {
            error = "depth_query po_exceeding requires a non-negative integer depth.";
            return false;
        }
        query.threshold = static_cast<int>(threshold);
        query.includeCriticalPath = false;
    } else if (m == "gate_on_critical") {
        query.type = Netlist::DepthQueryType::GateOnCriticalPath;
        if (!requireValue("depth_query gate_on_critical", query.gateName)) return false;
    } else if (m == "deepest_output") {
        query.type = Netlist::DepthQueryType::DeepestOutputCone;
    } else {
        error = "Unknown depth_query mode: " + mode;
        return false;
    }
    std::string trailing;
    if (iss >> trailing) {
        error = "Unexpected depth_query token: " + trailing;
        return false;
    }
    return true;
}

// 將 func_query 的 mode 轉成 FunctionQuery。
bool buildFunctionQuery(std::istringstream& iss,
                        const std::string& mode,
                        Netlist::FunctionQuery& query,
                        std::string& error) {
    const std::string m = toLower(mode);
    std::vector<std::string> tokens;
    std::string token;
    while (iss >> token) tokens.push_back(token);

    const auto requireArity = [&](size_t expected,
                                  size_t nameCount,
                                  const std::string& usage) {
        if (tokens.size() < expected) {
            error = m + " requires " + usage;
            return false;
        }
        if (tokens.size() > expected) {
            error = "Unexpected func_query argument: " + tokens[expected];
            return false;
        }
        for (size_t index = 0; index < nameCount; ++index) {
            if (isOptionToken(tokens[index])) {
                error = m + " requires " + usage;
                return false;
            }
        }
        return true;
    };

    if (m == "equivalence") {
        query.type = Netlist::FunctionQueryType::Equivalence;
        if (!requireArity(2, 2, "<net_a> <net_b>")) return false;
        query.netNameA = tokens[0];
        query.netNameB = tokens[1];
    } else if (m == "conditional_equivalence" || m == "equivalence_when") {
        query.type = Netlist::FunctionQueryType::ConditionalEquivalence;
        if (!requireArity(4, 3, "<net_a> <net_b> <condition_net> <0|1>")) {
            return false;
        }
        query.netNameA = tokens[0];
        query.netNameB = tokens[1];
        query.conditionNetName = tokens[2];
        if (!parseStrictInteger(tokens[3], query.conditionValue) ||
            (query.conditionValue != 0 && query.conditionValue != 1)) {
            error = m + " requires condition value 0 or 1";
            return false;
        }
    } else if (m == "can_be_value") {
        query.type = Netlist::FunctionQueryType::CanBeValue;
        if (!requireArity(2, 1, "<net> <0|1>")) return false;
        query.netNameA = tokens[0];
        if (!parseStrictInteger(tokens[1], query.constValue) ||
            (query.constValue != 0 && query.constValue != 1)) {
            error = m + " requires value 0 or 1";
            return false;
        }
    } else if (m == "constant") {
        query.type = Netlist::FunctionQueryType::ConstantFunction;
        if (!requireArity(2, 1, "<net> <0|1>")) return false;
        query.netNameA = tokens[0];
        if (!parseStrictInteger(tokens[1], query.constValue) ||
            (query.constValue != 0 && query.constValue != 1)) {
            error = m + " requires value 0 or 1";
            return false;
        }
    } else if (m == "always_zero") {
        query.type = Netlist::FunctionQueryType::AlwaysZero;
        if (!requireArity(1, 1, "<net>")) return false;
        query.netNameA = tokens[0];
    } else if (m == "always_one") {
        query.type = Netlist::FunctionQueryType::AlwaysOne;
        if (!requireArity(1, 1, "<net>")) return false;
        query.netNameA = tokens[0];
    } else if (m == "truth_status") {
        query.type = Netlist::FunctionQueryType::TruthStatus;
        if (!requireArity(1, 1, "<net>")) return false;
        query.netNameA = tokens[0];
    } else if (m == "depends_on" || m == "functional_dependence") {
        query.type = Netlist::FunctionQueryType::FunctionalDependence;
        if (!requireArity(2, 2, "<target_net> <input_net>")) return false;
        query.netNameA = tokens[0];
        query.netNameB = tokens[1];
    } else if (m == "symmetry" || m == "symmetric") {
        query.type = Netlist::FunctionQueryType::Symmetry;
        if (!requireArity(3, 3, "<target_net_or_bus> <input_a> <input_b>")) {
            return false;
        }
        query.netNameA = tokens[0];
        query.symmetryInputNameA = tokens[1];
        query.symmetryInputNameB = tokens[2];
    } else if (m == "boolean_expression" || m == "expression") {
        query.type = Netlist::FunctionQueryType::BooleanExpression;
        if (!requireArity(1, 1, "<net>")) return false;
        query.netNameA = tokens[0];
    } else if (m == "simplified_expression") {
        query.type = Netlist::FunctionQueryType::SimplifiedBooleanExpression;
        if (!requireArity(2, 1, "<net> <non_negative_max_depth>")) return false;
        query.netNameA = tokens[0];
        if (!parseStrictInteger(tokens[1], query.maxExpressionDepth) ||
            query.maxExpressionDepth < 0) {
            error = m + " requires a non-negative integer max depth";
            return false;
        }
    } else if (m == "support_pi" || m == "primary_inputs_of_net") {
        query.type = Netlist::FunctionQueryType::PrimaryInputsOfNet;
        if (!requireArity(1, 1, "<net>")) return false;
        query.netNameA = tokens[0];
    } else {
        return false;
    }
    return true;
}

bool buildFunctionSearchQuery(const Netlist& netlist,
                              std::istringstream& iss,
                              const std::string& mode,
                              Netlist::FunctionSearchQuery& query,
                              std::string& error) {
    const std::string loweredMode = toLower(mode);
    const bool nandSearch =
        loweredMode == "nand_pair" || loweredMode == "nand_equivalent_pairs";
    const bool patternSearch =
        loweredMode == "pattern" || loweredMode == "pattern_operands";
    const bool equivalentPairSearch =
        loweredMode == "equivalent_pairs" || loweredMode == "equivalent_gate_pairs";
    if (!nandSearch && !patternSearch && !equivalentPairSearch) {
        error = "Unknown func_search mode: " + mode;
        return false;
    }

    if (nandSearch) {
        query.type = Netlist::FunctionSearchQueryType::NandEquivalentInputPairs;
        if (!(iss >> query.targetNetName)) {
            error = "nand_pair requires a scalar target net.";
            return false;
        }
    } else if (patternSearch) {
        std::string patternTypeToken;
        if (!(iss >> patternTypeToken >> query.targetNetName)) {
            error = "pattern requires a gate type and scalar target net.";
            return false;
        }
        query.type = Netlist::FunctionSearchQueryType::FunctionalPatternOperands;
        query.patternGateType = netlist.stringToGateType(patternTypeToken);
        if (query.patternGateType == GateType::UNKNOWN ||
            query.patternGateType == GateType::DFF) {
            error = "pattern gate type must be BUF, NOT, AND, NAND, OR, NOR, XOR, or XNOR.";
            return false;
        }
    } else {
        query.type = Netlist::FunctionSearchQueryType::EquivalentGatePairs;
        std::string scopeToken;
        if (!(iss >> scopeToken)) {
            error = "equivalent_pairs requires a scope.";
            return false;
        }
        const std::string loweredScope = toLower(scopeToken);
        if (loweredScope == "whole" || loweredScope == "whole_design") {
            query.scope = Netlist::FunctionSearchScope::WholeDesign;
        } else if (loweredScope == "net_fanin") {
            query.scope = Netlist::FunctionSearchScope::NetFanin;
        } else if (loweredScope == "net_fanout") {
            query.scope = Netlist::FunctionSearchScope::NetFanout;
        } else if (loweredScope == "gate_fanin") {
            query.scope = Netlist::FunctionSearchScope::GateFanin;
        } else if (loweredScope == "gate_fanout") {
            query.scope = Netlist::FunctionSearchScope::GateFanout;
        } else {
            error = "Unknown equivalent_pairs scope: " + scopeToken;
            return false;
        }
        if (query.scope != Netlist::FunctionSearchScope::WholeDesign &&
            !(iss >> query.scopeName)) {
            error = "The selected equivalent_pairs scope requires a net or gate name.";
            return false;
        }
    }

    std::string option;
    while (iss >> option) {
        const std::string lowered = toLower(option);
        if (lowered == "--all" || lowered == "-all") {
            query.mode = Netlist::FunctionSearchMode::FindAll;
        } else if (lowered == "--find-any" || lowered == "-find_any") {
            query.mode = Netlist::FunctionSearchMode::FindAny;
        } else if (lowered == "--allow-same" || lowered == "-allow_same") {
            if (equivalentPairSearch) {
                error = "--allow-same is only valid for nand_pair.";
                return false;
            }
            if (patternSearch &&
                (query.patternGateType == GateType::BUF ||
                 query.patternGateType == GateType::NOT)) {
                error = "--allow-same is not meaningful for unary BUF/NOT patterns.";
                return false;
            }
            query.allowSameSignalPair = true;
        } else if (lowered == "--include-boundary-signals" ||
                   lowered == "-include_boundary_signals") {
            if (equivalentPairSearch) {
                error = "--include-boundary-signals is only valid for nand_pair.";
                return false;
            }
            query.internalSignalsOnly = false;
        } else if (lowered == "--scope" || lowered == "-scope") {
            if (!nandSearch && !patternSearch) {
                error = "--scope is only valid for operand-pattern search.";
                return false;
            }
            std::string scopeToken;
            if (!(iss >> scopeToken)) {
                error = "--scope requires whole, net_fanin, net_fanout, gate_fanin, or gate_fanout.";
                return false;
            }
            const std::string loweredScope = toLower(scopeToken);
            if (loweredScope == "whole" || loweredScope == "whole_design") {
                query.scope = Netlist::FunctionSearchScope::WholeDesign;
            } else if (loweredScope == "net_fanin") {
                query.scope = Netlist::FunctionSearchScope::NetFanin;
            } else if (loweredScope == "net_fanout") {
                query.scope = Netlist::FunctionSearchScope::NetFanout;
            } else if (loweredScope == "gate_fanin") {
                query.scope = Netlist::FunctionSearchScope::GateFanin;
            } else if (loweredScope == "gate_fanout") {
                query.scope = Netlist::FunctionSearchScope::GateFanout;
            } else {
                error = "Unknown operand-pattern scope: " + scopeToken;
                return false;
            }
            if (query.scope != Netlist::FunctionSearchScope::WholeDesign &&
                !(iss >> query.scopeName)) {
                error = "The selected operand-pattern scope requires a net or gate name.";
                return false;
            }
        } else if (lowered == "--gate-type" || lowered == "-gate_type") {
            if (!equivalentPairSearch) {
                error = "--gate-type is only valid for equivalent_pairs.";
                return false;
            }
            std::string typeToken;
            if (!(iss >> typeToken)) {
                error = "--gate-type requires a combinational gate type.";
                return false;
            }
            query.gateTypeFilter = netlist.stringToGateType(typeToken);
            if (query.gateTypeFilter == GateType::UNKNOWN ||
                query.gateTypeFilter == GateType::DFF) {
                error = "--gate-type requires a supported combinational gate type.";
                return false;
            }
        } else if (lowered == "--max-results" || lowered == "-max_results") {
            std::string valueToken;
            int value = 0;
            if (!(iss >> valueToken) || !parseStrictInteger(valueToken, value) || value < 1) {
                error = "--max-results requires an integer greater than zero.";
                return false;
            }
            query.maxResults = static_cast<size_t>(value);
        } else if (lowered == "--patterns" || lowered == "-patterns") {
            std::string valueToken;
            int value = 0;
            if (!(iss >> valueToken) || !parseStrictInteger(valueToken, value) ||
                value < 1 || value > 4096) {
                error = "--patterns requires an integer in the range 1..4096.";
                return false;
            }
            query.simulationPatternCount = static_cast<size_t>(value);
        } else if (lowered == "--time-limit" || lowered == "-time_limit") {
            std::string valueToken;
            double value = 0.0;
            if (!(iss >> valueToken) || !parseStrictDouble(valueToken, value) ||
                value <= 0.0) {
                error = "--time-limit requires a positive number of seconds.";
                return false;
            }
            query.timeLimitSeconds = value;
        } else {
            error = "Unknown func_search option: " + option;
            return false;
        }
    }
    return true;
}

bool buildGraphQuery(std::istringstream& iss,
                     const std::string& mode,
                     Netlist::GraphQuery& query) {
    const std::string m = toLower(mode);
    if (m == "pi_po_cut" || m == "is_cut_net") {
        query.type = Netlist::GraphQueryType::IsCutNetBetweenPiPo;
        return static_cast<bool>(iss >> query.netName);
    }
    if (m == "articulation_between" || m == "articulation_points_between") {
        query.type = Netlist::GraphQueryType::ArticulationPointsBetween;
        return static_cast<bool>(iss >> query.sourceNetName >> query.targetNetName);
    }
    return false;
}

bool parseTargetScope(const std::string& token, TargetScope& scope) {
    const std::string value = toLower(token);
    if (value == "whole" || value == "whole_netlist") {
        scope = TargetScope::WHOLE_NETLIST;
    } else if (value == "net_fanin") {
        scope = TargetScope::NET_FANIN;
    } else if (value == "net_fanout") {
        scope = TargetScope::NET_FANOUT;
    } else if (value == "gate_fanin") {
        scope = TargetScope::GATE_FANIN;
    } else if (value == "gate_fanout") {
        scope = TargetScope::GATE_FANOUT;
    } else {
        return false;
    }
    return true;
}

bool scopeNeedsName(TargetScope scope) {
    return scope != TargetScope::WHOLE_NETLIST;
}

bool parseGateType(const Netlist& netlist, const std::string& token, GateType& type) {
    type = netlist.stringToGateType(token);
    return type != GateType::UNKNOWN && type != GateType::DFF;
}

bool requireNoTrailingEditArgs(std::istringstream& iss, std::string& error);

bool parseStrictInteger(const std::string& token, int& value) {
    std::istringstream parser(token);
    char trailing = '\0';
    return (parser >> value) && !(parser >> trailing);
}

bool parseStrictDouble(const std::string& token, double& value) {
    std::istringstream parser(token);
    char trailing = '\0';
    return (parser >> value) && std::isfinite(value) && !(parser >> trailing);
}

bool parseConstantFilter(const std::string& token, int& value) {
    const std::string lowered = toLower(token);
    if (lowered == "any" || lowered == "all" || lowered == "both") {
        value = -1;
        return true;
    }
    return parseStrictInteger(token, value) && (value == 0 || value == 1);
}

bool parseConstantSimplificationEdit(
    const Netlist& netlist,
    std::istringstream& iss,
    Netlist::EditApplyRequest& request,
    std::string& error)
{
    request.kind = Netlist::EditCommandKind::SimplifyConstants;

    std::string token;
    if (!(iss >> token)) return true;

    const std::string loweredType = toLower(token);
    if (loweredType == "all" || loweredType == "any") {
        request.gateType = GateType::UNKNOWN;
    } else if (!parseGateType(netlist, token, request.gateType)) {
        error = "simplify_constants requires a valid combinational <gate_type> or all.";
        return false;
    }

    if (!(iss >> token)) return true;
    if (token != "--inputs" && token != "-inputs") {
        if (!parseConstantFilter(token, request.constValue)) {
            error = "Constant filter must be 0, 1, or any.";
            return false;
        }
        if (!(iss >> token)) return true;
    }

    if (token != "--inputs" && token != "-inputs") {
        error = "Unknown simplify_constants option: " + token;
        return false;
    }
    if (!(iss >> token) || !parseStrictInteger(token, request.inputCount) || request.inputCount < 1) {
        error = "--inputs requires a positive integer.";
        return false;
    }
    return requireNoTrailingEditArgs(iss, error);
}

bool parseTechnologyEditApply(const Netlist& netlist,
                              std::istringstream& iss,
                              const std::string& mode,
                              Netlist::EditApplyRequest& request,
                              std::string& error) {
    const std::string loweredMode = toLower(mode);
    if (loweredMode == "convert_basis") {
        request.kind = Netlist::EditCommandKind::ConvertToBasis;
    } else if (loweredMode == "replace_type") {
        request.kind = Netlist::EditCommandKind::ReplaceGateType;
    } else {
        error = "Unknown edit_apply mode: " + mode;
        return false;
    }

    std::string scopeToken;
    if (!(iss >> scopeToken) || !parseTargetScope(scopeToken, request.scope)) {
        error = "Missing or invalid scope. Use whole, net_fanin, net_fanout, gate_fanin, or gate_fanout.";
        return false;
    }
    if (scopeNeedsName(request.scope) && !(iss >> request.scopeName)) {
        error = "Missing scope target name.";
        return false;
    }

    if (request.kind == Netlist::EditCommandKind::ReplaceGateType) {
        std::string targetTypeToken;
        if (!(iss >> targetTypeToken) || !parseGateType(netlist, targetTypeToken, request.targetGateType)) {
            error = "Missing or invalid target gate type.";
            return false;
        }
    }

    int listMode = 0; // 0=none, 1=allow, 2=ban
    std::string token;
    while (iss >> token) {
        const std::string lowered = toLower(token);
        if (lowered == "-allow" || lowered == "--allow") {
            listMode = 1;
            continue;
        }
        if (lowered == "-ban" || lowered == "--ban") {
            listMode = 2;
            continue;
        }
        if (lowered == "--verbose") {
            request.verbose = true;
            continue;
        }
        if (lowered == "--validate_equivalence") {
            request.validateEquivalence = true;
            continue;
        }

        GateType type = GateType::UNKNOWN;
        if (!parseGateType(netlist, token, type)) {
            error = "Invalid gate type: " + token;
            return false;
        }
        if (listMode == 1) {
            request.allowedTypes.push_back(type);
        } else if (listMode == 2) {
            request.bannedTypes.push_back(type);
        } else {
            error = "Gate type must appear after -allow or -ban: " + token;
            return false;
        }
    }

    if (request.kind == Netlist::EditCommandKind::ConvertToBasis &&
        request.allowedTypes.empty() && request.bannedTypes.empty()) {
        error = "convert_basis requires -allow and/or -ban.";
        return false;
    }
    if (request.kind == Netlist::EditCommandKind::ReplaceGateType &&
        request.allowedTypes.empty()) {
        error = "replace_type requires -allow.";
        return false;
    }
    return true;
}

bool isCliOptionToken(const std::string& token) {
    return !token.empty() && token.front() == '-';
}

bool appendOptGateTypes(const Netlist& netlist,
                        const std::string& token,
                        std::vector<GateType>& types,
                        std::string& error) {
    const std::vector<std::string> names = split(token, ',');
    for (const std::string& name : names) {
        GateType type = GateType::UNKNOWN;
        if (name.empty() || !parseGateType(netlist, name, type)) {
            error = "Invalid combinational gate type: " + token;
            return false;
        }
        if (std::find(types.begin(), types.end(), type) == types.end()) {
            types.push_back(type);
        }
    }
    return true;
}

bool parsePublicOptApply(const Netlist& netlist,
                         std::istringstream& iss,
                         const std::string& mode,
                         Netlist::OptApplyRequest& request,
                         std::string& error) {
    if (toLower(mode) != "critical_path_depth") {
        error = "Unknown or non-public opt_apply mode: " + mode;
        return false;
    }
    request.passKind = OptPassKind::CriticalPathDepth;

    std::vector<std::string> args;
    std::string token;
    while (iss >> token) {
        args.push_back(token);
    }

    for (size_t i = 0; i < args.size();) {
        const std::string option = toLower(args[i]);
        if (option == "--scope" || option == "-scope") {
            if (++i >= args.size() || !parseTargetScope(args[i], request.scope)) {
                error = "--scope requires whole, net_fanin, net_fanout, gate_fanin, or gate_fanout.";
                return false;
            }
            ++i;
            if (scopeNeedsName(request.scope) &&
                i < args.size() &&
                !isCliOptionToken(args[i])) {
                request.scopeName = args[i++];
            }
        } else if (option == "--name" || option == "-name") {
            if (++i >= args.size() || isCliOptionToken(args[i])) {
                error = "--name requires a scope target name.";
                return false;
            }
            request.scopeName = args[i++];
        } else if (option == "--basis-scope" || option == "--basis_scope" ||
                   option == "-basis_scope") {
            if (++i >= args.size() || !parseTargetScope(args[i], request.basisScope)) {
                error = "--basis-scope requires whole, net_fanin, net_fanout, gate_fanin, or gate_fanout.";
                return false;
            }
            ++i;
            if (scopeNeedsName(request.basisScope) &&
                i < args.size() && !isCliOptionToken(args[i])) {
                request.basisScopeName = args[i++];
            }
        } else if (option == "--basis-name" || option == "--basis_name" ||
                   option == "-basis_name") {
            if (++i >= args.size() || isCliOptionToken(args[i])) {
                error = "--basis-name requires a scope target name.";
                return false;
            }
            request.basisScopeName = args[i++];
        } else if (option == "--objective" || option == "-objective") {
            if (++i >= args.size()) {
                error = "--objective requires global or cone.";
                return false;
            }
            const std::string objective = toLower(args[i++]);
            if (objective == "global" || objective == "global_maximum") {
                request.depthObjective = OptDepthObjective::GlobalMaximum;
            } else if (objective == "cone" ||
                       objective == "scoped_fanin" ||
                       objective == "scoped_fanin_cone") {
                request.depthObjective = OptDepthObjective::ScopedFaninCone;
            } else {
                error = "--objective requires global or cone.";
                return false;
            }
        } else if (option == "--allowed" || option == "--allow" ||
                   option == "-allowed" || option == "-allow" ||
                   option == "--banned" || option == "--ban" ||
                   option == "-banned" || option == "-ban") {
            const bool allowed =
                option == "--allowed" || option == "--allow" ||
                option == "-allowed" || option == "-allow";
            std::vector<GateType>& destination =
                allowed ? request.allowedTypes : request.bannedTypes;
            const size_t firstType = ++i;
            while (i < args.size() && !isCliOptionToken(args[i])) {
                if (!appendOptGateTypes(
                        netlist, args[i], destination, error)) {
                    return false;
                }
                ++i;
            }
            if (i == firstType) {
                error = allowed
                    ? "--allowed requires at least one gate type."
                    : "--banned requires at least one gate type.";
                return false;
            }
        } else if (option == "--target-depth" ||
                   option == "--target_depth" ||
                   option == "-target_depth") {
            if (++i >= args.size() ||
                !parseStrictInteger(args[i], request.targetDepth) ||
                request.targetDepth < 0) {
                error = "--target-depth requires a non-negative integer.";
                return false;
            }
            ++i;
        } else if (option == "--time-limit" ||
                   option == "--time_limit" ||
                   option == "-time_limit") {
            if (++i >= args.size() ||
                !parseStrictDouble(args[i], request.timeLimitSeconds) ||
                request.timeLimitSeconds <= 0.0) {
                error = "--time-limit requires a positive number of seconds.";
                return false;
            }
            ++i;
        } else if (option == "--allow-no-improvement" ||
                   option == "--allow_no_improvement") {
            request.requireDepthImprovement = false;
            ++i;
        } else if (option == "--verbose" || option == "-verbose") {
            request.verbose = true;
            ++i;
        } else {
            error = "Unknown opt_apply option: " + args[i];
            return false;
        }
    }

    if (scopeNeedsName(request.scope) && request.scopeName.empty()) {
        error = "The selected scope requires --name <net_or_gate>, or the name immediately after --scope.";
        return false;
    }
    if (!scopeNeedsName(request.scope) && !request.scopeName.empty()) {
        error = "Whole-netlist scope does not accept --name.";
        return false;
    }
    if (request.depthObjective == OptDepthObjective::ScopedFaninCone &&
        request.scope != TargetScope::NET_FANIN &&
        request.scope != TargetScope::GATE_FANIN) {
        error = "Cone depth objective requires net_fanin or gate_fanin scope.";
        return false;
    }
    if (scopeNeedsName(request.basisScope) && request.basisScopeName.empty()) {
        error = "--basis-scope requires --basis-name <net_or_gate>, or the name "
                "immediately after --basis-scope.";
        return false;
    }
    if (!scopeNeedsName(request.basisScope) && !request.basisScopeName.empty()) {
        error = "Whole-netlist basis scope does not accept --basis-name.";
        return false;
    }
    if (request.basisScope != TargetScope::WHOLE_NETLIST &&
        request.allowedTypes.empty() && request.bannedTypes.empty()) {
        error = "--basis-scope only makes sense together with --allowed and/or --banned.";
        return false;
    }
    for (GateType type : request.allowedTypes) {
        if (std::find(
                request.bannedTypes.begin(),
                request.bannedTypes.end(),
                type) != request.bannedTypes.end()) {
            error = "A gate type cannot appear in both --allowed and --banned.";
            return false;
        }
    }
    return true;
}

bool requireNoTrailingEditArgs(std::istringstream& iss, std::string& error) {
    std::string extra;
    if (iss >> extra) {
        error = "Unexpected edit_apply argument: " + extra;
        return false;
    }
    return true;
}

bool parsePublicEditApply(const Netlist& netlist,
                          std::istringstream& iss,
                          const std::string& mode,
                          Netlist::EditApplyRequest& request,
                          std::string& error) {
    const std::string m = toLower(mode);

    if (m == "convert_basis" || m == "replace_type") {
        return parseTechnologyEditApply(netlist, iss, mode, request, error);
    }

    if (m == "merge_functionally_equivalent_gates") {
        request.kind =
            Netlist::EditCommandKind::MergeFunctionallyEquivalentGates;
        std::string scopeToken;
        if (!(iss >> scopeToken) || !parseTargetScope(scopeToken, request.scope)) {
            error = "merge_functionally_equivalent_gates requires a valid <scope>.";
            return false;
        }
        if (scopeNeedsName(request.scope) && !(iss >> request.scopeName)) {
            error = "The selected functional merge scope requires a net or gate name.";
            return false;
        }

        std::string option;
        while (iss >> option) {
            const std::string lowered = toLower(option);
            if (lowered == "--gate-type" || lowered == "-gate_type") {
                std::string typeToken;
                if (!(iss >> typeToken) ||
                    !parseGateType(netlist, typeToken, request.gateType)) {
                    error = "--gate-type requires a valid combinational gate type.";
                    return false;
                }
            } else if (lowered == "--patterns" || lowered == "-patterns") {
                std::string valueToken;
                int value = 0;
                if (!(iss >> valueToken) ||
                    !parseStrictInteger(valueToken, value) ||
                    value < 1 || value > 4096) {
                    error = "--patterns requires an integer in the range 1..4096.";
                    return false;
                }
                request.simulationPatternCount = static_cast<size_t>(value);
            } else if (lowered == "--time-limit" || lowered == "-time_limit") {
                std::string valueToken;
                double value = 0.0;
                if (!(iss >> valueToken) ||
                    !parseStrictDouble(valueToken, value) || value <= 0.0) {
                    error = "--time-limit requires a positive number of seconds.";
                    return false;
                }
                request.timeLimitSeconds = value;
            } else {
                error = "Unknown functional merge option: " + option;
                return false;
            }
        }
        return true;
    }

    if (m == "rename_gate" || m == "rename_net") {
        request.kind = m == "rename_gate"
            ? Netlist::EditCommandKind::RenameGate
            : Netlist::EditCommandKind::RenameNet;
        if (!(iss >> request.oldName >> request.newName)) {
            error = m + " requires <old_name> <new_name>.";
            return false;
        }
        return requireNoTrailingEditArgs(iss, error);
    }

    if (m == "cleanup_buffers") {
        request.kind = Netlist::EditCommandKind::CleanupBuffers;
    } else if (m == "collapse_double_inverter") {
        request.kind = Netlist::EditCommandKind::CollapseDoubleInverter;
    } else if (m == "local_simplification_fixpoint") {
        request.kind = Netlist::EditCommandKind::LocalSimplificationFixpoint;
    } else if (m == "safe_cleanup_fixpoint") {
        request.kind = Netlist::EditCommandKind::SafeCleanupFixpoint;
    } else if (m == "remove_dead_logic" ||
               m == "trim_dead_logic" ||
               m == "remove_dangling_logic") {
        request.kind = Netlist::EditCommandKind::RemoveDeadLogic;
        std::string option;
        while (iss >> option) {
            const std::string lowered = toLower(option);
            if (lowered == "--include-sequential" ||
                lowered == "-include_sequential") {
                request.includeSequential = true;
            } else {
                error = "Unknown remove_dead_logic option: " + option;
                return false;
            }
        }
        return true;
    } else if (m == "remove_unused_nets") {
        request.kind = Netlist::EditCommandKind::RemoveUnusedNets;
    } else if (m == "merge_equivalent_gates") {
        error = "merge_equivalent_gates is not a public command: the legacy implementation "
                "only proves structural identity. Use merge_structurally_equivalent_gates "
                "for structural duplicates or merge_functionally_equivalent_gates for "
                "SAT-proven functional merge.";
        return false;
    } else if (m == "merge_structurally_equivalent_gates") {
        request.kind = Netlist::EditCommandKind::MergeStructurallyEquivalentGates;
    } else if (m == "simplify_constants") {
        return parseConstantSimplificationEdit(netlist, iss, request, error);
    } else if (m == "simplify_same_input") {
        request.kind = Netlist::EditCommandKind::SimplifySameInput;
    } else if (m == "remove_net_if_unused") {
        request.kind = Netlist::EditCommandKind::RemoveNetIfUnused;
        if (!(iss >> request.netName)) {
            error = "remove_net_if_unused requires <net_name>.";
            return false;
        }
    } else if (m == "remove_redundant_logic") {
        request.kind = Netlist::EditCommandKind::RemoveRedundantLogic;
        std::string option;
        while (iss >> option) {
            const std::string lowered = toLower(option);
            if (lowered == "--time-limit" || lowered == "-time_limit") {
                std::string valueToken;
                double value = 0.0;
                if (!(iss >> valueToken) || !parseStrictDouble(valueToken, value) ||
                    value <= 0.0) {
                    error = "--time-limit requires a positive number of seconds.";
                    return false;
                }
                request.timeLimitSeconds = value;
            } else {
                error = "Unknown remove_redundant_logic option: " + option;
                return false;
            }
        }
        return true;
    } else if (m == "insert_buffers_for_fanout") {
        request.kind = Netlist::EditCommandKind::InsertBuffersForFanout;
        if (!(iss >> request.maxFanout)) {
            error = "insert_buffers_for_fanout requires <max_fanout>.";
            return false;
        }
    } else if (m == "insert_buffers_for_net" ||
               m == "insert_buffers_for_specific_net") {
        request.kind = Netlist::EditCommandKind::InsertBuffersForSpecificNet;
        if (!(iss >> request.netName >> request.maxFanout)) {
            error = m + " requires <net_name> <max_fanout>.";
            return false;
        }
    } else if (m == "insert_buffers_for_dff_control") {
        request.kind = Netlist::EditCommandKind::InsertBuffersForDffControl;
        if (!(iss >> request.maxFanout)) {
            error = "insert_buffers_for_dff_control requires <max_fanout> and -clock and/or -reset.";
            return false;
        }
        std::string option;
        while (iss >> option) {
            const std::string lowered = toLower(option);
            if (lowered == "-clock" || lowered == "--clock") {
                request.processClock = true;
            } else if (lowered == "-reset" || lowered == "--reset") {
                request.processReset = true;
            } else {
                error = "Unknown DFF control buffer option: " + option;
                return false;
            }
        }
        if (!request.processClock && !request.processReset) {
            error = "insert_buffers_for_dff_control requires -clock and/or -reset.";
            return false;
        }
        return true;
    } else if (m == "insert_buffers_on_each_load") {
        request.kind = Netlist::EditCommandKind::InsertBuffersOnEachLoad;
        if (!(iss >> request.netName)) {
            error = "insert_buffers_on_each_load requires <net_name>.";
            return false;
        }
    } else if (m == "insert_buffer_at_driver") {
        request.kind = Netlist::EditCommandKind::InsertBufferAtDriver;
        if (!(iss >> request.netName)) {
            error = "insert_buffer_at_driver requires <net_name>.";
            return false;
        }
    } else if (m == "insert_buffer_before_gate") {
        request.kind = Netlist::EditCommandKind::InsertBufferBeforeGate;
        if (!(iss >> request.netName >> request.targetGateName)) {
            error = "insert_buffer_before_gate requires <net_name> <target_gate_name>.";
            return false;
        }
    } else if (m == "insert_buffers_by_gate_type") {
        request.kind = Netlist::EditCommandKind::InsertBuffersByGateType;
        std::string gateTypeToken;
        if (!(iss >> gateTypeToken) || !parseGateType(netlist, gateTypeToken, request.gateType)) {
            error = "insert_buffers_by_gate_type requires a valid combinational <gate_type>.";
            return false;
        }

        bool directionSpecified = false;
        request.bufferInputs = false;
        request.bufferOutputs = false;
        std::string option;
        while (iss >> option) {
            const std::string lowered = toLower(option);
            if (lowered == "-inputs" || lowered == "--inputs" || lowered == "inputs") {
                request.bufferInputs = true;
                directionSpecified = true;
            } else if (lowered == "-outputs" || lowered == "--outputs" || lowered == "outputs") {
                request.bufferOutputs = true;
                directionSpecified = true;
            } else if (lowered == "-both" || lowered == "--both" || lowered == "both") {
                request.bufferInputs = true;
                request.bufferOutputs = true;
                directionSpecified = true;
            } else {
                error = "Unknown gate-type buffer option: " + option;
                return false;
            }
        }
        if (!directionSpecified) {
            request.bufferInputs = true;
            request.bufferOutputs = true;
        }
        return true;
    } else {
        error = "Unknown or non-public edit_apply mode: " + mode;
        return false;
    }

    return requireNoTrailingEditArgs(iss, error);
}

// 將 path_query 的 mode 轉成 PathQueryMode。
bool parsePathMode(const std::string& mode, Netlist::PathQueryMode& outMode) {
    const std::string m = toLower(mode);
    if (m == "exists") {
        outMode = Netlist::PathQueryMode::Exists;
    } else if (m == "find_any") {
        outMode = Netlist::PathQueryMode::FindAny;
    } else if (m == "enumerate") {
        outMode = Netlist::PathQueryMode::EnumerateAll;
    } else if (m == "min_depth") {
        outMode = Netlist::PathQueryMode::MinDepth;
    } else if (m == "max_depth") {
        outMode = Netlist::PathQueryMode::MaxDepth;
    } else if (m == "every_through") {
        outMode = Netlist::PathQueryMode::EveryPathThrough;
    } else if (m == "every_avoids") {
        outMode = Netlist::PathQueryMode::EveryPathAvoids;
    } else if (m == "mandatory_nodes" || m == "articulation_between") {
        outMode = Netlist::PathQueryMode::FindMandatoryNodes;
    } else if (m == "is_separator" || m == "pi_po_cut") {
        outMode = Netlist::PathQueryMode::IsSeparator;
    } else if (m == "direct_pi_po" || m == "direct_connections") {
        outMode = Netlist::PathQueryMode::DirectPiPoConnections;
    } else {
        return false;
    }
    return true;
}

// 將 reg_path_query 的 mode 轉成 RegisterPathQueryMode。
bool parseRegisterPathMode(const std::string& mode,
                           Netlist::RegisterPathQueryMode& outMode) {
    const std::string m = toLower(mode);
    if (m == "exists") {
        outMode = Netlist::RegisterPathQueryMode::Exists;
    } else if (m == "find_any") {
        outMode = Netlist::RegisterPathQueryMode::FindAny;
    } else if (m == "enumerate") {
        outMode = Netlist::RegisterPathQueryMode::EnumerateAll;
    } else if (m == "min_depth") {
        outMode = Netlist::RegisterPathQueryMode::MinDepth;
    } else if (m == "max_depth") {
        outMode = Netlist::RegisterPathQueryMode::MaxDepth;
    } else {
        return false;
    }
    return true;
}

// 印出統一 CLI 的 help；只保留分類式高階入口，避免 LLM 選到低階散裝 API。
void printHelp() {
    std::cout
        << "Unified EDA query CLI\n"
        << "\nI/O\n"
        << "  read <verilog_file>\n"
        << "  write <verilog_file>\n"
        << "  quit\n"
        << "\nStructural query\n"
        << "  structure_query <mode> [args]\n"
        << "  mode: summary | list_gates | list_nets | net_classes | list_pi | list_po\n"
        << "        list_dffs | list_comb | gate_info <gate> | net_info <net>\n"
        << "        port_info <port> | count_by_type [type] [gate-type filters]\n"
        << "        gates_by_type [type] [gate-type filters] [--with-pins]\n"
        << "        const_input_gates [type|all] [0|1|any] [gate-type filters] [--with-pins]\n"
        << "        gate-type filters: [--gate-types <type...>] [--exclude-gate-types <type...>]\n"
        << "        structural_issues\n"
        << "        net_driver <net> [--with-pins]\n"
        << "        net_loads <net> [--with-pins] | gate_inputs <gate>\n"
        << "        fanout_load <net> | fanout_report <net>\n"
        << "        global_fanout [limit] | pi_fanout [limit] | fanout_violations <limit>\n"
        << "        fanout_filter <all|pi> <eq|ne|gt|ge|lt|le|between> <value> [upper]\n"
        << "        fanout_rank <scope> <highest|lowest|nth_highest|nth_lowest|top|bottom> [k]\n"
        << "        gate_output <gate> | gate_fanin <gate> | gate_fanout <gate>\n"
        << "        is_connected <gate> <net>\n"
        << "\nCone query\n"
        << "  cone_query <mode> [name] [options]\n"
        << "  mode: net_fanin | net_fanout | gate_fanin | gate_fanout | largest_output\n"
        << "        output_rank <gates|filtered_gates|nets>\n"
        << "                    <highest|lowest|nth_highest|nth_lowest|top|bottom> [k]\n"
        << "        output_filter <gates|filtered_gates|nets>\n"
        << "                      <eq|ne|gt|ge|lt|le|between> <value> [upper]\n"
        << "        shared_fanin <net_a> <net_b>\n"
        << "  options: [with_paths|--with-paths] [--gate-types <type...>] [--with-pins]\n"
        << "  gate types: AND OR NOT NAND NOR XOR XNOR BUF DFF\n"
        << "\nPath query\n"
        << "  path_query <mode> <start_endpoint> <end_endpoint> [-req node...] [-avoid node...]\n"
        << "  path_query enumerate <start_endpoint> <end_endpoint> [-count_only]\n"
        << "  path_query direct_pi_po\n"
        << "  enumerate automatically writes the complete list to a unique file and returns a summary\n"
        << "  output files, display size, and execution budget are managed automatically\n"
        << "  mode: exists | find_any | enumerate | min_depth | max_depth\n"
        << "        every_through | every_avoids | mandatory_nodes | articulation_between\n"
        << "        is_separator | pi_po_cut <internal_net> | direct_pi_po\n"
        << "  endpoint: net:<n> | pi:<p> | po:<p> | all_pi | all_po | dff_q:<ff> | dff_d:<ff>\n"
        << "            all_dff_q | all_dff_d\n"
        << "            dff_clk:<ff>[:pin] | dff_reset:<ff>[:pin]\n"
        << "            gate_out:<g> | gate_in:<g>:<index_or_pin> | bare_net\n"
        << "  node: gate:<g> | net:<n> | bare_net\n"
        << "\nDepth query\n"
        << "  depth_query <mode> [args]\n"
        << "  mode: net <net> | all_po | all_dff_d | global_critical | exceeding <depth>\n"
        << "        po_exceeding <depth>\n"
        << "        gate_on_critical <gate> | deepest_output\n"
        << "\nFunction query\n"
        << "  func_query <mode> [args]\n"
        << "  mode: equivalence <net_a> <net_b> | can_be_value <net> <0|1>\n"
        << "        conditional_equivalence <net_a> <net_b> <condition_net> <0|1>\n"
        << "        constant <net> <0|1> | always_zero <net> | always_one <net>\n"
        << "        truth_status <net> | depends_on <target_net> <input_net>\n"
        << "        symmetry <target_net_or_bus> <input_a> <input_b>\n"
        << "        boolean_expression <net>\n"
        << "        simplified_expression <net> <max_depth> | support_pi <net>\n"
        << "\nFunction search\n"
        << "  func_search pattern <BUF|NOT|AND|NAND|OR|NOR|XOR|XNOR> <target_net>\n"
        << "              [--all] [--scope <scope> [scope_name]]\n"
        << "              [--max-results n] [--patterns 1..4096]\n"
        << "              [--time-limit seconds] [--allow-same]\n"
        << "              [--include-boundary-signals]\n"
        << "  func_search nand_pair <target_net> [--all] [--max-results n]\n"
        << "              [--patterns 1..4096] [--time-limit seconds]\n"
        << "              [--allow-same] [--include-boundary-signals]\n"
        << "  func_search equivalent_pairs <scope> [scope_name] [--all]\n"
        << "              [--gate-type type] [--max-results n]\n"
        << "              [--patterns 1..4096] [--time-limit seconds]\n"
        << "  scope: whole | net_fanin <net> | net_fanout <net>\n"
        << "         gate_fanin <gate> | gate_fanout <gate>\n"
        << "  --all performs a complete search, writes all matches to a unique file,\n"
        << "  and returns only a summary; use --max-results only when requested\n"
        << "  default mode finds one SAT-proven pair; --all requests complete enumeration\n"
        << "\nSequential pattern query\n"
        << "  sequential_query enable_hold <all|dff_name> [--summary-only]\n"
        << "                   [--confirmed-only] [--include-no-pattern]\n"
        << "                   [--offset n] [--limit n] [--verify-sat]\n"
        << "                   [--functional-fallback]\n"
        << "                   [--max-functional-candidates n]\n"
        << "                   [--max-functional-matches n] [--functional-find-any]\n"
        << "                   [--resolve-functional-data|--no-resolve-functional-data]\n"
        << "                   [--max-functional-data-candidates n]\n"
        << "                   [--no-functional-simulation-filter]\n"
        << "                   [--functional-simulation-patterns 1..4096]\n"
        << "                   [--functional-per-dff-time-limit seconds]\n"
        << "                   [--functional-time-limit seconds]\n"
        << "  all-DFF detail writes all records to a unique file and returns a summary\n"
        << "  use offset/limit only when a prompt explicitly requests a record window\n"
        << "  functional classification is always enabled for canonical and restructured logic\n"
        << "  --verify-sat/--functional-fallback are retained for compatibility;\n"
        << "  explicit functional tuning options still require --functional-fallback\n"
        << "\nDepth optimization\n"
        << "  opt_query critical_path_depth\n"
        << "  opt_apply critical_path_depth [--scope <scope> [scope_name]]\n"
        << "            [--name <scope_name>] [--objective global|cone]\n"
        << "            [--basis-scope <scope> [scope_name]] [--basis-name <name>]\n"
        << "            [--allowed <type...>] [--banned <type...>]\n"
        << "            [--target-depth N] [--time-limit seconds]\n"
        << "            [--allow-no-improvement] [--verbose]\n"
        << "  --scope/--objective select the cost function; --basis-scope selects where\n"
        << "  the gate-type constraint applies (default: the whole netlist)\n"
        << "  gate-type lists accept spaces or commas, for example NOR NOT or nor,not\n"
        << "  accepted rewrites use CertifiedRewrite; use equiv_query only for explicit SAT proof\n"
        << "\nEdit apply\n"
        << "  edit_apply rename_gate <old> <new> | rename_net <old> <new>\n"
        << "  edit_apply cleanup_buffers | collapse_double_inverter | local_simplification_fixpoint\n"
        << "  edit_apply remove_dead_logic [--include-sequential]\n"
        << "             (aliases: trim_dead_logic, remove_dangling_logic)\n"
        << "  edit_apply safe_cleanup_fixpoint | trim_dead_logic | remove_dangling_logic\n"
        << "  edit_apply remove_unused_nets | remove_net_if_unused <net>\n"
        << "  edit_apply remove_redundant_logic [--time-limit seconds]\n"
        << "  edit_apply merge_structurally_equivalent_gates\n"
        << "  edit_apply merge_functionally_equivalent_gates <scope> [scope_name]\n"
        << "             [--gate-type type] [--patterns 1..4096] [--time-limit seconds]\n"
        << "  edit_apply simplify_constants [gate_type|all] [0|1|any] [--inputs N]\n"
        << "  edit_apply simplify_same_input\n"
        << "  edit_apply insert_buffers_for_fanout <max_fanout>\n"
        << "  edit_apply insert_buffers_for_net <net> <max_fanout>\n"
        << "  edit_apply insert_buffers_for_dff_control <max_fanout> -clock|-reset\n"
        << "  edit_apply insert_buffers_on_each_load <net> | insert_buffer_at_driver <net>\n"
        << "  edit_apply insert_buffer_before_gate <net> <gate>\n"
        << "  edit_apply insert_buffers_by_gate_type <type> [-inputs] [-outputs] [-both]\n"
        << "  edit_apply convert_basis <scope> [scope_name] -allow <type...> [-ban <type...>] [--validate_equivalence]\n"
        << "  edit_apply replace_type <scope> [scope_name] <target_type> -allow <type...> [--validate_equivalence]\n"
        << "  scope: whole | net_fanin <net> | net_fanout <net> | gate_fanin <gate> | gate_fanout <gate>\n"
        << "  examples:\n"
        << "    edit_apply convert_basis whole -allow AND NOT\n"
        << "    edit_apply convert_basis net_fanin n10 -allow NOR NOT\n"
        << "    edit_apply replace_type whole XOR -allow NAND\n"
        << "\nCached report query\n"
        << "  report_query last_edit\n"
        << "\nWhole-design equivalence\n"
        << "  equiv_query original [time_budget_seconds]\n"
        << "  equiv_query previous_edit [time_budget_seconds]\n";
}

bool dispatchCommand(ToolSession& session, const std::string& inputLine) {
    std::istringstream iss(inputLine);
    std::string command;
    iss >> command;
    command = toLower(command);
    if (command.empty()) {
        return true;
    }

    if (command == "quit" || command == "exit") {
        ToolResponse response;
        response.ok = true;
        response.status = ToolStatus::Ok;
        response.command = command;
        response.message = "Session closed.";
        response.complete = true;
        emitToolResponse(session, response);
        return false;
    }

    if (command == "help") {
        ToolResponse response;
        response.ok = true;
        response.status = ToolStatus::Ok;
        response.command = command;
        response.message = "Unified command help.";
        response.complete = true;
        emitToolResponse(session, response, []() { printHelp(); });
        return true;
    }

    if (command == "read") {
        const std::string filepath = readRestPath(iss);
        if (filepath.empty()) {
            emitToolError(session, command, "", "Usage: read <verilog_file>");
            return true;
        }

        Netlist loaded;
        bool loadedOk = false;
        std::string errorMessage;
        try {
            loadedOk = session.reader.read(filepath, loaded);
        } catch (const std::exception& error) {
            errorMessage = error.what();
        }

        if (!loadedOk) {
            const std::string message = errorMessage.empty()
                ? "Failed to read design: " + filepath
                : "Failed to read design: " + filepath + " (" + errorMessage + ")";
            emitToolError(session, command, "", message);
            return true;
        }

        session.current = std::move(loaded);
        session.original = session.current.cloneForRollback();
        session.lastEditBaseline.reset();
        session.lastEditReport.reset();
        session.loadedFilePath = filepath;
        session.designRevision = 0;
        session.designLoaded = true;

        const NetlistStats stats = session.current.collectNetlistStats();
        ToolResponse response;
        response.ok = true;
        response.status = ToolStatus::Ok;
        response.command = command;
        response.message = "Design loaded and original snapshot created.";
        response.complete = true;
        emitToolResponse(session, response, [&]() {
            std::cout << "  loaded_file: " << filepath << "\n";
            std::cout << "  gate_count: " << stats.activeGateCount << "\n";
            std::cout << "  net_count: " << stats.activeNetCount << "\n";
            std::cout << "  primary_input_count: " << stats.primaryInputCount << "\n";
            std::cout << "  primary_output_count: " << stats.primaryOutputCount << "\n";
            std::cout << "  original_snapshot_available: true\n";
        });
        return true;
    }

    if (command == "write") {
        if (!session.designLoaded) {
            emitToolError(session, command, "", "NO_DESIGN_LOADED: load a design before write.");
            return true;
        }
        const std::string filepath = readRestPath(iss);
        if (filepath.empty()) {
            emitToolError(session, command, "", "Usage: write <verilog_file>");
            return true;
        }
        const bool wrote = session.writer.write(filepath, session.current);
        ToolResponse response;
        response.ok = wrote;
        response.status = wrote ? ToolStatus::Ok : ToolStatus::Error;
        response.command = command;
        response.message = wrote ? "Current design written." : "Failed to write design: " + filepath;
        response.complete = wrote;
        emitToolResponse(session, response, [&]() {
            std::cout << "  output_file: " << filepath << "\n";
            std::cout << "  written: " << (wrote ? "true" : "false") << "\n";
        });
        return true;
    }

    auto requireDesign = [&]() {
        if (session.designLoaded) {
            return true;
        }
        emitToolError(session, command, "", "NO_DESIGN_LOADED: use read before running this command.");
        return false;
    };

    if (command == "structure_query") {
        if (!requireDesign()) return true;
        std::string mode;
        if (!(iss >> mode)) {
            emitToolError(session, command, "", "Usage: structure_query <mode> [args]");
            return true;
        }

        std::string remainingArgs;
        std::getline(iss, remainingArgs);
        Netlist::BasicQuery basicQuery;
        std::string basicError;
        std::istringstream basicArgs(remainingArgs);
        if (buildBasicQuery(session.current, basicArgs, mode, basicQuery, basicError)) {
            const Netlist::BasicReport report = session.current.runBasicQuery(basicQuery);
            const ListArtifactResult artifact = report.ok
                ? writeAutomaticListArtifact(
                    session, command, toLower(mode),
                    makeBasicListArtifactContent(session.current, report))
                : ListArtifactResult{};
            ToolResponse response;
            response.ok = report.ok;
            response.status = report.ok ? ToolStatus::Ok : ToolStatus::Error;
            response.command = command;
            response.mode = toLower(mode);
            response.message = report.message;
            response.complete = report.ok;
            emitToolResponse(session, response, [&]() {
                printBasicReport(session.current, report, artifact.complete);
                printListArtifactMetadata(artifact);
            });
            return true;
        }
        if (!basicError.empty()) {
            emitToolError(session, command, toLower(mode), basicError);
            return true;
        }

        Netlist::DirectConnectivityQuery connectivityQuery;
        std::istringstream connectivityArgs(remainingArgs);
        std::string connectivityError;
        if (buildConnectivityQuery(
                connectivityArgs, mode, connectivityQuery, connectivityError)) {
            const Netlist::DirectConnectivityReport report =
                session.current.runDirectConnectivityQuery(connectivityQuery);
            const ListArtifactResult artifact = report.ok
                ? writeAutomaticListArtifact(
                    session, command, toLower(mode),
                    makeConnectivityListArtifactContent(session.current, report))
                : ListArtifactResult{};
            ToolResponse response;
            response.ok = report.ok;
            response.status = report.ok ? ToolStatus::Ok : ToolStatus::Error;
            response.command = command;
            response.mode = toLower(mode);
            response.message = report.message;
            response.complete = report.ok;
            emitToolResponse(session, response, [&]() {
                printConnectivityReport(
                    session.current, connectivityQuery, report, artifact.complete);
                printListArtifactMetadata(artifact);
            });
            return true;
        }

        if (!connectivityError.empty()) {
            emitToolError(session, command, mode, connectivityError);
            return true;
        }

        emitToolError(session, command, mode,
                      "Unknown structure_query mode: " + mode);
        return true;
    }

    if (command == "basic_query") {
        if (!requireDesign()) return true;
        std::string mode;
        if (!(iss >> mode)) {
            emitToolError(session, command, "", "Usage: basic_query <mode> [args]");
            return true;
        }
        Netlist::BasicQuery query;
        std::string basicError;
        if (!buildBasicQuery(session.current, iss, mode, query, basicError)) {
            emitToolError(session, command, mode, basicError.empty()
                ? "Unknown basic_query mode: " + mode
                : basicError);
            return true;
        }
        const Netlist::BasicReport report = session.current.runBasicQuery(query);
        const ListArtifactResult artifact = report.ok
            ? writeAutomaticListArtifact(
                session, command, toLower(mode),
                makeBasicListArtifactContent(session.current, report))
            : ListArtifactResult{};
        ToolResponse response;
        response.ok = report.ok;
        response.status = report.ok ? ToolStatus::Ok : ToolStatus::Error;
        response.command = command;
        response.mode = toLower(mode);
        response.message = report.message;
        response.complete = report.ok;
        emitToolResponse(session, response, [&]() {
            printBasicReport(session.current, report, artifact.complete);
            printListArtifactMetadata(artifact);
        });
        return true;
    }

    if (command == "conn_query") {
        if (!requireDesign()) return true;
        std::string mode;
        if (!(iss >> mode)) {
            emitToolError(session, command, "", "Usage: conn_query <mode> [args]");
            return true;
        }
        Netlist::DirectConnectivityQuery query;
        std::string connectivityError;
        if (!buildConnectivityQuery(iss, mode, query, connectivityError)) {
            emitToolError(session, command, mode, connectivityError.empty()
                ? "Unknown conn_query mode: " + mode
                : connectivityError);
            return true;
        }
        const Netlist::DirectConnectivityReport report =
            session.current.runDirectConnectivityQuery(query);
        const ListArtifactResult artifact = report.ok
            ? writeAutomaticListArtifact(
                session, command, toLower(mode),
                makeConnectivityListArtifactContent(session.current, report))
            : ListArtifactResult{};
        ToolResponse response;
        response.ok = report.ok;
        response.status = report.ok ? ToolStatus::Ok : ToolStatus::Error;
        response.command = command;
        response.mode = toLower(mode);
        response.message = report.message;
        response.complete = report.ok;
        emitToolResponse(session, response, [&]() {
            printConnectivityReport(session.current, query, report, artifact.complete);
            printListArtifactMetadata(artifact);
        });
        return true;
    }

    if (command == "cone_query") {
        if (!requireDesign()) return true;
        std::string mode;
        if (!(iss >> mode)) {
            emitToolError(session, command, "", "Usage: cone_query <mode> [name] [options]");
            return true;
        }
        Netlist::ConeQuery query;
        std::string parseError;
        if (!buildConeQuery(session.current, iss, mode, query, parseError)) {
            emitToolError(session, command, mode, parseError);
            return true;
        }
        const Netlist::ConeReport report = session.current.runConeQuery(query);
        const ListArtifactResult artifact = report.ok
            ? writeAutomaticListArtifact(
                session, command, toLower(mode),
                makeConeListArtifactContent(session.current, report))
            : ListArtifactResult{};
        ToolResponse response;
        response.ok = report.ok;
        response.status = report.ok ? ToolStatus::Ok : ToolStatus::Error;
        response.command = command;
        response.mode = toLower(mode);
        response.message = report.message;
        response.complete = report.ok;
        emitToolResponse(session, response, [&]() {
            printConeReport(session.current, report, artifact.complete);
            printListArtifactMetadata(artifact);
        });
        return true;
    }

    if (command == "path_query") {
        if (!requireDesign()) return true;
        std::string modeText;
        std::string startToken;
        std::string endToken;
        if (!(iss >> modeText)) {
            emitToolError(session, command, "", "Usage: path_query <mode> [start_endpoint end_endpoint] [options]");
            return true;
        }

        Netlist::PathQuery query;
        if (!parsePathMode(modeText, query.mode)) {
            emitToolError(session, command, modeText, "Unknown path_query mode: " + modeText);
            return true;
        }
        if (query.mode == Netlist::PathQueryMode::EnumerateAll) {
            // 完整列表由 backend 自動寫檔；terminal 預設只保留摘要。
            query.maxPrintedPaths = 0;
        } else if (query.mode == Netlist::PathQueryMode::DirectPiPoConnections) {
            // 此 mode 不自動寫檔，因此預設完整顯示所有 direct connections。
            query.maxPrintedPaths = std::numeric_limits<size_t>::max();
        }
        const std::string loweredMode = toLower(modeText);
        const bool piPoCutMode = loweredMode == "pi_po_cut";
        if (piPoCutMode) {
            if (!(iss >> query.separatorCandidateNetName)) {
                emitToolError(session, command, modeText,
                              "pi_po_cut requires an internal candidate net.");
                return true;
            }
        } else if (query.mode != Netlist::PathQueryMode::DirectPiPoConnections) {
            if (!(iss >> startToken >> endToken)) {
                emitToolError(session, command, modeText,
                              "This path_query mode requires start and end endpoints.");
                return true;
            }
            query.startpoints.push_back(parseEndpoint(startToken));
            query.endpoints.push_back(parseEndpoint(endToken));
            if (query.mode == Netlist::PathQueryMode::IsSeparator &&
                !(iss >> query.separatorCandidateNetName)) {
                emitToolError(session, command, modeText,
                              "is_separator requires start, end, and candidate net.");
                return true;
            }
        }

        int listMode = 0;
        bool listHasValue = false;
        const auto requireCompletedNodeList = [&]() -> bool {
            if (listMode != 0 && !listHasValue) {
                emitToolError(session, command, modeText,
                              listMode == 1 ? "-req requires at least one node."
                                            : "-avoid requires at least one node.");
                return false;
            }
            return true;
        };
        std::string token;
        while (iss >> token) {
            if (token == "-req") {
                if (!requireCompletedNodeList()) return true;
                listMode = 1;
                listHasValue = false;
                continue;
            }
            if (token == "-avoid") {
                if (!requireCompletedNodeList()) return true;
                listMode = 2;
                listHasValue = false;
                continue;
            }
            if (token == "-out") {
                if (!requireCompletedNodeList()) return true;
                std::string outputPath;
                if (!(iss >> outputPath) || isOptionToken(outputPath)) {
                    emitToolError(session, command, modeText,
                                  "-out requires a file path value.");
                    return true;
                }
                query.writePathsToFile = true;
                query.outputFilePath = outputPath;
                listMode = 0;
                continue;
            }
            if (token == "-max_print") {
                if (!requireCompletedNodeList()) return true;
                std::string valueToken;
                size_t maxPrinted = 0;
                if (!(iss >> valueToken) || !parseNonNegativeSize(valueToken, maxPrinted)) {
                    emitToolError(session, command, modeText, "-max_print requires a non-negative integer.");
                    return true;
                }
                query.maxPrintedPaths = maxPrinted;
                listMode = 0;
                continue;
            }
            if (token == "-max_paths") {
                if (!requireCompletedNodeList()) return true;
                std::string valueToken;
                size_t maxPaths = 0;
                if (!(iss >> valueToken) || !parseNonNegativeSize(valueToken, maxPaths)) {
                    emitToolError(session, command, modeText, "-max_paths requires a non-negative integer.");
                    return true;
                }
                query.maxEnumeratedPaths = maxPaths;
                listMode = 0;
                continue;
            }
            if (token == "-time_limit") {
                if (!requireCompletedNodeList()) return true;
                std::string valueToken;
                double timeLimit = 0.0;
                if (!(iss >> valueToken) || !parsePositiveFiniteSeconds(valueToken, timeLimit)) {
                    emitToolError(session, command, modeText, "-time_limit requires a positive finite number of seconds.");
                    return true;
                }
                query.enumerationTimeLimitSeconds = timeLimit;
                listMode = 0;
                continue;
            }
            if (token == "-count_only") {
                if (!requireCompletedNodeList()) return true;
                query.countOnly = true;
                query.writePathsToFile = false;
                listMode = 0;
                continue;
            }
            if (isOptionToken(token)) {
                emitToolError(session, command, modeText,
                              "Unknown path_query option: " + token +
                              ". Supported public options: -req -avoid -count_only.");
                return true;
            }
            if (listMode == 1) {
                query.requiredNodes.push_back(parsePathNode(token));
                listHasValue = true;
            } else if (listMode == 2) {
                query.avoidedNodes.push_back(parsePathNode(token));
                listHasValue = true;
            } else {
                emitToolError(session, command, modeText,
                              "Unexpected token '" + token +
                              "'. Supported public options: -req -avoid -count_only.");
                return true;
            }
        }
        if (!requireCompletedNodeList()) return true;

        if (query.mode == Netlist::PathQueryMode::EnumerateAll &&
            !query.countOnly && query.outputFilePath.empty()) {
            query.writePathsToFile = true;
            query.outputFilePath = makeAutomaticPathOutputPath(session);
        }

        const Netlist::PathQueryResult result = session.current.runPathQuery(query);
        const ListArtifactResult artifact = result.ok
            ? writeAutomaticListArtifact(
                  session, command, toLower(modeText),
                  makePathListArtifactContent(session.current, query, result))
            : ListArtifactResult{};
        ToolResponse response;
        response.ok = result.ok;
        response.command = command;
        response.mode = toLower(modeText);
        response.complete = result.ok && result.completeEnumeration;
        if (!result.ok) {
            response.status = result.unsupported ? ToolStatus::Unsupported : ToolStatus::Error;
            response.message = result.message;
        } else if (result.enumerationTimedOut) {
            response.status = ToolStatus::Timeout;
            response.message = result.enumerationStopReason;
        } else if (!result.completeEnumeration) {
            response.status = ToolStatus::Partial;
            response.message = result.enumerationStopReason;
        } else {
            response.status = ToolStatus::Ok;
            response.message = result.message;
        }
        emitToolResponse(session, response, [&]() {
            printPathResult(session.current, query, result, artifact.complete);
            printListArtifactMetadata(artifact);
        });
        return true;
    }

    if (command == "reg_path_query") {
        if (!requireDesign()) return true;
        std::string modeText;
        if (!(iss >> modeText)) {
            emitToolError(session, command, "", "Usage: reg_path_query <mode> [options]");
            return true;
        }

        Netlist::RegisterPathQuery query;
        if (!parseRegisterPathMode(modeText, query.mode)) {
            emitToolError(session, command, modeText, "Unknown reg_path_query mode: " + modeText);
            return true;
        }

        int listMode = 0;
        bool listHasValue = false;
        const auto requireCompletedList = [&]() -> bool {
            if (listMode != 0 && !listHasValue) {
                static const char* names[] = {"", "-from", "-to", "-req", "-avoid"};
                emitToolError(session, command, modeText,
                              std::string(names[listMode]) + " requires at least one value.");
                return false;
            }
            return true;
        };
        std::string token;
        while (iss >> token) {
            if (token == "-from" || token == "-to" || token == "-req" || token == "-avoid") {
                if (!requireCompletedList()) return true;
                listMode = token == "-from" ? 1 : token == "-to" ? 2 : token == "-req" ? 3 : 4;
                listHasValue = false;
                continue;
            }
            if (token == "-out") {
                if (!requireCompletedList()) return true;
                std::string outputPath;
                if (!(iss >> outputPath) || isOptionToken(outputPath)) {
                    emitToolError(session, command, modeText,
                                  "-out requires a file path value.");
                    return true;
                }
                query.outputFilePath = outputPath;
                listMode = 0;
                continue;
            }
            if (token == "-max_print") {
                if (!requireCompletedList()) return true;
                std::string valueToken;
                size_t maxPrinted = 0;
                if (!(iss >> valueToken) || !parseNonNegativeSize(valueToken, maxPrinted)) {
                    emitToolError(session, command, modeText, "-max_print requires a non-negative integer.");
                    return true;
                }
                query.maxPrintedPaths = maxPrinted;
                listMode = 0;
                continue;
            }
            if (token == "-max_paths") {
                if (!requireCompletedList()) return true;
                std::string valueToken;
                size_t maxPaths = 0;
                if (!(iss >> valueToken) || !parseNonNegativeSize(valueToken, maxPaths)) {
                    emitToolError(session, command, modeText, "-max_paths requires a non-negative integer.");
                    return true;
                }
                query.maxEnumeratedPaths = maxPaths;
                listMode = 0;
                continue;
            }
            if (token == "-time_limit") {
                if (!requireCompletedList()) return true;
                std::string valueToken;
                double timeLimit = 0.0;
                if (!(iss >> valueToken) || !parsePositiveFiniteSeconds(valueToken, timeLimit)) {
                    emitToolError(session, command, modeText, "-time_limit requires a positive finite number of seconds.");
                    return true;
                }
                query.enumerationTimeLimitSeconds = timeLimit;
                listMode = 0;
                continue;
            }
            if (token == "-count_only") {
                if (!requireCompletedList()) return true;
                query.countOnly = true;
                listMode = 0;
                continue;
            }
            if (isOptionToken(token)) {
                emitToolError(session, command, modeText,
                              "Unknown reg_path_query option: " + token +
                              ". Supported public options: -from -to -req -avoid -count_only.");
                return true;
            }
            if (listMode == 1) query.startDffNames.push_back(token);
            else if (listMode == 2) query.endDffNames.push_back(token);
            else if (listMode == 3) query.requiredNodes.push_back(parsePathNode(token));
            else if (listMode == 4) query.avoidedNodes.push_back(parsePathNode(token));
            else {
                emitToolError(session, command, modeText,
                              "Unexpected token '" + token +
                              "'. Supported public options: -from -to -req -avoid -count_only.");
                return true;
            }
            listHasValue = true;
        }
        if (!requireCompletedList()) return true;

        const Netlist::RegisterPathReport report = session.current.runRegisterPathQuery(query);
        ToolResponse response;
        response.ok = report.ok;
        response.command = command;
        response.mode = toLower(modeText);
        response.complete = report.ok && report.pathResult.completeEnumeration;
        if (!report.ok) {
            response.status = ToolStatus::Error;
            response.message = report.message;
        } else if (report.pathResult.enumerationTimedOut) {
            response.status = ToolStatus::Timeout;
            response.message = report.pathResult.enumerationStopReason;
        } else if (!report.pathResult.completeEnumeration) {
            response.status = ToolStatus::Partial;
            response.message = report.pathResult.enumerationStopReason;
        } else {
            response.status = ToolStatus::Ok;
            response.message = report.message;
        }
        emitToolResponse(session, response, [&]() {
            printRegisterPathReport(session.current, query, report);
        });
        return true;
    }

    if (command == "depth_query") {
        if (!requireDesign()) return true;
        std::string mode;
        if (!(iss >> mode)) {
            emitToolError(session, command, "", "Usage: depth_query <mode> [args]");
            return true;
        }
        Netlist::DepthQuery query;
        std::string parseError;
        if (!buildDepthQuery(iss, mode, query, parseError)) {
            emitToolError(session, command, mode, parseError);
            return true;
        }
        const Netlist::DepthReportSet report = session.current.runDepthQuery(query);
        const ListArtifactResult artifact = report.ok
            ? writeAutomaticListArtifact(
                  session, command, toLower(mode),
                  makeDepthListArtifactContent(session.current, report))
            : ListArtifactResult{};
        ToolResponse response;
        response.ok = report.ok;
        response.status = report.ok ? ToolStatus::Ok : ToolStatus::Error;
        response.command = command;
        response.mode = toLower(mode);
        response.message = report.message;
        response.complete = report.ok;
        emitToolResponse(session, response, [&]() {
            printDepthReportSet(session.current, report, artifact.complete);
            printListArtifactMetadata(artifact);
        });
        return true;
    }

    if (command == "func_query") {
        if (!requireDesign()) return true;
        std::string mode;
        if (!(iss >> mode)) {
            emitToolError(session, command, "", "Usage: func_query <mode> [args]");
            return true;
        }
        Netlist::FunctionQuery query;
        std::string functionError;
        if (!buildFunctionQuery(iss, mode, query, functionError)) {
            emitToolError(session, command, mode, functionError.empty()
                ? "Unknown func_query mode: " + mode
                : functionError);
            return true;
        }
        if (query.type == Netlist::FunctionQueryType::BooleanExpression) {
            query.writeExpressionToFile = true;
            query.expressionOutputFilePath =
                makeAutomaticFunctionExpressionOutputPath(session);
        }
        const Netlist::FunctionReport report = session.current.runFunctionQuery(query);
        const bool allowGenericListArtifact =
            query.type != Netlist::FunctionQueryType::BooleanExpression;
        const ListArtifactResult listArtifact =
            report.ok && allowGenericListArtifact
                ? writeAutomaticListArtifact(
                    session,
                    command,
                    toLower(mode),
                    makeFunctionListArtifactContent(report))
                : ListArtifactResult{};
        ToolResponse response;
        response.ok = report.ok;
        response.command = command;
        response.mode = toLower(mode);
        const bool expressionArtifact =
            query.type == Netlist::FunctionQueryType::BooleanExpression &&
            query.writeExpressionToFile;
        response.complete = expressionArtifact
            ? report.expressionArtifactComplete
            : (report.ok && !report.solverUnknown && !report.unsupported);
        if (report.solverTimedOut || report.expressionArtifactTimedOut) {
            response.status = ToolStatus::Timeout;
        }
        else if (report.unsupported) response.status = ToolStatus::Unsupported;
        else if (report.solverUnknown) response.status = ToolStatus::Partial;
        else if (expressionArtifact && !report.expressionArtifactComplete) {
            response.status = ToolStatus::Partial;
        }
        else response.status = report.ok ? ToolStatus::Ok : ToolStatus::Error;
        response.message = report.message;
        emitToolResponse(session, response, [&]() {
            printFunctionReport(report, listArtifact.complete);
            printListArtifactMetadata(listArtifact);
        });
        return true;
    }

    if (command == "func_search") {
        if (!requireDesign()) return true;
        std::string mode;
        if (!(iss >> mode)) {
            emitToolError(
                session,
                command,
                "",
                "Usage: func_search pattern <gate_type> <target_net> [options] | "
                "func_search nand_pair <target_net> [options] | "
                "func_search equivalent_pairs <scope> [scope_name] [options]");
            return true;
        }

        Netlist::FunctionSearchQuery query;
        std::string error;
        if (!buildFunctionSearchQuery(session.current, iss, mode, query, error)) {
            emitToolError(session, command, mode, error);
            return true;
        }

        if (query.mode == Netlist::FunctionSearchMode::FindAll) {
            query.writeMatchesToFile = true;
            query.maxStoredMatches = 0;
            query.outputFilePath =
                makeAutomaticFunctionSearchOutputPath(session);
        }

        const Netlist::FunctionSearchReport report =
            session.current.runFunctionSearchQuery(query);
        ToolResponse response;
        response.ok = report.ok;
        response.command = command;
        response.mode = toLower(mode);
        response.message = report.message;
        response.complete = report.complete;
        if (report.timedOut) {
            response.status = ToolStatus::Timeout;
        } else if (report.unsupported) {
            response.status = ToolStatus::Unsupported;
        } else if (report.truncated || report.satUnknownCount > 0 ||
                   (!report.ok && report.found)) {
            response.status = ToolStatus::Partial;
        } else {
            response.status = report.ok ? ToolStatus::Ok : ToolStatus::Error;
        }
        emitToolResponse(session, response, [&]() {
            printFunctionSearchReport(session.current, report);
        });
        return true;
    }

    if (command == "graph_query") {
        if (!requireDesign()) return true;
        std::string mode;
        if (!(iss >> mode)) {
            emitToolError(session, command, "", "Usage: graph_query <mode> [args]");
            return true;
        }
        Netlist::GraphQuery query;
        if (!buildGraphQuery(iss, mode, query)) {
            emitToolError(session, command, mode, "Unknown graph_query mode or invalid arguments: " + mode);
            return true;
        }
        const Netlist::GraphReport report = session.current.runGraphQuery(query);
        ToolResponse response;
        response.ok = report.ok;
        response.command = command;
        response.mode = toLower(mode);
        response.complete = report.ok && !report.unsupported;
        response.status = report.unsupported
            ? ToolStatus::Unsupported
            : report.ok ? ToolStatus::Ok : ToolStatus::Error;
        response.message = report.message;
        emitToolResponse(session, response, [&]() { printGraphReport(report); });
        return true;
    }

    if (command == "sequential_query") {
        if (!requireDesign()) return true;
        std::string mode;
        if (!(iss >> mode)) {
            emitToolError(
                session,
                command,
                "",
                "Usage: sequential_query enable_hold <all|dff_name> [options]");
            return true;
        }

        Netlist::SequentialPatternQuery query;
        SequentialPrintOptions printOptions;
        std::string error;
        if (!buildSequentialPatternQuery(
                iss, mode, query, printOptions, error)) {
            emitToolError(session, command, toLower(mode), error);
            return true;
        }

        const Netlist::SequentialPatternReportSet report =
            session.current.runSequentialPatternQuery(query);
        const bool writeAutomaticArtifact =
            query.dffName.empty() && !printOptions.summaryOnly &&
            !printOptions.recordWindowExplicit;
        SequentialPrintOptions envelopeOptions = printOptions;
        bool wroteRecordsToFile = false;
        std::string outputFilePath;
        size_t artifactRecordCount = 0;
        if (writeAutomaticArtifact) {
            outputFilePath = makeAutomaticSequentialOutputPath(session);
            SequentialPrintOptions artifactOptions = printOptions;
            artifactOptions.summaryOnly = false;
            artifactOptions.recordOffset = 0;
            artifactOptions.recordLimit = std::numeric_limits<size_t>::max();
            artifactRecordCount =
                getSequentialPageStats(report, artifactOptions).reportedRecordCount;
            wroteRecordsToFile = writeSequentialPatternArtifact(
                outputFilePath, report, artifactOptions);
            envelopeOptions.summaryOnly = true;
            envelopeOptions.recordOffset = 0;
            envelopeOptions.recordLimit = 50;
        }
        const SequentialPageStats page =
            getSequentialPageStats(report, envelopeOptions);
        bool solverTimedOut = false;
        bool solverUnknown = false;
        for (const DffInputPatternReport& dff : report.reports) {
            for (const DffInputPattern& pattern : dff.patterns) {
                solverTimedOut = solverTimedOut || pattern.solverTimedOut;
                solverUnknown = solverUnknown || pattern.solverUnknown;
            }
        }

        ToolResponse response;
        response.ok = report.ok &&
                      (!writeAutomaticArtifact || wroteRecordsToFile);
        response.command = command;
        response.mode = toLower(mode);
        if (writeAutomaticArtifact && !wroteRecordsToFile) {
            response.message =
                "Failed to write Sequential Pattern output file: " +
                outputFilePath;
        } else if (writeAutomaticArtifact) {
            response.message = report.message +
                               " Complete detail records were written to " +
                               outputFilePath + ".";
        } else if (page.truncated) {
            response.message =
                report.message + " Record page truncated; continue with --offset " +
                std::to_string(page.nextRecordOffset) + ".";
        } else {
            response.message = report.message;
        }
        response.complete = report.ok && report.complete &&
                            !solverTimedOut && !solverUnknown && !page.truncated &&
                            (!writeAutomaticArtifact || wroteRecordsToFile);
        if (writeAutomaticArtifact && !wroteRecordsToFile) {
            response.status = ToolStatus::Error;
        } else if (report.timedOut || solverTimedOut) {
            response.status = ToolStatus::Timeout;
        } else if (solverUnknown || report.status == "PARTIAL") {
            response.status = ToolStatus::Partial;
        } else if (page.truncated) {
            response.status = ToolStatus::Partial;
        } else {
            response.status = report.ok ? ToolStatus::Ok : ToolStatus::Error;
        }
        emitToolResponse(session, response, [&]() {
            printSequentialPatternReport(report, envelopeOptions);
            std::cout << "  wrote_records_to_file: "
                      << (wroteRecordsToFile ? "true" : "false") << "\n";
            std::cout << "  artifact_record_count: "
                      << (wroteRecordsToFile ? artifactRecordCount : 0) << "\n";
            if (writeAutomaticArtifact) {
                std::cout << "  output_file: " << outputFilePath << "\n";
            }
        });
        return true;
    }

    if (command == "opt_query") {
        if (!requireDesign()) return true;
        std::string mode;
        if (!(iss >> mode)) {
            emitToolError(
                session,
                command,
                "",
                "Usage: opt_query critical_path_depth");
            return true;
        }
        if (toLower(mode) != "critical_path_depth") {
            emitToolError(
                session,
                command,
                mode,
                "Unknown or non-public opt_query mode: " + mode);
            return true;
        }
        std::string extra;
        if (iss >> extra) {
            emitToolError(
                session,
                command,
                mode,
                "Unexpected opt_query argument: " + extra);
            return true;
        }

        Netlist::OptQueryRequest request;
        request.passKind = OptPassKind::CriticalPathDepth;
        const Netlist::OptQueryReport report =
            session.current.runOptQuery(request);

        ToolResponse response;
        response.ok = report.ok;
        response.status = report.ok ? ToolStatus::Ok : ToolStatus::Error;
        response.command = command;
        response.mode = "critical_path_depth";
        response.message = report.message;
        response.complete = report.ok;
        emitToolResponse(session, response, [&]() {
            printOptQueryReport(session.current, report);
        });
        return true;
    }

    if (command == "opt_apply") {
        if (!requireDesign()) return true;
        std::string mode;
        if (!(iss >> mode)) {
            emitToolError(
                session,
                command,
                "",
                "Usage: opt_apply critical_path_depth [options]");
            return true;
        }

        Netlist::OptApplyRequest request;
        request.validateEquivalence = true;
        request.rollbackOnFailure = true;
        std::string error;
        if (!parsePublicOptApply(
                session.current, iss, mode, request, error)) {
            emitToolError(session, command, mode, error);
            return true;
        }

        session.lastEditBaseline = session.current.cloneForRollback();
        const Netlist::NetlistEditReport report =
            session.current.runOptApply(request);
        session.lastEditReport = report;
        if (report.success && report.changed) {
            ++session.designRevision;
        }

        const bool optimizationTimedOut =
            report.depthOptimization &&
            report.depthOptimization->coreStatus == "TIMEOUT";
        const bool equivalenceComplete =
            report.validation.equivalenceChecked &&
            report.validation.functionallyEquivalent;

        ToolResponse response;
        response.ok = report.success;
        if (optimizationTimedOut) {
            response.status = ToolStatus::Timeout;
        } else if (!report.success) {
            response.status = ToolStatus::Error;
        } else {
            response.status = report.changed ? ToolStatus::Ok : ToolStatus::NoChange;
        }
        response.command = command;
        response.mode = toLower(mode);
        response.message = report.message;
        response.complete = report.success;
        emitToolResponse(session, response, [&]() {
            printEditReport(session.current, report);
        });
        return true;
    }

    if (command == "equiv_query") {
        if (!requireDesign()) return true;
        std::string mode;
        if (!(iss >> mode)) {
            emitToolError(session, command, "", "Usage: equiv_query <original|previous_edit> [time_budget_seconds]");
            return true;
        }
        mode = toLower(mode);

        double timeBudgetSeconds =
            request_time_budget::kGeneralToolBudgetSeconds;
        std::string budgetToken;
        if (iss >> budgetToken) {
            std::istringstream budgetStream(budgetToken);
            char trailing = '\0';
            if (!(budgetStream >> timeBudgetSeconds) ||
                (budgetStream >> trailing) ||
                !std::isfinite(timeBudgetSeconds) ||
                timeBudgetSeconds <= 0.0) {
                emitToolError(session, command, mode,
                              "time_budget_seconds must be a finite number greater than zero.");
                return true;
            }
        }
        std::string extra;
        if (iss >> extra) {
            emitToolError(session, command, mode, "Unexpected equiv_query argument: " + extra);
            return true;
        }

        const Netlist* baseline = nullptr;
        if (mode == "original") {
            if (!session.original) {
                emitToolError(session, command, mode,
                              "NO_ORIGINAL_SNAPSHOT: read a design before comparing with original.");
                return true;
            }
            baseline = &*session.original;
        } else if (mode == "previous_edit") {
            if (!session.lastEditBaseline) {
                emitToolError(session, command, mode,
                              "NO_PREVIOUS_EDIT_BASELINE: run edit_apply or opt_apply before this comparison.");
                return true;
            }
            baseline = &*session.lastEditBaseline;
        } else {
            emitToolError(session, command, mode,
                          "Unknown equiv_query mode: " + mode + ". Use original or previous_edit.");
            return true;
        }

        const Netlist::WholeDesignEquivalenceReport report =
            session.current.checkWholeDesignEquivalence(*baseline, timeBudgetSeconds);
        const bool interfaceMismatch =
            !report.missingInputNames.empty() || !report.extraInputNames.empty() ||
            !report.missingOutputNames.empty() || !report.extraOutputNames.empty() ||
            !report.missingDffNames.empty() || !report.extraDffNames.empty();
        const bool incomplete =
            report.timeBudgetExceeded || report.skippedOutputCount > 0 ||
            report.skippedDffDCount > 0 ||
            !report.unsupportedReasons.empty();

        ToolResponse response;
        response.command = command;
        response.mode = mode;
        response.message = report.message;
        if (interfaceMismatch) {
            response.ok = true;
            response.status = ToolStatus::Ok;
            response.complete = true;
        } else if (report.timeBudgetExceeded) {
            response.ok = false;
            response.status = ToolStatus::Timeout;
            response.complete = false;
        } else if (!report.unsupportedReasons.empty()) {
            response.ok = false;
            response.status = ToolStatus::Unsupported;
            response.complete = false;
        } else if (report.skippedOutputCount > 0) {
            response.ok = true;
            response.status = ToolStatus::Partial;
            response.complete = false;
        } else if (report.ok && !incomplete) {
            response.ok = true;
            response.status = ToolStatus::Ok;
            response.complete = true;
        } else {
            response.ok = false;
            response.status = ToolStatus::Error;
            response.complete = false;
        }
        emitToolResponse(session, response, [&]() {
            printWholeDesignEquivalenceReport(report);
        });
        return true;
    }

    if (command == "report_query") {
        if (!requireDesign()) return true;
        std::string mode;
        if (!(iss >> mode) || toLower(mode) != "last_edit") {
            emitToolError(session, command, mode, "Usage: report_query last_edit");
            return true;
        }
        std::string extra;
        if (iss >> extra) {
            emitToolError(session, command, mode, "Unexpected report_query argument: " + extra);
            return true;
        }
        if (!session.lastEditReport) {
            emitToolError(
                session,
                command,
                mode,
                "NO_LAST_EDIT_REPORT: run edit_apply or opt_apply first.");
            return true;
        }

        ToolResponse response;
        response.ok = true;
        response.status = ToolStatus::Ok;
        response.command = command;
        response.mode = "last_edit";
        response.message = "Cached last edit report.";
        response.complete = true;
        emitToolResponse(session, response, [&]() {
            printEditReport(session.current, *session.lastEditReport);
        });
        return true;
    }

    if (command == "edit_apply") {
        if (!requireDesign()) return true;
        std::string mode;
        if (!(iss >> mode)) {
            emitToolError(session, command, "", "Usage: edit_apply <mode> [args]");
            return true;
        }

        Netlist::EditApplyRequest request;
        request.validateEquivalence = true;
        request.rollbackOnFailure = true;
        std::string error;
        if (!parsePublicEditApply(session.current, iss, mode, request, error)) {
            emitToolError(session, command, mode, error);
            return true;
        }

        session.lastEditBaseline = session.current.cloneForRollback();
        const Netlist::NetlistEditReport report = session.current.runEditApply(request);
        session.lastEditReport = report;
        if (report.success && report.changed) {
            ++session.designRevision;
        }

        const bool equivalenceComplete =
            !request.validateEquivalence ||
            (report.validation.equivalenceChecked &&
             report.validation.functionallyEquivalent);
        const bool functionalMergeTimedOut =
            report.functionalMerge &&
            (report.functionalMerge->searchTimedOut ||
             report.functionalMerge->wholeDesignTimedOut);
        const bool deadLogicTimedOut =
            report.deadLogic && report.deadLogic->timedOut;
        const bool redundancyTimedOut =
            report.redundancyRemoval && report.redundancyRemoval->timedOut;
        const bool anyTimedOut =
            functionalMergeTimedOut || deadLogicTimedOut || redundancyTimedOut;

        ToolResponse response;
        response.ok = report.success;
        if (anyTimedOut) {
            response.status = ToolStatus::Timeout;
        } else if (!report.success) {
            response.status = ToolStatus::Error;
        } else if (!equivalenceComplete) {
            response.status = ToolStatus::Partial;
        } else {
            response.status = report.changed ? ToolStatus::Ok : ToolStatus::NoChange;
        }
        response.command = command;
        response.mode = toLower(mode);
        response.message = !report.success || equivalenceComplete
            ? report.message
            : "Edit completed, but the requested equivalence certificate is unavailable. " + report.message;
        response.complete = report.success && equivalenceComplete && !anyTimedOut;
        emitToolResponse(session, response, [&]() { printEditReport(session.current, report); });
        return true;
    }

    emitToolError(session, command, "", "Unknown command: " + command + ". Type help for unified commands.");
    return true;
}

} // namespace

int main() {
    ToolSession session;
    std::string inputLine;

    while (true) {
        std::cout << "eda> ";
        if (!std::getline(std::cin, inputLine)) {
            break;
        }
        if (!dispatchCommand(session, inputLine)) {
            break;
        }
    }

    return 0;
}
