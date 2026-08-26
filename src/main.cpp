#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commdlg.h>
#include <shlobj.h>
#include <gdiplus.h>
#include <winhttp.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace Gdiplus;
namespace fs = std::filesystem;

namespace {
constexpr wchar_t kWindowClass[] = L"KeyPulseNativeWindow";
constexpr wchar_t kPreviewWindowClass[] = L"KeyPulseExportPreviewWindow";
constexpr wchar_t kAppName[] = L"KeyPulse";
constexpr wchar_t kAppVersion[] = L"0.3.3";
constexpr wchar_t kLatestReleaseApi[] = L"https://api.github.com/repos/tangyuanx/KeyPulse/releases/latest";
constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT WM_UPDATE_RESULT = WM_APP + 2;
constexpr UINT_PTR TIMER_UI = 1;
constexpr UINT_PTR TIMER_UPDATE = 2;
constexpr UINT UPDATE_INTERVAL_MS = 10 * 60 * 1000;
constexpr UINT ID_TRAY_OPEN = 1001;
constexpr UINT ID_TRAY_PAUSE = 1002;
constexpr UINT ID_TRAY_EXPORT = 1003;
constexpr UINT ID_TRAY_UPDATE = 1004;
constexpr UINT ID_TRAY_EXIT = 1005;
constexpr uint32_t DATA_MAGIC = 0x4B50554C; // KPUL
constexpr uint32_t HISTORY_MAGIC = 0x4B504844; // KPHD

struct KeyDef { const wchar_t* label; UINT vk; float units; };

enum class ShareTemplate { Balanced, Dark, Gallery };
enum class KeyboardVisual { App, ShareLight, ShareDark };

enum class UpdateResultKind { UpToDate, Available, Downloaded, Error };

struct UpdateResult {
    UpdateResultKind kind = UpdateResultKind::Error;
    std::wstring version;
    std::wstring download_url;
    std::wstring checksum_url;
    std::wstring downloaded_file;
    std::wstring message;
};

struct PersistedData {
    uint32_t magic = DATA_MAGIC;
    uint32_t version = 1;
    int32_t date_key = 0;
    uint8_t paused = 0;
    uint8_t reserved[7]{};
    uint64_t counts[256]{};
    int64_t minute_stamp[60]{};
    uint32_t minute_count[60]{};
};

struct DailyHistoryData {
    uint32_t magic = HISTORY_MAGIC;
    uint32_t version = 1;
    int32_t date_key = 0;
    uint32_t reserved = 0;
    uint64_t counts[256]{};
    uint32_t minute_count[1440]{};
};

struct AppState {
    HWND window = nullptr;
    HINSTANCE instance = nullptr;
    NOTIFYICONDATAW tray{};
    ULONG_PTR gdiplus_token = 0;
    std::array<uint64_t, 256> counts{};
    std::array<bool, 256> key_down{};
    std::array<int64_t, 60> minute_stamp{};
    std::array<uint32_t, 60> minute_count{};
    std::array<uint32_t, 1440> day_minutes{};
    std::array<uint64_t, 256> history_counts{};
    std::array<uint32_t, 1440> history_minutes{};
    bool running = true;
    bool exit_requested = false;
    bool dirty = false;
    bool tray_hint_shown = false;
    int date_key = 0;
    int selected_date_key = 0;
    bool history_has_data = false;
    bool calendar_visible = false;
    int calendar_year = 0;
    int calendar_month = 0;
    RectF calendar_bounds{};
    RectF calendar_previous_button{};
    RectF calendar_next_button{};
    RectF calendar_today_button{};
    std::array<RectF, 42> calendar_date_bounds{};
    std::array<int, 42> calendar_date_keys{};
    std::array<bool, 42> calendar_has_data{};
    UINT selected_vk = VK_SPACE;
    ULONGLONG last_save_tick = 0;
    RectF pause_button{};
    RectF reset_button{};
    RectF export_button{};
    RectF update_button{};
    RectF date_button{};
    int update_busy = 0;
    bool update_available = false;
    bool update_check_silent = false;
    std::wstring available_version;
    std::wstring available_download_url;
    std::wstring available_checksum_url;
    std::vector<std::pair<RectF, UINT>> key_hitboxes;
    fs::path data_directory;
};

AppState g_app;

struct ExportPreviewState {
    HWND window = nullptr;
    HWND owner = nullptr;
    ShareTemplate selected_template = ShareTemplate::Balanced;
    std::unique_ptr<Bitmap> image;
    std::array<RectF, 3> template_buttons{};
    RectF save_button{};
    RectF cancel_button{};
};

ExportPreviewState g_preview;

Color C(BYTE r, BYTE g, BYTE b, BYTE a = 255) { return Color(a, r, g, b); }

int TodayKey() {
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    return (local.tm_year + 1900) * 10000 + (local.tm_mon + 1) * 100 + local.tm_mday;
}

int64_t EpochMinute() {
    return static_cast<int64_t>(std::time(nullptr) / 60);
}

SYSTEMTIME DateKeyToSystemTime(int date_key) {
    SYSTEMTIME value{};
    value.wYear = static_cast<WORD>(date_key / 10000);
    value.wMonth = static_cast<WORD>((date_key / 100) % 100);
    value.wDay = static_cast<WORD>(date_key % 100);
    return value;
}

std::wstring DateText(int date_key, bool mark_today = true) {
    SYSTEMTIME value = DateKeyToSystemTime(date_key);
    wchar_t text[64]{};
    swprintf_s(text, L"%d年%d月%d日%s", value.wYear, value.wMonth, value.wDay,
        mark_today && date_key == TodayKey() ? L"  今天" : L"");
    return text;
}

int CurrentMinuteOfDay() {
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    return local.tm_hour * 60 + local.tm_min;
}

std::wstring FormatNumber(uint64_t n) {
    std::wstring raw = std::to_wstring(n);
    for (int i = static_cast<int>(raw.size()) - 3; i > 0; i -= 3) raw.insert(static_cast<size_t>(i), L",");
    return raw;
}

bool PointInside(const RectF& r, float x, float y) {
    return x >= r.X && x <= r.GetRight() && y >= r.Y && y <= r.GetBottom();
}

void RoundedPath(GraphicsPath& path, const RectF& r, float radius) {
    float d = radius * 2.0f;
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
    path.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
    path.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
    path.CloseFigure();
}

void FillRound(Graphics& g, const RectF& r, float radius, const Color& color) {
    GraphicsPath path;
    RoundedPath(path, r, radius);
    SolidBrush brush(color);
    g.FillPath(&brush, &path);
}

void StrokeRound(Graphics& g, const RectF& r, float radius, const Color& color, float width = 1.0f) {
    GraphicsPath path;
    RoundedPath(path, r, radius);
    Pen pen(color, width);
    g.DrawPath(&pen, &path);
}

void Text(Graphics& g, const std::wstring& text, const RectF& rect, float size, const Color& color,
          FontStyle style = FontStyleRegular, StringAlignment align = StringAlignmentNear,
          const wchar_t* family_name = L"Microsoft YaHei UI") {
    FontFamily requested(family_name);
    FontFamily fallback(L"Microsoft YaHei UI");
    FontFamily* family = requested.IsAvailable() ? &requested : &fallback;
    Font font(family, size, style, UnitPixel);
    SolidBrush brush(color);
    StringFormat format;
    format.SetAlignment(align);
    format.SetLineAlignment(StringAlignmentCenter);
    format.SetTrimming(StringTrimmingEllipsisCharacter);
    g.DrawString(text.c_str(), -1, &font, rect, &format, &brush);
}

float TextWidth(Graphics& g, const std::wstring& text, float size, FontStyle style,
                const wchar_t* family_name) {
    FontFamily requested(family_name);
    FontFamily fallback(L"Microsoft YaHei UI");
    FontFamily* family = requested.IsAvailable() ? &requested : &fallback;
    Font font(family, size, style, UnitPixel);
    RectF measured;
    PointF origin(0, 0);
    g.MeasureString(text.c_str(), -1, &font, origin, &measured);
    return measured.Width;
}

void MixedValueText(Graphics& g, const std::wstring& value, const RectF& rect,
                    float number_size, float unit_size, const Color& color,
                    FontStyle number_style = FontStyleBold) {
    size_t separator = value.find(L' ');
    if (separator == std::wstring::npos) {
        Text(g, value, rect, number_size, color, number_style, StringAlignmentNear, L"Bahnschrift");
        return;
    }
    std::wstring number = value.substr(0, separator);
    std::wstring unit = value.substr(separator + 1);
    float number_width = TextWidth(g, number, number_size, number_style, L"Bahnschrift");
    Text(g, number, RectF(rect.X, rect.Y, number_width + 3.0f, rect.Height), number_size,
        color, number_style, StringAlignmentNear, L"Bahnschrift");
    Text(g, unit, RectF(rect.X + number_width + 5.0f, rect.Y, rect.Width - number_width - 5.0f, rect.Height),
        unit_size, color, FontStyleBold);
}

std::vector<std::vector<KeyDef>> KeyboardRows() {
    return {
        {{L"Esc",VK_ESCAPE,1},{L"",0,.35f},{L"F1",VK_F1,1},{L"F2",VK_F2,1},{L"F3",VK_F3,1},{L"F4",VK_F4,1},{L"",0,.35f},{L"F5",VK_F5,1},{L"F6",VK_F6,1},{L"F7",VK_F7,1},{L"F8",VK_F8,1},{L"",0,.35f},{L"F9",VK_F9,1},{L"F10",VK_F10,1},{L"F11",VK_F11,1},{L"F12",VK_F12,1},{L"",0,.35f},{L"PrtSc",VK_SNAPSHOT,1},{L"ScrLk",VK_SCROLL,1},{L"Pause",VK_PAUSE,1}},
        {{L"`",VK_OEM_3,1},{L"1",'1',1},{L"2",'2',1},{L"3",'3',1},{L"4",'4',1},{L"5",'5',1},{L"6",'6',1},{L"7",'7',1},{L"8",'8',1},{L"9",'9',1},{L"0",'0',1},{L"-",VK_OEM_MINUS,1},{L"=",VK_OEM_PLUS,1},{L"Backspace",VK_BACK,2},{L"",0,.35f},{L"Ins",VK_INSERT,1},{L"Home",VK_HOME,1},{L"PgUp",VK_PRIOR,1},{L"",0,.35f},{L"Num",VK_NUMLOCK,1},{L"/",VK_DIVIDE,1},{L"*",VK_MULTIPLY,1},{L"-",VK_SUBTRACT,1}},
        {{L"Tab",VK_TAB,1.5f},{L"Q",'Q',1},{L"W",'W',1},{L"E",'E',1},{L"R",'R',1},{L"T",'T',1},{L"Y",'Y',1},{L"U",'U',1},{L"I",'I',1},{L"O",'O',1},{L"P",'P',1},{L"[",VK_OEM_4,1},{L"]",VK_OEM_6,1},{L"\\",VK_OEM_5,1.5f},{L"",0,.35f},{L"Del",VK_DELETE,1},{L"End",VK_END,1},{L"PgDn",VK_NEXT,1},{L"",0,.35f},{L"7",VK_NUMPAD7,1},{L"8",VK_NUMPAD8,1},{L"9",VK_NUMPAD9,1},{L"+",VK_ADD,1}},
        {{L"Caps",VK_CAPITAL,1.8f},{L"A",'A',1},{L"S",'S',1},{L"D",'D',1},{L"F",'F',1},{L"G",'G',1},{L"H",'H',1},{L"J",'J',1},{L"K",'K',1},{L"L",'L',1},{L";",VK_OEM_1,1},{L"'",VK_OEM_7,1},{L"Enter",VK_RETURN,2.2f},{L"",0,.35f},{L"",0,3.35f},{L"",0,.35f},{L"4",VK_NUMPAD4,1},{L"5",VK_NUMPAD5,1},{L"6",VK_NUMPAD6,1},{L"+",VK_ADD,1}},
        {{L"Shift",VK_LSHIFT,2.3f},{L"Z",'Z',1},{L"X",'X',1},{L"C",'C',1},{L"V",'V',1},{L"B",'B',1},{L"N",'N',1},{L"M",'M',1},{L",",VK_OEM_COMMA,1},{L".",VK_OEM_PERIOD,1},{L"/",VK_OEM_2,1},{L"Shift",VK_RSHIFT,2.7f},{L"",0,.35f},{L"",0,1},{L"↑",VK_UP,1},{L"",0,1},{L"",0,.35f},{L"1",VK_NUMPAD1,1},{L"2",VK_NUMPAD2,1},{L"3",VK_NUMPAD3,1},{L"Enter",VK_RETURN,1}},
        {{L"Ctrl",VK_LCONTROL,1.4f},{L"Win",VK_LWIN,1.2f},{L"Alt",VK_LMENU,1.2f},{L"Space",VK_SPACE,6.4f},{L"Alt",VK_RMENU,1.2f},{L"Win",VK_RWIN,1.2f},{L"Menu",VK_APPS,1.2f},{L"Ctrl",VK_RCONTROL,1.4f},{L"",0,.35f},{L"←",VK_LEFT,1},{L"↓",VK_DOWN,1},{L"→",VK_RIGHT,1},{L"",0,.35f},{L"0",VK_NUMPAD0,2.05f},{L".",VK_DECIMAL,1},{L"Enter",VK_RETURN,1}}
    };
}

std::wstring KeyLabel(UINT vk) {
    for (const auto& row : KeyboardRows()) for (const auto& key : row) if (key.vk == vk) return key.label;
    return L"Key " + std::to_wstring(vk);
}

uint64_t TotalCount() {
    const auto& counts = g_app.selected_date_key == g_app.date_key ? g_app.counts : g_app.history_counts;
    return std::accumulate(counts.begin(), counts.end(), uint64_t{0});
}

bool ViewingToday() {
    return g_app.selected_date_key == g_app.date_key;
}

const std::array<uint64_t, 256>& VisibleCounts() {
    return ViewingToday() ? g_app.counts : g_app.history_counts;
}

const std::array<uint32_t, 1440>& VisibleMinutes() {
    return ViewingToday() ? g_app.day_minutes : g_app.history_minutes;
}

uint32_t CurrentRate() {
    int64_t now = EpochMinute();
    size_t slot = static_cast<size_t>(now % 60);
    return g_app.minute_stamp[slot] == now ? g_app.minute_count[slot] : 0;
}

uint32_t PeakRate() {
    uint32_t peak = 0;
    int64_t now = EpochMinute();
    for (size_t i = 0; i < 60; ++i) if (g_app.minute_stamp[i] > now - 60) peak = (std::max)(peak, g_app.minute_count[i]);
    return peak;
}

uint32_t ActiveMinutes() {
    return static_cast<uint32_t>(std::count_if(VisibleMinutes().begin(), VisibleMinutes().end(),
        [](uint32_t value) { return value > 0; }));
}

uint32_t DayPeakRate() {
    return *std::max_element(VisibleMinutes().begin(), VisibleMinutes().end());
}

void SaveData();
void ResetForNewDay();

fs::path ExecutablePath() {
    std::array<wchar_t, 32768> buffer{};
    GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    return fs::path(buffer.data());
}

fs::path ExecutableDirectory() {
    return ExecutablePath().parent_path();
}

fs::path ResolveDataDirectory() {
    fs::path exe_dir = ExecutableDirectory();
    if (fs::exists(exe_dir / L"portable.flag")) return exe_dir / L"data";
    PWSTR path = nullptr;
    fs::path result = exe_dir / L"data";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &path))) {
        result = fs::path(path) / L"KeyPulse";
        CoTaskMemFree(path);
    }
    return result;
}

struct WinHttpHandle {
    HINTERNET value = nullptr;
    explicit WinHttpHandle(HINTERNET handle = nullptr) : value(handle) {}
    ~WinHttpHandle() { if (value) WinHttpCloseHandle(value); }
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    operator HINTERNET() const { return value; }
    explicit operator bool() const { return value != nullptr; }
};

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) return {};
    int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

bool HttpGetOnce(const std::wstring& url, std::vector<unsigned char>& body, std::wstring& error,
                 size_t maximum_bytes, DWORD access_type, DWORD& winhttp_error) {
    winhttp_error = ERROR_SUCCESS;
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) {
        winhttp_error = GetLastError();
        error = L"无法解析更新地址，错误代码 " + std::to_wstring(winhttp_error);
        return false;
    }
    std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength > 0) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    std::wstring user_agent = std::wstring(L"KeyPulse/") + kAppVersion;
    WinHttpHandle session(WinHttpOpen(user_agent.c_str(), access_type,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        winhttp_error = GetLastError();
        error = L"无法初始化网络连接，错误代码 " + std::to_wstring(winhttp_error);
        return false;
    }
    WinHttpSetTimeouts(session, 10000, 15000, 30000, 60000);
    WinHttpHandle connection(WinHttpConnect(session, host.c_str(), parts.nPort, 0));
    if (!connection) {
        winhttp_error = GetLastError();
        error = L"无法连接 GitHub，错误代码 " + std::to_wstring(winhttp_error);
        return false;
    }
    DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    WinHttpHandle request(WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!request) {
        winhttp_error = GetLastError();
        error = L"无法创建更新请求，错误代码 " + std::to_wstring(winhttp_error);
        return false;
    }
    DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect_policy, sizeof(redirect_policy));
    const wchar_t headers[] = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    if (!WinHttpSendRequest(request, headers, static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        winhttp_error = GetLastError();
        error = L"发送 GitHub 请求失败，错误代码 " + std::to_wstring(winhttp_error);
        return false;
    }
    if (!WinHttpReceiveResponse(request, nullptr)) {
        winhttp_error = GetLastError();
        error = L"等待 GitHub 响应失败，错误代码 " + std::to_wstring(winhttp_error);
        return false;
    }
    DWORD status = 0, status_size = sizeof(status);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX) || status != 200) {
        error = L"GitHub 返回状态码 " + std::to_wstring(status);
        if (status == 408 || status == 429 || status == 500 || status == 502 || status == 503 || status == 504) {
            winhttp_error = ERROR_WINHTTP_INVALID_SERVER_RESPONSE;
        }
        return false;
    }
    body.clear();
    std::array<unsigned char, 16384> buffer{};
    while (true) {
        DWORD bytes_read = 0;
        if (!WinHttpReadData(request, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes_read)) {
            winhttp_error = GetLastError();
            error = L"读取更新数据失败，错误代码 " + std::to_wstring(winhttp_error);
            return false;
        }
        if (bytes_read == 0) break;
        if (body.size() + bytes_read > maximum_bytes) {
            error = L"更新文件大小异常，已停止下载";
            return false;
        }
        body.insert(body.end(), buffer.begin(), buffer.begin() + bytes_read);
    }
    return true;
}

bool IsRetryableWinHttpError(DWORD error) {
    return error == ERROR_WINHTTP_TIMEOUT || error == ERROR_WINHTTP_NAME_NOT_RESOLVED ||
        error == ERROR_WINHTTP_CANNOT_CONNECT || error == ERROR_WINHTTP_CONNECTION_ERROR ||
        error == ERROR_WINHTTP_RESEND_REQUEST || error == ERROR_WINHTTP_INVALID_SERVER_RESPONSE ||
        error == ERROR_WINHTTP_AUTO_PROXY_SERVICE_ERROR || error == ERROR_WINHTTP_UNABLE_TO_DOWNLOAD_SCRIPT;
}

bool HttpGet(const std::wstring& url, std::vector<unsigned char>& body, std::wstring& error,
             size_t maximum_bytes = 100 * 1024 * 1024) {
    constexpr DWORD access_types[] = {
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_ACCESS_TYPE_NO_PROXY
    };
    DWORD last_error = ERROR_SUCCESS;
    for (size_t attempt = 0; attempt < std::size(access_types); ++attempt) {
        body.clear();
        if (HttpGetOnce(url, body, error, maximum_bytes, access_types[attempt], last_error)) return true;
        if (!IsRetryableWinHttpError(last_error)) return false;
        if (attempt + 1 < std::size(access_types)) Sleep(attempt == 0 ? 700 : 1800);
    }
    if (last_error == ERROR_WINHTTP_TIMEOUT) {
        error = L"连接 GitHub 超时，已使用三种网络方式重试。请检查代理、VPN 或防火墙设置（错误代码 12002）。";
    } else {
        error += L"，已自动重试三次";
    }
    return false;
}

bool FindJsonString(const std::string& json, const std::string& key, size_t start,
                    std::string& value, size_t& next) {
    size_t key_pos = json.find("\"" + key + "\"", start);
    if (key_pos == std::string::npos) return false;
    size_t colon = json.find(':', key_pos + key.size() + 2);
    size_t quote = colon == std::string::npos ? std::string::npos : json.find('"', colon + 1);
    if (quote == std::string::npos) return false;
    value.clear();
    bool escaped = false;
    for (size_t i = quote + 1; i < json.size(); ++i) {
        char ch = json[i];
        if (escaped) {
            if (ch == 'n') value.push_back('\n');
            else if (ch == 'r') value.push_back('\r');
            else if (ch == 't') value.push_back('\t');
            else value.push_back(ch);
            escaped = false;
        } else if (ch == '\\') {
            escaped = true;
        } else if (ch == '"') {
            next = i + 1;
            return true;
        } else {
            value.push_back(ch);
        }
    }
    return false;
}

std::string FindReleaseAssetUrl(const std::string& json, const std::string& wanted_name) {
    size_t cursor = 0;
    size_t next = 0;
    std::string name;
    std::string url;
    while (FindJsonString(json, "name", cursor, name, next)) {
        cursor = next;
        if (name == wanted_name && FindJsonString(json, "browser_download_url", cursor, url, next)) return url;
    }
    return {};
}

std::string Sha256Hex(const std::vector<unsigned char>& bytes) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_length = 0, hash_length = 0, result_size = 0;
    std::string result;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return result;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_length),
        sizeof(object_length), &result_size, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_length),
        sizeof(hash_length), &result_size, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return result;
    }
    std::vector<unsigned char> object(object_length);
    std::vector<unsigned char> digest(hash_length);
    if (BCryptCreateHash(algorithm, &hash, object.data(), object_length, nullptr, 0, 0) >= 0 &&
        BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), 0) >= 0 &&
        BCryptFinishHash(hash, digest.data(), hash_length, 0) >= 0) {
        constexpr char digits[] = "0123456789abcdef";
        result.reserve(digest.size() * 2);
        for (unsigned char byte : digest) {
            result.push_back(digits[byte >> 4]);
            result.push_back(digits[byte & 0x0f]);
        }
    }
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return result;
}

std::string FirstSha256(const std::vector<unsigned char>& bytes) {
    std::string text(bytes.begin(), bytes.end());
    std::string result;
    for (char ch : text) {
        bool hexadecimal = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
        if (!hexadecimal) {
            if (!result.empty()) break;
            continue;
        }
        result.push_back(ch >= 'A' && ch <= 'F' ? static_cast<char>(ch - 'A' + 'a') : ch);
        if (result.size() == 64) break;
    }
    return result.size() == 64 ? result : std::string{};
}

std::vector<int> VersionParts(std::wstring version) {
    if (!version.empty() && (version.front() == L'v' || version.front() == L'V')) version.erase(version.begin());
    std::vector<int> parts;
    size_t start = 0;
    while (start <= version.size()) {
        size_t end = version.find(L'.', start);
        std::wstring item = version.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        try { parts.push_back(item.empty() ? 0 : std::stoi(item)); } catch (...) { parts.push_back(0); }
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return parts;
}

bool IsNewerVersion(const std::wstring& candidate, const std::wstring& current) {
    auto left = VersionParts(candidate);
    auto right = VersionParts(current);
    size_t count = (std::max)(left.size(), right.size());
    left.resize(count);
    right.resize(count);
    for (size_t i = 0; i < count; ++i) {
        if (left[i] != right[i]) return left[i] > right[i];
    }
    return false;
}

void PostUpdateResult(HWND window, std::unique_ptr<UpdateResult> result) {
    UpdateResult* raw = result.release();
    if (!PostMessageW(window, WM_UPDATE_RESULT, 0, reinterpret_cast<LPARAM>(raw))) delete raw;
}

void StartUpdateCheck(HWND window, bool silent = false) {
    if (g_app.update_busy != 0 || g_app.update_available) return;
    g_app.update_busy = 1;
    g_app.update_check_silent = silent;
    InvalidateRect(window, nullptr, FALSE);
    std::thread([window]() {
        auto result = std::make_unique<UpdateResult>();
        std::vector<unsigned char> bytes;
        std::wstring error;
        if (!HttpGet(kLatestReleaseApi, bytes, error, 2 * 1024 * 1024)) {
            result->message = error;
            PostUpdateResult(window, std::move(result));
            return;
        }
        std::string json(bytes.begin(), bytes.end());
        std::string tag;
        size_t next = 0;
        if (!FindJsonString(json, "tag_name", 0, tag, next)) {
            result->message = L"GitHub Release 信息格式无法识别";
            PostUpdateResult(window, std::move(result));
            return;
        }
        result->version = Utf8ToWide(tag);
        if (!IsNewerVersion(result->version, kAppVersion)) {
            result->kind = UpdateResultKind::UpToDate;
            PostUpdateResult(window, std::move(result));
            return;
        }
        std::string url = FindReleaseAssetUrl(json, "KeyPulse.exe");
        std::string checksum_url = FindReleaseAssetUrl(json, "KeyPulse.exe.sha256");
        if (url.empty() || checksum_url.empty()) {
            result->message = L"最新 Release 缺少程序或 SHA-256 校验文件";
            PostUpdateResult(window, std::move(result));
            return;
        }
        result->kind = UpdateResultKind::Available;
        result->download_url = Utf8ToWide(url);
        result->checksum_url = Utf8ToWide(checksum_url);
        PostUpdateResult(window, std::move(result));
    }).detach();
}

void StartUpdateDownload(HWND window, std::wstring version, std::wstring url, std::wstring checksum_url) {
    g_app.update_busy = 2;
    InvalidateRect(window, nullptr, FALSE);
    std::thread([window, version = std::move(version), url = std::move(url), checksum_url = std::move(checksum_url)]() {
        auto result = std::make_unique<UpdateResult>();
        result->version = version;
        std::vector<unsigned char> bytes;
        std::wstring error;
        if (!HttpGet(url, bytes, error) || bytes.size() < 2 || bytes[0] != 'M' || bytes[1] != 'Z') {
            result->message = error.empty() ? L"下载到的文件不是有效的 Windows 程序" : error;
            PostUpdateResult(window, std::move(result));
            return;
        }
        std::vector<unsigned char> checksum_bytes;
        if (!HttpGet(checksum_url, checksum_bytes, error, 4096)) {
            result->message = L"无法下载 SHA-256 校验文件：" + error;
            PostUpdateResult(window, std::move(result));
            return;
        }
        std::string expected_hash = FirstSha256(checksum_bytes);
        std::string actual_hash = Sha256Hex(bytes);
        if (expected_hash.empty() || actual_hash.empty() || expected_hash != actual_hash) {
            result->message = L"新版程序的 SHA-256 校验失败，已停止更新";
            PostUpdateResult(window, std::move(result));
            return;
        }
        try {
            fs::path file = fs::temp_directory_path() /
                (L"KeyPulse-update-" + std::to_wstring(GetCurrentProcessId()) + L".exe");
            std::ofstream stream(file, std::ios::binary | std::ios::trunc);
            stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            stream.close();
            if (!stream) throw std::runtime_error("write failed");
            result->kind = UpdateResultKind::Downloaded;
            result->downloaded_file = file.wstring();
        } catch (...) {
            result->message = L"无法把新版程序写入临时目录";
        }
        PostUpdateResult(window, std::move(result));
    }).detach();
}

bool LaunchUpdateInstaller(const std::wstring& downloaded_file, std::wstring& error) {
    fs::path target = ExecutablePath();
    fs::path probe = ExecutableDirectory() /
        (L".keypulse-write-test-" + std::to_wstring(GetCurrentProcessId()));
    HANDLE probe_handle = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (probe_handle == INVALID_HANDLE_VALUE) {
        error = L"当前程序目录不可写，请把 KeyPulse.exe 移到普通文件夹后再更新";
        return false;
    }
    CloseHandle(probe_handle);
    DeleteFileW(probe.c_str());
    fs::path helper = fs::temp_directory_path() /
        (L"KeyPulse-updater-" + std::to_wstring(GetCurrentProcessId()) + L".exe");
    if (!CopyFileW(target.c_str(), helper.c_str(), FALSE)) {
        error = L"无法创建原生更新程序，错误代码 " + std::to_wstring(GetLastError());
        return false;
    }
    std::wstring command = L"\"" + helper.wstring() + L"\" --apply-update \"" + downloaded_file +
        L"\" \"" + target.wstring() + L"\" " + std::to_wstring(GetCurrentProcessId());
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(helper.c_str(), mutable_command.data(), nullptr, nullptr, FALSE, 0,
        nullptr, nullptr, &startup, &process)) {
        error = L"无法启动更新程序，错误代码 " + std::to_wstring(GetLastError());
        DeleteFileW(helper.c_str());
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

int ApplyDownloadedUpdate(const fs::path& source, const fs::path& target, DWORD parent_process_id) {
    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, parent_process_id);
    if (parent) {
        WaitForSingleObject(parent, 30000);
        CloseHandle(parent);
    } else {
        Sleep(1000);
    }
    bool replaced = false;
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (CopyFileW(source.c_str(), target.c_str(), FALSE)) {
            replaced = true;
            break;
        }
        Sleep(500);
    }
    if (!replaced) {
        MessageBoxW(nullptr, L"无法替换旧版 KeyPulse，请手动下载最新 Release。", L"KeyPulse 更新失败", MB_OK | MB_ICONERROR);
        return 1;
    }
    DeleteFileW(source.c_str());
    HINSTANCE launched = ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, target.parent_path().c_str(), SW_SHOWNORMAL);
    fs::path helper = ExecutablePath();
    MoveFileExW(helper.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    return reinterpret_cast<INT_PTR>(launched) > 32 ? 0 : 1;
}

fs::path HistoryPath(int date_key) {
    return g_app.data_directory / L"history" / (std::to_wstring(date_key) + L".dat");
}

bool WriteHistoryFile(int date_key, const std::array<uint64_t, 256>& counts,
                      const std::array<uint32_t, 1440>& minutes) {
    try {
        fs::create_directories(g_app.data_directory / L"history");
        DailyHistoryData data;
        data.date_key = date_key;
        std::copy(counts.begin(), counts.end(), data.counts);
        std::copy(minutes.begin(), minutes.end(), data.minute_count);
        fs::path target = HistoryPath(date_key);
        fs::path temp = target;
        temp += L".tmp";
        std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(&data), sizeof(data));
        stream.close();
        return stream && MoveFileExW(temp.c_str(), target.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    } catch (...) {
        return false;
    }
}

bool ReadHistoryFile(int date_key, std::array<uint64_t, 256>& counts,
                     std::array<uint32_t, 1440>& minutes) {
    try {
        DailyHistoryData data;
        std::ifstream stream(HistoryPath(date_key), std::ios::binary);
        stream.read(reinterpret_cast<char*>(&data), sizeof(data));
        if (stream.gcount() != sizeof(data) || data.magic != HISTORY_MAGIC ||
            data.version != 1 || data.date_key != date_key) return false;
        std::copy(std::begin(data.counts), std::end(data.counts), counts.begin());
        std::copy(std::begin(data.minute_count), std::end(data.minute_count), minutes.begin());
        return true;
    } catch (...) {
        return false;
    }
}

std::array<uint32_t, 1440> MinutesFromLegacyRing(const PersistedData& data) {
    std::array<uint32_t, 1440> result{};
    for (size_t i = 0; i < 60; ++i) {
        if (data.minute_stamp[i] <= 0 || data.minute_count[i] == 0) continue;
        std::time_t timestamp = static_cast<std::time_t>(data.minute_stamp[i] * 60);
        std::tm local{};
        localtime_s(&local, &timestamp);
        int date_key = (local.tm_year + 1900) * 10000 + (local.tm_mon + 1) * 100 + local.tm_mday;
        if (date_key == data.date_key) result[static_cast<size_t>(local.tm_hour * 60 + local.tm_min)] = data.minute_count[i];
    }
    return result;
}

void SaveData() {
    try {
        fs::create_directories(g_app.data_directory);
        PersistedData data;
        data.date_key = g_app.date_key;
        data.paused = g_app.running ? 0 : 1;
        std::copy(g_app.counts.begin(), g_app.counts.end(), data.counts);
        std::copy(g_app.minute_stamp.begin(), g_app.minute_stamp.end(), data.minute_stamp);
        std::copy(g_app.minute_count.begin(), g_app.minute_count.end(), data.minute_count);
        fs::path target = g_app.data_directory / L"keypulse.dat";
        fs::path temp = g_app.data_directory / L"keypulse.tmp";
        std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(&data), sizeof(data));
        stream.close();
        MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        WriteHistoryFile(g_app.date_key, g_app.counts, g_app.day_minutes);
        g_app.dirty = false;
        g_app.last_save_tick = GetTickCount64();
    } catch (...) { }
}

void LoadData() {
    g_app.data_directory = ResolveDataDirectory();
    g_app.date_key = TodayKey();
    g_app.selected_date_key = g_app.date_key;
    try {
        std::ifstream stream(g_app.data_directory / L"keypulse.dat", std::ios::binary);
        PersistedData data;
        stream.read(reinterpret_cast<char*>(&data), sizeof(data));
        if (stream.gcount() == sizeof(data) && data.magic == DATA_MAGIC && data.version == 1) {
            g_app.running = data.paused == 0;
            auto legacy_minutes = MinutesFromLegacyRing(data);
            if (data.date_key == g_app.date_key) {
                std::copy(std::begin(data.counts), std::end(data.counts), g_app.counts.begin());
                std::copy(std::begin(data.minute_stamp), std::end(data.minute_stamp), g_app.minute_stamp.begin());
                std::copy(std::begin(data.minute_count), std::end(data.minute_count), g_app.minute_count.begin());
                ReadHistoryFile(g_app.date_key, g_app.history_counts, g_app.day_minutes);
                for (size_t i = 0; i < legacy_minutes.size(); ++i) {
                    if (legacy_minutes[i] > 0) g_app.day_minutes[i] = legacy_minutes[i];
                }
            } else if (data.date_key > 20000101 && data.date_key < g_app.date_key) {
                std::array<uint64_t, 256> previous_counts{};
                std::copy(std::begin(data.counts), std::end(data.counts), previous_counts.begin());
                WriteHistoryFile(data.date_key, previous_counts, legacy_minutes);
            }
        }
    } catch (...) { }
    g_app.last_save_tick = GetTickCount64();
}

void ResetForNewDay() {
    int today = TodayKey();
    if (g_app.date_key != 0 && g_app.date_key != today) SaveData();
    g_app.counts.fill(0);
    g_app.minute_stamp.fill(0);
    g_app.minute_count.fill(0);
    g_app.day_minutes.fill(0);
    g_app.history_counts.fill(0);
    g_app.history_minutes.fill(0);
    g_app.date_key = today;
    g_app.selected_date_key = today;
    g_app.history_has_data = false;
    g_app.calendar_visible = false;
    g_app.dirty = true;
}

void SelectDate(int date_key) {
    if (date_key > TodayKey()) return;
    g_app.selected_date_key = date_key;
    g_app.history_counts.fill(0);
    g_app.history_minutes.fill(0);
    g_app.history_has_data = date_key == g_app.date_key ||
        ReadHistoryFile(date_key, g_app.history_counts, g_app.history_minutes);
    InvalidateRect(g_app.window, nullptr, FALSE);
}

bool IsLeapYear(int year) {
    return year % 400 == 0 || (year % 4 == 0 && year % 100 != 0);
}

int DaysInMonth(int year, int month) {
    constexpr int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == 2 && IsLeapYear(year) ? 29 : days[month - 1];
}

int MondayFirstWeekday(int year, int month, int day) {
    constexpr int offsets[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (month < 3) --year;
    int sunday_first = (year + year / 4 - year / 100 + year / 400 + offsets[month - 1] + day) % 7;
    return (sunday_first + 6) % 7;
}

int CalendarDateAt(size_t index) {
    int first = MondayFirstWeekday(g_app.calendar_year, g_app.calendar_month, 1);
    int day = static_cast<int>(index) - first + 1;
    int year = g_app.calendar_year;
    int month = g_app.calendar_month;
    if (day <= 0) {
        if (--month == 0) { month = 12; --year; }
        day += DaysInMonth(year, month);
    } else if (day > DaysInMonth(year, month)) {
        day -= DaysInMonth(year, month);
        if (++month == 13) { month = 1; ++year; }
    }
    return year * 10000 + month * 100 + day;
}

void RefreshCalendarHistory() {
    uint64_t today_total = std::accumulate(g_app.counts.begin(), g_app.counts.end(), uint64_t{0});
    for (size_t i = 0; i < g_app.calendar_date_keys.size(); ++i) {
        int date_key = CalendarDateAt(i);
        g_app.calendar_date_keys[i] = date_key;
        std::error_code error;
        g_app.calendar_has_data[i] = date_key == g_app.date_key ? today_total > 0 : fs::exists(HistoryPath(date_key), error);
    }
}

bool CanShowNextCalendarMonth() {
    int today = TodayKey();
    int current_month_key = g_app.calendar_year * 100 + g_app.calendar_month;
    return current_month_key < today / 100;
}

void ShiftCalendarMonth(int delta) {
    int year = g_app.calendar_year;
    int month = g_app.calendar_month + delta;
    if (month == 0) { month = 12; --year; }
    if (month == 13) { month = 1; ++year; }
    if (year < 2000 || (delta > 0 && year * 100 + month > TodayKey() / 100)) return;
    g_app.calendar_year = year;
    g_app.calendar_month = month;
    RefreshCalendarHistory();
    InvalidateRect(g_app.window, nullptr, FALSE);
}

Color HeatColor(uint64_t value, uint64_t maximum) {
    if (value == 0 || maximum == 0) return C(249, 250, 247);
    float p = static_cast<float>(value) / static_cast<float>(maximum);
    if (p > .72f) return C(49, 118, 78);
    if (p > .45f) return C(104, 158, 106);
    if (p > .25f) return C(151, 190, 143);
    if (p > .10f) return C(196, 217, 188);
    return C(226, 236, 220);
}

Color ShareHeatColor(uint64_t value, uint64_t maximum, bool dark) {
    if (dark) {
        if (value == 0 || maximum == 0) return C(27, 57, 42);
        float p = static_cast<float>(value) / static_cast<float>(maximum);
        if (p > .72f) return C(111, 156, 117);
        if (p > .45f) return C(82, 130, 90);
        if (p > .25f) return C(62, 106, 75);
        if (p > .10f) return C(47, 84, 61);
        return C(36, 69, 50);
    }
    if (value == 0 || maximum == 0) return C(248, 249, 247);
    float p = static_cast<float>(value) / static_cast<float>(maximum);
    if (p > .72f) return C(49, 101, 71);
    if (p > .45f) return C(105, 151, 108);
    if (p > .25f) return C(151, 184, 148);
    if (p > .10f) return C(193, 211, 188);
    return C(226, 234, 223);
}

void DrawStatCard(Graphics& g, float x, float y, float w, const std::wstring& label,
                  const std::wstring& value, const std::wstring& note, const Color& accent) {
    Text(g, label, RectF(x + 22, y + 12, w - 44, 20), 11, C(76, 84, 77));
    MixedValueText(g, value, RectF(x + 22, y + 33, w - 44, 34), 27, 18, accent);
    Text(g, note, RectF(x + 22, y + 70, w - 44, 18), 9.5f, C(133, 141, 134));
}

void DrawButton(Graphics& g, const RectF& r, const std::wstring& label, bool primary = false, bool accent = false) {
    Color fill = accent ? C(47, 107, 77) : (primary ? C(28, 38, 28) : C(251, 252, 249));
    Color border = accent ? C(37, 88, 62) : (primary ? C(28, 38, 28) : C(218, 225, 216));
    Color text_color = (accent || primary) ? C(245, 248, 241) : C(73, 84, 74);
    FillRound(g, r, 8, fill);
    StrokeRound(g, r, 8, border);
    Text(g, label, r, 11, text_color, accent ? FontStyleBold : FontStyleRegular, StringAlignmentCenter);
}

void DrawChevron(Graphics& g, const RectF& button, bool right, bool enabled = true) {
    SolidBrush fill(C(245, 247, 244));
    g.FillEllipse(&fill, button);
    Pen border(C(224, 229, 222));
    g.DrawEllipse(&border, button);
    Color color = enabled ? C(48, 64, 52) : C(178, 185, 179);
    Pen pen(color, 1.8f);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    float center_x = button.X + button.Width / 2.0f;
    float center_y = button.Y + button.Height / 2.0f;
    float direction = right ? 1.0f : -1.0f;
    g.DrawLine(&pen, center_x - direction * 3.0f, center_y - 5.0f, center_x + direction * 2.0f, center_y);
    g.DrawLine(&pen, center_x + direction * 2.0f, center_y, center_x - direction * 3.0f, center_y + 5.0f);
}

void DrawCalendar(Graphics& g, int window_width) {
    constexpr float popup_width = 310.0f;
    constexpr float popup_height = 342.0f;
    float x = g_app.date_button.GetRight() - popup_width;
    x = (std::max)(8.0f, (std::min)(x, static_cast<float>(window_width) - popup_width - 8.0f));
    float y = g_app.date_button.GetBottom() + 6.0f;
    g_app.calendar_bounds = RectF(x, y, popup_width, popup_height);

    FillRound(g, RectF(x + 2, y + 7, popup_width, popup_height), 12, C(62, 86, 66, 30));
    FillRound(g, g_app.calendar_bounds, 12, C(255, 255, 253));
    StrokeRound(g, g_app.calendar_bounds, 12, C(210, 220, 210));

    Text(g, std::to_wstring(g_app.calendar_year) + L"年" + std::to_wstring(g_app.calendar_month) + L"月",
        RectF(x + 62, y + 14, popup_width - 124, 34), 16, C(35, 44, 36), FontStyleBold, StringAlignmentCenter);
    g_app.calendar_previous_button = RectF(x + 18, y + 16, 30, 30);
    g_app.calendar_next_button = RectF(x + popup_width - 48, y + 16, 30, 30);
    DrawChevron(g, g_app.calendar_previous_button, false, g_app.calendar_year > 2000 || g_app.calendar_month > 1);
    DrawChevron(g, g_app.calendar_next_button, true, CanShowNextCalendarMonth());

    constexpr const wchar_t* weekdays[] = {L"一", L"二", L"三", L"四", L"五", L"六", L"日"};
    float grid_x = x + 16.0f;
    float cell_width = (popup_width - 32.0f) / 7.0f;
    for (int column = 0; column < 7; ++column) {
        Text(g, weekdays[column], RectF(grid_x + cell_width * column, y + 57, cell_width, 24),
            10, C(117, 127, 118), FontStyleRegular, StringAlignmentCenter);
    }
    Pen divider(C(232, 236, 231));
    g.DrawLine(&divider, x + 16, y + 84, x + popup_width - 16, y + 84);

    int today = TodayKey();
    uint64_t today_total = std::accumulate(g_app.counts.begin(), g_app.counts.end(), uint64_t{0});
    float grid_y = y + 91.0f;
    for (size_t i = 0; i < g_app.calendar_date_keys.size(); ++i) {
        int row = static_cast<int>(i / 7);
        int column = static_cast<int>(i % 7);
        RectF cell(grid_x + cell_width * column, grid_y + 35.0f * row, cell_width, 35.0f);
        g_app.calendar_date_bounds[i] = cell;
        int date_key = g_app.calendar_date_keys[i];
        bool current_month = date_key / 100 == g_app.calendar_year * 100 + g_app.calendar_month;
        bool future = date_key > today;
        bool selected = date_key == g_app.selected_date_key;
        float circle_size = 29.0f;
        RectF circle(cell.X + (cell.Width - circle_size) / 2.0f, cell.Y + 1.0f, circle_size, circle_size);
        if (selected) {
            SolidBrush selected_fill(C(47, 107, 77));
            g.FillEllipse(&selected_fill, circle);
        } else if (date_key == today) {
            Pen today_ring(C(47, 107, 77), 1.5f);
            g.DrawEllipse(&today_ring, circle);
        }
        Color date_color = selected ? C(255, 255, 253) :
            (!current_month || future ? C(174, 181, 175) : C(45, 54, 46));
        Text(g, std::to_wstring(date_key % 100), RectF(cell.X, cell.Y - 1, cell.Width, 27),
            11.0f, date_color, selected ? FontStyleBold : FontStyleRegular, StringAlignmentCenter, L"Bahnschrift");
        if (g_app.calendar_has_data[i] || (date_key == g_app.date_key && today_total > 0)) {
            SolidBrush dot(selected ? C(224, 239, 228) : C(47, 107, 77));
            g.FillEllipse(&dot, RectF(cell.X + cell.Width / 2.0f - 2.0f, cell.Y + 27.0f, 4.0f, 4.0f));
        }
    }

    RectF footer(x, y + popup_height - 43.0f, popup_width, 43.0f);
    FillRound(g, footer, 12, C(244, 248, 243));
    SolidBrush footer_cover(C(244, 248, 243));
    g.FillRectangle(&footer_cover, footer.X, footer.Y, footer.Width, 12.0f);
    g.DrawLine(&divider, footer.X, footer.Y, footer.GetRight(), footer.Y);
    SolidBrush legend_dot(C(47, 107, 77));
    g.FillEllipse(&legend_dot, RectF(x + 20, footer.Y + 19, 6, 6));
    Text(g, L"有记录", RectF(x + 32, footer.Y + 8, 66, 27), 9, C(101, 113, 103));
    g_app.calendar_today_button = RectF(x + popup_width - 98, footer.Y + 7, 82, 29);
    Text(g, L"回到今天", g_app.calendar_today_button, 10, C(47, 107, 77), FontStyleBold, StringAlignmentCenter);
}

void DrawKeyboard(Graphics& g, const RectF& bounds, bool register_hits,
                  KeyboardVisual visual = KeyboardVisual::App) {
    bool app_style = visual == KeyboardVisual::App;
    bool dark_style = visual == KeyboardVisual::ShareDark;
    if (app_style) {
        FillRound(g, bounds, 8, C(245, 247, 243));
        StrokeRound(g, bounds, 8, C(218, 224, 216));
    }
    auto rows = KeyboardRows();
    const auto& counts = VisibleCounts();
    uint64_t maximum = *std::max_element(counts.begin(), counts.end());
    const float pad = 11.0f, gap = 5.0f;
    float row_h = (bounds.Height - pad * 2 - gap * static_cast<float>(rows.size() - 1)) / static_cast<float>(rows.size());
    if (register_hits) g_app.key_hitboxes.clear();
    for (size_t ri = 0; ri < rows.size(); ++ri) {
        const auto& row = rows[ri];
        float unit_sum = 0;
        for (const auto& key : row) unit_sum += key.units;
        float unit = (bounds.Width - pad * 2 - gap * static_cast<float>(row.size() - 1)) / unit_sum;
        float x = bounds.X + pad;
        float y = bounds.Y + pad + static_cast<float>(ri) * (row_h + gap);
        for (const auto& key : row) {
            RectF r(x, y, unit * key.units, row_h);
            if (key.vk == 0) {
                x += r.Width + gap;
                continue;
            }
            Color fill = app_style ? HeatColor(counts[key.vk], maximum) :
                ShareHeatColor(counts[key.vk], maximum, dark_style);
            FillRound(g, r, 4, fill);
            bool bright = counts[key.vk] > maximum * 45 / 100 && maximum > 0;
            bool selected = register_hits && key.vk == g_app.selected_vk;
            Color border = app_style ? C(204, 211, 202) : (dark_style ? C(57, 86, 68) : C(207, 215, 207));
            StrokeRound(g, r, 4, selected ? C(34, 103, 68) : border, selected ? 2.0f : 1.0f);
            if (app_style) {
                Text(g, key.label, RectF(r.X + 6, r.Y + 3, r.Width - 12, r.Height * .46f), r.Width < 42 ? 8.2f : 9.0f,
                     bright ? C(255, 255, 253) : C(39, 46, 40), FontStyleRegular, StringAlignmentNear, L"Bahnschrift");
                if (counts[key.vk] > 0) Text(g, FormatNumber(counts[key.vk]), RectF(r.X + 6, r.Y + r.Height * .48f, r.Width - 12, r.Height * .40f), 7.5f,
                     bright ? C(226, 240, 229) : C(115, 126, 116), FontStyleRegular, StringAlignmentNear, L"Bahnschrift");
            } else {
                Color label = dark_style ? C(231, 239, 232) : (bright ? C(255, 255, 253) : C(38, 48, 41));
                float label_size = r.Width < 40 ? 8.0f : (r.Width < 54 ? 9.0f : 10.0f);
                Text(g, key.label, RectF(r.X + 4, r.Y, r.Width - 8, r.Height), label_size,
                    label, FontStyleRegular, StringAlignmentCenter, L"Bahnschrift");
            }
            if (register_hits) g_app.key_hitboxes.emplace_back(r, key.vk);
            x += r.Width + gap;
        }
    }
}

std::vector<std::pair<UINT, uint64_t>> TopKeys(size_t count) {
    std::vector<std::pair<UINT, uint64_t>> items;
    const auto& counts = VisibleCounts();
    for (UINT i = 0; i < 256; ++i) if (counts[i] > 0) items.emplace_back(i, counts[i]);
    std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    if (items.size() > count) items.resize(count);
    return items;
}

void DrawRankPanel(Graphics& g, const RectF& panel) {
    FillRound(g, panel, 8, C(255, 255, 253));
    StrokeRound(g, panel, 8, C(216, 222, 214));
    Text(g, L"高频按键", RectF(panel.X + 16, panel.Y + 12, panel.Width - 32, 24), 14, C(30, 39, 30), FontStyleBold);
    Text(g, L"#", RectF(panel.X + 16, panel.Y + 43, 26, 20), 9.5f, C(132, 140, 133), FontStyleRegular, StringAlignmentNear, L"Bahnschrift");
    Text(g, L"按键", RectF(panel.X + 48, panel.Y + 43, 72, 20), 9.5f, C(132, 140, 133));
    Text(g, L"次数", RectF(panel.X + 122, panel.Y + 43, panel.Width - 138, 20), 9.5f, C(132, 140, 133), FontStyleRegular, StringAlignmentFar);
    Pen line(C(229, 233, 227));
    g.DrawLine(&line, panel.X + 14, panel.Y + 66, panel.GetRight() - 14, panel.Y + 66);
    auto top = TopKeys(10);
    for (size_t i = 0; i < 10; ++i) {
        float y = panel.Y + 70 + static_cast<float>(i) * 30;
        if (i >= top.size()) continue;
        Color strong = i < 3 ? C(47, 107, 77) : C(65, 74, 66);
        Text(g, std::to_wstring(i + 1), RectF(panel.X + 16, y, 26, 25), 9.5f, C(136, 144, 137), FontStyleRegular, StringAlignmentNear, L"Bahnschrift");
        Text(g, KeyLabel(top[i].first), RectF(panel.X + 48, y, 74, 25), 10.5f, strong, i < 3 ? FontStyleBold : FontStyleRegular, StringAlignmentNear, L"Bahnschrift");
        Text(g, FormatNumber(top[i].second), RectF(panel.X + 122, y, panel.Width - 138, 25), 10.5f, strong, i < 3 ? FontStyleBold : FontStyleRegular, StringAlignmentFar, L"Bahnschrift");
    }
}

void DrawChart(Graphics& g, const RectF& panel) {
    FillRound(g, panel, 8, C(255, 255, 253));
    StrokeRound(g, panel, 8, C(216, 222, 214));
    Text(g, ViewingToday() ? L"最近 60 分钟" : L"当日节奏", RectF(panel.X + 16, panel.Y + 10, 150, 22), 13, C(30, 39, 30), FontStyleBold);
    Text(g, ViewingToday() ? L"每分钟敲击次数" : L"每 10 分钟敲击次数", RectF(panel.X + 16, panel.Y + 32, 150, 17), 9, C(148, 156, 148));
    RectF chart(panel.X + 170, panel.Y + 14, panel.Width - 188, panel.Height - 26);
    Pen grid(C(232, 236, 230));
    for (int i = 0; i < 3; ++i) {
        float y = chart.Y + chart.Height * static_cast<float>(i) / 2.0f;
        g.DrawLine(&grid, chart.X, y, chart.GetRight(), y);
    }
    std::vector<PointF> points;
    if (ViewingToday()) {
        uint32_t peak = (std::max)(PeakRate(), 1u);
        int64_t now = EpochMinute();
        points.reserve(60);
        for (int i = 0; i < 60; ++i) {
            int64_t stamp = now - 59 + i;
            size_t slot = static_cast<size_t>(stamp % 60);
            uint32_t value = g_app.minute_stamp[slot] == stamp ? g_app.minute_count[slot] : 0;
            float x = chart.X + chart.Width * static_cast<float>(i) / 59.0f;
            float y = chart.GetBottom() - chart.Height * static_cast<float>(value) / static_cast<float>(peak);
            points.emplace_back(x, y);
        }
    } else {
        std::array<uint32_t, 144> buckets{};
        for (size_t i = 0; i < g_app.history_minutes.size(); ++i) buckets[i / 10] += g_app.history_minutes[i];
        uint32_t peak = (std::max)(*std::max_element(buckets.begin(), buckets.end()), 1u);
        points.reserve(buckets.size());
        for (size_t i = 0; i < buckets.size(); ++i) {
            float x = chart.X + chart.Width * static_cast<float>(i) / static_cast<float>(buckets.size() - 1);
            float y = chart.GetBottom() - chart.Height * static_cast<float>(buckets[i]) / static_cast<float>(peak);
            points.emplace_back(x, y);
        }
        if (!g_app.history_has_data || TotalCount() == 0) {
            Text(g, L"这一天没有统计数据", chart, 12, C(132, 140, 133), FontStyleRegular, StringAlignmentCenter);
        }
    }
    if (points.size() > 1) {
        Pen line(C(47, 107, 77), 2.2f);
        line.SetLineJoin(LineJoinRound);
        g.DrawLines(&line, points.data(), static_cast<INT>(points.size()));
        SolidBrush dot(C(47, 107, 77));
        g.FillEllipse(&dot, RectF(points.back().X - 3.0f, points.back().Y - 3.0f, 6.0f, 6.0f));
    }
}

void DrawDashboard(Graphics& g, int width, int height) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    g.Clear(C(245, 247, 245));
    SolidBrush nav(C(255, 255, 253));
    g.FillRectangle(&nav, 0, 0, width, 68);
    Pen nav_line(C(222, 228, 220));
    g.DrawLine(&nav_line, 0, 67, width, 67);
    FillRound(g, RectF(28, 16, 36, 36), 8, C(47, 107, 77));
    Text(g, L"K", RectF(28, 16, 36, 36), 17, C(255, 255, 253), FontStyleBold, StringAlignmentCenter, L"Bahnschrift");
    Text(g, L"KeyPulse", RectF(74, 16, 88, 34), 16, C(27, 35, 27), FontStyleBold, StringAlignmentNear, L"Bahnschrift");
    Text(g, std::wstring(L"v") + kAppVersion, RectF(164, 18, 58, 30), 9.5f, C(118, 128, 119), FontStyleRegular, StringAlignmentNear, L"Bahnschrift");
    float right = static_cast<float>(width) - 28;
    g_app.export_button = RectF(right - 104, 17, 104, 34);
    g_app.update_button = RectF(right - 214, 17, 100, 34);
    g_app.pause_button = RectF(right - 324, 17, 100, 34);
    g_app.reset_button = RectF(right - 418, 17, 84, 34);
    DrawButton(g, g_app.reset_button, L"清空数据");
    DrawButton(g, g_app.pause_button, g_app.running ? L"暂停记录" : L"继续记录", true);
    std::wstring update_label = L"检查更新";
    if (g_app.update_busy == 1 && !g_app.update_check_silent) update_label = L"正在检查";
    else if (g_app.update_busy == 2) update_label = L"正在下载";
    else if (g_app.update_available) update_label = L"更新 " + g_app.available_version;
    DrawButton(g, g_app.update_button, update_label, false, g_app.update_available || g_app.update_busy == 2);
    DrawButton(g, g_app.export_button, L"导出图片");
    float content_w = static_cast<float>(width) - 56;
    Text(g, L"键盘敲击统计", RectF(28, 86, 330, 38), 26, C(25, 31, 26), FontStyleBold);
    g_app.date_button = RectF(static_cast<float>(width) - 250, 88, 222, 36);
    DrawButton(g, g_app.date_button, DateText(g_app.selected_date_key) + L"  ▾");
    float cards_y = 140;
    float card_w = content_w / 4;
    RectF metric_strip(28, cards_y, content_w, 102);
    FillRound(g, metric_strip, 8, C(255, 255, 253));
    StrokeRound(g, metric_strip, 8, C(210, 217, 208));
    Pen metric_divider(C(216, 222, 214));
    for (int i = 1; i < 4; ++i) {
        float divider_x = metric_strip.X + card_w * static_cast<float>(i);
        g.DrawLine(&metric_divider, divider_x, cards_y + 18, divider_x, cards_y + 84);
    }
    if (ViewingToday()) {
        DrawStatCard(g, 28, cards_y, card_w, L"今日敲击", FormatNumber(TotalCount()) + L" 次", g_app.running ? L"正在实时记录" : L"记录已暂停", C(47, 107, 77));
        DrawStatCard(g, 28 + card_w, cards_y, card_w, L"当前速度", std::to_wstring(CurrentRate()) + L" 次/分", L"当前分钟累计", C(47, 107, 77));
        DrawStatCard(g, 28 + card_w * 2, cards_y, card_w, L"峰值速度", std::to_wstring(DayPeakRate()) + L" 次/分", L"今天", C(225, 124, 36));
        DrawStatCard(g, 28 + card_w * 3, cards_y, card_w, L"活跃时间", std::to_wstring(ActiveMinutes()) + L" 分钟", L"今天", C(47, 107, 77));
    } else {
        size_t key_count = static_cast<size_t>(std::count_if(VisibleCounts().begin(), VisibleCounts().end(), [](uint64_t value) { return value > 0; }));
        DrawStatCard(g, 28, cards_y, card_w, L"当日敲击", FormatNumber(TotalCount()) + L" 次", g_app.history_has_data ? L"历史记录" : L"无统计数据", C(47, 107, 77));
        DrawStatCard(g, 28 + card_w, cards_y, card_w, L"峰值速度", std::to_wstring(DayPeakRate()) + L" 次/分", L"当日每分钟峰值", C(225, 124, 36));
        DrawStatCard(g, 28 + card_w * 2, cards_y, card_w, L"活跃时间", std::to_wstring(ActiveMinutes()) + L" 分钟", L"当日", C(47, 107, 77));
        DrawStatCard(g, 28 + card_w * 3, cards_y, card_w, L"使用按键", std::to_wstring(key_count) + L" 个", L"当日", C(47, 107, 77));
    }
    float main_y = 256;
    float rank_w = 252;
    float keyboard_w = content_w - rank_w - 12;
    RectF keyboard_panel(28, main_y, keyboard_w, 398);
    FillRound(g, keyboard_panel, 8, C(255, 255, 253));
    StrokeRound(g, keyboard_panel, 8, C(216, 222, 214));
    Text(g, L"键盘热力图", RectF(44, main_y + 10, 200, 23), 14, C(30, 39, 30), FontStyleBold);
    RectF keyboard(43, main_y + 42, keyboard_w - 30, 298);
    DrawKeyboard(g, keyboard, true);
    RectF selected(43, main_y + 352, keyboard_w - 30, 32);
    FillRound(g, selected, 7, C(240, 243, 237));
    Text(g, L"已选按键", RectF(selected.X + 10, selected.Y, 62, selected.Height), 9, C(135, 144, 136));
    RectF selected_key(selected.X + 75, selected.Y + 5, 48, 22);
    FillRound(g, selected_key, 4, C(35, 45, 34));
    Text(g, KeyLabel(g_app.selected_vk), selected_key, 8.5f, C(232, 239, 228), FontStyleBold, StringAlignmentCenter, L"Bahnschrift");
    MixedValueText(g, FormatNumber(VisibleCounts()[g_app.selected_vk]) + L" 次敲击",
        RectF(selected.X + 134, selected.Y, 170, selected.Height), 10.5f, 9.5f, C(45, 56, 45));
    DrawRankPanel(g, RectF(28 + keyboard_w + 12, main_y, rank_w, 398));
    if (height > 764) DrawChart(g, RectF(28, 668, content_w, static_cast<float>(height) - 696));
    if (g_app.calendar_visible) DrawCalendar(g, width);
}

void DrawShareTotal(Graphics& g, const RectF& rect, float number_size, float unit_size,
                    const Color& color, bool centered = false) {
    std::wstring number = FormatNumber(TotalCount());
    float number_width = TextWidth(g, number, number_size, FontStyleBold, L"Bahnschrift");
    float unit_width = TextWidth(g, L"次", unit_size, FontStyleRegular, L"Microsoft YaHei UI");
    float total_width = number_width + 14.0f + unit_width;
    float x = centered ? rect.X + (rect.Width - total_width) / 2.0f : rect.X;
    Text(g, number, RectF(x, rect.Y, number_width + 4.0f, rect.Height), number_size,
        color, FontStyleBold, StringAlignmentNear, L"Bahnschrift");
    Text(g, L"次", RectF(x + number_width + 14.0f, rect.Y + number_size * .13f,
        unit_width + 8.0f, rect.Height), unit_size, color);
}

void DrawShareCard(Graphics& g, int width, int height, ShareTemplate share_template) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    const std::wstring date = DateText(g_app.selected_date_key, false);
    const std::wstring privacy = L"仅统计敲击次数，不记录输入内容";

    if (share_template == ShareTemplate::Dark) {
        g.Clear(C(20, 45, 33));
        Text(g, L"KeyPulse", RectF(40, 28, 240, 44), 21, C(241, 245, 241),
            FontStyleBold, StringAlignmentNear, L"Bahnschrift");
        Text(g, date, RectF(width - 340.0f, 28, 300, 44), 16, C(151, 177, 158),
            FontStyleRegular, StringAlignmentFar);
        DrawShareTotal(g, RectF(280, 116, width - 560.0f, 180), 104, 40,
            C(242, 246, 242), true);
        DrawKeyboard(g, RectF(108, 328, width - 216.0f, 412), false, KeyboardVisual::ShareDark);
        Text(g, privacy, RectF(450, height - 66.0f, width - 900.0f, 30), 12,
            C(126, 158, 135), FontStyleRegular, StringAlignmentCenter);
        return;
    }

    g.Clear(share_template == ShareTemplate::Gallery ? C(244, 246, 244) : C(248, 249, 247));
    Color primary = C(31, 48, 39);
    Color secondary = C(91, 105, 96);
    if (share_template == ShareTemplate::Gallery) {
        Text(g, L"KeyPulse", RectF(48, 198, 310, 52), 30, primary,
            FontStyleRegular, StringAlignmentNear, L"Bahnschrift");
        Text(g, date, RectF(48, 268, 310, 40), 18, primary);
        Text(g, FormatNumber(TotalCount()), RectF(44, 344, 350, 150), 94, primary,
            FontStyleBold, StringAlignmentNear, L"Bahnschrift");
        Text(g, L"次", RectF(50, 482, 80, 54), 28, primary);
        DrawKeyboard(g, RectF(414, 196, width - 452.0f, 444), false, KeyboardVisual::ShareLight);
        Text(g, privacy, RectF(48, height - 72.0f, 360, 30), 12, C(84, 96, 88));
        return;
    }

    Text(g, L"KeyPulse", RectF(80, 42, 240, 44), 21, primary,
        FontStyleBold, StringAlignmentNear, L"Bahnschrift");
    Text(g, date, RectF(width - 360.0f, 42, 280, 44), 16, primary,
        FontStyleRegular, StringAlignmentFar);
    DrawShareTotal(g, RectF(90, 132, 850, 160), 86, 36, primary);
    DrawKeyboard(g, RectF(78, 332, width - 156.0f, 406), false, KeyboardVisual::ShareLight);
    Text(g, privacy, RectF(width - 500.0f, height - 66.0f, 420, 30), 12,
        secondary, FontStyleRegular, StringAlignmentFar);
}

int GetEncoderClsid(const WCHAR* format, CLSID* clsid) {
    UINT count = 0, bytes = 0;
    GetImageEncodersSize(&count, &bytes);
    if (bytes == 0) return -1;
    std::vector<BYTE> storage(bytes);
    auto* codecs = reinterpret_cast<ImageCodecInfo*>(storage.data());
    GetImageEncoders(count, bytes, codecs);
    for (UINT i = 0; i < count; ++i) if (wcscmp(codecs[i].MimeType, format) == 0) { *clsid = codecs[i].Clsid; return static_cast<int>(i); }
    return -1;
}

enum class SaveImageResult { Cancelled, Saved, Error };

SaveImageResult SaveShareImage(HWND owner, Bitmap& image) {
    wchar_t file[MAX_PATH]{};
    SYSTEMTIME selected = DateKeyToSystemTime(g_app.selected_date_key);
    swprintf_s(file, L"KeyPulse-%04d-%02d-%02d.png", selected.wYear, selected.wMonth, selected.wDay);
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = L"PNG 图片 (*.png)\0*.png\0所有文件 (*.*)\0*.*\0";
    dialog.lpstrFile = file;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrDefExt = L"png";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&dialog)) return SaveImageResult::Cancelled;
    CLSID encoder{};
    if (GetEncoderClsid(L"image/png", &encoder) < 0) {
        MessageBoxW(owner, L"系统中没有可用的 PNG 编码器。", L"导出失败", MB_OK | MB_ICONERROR);
        return SaveImageResult::Error;
    }
    Status status = image.Save(file, &encoder, nullptr);
    if (status == Ok) {
        MessageBoxW(owner, L"PNG 分享图已成功导出。", L"KeyPulse", MB_OK | MB_ICONINFORMATION);
        return SaveImageResult::Saved;
    }
    MessageBoxW(owner, L"图片导出失败，请尝试选择其他保存位置。", L"KeyPulse", MB_OK | MB_ICONERROR);
    return SaveImageResult::Error;
}

bool RefreshPreviewImage() {
    auto image = std::make_unique<Bitmap>(1600, 900, PixelFormat32bppARGB);
    if (image->GetLastStatus() != Ok) return false;
    Graphics graphics(image.get());
    DrawShareCard(graphics, 1600, 900, g_preview.selected_template);
    g_preview.image = std::move(image);
    return true;
}

void DrawPreviewTemplateButton(Graphics& g, const RectF& button, const std::wstring& label, bool selected) {
    FillRound(g, button, 8, selected ? C(47, 107, 77) : C(250, 251, 249));
    StrokeRound(g, button, 8, selected ? C(47, 107, 77) : C(211, 219, 211));
    Text(g, label, button, 10.5f, selected ? C(248, 251, 247) : C(65, 77, 68),
        selected ? FontStyleBold : FontStyleRegular, StringAlignmentCenter);
}

void SavePreviewAndClose(HWND window) {
    if (!g_preview.image) return;
    if (SaveShareImage(window, *g_preview.image) == SaveImageResult::Saved) DestroyWindow(window);
}

LRESULT CALLBACK PreviewWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(window, &ps);
        RECT client{};
        GetClientRect(window, &client);
        int width = (std::max)(client.right, 1);
        int height = (std::max)(client.bottom, 1);
        Bitmap buffer(width, height, PixelFormat32bppPARGB);
        Graphics graphics(&buffer);
        graphics.SetSmoothingMode(SmoothingModeAntiAlias);
        graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        graphics.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        graphics.Clear(C(245, 247, 245));

        Text(graphics, L"分享图片预览", RectF(24, 16, 170, 38), 14, C(31, 42, 34), FontStyleBold);
        constexpr const wchar_t* labels[] = {L"A1  均衡留白", L"B  深色沉浸", L"C  非对称画廊"};
        for (size_t i = 0; i < g_preview.template_buttons.size(); ++i) {
            g_preview.template_buttons[i] = RectF(210.0f + static_cast<float>(i) * 142.0f, 16, 130, 38);
            DrawPreviewTemplateButton(graphics, g_preview.template_buttons[i], labels[i],
                static_cast<int>(g_preview.selected_template) == static_cast<int>(i));
        }

        constexpr float margin = 24.0f;
        RectF available(margin, 72, static_cast<float>(width) - margin * 2.0f,
            static_cast<float>(height) - 72.0f - 92.0f);
        float preview_width = available.Width;
        float preview_height = preview_width * 9.0f / 16.0f;
        if (preview_height > available.Height) {
            preview_height = available.Height;
            preview_width = preview_height * 16.0f / 9.0f;
        }
        RectF preview(available.X + (available.Width - preview_width) / 2.0f,
            available.Y + (available.Height - preview_height) / 2.0f, preview_width, preview_height);
        FillRound(graphics, RectF(preview.X + 3, preview.Y + 5, preview.Width, preview.Height), 8, C(48, 72, 53, 24));
        if (g_preview.image) graphics.DrawImage(g_preview.image.get(), preview);
        StrokeRound(graphics, preview, 6, C(204, 213, 204));

        float button_y = static_cast<float>(height) - 58.0f;
        g_preview.save_button = RectF(static_cast<float>(width) - 160.0f, button_y, 132.0f, 38.0f);
        g_preview.cancel_button = RectF(g_preview.save_button.X - 102.0f, button_y, 90.0f, 38.0f);
        Text(graphics, L"1600 × 900 PNG", RectF(28, button_y, 220, 38), 10.5f, C(105, 115, 106),
            FontStyleRegular, StringAlignmentNear, L"Bahnschrift");
        DrawButton(graphics, g_preview.cancel_button, L"取消");
        DrawButton(graphics, g_preview.save_button, L"保存图片", false, true);

        Graphics screen(dc);
        screen.DrawImage(&buffer, 0, 0);
        EndPaint(window, &ps);
        return 0;
    }
    case WM_LBUTTONUP: {
        float x = static_cast<float>(GET_X_LPARAM(lparam));
        float y = static_cast<float>(GET_Y_LPARAM(lparam));
        for (size_t i = 0; i < g_preview.template_buttons.size(); ++i) {
            if (!PointInside(g_preview.template_buttons[i], x, y)) continue;
            g_preview.selected_template = static_cast<ShareTemplate>(i);
            if (!RefreshPreviewImage()) {
                MessageBoxW(window, L"无法生成分享图预览。", L"预览失败", MB_OK | MB_ICONERROR);
            }
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        if (PointInside(g_preview.save_button, x, y)) SavePreviewAndClose(window);
        else if (PointInside(g_preview.cancel_button, x, y)) DestroyWindow(window);
        return 0;
    }
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) DestroyWindow(window);
        else if (wparam == VK_RETURN) SavePreviewAndClose(window);
        return 0;
    case WM_SIZE:
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
        info->ptMinTrackSize.x = 820;
        info->ptMinTrackSize.y = 590;
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        g_preview.window = nullptr;
        g_preview.image.reset();
        if (g_preview.owner && IsWindow(g_preview.owner)) {
            EnableWindow(g_preview.owner, TRUE);
            SetForegroundWindow(g_preview.owner);
        }
        g_preview.owner = nullptr;
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

void ShowExportPreview(HWND owner) {
    if (g_preview.window) {
        ShowWindow(g_preview.window, SW_RESTORE);
        SetForegroundWindow(g_preview.window);
        return;
    }
    g_preview.selected_template = ShareTemplate::Balanced;
    if (!RefreshPreviewImage()) {
        MessageBoxW(owner, L"无法生成分享图预览。", L"导出失败", MB_OK | MB_ICONERROR);
        return;
    }

    constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME;
    RECT desired{0, 0, 1180, 760};
    AdjustWindowRectEx(&desired, style, FALSE, WS_EX_DLGMODALFRAME);
    int window_width = desired.right - desired.left;
    int window_height = desired.bottom - desired.top;
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    window_width = (std::min)(window_width, work.right - work.left - 48);
    window_height = (std::min)(window_height, work.bottom - work.top - 48);
    RECT owner_rect{};
    GetWindowRect(owner, &owner_rect);
    int x = owner_rect.left + ((owner_rect.right - owner_rect.left) - window_width) / 2;
    int y = owner_rect.top + ((owner_rect.bottom - owner_rect.top) - window_height) / 2;
    x = (std::max)(work.left + 24, (std::min)(x, work.right - window_width - 24));
    y = (std::max)(work.top + 24, (std::min)(y, work.bottom - window_height - 24));

    g_preview.owner = owner;
    EnableWindow(owner, FALSE);
    g_preview.window = CreateWindowExW(WS_EX_DLGMODALFRAME, kPreviewWindowClass, L"分享图片预览",
        style, x, y, window_width, window_height, owner, nullptr, g_app.instance, nullptr);
    if (!g_preview.window) {
        EnableWindow(owner, TRUE);
        g_preview.owner = nullptr;
        g_preview.image.reset();
        MessageBoxW(owner, L"无法打开分享图预览窗口。", L"导出失败", MB_OK | MB_ICONERROR);
        return;
    }
    ShowWindow(g_preview.window, SW_SHOW);
    UpdateWindow(g_preview.window);
    SetFocus(g_preview.window);
}

void ShowMainWindow() {
    ShowWindow(g_app.window, SW_RESTORE);
    SetForegroundWindow(g_app.window);
    InvalidateRect(g_app.window, nullptr, FALSE);
}

void ShowCalendar() {
    SYSTEMTIME selected = DateKeyToSystemTime(g_app.selected_date_key);
    g_app.calendar_year = selected.wYear;
    g_app.calendar_month = selected.wMonth;
    g_app.calendar_visible = true;
    RefreshCalendarHistory();
    InvalidateRect(g_app.window, nullptr, FALSE);
}

void HideCalendar() {
    if (!g_app.calendar_visible) return;
    g_app.calendar_visible = false;
    InvalidateRect(g_app.window, nullptr, FALSE);
}

bool HandleCalendarClick(float x, float y) {
    if (!g_app.calendar_visible || !PointInside(g_app.calendar_bounds, x, y)) return false;
    if (PointInside(g_app.calendar_previous_button, x, y)) {
        ShiftCalendarMonth(-1);
        return true;
    }
    if (PointInside(g_app.calendar_next_button, x, y)) {
        ShiftCalendarMonth(1);
        return true;
    }
    if (PointInside(g_app.calendar_today_button, x, y)) {
        SelectDate(TodayKey());
        HideCalendar();
        return true;
    }
    for (size_t i = 0; i < g_app.calendar_date_bounds.size(); ++i) {
        if (PointInside(g_app.calendar_date_bounds[i], x, y) && g_app.calendar_date_keys[i] <= TodayKey()) {
            SelectDate(g_app.calendar_date_keys[i]);
            HideCalendar();
            return true;
        }
    }
    return true;
}

void UpdateTrayTip() {
    const wchar_t* tip = g_app.update_available ? L"KeyPulse · 有新版本可用" :
        (g_app.running ? L"KeyPulse · 正在记录" : L"KeyPulse · 已暂停");
    wcscpy_s(g_app.tray.szTip, tip);
    Shell_NotifyIconW(NIM_MODIFY, &g_app.tray);
}

void ToggleRunning() {
    g_app.running = !g_app.running;
    g_app.dirty = true;
    UpdateTrayTip();
    InvalidateRect(g_app.window, nullptr, FALSE);
}

void ResetToday(HWND owner) {
    std::wstring date = DateText(g_app.selected_date_key, false);
    std::wstring message = L"确定清空 " + date + L" 的全部敲击统计吗？此操作无法撤销。";
    if (MessageBoxW(owner, message.c_str(), L"清空当天数据", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    if (ViewingToday()) {
        ResetForNewDay();
        SaveData();
    } else {
        DeleteFileW(HistoryPath(g_app.selected_date_key).c_str());
        g_app.history_counts.fill(0);
        g_app.history_minutes.fill(0);
        g_app.history_has_data = false;
    }
    InvalidateRect(g_app.window, nullptr, FALSE);
}

UINT NormalizeRawKey(const RAWKEYBOARD& keyboard) {
    UINT vk = keyboard.VKey;
    if (vk == 255) return 0;
    if (vk == VK_SHIFT) return MapVirtualKeyW(keyboard.MakeCode, MAPVK_VSC_TO_VK_EX);
    if (vk == VK_CONTROL) return (keyboard.Flags & RI_KEY_E0) ? VK_RCONTROL : VK_LCONTROL;
    if (vk == VK_MENU) return (keyboard.Flags & RI_KEY_E0) ? VK_RMENU : VK_LMENU;
    return vk;
}

void RecordRawKey(const RAWKEYBOARD& keyboard) {
    UINT vk = NormalizeRawKey(keyboard);
    if (vk == 0 || vk >= g_app.key_down.size()) return;
    bool released = (keyboard.Flags & RI_KEY_BREAK) != 0;
    if (released) {
        g_app.key_down[vk] = false;
        return;
    }
    // Press state is tracked per virtual key. Holding Alt does not block a new
    // Tab press, and holding Ctrl does not block C or V from being counted.
    if (g_app.key_down[vk]) return;
    g_app.key_down[vk] = true;
    if (!g_app.running) return;
    if (TodayKey() != g_app.date_key) ResetForNewDay();
    ++g_app.counts[vk];
    int64_t minute = EpochMinute();
    size_t slot = static_cast<size_t>(minute % 60);
    if (g_app.minute_stamp[slot] != minute) {
        g_app.minute_stamp[slot] = minute;
        g_app.minute_count[slot] = 0;
    }
    ++g_app.minute_count[slot];
    ++g_app.day_minutes[static_cast<size_t>(CurrentMinuteOfDay())];
    g_app.dirty = true;
}

void HandleRawInput(LPARAM lparam) {
    RAWINPUT input{};
    UINT size = sizeof(input);
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, &input, &size,
        sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1)) return;
    if (input.header.dwType == RIM_TYPEKEYBOARD) RecordRawKey(input.data.keyboard);
}

bool RegisterKeyboardInput(HWND window) {
    RAWINPUTDEVICE device{};
    device.usUsagePage = 0x01;
    device.usUsage = 0x06;
    device.dwFlags = RIDEV_INPUTSINK;
    device.hwndTarget = window;
    return RegisterRawInputDevices(&device, 1, sizeof(device)) == TRUE;
}

void AddTrayIcon(HWND window) {
    g_app.tray = {};
    g_app.tray.cbSize = sizeof(g_app.tray);
    g_app.tray.hWnd = window;
    g_app.tray.uID = 1;
    g_app.tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    g_app.tray.uCallbackMessage = WM_TRAYICON;
    g_app.tray.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_app.tray.szTip, L"KeyPulse · 正在记录");
    Shell_NotifyIconW(NIM_ADD, &g_app.tray);
    g_app.tray.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_app.tray);
}

void ShowTrayMenu(HWND window) {
    POINT point{};
    GetCursorPos(&point);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_TRAY_OPEN, L"打开 KeyPulse");
    AppendMenuW(menu, MF_STRING, ID_TRAY_PAUSE, g_app.running ? L"暂停记录" : L"继续记录");
    AppendMenuW(menu, MF_STRING, ID_TRAY_EXPORT, L"导出 PNG 分享图");
    std::wstring update_label = g_app.update_available ? L"更新 " + g_app.available_version : L"检查更新";
    AppendMenuW(menu, MF_STRING, ID_TRAY_UPDATE, update_label.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"退出");
    SetForegroundWindow(window);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, point.x, point.y, 0, window, nullptr);
    DestroyMenu(menu);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE:
        if (!RegisterKeyboardInput(window)) {
            MessageBoxW(window, L"无法注册 Windows Raw Input 键盘输入。", L"KeyPulse", MB_OK | MB_ICONERROR);
            return -1;
        }
        AddTrayIcon(window);
        SetTimer(window, TIMER_UI, 500, nullptr);
        SetTimer(window, TIMER_UPDATE, UPDATE_INTERVAL_MS, nullptr);
        StartUpdateCheck(window, true);
        return 0;
    case WM_INPUT:
        HandleRawInput(lparam);
        return DefWindowProcW(window, message, wparam, lparam);
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(window, &ps);
        RECT client{};
        GetClientRect(window, &client);
        Bitmap buffer(client.right, client.bottom, PixelFormat32bppPARGB);
        Graphics graphics(&buffer);
        DrawDashboard(graphics, client.right, client.bottom);
        Graphics screen(dc);
        screen.DrawImage(&buffer, 0, 0);
        EndPaint(window, &ps);
        return 0;
    }
    case WM_TIMER:
        if (wparam == TIMER_UPDATE) {
            StartUpdateCheck(window, true);
            return 0;
        }
        if (TodayKey() != g_app.date_key) ResetForNewDay();
        if (g_app.dirty && GetTickCount64() - g_app.last_save_tick >= 15000) SaveData();
        if (IsWindowVisible(window)) InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_UPDATE_RESULT: {
        std::unique_ptr<UpdateResult> result(reinterpret_cast<UpdateResult*>(lparam));
        bool silent = g_app.update_check_silent;
        g_app.update_busy = 0;
        g_app.update_check_silent = false;
        InvalidateRect(window, nullptr, FALSE);
        if (!result) return 0;
        if (result->kind == UpdateResultKind::UpToDate) {
            g_app.update_available = false;
            g_app.available_version.clear();
            g_app.available_download_url.clear();
            g_app.available_checksum_url.clear();
            UpdateTrayTip();
            if (!silent) MessageBoxW(window, L"当前已经是最新版本。", L"KeyPulse 更新", MB_OK | MB_ICONINFORMATION);
        } else if (result->kind == UpdateResultKind::Available) {
            g_app.update_available = true;
            g_app.available_version = result->version;
            g_app.available_download_url = result->download_url;
            g_app.available_checksum_url = result->checksum_url;
            UpdateTrayTip();
            InvalidateRect(window, nullptr, FALSE);
        } else if (result->kind == UpdateResultKind::Downloaded) {
            std::wstring error;
            if (LaunchUpdateInstaller(result->downloaded_file, error)) {
                SaveData();
                g_app.exit_requested = true;
                SendMessageW(window, WM_CLOSE, 0, 0);
            } else {
                MessageBoxW(window, error.c_str(), L"更新失败", MB_OK | MB_ICONERROR);
            }
        } else {
            std::wstring message_text = result->message.empty() ? L"检查更新失败，请稍后重试。" : result->message;
            if (!silent) MessageBoxW(window, message_text.c_str(), L"更新失败", MB_OK | MB_ICONERROR);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        float x = static_cast<float>(GET_X_LPARAM(lparam));
        float y = static_cast<float>(GET_Y_LPARAM(lparam));
        if (HandleCalendarClick(x, y)) return 0;
        if (PointInside(g_app.date_button, x, y)) {
            if (g_app.calendar_visible) HideCalendar(); else ShowCalendar();
            return 0;
        }
        HideCalendar();
        if (PointInside(g_app.pause_button, x, y)) ToggleRunning();
        else if (PointInside(g_app.reset_button, x, y)) ResetToday(window);
        else if (PointInside(g_app.update_button, x, y)) {
            if (g_app.update_available && g_app.update_busy == 0) {
                StartUpdateDownload(window, g_app.available_version, g_app.available_download_url,
                    g_app.available_checksum_url);
            } else {
                StartUpdateCheck(window, false);
            }
        }
        else if (PointInside(g_app.export_button, x, y)) ShowExportPreview(window);
        else for (const auto& item : g_app.key_hitboxes) if (PointInside(item.first, x, y)) { g_app.selected_vk = item.second; InvalidateRect(window, nullptr, FALSE); break; }
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (g_app.calendar_visible) {
            ShiftCalendarMonth(GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? -1 : 1);
            return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    case WM_SIZE:
        HideCalendar();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
        info->ptMinTrackSize.x = 1160;
        info->ptMinTrackSize.y = 800;
        return 0;
    }
    case WM_CLOSE:
        if (!g_app.exit_requested) {
            ShowWindow(window, SW_HIDE);
            if (!g_app.tray_hint_shown) {
                g_app.tray.uFlags = NIF_INFO;
                wcscpy_s(g_app.tray.szInfoTitle, L"KeyPulse 仍在记录");
                wcscpy_s(g_app.tray.szInfo, L"程序已缩小到系统托盘，右键托盘图标可以退出。");
                g_app.tray.dwInfoFlags = NIIF_INFO;
                Shell_NotifyIconW(NIM_MODIFY, &g_app.tray);
                g_app.tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
                g_app.tray_hint_shown = true;
            }
            return 0;
        }
        DestroyWindow(window);
        return 0;
    case WM_TRAYICON:
        if (LOWORD(lparam) == WM_LBUTTONDBLCLK || LOWORD(lparam) == NIN_SELECT || LOWORD(lparam) == NIN_KEYSELECT) ShowMainWindow();
        else if (LOWORD(lparam) == WM_CONTEXTMENU || LOWORD(lparam) == WM_RBUTTONUP) ShowTrayMenu(window);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case ID_TRAY_OPEN: ShowMainWindow(); break;
        case ID_TRAY_PAUSE: ToggleRunning(); break;
        case ID_TRAY_EXPORT: ShowMainWindow(); ShowExportPreview(window); break;
        case ID_TRAY_UPDATE:
            ShowMainWindow();
            if (g_app.update_available && g_app.update_busy == 0) {
                StartUpdateDownload(window, g_app.available_version, g_app.available_download_url,
                    g_app.available_checksum_url);
            } else {
                StartUpdateCheck(window, false);
            }
            break;
        case ID_TRAY_EXIT: g_app.exit_requested = true; SendMessageW(window, WM_CLOSE, 0, 0); break;
        default: break;
        }
        return 0;
    case WM_DESTROY:
        KillTimer(window, TIMER_UI);
        KillTimer(window, TIMER_UPDATE);
        SaveData();
        Shell_NotifyIconW(NIM_DELETE, &g_app.tray);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    int argument_count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    if (arguments && argument_count == 5 && wcscmp(arguments[1], L"--apply-update") == 0) {
        fs::path source = arguments[2];
        fs::path target = arguments[3];
        DWORD parent_process_id = wcstoul(arguments[4], nullptr, 10);
        LocalFree(arguments);
        return ApplyDownloadedUpdate(source, target, parent_process_id);
    }
    if (arguments) LocalFree(arguments);
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\KeyPulse-SingleInstance-6B1A5F7C");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(kWindowClass, nullptr)) { ShowWindow(existing, SW_RESTORE); SetForegroundWindow(existing); }
        CloseHandle(mutex);
        return 0;
    }
    SetProcessDPIAware();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    GdiplusStartupInput gdiplus_input;
    GdiplusStartup(&g_app.gdiplus_token, &gdiplus_input, nullptr);
    g_app.instance = instance;
    LoadData();
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = instance;
    wc.lpfnWndProc = WindowProc;
    wc.lpszClassName = kWindowClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!RegisterClassExW(&wc)) return 1;
    WNDCLASSEXW preview_class = wc;
    preview_class.lpfnWndProc = PreviewWindowProc;
    preview_class.lpszClassName = kPreviewWindowClass;
    if (!RegisterClassExW(&preview_class)) return 1;
    RECT desired{0, 0, 1360, 880};
    AdjustWindowRectEx(&desired, WS_OVERLAPPEDWINDOW, FALSE, 0);
    int screen_w = GetSystemMetrics(SM_CXSCREEN), screen_h = GetSystemMetrics(SM_CYSCREEN);
    g_app.window = CreateWindowExW(0, kWindowClass, kAppName, WS_OVERLAPPEDWINDOW,
        (screen_w - (desired.right - desired.left)) / 2, (screen_h - (desired.bottom - desired.top)) / 2,
        desired.right - desired.left, desired.bottom - desired.top, nullptr, nullptr, instance, nullptr);
    if (!g_app.window) return 1;
    ShowWindow(g_app.window, show_command);
    UpdateWindow(g_app.window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    GdiplusShutdown(g_app.gdiplus_token);
    CoUninitialize();
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return static_cast<int>(message.wParam);
}
