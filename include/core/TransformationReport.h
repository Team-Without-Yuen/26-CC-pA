#pragma once

#include <vector>
#include <string>
#include <unordered_map>

// 單筆 Buffer 插入紀錄
struct BufInsertRecord {
    // Buffer 自身的資訊
    int bufGateId;
    std::string bufGateName;

    // 驅動這個 Buffer 的源頭 (由誰推動)
    int inputNetId;
    std::string inputNetName;
    int driverGateId;           // 如果是 Primary Input (PI) 驅動，這個值會是 -1
    std::string driverGateName; // 如果是 PI，可以存為 "PI" 或 "None"

    // 這個 Buffer 推動的目標 (Fanout)
    int outputNetId;
    std::string outputNetName;
    int fanoutCount;                          // 這個 Buffer 推動的 Gate 數量
    std::vector<int> drivenGateIds;           // 具體推動的 Load Gate IDs
    std::vector<std::string> drivenGateNames; // 具體推動的 Load Gate 名稱
};

// 整體 Buffer 插入報告
struct BufferInsertionReport {
    std::vector<BufInsertRecord> records;

    // 取得新增的 BUF 總數
    int getTotalInserted() const {
        return records.size();
    }

    // 將所有的紀錄合併到另一份報告中 (當你連續呼叫多個插入 function 時很好用)
    void merge(const BufferInsertionReport& other) {
        records.insert(records.end(), other.records.begin(), other.records.end());
    }
};

// 定義 ECO 替換的狀態碼，方便程式邏輯判斷
enum class TechMapStatus {
    SUCCESS,                  // 替換成功
    ERROR_RULE_NOT_FOUND,     // 找不到符合數量/類型的替換規則
    ERROR_NOT_EQUIVALENT,     // (真值表驗證) 邏輯不等價
    ERROR_UNSAT,              // (SAT Solver) 數學證明無法拼出該邏輯
    ERROR_SIMULATION_FAILED,  // 真值表模擬失敗
    ERROR_INVALID_CONSTRAINTS // 沒有給合理的CONSTRAINTS
};

// 描述由 SAT Solver 合成出來的抽象閘與接線
struct SynthesizedGate {
    GateType type;
    // 儲存輸入來源的 ID。
    // 如果值 < N (PI 數量)，代表接到了 PI_0, PI_1 等。
    // 如果值 >= N，代表接到了之前生成的另一顆 SynthesizedGate (ID = 值 - N)
    std::vector<int> inputSourceIds; 
};

// TechMapper 邏輯替換報告
struct TechMapReport {
    // 紀錄被拔除/替換掉的 Gate 數量 (分類統計，屬於 targetTypes)
    std::unordered_map<GateType, int> removedCountByType;

    // 紀錄被變更的 Gate 名字 (被拔除或被取代的 Target Gates)
    std::vector<std::string> modifiedGateNames;

    // 紀錄新生成的 Gate 數量 (分類統計，屬於 allowedTypes)
    std::unordered_map<GateType, int> addedCountByType;

    // 修改後，整個電路的 Gate 數量快照 (僅統計 allowedTypes 裡的類型)
    std::unordered_map<GateType, int> finalGateCount;

    // 狀態與訊息回報
    TechMapStatus status = TechMapStatus::SUCCESS;
    std::string message = "Success";

    // 用來儲存 Exact Synthesis 算出來的全新電路拓樸
    std::vector<SynthesizedGate> synthesizedTopology;

    // 輔助函式：用來合併多個小操作的報告
    void merge(const TechMapReport& other) {
        // 如果遇到失敗狀態，優先保留失敗狀態與錯誤訊息
        if (other.status != TechMapStatus::SUCCESS) {
            this->status = other.status;
            this->message = other.message;
        }

        for (const auto& pair : other.removedCountByType) removedCountByType[pair.first] += pair.second;
        for (const auto& pair : other.addedCountByType) addedCountByType[pair.first] += pair.second;
        
        modifiedGateNames.insert(modifiedGateNames.end(), 
                                 other.modifiedGateNames.begin(), 
                                 other.modifiedGateNames.end());
        
        finalGateCount = other.finalGateCount; 
    }
};