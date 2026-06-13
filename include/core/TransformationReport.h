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

// TechMapper 邏輯替換報告
struct TechMapReport {
    bool isOneToMany; // 是展開還是濃縮

    // 紀錄被拔除/替換掉的 Gate 數量 (分類統計，屬於 targetTypes)
    std::unordered_map<GateType, int> removedCountByType;

    // 紀錄被變更的 Gate 名字 (被拔除或被取代的 Target Gates)
    std::vector<std::string> modifiedGateNames;

    // 紀錄新生成的 Gate 數量 (分類統計，屬於 allowedTypes)
    std::unordered_map<GateType, int> addedCountByType;

    // 修改後，整個電路的 Gate 數量快照 (僅統計 allowedTypes 裡的類型)
    std::unordered_map<GateType, int> finalGateCount;

    // 用來合併多個小操作的報告
    void merge(const TechMapReport& other) {
        for (const auto& pair : other.removedCountByType) removedCountByType[pair.first] += pair.second;
        for (const auto& pair : other.addedCountByType) addedCountByType[pair.first] += pair.second;
        
        modifiedGateNames.insert(modifiedGateNames.end(), 
                                 other.modifiedGateNames.begin(), 
                                 other.modifiedGateNames.end());
        
        // finalGateCount 通常只以最後一次的快照為主，所以合併時直接覆蓋或保留最新
        finalGateCount = other.finalGateCount; 
    }
};