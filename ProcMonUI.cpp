// ProcMonUI - Windows Process Monitor / Task Killer (Win32 GUI, C++17)
// Author / Maintainer: Bob Paydar
//
// Features:
//   - Lists all running processes: PID, PPID, memory usage (RSS), name, full path.
//   - Search box with live filtering by name or path.
//   - Buttons: Refresh, Kill, Suspend, Resume, Export JSON, Export CSV.
//   - "Tree" checkbox to apply actions recursively to children.
//   - Auto-refresh the list after any button click.
//   - Real status bar at the bottom with fixed text: "Ready - Bob Paydar".
//   - No background auto-refresh/timers (never blocks user).
//
// Design/Notes:
//   - Pure Win32 API (no MFC/WTL). All UI created in code (no .rc file).
//   - Process enumeration via ToolHelp32 + PSAPI for memory usage.
//   - Suspend/Resume via NtSuspendProcess/NtResumeProcess loaded from ntdll.
//   - JSON/CSV exports encoded UTF-8 with BOM.
//   - Build in Visual Studio 2019/2022, /std:c++17, /SUBSYSTEM:WINDOWS.
//
// Linker inputs: psapi.lib; comctl32.lib; shlwapi.lib
// Manifest: enable Common Controls v6 for visual styles.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <commctrl.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <processthreadsapi.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <sal.h>

#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <sstream>
#include <fstream>
#include <iomanip>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shlwapi.lib")

// Common Controls v6 for visual styles
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#ifndef PROCESS_SUSPEND_RESUME
#define PROCESS_SUSPEND_RESUME 0x0800
#endif

// Forward declaration
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

// -------------------- Control IDs --------------------
enum : int {
    IDC_STATIC_SEARCH = 1000,
    IDC_EDIT_SEARCH = 1001,
    IDC_BTN_REFRESH = 1002,
    IDC_BTN_KILL = 1003,
    IDC_BTN_SUSPEND = 1004,
    IDC_BTN_RESUME = 1005,
    IDC_CHK_TREE = 1006,
    IDC_BTN_JSON = 1007,
    IDC_BTN_CSV = 1008,
    IDC_LIST = 1100,
    IDC_STATUS = 1200
};

// -------------------- Small helpers --------------------
static std::wstring ToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
    return s;
}
static bool IContains(const std::wstring& hay, const std::wstring& needle) {
    return ToLower(hay).find(ToLower(needle)) != std::wstring::npos;
}
static std::wstring LastErrorMessage(DWORD err = GetLastError()) {
    if (err == 0) return L"OK";
    LPWSTR buf = nullptr;
    DWORD len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPWSTR)&buf, 0, nullptr);
    std::wstring msg = (len && buf) ? std::wstring(buf, len) : L"(unknown)";
    if (buf) LocalFree(buf);
    while (!msg.empty() && (msg.back() == L'\r' || msg.back() == L'\n')) msg.pop_back();
    return msg;
}
static std::wstring HumanSize(SIZE_T b) {
    const wchar_t* u[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
    double d = (double)b; int i = 0;
    while (d >= 1024.0 && i < 4) { d /= 1024.0; ++i; }
    std::wstringstream ss; ss << std::fixed << std::setprecision(i ? 1 : 0) << d << L" " << u[i];
    return ss.str();
}
static std::wstring JsonEscape(const std::wstring& s) {
    std::wstringstream o;
    for (wchar_t c : s) {
        switch (c) {
        case L'"': o << L"\\\""; break;
        case L'\\':o << L"\\\\"; break;
        case L'\b':o << L"\\b";  break;
        case L'\f':o << L"\\f";  break;
        case L'\n':o << L"\\n";  break;
        case L'\r':o << L"\\r";  break;
        case L'\t':o << L"\\t";  break;
        default:
            if (c < 32) { o << L"\\u" << std::setw(4) << std::setfill(L'0') << std::hex << (int)c << std::dec; }
            else o << c;
        }
    }
    return o.str();
}
static std::wstring CsvEscape(const std::wstring& s) {
    std::wstring out; out.reserve(s.size() + 8);
    for (wchar_t c : s) { out += (c == L'"') ? L"""" : std::wstring(1, c); }
    return out;
}
static std::string WToUtf8(const std::wstring& ws) {
    if (ws.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
    std::string out; out.resize(len);
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), &out[0], len, nullptr, nullptr);
    return out;
}
static bool SaveUtf8File(const std::wstring& path, const std::wstring& content, bool bom = true) {
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) return false;
    if (bom) { unsigned char B[3] = { 0xEF,0xBB,0xBF }; ofs.write((char*)B, 3); }
    auto u8 = WToUtf8(content);
    ofs.write(u8.data(), (std::streamsize)u8.size());
    return true;
}

// -------------------- Nt Suspend/Resume --------------------
using NtSuspendProcess_t = LONG(WINAPI*)(HANDLE);
using NtResumeProcess_t = LONG(WINAPI*)(HANDLE);
static NtSuspendProcess_t pNtSuspendProcess = nullptr;
static NtResumeProcess_t  pNtResumeProcess = nullptr;
static void LoadNtFunctions() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return;
    pNtSuspendProcess = (NtSuspendProcess_t)GetProcAddress(ntdll, "NtSuspendProcess");
    pNtResumeProcess = (NtResumeProcess_t)GetProcAddress(ntdll, "NtResumeProcess");
}

// -------------------- Process model --------------------
struct Proc {
    DWORD pid{}, ppid{};
    std::wstring name, path;
    SIZE_T rss{}; // working set bytes
};

// Snapshot of processes
static std::vector<Proc> Snapshot() {
    std::vector<Proc> v;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return v;

    PROCESSENTRY32W pe; pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            Proc p; p.pid = pe.th32ProcessID; p.ppid = pe.th32ParentProcessID; p.name = pe.szExeFile;
            v.push_back(std::move(p));
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    for (auto& p : v) {
        if (p.pid == 0) continue; // Idle
        HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, p.pid);
        if (h) {
            wchar_t buf[MAX_PATH * 4] = { 0 }; DWORD sz = (DWORD)(MAX_PATH * 4);
            if (QueryFullProcessImageNameW(h, 0, buf, &sz)) p.path.assign(buf, sz);
            PROCESS_MEMORY_COUNTERS_EX pmc{};
            if (GetProcessMemoryInfo(h, (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
                p.rss = pmc.WorkingSetSize;
            }
            CloseHandle(h);
        }
    }
    return v;
}

// Build parent->children map
static std::unordered_map<DWORD, std::vector<DWORD>> BuildChildren(const std::vector<Proc>& v) {
    std::unordered_map<DWORD, std::vector<DWORD>> m;
    for (auto& p : v) m[p.ppid].push_back(p.pid);
    return m;
}
static void CollectTree(DWORD pid, const std::unordered_map<DWORD, std::vector<DWORD>>& ch, std::vector<DWORD>& out) {
    out.push_back(pid);
    auto it = ch.find(pid); if (it == ch.end()) return;
    for (DWORD c : it->second) CollectTree(c, ch, out);
}

static bool TerminatePid(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!h) return false;
    BOOL ok = TerminateProcess(h, 1);
    CloseHandle(h);
    return ok != 0;
}
static bool SuspendPid(DWORD pid) {
    if (!pNtSuspendProcess) return false;
    HANDLE h = OpenProcess(PROCESS_SUSPEND_RESUME | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h) return false;
    LONG st = pNtSuspendProcess(h);
    CloseHandle(h);
    return st == 0;
}
static bool ResumePid(DWORD pid) {
    if (!pNtResumeProcess) return false;
    HANDLE h = OpenProcess(PROCESS_SUSPEND_RESUME | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h) return false;
    LONG st = pNtResumeProcess(h);
    CloseHandle(h);
    return st == 0;
}

// -------------------- App state --------------------
struct AppState {
    HWND hwnd{}, hwndList{}, hLblSearch{}, hSearch{}, hChkTree{};
    HWND hBtnRefresh{}, hBtnKill{}, hBtnSuspend{}, hBtnResume{}, hBtnJson{}, hBtnCsv{}, hStatus{};
    std::vector<Proc> all, filtered;
    std::wstring filter;
} g;

// -------------------- Output builders --------------------
static std::wstring BuildJson(const std::vector<Proc>& v) {
    std::wstringstream w;
    w << L"{\"processes\":[";
    for (size_t i = 0; i < v.size(); ++i) {
        const auto& p = v[i];
        w << L"{\"pid\":" << p.pid << L",\"ppid\":" << p.ppid
            << L",\"name\":\"" << JsonEscape(p.name) << L"\","
            << L"\"path\":\"" << JsonEscape(p.path) << L"\","
            << L"\"rss_bytes\":" << p.rss << L"}"
            << (i + 1 < v.size() ? L"," : L"");
    }
    w << L"]}\n";
    return w.str();
}
static std::wstring BuildCsv(const std::vector<Proc>& v) {
    std::wstringstream w; w << L"PID,PPID,RSS_BYTES,Name,Path\n";
    for (auto& p : v) {
        auto q = [&](const std::wstring& s) { return L"\"" + CsvEscape(s) + L"\""; };
        w << p.pid << L"," << p.ppid << L"," << p.rss << L"," << q(p.name) << L"," << q(p.path) << L"\n";
    }
    return w.str();
}

// Double-NUL filter must be an LPCWSTR literal (wstring would cut at first '\0')
static bool SaveWithDialog(const std::wstring& title, const std::wstring& defExt, LPCWSTR filter, const std::wstring& content) {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn{ 0 };
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g.hwnd;
    ofn.lpstrFilter = filter; // e.g. L"JSON (*.json)\0*.json\0All Files (*.*)\0*.*\0"
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = defExt.c_str();
    ofn.lpstrTitle = title.c_str();
    ofn.Flags = OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameW(&ofn)) return false;
    return SaveUtf8File(file, content, true);
}

// -------------------- ListView helpers --------------------
static void ListView_SetupColumns(HWND lv) {
    LVCOLUMNW col{ 0 }; col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    // 5 columns: PID, PPID, RSS, Name, Path
    int w[] = { 80,80,110,220,700 };
    const wchar_t* t[] = { L"PID",L"PPID",L"RSS",L"Name",L"Path" };
    for (int i = 0; i < 5; ++i) {
        col.pszText = (LPWSTR)t[i]; col.cx = w[i]; col.iSubItem = i;
        ListView_InsertColumn(lv, i, &col);
    }
}
static void ListView_Clear(HWND lv) { ListView_DeleteAllItems(lv); }
static void ListView_AddOrSet(HWND lv, int row, const Proc& p) {
    wchar_t buf[64] = { 0 };
    LVITEMW it{ 0 }; it.mask = LVIF_TEXT; it.iItem = row;

    // Column 0: PID
    _itow_s((int)p.pid, buf, 10); it.pszText = buf;
    if (ListView_GetItemCount(lv) <= row) ListView_InsertItem(lv, &it);
    else ListView_SetItem(lv, &it);

    // Subitems
    _itow_s((int)p.ppid, buf, 10); ListView_SetItemText(lv, row, 1, buf);
    std::wstring rss = HumanSize(p.rss); ListView_SetItemText(lv, row, 2, (LPWSTR)rss.c_str());
    ListView_SetItemText(lv, row, 3, (LPWSTR)p.name.c_str());
    ListView_SetItemText(lv, row, 4, (LPWSTR)p.path.c_str());
}

// -------------------- Filtering & Refresh --------------------
static void ApplyFilter() {
    g.filtered.clear();
    if (g.filter.empty()) { g.filtered = g.all; return; }
    for (auto& p : g.all) {
        if (IContains(p.name, g.filter) || IContains(p.path, g.filter)) g.filtered.push_back(p);
    }
}
static void RefreshData() {
    g.all = Snapshot();
    std::sort(g.all.begin(), g.all.end(), [](const Proc& a, const Proc& b) {
        if (a.rss != b.rss) return a.rss > b.rss;
        return a.name < b.name;
        });
    ApplyFilter();
    ListView_Clear(g.hwndList);
    ListView_SetItemCount(g.hwndList, (int)g.filtered.size());
    for (int i = 0; i < (int)g.filtered.size(); ++i) ListView_AddOrSet(g.hwndList, i, g.filtered[i]);
}
static std::vector<DWORD> GetSelectedPids() {
    std::vector<DWORD> pids;
    int idx = -1;
    while ((idx = ListView_GetNextItem(g.hwndList, idx, LVNI_SELECTED)) != -1) {
        wchar_t buf[32] = { 0 };
        ListView_GetItemText(g.hwndList, idx, 0, buf, 31);
        buf[31] = L'\0';
        pids.push_back((DWORD)_wtoi(buf));
    }
    return pids;
}
static std::unordered_map<DWORD, std::vector<DWORD>> CurrentChildren() {
    return BuildChildren(g.all);
}
static void ActOnSelection(int action/*1=kill 2=suspend 3=resume*/) {
    auto pids = GetSelectedPids();
    if (pids.empty()) {
        MessageBoxW(g.hwnd, L"Select one or more rows first.", L"ProcMonUI", MB_ICONINFORMATION);
        return;
    }
    bool tree = (SendMessageW(g.hChkTree, BM_GETCHECK, 0, 0) == BST_CHECKED);
    auto ch = CurrentChildren();
    std::vector<DWORD> victims;
    for (DWORD pid : pids) {
        if (tree) CollectTree(pid, ch, victims);
        else victims.push_back(pid);
    }
    std::sort(victims.begin(), victims.end());
    victims.erase(std::unique(victims.begin(), victims.end()), victims.end());

    int ok = 0, fail = 0;
    for (auto it = victims.rbegin(); it != victims.rend(); ++it) {
        DWORD pid = *it; bool res = false;
        if (action == 1) res = TerminatePid(pid);
        else if (action == 2) res = SuspendPid(pid);
        else if (action == 3) res = ResumePid(pid);
        if (res) ++ok; else ++fail;
    }
    std::wstringstream ss; ss << L"OK=" << ok << L" FAIL=" << fail;
    MessageBoxW(g.hwnd, ss.str().c_str(), L"Action result", fail ? MB_ICONWARNING : MB_ICONINFORMATION);
}

// -------------------- Window Proc --------------------
LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        g.hwnd = h;
        INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_BAR_CLASSES };
        InitCommonControlsEx(&icc);

        // "Search:" label
        g.hLblSearch = CreateWindowW(L"STATIC", L"Search:", WS_CHILD | WS_VISIBLE,
            10, 12, 60, 18, h, (HMENU)IDC_STATIC_SEARCH, GetModuleHandleW(nullptr), nullptr);

        // Search edit
        g.hSearch = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            75, 10, 300, 24, h, (HMENU)IDC_EDIT_SEARCH, GetModuleHandleW(nullptr), nullptr);

        // Buttons row
        g.hBtnRefresh = CreateWindowW(L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            390, 10, 90, 24, h, (HMENU)IDC_BTN_REFRESH, nullptr, nullptr);
        g.hBtnKill = CreateWindowW(L"BUTTON", L"Kill", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            490, 10, 70, 24, h, (HMENU)IDC_BTN_KILL, nullptr, nullptr);
        g.hBtnSuspend = CreateWindowW(L"BUTTON", L"Suspend", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            570, 10, 80, 24, h, (HMENU)IDC_BTN_SUSPEND, nullptr, nullptr);
        g.hBtnResume = CreateWindowW(L"BUTTON", L"Resume", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            660, 10, 80, 24, h, (HMENU)IDC_BTN_RESUME, nullptr, nullptr);
        g.hChkTree = CreateWindowW(L"BUTTON", L"Tree", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            750, 12, 70, 20, h, (HMENU)IDC_CHK_TREE, nullptr, nullptr);
        g.hBtnJson = CreateWindowW(L"BUTTON", L"Export JSON", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            830, 10, 110, 24, h, (HMENU)IDC_BTN_JSON, nullptr, nullptr);
        g.hBtnCsv = CreateWindowW(L"BUTTON", L"Export CSV", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            950, 10, 110, 24, h, (HMENU)IDC_BTN_CSV, nullptr, nullptr);

        // ListView
        g.hwndList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL,
            10, 44, 1060, 520, h, (HMENU)IDC_LIST, GetModuleHandleW(nullptr), nullptr);
        ListView_SetExtendedListViewStyle(g.hwndList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
        ListView_SetupColumns(g.hwndList);

        // Status bar at bottom with fixed text
        g.hStatus = CreateWindowExW(
            0, STATUSCLASSNAMEW, L"",
            WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP | CCS_BOTTOM,
            0, 0, 0, 0,
            h, (HMENU)IDC_STATUS, GetModuleHandleW(nullptr), nullptr
        );
        int parts[1] = { -1 };
        SendMessageW(g.hStatus, SB_SETPARTS, 1, (LPARAM)parts);
        SendMessageW(g.hStatus, WM_SIZE, 0, 0);
        SendMessageW(g.hStatus, SB_SETTEXT, 0, (LPARAM)L"Ready - Bob Paydar");

        LoadNtFunctions();

        // One-time initial load
        RefreshData();
        return 0;
    }

    case WM_SIZE: {
        int wClient = LOWORD(l), hClient = HIWORD(l);

        // Let status bar size itself; then measure its height
        if (g.hStatus) SendMessageW(g.hStatus, WM_SIZE, 0, 0);
        RECT rcStatus{}; int statusH = 0;
        if (g.hStatus && GetWindowRect(g.hStatus, &rcStatus))
            statusH = rcStatus.bottom - rcStatus.top;

        // Top row
        MoveWindow(g.hLblSearch, 10, 12, 60, 18, TRUE);
        MoveWindow(g.hSearch, 75, 10, 300, 24, TRUE);
        MoveWindow(g.hBtnRefresh, 390, 10, 90, 24, TRUE);
        MoveWindow(g.hBtnKill, 490, 10, 70, 24, TRUE);
        MoveWindow(g.hBtnSuspend, 570, 10, 80, 24, TRUE);
        MoveWindow(g.hBtnResume, 660, 10, 80, 24, TRUE);
        MoveWindow(g.hChkTree, 750, 12, 70, 20, TRUE);
        MoveWindow(g.hBtnJson, 830, 10, 110, 24, TRUE);
        MoveWindow(g.hBtnCsv, 950, 10, 110, 24, TRUE);

        // List view fills space above status bar
        MoveWindow(g.hwndList, 10, 44, wClient - 20, hClient - 44 - statusH, TRUE);
        return 0;
    }

    case WM_COMMAND: {
        const WORD ctrlId = LOWORD(w);
        const WORD code = HIWORD(w);

        if (ctrlId == IDC_EDIT_SEARCH && code == EN_CHANGE) {
            wchar_t buf[512] = { 0 };
            GetWindowTextW(g.hSearch, buf, 511);
            buf[511] = L'\0';
            g.filter = buf;
            ApplyFilter();
            ListView_Clear(g.hwndList);
            ListView_SetItemCount(g.hwndList, (int)g.filtered.size());
            for (int i = 0; i < (int)g.filtered.size(); ++i) ListView_AddOrSet(g.hwndList, i, g.filtered[i]);
            return 0;
        }

        switch (ctrlId) {
        case IDC_BTN_REFRESH:
            RefreshData();
            break;

        case IDC_BTN_KILL:
            ActOnSelection(1);
            RefreshData(); // auto-refresh after click
            break;

        case IDC_BTN_SUSPEND:
            ActOnSelection(2);
            RefreshData(); // auto-refresh after click
            break;

        case IDC_BTN_RESUME:
            ActOnSelection(3);
            RefreshData(); // auto-refresh after click
            break;

        case IDC_BTN_JSON: {
            if (g.filtered.empty()) {
                MessageBoxW(h, L"No rows to export.", L"Export", MB_ICONINFORMATION);
            }
            else {
                auto json = BuildJson(g.filtered);
                SaveWithDialog(L"Export JSON", L"json",
                    L"JSON (*.json)\0*.json\0All Files (*.*)\0*.*\0", json);
            }
            RefreshData(); // auto-refresh after click
            break;
        }
        case IDC_BTN_CSV: {
            if (g.filtered.empty()) {
                MessageBoxW(h, L"No rows to export.", L"Export", MB_ICONINFORMATION);
            }
            else {
                auto csv = BuildCsv(g.filtered);
                SaveWithDialog(L"Export CSV", L"csv",
                    L"CSV (*.csv)\0*.csv\0All Files (*.*)\0*.*\0", csv);
            }
            RefreshData(); // auto-refresh after click
            break;
        }
        }
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// -------------------- Entry Point --------------------
int APIENTRY wWinMain(_In_ HINSTANCE hInst,
    _In_opt_ HINSTANCE /*hPrev*/,
    _In_ LPWSTR /*lpCmdLine*/,
    _In_ int nShow) {
    WNDCLASSW wc{ 0 };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"ProcMonUIWnd";
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowW(wc.lpszClassName, L"ProcMon — Windows Process Monitor / Task Killer",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 1080, 680,
        nullptr, nullptr, hInst, nullptr);

    if (!hwnd) return 2;

    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
