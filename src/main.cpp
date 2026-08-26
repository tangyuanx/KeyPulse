#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commdlg.h>
#include <shlobj.h>
#include <gdiplus.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <string>
#include <vector>

using namespace Gdiplus;
namespace fs = std::filesystem;

namespace {
constexpr wchar_t kWindowClass[] = L"KeyPulseNativeWindow";
constexpr wchar_t kAppName[] = L"KeyPulse";
constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT_PTR TIMER_UI = 1;
constexpr UINT ID_TRAY_OPEN = 1001;
constexpr UINT ID_TRAY_PAUSE = 1002;
constexpr UINT ID_TRAY_EXPORT = 1003;
constexpr UINT ID_TRAY_EXIT = 1004;
constexpr uint32_t DATA_MAGIC = 0x4B50554C; // KPUL

struct KeyDef { const wchar_t* label; UINT vk; float units; };
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
    HHOOK hook = nullptr;
    NOTIFYICONDATAW tray{};
    ULONG_PTR gdiplus_token = 0;
    std::array<uint64_t, 256> counts{};
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
    FontFamily family(family_name);
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
        {{L"Esc",VK_ESCAPE,1},{L"F1",VK_F1,1},{L"F2",VK_F2,1},{L"F3",VK_F3,1},{L"F4",VK_F4,1},{L"F5",VK_F5,1},{L"F6",VK_F6,1},{L"F7",VK_F7,1},{L"F8",VK_F8,1},{L"F9",VK_F9,1},{L"F10",VK_F10,1},{L"F11",VK_F11,1},{L"F12",VK_F12,1},{L"Del",VK_DELETE,1}},
        {{L"`",VK_OEM_3,1},{L"1",'1',1},{L"2",'2',1},{L"3",'3',1},{L"4",'4',1},{L"5",'5',1},{L"6",'6',1},{L"7",'7',1},{L"8",'8',1},{L"9",'9',1},{L"0",'0',1},{L"-",VK_OEM_MINUS,1},{L"=",VK_OEM_PLUS,1},{L"Backspace",VK_BACK,2}},
        {{L"Tab",VK_TAB,1.5f},{L"Q",'Q',1},{L"W",'W',1},{L"E",'E',1},{L"R",'R',1},{L"T",'T',1},{L"Y",'Y',1},{L"U",'U',1},{L"I",'I',1},{L"O",'O',1},{L"P",'P',1},{L"[",VK_OEM_4,1},{L"]",VK_OEM_6,1},{L"\\",VK_OEM_5,1.5f}},
        {{L"Caps",VK_CAPITAL,1.8f},{L"A",'A',1},{L"S",'S',1},{L"D",'D',1},{L"F",'F',1},{L"G",'G',1},{L"H",'H',1},{L"J",'J',1},{L"K",'K',1},{L"L",'L',1},{L";",VK_OEM_1,1},{L"'",VK_OEM_7,1},{L"Enter",VK_RETURN,2.2f}},
        {{L"Shift",VK_LSHIFT,2.3f},{L"Z",'Z',1},{L"X",'X',1},{L"C",'C',1},{L"V",'V',1},{L"B",'B',1},{L"N",'N',1},{L"M",'M',1},{L",",VK_OEM_COMMA,1},{L".",VK_OEM_PERIOD,1},{L"/",VK_OEM_2,1},{L"Shift",VK_RSHIFT,2.7f}},
        {{L"Ctrl",VK_LCONTROL,1.4f},{L"Win",VK_LWIN,1.2f},{L"Alt",VK_LMENU,1.2f},{L"Space",VK_SPACE,6.4f},{L"Alt",VK_RMENU,1.2f},{L"Ctrl",VK_RCONTROL,1.4f},{L"←",VK_LEFT,1},{L"↑",VK_UP,1},{L"↓",VK_DOWN,1},{L"→",VK_RIGHT,1}}
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

void ResetForNewDay() {
    g_app.counts.fill(0);
    g_app.minute_stamp.fill(0);
    g_app.minute_count.fill(0);
    g_app.date_key = TodayKey();
    g_app.dirty = true;
}

fs::path ExecutableDirectory() {
    std::array<wchar_t, 32768> buffer{};
    GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    return fs::path(buffer.data()).parent_path();
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
    if (value == 0 || maximum == 0) return C(47, 58, 47);
    float p = static_cast<float>(value) / static_cast<float>(maximum);
    if (p > .72f) return C(202, 240, 106);
    if (p > .45f) return C(158, 201, 93);
    if (p > .25f) return C(119, 157, 81);
    if (p > .10f) return C(88, 116, 72);
    return C(65, 81, 58);
}

void DrawStatCard(Graphics& g, float x, float y, float w, const std::wstring& label,
                  const std::wstring& value, const std::wstring& note, const Color& accent) {
    RectF card(x, y, w, 110);
    FillRound(g, card, 12, C(251, 252, 248));
    StrokeRound(g, card, 12, C(222, 229, 220));
    FillRound(g, RectF(x + w - 48, y + 14, 32, 32), 8, Color(35, accent.GetR(), accent.GetG(), accent.GetB()));
    Text(g, L"●", RectF(x + w - 48, y + 14, 32, 32), 12, accent, FontStyleBold, StringAlignmentCenter);
    Text(g, label, RectF(x + 16, y + 13, w - 70, 20), 11, C(111, 122, 112));
    Text(g, value, RectF(x + 16, y + 38, w - 32, 34), 25, C(27, 35, 27), FontStyleBold, StringAlignmentNear, L"Georgia");
    Text(g, note, RectF(x + 16, y + 78, w - 32, 18), 9, C(145, 154, 146));
}

void DrawButton(Graphics& g, const RectF& r, const std::wstring& label, bool primary = false) {
    FillRound(g, r, 8, primary ? C(28, 38, 28) : C(251, 252, 249));
    StrokeRound(g, r, 8, primary ? C(28, 38, 28) : C(218, 225, 216));
    Text(g, label, r, 11, primary ? C(245, 248, 241) : C(73, 84, 74), FontStyleRegular, StringAlignmentCenter);
}

void DrawKeyboard(Graphics& g, const RectF& bounds, bool register_hits) {
    FillRound(g, bounds, 12, C(31, 40, 31));
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
            Color fill = HeatColor(g_app.counts[key.vk], maximum);
            FillRound(g, r, 5, fill);
            StrokeRound(g, r, 5, key.vk == g_app.selected_vk ? C(218, 255, 128) : C(72, 84, 70));
            bool bright = g_app.counts[key.vk] > maximum / 4 && maximum > 0;
            Text(g, key.label, RectF(r.X + 6, r.Y + 3, r.Width - 12, r.Height * .46f), r.Width < 42 ? 8 : 9,
                 bright ? C(22, 31, 20) : C(218, 225, 215), FontStyleRegular);
            if (g_app.counts[key.vk] > 0) Text(g, FormatNumber(g_app.counts[key.vk]), RectF(r.X + 6, r.Y + r.Height * .48f, r.Width - 12, r.Height * .40f), 7,
                 bright ? C(57, 75, 47) : C(162, 174, 159));
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
    FillRound(g, panel, 12, C(251, 252, 248));
    StrokeRound(g, panel, 12, C(222, 229, 220));
    Text(g, L"高频按键", RectF(panel.X + 16, panel.Y + 12, panel.Width - 32, 24), 13, C(30, 39, 30), FontStyleBold);
    Text(g, L"今日 Top 5", RectF(panel.X + 16, panel.Y + 35, panel.Width - 32, 18), 9, C(148, 156, 148));
    auto top = TopKeys(5);
    uint64_t maximum = top.empty() ? 1 : top.front().second;
    for (size_t i = 0; i < 5; ++i) {
        float y = panel.Y + 62 + static_cast<float>(i) * 55;
        Pen line(C(235, 239, 233));
        g.DrawLine(&line, panel.X + 14, y, panel.GetRight() - 14, y);
        if (i >= top.size()) continue;
        Text(g, L"0" + std::to_wstring(i + 1), RectF(panel.X + 15, y + 8, 24, 30), 8, C(160, 168, 160));
        RectF key_rect(panel.X + 42, y + 11, 34, 28);
        FillRound(g, key_rect, 5, C(34, 44, 33));
        Text(g, KeyLabel(top[i].first), key_rect, 9, C(231, 238, 227), FontStyleBold, StringAlignmentCenter);
        Text(g, FormatNumber(top[i].second), RectF(panel.X + 87, y + 4, panel.Width - 104, 24), 12, C(35, 44, 35), FontStyleBold);
        RectF track(panel.X + 87, y + 33, panel.Width - 104, 4);
        FillRound(g, track, 2, C(228, 233, 226));
        FillRound(g, RectF(track.X, track.Y, track.Width * static_cast<float>(top[i].second) / static_cast<float>(maximum), 4), 2, C(126, 174, 84));
    }
}

void DrawChart(Graphics& g, const RectF& panel) {
    FillRound(g, panel, 12, C(251, 252, 248));
    StrokeRound(g, panel, 12, C(222, 229, 220));
    Text(g, L"最近 60 分钟", RectF(panel.X + 16, panel.Y + 10, 150, 22), 12, C(30, 39, 30), FontStyleBold);
    Text(g, L"每分钟敲击次数", RectF(panel.X + 16, panel.Y + 32, 150, 16), 8, C(148, 156, 148));
    RectF chart(panel.X + 170, panel.Y + 14, panel.Width - 188, panel.Height - 26);
    uint32_t peak = (std::max)(PeakRate(), 1u);
    int64_t now = EpochMinute();
    float gap = 2.0f;
    float bar_w = (chart.Width - gap * 59) / 60.0f;
    Pen grid(C(232, 236, 230));
    for (int i = 0; i < 3; ++i) {
        float y = chart.Y + chart.Height * static_cast<float>(i) / 2.0f;
        g.DrawLine(&grid, chart.X, y, chart.GetRight(), y);
    }
    for (int i = 0; i < 60; ++i) {
        int64_t stamp = now - 59 + i;
        size_t slot = static_cast<size_t>(stamp % 60);
        uint32_t value = g_app.minute_stamp[slot] == stamp ? g_app.minute_count[slot] : 0;
        float h = chart.Height * static_cast<float>(value) / static_cast<float>(peak);
        RectF bar(chart.X + i * (bar_w + gap), chart.GetBottom() - h, bar_w, h);
        SolidBrush brush(i == 59 ? C(50, 111, 76) : C(153, 190, 112));
        g.FillRectangle(&brush, bar);
    }
}

void DrawDashboard(Graphics& g, int width, int height) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    g.Clear(C(244, 246, 241));
    SolidBrush nav(C(251, 252, 248));
    g.FillRectangle(&nav, 0, 0, width, 68);
    Pen nav_line(C(222, 228, 220));
    g.DrawLine(&nav_line, 0, 67, width, 67);
    FillRound(g, RectF(28, 16, 36, 36), 9, C(27, 36, 27));
    Text(g, L"K", RectF(28, 16, 36, 36), 17, C(202, 240, 106), FontStyleBold, StringAlignmentCenter, L"Segoe UI");
    Text(g, L"KeyPulse", RectF(74, 14, 150, 22), 16, C(27, 35, 27), FontStyleBold);
    Text(g, L"输入节奏仪表盘", RectF(74, 35, 150, 16), 9, C(118, 128, 119));
    float right = static_cast<float>(width) - 28;
    g_app.export_button = RectF(right - 104, 17, 104, 34);
    g_app.pause_button = RectF(right - 196, 17, 82, 34);
    g_app.reset_button = RectF(right - 278, 17, 72, 34);
    DrawButton(g, g_app.reset_button, L"清空");
    DrawButton(g, g_app.pause_button, g_app.running ? L"暂停" : L"继续", true);
    DrawButton(g, g_app.export_button, L"导出 PNG");
    float content_w = static_cast<float>(width) - 56;
    Text(g, L"DAILY OVERVIEW", RectF(28, 86, 200, 16), 9, C(108, 124, 104), FontStyleBold);
    Text(g, L"今天，手指走了多远？", RectF(28, 103, 430, 40), 27, C(28, 36, 28), FontStyleRegular, StringAlignmentNear, L"Georgia");
    Text(g, L"所有数据仅在本机聚合，不记录输入内容与顺序。", RectF(28, 141, 460, 22), 11, C(117, 128, 118));
    Text(g, TodayText(), RectF(static_cast<float>(width) - 280, 110, 252, 24), 10, C(116, 126, 117), FontStyleRegular, StringAlignmentFar);
    float cards_y = 177, cards_gap = 12;
    float card_w = (content_w - cards_gap * 3) / 4;
    DrawStatCard(g, 28, cards_y, card_w, L"今日总敲击", FormatNumber(TotalCount()), g_app.running ? L"正在实时记录" : L"记录已暂停", C(76, 129, 83));
    DrawStatCard(g, 28 + (card_w + cards_gap), cards_y, card_w, L"当前速度", std::to_wstring(CurrentRate()) + L" 次/分钟", L"当前分钟累计", C(173, 126, 40));
    DrawStatCard(g, 28 + (card_w + cards_gap) * 2, cards_y, card_w, L"近一小时峰值", std::to_wstring(PeakRate()) + L" 次/分钟", L"自动滚动统计", C(75, 122, 133));
    DrawStatCard(g, 28 + (card_w + cards_gap) * 3, cards_y, card_w, L"本地隐私模式", L"已开启", L"无网络 · 15 秒批量保存", C(74, 139, 92));
    float main_y = 301;
    float rank_w = 244;
    float keyboard_w = content_w - rank_w - 12;
    RectF keyboard_panel(28, main_y, keyboard_w, 374);
    FillRound(g, keyboard_panel, 12, C(251, 252, 248));
    StrokeRound(g, keyboard_panel, 12, C(222, 229, 220));
    Text(g, L"键盘热力图", RectF(44, main_y + 10, 200, 23), 13, C(30, 39, 30), FontStyleBold);
    Text(g, L"颜色越亮，敲击越频繁 · 点击按键查看详情", RectF(44, main_y + 32, 300, 17), 9, C(148, 156, 148));
    RectF keyboard(43, main_y + 58, keyboard_w - 30, 258);
    DrawKeyboard(g, keyboard, true);
    RectF selected(43, main_y + 328, keyboard_w - 30, 32);
    FillRound(g, selected, 7, C(240, 243, 237));
    Text(g, L"已选按键", RectF(selected.X + 10, selected.Y, 62, selected.Height), 8, C(135, 144, 136));
    RectF selected_key(selected.X + 75, selected.Y + 5, 48, 22);
    FillRound(g, selected_key, 5, C(35, 45, 34));
    Text(g, KeyLabel(g_app.selected_vk), selected_key, 8, C(232, 239, 228), FontStyleBold, StringAlignmentCenter);
    Text(g, FormatNumber(g_app.counts[g_app.selected_vk]) + L" 次敲击", RectF(selected.X + 134, selected.Y, 150, selected.Height), 10, C(45, 56, 45), FontStyleBold);
    DrawRankPanel(g, RectF(28 + keyboard_w + 12, main_y, rank_w, 374));
    if (height > 760) DrawChart(g, RectF(28, 688, content_w, static_cast<float>(height) - 716));
}

void DrawShareCard(Graphics& g, int width, int height) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.Clear(C(244, 246, 241));
    FillRound(g, RectF(60, 54, 54, 54), 13, C(27, 36, 27));
    Text(g, L"K", RectF(60, 54, 54, 54), 25, C(202, 240, 106), FontStyleBold, StringAlignmentCenter, L"Segoe UI");
    Text(g, L"KeyPulse", RectF(130, 52, 300, 34), 27, C(27, 35, 27), FontStyleBold);
    Text(g, L"我的今日键盘热力图", RectF(130, 86, 300, 25), 13, C(117, 128, 118));
    Text(g, TodayText(), RectF(width - 420.0f, 58, 350, 40), 14, C(106, 118, 107), FontStyleRegular, StringAlignmentFar);
    Text(g, L"TODAY'S KEYBOARD RHYTHM", RectF(60, 150, 500, 24), 12, C(92, 114, 87), FontStyleBold);
    Text(g, FormatNumber(TotalCount()), RectF(60, 176, 400, 78), 58, C(27, 36, 27), FontStyleBold, StringAlignmentNear, L"Georgia");
    Text(g, L"次敲击", RectF(410, 205, 100, 30), 16, C(116, 127, 117));
    FillRound(g, RectF(60, 278, width - 120.0f, 500), 18, C(251, 252, 248));
    StrokeRound(g, RectF(60, 278, width - 120.0f, 500), 18, C(220, 227, 218));
    Text(g, L"键盘热力图", RectF(86, 296, 260, 30), 18, C(29, 38, 29), FontStyleBold);
    Text(g, L"颜色越亮，今天使用得越频繁", RectF(86, 329, 320, 22), 11, C(142, 151, 142));
    DrawKeyboard(g, RectF(84, 370, width - 168.0f, 350), false);
    auto top = TopKeys(3);
    std::wstring top_text = L"最常用：";
    for (size_t i = 0; i < top.size(); ++i) {
        if (i) top_text += L"  ·  ";
        top_text += KeyLabel(top[i].first) + L" " + FormatNumber(top[i].second) + L" 次";
    }
    Text(g, top_text, RectF(85, 735, width - 170.0f, 25), 12, C(79, 94, 79));
    Text(g, L"仅统计次数，不记录输入内容 · KeyPulse", RectF(60, height - 78.0f, width - 120.0f, 28), 11, C(137, 147, 138), FontStyleRegular, StringAlignmentCenter);
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

LRESULT CALLBACK KeyboardHook(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HC_ACTION && g_app.running && (wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN)) {
        const auto* key = reinterpret_cast<KBDLLHOOKSTRUCT*>(lparam);
        if ((key->flags & LLKHF_INJECTED) == 0 && key->vkCode < 256) {
            if (TodayKey() != g_app.date_key) ResetForNewDay();
            ++g_app.counts[key->vkCode];
            int64_t minute = EpochMinute();
            size_t slot = static_cast<size_t>(minute % 60);
            if (g_app.minute_stamp[slot] != minute) {
                g_app.minute_stamp[slot] = minute;
                g_app.minute_count[slot] = 0;
            }
            ++g_app.minute_count[slot];
            g_app.dirty = true;
        }
    }
    return CallNextHookEx(g_app.hook, code, wparam, lparam);
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
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"退出");
    SetForegroundWindow(window);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, point.x, point.y, 0, window, nullptr);
    DestroyMenu(menu);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE:
        AddTrayIcon(window);
        SetTimer(window, TIMER_UI, 500, nullptr);
        return 0;
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
    case WM_LBUTTONUP: {
        float x = static_cast<float>(GET_X_LPARAM(lparam));
        float y = static_cast<float>(GET_Y_LPARAM(lparam));
        if (PointInside(g_app.pause_button, x, y)) ToggleRunning();
        else if (PointInside(g_app.reset_button, x, y)) ResetToday(window);
        else if (PointInside(g_app.export_button, x, y)) ExportPng(window);
        else for (const auto& item : g_app.key_hitboxes) if (PointInside(item.first, x, y)) { g_app.selected_vk = item.second; InvalidateRect(window, nullptr, FALSE); break; }
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
        info->ptMinTrackSize.x = 1080;
        info->ptMinTrackSize.y = 760;
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
    RECT desired{0, 0, 1280, 840};
    AdjustWindowRectEx(&desired, WS_OVERLAPPEDWINDOW, FALSE, 0);
    int screen_w = GetSystemMetrics(SM_CXSCREEN), screen_h = GetSystemMetrics(SM_CYSCREEN);
    g_app.window = CreateWindowExW(0, kWindowClass, kAppName, WS_OVERLAPPEDWINDOW,
        (screen_w - (desired.right - desired.left)) / 2, (screen_h - (desired.bottom - desired.top)) / 2,
        desired.right - desired.left, desired.bottom - desired.top, nullptr, nullptr, instance, nullptr);
    if (!g_app.window) return 1;
    g_app.hook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardHook, instance, 0);
    if (!g_app.hook) {
        MessageBoxW(g_app.window, L"无法启动全局键盘统计。请关闭可能限制键盘钩子的安全软件后重试。", L"KeyPulse", MB_OK | MB_ICONERROR);
        DestroyWindow(g_app.window);
        return 1;
    }
    ShowWindow(g_app.window, show_command);
    UpdateWindow(g_app.window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (g_app.hook) UnhookWindowsHookEx(g_app.hook);
    GdiplusShutdown(g_app.gdiplus_token);
    CoUninitialize();
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return static_cast<int>(message.wParam);
}
