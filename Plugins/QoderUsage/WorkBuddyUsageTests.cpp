#include "QoderUsagePlugin.h"
#include "WorkBuddyUsage.h"
#include <cmath>
#include <iostream>

namespace
{
    int failures{};
    void Check(bool passed, const char* message)
    {
        if (!passed) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
    }
}

int main(int argc, char** argv)
{
    QoderSnapshot snapshot;
    Check(WorkBuddyUsage::ParseSummary(LR"({"code":0,"account":"0123456789abcdef","data":{
      "SubscriptionPackageName":"Trial","Packages":[
       {"PackageCode":"trial","CapacityUnit":"credits","CycleTotalCapacity":"500","CycleRemainCapacity":"420.25","CycleFrozenCapacity":"10"},
       {"PackageCode":"gift","CapacityUnit":"credits","CycleTotalCapacity":100,"CycleRemainCapacity":80},
       {"CapacityUnit":"tokens","CycleTotalCapacity":1000000,"CycleRemainCapacity":999999}],
      "CreditGroups":[{"CycleTotalCapacity":"600","CycleRemainCapacity":"500.25"}]}})", snapshot), "valid numeric strings and numbers");
    Check(snapshot.has_data && snapshot.usage.rows.size() == 2, "credits only; groups must not double-count packages");
    if (snapshot.usage.rows.size() == 2)
    {
        Check(std::abs(snapshot.usage.rows[0].quota.remaining - 420.25) < 1e-9, "reserved must not be deducted a second time");
        Check(snapshot.usage.rows[0].quota.reserved == 10, "preserve reserved separately");
        Check(std::abs(snapshot.usage.total_usage_percentage - 99.75 / 600 * 100) < 1e-9, "aggregate ratio");
    }
    Check(snapshot.history_identity == L"workbuddy-cn-0123456789abcdef", "account-isolated history");
    const std::wstring international_json = LR"({"code":0,"account":"0123456789abcdef","data":{"Packages":[
      {"CapacityUnit":"credit","CycleTotalCapacity":"250","CycleRemainCapacity":"250"},
      {"CapacityUnit":"credits","CycleTotalCapacity":"100","CycleRemainCapacity":"100"}],
      "CreditGroups":[{"CycleTotalCapacity":"350","CycleRemainCapacity":"350"}]}})";
    QoderSnapshot international;
    Check(WorkBuddyUsage::ParseSummary(international_json, international, WorkBuddyUsage::Edition::International), "international summary");
    Check(international.usage.rows.size() == 2 && international.usage.rows[0].quota.remaining + international.usage.rows[1].quota.remaining == 350,
        "singular credit signup grant is included exactly once");
    Check(international.history_identity == L"workbuddy-global-0123456789abcdef" && international.history_identity != snapshot.history_identity,
        "same uid across editions must have separate history");
    Check(international.usage.detail_url == L"https://www.workbuddy.ai/profile/plans-usage" &&
        snapshot.usage.detail_url == L"https://www.workbuddy.cn/profile/plans-usage", "edition-specific website links");
    for (const wchar_t* malformed : {
        LR"({"code":1,"data":{"Packages":[]}})",
        LR"({"data":{"Packages":[]}})",
        LR"({"code":0,"data":{}})",
        LR"({"code":0,"data":{"Packages":null}})",
        LR"({"code":0,"data":{"Packages":[{"CapacityUnit":"credits","CycleTotalCapacity":"500"}]}})",
        LR"({"code":0,"data":{"Packages":[{"CapacityUnit":"credits","CycleTotalCapacity":"NaN","CycleRemainCapacity":0}]}})",
        LR"({"code":0,"data":{"Packages":[{"CapacityUnit":"credits","CycleTotalCapacity":"100oops","CycleRemainCapacity":0}]}})",
        LR"({"code":0,"data":{"Packages":[{"CapacityUnit":"credits","CycleTotalCapacity":100,"CycleRemainCapacity":-1}]}})",
        LR"({"code":0,"data":{"Packages":[{"CapacityUnit":"tokens","CycleTotalCapacity":100,"CycleRemainCapacity":50}]}})" })
    {
        QoderSnapshot bad;
        Check(!WorkBuddyUsage::ParseSummary(malformed, bad) && !bad.has_data, "invalid response must not become a zero balance");
    }
    QoderSnapshot zero;
    Check(WorkBuddyUsage::ParseSummary(LR"({"code":0,"data":{"Packages":[]}})", zero) && zero.has_data, "known empty balance");
    QoderSnapshot rollover;
    Check(WorkBuddyUsage::ParseSummary(LR"({"code":0,"data":{"Packages":[{"CapacityUnit":"credits","CycleTotalCapacity":100,"CycleRemainCapacity":120}]}})", rollover)
        && rollover.usage.rows[0].quota.remaining == 120 && rollover.usage.total_usage_percentage == 0, "rollover credits remain visible without negative used percentage");
    if (argc > 1 && std::string(argv[1]) == "--live")
    {
        for (const auto edition : { WorkBuddyUsage::Edition::China, WorkBuddyUsage::Edition::International })
        {
            QoderSnapshot live;
            WorkBuddyUsage::Refresh(live, false, edition);
            Check(live.has_data, "installed WorkBuddy runtime and live billing query");
            double total = 0, remaining = 0;
            for (const auto& row : live.usage.rows) { total += row.quota.total; remaining += row.quota.remaining; }
            std::wcout << (edition == WorkBuddyUsage::Edition::China ? L"China" : L"International")
                << L" live status: " << live.status << L"; credits " << remaining << L"/" << total << L'\n';
        }
    }
    std::cout << (failures ? "FAILED" : "PASSED") << ": WorkBuddy billing parsing and credential transport\n";
    return failures ? 1 : 0;
}
