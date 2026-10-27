// ZelRaccy Commission Manager — plain Win32 UI.
#include "core.h"
#include "resource.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <uxtheme.h>

#include <algorithm>
#include <thread>

namespace {

constexpr UINT WM_APP_INIT = WM_APP + 1;
constexpr UINT WM_APP_LOG = WM_APP + 2;      // lParam: std::wstring*
constexpr UINT WM_APP_GITDONE = WM_APP + 3; // lParam: GitTaskResult*
constexpr UINT_PTR kThumbTimer = 1;

const wchar_t kAppName[] = L"ZelRaccy Commission Manager";
const wchar_t kClassName[] = L"ZelRaccyCommissionManager";
const wchar_t kDefaultRepo[] = L"C:\\Users\\Hakai\\source\\repos\\zelraccy-commissions-purple";

const COLORREF kAccent = RGB(98, 54, 170);
const COLORREF kOpen = RGB(30, 132, 73);
const COLORREF kClosed = RGB(192, 41, 43);
const COLORREF kMuted = RGB(110, 110, 120);

enum : int {
    IDC_FOLDER_LABEL = 200,
    IDC_FOLDER,
    IDC_BROWSE,
    IDC_HEAD_STATUS,
    IDC_TOGGLE,
    IDC_TOGGLE_DESC,
    IDC_HEAD_GALLERY,
    IDC_LIST,
    IDC_ADD,
    IDC_RENAME,
    IDC_REMOVE,
    IDC_UP,
    IDC_DOWN,
    IDC_HEAD_PUBLISH,
    IDC_REMOTE_LABEL,
    IDC_REMOTE,
    IDC_CONNECT,
    IDC_PUSH,
    IDC_PULL,
    IDC_PUSH_RESULT,
    IDC_LOG,
    IDC_STATUSBAR,
};

struct AppState {
    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    UINT dpi = 96;
    HFONT font = nullptr, fontHead = nullptr, fontBig = nullptr, fontMono = nullptr;

    HWND folderLabel{}, folder{}, browse{}, headStatus{}, toggle{}, toggleDesc{}, headGallery{}, list{}, add{},
        rename{}, remove{}, up{}, down{}, headPublish{}, remoteLabel{}, remote{}, connect{}, push{}, pull{},
        pushResult{}, log{}, status{};

    std::wstring repo;
    SiteData data;
    bool loaded = false;
    bool busy = false; // a git task is running
    std::vector<std::wstring> changes; // since last successful push → commit summary
    COLORREF pushResultColor = RGB(110, 110, 120);

    RECT previewRect{};
    HBITMAP thumb = nullptr;
    UINT thumbW = 0, thumbH = 0;
} g;

int S(int v) { return MulDiv(v, (int)g.dpi, 96); }

// ---------------------------------------------------------------------------
// Settings (%APPDATA%\ZelRaccyCommissionManager\settings.ini, UTF-16)
// ---------------------------------------------------------------------------
std::wstring SettingsPath() {
    PWSTR appData = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) dir = appData;
    CoTaskMemFree(appData);
    if (dir.empty()) return {};
    dir += L"\\ZelRaccyCommissionManager";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring path = dir + L"\\settings.ini";
    if (!FileExists(path)) {
        // A UTF-16 BOM makes the profile APIs store Unicode paths correctly.
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            const BYTE bom[] = {0xFF, 0xFE};
            DWORD n;
            WriteFile(h, bom, sizeof(bom), &n, nullptr);
            CloseHandle(h);
        }
    }
    return path;
}

std::wstring LoadSetting(const wchar_t* key, const wchar_t* fallback) {
    std::wstring path = SettingsPath();
    if (path.empty()) return fallback;
    wchar_t buf[2048];
    GetPrivateProfileStringW(L"Settings", key, fallback, buf, ARRAYSIZE(buf), path.c_str());
    return Trim(buf).empty() ? std::wstring(fallback) : Trim(buf);
}

void SaveSetting(const wchar_t* key, const std::wstring& value) {
    std::wstring path = SettingsPath();
    if (!path.empty()) WritePrivateProfileStringW(L"Settings", key, value.c_str(), path.c_str());
}

// ---------------------------------------------------------------------------
// Small UI helpers
// ---------------------------------------------------------------------------
std::wstring NowText() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[32];
    swprintf_s(buf, L"%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);
    return buf;
}

void SetStatus(const std::wstring& text) { SendMessageW(g.status, SB_SETTEXTW, 0, (LPARAM)text.c_str()); }

void SetPushResult(const std::wstring& text, COLORREF color) {
    g.pushResultColor = color;
    SetWindowTextW(g.pushResult, text.c_str());
    InvalidateRect(g.pushResult, nullptr, TRUE);
}

void AppendLog(const std::wstring& line) {
    std::wstring text;
    for (wchar_t c : line) {
        if (c == L'\r') continue;
        if (c == L'\n') text += L"\r\n";
        else text += c;
    }
    text += L"\r\n";
    int len = GetWindowTextLengthW(g.log);
    SendMessageW(g.log, EM_SETSEL, len, len);
    SendMessageW(g.log, EM_REPLACESEL, FALSE, (LPARAM)text.c_str());
    SendMessageW(g.log, EM_SCROLLCARET, 0, 0);
}

int Message(const std::wstring& text, UINT flags) { return MessageBoxW(g.hwnd, text.c_str(), kAppName, flags); }

std::wstring GetText(HWND h) {
    std::wstring s(GetWindowTextLengthW(h) + 1, L'\0');
    GetWindowTextW(h, s.data(), (int)s.size());
    s.resize(wcslen(s.c_str()));
    return s;
}

int SelectedIndex() { return ListView_GetNextItem(g.list, -1, LVNI_SELECTED); }

void Select(int i) {
    ListView_SetItemState(g.list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    if (i >= 0) {
        ListView_SetItemState(g.list, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(g.list, i, FALSE);
    }
}

void NoteChange(const std::wstring& change) {
    if (change.rfind(L"commissions ", 0) == 0)
        g.changes.erase(std::remove_if(g.changes.begin(), g.changes.end(),
                                       [](const std::wstring& c) { return c.rfind(L"commissions ", 0) == 0; }),
                        g.changes.end());
    if (std::find(g.changes.begin(), g.changes.end(), change) == g.changes.end()) g.changes.push_back(change);
}

std::wstring CommitSummary() {
    std::wstring s;
    for (auto& c : g.changes) s += (s.empty() ? L"" : L", ") + c;
    if (s.size() > 60) s = std::to_wstring(g.changes.size()) + L" changes (" + s.substr(0, 45) + L"…)";
    return s;
}

// ---------------------------------------------------------------------------
// Fonts & layout
// ---------------------------------------------------------------------------
HFONT MakeFont(int pt, int weight, const wchar_t* face) {
    return CreateFontW(-MulDiv(pt, (int)g.dpi, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
}

void CreateFonts() {
    for (HFONT f : {g.font, g.fontHead, g.fontBig, g.fontMono})
        if (f) DeleteObject(f);
    g.font = MakeFont(10, FW_NORMAL, L"Segoe UI");
    g.fontHead = MakeFont(12, FW_SEMIBOLD, L"Segoe UI");
    g.fontBig = MakeFont(15, FW_BOLD, L"Segoe UI");
    g.fontMono = MakeFont(9, FW_NORMAL, L"Consolas");
}

void ApplyFonts() {
    EnumChildWindows(
        g.hwnd, [](HWND child, LPARAM) -> BOOL {
            SendMessageW(child, WM_SETFONT, (WPARAM)g.font, FALSE);
            return TRUE;
        },
        0);
    for (HWND h : {g.headStatus, g.headGallery, g.headPublish}) SendMessageW(h, WM_SETFONT, (WPARAM)g.fontHead, FALSE);
    SendMessageW(g.log, WM_SETFONT, (WPARAM)g.fontMono, FALSE);
    // The status bar doesn't grow with its font on its own.
    TEXTMETRICW tm{};
    HDC dc = GetDC(g.hwnd);
    HGDIOBJ old = SelectObject(dc, g.font);
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(g.hwnd, dc);
    SendMessageW(g.status, SB_SETMINHEIGHT, tm.tmHeight + S(6), 0);
    InvalidateRect(g.hwnd, nullptr, TRUE);
}

void Place(HDWP& dwp, HWND h, int x, int y, int w, int hgt) {
    dwp = DeferWindowPos(dwp, h, nullptr, x, y, std::max(w, 0), std::max(hgt, 0), SWP_NOZORDER | SWP_NOACTIVATE);
}

void Layout() {
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    SendMessageW(g.status, WM_SIZE, 0, 0);
    RECT sr;
    GetWindowRect(g.status, &sr);
    const int W = rc.right, H = rc.bottom - (sr.bottom - sr.top);
    const int m = S(16), btnH = S(30), headH = S(26), headGap = S(30);

    const int labelW = S(140), fieldX = m + labelW + S(4);
    HDWP dwp = BeginDeferWindowPos(24);
    int y = m;
    Place(dwp, g.folderLabel, m, y + S(5), labelW, S(22));
    Place(dwp, g.folder, fieldX, y + S(1), W - m - fieldX - S(104), S(28));
    Place(dwp, g.browse, W - m - S(96), y, S(96), btnH);
    y += btnH + S(16);

    Place(dwp, g.headStatus, m, y, W - 2 * m, headH);
    y += headGap;
    Place(dwp, g.toggle, m, y, S(330), S(50));
    Place(dwp, g.toggleDesc, m + S(346), y + S(3), W - 2 * m - S(346), S(46));
    y += S(50) + S(14);

    Place(dwp, g.headGallery, m, y, W - 2 * m, headH);
    y += headGap;

    // Remaining height is shared by the gallery and the publish log. The gallery keeps room for its
    // preview first; the log shrinks before it does, and extra height goes to the gallery.
    const int pushH = S(34), remoteRowH = btnH + S(8), sectionGap = S(14);
    const int avail = H - m - y - sectionGap - headGap - remoteRowH - pushH - S(10);
    int logH = S(120), galH = avail - logH;
    if (galH < S(300)) {
        logH = std::max(S(64), avail - S(300));
        galH = std::max(S(170), avail - logH);
    }
    const int colW = S(200), gap = S(12);
    const int listW = W - 2 * m - colW - gap;
    Place(dwp, g.list, m, y, listW, galH);
    const int cx = m + listW + gap;
    int by = y;
    for (HWND b : {g.add, g.rename, g.remove}) {
        Place(dwp, b, cx, by, colW, btnH);
        by += btnH + S(6);
    }
    const int half = (colW - S(6)) / 2;
    Place(dwp, g.up, cx, by, half, btnH);
    Place(dwp, g.down, cx + half + S(6), by, colW - half - S(6), btnH);
    by += btnH + S(12);
    RECT oldPreview = g.previewRect;
    g.previewRect = {cx, by, cx + colW, y + galH};
    y += galH + sectionGap;

    Place(dwp, g.headPublish, m, y, W - 2 * m, headH);
    y += headGap;
    Place(dwp, g.remoteLabel, m, y + S(5), labelW, S(22));
    Place(dwp, g.remote, fieldX, y + S(1), W - m - fieldX - S(190), S(28));
    Place(dwp, g.connect, W - m - S(182), y, S(182), btnH);
    y += remoteRowH;
    Place(dwp, g.push, m, y, S(180), pushH);
    Place(dwp, g.pull, m + S(190), y, S(110), pushH);
    Place(dwp, g.pushResult, m + S(314), y + S(6), W - 2 * m - S(314), S(24));
    y += pushH + S(10);
    Place(dwp, g.log, m, y, W - 2 * m, logH);
    EndDeferWindowPos(dwp);

    const int numW = S(56), nameW = std::max(S(120), (listW - numW - S(24)) * 2 / 5);
    ListView_SetColumnWidth(g.list, 0, numW);
    ListView_SetColumnWidth(g.list, 1, nameW);
    ListView_SetColumnWidth(g.list, 2, std::max(S(120), listW - numW - nameW - S(24)));

    if (!EqualRect(&oldPreview, &g.previewRect)) {
        InvalidateRect(g.hwnd, &oldPreview, TRUE);
        InvalidateRect(g.hwnd, &g.previewRect, TRUE);
        SetTimer(g.hwnd, kThumbTimer, 150, nullptr); // re-render the preview once resizing settles
    }
}

// ---------------------------------------------------------------------------
// State → UI
// ---------------------------------------------------------------------------
void UpdatePreview() {
    if (g.thumb) DeleteObject(g.thumb);
    g.thumb = nullptr;
    int sel = SelectedIndex();
    const int pad = S(6);
    int pw = g.previewRect.right - g.previewRect.left - 2 * pad, ph = g.previewRect.bottom - g.previewRect.top - 2 * pad;
    if (g.loaded && sel >= 0 && pw > S(40) && ph > S(40)) {
        std::wstring path = RepoPath(g.repo, g.data.Item((size_t)sel).file);
        g.thumb = LoadThumbnail(path, (UINT)pw, (UINT)ph, g.thumbW, g.thumbH);
    }
    InvalidateRect(g.hwnd, &g.previewRect, TRUE);
}

void UpdateButtons() {
    const bool editable = g.loaded && !g.busy;
    const int sel = SelectedIndex(), count = g.loaded ? (int)g.data.Count() : 0;
    EnableWindow(g.browse, !g.busy);
    EnableWindow(g.toggle, editable);
    EnableWindow(g.list, g.loaded);
    EnableWindow(g.add, editable);
    EnableWindow(g.rename, editable && sel >= 0);
    EnableWindow(g.remove, editable && sel >= 0);
    EnableWindow(g.up, editable && sel > 0);
    EnableWindow(g.down, editable && sel >= 0 && sel + 1 < count);
    EnableWindow(g.remote, !g.busy);
    EnableWindow(g.connect, editable);
    EnableWindow(g.push, editable);
    EnableWindow(g.pull, editable);
    HWND focus = GetFocus();
    if (!focus || (focus != g.hwnd && !IsWindowEnabled(focus))) SetFocus(g.loaded ? g.list : g.browse);
}

void UpdateToggle() {
    std::wstring desc, label;
    if (!g.loaded) {
        label = L"Status unavailable";
        desc = L"Choose your website folder above to manage it.";
    } else if (g.data.CommissionsOpen()) {
        label = L"Commissions: OPEN";
        desc = L"Your website currently says commissions are OPEN.\r\nClick the button to switch to CLOSED.";
    } else {
        label = L"Commissions: CLOSED";
        desc = L"Your website currently says commissions are CLOSED.\r\nClick the button to switch to OPEN.";
    }
    SetWindowTextW(g.toggle, label.c_str());
    SetWindowTextW(g.toggleDesc, desc.c_str());
    InvalidateRect(g.toggle, nullptr, TRUE);
}

void RefreshList(int select) {
    SendMessageW(g.list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g.list);
    if (g.loaded) {
        for (size_t i = 0; i < g.data.Count(); ++i) {
            GalleryItem it = g.data.Item(i);
            std::wstring num = std::to_wstring(i + 1) + (i == 0 ? L" \u2605" : L"");
            LVITEMW lvi = {};
            lvi.mask = LVIF_TEXT;
            lvi.iItem = (int)i;
            lvi.pszText = num.data();
            ListView_InsertItem(g.list, &lvi);
            ListView_SetItemText(g.list, (int)i, 1, it.name.data());
            std::wstring file = it.file;
            if (!file.empty() && !FileExists(RepoPath(g.repo, file))) file += L"   (file missing!)";
            ListView_SetItemText(g.list, (int)i, 2, file.data());
        }
    }
    SendMessageW(g.list, WM_SETREDRAW, TRUE, 0);
    Select(g.loaded ? std::min(select, (int)g.data.Count() - 1) : -1);
    InvalidateRect(g.list, nullptr, TRUE);
    UpdateButtons();
    UpdatePreview();
}

void RefreshAll(int select) {
    SetWindowTextW(g.folder, g.repo.c_str());
    UpdateToggle();
    RefreshList(select);
}

// Shows whether the folder is connected to GitHub (runs a few quick git queries).
void UpdateGitState() {
    if (!g.loaded || g.busy) return;
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    RepoInfo info = InspectRepo(g.repo);
    SetCursor(old);
    if (!info.gitAvailable)
        SetPushResult(L"Git isn't installed \u2014 install GitHub Desktop or Git for Windows to publish.", kClosed);
    else if (!info.isRepo || info.originUrl.empty())
        SetPushResult(L"Not connected to GitHub yet \u2014 click \u201cConnect to GitHub\u2026\u201d first.", kClosed);
    else if (!SameRemoteUrl(info.originUrl, GetText(g.remote)))
        SetPushResult(L"Connected to a different address: " + info.originUrl, kMuted);
    else
        SetPushResult(L"Connected to GitHub (branch " + (info.branch.empty() ? std::wstring(L"?") : info.branch) +
                          L"). Push uploads your saved changes.",
                      kMuted);
}

// Loads site data from `path`. Shows a message box on failure when `interactive`.
bool LoadRepo(const std::wstring& path, bool interactive) {
    if (path != g.repo) g.changes.clear();
    g.repo = path;
    SiteData d;
    std::wstring err;
    g.loaded = d.Load(path, err);
    if (g.loaded) {
        g.data = std::move(d);
        g.repo = g.data.Repo();
        SetStatus(L"Loaded " + std::to_wstring(g.data.Count()) +
                  L" gallery images. Every change you make is saved automatically to data\\site-data.js.");
    } else {
        g.data = SiteData();
        SetStatus(L"Problem: " + err.substr(0, err.find(L'\n')) + L" Use Browse\u2026 to pick your website folder.");
        SetPushResult(L"", kMuted);
    }
    RefreshAll(0);
    UpdateGitState();
    if (!g.loaded && interactive) Message(err, MB_ICONERROR);
    return g.loaded;
}

// Saves after an in-memory change. On failure, explains and reloads what's on disk.
bool Commit(const std::wstring& change, int select) {
    std::wstring err;
    if (!g.data.Save(err)) {
        Message(L"Your change could not be saved:\n\n" + err, MB_ICONERROR);
        LoadRepo(g.repo, true);
        SetStatus(L"Not saved: " + change + L" — see the error message.");
        return false;
    }
    NoteChange(change);
    UpdateToggle();
    RefreshList(select);
    SetStatus(L"\u2714 Saved automatically at " + NowText() + L" (" + change +
              L"). Click \u201cPush changes\u201d to publish to the website.");
    return true;
}

// ---------------------------------------------------------------------------
// File pickers
// ---------------------------------------------------------------------------
bool PickImageFile(HWND owner, std::wstring& out) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
    COMDLG_FILTERSPEC filters[] = {{L"Images (*.png; *.jpg; *.jpeg; *.webp; *.bmp)", L"*.png;*.jpg;*.jpeg;*.webp;*.bmp"}};
    dlg->SetFileTypes(ARRAYSIZE(filters), filters);
    dlg->SetTitle(L"Choose an image to add to the gallery");
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
    bool ok = false;
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                out = p;
                ok = true;
                CoTaskMemFree(p);
            }
            item->Release();
        }
    }
    dlg->Release();
    return ok;
}

bool PickFolder(HWND owner, const std::wstring& start, std::wstring& out) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(L"Choose your website folder (the one containing index.html and data)");
    IShellItem* startItem = nullptr;
    if (DirExists(start) && SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&startItem)))) {
        dlg->SetFolder(startItem);
        startItem->Release();
    }
    bool ok = false;
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                out = p;
                ok = true;
                CoTaskMemFree(p);
            }
            item->Release();
        }
    }
    dlg->Release();
    return ok;
}

std::wstring NameFromFile(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    std::wstring stem = path.substr(slash == std::wstring::npos ? 0 : slash + 1);
    size_t dot = stem.find_last_of(L'.');
    if (dot != std::wstring::npos) stem.erase(dot);
    std::replace(stem.begin(), stem.end(), L'_', L' ');
    return Trim(stem);
}

// ---------------------------------------------------------------------------
// Dialogs
// ---------------------------------------------------------------------------
struct AddDialogData {
    std::wstring file;
    std::wstring name;
};

void UpdateAddHint(HWND dlg) {
    std::wstring name = Trim(GetText(GetDlgItem(dlg, IDC_ADD_NAME)));
    std::wstring hint = name.empty()
                            ? std::wstring(L"Type the name visitors will see under the picture.")
                            : L"Will be copied to assets/gallery/ as \u201c" + Slugify(name) +
                                  L"\u201d, resized to at most 1920 px (JPEG, or PNG if it has transparency). "
                                  L"Your original file isn't changed.";
    SetDlgItemTextW(dlg, IDC_ADD_HINT, hint.c_str());
}

INT_PTR CALLBACK AddDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* d = reinterpret_cast<AddDialogData*>(GetWindowLongPtrW(dlg, DWLP_USER));
    switch (msg) {
    case WM_INITDIALOG:
        d = reinterpret_cast<AddDialogData*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        SetDlgItemTextW(dlg, IDC_ADD_BROWSE, L"Browse\u2026");
        SetDlgItemTextW(dlg, IDC_ADD_FILE, d->file.c_str());
        SetDlgItemTextW(dlg, IDC_ADD_NAME, d->name.c_str());
        UpdateAddHint(dlg);
        SetFocus(GetDlgItem(dlg, IDC_ADD_NAME));
        SendDlgItemMessageW(dlg, IDC_ADD_NAME, EM_SETSEL, 0, -1);
        return FALSE;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_ADD_NAME:
            if (HIWORD(wp) == EN_CHANGE) UpdateAddHint(dlg);
            return TRUE;
        case IDC_ADD_BROWSE: {
            std::wstring file;
            if (PickImageFile(dlg, file)) {
                std::wstring oldName = Trim(GetText(GetDlgItem(dlg, IDC_ADD_NAME)));
                if (oldName.empty() || oldName == NameFromFile(d->file))
                    SetDlgItemTextW(dlg, IDC_ADD_NAME, NameFromFile(file).c_str());
                d->file = file;
                SetDlgItemTextW(dlg, IDC_ADD_FILE, file.c_str());
            }
            return TRUE;
        }
        case IDOK: {
            std::wstring name = Trim(GetText(GetDlgItem(dlg, IDC_ADD_NAME)));
            if (name.empty()) {
                MessageBoxW(dlg, L"Please type a display name for the image.", kAppName, MB_ICONINFORMATION);
                SetFocus(GetDlgItem(dlg, IDC_ADD_NAME));
                return TRUE;
            }
            if (!FileExists(d->file) || !IsSupportedImageExt(d->file)) {
                MessageBoxW(dlg, L"Please choose a .png, .jpg, .jpeg, .webp or .bmp image file.", kAppName,
                            MB_ICONINFORMATION);
                return TRUE;
            }
            d->name = name;
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        case IDCANCEL: EndDialog(dlg, IDCANCEL); return TRUE;
        }
        break;
    }
    return FALSE;
}

INT_PTR CALLBACK RenameDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* name = reinterpret_cast<std::wstring*>(GetWindowLongPtrW(dlg, DWLP_USER));
    switch (msg) {
    case WM_INITDIALOG:
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        SetDlgItemTextW(dlg, IDC_RENAME_NAME, reinterpret_cast<std::wstring*>(lp)->c_str());
        SetFocus(GetDlgItem(dlg, IDC_RENAME_NAME));
        SendDlgItemMessageW(dlg, IDC_RENAME_NAME, EM_SETSEL, 0, -1);
        return FALSE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            std::wstring n = Trim(GetText(GetDlgItem(dlg, IDC_RENAME_NAME)));
            if (n.empty()) {
                MessageBoxW(dlg, L"The name can't be empty.", kAppName, MB_ICONINFORMATION);
                return TRUE;
            }
            *name = n;
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------
void OnBrowseRepo() {
    std::wstring folder;
    if (!PickFolder(g.hwnd, g.repo, folder)) return;
    LoadRepo(folder, true);
    SaveSetting(L"RepoFolder", g.repo);
}

void OnToggle() {
    if (!g.loaded || g.busy) return;
    bool open = !g.data.CommissionsOpen();
    g.data.SetCommissionsOpen(open);
    Commit(open ? L"commissions open" : L"commissions closed", SelectedIndex());
}

void OnAdd() {
    if (!g.loaded || g.busy) return;
    AddDialogData d;
    if (!PickImageFile(g.hwnd, d.file)) return;
    d.name = NameFromFile(d.file);
    if (DialogBoxParamW(g.inst, MAKEINTRESOURCEW(IDD_ADD), g.hwnd, AddDlgProc, (LPARAM)&d) != IDOK) return;

    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    SetStatus(L"Importing image…");
    std::wstring rel, err;
    bool ok = ImportImage(g.repo, d.file, d.name, g.data.Files(), rel, err);
    SetCursor(old);
    if (!ok) {
        SetStatus(L"The image was not added.");
        Message(err, MB_ICONERROR);
        return;
    }
    g.data.Add(d.name, rel);
    if (!Commit(L"added " + d.name, (int)g.data.Count() - 1)) DeleteFileW(RepoPath(g.repo, rel).c_str());
}

void OnRename() {
    int sel = SelectedIndex();
    if (!g.loaded || g.busy || sel < 0) return;
    std::wstring oldName = g.data.Item((size_t)sel).name, name = oldName;
    if (DialogBoxParamW(g.inst, MAKEINTRESOURCEW(IDD_RENAME), g.hwnd, RenameDlgProc, (LPARAM)&name) != IDOK ||
        name == oldName)
        return;
    g.data.Rename((size_t)sel, name);
    Commit(L"renamed " + oldName + L" to " + name, sel);
}

bool RecycleFile(const std::wstring& path) {
    std::wstring from = path;
    from.push_back(L'\0'); // double-null terminated list
    SHFILEOPSTRUCTW op = {};
    op.hwnd = g.hwnd;
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}

void OnRemove() {
    int sel = SelectedIndex();
    if (!g.loaded || g.busy || sel < 0) return;
    GalleryItem it = g.data.Item((size_t)sel);

    // Only offer to delete files inside assets/gallery that no other entry uses.
    std::vector<std::wstring> files = g.data.Files();
    bool canDelete = it.file.rfind(L"assets/gallery/", 0) == 0 && it.file.find(L"..") == std::wstring::npos &&
                     std::count(files.begin(), files.end(), it.file) == 1 && FileExists(RepoPath(g.repo, it.file));

    std::wstring title = L"Remove \u201c" + it.name + L"\u201d from the gallery?";
    std::wstring verify = L"Also delete the image file " + it.file + L" (it goes to the Recycle Bin)";
    TASKDIALOGCONFIG tc = {};
    tc.cbSize = sizeof(tc);
    tc.hwndParent = g.hwnd;
    tc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    tc.dwCommonButtons = TDCBF_YES_BUTTON | TDCBF_NO_BUTTON;
    tc.pszWindowTitle = kAppName;
    tc.pszMainIcon = TD_WARNING_ICON;
    tc.pszMainInstruction = title.c_str();
    tc.pszContent = L"It will disappear from the website after you push your changes.";
    tc.nDefaultButton = IDNO;
    if (canDelete) tc.pszVerificationText = verify.c_str();
    int button = 0;
    BOOL deleteFile = FALSE;
    if (FAILED(TaskDialogIndirect(&tc, &button, nullptr, &deleteFile)) || button != IDYES) return;

    g.data.Remove((size_t)sel);
    if (!Commit(L"removed " + it.name, sel)) return;
    if (canDelete && deleteFile && !RecycleFile(RepoPath(g.repo, it.file)))
        Message(L"The image was removed from the gallery, but the file couldn't be deleted:\n" +
                    RepoPath(g.repo, it.file),
                MB_ICONWARNING);
    RefreshList(sel);
}

void OnMove(int delta) {
    int sel = SelectedIndex();
    if (!g.loaded || g.busy || sel < 0) return;
    int to = sel + delta;
    if (to < 0 || to >= (int)g.data.Count()) return;
    g.data.Move((size_t)sel, (size_t)to);
    Commit(L"reordered gallery", to);
}

enum class GitTask { Connect, Push, Pull };

struct GitTaskResult {
    GitTask task;
    GitResult result;
};

const wchar_t kNoGitHelp[] = L"Git isn't installed, so changes can't be published.\n\n"
                             L"Install GitHub Desktop (desktop.github.com) or Git for Windows (git-scm.com), "
                             L"then restart this app.";

// Runs `work` on a worker thread; output streams into the log, the result arrives as WM_APP_GITDONE.
void RunGitTask(GitTask task, const std::wstring& title, std::function<GitResult(const LogFn&)> work) {
    g.busy = true;
    UpdateButtons();
    SetPushResult(title + L"\u2026 please wait.", kMuted);
    SetStatus(title + L"\u2026");
    AppendLog(L"=== " + title + L" (" + NowText() + L") ===");
    HWND hwnd = g.hwnd;
    std::thread([task, work = std::move(work), hwnd] {
        LogFn log = [hwnd](const std::wstring& line) {
            auto* p = new std::wstring(line);
            if (!PostMessageW(hwnd, WM_APP_LOG, 0, (LPARAM)p)) delete p;
        };
        auto* r = new GitTaskResult{task, work(log)};
        if (!PostMessageW(hwnd, WM_APP_GITDONE, 0, (LPARAM)r)) delete r;
    }).detach();
}

std::wstring RemoteUrlText() {
    std::wstring url = Trim(GetText(g.remote));
    if (!url.empty()) SaveSetting(L"RemoteUrl", url);
    return url;
}

void OnConnect() {
    if (!g.loaded || g.busy) return;
    const std::wstring url = RemoteUrlText();
    if (url.empty()) {
        Message(L"Please enter the GitHub repository address, for example:\n" + std::wstring(kDefaultRemoteUrl),
                MB_ICONINFORMATION);
        SetFocus(g.remote);
        return;
    }
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    RepoInfo info = InspectRepo(g.repo);
    SetCursor(old);
    if (!info.gitAvailable) {
        Message(kNoGitHelp, MB_ICONINFORMATION);
        return;
    }

    std::wstring steps;
    bool changeOrigin = false;
    if (!info.isRepo) steps += L"\u2022 Make this folder a git repository (git init)\n";
    if (info.branch != kBranch)
        steps += info.hasCommits ? L"\u2022 Rename the current branch \u201c" + info.branch + L"\u201d to \u201cmain\u201d\n"
                                 : std::wstring(L"\u2022 Use \u201cmain\u201d as the branch name\n");
    if (info.originUrl.empty()) {
        steps += L"\u2022 Link it to " + url + L"\n";
    } else if (!SameRemoteUrl(info.originUrl, url)) {
        if (Message(L"This folder is already linked to a different GitHub address:\n\n    " + info.originUrl +
                        L"\n\nChange it to:\n\n    " + url + L"\n\n?",
                    MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES)
            return;
        changeOrigin = true;
        steps += L"\u2022 Change its GitHub address to " + url + L"\n";
    }
    if (steps.empty()) {
        Message(L"This folder is already connected to\n" + url +
                    L"\n(branch main). Use \u201cPush changes\u201d to publish.",
                MB_ICONINFORMATION);
        UpdateGitState();
        return;
    }
    if (Message(L"Connect this website folder to GitHub?\n\n" + g.repo + L"\n\nThis will:\n" + steps +
                    L"\nNothing is uploaded until you click \u201cPush changes\u201d.",
                MB_ICONQUESTION | MB_YESNO) != IDYES)
        return;
    const std::wstring repo = g.repo;
    RunGitTask(GitTask::Connect, L"Connecting to GitHub", [repo, url, changeOrigin](const LogFn& log) {
        return ConnectRepo(repo, url, changeOrigin, log);
    });
}

void OnPush() {
    if (!g.loaded || g.busy) return;
    const std::wstring repo = g.repo, summary = CommitSummary();
    RunGitTask(GitTask::Push, L"Publishing to GitHub",
               [repo, summary](const LogFn& log) { return PushChanges(repo, summary, log); });
}

void OnPull(bool allowUnrelated) {
    if (!g.loaded || g.busy) return;
    const std::wstring repo = g.repo;
    RunGitTask(GitTask::Pull, L"Pulling from GitHub",
               [repo, allowUnrelated](const LogFn& log) { return PullChanges(repo, allowUnrelated, log); });
}

void OnGitDone(GitTaskResult* r) {
    const GitTask task = r->task;
    const GitResult res = r->result;
    delete r;
    g.busy = false;
    UpdateButtons();
    AppendLog(res.message);
    AppendLog(L"");
    const wchar_t* what = task == GitTask::Connect ? L"Connect" : task == GitTask::Push ? L"Push" : L"Pull";

    switch (res.outcome) {
    case GitOutcome::Done:
        if (task == GitTask::Pull) LoadRepo(g.repo, true); // GitHub may have changed the site data
        if (task == GitTask::Push) g.changes.clear();
        SetPushResult(L"\u2714 " + res.message, kOpen);
        SetStatus(std::wstring(what) + L" finished at " + NowText() + L".");
        break;
    case GitOutcome::NothingToDo:
        if (task == GitTask::Push) g.changes.clear();
        SetPushResult(L"\u2714 " + res.message, kOpen);
        SetStatus(res.message);
        break;
    case GitOutcome::NoGit:
        SetPushResult(L"\u2716 Git isn't installed.", kClosed);
        Message(kNoGitHelp, MB_ICONINFORMATION);
        break;
    case GitOutcome::NotRepo:
    case GitOutcome::NoRemote:
        SetPushResult(L"\u2716 Not connected to GitHub yet \u2014 click \u201cConnect to GitHub\u2026\u201d.", kClosed);
        SetStatus(res.message);
        if (Message(res.message + L"\n\nYour changes are saved on this computer. Connect this folder to\n" +
                        RemoteUrlText() + L"\nnow?",
                    MB_ICONQUESTION | MB_YESNO) == IDYES)
            OnConnect();
        break;
    case GitOutcome::Rejected:
        SetPushResult(L"\u2716 GitHub has newer changes \u2014 click \u201cPull\u201d, then \u201cPush changes\u201d again.",
                      kClosed);
        SetStatus(L"Push refused: pull first.");
        Message(res.message, MB_ICONWARNING);
        SetFocus(g.pull);
        break;
    case GitOutcome::Unrelated:
        SetPushResult(L"Pull needs your confirmation.", kMuted);
        if (Message(L"This folder and the GitHub repository have separate histories \u2014 for example, the "
                    L"repository was created on github.com with its own files (like a LICENSE or README).\n\n"
                    L"Combine them? Files that exist only on GitHub are added to this folder; where the same file "
                    L"exists in both places, the version in this folder is kept.\n\n"
                    L"Nothing is uploaded until you click \u201cPush changes\u201d.",
                    MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) == IDYES)
            OnPull(true);
        else
            SetPushResult(L"Pull cancelled \u2014 nothing was changed.", kMuted);
        break;
    case GitOutcome::Conflict:
    case GitOutcome::Failed:
        SetPushResult(L"\u2716 " + std::wstring(what) + L" failed \u2014 see the details below.", kClosed);
        SetStatus(std::wstring(what) + L" failed.");
        Message(res.message, MB_ICONERROR);
        break;
    }
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------
void DrawToggle(const DRAWITEMSTRUCT* di) {
    HDC dc = di->hDC;
    RECT rc = di->rcItem;
    FillRect(dc, &rc, GetSysColorBrush(COLOR_WINDOW));
    const bool enabled = !(di->itemState & ODS_DISABLED) && g.loaded;
    const bool open = g.loaded && g.data.CommissionsOpen();
    COLORREF fill = !g.loaded ? RGB(160, 160, 168) : open ? kOpen : kClosed;
    if (di->itemState & ODS_SELECTED) fill = RGB(GetRValue(fill) * 4 / 5, GetGValue(fill) * 4 / 5, GetBValue(fill) * 4 / 5);
    if (!enabled && g.loaded) fill = RGB((GetRValue(fill) + 255) / 2, (GetGValue(fill) + 255) / 2, (GetBValue(fill) + 255) / 2);

    HBRUSH br = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, fill);
    HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, S(14), S(14));
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pen);

    // Status dot + label.
    const int dot = S(16);
    std::wstring text = GetText(di->hwndItem);
    HGDIOBJ of = SelectObject(dc, g.fontBig);
    SIZE ts{};
    GetTextExtentPoint32W(dc, text.c_str(), (int)text.size(), &ts);
    int total = dot + S(12) + ts.cx;
    int x = rc.left + (rc.right - rc.left - total) / 2, cy = (rc.top + rc.bottom) / 2;
    HBRUSH white = (HBRUSH)GetStockObject(WHITE_BRUSH);
    HGDIOBJ ob2 = SelectObject(dc, white), op2 = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, x, cy - dot / 2, x + dot, cy + dot / 2);
    SelectObject(dc, ob2);
    SelectObject(dc, op2);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT tr = {x + dot + S(12), rc.top, rc.right, rc.bottom};
    DrawTextW(dc, text.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, of);

    if ((di->itemState & ODS_FOCUS) && !(di->itemState & ODS_NOFOCUSRECT)) {
        RECT fr = rc;
        InflateRect(&fr, -S(4), -S(4));
        SetTextColor(dc, RGB(0, 0, 0));
        SetBkColor(dc, RGB(255, 255, 255));
        DrawFocusRect(dc, &fr);
    }
}

void PaintPreview(HDC dc) {
    RECT r = g.previewRect;
    if (r.bottom - r.top < S(48)) return;
    HBRUSH frame = CreateSolidBrush(RGB(214, 208, 226));
    FrameRect(dc, &r, frame);
    DeleteObject(frame);
    const int pad = S(6);
    InflateRect(&r, -pad, -pad);
    if (g.thumb) {
        int aw = r.right - r.left, ah = r.bottom - r.top;
        double s = std::min(1.0, std::min((double)aw / g.thumbW, (double)ah / g.thumbH));
        int dw = std::max(1, (int)(g.thumbW * s)), dh = std::max(1, (int)(g.thumbH * s));
        int x = r.left + (aw - dw) / 2, y = r.top + (ah - dh) / 2;
        HDC mem = CreateCompatibleDC(dc);
        HGDIOBJ old = SelectObject(mem, g.thumb);
        BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        AlphaBlend(dc, x, y, dw, dh, mem, 0, 0, (int)g.thumbW, (int)g.thumbH, bf);
        SelectObject(mem, old);
        DeleteDC(mem);
    } else {
        int sel = SelectedIndex();
        const wchar_t* t = !g.loaded ? L"" : sel < 0 ? L"Select an image to preview it" : L"No preview available";
        HGDIOBJ of = SelectObject(dc, g.font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, kMuted);
        DrawTextW(dc, t, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(dc, of);
    }
}

// ---------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------
HWND Ctl(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD exStyle = 0) {
    return CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, g.hwnd,
                           (HMENU)(INT_PTR)id, g.inst, nullptr);
}

void CreateControls() {
    g.folderLabel = Ctl(L"STATIC", L"Website folder:", SS_LEFT, IDC_FOLDER_LABEL);
    g.folder = Ctl(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL | ES_READONLY, IDC_FOLDER, WS_EX_CLIENTEDGE);
    g.browse = Ctl(L"BUTTON", L"&Browse\u2026", WS_TABSTOP | BS_PUSHBUTTON, IDC_BROWSE);

    g.headStatus = Ctl(L"STATIC", L"Commission status", SS_LEFT, IDC_HEAD_STATUS);
    g.toggle = Ctl(L"BUTTON", L"", WS_TABSTOP | BS_OWNERDRAW, IDC_TOGGLE);
    g.toggleDesc = Ctl(L"STATIC", L"", SS_LEFT, IDC_TOGGLE_DESC);

    g.headGallery = Ctl(L"STATIC", L"Gallery \u2014 shown in this order; the first image (\u2605) is the featured one",
                        SS_LEFT | SS_ENDELLIPSIS, IDC_HEAD_GALLERY);
    g.list = Ctl(WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
                 IDC_LIST, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(g.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    SetWindowTheme(g.list, L"Explorer", nullptr);
    const wchar_t* cols[] = {L"#", L"Name", L"Image file"};
    for (int i = 0; i < 3; ++i) {
        LVCOLUMNW c = {};
        c.mask = LVCF_TEXT | LVCF_WIDTH;
        c.pszText = const_cast<wchar_t*>(cols[i]);
        c.cx = 100;
        ListView_InsertColumn(g.list, i, &c);
    }
    g.add = Ctl(L"BUTTON", L"&Add image\u2026", WS_TABSTOP | BS_PUSHBUTTON, IDC_ADD);
    g.rename = Ctl(L"BUTTON", L"Re&name\u2026", WS_TABSTOP | BS_PUSHBUTTON, IDC_RENAME);
    g.remove = Ctl(L"BUTTON", L"&Remove\u2026", WS_TABSTOP | BS_PUSHBUTTON, IDC_REMOVE);
    g.up = Ctl(L"BUTTON", L"Move &up", WS_TABSTOP | BS_PUSHBUTTON, IDC_UP);
    g.down = Ctl(L"BUTTON", L"Move &down", WS_TABSTOP | BS_PUSHBUTTON, IDC_DOWN);

    g.headPublish = Ctl(L"STATIC", L"Publish to the website", SS_LEFT, IDC_HEAD_PUBLISH);
    g.remoteLabel = Ctl(L"STATIC", L"GitHub repository:", SS_LEFT, IDC_REMOTE_LABEL);
    g.remote = Ctl(L"EDIT", LoadSetting(L"RemoteUrl", kDefaultRemoteUrl).c_str(), WS_TABSTOP | ES_AUTOHSCROLL,
                   IDC_REMOTE, WS_EX_CLIENTEDGE);
    g.connect = Ctl(L"BUTTON", L"&Connect to GitHub\u2026", WS_TABSTOP | BS_PUSHBUTTON, IDC_CONNECT);
    g.push = Ctl(L"BUTTON", L"&Push changes", WS_TABSTOP | BS_PUSHBUTTON, IDC_PUSH);
    g.pull = Ctl(L"BUTTON", L"P&ull", WS_TABSTOP | BS_PUSHBUTTON, IDC_PULL);
    g.pushResult = Ctl(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX, IDC_PUSH_RESULT);
    g.log = Ctl(L"EDIT", L"", WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, IDC_LOG,
                WS_EX_CLIENTEDGE);
    SendMessageW(g.log, EM_SETLIMITTEXT, 0x7FFFFFFE, 0);
    g.status = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, g.hwnd,
                               (HMENU)(INT_PTR)IDC_STATUSBAR, g.inst, nullptr);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g.hwnd = hwnd;
        g.dpi = GetDpiForWindow(hwnd);
        CreateFonts();
        CreateControls();
        ApplyFonts();
        PostMessageW(hwnd, WM_APP_INIT, 0, 0);
        return 0;

    case WM_APP_INIT:
        LoadRepo(g.repo, true);
        return 0;

    case WM_SIZE:
        Layout();
        return 0;

    case WM_TIMER:
        if (wp == kThumbTimer) {
            KillTimer(hwnd, kThumbTimer);
            UpdatePreview();
        }
        return 0;

    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize = {S(700), S(640)};
        return 0;
    }

    case WM_DPICHANGED: {
        g.dpi = HIWORD(wp);
        CreateFonts();
        ApplyFonts();
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
        UpdatePreview();
        return 0;
    }

    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        HWND ctl = (HWND)lp;
        SetBkColor(dc, GetSysColor(COLOR_WINDOW));
        if (ctl == g.headStatus || ctl == g.headGallery || ctl == g.headPublish) SetTextColor(dc, kAccent);
        else if (ctl == g.pushResult) SetTextColor(dc, g.pushResultColor);
        else SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }

    case WM_DRAWITEM: {
        auto* di = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (di->CtlID == IDC_TOGGLE) {
            DrawToggle(di);
            return TRUE;
        }
        break;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        PaintPreview(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_BROWSE: OnBrowseRepo(); return 0;
        case IDC_TOGGLE: OnToggle(); return 0;
        case IDC_ADD: OnAdd(); return 0;
        case IDC_RENAME: OnRename(); return 0;
        case IDC_REMOVE: OnRemove(); return 0;
        case IDC_UP: OnMove(-1); return 0;
        case IDC_DOWN: OnMove(+1); return 0;
        case IDC_CONNECT: OnConnect(); return 0;
        case IDC_PUSH: OnPush(); return 0;
        case IDC_PULL: OnPull(false); return 0;
        case IDC_REMOTE:
            if (HIWORD(wp) == EN_KILLFOCUS) {
                RemoteUrlText();
                UpdateGitState();
            }
            return 0;
        }
        break;

    case WM_NOTIFY: {
        auto* nm = reinterpret_cast<NMHDR*>(lp);
        if (nm->idFrom != IDC_LIST) break;
        if (nm->code == LVN_ITEMCHANGED) {
            auto* lv = reinterpret_cast<NMLISTVIEW*>(lp);
            if ((lv->uChanged & LVIF_STATE) && ((lv->uOldState ^ lv->uNewState) & LVIS_SELECTED)) {
                UpdateButtons();
                UpdatePreview();
            }
        } else if (nm->code == NM_DBLCLK) {
            OnRename();
        } else if (nm->code == LVN_KEYDOWN) {
            auto* kd = reinterpret_cast<NMLVKEYDOWN*>(lp);
            const bool ctrl = GetKeyState(VK_CONTROL) < 0;
            if (kd->wVKey == VK_DELETE) OnRemove();
            else if (kd->wVKey == VK_F2) OnRename();
            else if (kd->wVKey == VK_INSERT) OnAdd();
            else if (ctrl && kd->wVKey == VK_UP) OnMove(-1);
            else if (ctrl && kd->wVKey == VK_DOWN) OnMove(+1);
        }
        return 0;
    }

    case WM_APP_LOG: {
        auto* line = reinterpret_cast<std::wstring*>(lp);
        AppendLog(*line);
        delete line;
        return 0;
    }

    case WM_APP_GITDONE:
        OnGitDone(reinterpret_cast<GitTaskResult*>(lp));
        return 0;

    case WM_CLOSE:
        if (g.busy && Message(L"Git is still working. Quit anyway?", MB_ICONWARNING | MB_YESNO) != IDYES)
            return 0;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (g.thumb) DeleteObject(g.thumb);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    // Fallback for when the manifest isn't embedded; harmless otherwise.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    g.inst = inst;

    // Optional command-line folder (used for this session only, not remembered).
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc >= 2) {
        g.repo = argv[1];
    } else {
        g.repo = LoadSetting(L"RepoFolder", kDefaultRepo);
    }
    if (argv) LocalFree(argv);

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = kClassName;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, kClassName, kAppName, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                                CW_USEDEFAULT, 900, 760, nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;

    // Size for the monitor's DPI and fit inside its work area.
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
    const RECT& wa = mi.rcWork;
    int w = std::min(S(940), (int)(wa.right - wa.left)), h = std::min(S(800), (int)(wa.bottom - wa.top));
    SetWindowPos(hwnd, nullptr, wa.left + (wa.right - wa.left - w) / 2, wa.top + (wa.bottom - wa.top - h) / 2, w, h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    CoUninitialize();
    return (int)msg.wParam;
}
