#pragma once

#include <vector>
#include <string>

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