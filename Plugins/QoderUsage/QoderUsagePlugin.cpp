#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>

#include "QoderUsagePlugin.h"
#include "QoderDesktopAuth.h"
#include "WorkBuddyUsage.h"
#include "JsonValue.h"
#include "OptionsResource.h"
#include "PluginTooltipGuard.h"
#include "PluginAppButton.h"
#include "../CodexUsage/TimeBarMarkers.h"
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace
{
    constexpr wchar_t kUsagePath[] = L"/sash/api/v2/me/usage";
    constexpr DWORD kHttpTimeoutMs = 15000;
    constexpr wchar_t kDetailPopupClass[] = L"TrafficMonitorQoderUsageDetails";

    struct Region
    {
        const wchar_t* name;
        const wchar_t* base;
    };
    constexpr Region kRegions[] = {
        { L"global", L"https://openapi.qoder.sh" },
        { L"cn",     L"https://openapi.qoder.com.cn" },
    };

    struct Handle
    {
        HINTERNET value{};
        explicit Handle(HINTERNET h = nullptr) : value(h) {}
        ~Handle() { if (value) WinHttpCloseHandle(value); }
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;
        operator HINTERNET() const { return value; }
    };

    HWND g_detail_popup{};
    PluginTooltipGuard g_tooltip_guard;
    HINSTANCE g_popup_instance{};
    bool g_popup_tracking_mouse{};
    int g_popup_dpi{ 96 };
    POINT g_popup_anchor{};
    ULONGLONG g_popup_opened{};
    QuotaProvider g_popup_provider{};

    bool IsWorkBuddy(QuotaProvider provider)
    {
        return provider == QuotaProvider::WorkBuddyChina || provider == QuotaProvider::WorkBuddyInternational;
    }

    bool IsChina(QuotaProvider provider)
    {
        return provider == QuotaProvider::QoderChina || provider == QuotaProvider::WorkBuddyChina;
    }

    QoderDesktopAuth::Edition QoderEdition(QuotaProvider provider)
    {
        return provider == QuotaProvider::QoderChina ? QoderDesktopAuth::Edition::China : QoderDesktopAuth::Edition::International;
    }

    WorkBuddyUsage::Edition BuddyEdition(QuotaProvider provider)
    {
        return provider == QuotaProvider::WorkBuddyInternational
            ? WorkBuddyUsage::Edition::International : WorkBuddyUsage::Edition::China;
    }

    const wchar_t* ProviderName(QuotaProvider provider)
    {
        if (provider == QuotaProvider::QoderChina) return L"QoderCN";
        if (provider == QuotaProvider::WorkBuddyChina) return L"WorkBuddyCN";
        if (provider == QuotaProvider::WorkBuddyInternational) return L"WorkBuddy";
        return L"Qoder";
    }

    std::wstring ApplicationPath(QuotaProvider provider)
    {
        return IsWorkBuddy(provider) ? WorkBuddyUsage::ExecutablePath(BuddyEdition(provider))
            : QoderDesktopAuth::ExecutablePath(QoderEdition(provider));
    }

    // ---------- helpers ----------

    int DPI(int pixel)
    {
        const ITrafficMonitor* app = CQoderUsagePlugin::Instance().App();
        if (app != nullptr)
            return app->GetDPI(ITrafficMonitor::DPI_TASKBAR) * pixel / 96;
        return pixel;
    }

    std::wstring FormatLocalTime(long long unix_seconds)
    {
        if (unix_seconds <= 0) return L"--";
        std::tm value{};
        if (localtime_s(&value, &unix_seconds) != 0) return std::to_wstring(unix_seconds);
        std::wostringstream out;
        out << (value.tm_year + 1900) << L"-" << std::setw(2) << std::setfill(L'0') << (value.tm_mon + 1)
            << L"-" << std::setw(2) << std::setfill(L'0') << value.tm_mday << L" "
            << std::setw(2) << std::setfill(L'0') << value.tm_hour << L":"
            << std::setw(2) << std::setfill(L'0') << value.tm_min;
        return out.str();
    }

    long long Now()
    {
        return static_cast<long long>(std::time(nullptr));
    }

    std::wstring FormatClock(int minute)
    {
        wchar_t text[6]{};
        swprintf_s(text, L"%02d:%02d", minute / 60, minute % 60);
        return text;
    }

    bool ParseClock(HWND dialog, int id, int& minute)
    {
        wchar_t text[16]{};
        GetDlgItemTextW(dialog, id, text, static_cast<int>(std::size(text)));
        if (wcslen(text) != 5 || text[2] != L':') return false;
        for (int i : { 0, 1, 3, 4 }) if (text[i] < L'0' || text[i] > L'9') return false;
        const int hour = (text[0] - L'0') * 10 + text[1] - L'0';
        const int mins = (text[3] - L'0') * 10 + text[4] - L'0';
        if (hour > 23 || mins > 59) return false;
        minute = hour * 60 + mins;
        return true;
    }

    long long ParseEpoch(const JsonValue* value)
    {
        if (value == nullptr || value->type != JsonValue::Type::Number || !std::isfinite(value->number))
            return 0;
        if (value->number <= 0.0) return 0;
        const double seconds = value->number < 1e12 ? value->number : value->number / 1000.0;
        return static_cast<long long>(seconds);
    }

    double PickNumber(const JsonValue* object, std::initializer_list<const wchar_t*> keys)
    {
        if (object == nullptr || object->type != JsonValue::Type::Object) return 0.0;
        for (const wchar_t* key : keys)
        {
            const JsonValue* child = object->Get(key);
            if (child != nullptr && child->type == JsonValue::Type::Number && std::isfinite(child->number))
                return child->number;
        }
        return 0.0;
    }

    std::wstring PickString(const JsonValue* object, std::initializer_list<const wchar_t*> keys)
    {
        if (object == nullptr || object->type != JsonValue::Type::Object) return {};
        for (const wchar_t* key : keys)
        {
            const JsonValue* child = object->Get(key);
            if (child != nullptr && child->type == JsonValue::Type::String && !child->string.empty())
                return child->string;
        }
        return {};
    }

    bool PickBool(const JsonValue* object, std::initializer_list<const wchar_t*> keys)
    {
        if (object == nullptr || object->type != JsonValue::Type::Object) return false;
        for (const wchar_t* key : keys)
        {
            const JsonValue* child = object->Get(key);
            if (child != nullptr && child->type == JsonValue::Type::Boolean) return child->boolean;
        }
        return false;
    }

    double NormalizePercentage(double raw, double used, double total)
    {
        if (std::isfinite(raw))
        {
            if (raw >= 0.0 && raw <= 1.0) return raw * 100.0;
            if (raw >= 0.0 && raw <= 100.0) return raw;
        }
        if (total > 0.0) return (std::clamp)(used / total * 100.0, 0.0, 100.0);
        return 0.0;
    }

    QoderQuota MakeQuota(const JsonValue* raw)
    {
        QoderQuota quota;
        if (raw == nullptr || raw->type != JsonValue::Type::Object) return quota;
        quota.total = PickNumber(raw, { L"total" });
        quota.used = PickNumber(raw, { L"used" });
        const double remaining = PickNumber(raw, { L"remaining" });
        quota.remaining = (std::isfinite(remaining) && remaining >= 0.0) ? remaining : (std::max)(0.0, quota.total - quota.used);
        quota.used_percentage = NormalizePercentage(PickNumber(raw, { L"percentage" }), quota.used, quota.total);
        const std::wstring unit = PickString(raw, { L"unit" });
        if (!unit.empty()) quota.unit = unit;
        const JsonValue* available = raw->Get(L"available");
        quota.available = (available == nullptr || available->type != JsonValue::Type::Boolean) ? true : available->boolean;
        const JsonValue* expires = raw->Get(L"expires_at");
        if (expires == nullptr) expires = raw->Get(L"expiresAt");
        quota.expires_at = ParseEpoch(expires);
        return quota;
    }

    void AppendRow(std::vector<QoderQuotaRow>& rows, QoderQuotaKind kind, const std::wstring& key,
        const std::wstring& label, const QoderQuota& quota)
    {
        if (quota.total <= 0.0 && quota.used <= 0.0 && quota.remaining <= 0.0) return;
        rows.push_back({ kind, key, label, quota });
    }

    bool ParseUsage(const JsonValue& root, QoderUsage& usage)
    {
        if (root.type != JsonValue::Type::Object) return false;
        if (PickString(&root, { L"displayMode" }) == L"enterprise")
        {
            usage.enterprise = true;
            const JsonValue* enterprise = root.Get(L"enterpriseUsage");
            usage.detail_url = PickString(enterprise, { L"detailUrl", L"detail_url" });
            return true;
        }
        const JsonValue* q = root.Get(L"qoderUsage");
        if (q == nullptr || q->type != JsonValue::Type::Object) return false;
        usage.user_type = PickString(q, { L"user_type", L"userType" });
        usage.quota_exceeded = PickBool(q, { L"is_quota_exceeded", L"isQuotaExceeded" });
        const double total_pct = PickNumber(q, { L"total_usage_percentage", L"totalUsagePercentage" });
        usage.total_usage_percentage = (total_pct > 0.0) ? NormalizePercentage(total_pct, 0.0, 0.0) : -1.0;
        const JsonValue* expires = q->Get(L"expires_at");
        if (expires == nullptr) expires = q->Get(L"expiresAt");
        usage.cycle_expires_at = ParseEpoch(expires);
        usage.detail_url = PickString(q, { L"upgrade_url", L"upgradeUrl" });

        const JsonValue* user_quota = q->Get(L"user_quota");
        if (user_quota == nullptr) user_quota = q->Get(L"userQuota");
        if (user_quota != nullptr)
            AppendRow(usage.rows, QoderQuotaKind::User, L"user", L"", MakeQuota(user_quota));

        const JsonValue* add_on_quota = q->Get(L"add_on_quota");
        if (add_on_quota == nullptr) add_on_quota = q->Get(L"addOnQuota");
        if (add_on_quota != nullptr)
            AppendRow(usage.rows, QoderQuotaKind::AddOn, L"addon", L"", MakeQuota(add_on_quota));

        const JsonValue* org_raw = q->Get(L"org_resource_package");
        if (org_raw == nullptr) org_raw = q->Get(L"orgResourcePackage");
        if (org_raw == nullptr) org_raw = q->Get(L"shared_quota");
        if (org_raw == nullptr) org_raw = q->Get(L"sharedQuota");
        if (org_raw != nullptr && org_raw->type == JsonValue::Type::Object)
        {
            QoderQuota org = MakeQuota(org_raw);
            const double cap = PickNumber(org_raw, { L"cap", L"total" });
            if (cap > 0.0)
            {
                org.total = cap;
                org.remaining = (std::max)(0.0, cap - org.used);
                org.used_percentage = NormalizePercentage(PickNumber(org_raw, { L"percentage" }), org.used, cap);
            }
            const JsonValue* available = org_raw->Get(L"available");
            org.available = (available == nullptr || available->type != JsonValue::Type::Boolean)
                ? (org.total > 0.0) : available->boolean;
            AppendRow(usage.rows, QoderQuotaKind::Org, L"org", L"", org);
        }

        const JsonValue* packages = q->Get(L"dedicated_resource_packages");
        if (packages == nullptr) packages = q->Get(L"dedicatedResourcePackages");
        if (packages != nullptr && packages->type == JsonValue::Type::Array)
        {
            int index = 0;
            for (const JsonValue& pkg : packages->array)
            {
                QoderQuota quota = MakeQuota(&pkg);
                if (quota.total <= 0.0) { ++index; continue; }
                const std::wstring id = PickString(&pkg, { L"id" });
                const std::wstring name = PickString(&pkg, { L"name" });
                const std::wstring label = name.empty() ? id : name;
                std::wstring key = L"pkg:";
                key += id.empty() ? std::to_wstring(index) : id;
                AppendRow(usage.rows, QoderQuotaKind::Package, key, label, quota);
                ++index;
            }
        }
        return true;
    }

    // ---------- HTTP ----------

    bool HttpGet(const wchar_t* base, const std::wstring& token, const wchar_t* path, std::wstring& body)
    {
        if (token.empty()) return false;
        // WinHttpConnect expects a hostname, not an https:// URL.
        URL_COMPONENTS url{};
        url.dwStructSize = sizeof(url);
        url.dwHostNameLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(base, 0, 0, &url) || url.nScheme != INTERNET_SCHEME_HTTPS)
            return false;
        const std::wstring host(url.lpszHostName, url.dwHostNameLength);
        Handle session(WinHttpOpen(L"qoder-usage", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        if (!session) return false;
        WinHttpSetTimeouts(session, kHttpTimeoutMs, kHttpTimeoutMs, kHttpTimeoutMs, kHttpTimeoutMs);
        Handle connection(WinHttpConnect(session, host.c_str(), url.nPort, 0));
        if (!connection) return false;
        Handle request(WinHttpOpenRequest(connection, L"GET", path, nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
        if (!request) return false;
        std::wstring headers = L"Authorization: Bearer " + token + L"\r\nAccept: application/json\r\n";
        const BOOL sent = WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(headers.size()),
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
        SecureZeroMemory(headers.data(), headers.size() * sizeof(wchar_t));
        if (!sent || !WinHttpReceiveResponse(request, nullptr)) return false;
        DWORD status = 0, status_size = sizeof(status);
        if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX)
            || status < 200 || status >= 300) return false;
        std::string bytes;
        for (;;)
        {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available)) return false;
            if (available == 0) break;
            if (bytes.size() + available > 4 * 1024 * 1024) return false;
            const size_t old_size = bytes.size();
            bytes.resize(old_size + available);
            DWORD read = 0;
            if (!WinHttpReadData(request, bytes.data() + old_size, available, &read)) return false;
            bytes.resize(old_size + read);
        }
        if (bytes.empty()) return false;
        const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
            static_cast<int>(bytes.size()), nullptr, 0);
        if (count <= 0) return false;
        body.resize(static_cast<std::size_t>(count));
        return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
            static_cast<int>(bytes.size()), body.data(), count) == count;
    }

    // ---------- snapshot ----------

    void RefreshSnapshotWorker(QuotaProvider provider)
    {
        auto& plugin = CQoderUsagePlugin::Instance();
        const bool cn = plugin.IsChinese();

        QoderSnapshot snapshot;
        snapshot.fetched_at = Now();
        const bool china = provider == QuotaProvider::QoderChina;
        const auto& region = kRegions[china ? 1 : 0];
        snapshot.region = region.name;
        HMODULE module{};
        wchar_t module_path[32768]{};
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&TMPluginGetInstance), &module) &&
            GetModuleFileNameW(module, module_path, 32768) > 0)
        {
            const auto folder = std::filesystem::path(module_path).parent_path() / L"calendar";
            std::error_code error;
            for (std::filesystem::directory_iterator it(folder, error), end; !error && it != end; it.increment(error))
            {
                if (!it->is_regular_file(error) || it->path().extension() != L".txt") continue;
                const auto year = it->path().stem().u8string();
                if (year.size() != 4 || !std::all_of(year.begin(), year.end(), [](char ch) { return ch >= '0' && ch <= '9'; })) continue;
                std::ifstream file(it->path());
                if (file) snapshot.calendar.ReadYear(file, std::stoi(year));
            }
        }

        QoderDesktopAuth::Credential credential;
        if (!QoderDesktopAuth::LoadDesktopCredential(credential, QoderEdition(provider)))
        {
            snapshot.status = std::wstring(ProviderName(provider)) + (cn ? L" 未找到桌面登录凭据" : L" desktop credentials not found");
            plugin.PublishSnapshot(snapshot, provider);
            return;
        }
        snapshot.credential_label = credential.origin + L" / auth.v1.dat";
        std::wstring body;
        // Each visible item has one credential source and one destination. Never retry a
        // token against the other region or let the legacy Region setting swap accounts.
        const bool success = HttpGet(region.base, credential.token, kUsagePath, body);
        if (!success)
        {
            snapshot.status = cn ? L"Qoder 接口请求失败" : L"Qoder request failed";
        }
        else
        {
            JsonValue root;
            if (!ParseJson(body, root))
            {
                snapshot.status = cn ? L"响应解析失败" : L"Failed to parse response";
            }
            else if (!ParseUsage(root, snapshot.usage))
            {
                snapshot.status = cn ? L"不支持的响应格式" : L"Unsupported response";
            }
            else if (snapshot.usage.enterprise)
            {
                snapshot.status = cn ? L"企业账户，请用网页查看" : L"Enterprise account, view in browser";
            }
            else if (!snapshot.usage.rows.empty())
            {
                snapshot.has_data = true;
                snapshot.status = cn ? L"已连接" : L"Connected";
            }
            else
                snapshot.status = cn ? L"账号未返回额度数据" : L"No quota data returned";
        }
        SecureZeroMemory(credential.token.data(), credential.token.size() * sizeof(wchar_t));
        plugin.PublishSnapshot(snapshot, provider);
    }

    // ---------- drawing ----------

    int ScaleFromHeight(HDC dc, int n)
    {
        TEXTMETRICW metrics{};
        GetTextMetricsW(dc, &metrics);
        return MulDiv(n, (std::max)(16L, metrics.tmHeight), 16);
    }

    int CompactSessionBarWidth(HDC dc)
    {
        const int compact_width = ScaleFromHeight(dc, 69);
        constexpr int session_gap = 1;
        constexpr int weekly_gap = 1;
        for (int delta = 0; delta <= 35; ++delta)
        {
            const int candidates[] = { compact_width + delta, compact_width - delta };
            for (const int candidate : candidates)
            {
                if (candidate > 0 && (candidate - 9 * session_gap) % 10 == 0 &&
                    (candidate - 13 * weekly_gap) % 14 == 0)
                    return candidate;
            }
        }
        return compact_width;
    }

    void DrawText(HDC dc, const RECT& rect, const std::wstring& text, COLORREF color, UINT align = DT_LEFT)
    {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, color);
        RECT copy = rect;
        ::DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &copy,
            DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | align);
    }

    std::wstring FormatNumber(double value)
    {
        if (!std::isfinite(value)) return L"--";
        if (std::abs(value - std::round(value)) < 0.001)
            return std::to_wstring(static_cast<long long>(std::round(value)));
        std::wostringstream out;
        out << std::fixed << std::setprecision(1) << value;
        return out.str();
    }

    std::wstring Countdown(long long target)
    {
        if (target <= 0) return L"--";
        const long long seconds = (std::max)(0LL, target - Now());
        if (seconds < 60) return std::to_wstring(seconds) + L"s";
        if (seconds < 3600)
            return std::to_wstring(seconds / 60) + L"m" + std::to_wstring(seconds % 60) + L"s";
        const long long days = seconds / 86400;
        const long long hours = (seconds % 86400) / 3600;
        if (days > 0)
            return std::to_wstring(days) + L"d " + std::to_wstring(hours) + L"h";
        const long long minutes = (seconds % 3600) / 60;
        return std::to_wstring(hours) + L"h " + std::to_wstring(minutes) + L"m";
    }

    std::wstring TruncateForWidth(const std::wstring& text, bool /*chinese*/)
    {
        if (text.empty()) return text;
        if (text.size() <= 6) return text;
        return text.substr(0, 5) + L"…";
    }

    std::wstring QuotaLabel(QoderQuotaKind kind, bool cn)
    {
        switch (kind)
        {
        case QoderQuotaKind::User:   return cn ? L"用户额度"   : L"User";
        case QoderQuotaKind::AddOn:  return cn ? L"附加额度"   : L"Add-on";
        case QoderQuotaKind::Org:    return cn ? L"组织额度"   : L"Org";
        case QoderQuotaKind::Package: return cn ? L"专属包"     : L"Pack";
        }
        return {};
    }

    void DrawQuotaRow(HDC dc, const QoderOptions& options, const QoderQuotaRow& row, int x, int y,
        int w, int h, bool dark)
    {
        const bool cn = CQoderUsagePlugin::Instance().IsChinese();
        const auto sc = [&](int n) { return ScaleFromHeight(dc, n); };
        const COLORREF foreground = dark ? RGB(235, 238, 242) : RGB(45, 49, 54);
        const double remaining = (std::clamp)(100.0 - row.quota.used_percentage, 0.0, 100.0);
        const COLORREF filled = remaining <= 20.0
            ? (dark ? RGB(242, 139, 130) : RGB(197, 34, 31))
            : remaining <= 50.0
                ? (dark ? RGB(253, 214, 99) : RGB(227, 116, 0))
                : (dark ? RGB(129, 201, 149) : RGB(24, 128, 56));
        const COLORREF empty = dark ? RGB(82, 91, 101) : RGB(194, 202, 211);

        const std::wstring label_text = row.label.empty() ? QuotaLabel(row.kind, cn) : TruncateForWidth(row.label, cn);
        SIZE label_size{};
        GetTextExtentPoint32W(dc, label_text.c_str(), static_cast<int>(label_text.size()), &label_size);
        const int left = x + sc(2);
        RECT label{ left, y, left + label_size.cx + sc(4), y + h };
        DrawText(dc, label, label_text, foreground);

        const int bar_x = label.right + sc(2);
        const int bar_width = sc(80);
        const int segments = options.bar_style == 0 ? 10 : 1;
        const int gap = options.bar_style == 0 ? sc(1) : 0;
        const int segment_width = (bar_width - (segments - 1) * gap) / segments;
        const double filled_segments = remaining * segments / 100.0;
        const int bar_height = (std::max)(2, sc(11));
        const int vertical_offset = (std::max)(0, (h - bar_height) / 2);
        const int top = y + vertical_offset;
        for (int i = 0; i < segments; ++i)
        {
            RECT bar{ bar_x + i * (segment_width + gap), top,
                bar_x + i * (segment_width + gap) + segment_width, top + bar_height };
            HBRUSH brush = CreateSolidBrush(empty);
            FillRect(dc, &bar, brush);
            DeleteObject(brush);
            const double covered = (std::clamp)(filled_segments - i, 0.0, 1.0);
            if (covered > 0.0)
            {
                const int fill_height = static_cast<int>(std::lround(bar_height * covered));
                RECT fill = options.bar_style == 0
                    ? RECT{ bar.left, bar.bottom - fill_height, bar.right, bar.bottom }
                    : RECT{ bar.left, bar.top, bar.left + static_cast<int>(std::lround(segment_width * covered)), bar.bottom };
                brush = CreateSolidBrush(filled);
                FillRect(dc, &fill, brush);
                DeleteObject(brush);
            }
        }

        const int value_x = bar_x + bar_width + sc(6);
        std::wstring summary;
        if (row.quota.total > 0.0)
            summary = FormatNumber(row.quota.remaining) + L"/" + FormatNumber(row.quota.total);
        else
            summary = L"--";
        if (cn) summary = L"剩 " + summary;
        SIZE value_size{};
        GetTextExtentPoint32W(dc, summary.c_str(), static_cast<int>(summary.size()), &value_size);
        RECT value{ value_x, y, value_x + value_size.cx + sc(2), y + h };
        DrawText(dc, value, summary, foreground);

        const int badge_x = value.right + sc(4);
        if (badge_x + sc(40) > x + w) return;
        std::wstring countdown;
        if (row.quota.expires_at > 0)
            countdown = Countdown(row.quota.expires_at);
        if (!countdown.empty())
        {
            SIZE size{};
            GetTextExtentPoint32W(dc, countdown.c_str(), static_cast<int>(countdown.size()), &size);
            RECT countdown_rect{ badge_x, y, badge_x + size.cx + sc(2), y + h };
            DrawText(dc, countdown_rect, countdown, foreground);
        }
    }

    void DrawTotalRow(HDC dc, const QoderOptions& options, const QoderUsage& usage, int x, int y,
        int w, int h, bool dark)
    {
        QoderQuotaRow row;
        row.kind = QoderQuotaKind::User;
        row.label = CQoderUsagePlugin::Instance().IsChinese() ? L"合计用量" : L"Total";
        row.quota.used_percentage = usage.total_usage_percentage >= 0.0 ? usage.total_usage_percentage : 0.0;
        row.quota.total = 100.0;
        row.quota.remaining = (std::clamp)(100.0 - row.quota.used_percentage, 0.0, 100.0);
        DrawQuotaRow(dc, options, row, x, y, w, h, dark);
    }

    void DrawCycleRow(HDC dc, const QoderOptions& options, const QoderUsage& usage, int x, int y,
        int w, int h, bool dark)
    {
        const bool cn = CQoderUsagePlugin::Instance().IsChinese();
        const auto sc = [&](int n) { return ScaleFromHeight(dc, n); };
        const COLORREF foreground = dark ? RGB(235, 238, 242) : RGB(45, 49, 54);
        const std::wstring label = cn ? L"到期" : L"Expires";
        SIZE label_size{};
        GetTextExtentPoint32W(dc, label.c_str(), static_cast<int>(label.size()), &label_size);
        const int left = x + sc(2);
        RECT label_rect{ left, y, left + label_size.cx + sc(4), y + h };
        DrawText(dc, label_rect, label, foreground);

        const int value_x = label_rect.right + sc(2);
        const std::wstring time_text = usage.cycle_expires_at > 0 ? FormatLocalTime(usage.cycle_expires_at) : L"--";
        RECT value_rect{ value_x, y, x + w - sc(60), y + h };
        DrawText(dc, value_rect, time_text, foreground);

        const std::wstring countdown_text = usage.cycle_expires_at > 0 ? Countdown(usage.cycle_expires_at) : L"--";
        SIZE size{};
        GetTextExtentPoint32W(dc, countdown_text.c_str(), static_cast<int>(countdown_text.size()), &size);
        RECT countdown_rect{ x + w - size.cx - sc(2), y, x + w, y + h };
        DrawText(dc, countdown_rect, countdown_text, foreground);
    }

    // ---------- detail popup ----------

    std::vector<std::pair<std::wstring, std::wstring>> BuildDetailLines()
    {
        std::vector<std::pair<std::wstring, std::wstring>> lines;
        const QoderSnapshot snapshot = CQoderUsagePlugin::Instance().Snapshot(g_popup_provider);
        const bool cn = CQoderUsagePlugin::Instance().IsChinese();
        lines.emplace_back(cn ? L"连接" : L"Status", snapshot.status);
        const auto region = cn ? std::wstring(IsChina(g_popup_provider) ? L"中国（cn）" : L"国际（global）") : snapshot.region;
        lines.emplace_back(cn ? L"区域" : L"Region", region);
        lines.emplace_back(cn ? L"凭据" : L"Credential", snapshot.credential_label);
        if (snapshot.fetched_at > 0)
            lines.emplace_back(cn ? L"更新于" : L"Updated",
                FormatLocalTime(snapshot.fetched_at));
        if (!snapshot.usage.user_type.empty())
            lines.emplace_back(cn ? L"套餐" : L"Plan", snapshot.usage.user_type);
        if (snapshot.usage.quota_exceeded)
            lines.emplace_back(cn ? L"额度耗尽" : L"Quota exhausted",
                cn ? L"是" : L"Yes");
        if (snapshot.usage.total_usage_percentage >= 0.0)
            lines.emplace_back(cn ? L"合计用量" : L"Total used",
                std::to_wstring(static_cast<int>(std::lround(snapshot.usage.total_usage_percentage))) + L"%");
        if (snapshot.usage.cycle_expires_at > 0)
        {
            lines.emplace_back(cn ? L"周期到期" : L"Cycle ends",
                FormatLocalTime(snapshot.usage.cycle_expires_at) + L" (" + Countdown(snapshot.usage.cycle_expires_at) + L")");
        }
        int package_index = 0;
        for (const QoderQuotaRow& row : snapshot.usage.rows)
        {
            std::wstring label = IsWorkBuddy(g_popup_provider) && cn ? L"积分包 " + std::to_wstring(++package_index)
                : row.label.empty() ? QuotaLabel(row.kind, cn) : row.label;
            std::wstring value;
            if (row.quota.total > 0.0)
            {
                value = FormatNumber(row.quota.used) + L"/" + FormatNumber(row.quota.total)
                    + L" " + row.quota.unit
                    + L" (" + std::to_wstring(static_cast<int>(std::lround(row.quota.used_percentage))) + L"%)";
                if (row.quota.expires_at > 0)
                    value += L" · " + Countdown(row.quota.expires_at);
                if (IsWorkBuddy(g_popup_provider) && row.quota.reserved > 0)
                    value += (cn ? L" · 冻结 " : L" · Reserved ") + FormatNumber(row.quota.reserved);
            }
            lines.emplace_back(std::move(label), std::move(value));
        }
        return lines;
    }

    void HidePopupIfCursorOutside()
    {
        if (!g_detail_popup || !IsWindowVisible(g_detail_popup)) return;
        POINT cursor{};
        RECT popup_rect{};
        if (!GetCursorPos(&cursor) || !GetWindowRect(g_detail_popup, &popup_rect)) return;
        const bool outside = !PtInRect(&popup_rect, cursor);
        const bool on_anchor = std::abs(cursor.x - g_popup_anchor.x) <= 12 && std::abs(cursor.y - g_popup_anchor.y) <= 12;
        const bool clicked = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (GetAsyncKeyState(VK_RBUTTON) & 0x8000);
        if (GetTickCount64() - g_popup_opened > 600 && outside && (!on_anchor || clicked))
            ShowWindow(g_detail_popup, SW_HIDE);
    }

    LRESULT CALLBACK DetailPopupProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_SHOWWINDOW:
            if (wParam) g_tooltip_guard.Acquire(GetWindow(hwnd, GW_OWNER));
            else g_tooltip_guard.Release();
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEMOVE:
            if (!g_popup_tracking_mouse)
            {
                TRACKMOUSEEVENT tracking{ sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0 };
                g_popup_tracking_mouse = TrackMouseEvent(&tracking) != FALSE;
            }
            break;
        case WM_MOUSELEAVE:
            g_popup_tracking_mouse = false;
            HidePopupIfCursorOutside();
            return 0;
        case WM_TIMER:
            if (wParam == 2)
            {
                if (IsWindowVisible(hwnd) && (GetAsyncKeyState(VK_ESCAPE) & 0x8000)) ShowWindow(hwnd, SW_HIDE);
                HidePopupIfCursorOutside();
                return 0;
            }
            if (IsWindowVisible(hwnd)) InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_PAINT:
        {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(hwnd, &paint);
            RECT rect{};
            GetClientRect(hwnd, &rect);
            HBRUSH background = CreateSolidBrush(RGB(255, 251, 225));
            FillRect(dc, &rect, background);
            DeleteObject(background);

            const bool cn = CQoderUsagePlugin::Instance().IsChinese();
            SetBkMode(dc, TRANSPARENT);
            const int line_height = MulDiv(22, g_popup_dpi, 96);
            const int pad = MulDiv(16, g_popup_dpi, 96);

            HFONT title_font = CreateFontW(MulDiv(18, g_popup_dpi, 96), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            HGDIOBJ old_font = SelectObject(dc, title_font);
            RECT button = PluginAppButton::Bounds(hwnd, g_popup_dpi);
            DrawFrameControl(dc, &button, DFC_BUTTON, DFCS_BUTTONPUSH);
            SetTextColor(dc, RGB(45, 49, 54));
            const auto button_text = std::wstring(cn ? L"打开 " : L"Open ") + ProviderName(g_popup_provider);
            ::DrawTextW(dc, button_text.c_str(), -1, &button, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            RECT title{ pad, MulDiv(10, g_popup_dpi, 96), rect.right - pad, MulDiv(38, g_popup_dpi, 96) };
            SetTextColor(dc, RGB(46, 48, 52));
            const std::wstring title_text = std::wstring(ProviderName(g_popup_provider))
                + (cn ? (IsWorkBuddy(g_popup_provider) ? L" 积分详情" : L" 用量详情") : L" usage details");
            ::DrawTextW(dc, title_text.c_str(), -1, &title, DT_SINGLELINE | DT_VCENTER);
            SelectObject(dc, old_font);
            DeleteObject(title_font);

            HFONT body_font = CreateFontW(-MulDiv(13, g_popup_dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            old_font = SelectObject(dc, body_font);

            const auto entries = BuildDetailLines();
            const int top = MulDiv(46, g_popup_dpi, 96);
            const int label_width = rect.right * 35 / 100;
            for (size_t i = 0; i < entries.size(); ++i)
            {
                const int row_top = top + static_cast<int>(i) * line_height;
                if (row_top + line_height > rect.bottom - MulDiv(8, g_popup_dpi, 96)) break;
                RECT label_rect{ pad, row_top, label_width, row_top + line_height };
                SetTextColor(dc, RGB(60, 64, 70));
                ::DrawTextW(dc, entries[i].first.c_str(), -1, &label_rect,
                    DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
                RECT value_rect{ label_width, row_top, rect.right - pad, row_top + line_height };
                SetTextColor(dc, RGB(32, 35, 40));
                ::DrawTextW(dc, entries[i].second.c_str(), -1, &value_rect,
                    DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            }
            SelectObject(dc, old_font);
            DeleteObject(body_font);

            EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_LBUTTONUP:
        {
            const POINT point{ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
            const RECT button = PluginAppButton::Bounds(hwnd, g_popup_dpi);
            if (PtInRect(&button, point))
            {
                const auto path = ApplicationPath(g_popup_provider);
                if (path.empty() || reinterpret_cast<INT_PTR>(ShellExecuteW(hwnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
                {
                    const auto error_text = std::wstring(ProviderName(g_popup_provider)) + L" 无法打开，请确认已安装。";
                    MessageBoxW(hwnd, error_text.c_str(), L"TrafficMonitor", MB_OK | MB_ICONWARNING);
                }
                else ShowWindow(hwnd, SW_HIDE);
                return 0;
            }
        }
        {
            RECT rect{}; GetClientRect(hwnd, &rect);
            const int side = MulDiv(32, g_popup_dpi, 96);
            if (static_cast<short>(LOWORD(lParam)) >= rect.right - side && static_cast<short>(HIWORD(lParam)) < side)
                ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        case WM_CLOSE:
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        case WM_DESTROY:
            g_tooltip_guard.Release();
            KillTimer(hwnd, 1);
            KillTimer(hwnd, 2);
            if (g_detail_popup == hwnd) g_detail_popup = nullptr;
            g_popup_tracking_mouse = false;
            return 0;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    void DestroyDetailPopup()
    {
        if (g_detail_popup && IsWindow(g_detail_popup)) DestroyWindow(g_detail_popup);
        g_detail_popup = nullptr;
        if (g_popup_instance) UnregisterClassW(kDetailPopupClass, g_popup_instance);
        g_popup_instance = nullptr;
    }

    void ShowDetailPopup(HWND owner, QuotaProvider provider = QuotaProvider::Qoder)
    {
        if (owner == nullptr || !IsWindow(owner)) return;
        if (g_detail_popup && IsWindowVisible(g_detail_popup) && g_popup_provider == provider)
        {
            ShowWindow(g_detail_popup, SW_HIDE);
            return;
        }
        g_popup_provider = provider;
        GetCursorPos(&g_popup_anchor);
        g_popup_opened = GetTickCount64();
        if (!g_detail_popup)
        {
            if (!g_popup_instance)
                GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(reinterpret_cast<ULONG_PTR>(&DetailPopupProc)), &g_popup_instance);
            if (!g_popup_instance) g_popup_instance = GetModuleHandleW(nullptr);
            WNDCLASSEXW wc{ sizeof(WNDCLASSEXW) };
            wc.lpfnWndProc = DetailPopupProc;
            wc.hInstance = g_popup_instance;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hbrBackground = nullptr;
            wc.lpszClassName = kDetailPopupClass;
            if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;

            RECT owner_rect{};
            GetWindowRect(owner, &owner_rect);
            HMONITOR monitor = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
            MONITORINFO monitor_info{ sizeof(MONITORINFO) };
            GetMonitorInfoW(monitor, &monitor_info);
            g_popup_dpi = static_cast<int>(GetDpiForWindow(owner));
            if (g_popup_dpi <= 0) g_popup_dpi = 96;
            const int width = (std::min)(MulDiv(560, g_popup_dpi, 96),
                static_cast<int>(monitor_info.rcWork.right - monitor_info.rcWork.left));
            const int height = (std::min)(MulDiv(420, g_popup_dpi, 96),
                static_cast<int>(monitor_info.rcWork.bottom - monitor_info.rcWork.top));
            int left = owner_rect.left;
            int top = owner_rect.top - height - 4;
            if (top < monitor_info.rcWork.top) top = owner_rect.bottom + 4;
            const int work_left = static_cast<int>(monitor_info.rcWork.left);
            const int work_top = static_cast<int>(monitor_info.rcWork.top);
            const int work_right = static_cast<int>(monitor_info.rcWork.right);
            const int work_bottom = static_cast<int>(monitor_info.rcWork.bottom);
            left = (std::clamp)(left, work_left, (std::max)(work_left, work_right - width));
            top = (std::clamp)(top, work_top, (std::max)(work_top, work_bottom - height));

            g_detail_popup = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                kDetailPopupClass, L"Qoder usage", WS_POPUP | WS_BORDER,
                left, top, width, height, owner, nullptr, wc.hInstance, nullptr);
            if (!g_detail_popup) return;
            SetTimer(g_detail_popup, 1, 1000, nullptr);
            SetTimer(g_detail_popup, 2, 100, nullptr);
        }
        SetWindowPos(g_detail_popup, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        ShowWindow(g_detail_popup, SW_SHOWNOACTIVATE);
        InvalidateRect(g_detail_popup, nullptr, FALSE);
    }

    // ---------- options dialog ----------

    std::wstring OptionsSection(const QoderOptions& options)
    {
        std::wstring section;
        const auto add = [&](const wchar_t* key, int value) {
            section += std::wstring(key) + L"=" + std::to_wstring(value);
            section.push_back(L'\0');
        };
        add(L"PollSeconds", options.poll_seconds);
        add(L"Theme", options.theme);
        add(L"BarStyle", options.bar_style);
        add(L"Region", options.region);
        add(L"AlertThreshold", options.alert_threshold);
        add(L"ShowTotal", options.show_total);
        add(L"ShowCycle", options.show_cycle);
        add(L"RestSchedule", options.rest_schedule);
        add(L"MorningStart", options.morning_start);
        add(L"MorningEnd", options.morning_end);
        add(L"AfternoonStart", options.afternoon_start);
        add(L"AfternoonEnd", options.afternoon_end);
        section.push_back(L'\0');
        return section;
    }

    bool OptionsEqual(const QoderOptions& a, const QoderOptions& b)
    {
        return OptionsSection(a) == OptionsSection(b);
    }

    INT_PTR CALLBACK OptionsDialogProc(HWND dialog, UINT message, WPARAM wp, LPARAM)
    {
        const bool cn = CQoderUsagePlugin::Instance().IsChinese();
        if (message == WM_INITDIALOG)
        {
            const QoderOptions options = CQoderUsagePlugin::Instance().Options();
            const auto combo = [&](int id, std::initializer_list<const wchar_t*> labels, int selected) {
                for (const wchar_t* label : labels)
                    SendDlgItemMessageW(dialog, id, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
                SendDlgItemMessageW(dialog, id, CB_SETCURSEL, selected, 0);
            };
            combo(IDC_QODER_POLL,
                cn ? std::initializer_list<const wchar_t*>{ L"1 分钟", L"5 分钟", L"15 分钟", L"1 小时" }
                   : std::initializer_list<const wchar_t*>{ L"1 minute", L"5 minutes", L"15 minutes", L"1 hour" },
                options.poll_seconds == 300 ? 1 : options.poll_seconds == 900 ? 2 : options.poll_seconds == 3600 ? 3 : 0);
            combo(IDC_QODER_THEME,
                cn ? std::initializer_list<const wchar_t*>{ L"跟随 TrafficMonitor", L"浅色", L"深色" }
                   : std::initializer_list<const wchar_t*>{ L"Follow TrafficMonitor", L"Light", L"Dark" },
                options.theme);
            combo(IDC_QODER_BAR,
                cn ? std::initializer_list<const wchar_t*>{ L"分段", L"连续" }
                   : std::initializer_list<const wchar_t*>{ L"Segmented", L"Continuous" },
                options.bar_style);
            combo(IDC_QODER_REGION,
                cn ? std::initializer_list<const wchar_t*>{ L"按显示项固定区域" }
                   : std::initializer_list<const wchar_t*>{ L"Fixed per display item" }, 0);
            EnableWindow(GetDlgItem(dialog, IDC_QODER_REGION), FALSE);
            combo(IDC_QODER_ALERT,
                cn ? std::initializer_list<const wchar_t*>{ L"关", L"50%", L"25%", L"10%" }
                   : std::initializer_list<const wchar_t*>{ L"Off", L"50%", L"25%", L"10%" },
                options.alert_threshold);
            combo(IDC_QODER_REST_SCHEDULE, cn
                ? std::initializer_list<const wchar_t*>{ L"双休（周六、周日休）", L"单休（周日休）", L"单双轮休 A（A 周双休）", L"单双轮休 B（B 周双休）" }
                : std::initializer_list<const wchar_t*>{ L"Two days off (Sat/Sun)", L"One day off (Sun)", L"Alternating A (A week: Sat/Sun off)", L"Alternating B (B week: Sat/Sun off)" },
                options.rest_schedule);
            CheckDlgButton(dialog, IDC_QODER_TOTAL, options.show_total ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(dialog, IDC_QODER_CYCLE, options.show_cycle ? BST_CHECKED : BST_UNCHECKED);
            SetDlgItemTextW(dialog, IDC_QODER_MORNING_START, FormatClock(options.morning_start).c_str());
            SetDlgItemTextW(dialog, IDC_QODER_MORNING_END, FormatClock(options.morning_end).c_str());
            SetDlgItemTextW(dialog, IDC_QODER_AFTERNOON_START, FormatClock(options.afternoon_start).c_str());
            SetDlgItemTextW(dialog, IDC_QODER_AFTERNOON_END, FormatClock(options.afternoon_end).c_str());

            if (cn)
            {
                const std::pair<int, const wchar_t*> labels[] = {
                    { IDC_QODER_POLL_LABEL, L"刷新频率" },
                    { IDC_QODER_THEME_LABEL, L"主题" },
                    { IDC_QODER_BAR_LABEL, L"额度进度条" },
                    { IDC_QODER_REGION_LABEL, L"API 区域" },
                    { IDC_QODER_ALERT_LABEL, L"低额度预警" },
                    { IDC_QODER_TOTAL, L"显示合计用量" },
                    { IDC_QODER_CYCLE, L"显示到期倒计时" },
                    { IDC_QODER_SCHEDULE_LABEL, L"每周休息安排" },
                    { IDC_QODER_MORNING_LABEL, L"上午时段" },
                    { IDC_QODER_AFTERNOON_LABEL, L"下午时段" },
                    { IDC_QODER_MORNING_TO, L"至" },
                    { IDC_QODER_AFTERNOON_TO, L"至" },
                    { IDC_QODER_HINT, L"窗口位置和背景由 TrafficMonitor 控制。" },
                    { IDOK, L"确定" }, { IDCANCEL, L"取消" },
                };
                for (const auto& label : labels) SetDlgItemTextW(dialog, label.first, label.second);
                SetWindowTextW(dialog, L"Qoder Usage 插件选项");
            }
            RECT bounds{}, work_area{};
            GetWindowRect(dialog, &bounds);
            const HWND owner = GetParent(dialog);
            const HMONITOR monitor = MonitorFromWindow(owner ? owner : dialog, MONITOR_DEFAULTTONEAREST);
            MONITORINFO monitor_info{ sizeof(monitor_info) };
            if (monitor && GetMonitorInfoW(monitor, &monitor_info))
                work_area = monitor_info.rcWork;
            else if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0))
                work_area = RECT{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };

            const LONG dialog_width = bounds.right - bounds.left;
            const LONG dialog_height = bounds.bottom - bounds.top;
            const LONG max_left = (std::max)(work_area.left, work_area.right - dialog_width);
            const LONG max_top = (std::max)(work_area.top, work_area.bottom - dialog_height);
            const LONG left = (std::clamp)(work_area.left + (work_area.right - work_area.left - dialog_width) / 2,
                work_area.left, max_left);
            const LONG top = (std::clamp)(work_area.top + (work_area.bottom - work_area.top - dialog_height) / 2,
                work_area.top, max_top);
            SetWindowPos(dialog, nullptr, left, top, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
            return TRUE;
        }
        if (message == WM_COMMAND)
        {
            if (LOWORD(wp) == IDCANCEL) { EndDialog(dialog, IDCANCEL); return TRUE; }
            if (LOWORD(wp) == IDOK)
            {
                QoderOptions options = CQoderUsagePlugin::Instance().Options();
                const int intervals[] = { 60, 300, 900, 3600 };
                options.poll_seconds = intervals[(std::clamp)(
                    static_cast<int>(SendDlgItemMessageW(dialog, IDC_QODER_POLL, CB_GETCURSEL, 0, 0)), 0, 3)];
                options.theme = (std::clamp)(
                    static_cast<int>(SendDlgItemMessageW(dialog, IDC_QODER_THEME, CB_GETCURSEL, 0, 0)), 0, 2);
                options.bar_style = (std::clamp)(
                    static_cast<int>(SendDlgItemMessageW(dialog, IDC_QODER_BAR, CB_GETCURSEL, 0, 0)), 0, 1);
                options.alert_threshold = (std::clamp)(
                    static_cast<int>(SendDlgItemMessageW(dialog, IDC_QODER_ALERT, CB_GETCURSEL, 0, 0)), 0, 3);
                options.show_total = IsDlgButtonChecked(dialog, IDC_QODER_TOTAL) == BST_CHECKED;
                options.show_cycle = IsDlgButtonChecked(dialog, IDC_QODER_CYCLE) == BST_CHECKED;
                options.rest_schedule = static_cast<int>(SendDlgItemMessageW(dialog, IDC_QODER_REST_SCHEDULE, CB_GETCURSEL, 0, 0));
                if (options.rest_schedule < 0 || options.rest_schedule > 3 ||
                    !ParseClock(dialog, IDC_QODER_MORNING_START, options.morning_start) ||
                    !ParseClock(dialog, IDC_QODER_MORNING_END, options.morning_end) ||
                    !ParseClock(dialog, IDC_QODER_AFTERNOON_START, options.afternoon_start) ||
                    !ParseClock(dialog, IDC_QODER_AFTERNOON_END, options.afternoon_end))
                {
                    MessageBoxW(dialog, cn ? L"请输入有效时间，格式为 HH:mm。" : L"Enter valid times in HH:mm format.",
                        L"Qoder Usage", MB_OK | MB_ICONINFORMATION);
                    return TRUE;
                }
                if (options.morning_start >= options.morning_end || options.morning_end > options.afternoon_start ||
                    options.afternoon_start >= options.afternoon_end)
                {
                    MessageBoxW(dialog, cn ? L"请确保上午开始 < 上午结束 ≤ 下午开始 < 下午结束。" :
                        L"Times must satisfy morning start < morning end ≤ afternoon start < afternoon end.",
                        L"Qoder Usage", MB_OK | MB_ICONINFORMATION);
                    return TRUE;
                }
                if (OptionsEqual(options, CQoderUsagePlugin::Instance().Options()))
                    EndDialog(dialog, IDCANCEL);
                else if (CQoderUsagePlugin::Instance().SaveOptions(options))
                    EndDialog(dialog, IDOK);
                else
                    MessageBoxW(dialog, cn ? L"无法保存配置,请检查配置目录是否可写。" : L"Unable to save settings. Check the configuration directory is writable.",
                        L"Qoder Usage", MB_OK | MB_ICONERROR);
                return TRUE;
            }
        }
        if (message == WM_CLOSE) { EndDialog(dialog, IDCANCEL); return TRUE; }
        return FALSE;
    }
}

// --------- CQoderUsagePlugin ----------

CQoderUsagePlugin& CQoderUsagePlugin::Instance()
{
    static CQoderUsagePlugin instance;
    return instance;
}

IPluginItem* CQoderUsagePlugin::GetItem(int index)
{
    if (index == 0) return &m_item;
    if (index == 1) return &m_qoder_cn_item;
    if (index == 2) return &m_workbuddy_intl_item;
    if (index == 3) return &m_workbuddy_item;
    return nullptr;
}

void CQoderUsagePlugin::DataRequired()
{
    // The worker owns network calls. Keep the host's sampling callback cheap.
}

const wchar_t* CQoderUsagePlugin::GetInfo(PluginInfoIndex index)
{
    switch (index)
    {
    case TMI_NAME: return L"Qoder Usage";
    case TMI_DESCRIPTION: return L"Qoder quota and WorkBuddy desktop credit monitoring";
    case TMI_AUTHOR: return L"Qoder Usage Monitor contributors";
    case TMI_VERSION: return L"0.4.0";
    case TMI_URL: return L"https://github.com/wang-yichun/qoder-usage-monitor";
    default: return L"";
    }
}

void CQoderUsagePlugin::OnInitialize(ITrafficMonitor* app)
{
    m_app = app;
    if (app != nullptr)
    {
        const LANGID language = app->GetLanguageId();
        m_is_chinese = PRIMARYLANGID(language == 0 ? GetUserDefaultUILanguage() : language) == LANG_CHINESE;
    }
    if (app != nullptr && app->GetPluginConfigDir())
    {
        m_config_path = (std::filesystem::path(app->GetPluginConfigDir()) / L"QoderUsage.ini").wstring();
        LoadConfig();
    }
    m_stop.store(false);
    m_refresh_requested.store(false);
    if (!m_worker.joinable()) m_worker = std::thread(&CQoderUsagePlugin::PollLoop, this);
}

void CQoderUsagePlugin::OnShutdown()
{
    m_stop.store(true);
    m_worker_cv.notify_all();
    if (m_worker.joinable()) m_worker.join();
    DestroyDetailPopup();
    m_app = nullptr;
}

void CQoderUsagePlugin::PollLoop()
{
    while (!m_stop.load())
    {
        RefreshSnapshotWorker(QuotaProvider::Qoder);
        if (m_stop.load()) break;
        RefreshSnapshotWorker(QuotaProvider::QoderChina);
        if (m_stop.load()) break;
        for (const auto provider : { QuotaProvider::WorkBuddyChina, QuotaProvider::WorkBuddyInternational })
        {
            if (m_stop.load()) break;
            QoderSnapshot workbuddy;
            workbuddy.calendar = Snapshot().calendar;
            WorkBuddyUsage::Refresh(workbuddy, IsChinese(), BuddyEdition(provider));
            workbuddy.fetched_at = Now();
            PublishSnapshot(std::move(workbuddy), provider);
        }
        std::unique_lock<std::mutex> lock(m_worker_mutex);
        const int seconds = Options().poll_seconds;
        m_worker_cv.wait_for(lock, std::chrono::seconds(seconds), [this] {
            return m_stop.load() || m_refresh_requested.exchange(false);
        });
        if (m_stop.load()) break;
    }
}

QoderSnapshot CQoderUsagePlugin::Snapshot(QuotaProvider provider) const
{
    std::lock_guard<std::mutex> lock(m_snapshot_mutex);
    return m_provider_states.at(static_cast<size_t>(provider)).snapshot;
}

void CQoderUsagePlugin::PublishSnapshot(QoderSnapshot snapshot, QuotaProvider provider)
{
    std::lock_guard<std::mutex> lock(m_snapshot_mutex);
    const bool workbuddy = IsWorkBuddy(provider);
    const wchar_t* history_file = provider == QuotaProvider::WorkBuddyInternational ? L"WorkBuddyInternationalUsage.history"
        : provider == QuotaProvider::WorkBuddyChina ? L"WorkBuddyUsage.history"
        : provider == QuotaProvider::QoderChina ? L"QoderCNUsage.history" : L"QoderUsage.history";
    const auto path = m_config_path.empty() ? std::filesystem::path{} :
        std::filesystem::path(m_config_path).parent_path() / history_file;
    auto& state = m_provider_states.at(static_cast<size_t>(provider));
    auto& history = state.history;
    auto& history_total = state.history_total;
    auto& history_identity = state.history_identity;
    auto& history_loaded = state.history_loaded;
    const auto identity = snapshot.history_identity.empty() ? snapshot.region : snapshot.history_identity;
    if (!history_loaded)
    {
        history_loaded = true;
        std::ifstream input(path);
        std::string version, region;
        CodexRecent::Window saved_history;
        double total{};
        if (input >> version >> region >> total && version == "v1" &&
            std::isfinite(total) && total > 0 && saved_history.Read(input))
        {
            history = std::move(saved_history);
            history_total = total;
            history_identity.assign(region.begin(), region.end());
        }
    }
    if (snapshot.has_data)
    {
        double total = 0, remaining = 0;
        for (const auto& row : snapshot.usage.rows)
            if (row.quota.available && row.quota.total > 0)
            {
                total += row.quota.total;
                remaining += workbuddy ? (std::max)(0.0, row.quota.remaining)
                    : (std::clamp)(row.quota.remaining, 0.0, row.quota.total);
            }
        if (total > 0)
        {
            if (history_identity != identity || std::abs(history_total - total) > 0.001)
                history = {};
            history_total = total;
            history_identity = identity;
            const long long now = snapshot.fetched_at;
            const long long reset = snapshot.usage.cycle_expires_at > now ? snapshot.usage.cycle_expires_at : now + 10LL * 365 * 86400;
            history.Record(now, (std::max)(0.0, 100.0 * (total - remaining) / total), reset, 30LL * 86400);
            history.Prune(now);
            snapshot.recent_percent = history.Recent(now, 86400);
            snapshot.recent_buckets = history.RecentBuckets(now, 86400);
            if (!path.empty())
            {
                std::error_code error;
                std::filesystem::create_directories(path.parent_path(), error);
                const auto temp = std::filesystem::path(path.wstring() + L".tmp");
                std::ofstream output(temp, std::ios::trunc);
                std::string saved_identity;
                for (wchar_t ch : history_identity) saved_identity.push_back(static_cast<char>(ch));
                output << "v1 " << saved_identity << ' '
                    << std::setprecision(17) << history_total << '\n';
                history.Write(output);
                output.close();
                if (output) MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
            }
        }
    }
    state.snapshot = std::move(snapshot);
}

QoderOptions CQoderUsagePlugin::Options() const
{
    std::lock_guard<std::mutex> lock(m_options_mutex);
    return m_options;
}

bool CQoderUsagePlugin::SaveOptions(const QoderOptions& options)
{
    if (m_config_path.empty()) return false;
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(m_config_path).parent_path(), error);
    if (error) return false;
    const auto section = OptionsSection(options);
    if (!WritePrivateProfileSectionW(L"Options", section.c_str(), m_config_path.c_str())) return false;
    {
        std::lock_guard<std::mutex> lock(m_options_mutex);
        m_options = options;
    }
    RequestRefresh();
    return true;
}

void CQoderUsagePlugin::LoadConfig()
{
    const auto read = [&](const wchar_t* key, int fallback) {
        return static_cast<int>(GetPrivateProfileIntW(L"Options", key, fallback, m_config_path.c_str()));
    };
    QoderOptions options;
    options.poll_seconds = read(L"PollSeconds", 300);
    if (options.poll_seconds != 60 && options.poll_seconds != 300 &&
        options.poll_seconds != 900 && options.poll_seconds != 3600) options.poll_seconds = 300;
    options.theme = (std::clamp)(read(L"Theme", 0), 0, 2);
    options.bar_style = (std::clamp)(read(L"BarStyle", 0), 0, 1);
    options.region = (std::clamp)(read(L"Region", 0), 0, 2);
    options.alert_threshold = (std::clamp)(read(L"AlertThreshold", 0), 0, 3);
    options.show_total = read(L"ShowTotal", 1) != 0;
    options.show_cycle = read(L"ShowCycle", 1) != 0;
    options.rest_schedule = (std::clamp)(read(L"RestSchedule", 0), 0, 3);
    options.morning_start = read(L"MorningStart", 9 * 60 + 30);
    options.morning_end = read(L"MorningEnd", 12 * 60);
    options.afternoon_start = read(L"AfternoonStart", 13 * 60 + 30);
    options.afternoon_end = read(L"AfternoonEnd", 18 * 60 + 30);
    const auto valid_minute = [](int minute) { return minute >= 0 && minute < 24 * 60; };
    if (!valid_minute(options.morning_start) || !valid_minute(options.morning_end) ||
        !valid_minute(options.afternoon_start) || !valid_minute(options.afternoon_end) ||
        options.morning_start >= options.morning_end || options.morning_end > options.afternoon_start ||
        options.afternoon_start >= options.afternoon_end)
    {
        options.morning_start = 9 * 60 + 30; options.morning_end = 12 * 60;
        options.afternoon_start = 13 * 60 + 30; options.afternoon_end = 18 * 60 + 30;
    }
    std::lock_guard<std::mutex> lock(m_options_mutex);
    m_options = options;
}

void CQoderUsagePlugin::RequestRefresh()
{
    m_refresh_requested.store(true);
    m_worker_cv.notify_all();
}

const wchar_t* CQoderUsagePlugin::GetTooltipInfo()
{
    thread_local std::wstring result;
    result.clear();
    for (const auto provider : { QuotaProvider::Qoder, QuotaProvider::QoderChina,
        QuotaProvider::WorkBuddyInternational, QuotaProvider::WorkBuddyChina })
    {
        const auto snapshot = Snapshot(provider);
        if (!result.empty()) result += L"\n";
        result += std::wstring(ProviderName(provider)) + L": " + snapshot.status;
        if (snapshot.has_data)
        {
            double remaining = 0, total = 0;
            for (const auto& row : snapshot.usage.rows)
            {
                if (!row.quota.available || row.quota.total <= 0) continue;
                remaining += IsWorkBuddy(provider) ? row.quota.remaining : (std::clamp)(row.quota.remaining, 0.0, row.quota.total);
                total += row.quota.total;
            }
            result += L" | " + FormatNumber(remaining) + L"/" + FormatNumber(total);
        }
    }
    return result.c_str();
}

const wchar_t* CQoderUsagePlugin::GetCommandName(int command_index)
{
    const bool cn = IsChinese();
    if (command_index == 0) return cn ? L"显示 Qoder 用量详情" : L"Show Qoder usage details";
    if (command_index == 1) return cn ? L"立即刷新全部用量" : L"Refresh all usage now";
    if (command_index == 2) return cn ? L"在浏览器中打开用量详情" : L"Open usage page in browser";
    if (command_index == 3) return cn ? L"显示 WorkBuddyCN 积分详情" : L"Show WorkBuddyCN credits";
    if (command_index == 4) return cn ? L"显示 WorkBuddy 积分详情" : L"Show WorkBuddy credits";
    if (command_index == 5) return cn ? L"显示 QoderCN 用量详情" : L"Show QoderCN usage details";
    return nullptr;
}

void CQoderUsagePlugin::OnPluginCommand(int command_index, void* hWnd, void*)
{
    if (command_index == 0)
    {
        ShowDetailPopup(static_cast<HWND>(hWnd));
    }
    else if (command_index == 1)
    {
        RequestRefresh();
    }
    else if (command_index == 2)
    {
        std::wstring url;
        {
            std::lock_guard<std::mutex> lock(m_snapshot_mutex);
            url = m_provider_states[0].snapshot.usage.detail_url;
        }
        if (!url.empty())
            ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    else if (command_index == 3)
        ShowDetailPopup(static_cast<HWND>(hWnd), QuotaProvider::WorkBuddyChina);
    else if (command_index == 4)
        ShowDetailPopup(static_cast<HWND>(hWnd), QuotaProvider::WorkBuddyInternational);
    else if (command_index == 5)
        ShowDetailPopup(static_cast<HWND>(hWnd), QuotaProvider::QoderChina);
}

ITMPlugin::OptionReturn CQoderUsagePlugin::ShowOptionsDialog(void* hParent)
{
    HMODULE module{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&TMPluginGetInstance), &module);
    const auto result = DialogBoxParamW(module, MAKEINTRESOURCEW(IDD_QODER_OPTIONS),
        static_cast<HWND>(hParent), OptionsDialogProc, 0);
    if (result == -1 && hParent != nullptr)
        MessageBoxW(static_cast<HWND>(hParent), IsChinese() ? L"无法打开插件选项。" : L"Unable to open plugin options.",
            L"Qoder Usage", MB_OK | MB_ICONERROR);
    return result == IDOK ? OR_OPTION_CHANGED : OR_OPTION_UNCHANGED;
}

// ---------- CQoderUsageItem ----------

const wchar_t* CQoderUsageItem::GetItemName() const
{
    return ProviderName(m_provider);
}

const wchar_t* CQoderUsageItem::GetItemId() const
{
    if (m_provider == QuotaProvider::QoderChina) return L"QoderCNUsageQuota";
    if (m_provider == QuotaProvider::WorkBuddyChina) return L"WorkBuddyCredits"; // preserve existing selection
    if (m_provider == QuotaProvider::WorkBuddyInternational) return L"WorkBuddyInternationalCredits";
    return L"QoderUsageQuota9";
}

const wchar_t* CQoderUsageItem::GetItemLableText() const { return L""; }
const wchar_t* CQoderUsageItem::GetItemValueText() const { return L""; }
const wchar_t* CQoderUsageItem::GetItemValueSampleText() const { return L"合计 100%"; }

namespace
{
    std::pair<double, double> CombinedCredits(const QoderSnapshot& snapshot, bool workbuddy)
    {
        double total = 0.0, remaining = 0.0;
        for (const auto& row : snapshot.usage.rows)
        {
            if (!row.quota.available || row.quota.total <= 0.0) continue;
            total += row.quota.total;
            remaining += workbuddy ? (std::max)(0.0, row.quota.remaining)
                : (std::clamp)(row.quota.remaining, 0.0, row.quota.total);
        }
        return { remaining, total };
    }

    double CombinedRemaining(const QoderSnapshot& snapshot, bool workbuddy)
    {
        const auto credits = CombinedCredits(snapshot, workbuddy);
        return snapshot.has_data ? (credits.second > 0.0 ? credits.first / credits.second * 100.0 : 0.0) : -1.0;
    }

    std::wstring RemainingPercent(const QoderSnapshot& snapshot, bool workbuddy)
    {
        const double remaining = CombinedRemaining(snapshot, workbuddy);
        return (remaining < 0.0 ? std::wstring(L"--") : std::to_wstring(static_cast<int>(std::lround((std::min)(100.0, remaining))))) + L"%";
    }

    std::wstring CompactSummary(const QoderSnapshot& snapshot, QuotaProvider provider)
    {
        const auto credits = CombinedCredits(snapshot, IsWorkBuddy(provider));
        return std::wstring(ProviderName(provider)) + L" " + (snapshot.has_data
            ? FormatNumber(credits.first) + L"/" + FormatNumber(credits.second) : L"--/--");
    }

    void DrawRecentHistogram(HDC dc, const RECT& area, const std::vector<double>& buckets)
    {
        const int width = area.right - area.left, height = area.bottom - area.top;
        if (width <= 0 || height <= 2 || buckets.empty()) return;
        const int columns = (std::min)(width, 96);
        const size_t per_column = 15;
        const size_t start = buckets.size() > static_cast<size_t>(columns) * per_column
            ? buckets.size() - static_cast<size_t>(columns) * per_column : 0;
        std::vector<double> values(columns, 0.0);
        double maximum = 0;
        for (int i = 0; i < columns; ++i)
            for (size_t j = start + i * per_column; j < (std::min)(start + (i + 1) * per_column, buckets.size()); ++j)
                maximum = (std::max)(maximum, values[i] += buckets[j]);
        if (maximum <= 0) return;
        HBRUSH brush = CreateSolidBrush(RGB(194, 202, 211));
        if (!brush) return;
        for (int i = 0; i < columns; ++i)
            if (values[i] > 0)
            {
                const int bar_height = (std::max)(1, static_cast<int>(std::lround((height - 2) * values[i] / maximum)));
                RECT bar{area.right - columns + i, area.bottom - bar_height, area.right - columns + i + 1, area.bottom};
                FillRect(dc, &bar, brush);
            }
        DeleteObject(brush);
    }

}

int CQoderUsageItem::GetItemWidthEx(void* hDC) const
{
    if (!hDC) return 320;
    HDC dc = static_cast<HDC>(hDC);
    const auto snapshot = CQoderUsagePlugin::Instance().Snapshot(m_provider);
    const auto summary = CompactSummary(snapshot, m_provider);
    SIZE size{};
    GetTextExtentPoint32W(dc, summary.c_str(), static_cast<int>(summary.size()), &size);
    {
        // The host measures once before the first asynchronous billing result arrives.
        const std::wstring sample = std::wstring(ProviderName(m_provider))
            + (IsWorkBuddy(m_provider) ? L" 88888.8/88888.8" : L" 8888.8/8888.8");
        SIZE minimum{};
        GetTextExtentPoint32W(dc, sample.c_str(), static_cast<int>(sample.size()), &minimum);
        size.cx = (std::max)(size.cx, minimum.cx);
    }
    SIZE percent_size{};
    GetTextExtentPoint32W(dc, L"100%", 4, &percent_size);
    return (std::max)(static_cast<int>(size.cx),
        CompactSessionBarWidth(dc) + ScaleFromHeight(dc, 8) + static_cast<int>(percent_size.cx))
        + ScaleFromHeight(dc, 4);
}

void CQoderUsageItem::DrawItem(void* hDC, int x, int y, int w, int h, bool dark_mode)
{
    if (!hDC || w <= 0 || h <= 0) return;
    HDC dc = static_cast<HDC>(hDC);
    const int saved = SaveDC(dc);
    IntersectClipRect(dc, x, y, x + w, y + h);
    const auto options = CQoderUsagePlugin::Instance().Options();
    const auto snapshot = CQoderUsagePlugin::Instance().Snapshot(m_provider);
    if (options.theme != 0) dark_mode = options.theme == 2;
    TEXTMETRICW metrics{}; GetTextMetricsW(dc, &metrics);
    const auto sc = [&](int n) { return MulDiv(n, (std::max)(16L, metrics.tmHeight), 16); };
    const int row_height = h / 2;
    const int left = x + sc(2);
    const double remaining = CombinedRemaining(snapshot, IsWorkBuddy(m_provider));
    const COLORREF filled = remaining <= 20.0
        ? (dark_mode ? RGB(242, 139, 130) : RGB(197, 34, 31))
        : remaining <= 50.0
            ? (dark_mode ? RGB(253, 214, 99) : RGB(227, 116, 0))
            : (dark_mode ? RGB(129, 201, 149) : RGB(24, 128, 56));
    const COLORREF empty = dark_mode ? RGB(82, 91, 101) : RGB(194, 202, 211);
    const int gap = 1;
    const int quota_bar_width = CompactSessionBarWidth(dc);
    const int cell_width = (quota_bar_width - 9 * gap) / 10;
    const int bar_height = (std::max)(2, (std::min)(sc(13), row_height - 1 - sc(2) - (std::max)(3, sc(3))));
    const int top = y + (std::max)(2, sc(1)) + 1
        + (std::max)(0, (row_height - bar_height - 1 - sc(2) - (std::max)(3, sc(3))) / 2);
    for (int i = 0; i < 10; ++i)
    {
        RECT cell{left + i * (cell_width + gap), top, left + i * (cell_width + gap) + cell_width, top + bar_height};
        HBRUSH brush = CreateSolidBrush(empty);
        FillRect(dc, &cell, brush); DeleteObject(brush);
        const double recent_covered = remaining < 0.0 ? 0.0 : CodexRecent::Coverage(
            (std::min)(100.0, remaining + snapshot.recent_percent), 10, i);
        const int recent_height = static_cast<int>(std::lround(bar_height * recent_covered));
        RECT recent_fill{cell.left, cell.bottom - recent_height, cell.right, cell.bottom};
        brush = CreateSolidBrush(dark_mode ? RGB(101, 110, 120) : RGB(174, 183, 193));
        FillRect(dc, &recent_fill, brush); DeleteObject(brush);
        const double covered = remaining < 0.0 ? 0.0 : (std::clamp)(remaining / 10.0 - i, 0.0, 1.0);
        const int fill_height = static_cast<int>(std::lround(bar_height * covered));
        if (fill_height > 0)
        {
            RECT fill{cell.left, cell.bottom - fill_height, cell.right, cell.bottom};
            brush = CreateSolidBrush(filled);
            FillRect(dc, &fill, brush); DeleteObject(brush);
        }
    }
    // Countdown through the current Beijing calendar day: now to 00:00 tomorrow.
    constexpr long long duration = 86400;
    const long long now = Now();
    const long long reset = ((now + 8 * 3600) / duration + 1) * duration - 8 * 3600;
    const int drawn_width = 10 * (cell_width + gap) - gap;
    const int visible_width = static_cast<int>(std::lround(drawn_width * static_cast<double>(reset - now) / duration));
    const CodexTimeBar::Schedule schedule{ options.rest_schedule, options.morning_start, options.morning_end,
        options.afternoon_start, options.afternoon_end };
    const auto markers = CodexTimeBar::MarkerPixels(now, reset, false, drawn_width, snapshot.calendar, duration, schedule);
    HBRUSH work = CreateSolidBrush(dark_mode ? RGB(138, 144, 153) : RGB(107, 114, 128));
    HBRUSH rest = CreateSolidBrush(dark_mode ? RGB(110, 118, 130) : RGB(151, 160, 174));
    for (int pixel = 0; pixel < visible_width; ++pixel)
    {
        if (std::binary_search(markers.begin(), markers.end(), pixel)) continue;
        const long long timestamp = (std::max)(now, reset - static_cast<long long>((pixel + 0.5) * duration / drawn_width));
        RECT section{left + pixel, top + bar_height + 1, left + pixel + 1, top + bar_height + 1 + sc(2)};
        FillRect(dc, &section, CodexTimeBar::IsWorkingTime(timestamp, false, snapshot.calendar, schedule) ? work : rest);
    }
    DeleteObject(work);
    DeleteObject(rest);
    RECT percent_rect{left + drawn_width + sc(8), y, x + w, y + row_height};
    DrawText(dc, percent_rect, RemainingPercent(snapshot, IsWorkBuddy(m_provider)), dark_mode ? RGB(235, 238, 242) : RGB(45, 49, 54), DT_LEFT);
    RECT text_rect{left, y + row_height, x + w, y + h};
    RECT histogram_rect{ text_rect.left, text_rect.top + sc(1), text_rect.right, text_rect.bottom - sc(1) };
    DrawRecentHistogram(dc, histogram_rect, snapshot.recent_buckets);
    DrawText(dc, text_rect, CompactSummary(snapshot, m_provider), dark_mode ? RGB(235, 238, 242) : RGB(45, 49, 54), DT_LEFT);
    if (saved) RestoreDC(dc, saved);
}
int CQoderUsageItem::OnMouseEvent(MouseEventType type, int /*x*/, int /*y*/, void* hWnd, int /*flag*/)
{
    if (type == MT_LCLICKED)
    {
        ShowDetailPopup(static_cast<HWND>(hWnd), m_provider);
        return 1;
    }
    return 0;
}

float CQoderUsageItem::GetResourceUsageGraphValue() const
{
    const QoderSnapshot snapshot = CQoderUsagePlugin::Instance().Snapshot(m_provider);
    if (snapshot.usage.total_usage_percentage < 0.0) return 0.0f;
    return static_cast<float>((std::clamp)(snapshot.usage.total_usage_percentage / 100.0, 0.0, 1.0));
}

extern "C" __declspec(dllexport) ITMPlugin* TMPluginGetInstance()
{
    return &CQoderUsagePlugin::Instance();
}
