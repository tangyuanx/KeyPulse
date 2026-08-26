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
constexpr wchar_t kAppName[] = L"KeyPulse";
constexpr wchar_t kAppVersion[] = L"0.2.1";
constexpr wchar_t kLatestReleaseApi[] = L"https://api.github.com/repos/tangyuanx/KeyPulse/releases/latest";
constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT WM_UPDATE_RESULT = WM_APP + 2;
constexpr UINT_PTR TIMER_UI = 1;
constexpr UINT ID_TRAY_OPEN = 1001;
constexpr UINT ID_TRAY_PAUSE = 1002;
constexpr UINT ID_TRAY_EXPORT = 1003;
constexpr UINT ID_TRAY_UPDATE = 1004;
constexpr UINT ID_TRAY_EXIT = 1005;
constexpr uint32_t DATA_MAGIC = 0x4B50554C; // KPUL

struct KeyDef { const wchar_t* label; UINT vk; float units; };

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

struct AppState {
    HWND window = nullptr;
    HINSTANCE instance = nullptr;
    NOTIFYICONDATAW tray{};
    ULONG_PTR gdiplus_token = 0;
    std::array<uint64_t, 256> counts{};
    std::array<bool, 256> key_down{};
    std::array<int64_t, 60> minute_stamp{};
    std::array<uint32_t, 60> minute_count{};
    bool running = true;
    bool exit_requested = false;
    bool dirty = false;
    bool tray_hint_shown = false;
    int date_key = 0;
    UINT selected_vk = VK_SPACE;
    ULONGLONG last_save_tick = 0;
    RectF pause_button{};
    RectF reset_button{};
    RectF export_button{};
    RectF update_button{};
    int update_busy = 0;
    std::vector<std::pair<RectF, UINT>> key_hitboxes;
    fs::path data_directory;
};

AppState g_app;

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

std::wstring TodayText() {
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    wchar_t value[64]{};
    swprintf_s(value, L"%d年%d月%d日 · 今天", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
    return value;
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
    // Keep the entire interface on one predictable Chinese UI typeface.
    // The parameter remains for source compatibility, but intentional font
    // mixing is disabled because it makes Chinese and numbers feel unrelated.
    (void)family_name;
    FontFamily family(L"Microsoft YaHei UI");
    Font font(&family, size, style, UnitPixel);
    SolidBrush brush(color);
    StringFormat format;
    format.SetAlignment(align);
    format.SetLineAlignment(StringAlignmentCenter);
    format.SetTrimming(StringTrimmingEllipsisCharacter);
    g.DrawString(text.c_str(), -1, &font, rect, &format, &brush);
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
    return std::accumulate(g_app.counts.begin(), g_app.counts.end(), uint64_t{0});
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
    uint32_t active = 0;
    int64_t now = EpochMinute();
    for (size_t i = 0; i < 60; ++i) {
        if (g_app.minute_stamp[i] > now - 60 && g_app.minute_count[i] > 0) ++active;
    }
    return active;
}

void ResetForNewDay() {
    g_app.counts.fill(0);
    g_app.minute_stamp.fill(0);
    g_app.minute_count.fill(0);
    g_app.date_key = TodayKey();
    g_app.dirty = true;
}

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

bool HttpGet(const std::wstring& url, std::vector<unsigned char>& body, std::wstring& error,
             size_t maximum_bytes = 100 * 1024 * 1024) {
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) {
        error = L"无法解析更新地址，错误代码 " + std::to_wstring(GetLastError());
        return false;
    }
    std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength > 0) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    std::wstring user_agent = std::wstring(L"KeyPulse/") + kAppVersion;
    WinHttpHandle session(WinHttpOpen(user_agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        error = L"无法初始化网络连接，错误代码 " + std::to_wstring(GetLastError());
        return false;
    }
    WinHttpSetTimeouts(session, 5000, 5000, 10000, 30000);
    WinHttpHandle connection(WinHttpConnect(session, host.c_str(), parts.nPort, 0));
    if (!connection) {
        error = L"无法连接 GitHub，错误代码 " + std::to_wstring(GetLastError());
        return false;
    }
    DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    WinHttpHandle request(WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!request) {
        error = L"无法创建更新请求，错误代码 " + std::to_wstring(GetLastError());
        return false;
    }
    const wchar_t headers[] = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    if (!WinHttpSendRequest(request, headers, static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        error = L"GitHub 请求失败，错误代码 " + std::to_wstring(GetLastError());
        return false;
    }
    DWORD status = 0, status_size = sizeof(status);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX) || status != 200) {
        error = L"GitHub 返回状态码 " + std::to_wstring(status);
        return false;
    }
    body.clear();
    std::array<unsigned char, 16384> buffer{};
    while (true) {
        DWORD bytes_read = 0;
        if (!WinHttpReadData(request, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes_read)) {
            error = L"读取更新数据失败，错误代码 " + std::to_wstring(GetLastError());
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

void StartUpdateCheck(HWND window) {
    if (g_app.update_busy != 0) return;
    g_app.update_busy = 1;
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
        g_app.dirty = false;
        g_app.last_save_tick = GetTickCount64();
    } catch (...) { }
}

void LoadData() {
    g_app.data_directory = ResolveDataDirectory();
    g_app.date_key = TodayKey();
    try {
        std::ifstream stream(g_app.data_directory / L"keypulse.dat", std::ios::binary);
        PersistedData data;
        stream.read(reinterpret_cast<char*>(&data), sizeof(data));
        if (stream.gcount() == sizeof(data) && data.magic == DATA_MAGIC && data.version == 1 && data.date_key == g_app.date_key) {
            std::copy(std::begin(data.counts), std::end(data.counts), g_app.counts.begin());
            std::copy(std::begin(data.minute_stamp), std::end(data.minute_stamp), g_app.minute_stamp.begin());
            std::copy(std::begin(data.minute_count), std::end(data.minute_count), g_app.minute_count.begin());
            g_app.running = data.paused == 0;
        }
    } catch (...) { }
    g_app.last_save_tick = GetTickCount64();
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

void DrawStatCard(Graphics& g, float x, float y, float w, const std::wstring& label,
                  const std::wstring& value, const std::wstring& note, const Color& accent) {
    Text(g, label, RectF(x + 22, y + 12, w - 44, 20), 11, C(76, 84, 77));
    Text(g, value, RectF(x + 22, y + 34, w - 44, 32), 23, accent, FontStyleBold);
    Text(g, note, RectF(x + 22, y + 70, w - 44, 17), 9, C(133, 141, 134));
}

void DrawButton(Graphics& g, const RectF& r, const std::wstring& label, bool primary = false) {
    FillRound(g, r, 8, primary ? C(28, 38, 28) : C(251, 252, 249));
    StrokeRound(g, r, 8, primary ? C(28, 38, 28) : C(218, 225, 216));
    Text(g, label, r, 11, primary ? C(245, 248, 241) : C(73, 84, 74), FontStyleRegular, StringAlignmentCenter);
}

void DrawKeyboard(Graphics& g, const RectF& bounds, bool register_hits) {
    FillRound(g, bounds, 8, C(245, 247, 243));
    StrokeRound(g, bounds, 8, C(218, 224, 216));
    auto rows = KeyboardRows();
    uint64_t maximum = *std::max_element(g_app.counts.begin(), g_app.counts.end());
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
            Color fill = HeatColor(g_app.counts[key.vk], maximum);
            FillRound(g, r, 4, fill);
            bool bright = g_app.counts[key.vk] > maximum * 45 / 100 && maximum > 0;
            bool selected = register_hits && key.vk == g_app.selected_vk;
            StrokeRound(g, r, 4, selected ? C(34, 103, 68) : C(204, 211, 202), selected ? 2.0f : 1.0f);
            Text(g, key.label, RectF(r.X + 6, r.Y + 3, r.Width - 12, r.Height * .46f), r.Width < 42 ? 8.0f : 9.0f,
                 bright ? C(255, 255, 253) : C(39, 46, 40), FontStyleRegular);
            if (g_app.counts[key.vk] > 0) Text(g, FormatNumber(g_app.counts[key.vk]), RectF(r.X + 6, r.Y + r.Height * .48f, r.Width - 12, r.Height * .40f), 7,
                 bright ? C(226, 240, 229) : C(115, 126, 116));
            if (register_hits) g_app.key_hitboxes.emplace_back(r, key.vk);
            x += r.Width + gap;
        }
    }
}

std::vector<std::pair<UINT, uint64_t>> TopKeys(size_t count) {
    std::vector<std::pair<UINT, uint64_t>> items;
    for (UINT i = 0; i < 256; ++i) if (g_app.counts[i] > 0) items.emplace_back(i, g_app.counts[i]);
    std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    if (items.size() > count) items.resize(count);
    return items;
}

void DrawRankPanel(Graphics& g, const RectF& panel) {
    FillRound(g, panel, 8, C(255, 255, 253));
    StrokeRound(g, panel, 8, C(216, 222, 214));
    Text(g, L"高频按键", RectF(panel.X + 16, panel.Y + 12, panel.Width - 32, 24), 13, C(30, 39, 30), FontStyleBold);
    Text(g, L"#", RectF(panel.X + 16, panel.Y + 43, 26, 20), 9, C(132, 140, 133));
    Text(g, L"按键", RectF(panel.X + 48, panel.Y + 43, 72, 20), 9, C(132, 140, 133));
    Text(g, L"次数", RectF(panel.X + 122, panel.Y + 43, panel.Width - 138, 20), 9, C(132, 140, 133), FontStyleRegular, StringAlignmentFar);
    Pen line(C(229, 233, 227));
    g.DrawLine(&line, panel.X + 14, panel.Y + 66, panel.GetRight() - 14, panel.Y + 66);
    auto top = TopKeys(10);
    for (size_t i = 0; i < 10; ++i) {
        float y = panel.Y + 70 + static_cast<float>(i) * 30;
        if (i >= top.size()) continue;
        Color strong = i < 3 ? C(47, 107, 77) : C(65, 74, 66);
        Text(g, std::to_wstring(i + 1), RectF(panel.X + 16, y, 26, 25), 9, C(136, 144, 137));
        Text(g, KeyLabel(top[i].first), RectF(panel.X + 48, y, 74, 25), 10, strong, i < 3 ? FontStyleBold : FontStyleRegular);
        Text(g, FormatNumber(top[i].second), RectF(panel.X + 122, y, panel.Width - 138, 25), 10, strong, i < 3 ? FontStyleBold : FontStyleRegular, StringAlignmentFar);
    }
}

void DrawChart(Graphics& g, const RectF& panel) {
    FillRound(g, panel, 8, C(255, 255, 253));
    StrokeRound(g, panel, 8, C(216, 222, 214));
    Text(g, L"最近 60 分钟", RectF(panel.X + 16, panel.Y + 10, 150, 22), 12, C(30, 39, 30), FontStyleBold);
    Text(g, L"每分钟敲击次数", RectF(panel.X + 16, panel.Y + 32, 150, 16), 8, C(148, 156, 148));
    RectF chart(panel.X + 170, panel.Y + 14, panel.Width - 188, panel.Height - 26);
    uint32_t peak = (std::max)(PeakRate(), 1u);
    int64_t now = EpochMinute();
    Pen grid(C(232, 236, 230));
    for (int i = 0; i < 3; ++i) {
        float y = chart.Y + chart.Height * static_cast<float>(i) / 2.0f;
        g.DrawLine(&grid, chart.X, y, chart.GetRight(), y);
    }
    std::vector<PointF> points;
    points.reserve(60);
    for (int i = 0; i < 60; ++i) {
        int64_t stamp = now - 59 + i;
        size_t slot = static_cast<size_t>(stamp % 60);
        uint32_t value = g_app.minute_stamp[slot] == stamp ? g_app.minute_count[slot] : 0;
        float x = chart.X + chart.Width * static_cast<float>(i) / 59.0f;
        float y = chart.GetBottom() - chart.Height * static_cast<float>(value) / static_cast<float>(peak);
        points.emplace_back(x, y);
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
    Text(g, L"K", RectF(28, 16, 36, 36), 17, C(255, 255, 253), FontStyleBold, StringAlignmentCenter);
    Text(g, L"KeyPulse", RectF(74, 14, 150, 22), 16, C(27, 35, 27), FontStyleBold);
    Text(g, std::wstring(L"输入节奏仪表盘 · v") + kAppVersion, RectF(74, 35, 190, 16), 9, C(118, 128, 119));
    float right = static_cast<float>(width) - 28;
    g_app.export_button = RectF(right - 104, 17, 104, 34);
    g_app.update_button = RectF(right - 214, 17, 100, 34);
    g_app.pause_button = RectF(right - 324, 17, 100, 34);
    g_app.reset_button = RectF(right - 418, 17, 84, 34);
    DrawButton(g, g_app.reset_button, L"清空数据");
    DrawButton(g, g_app.pause_button, g_app.running ? L"暂停记录" : L"继续记录", true);
    DrawButton(g, g_app.update_button, g_app.update_busy == 1 ? L"正在检查" :
        (g_app.update_busy == 2 ? L"正在下载" : L"检查更新"));
    DrawButton(g, g_app.export_button, L"导出图片");
    float content_w = static_cast<float>(width) - 56;
    Text(g, L"键盘敲击统计", RectF(28, 83, 330, 38), 27, C(25, 31, 26), FontStyleBold);
    Text(g, L"查看今天的输入节奏与按键分布", RectF(28, 121, 380, 22), 11, C(111, 120, 112));
    Text(g, L"仅统计次数，不记录输入内容", RectF(static_cast<float>(width) - 360, 86, 332, 24), 11, C(83, 99, 85), FontStyleRegular, StringAlignmentFar);
    Text(g, TodayText(), RectF(static_cast<float>(width) - 360, 114, 332, 20), 9, C(139, 147, 140), FontStyleRegular, StringAlignmentFar);
    float cards_y = 156;
    float card_w = content_w / 4;
    RectF metric_strip(28, cards_y, content_w, 102);
    FillRound(g, metric_strip, 8, C(255, 255, 253));
    StrokeRound(g, metric_strip, 8, C(210, 217, 208));
    Pen metric_divider(C(216, 222, 214));
    for (int i = 1; i < 4; ++i) {
        float divider_x = metric_strip.X + card_w * static_cast<float>(i);
        g.DrawLine(&metric_divider, divider_x, cards_y + 18, divider_x, cards_y + 84);
    }
    DrawStatCard(g, 28, cards_y, card_w, L"今日敲击", FormatNumber(TotalCount()) + L" 次", g_app.running ? L"正在实时记录" : L"记录已暂停", C(47, 107, 77));
    DrawStatCard(g, 28 + card_w, cards_y, card_w, L"当前速度", std::to_wstring(CurrentRate()) + L" 次/分", L"当前分钟累计", C(47, 107, 77));
    DrawStatCard(g, 28 + card_w * 2, cards_y, card_w, L"峰值速度", std::to_wstring(PeakRate()) + L" 次/分", L"最近 60 分钟", C(225, 124, 36));
    DrawStatCard(g, 28 + card_w * 3, cards_y, card_w, L"活跃时间", std::to_wstring(ActiveMinutes()) + L" 分钟", L"最近 60 分钟", C(47, 107, 77));
    float main_y = 272;
    float rank_w = 252;
    float keyboard_w = content_w - rank_w - 12;
    RectF keyboard_panel(28, main_y, keyboard_w, 398);
    FillRound(g, keyboard_panel, 8, C(255, 255, 253));
    StrokeRound(g, keyboard_panel, 8, C(216, 222, 214));
    Text(g, L"键盘热力图", RectF(44, main_y + 10, 200, 23), 13, C(30, 39, 30), FontStyleBold);
    Text(g, L"颜色越亮，敲击越频繁 · 点击按键查看详情", RectF(44, main_y + 32, 300, 17), 9, C(148, 156, 148));
    RectF keyboard(43, main_y + 56, keyboard_w - 30, 284);
    DrawKeyboard(g, keyboard, true);
    RectF selected(43, main_y + 352, keyboard_w - 30, 32);
    FillRound(g, selected, 7, C(240, 243, 237));
    Text(g, L"已选按键", RectF(selected.X + 10, selected.Y, 62, selected.Height), 8, C(135, 144, 136));
    RectF selected_key(selected.X + 75, selected.Y + 5, 48, 22);
    FillRound(g, selected_key, 4, C(35, 45, 34));
    Text(g, KeyLabel(g_app.selected_vk), selected_key, 8, C(232, 239, 228), FontStyleBold, StringAlignmentCenter);
    Text(g, FormatNumber(g_app.counts[g_app.selected_vk]) + L" 次敲击", RectF(selected.X + 134, selected.Y, 150, selected.Height), 10, C(45, 56, 45), FontStyleBold);
    DrawRankPanel(g, RectF(28 + keyboard_w + 12, main_y, rank_w, 398));
    if (height > 780) DrawChart(g, RectF(28, 684, content_w, static_cast<float>(height) - 712));
}

void DrawShareCard(Graphics& g, int width, int height) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.Clear(C(243, 244, 242));
    RectF canvas(20, 20, width - 40.0f, height - 40.0f);
    FillRound(g, canvas, 8, C(255, 255, 253));
    StrokeRound(g, canvas, 8, C(202, 209, 199));
    FillRound(g, RectF(58, 48, 46, 46), 8, C(47, 107, 77));
    Text(g, L"K", RectF(58, 48, 46, 46), 22, C(255, 255, 253), FontStyleBold, StringAlignmentCenter);
    Text(g, L"KeyPulse", RectF(118, 46, 190, 28), 22, C(25, 31, 26), FontStyleBold);
    Text(g, L"我的今日键盘热力图", RectF(118, 73, 260, 22), 12, C(94, 104, 95));
    Text(g, TodayText(), RectF(width - 400.0f, 50, 330, 36), 13, C(76, 84, 77), FontStyleRegular, StringAlignmentFar);
    Text(g, L"今日敲击", RectF(58, 122, 220, 66), 40, C(29, 34, 30), FontStyleBold);
    Text(g, FormatNumber(TotalCount()), RectF(280, 112, 330, 78), 58, C(47, 107, 77), FontStyleBold);
    Text(g, L"次", RectF(610, 122, 70, 66), 40, C(29, 34, 30), FontStyleBold);
    Pen divider(C(215, 221, 213));
    g.DrawLine(&divider, 720, 132, 720, 180);
    Text(g, L"峰值", RectF(760, 124, 80, 28), 13, C(83, 92, 84));
    Text(g, std::to_wstring(PeakRate()) + L" 次/分", RectF(760, 151, 190, 34), 22, C(225, 124, 36), FontStyleBold);
    g.DrawLine(&divider, 990, 132, 990, 180);
    Text(g, L"活跃", RectF(1030, 124, 80, 28), 13, C(83, 92, 84));
    Text(g, std::to_wstring(ActiveMinutes()) + L" 分钟", RectF(1030, 151, 220, 34), 22, C(47, 107, 77), FontStyleBold);
    Text(g, L"键盘热力图", RectF(58, 210, 240, 28), 16, C(29, 36, 30), FontStyleBold);
    Text(g, L"颜色越深，使用频率越高", RectF(300, 211, 230, 25), 10, C(125, 134, 126));
    DrawKeyboard(g, RectF(54, 248, width - 108.0f, 390), false);
    auto top = TopKeys(3);
    Text(g, L"最常用按键", RectF(58, 674, 180, 40), 16, C(29, 36, 30), FontStyleBold);
    for (size_t i = 0; i < top.size(); ++i) {
        float x = 300.0f + static_cast<float>(i) * 360.0f;
        RectF key(x, 672, i == 0 ? 100.0f : 62.0f, 48);
        FillRound(g, key, 4, C(230, 238, 226));
        StrokeRound(g, key, 4, C(171, 190, 169));
        Text(g, KeyLabel(top[i].first), key, 14, C(31, 76, 53), FontStyleBold, StringAlignmentCenter);
        Text(g, FormatNumber(top[i].second), RectF(key.GetRight() + 18, 674, 150, 42), 20, C(47, 107, 77), FontStyleBold);
    }
    g.DrawLine(&divider, PointF(58.0f, height - 98.0f), PointF(width - 58.0f, height - 98.0f));
    Text(g, L"仅统计次数，不记录输入内容", RectF(58, height - 85.0f, width - 116.0f, 34), 12, C(104, 114, 105));
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

bool ExportPng(HWND owner) {
    wchar_t file[MAX_PATH]{};
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    swprintf_s(file, L"KeyPulse-%04d-%02d-%02d.png", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = L"PNG 图片 (*.png)\0*.png\0所有文件 (*.*)\0*.*\0";
    dialog.lpstrFile = file;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrDefExt = L"png";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&dialog)) return false;
    Bitmap image(1600, 900, PixelFormat32bppARGB);
    Graphics graphics(&image);
    DrawShareCard(graphics, 1600, 900);
    CLSID encoder{};
    if (GetEncoderClsid(L"image/png", &encoder) < 0) return false;
    Status status = image.Save(file, &encoder, nullptr);
    if (status == Ok) {
        MessageBoxW(owner, L"PNG 分享图已成功导出。", L"KeyPulse", MB_OK | MB_ICONINFORMATION);
        return true;
    }
    MessageBoxW(owner, L"图片导出失败，请尝试选择其他保存位置。", L"KeyPulse", MB_OK | MB_ICONERROR);
    return false;
}

void ShowMainWindow() {
    ShowWindow(g_app.window, SW_RESTORE);
    SetForegroundWindow(g_app.window);
    InvalidateRect(g_app.window, nullptr, FALSE);
}

void UpdateTrayTip() {
    wcscpy_s(g_app.tray.szTip, g_app.running ? L"KeyPulse · 正在记录" : L"KeyPulse · 已暂停");
    Shell_NotifyIconW(NIM_MODIFY, &g_app.tray);
}

void ToggleRunning() {
    g_app.running = !g_app.running;
    g_app.dirty = true;
    UpdateTrayTip();
    InvalidateRect(g_app.window, nullptr, FALSE);
}

void ResetToday(HWND owner) {
    if (MessageBoxW(owner, L"确定清空今天的全部敲击统计吗？此操作无法撤销。", L"清空今日数据", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES) {
        ResetForNewDay();
        SaveData();
        InvalidateRect(g_app.window, nullptr, FALSE);
    }
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
    AppendMenuW(menu, MF_STRING, ID_TRAY_UPDATE, L"检查更新");
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
        if (TodayKey() != g_app.date_key) ResetForNewDay();
        if (g_app.dirty && GetTickCount64() - g_app.last_save_tick >= 15000) SaveData();
        if (IsWindowVisible(window)) InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_UPDATE_RESULT: {
        std::unique_ptr<UpdateResult> result(reinterpret_cast<UpdateResult*>(lparam));
        g_app.update_busy = 0;
        InvalidateRect(window, nullptr, FALSE);
        if (!result) return 0;
        if (result->kind == UpdateResultKind::UpToDate) {
            MessageBoxW(window, L"当前已经是最新版本。", L"KeyPulse 更新", MB_OK | MB_ICONINFORMATION);
        } else if (result->kind == UpdateResultKind::Available) {
            std::wstring prompt = L"发现新版本 " + result->version + L"。\n\n是否立即下载，完成后自动重启 KeyPulse？";
            if (MessageBoxW(window, prompt.c_str(), L"KeyPulse 更新", MB_YESNO | MB_ICONINFORMATION) == IDYES) {
                StartUpdateDownload(window, result->version, result->download_url, result->checksum_url);
            }
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
            MessageBoxW(window, message_text.c_str(), L"更新失败", MB_OK | MB_ICONERROR);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        float x = static_cast<float>(GET_X_LPARAM(lparam));
        float y = static_cast<float>(GET_Y_LPARAM(lparam));
        if (PointInside(g_app.pause_button, x, y)) ToggleRunning();
        else if (PointInside(g_app.reset_button, x, y)) ResetToday(window);
        else if (PointInside(g_app.update_button, x, y)) StartUpdateCheck(window);
        else if (PointInside(g_app.export_button, x, y)) ExportPng(window);
        else for (const auto& item : g_app.key_hitboxes) if (PointInside(item.first, x, y)) { g_app.selected_vk = item.second; InvalidateRect(window, nullptr, FALSE); break; }
        return 0;
    }
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
        case ID_TRAY_EXPORT: ShowMainWindow(); ExportPng(window); break;
        case ID_TRAY_UPDATE: ShowMainWindow(); StartUpdateCheck(window); break;
        case ID_TRAY_EXIT: g_app.exit_requested = true; SendMessageW(window, WM_CLOSE, 0, 0); break;
        default: break;
        }
        return 0;
    case WM_DESTROY:
        KillTimer(window, TIMER_UI);
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
