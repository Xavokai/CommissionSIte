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
#include <memory>
#include <thread>

namespace {

constexpr UINT WM_APP_INIT = WM_APP + 1;
constexpr UINT WM_APP_LOG = WM_APP + 2;      // lParam: std::wstring*
constexpr UINT WM_APP_GITDONE = WM_APP + 3;  // lParam: GitTaskResult*
constexpr UINT WM_APP_ACCOUNT = WM_APP + 4;  // lParam: GitHubAccount*
constexpr UINT WM_APP_LIVE = WM_APP + 5;     // lParam: LiveMsg*
constexpr UINT_PTR kThumbTimer = 1;
constexpr UINT_PTR kDisabledTipOffset = 10000; // tooltip ids for the "why is this disabled" rectangles

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
    IDC_STEPS,
    IDC_ACCOUNT_LABEL,
    IDC_ACCOUNT,
    IDC_SIGNIN,
    IDC_TOKEN,
    IDC_SIGNOUT,
    IDC_REMOTE_LABEL,
    IDC_REMOTE,
    IDC_CONNECT,
    IDC_PUSH,
    IDC_SYNC,
    IDC_OPEN_SITE,
    IDC_PUSH_RESULT,
    IDC_LOG,
    IDC_STATUSBAR,
};

// One "wait until the website shows the upload" check, shared by the UI and its worker thread.
struct LiveCheck {
    HANDLE cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr); // set = stop waiting
    LiveCheck() = default;
    LiveCheck(const LiveCheck&) = delete;
    LiveCheck& operator=(const LiveCheck&) = delete;
    ~LiveCheck() {
        if (cancel) CloseHandle(cancel);
    }
};

struct AppState {
    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    UINT dpi = 96;
    HFONT font = nullptr, fontHead = nullptr, fontBig = nullptr, fontMono = nullptr, fontBold = nullptr, fontLink = nullptr;

    HWND folderLabel{}, folder{}, browse{}, headStatus{}, toggle{}, toggleDesc{}, headGallery{}, list{}, add{},
        rename{}, remove{}, up{}, down{}, headPublish{}, steps{}, accountLabel{}, accountText{}, signIn{}, token{},
        signOut{}, remoteLabel{}, remote{}, connect{}, push{}, sync{}, openSite{}, pushResult{}, log{}, status{}, tips{};

    std::wstring repo;
    SiteData data;
    bool loaded = false;
    bool busy = false;      // a git task is running
    bool signingIn = false; // ...and it is the browser sign-in (which can be cancelled)
    std::vector<std::wstring> changes; // since last successful push → commit summary
    COLORREF pushResultColor = RGB(110, 110, 120);
    std::wstring pushResultLink; // opened when the push result line is clicked ("" = plain text)

    // Waiting for GitHub Pages after a push (see StartLiveCheck)
    std::shared_ptr<LiveCheck> live; // the running check, if any
    unsigned liveId = 0;             // messages from older checks are ignored
    std::wstring liveVersion, liveSite, liveActions, liveNote;
    ULONGLONG liveStart = 0;

    // GitHub state
    GitHubAccount account;
    bool accountChecked = false, accountChecking = false;
    bool startupSync = true; // sync once after the first sign-in check, if signed in and connected
    RepoInfo repoInfo;
    PublishState pub;
    bool connected = false; // repository with origin == the address in the box
    COLORREF accountColor = RGB(110, 110, 120);

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

// The line under the Push button. With a `link`, it is underlined and opens the link when clicked.
void SetPushResult(const std::wstring& text, COLORREF color, const std::wstring& link = L"") {
    g.pushResultColor = color;
    g.pushResultLink = link;
    SendMessageW(g.pushResult, WM_SETFONT, (WPARAM)(link.empty() ? g.font : g.fontLink), FALSE);
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
HFONT MakeFont(int pt, int weight, const wchar_t* face, bool underline = false) {
    return CreateFontW(-MulDiv(pt, (int)g.dpi, 72), 0, 0, 0, weight, FALSE, underline, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
}

void CreateFonts() {
    for (HFONT f : {g.font, g.fontHead, g.fontBig, g.fontMono, g.fontBold, g.fontLink})
        if (f) DeleteObject(f);
    g.font = MakeFont(10, FW_NORMAL, L"Segoe UI");
    g.fontBold = MakeFont(10, FW_BOLD, L"Segoe UI");
    g.fontHead = MakeFont(12, FW_SEMIBOLD, L"Segoe UI");
    g.fontBig = MakeFont(15, FW_BOLD, L"Segoe UI");
    g.fontMono = MakeFont(9, FW_NORMAL, L"Consolas");
    g.fontLink = MakeFont(10, FW_NORMAL, L"Segoe UI", true);
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
    if (!g.pushResultLink.empty()) SendMessageW(g.pushResult, WM_SETFONT, (WPARAM)g.fontLink, FALSE);
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

// Buttons that explain themselves in a tooltip, also while disabled.
std::vector<HWND> TipControls() {
    return {g.signIn, g.token, g.signOut, g.connect, g.push, g.sync, g.openSite, g.toggle, g.add, g.rename, g.remove, g.up, g.down};
}

// Disabled windows get no mouse messages, so each button also has a rectangle tool on the main window.
void UpdateTipRects() {
    for (HWND h : TipControls()) {
        TOOLINFOW ti = {sizeof(ti)};
        ti.hwnd = g.hwnd;
        ti.uId = kDisabledTipOffset + GetDlgCtrlID(h);
        GetWindowRect(h, &ti.rect);
        MapWindowPoints(nullptr, g.hwnd, (POINT*)&ti.rect, 2);
        SendMessageW(g.tips, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
    }
}

void SetTip(HWND h, const std::wstring& text) {
    TOOLINFOW ti = {sizeof(ti)};
    ti.hwnd = g.hwnd;
    ti.lpszText = const_cast<wchar_t*>(text.c_str());
    ti.uFlags = TTF_IDISHWND;
    ti.uId = (UINT_PTR)h;
    SendMessageW(g.tips, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
    ti.uFlags = 0;
    ti.uId = kDisabledTipOffset + GetDlgCtrlID(h);
    SendMessageW(g.tips, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
}

// Enables `h`; the tooltip says what it does, or why it can't be used yet.
void Enable(HWND h, const std::wstring& whyNot, const std::wstring& tip) {
    EnableWindow(h, whyNot.empty());
    SetTip(h, whyNot.empty() ? tip : whyNot);
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
    const int pushH = S(34), rowH = btnH + S(8), stepsH = S(30), sectionGap = S(14), resultH = S(26);
    const int avail = H - m - y - sectionGap - headGap - stepsH - 2 * rowH - pushH - resultH - S(10);
    int logH = S(120), galH = avail - logH;
    if (galH < S(250)) {
        logH = std::max(S(64), avail - S(250));
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
    Place(dwp, g.steps, m, y, W - 2 * m, stepsH - S(4));
    y += stepsH;
    const int signOutW = S(92), tokenW = S(150), signInW = S(170);
    int bx = W - m - signOutW;
    Place(dwp, g.signOut, bx, y, signOutW, btnH);
    bx -= S(8) + tokenW;
    Place(dwp, g.token, bx, y, tokenW, btnH);
    bx -= S(8) + signInW;
    Place(dwp, g.signIn, bx, y, signInW, btnH);
    Place(dwp, g.accountLabel, m, y + S(5), labelW, S(22));
    Place(dwp, g.accountText, fieldX, y + S(5), bx - S(8) - fieldX, S(22));
    y += rowH;
    Place(dwp, g.remoteLabel, m, y + S(5), labelW, S(22));
    Place(dwp, g.remote, fieldX, y + S(1), W - m - fieldX - S(190), S(28));
    Place(dwp, g.connect, W - m - S(182), y, S(182), btnH);
    y += rowH;
    Place(dwp, g.push, m, y, S(170), pushH);
    Place(dwp, g.sync, m + S(180), y, S(170), pushH);
    Place(dwp, g.openSite, W - m - S(182), y, S(182), pushH);
    y += pushH + S(4);
    Place(dwp, g.pushResult, m, y, W - 2 * m, resultH - S(4));
    y += resultH + S(6);
    Place(dwp, g.log, m, y, W - 2 * m, logH);
    EndDeferWindowPos(dwp);
    UpdateTipRects();

    const int numW = S(56), githubW = S(130), nameW = std::max(S(120), (listW - numW - githubW - S(24)) * 2 / 5);
    ListView_SetColumnWidth(g.list, 0, numW);
    ListView_SetColumnWidth(g.list, 1, nameW);
    ListView_SetColumnWidth(g.list, 2, std::max(S(120), listW - numW - nameW - githubW - S(24)));
    ListView_SetColumnWidth(g.list, 3, githubW);

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

const wchar_t kNoGitHelp[] = L"Git isn't installed, so changes can't be published.\n\n"
                             L"Install Git for Windows (git-scm.com) \u2014 it includes the GitHub sign-in helper \u2014 "
                             L"then restart this app.";

bool RemoteNeedsSignIn() { return _wcsnicmp(Trim(GetText(g.remote)).c_str(), L"https://", 8) == 0; }

// The GitHub address the website is published from: the folder's "origin", else the address in step 2.
std::wstring SiteRemoteUrl() { return g.repoInfo.originUrl.empty() ? Trim(GetText(g.remote)) : g.repoInfo.originUrl; }

// The first reason that applies, or "" when the control can be used.
std::wstring WhyNot(std::initializer_list<std::pair<bool, std::wstring>> checks) {
    for (auto& c : checks)
        if (c.first) return c.second;
    return {};
}

void UpdateButtons() {
    const int sel = SelectedIndex(), count = g.loaded ? (int)g.data.Count() : 0;
    const std::wstring busy = L"Please wait until the current GitHub task has finished.";
    const std::wstring noSite = L"Choose your website folder first (the one with data\\site-data.js).";
    const std::wstring pick = L"Select an image in the list first.";
    EnableWindow(g.browse, !g.busy);
    EnableWindow(g.list, g.loaded);
    Enable(g.toggle, WhyNot({{g.busy, busy}, {!g.loaded, noSite}}), L"Switch the website between commissions OPEN and CLOSED.");
    Enable(g.add, WhyNot({{g.busy, busy}, {!g.loaded, noSite}}), L"Copy an image into the gallery (Insert).");
    Enable(g.rename, WhyNot({{g.busy, busy}, {!g.loaded, noSite}, {sel < 0, pick}}),
           L"Change the name shown on the website (F2 or double-click).");
    Enable(g.remove, WhyNot({{g.busy, busy}, {!g.loaded, noSite}, {sel < 0, pick}}),
           L"Take the selected image out of the gallery (Delete).");
    Enable(g.up, WhyNot({{g.busy, busy}, {!g.loaded, noSite}, {sel < 0, pick}, {sel == 0, L"This image is already first (featured)."}}),
           L"Show this image earlier (Ctrl+Up). The first image is the featured one.");
    Enable(g.down, WhyNot({{g.busy, busy}, {!g.loaded, noSite}, {sel < 0, pick}, {sel + 1 >= count, L"This image is already last."}}),
           L"Show this image later (Ctrl+Down).");
    EnableWindow(g.remote, !g.busy);

    const bool git = g.repoInfo.gitAvailable;
    const std::wstring noGit = L"Git isn't installed. Install Git for Windows (git-scm.com), then restart this app.";
    const std::wstring checking = L"Checking whether you're signed in to GitHub\u2026";
    const bool firstCheck = g.accountChecking && !g.accountChecked;
    // Step 1: sign in
    if (g.signingIn) {
        SetWindowTextW(g.signIn, L"Cancel sign-in");
        Enable(g.signIn, L"", L"Stop waiting for the sign-in in your web browser.");
    } else {
        SetWindowTextW(g.signIn, L"Sign in to GitHub\u2026"); // no mnemonic: a stray key must never start a sign-in
        Enable(g.signIn,
               WhyNot({{!git, noGit}, {g.busy, busy}, {firstCheck, checking},
                       {g.account.signedIn, L"You're signed in as " + g.account.username +
                                                L". To use a different account, click \u201cSign out\u201d first."}}),
               g.accountChecked && g.account.gcmVersion.empty()
                   ? L"Sign in by pasting a GitHub access token."
                   : L"Opens GitHub in your web browser: sign in there and click \u201cAuthorize\u201d. The sign-in is "
                     L"kept safely in Windows Credential Manager, and Push uses it automatically.");
    }
    Enable(g.token, WhyNot({{!git, noGit}, {g.busy, busy}, {g.account.signedIn, L"You're already signed in. Sign out first to use a token instead."}}),
           L"Sign in by pasting a GitHub access token instead of using the browser.");
    Enable(g.signOut, WhyNot({{g.busy, busy}, {!g.account.signedIn && g.account.username.empty(), L"You aren't signed in."}}),
           L"Remove the saved GitHub sign-in from this computer.");
    // Step 2: connect the folder
    Enable(g.connect, WhyNot({{!git, noGit}, {g.busy, busy}, {!DirExists(g.repo), L"Choose your website folder first."}}),
           g.connected ? L"This folder is connected to the repository address on the left."
                       : L"Link this website folder to the GitHub repository (one time). Nothing is uploaded yet.");
    // Step 3: publish
    const bool needSignIn = RemoteNeedsSignIn() && !g.account.signedIn;
    Enable(g.push,
           WhyNot({{!git, noGit}, {g.busy, busy}, {!g.loaded, noSite}, {needSignIn && firstCheck, checking},
                   {needSignIn, L"Sign in to GitHub first (step 1)."}, {!g.connected, L"Connect this folder to GitHub first (step 2)."}}),
           L"Upload every change in this folder to GitHub (GitHub's newer changes are downloaded and combined first).");
    Enable(g.sync, WhyNot({{!git, noGit}, {g.busy, busy}, {!g.connected, L"Connect this folder to GitHub first (step 2)."}}),
           L"Download what's on GitHub (site data and gallery images) into this folder.");
    Enable(g.openSite, L"", L"Open your website (" + SiteUrlFor(SiteRemoteUrl()) +
                                L") in your web browser \u2014 always the newest copy, not an old one the browser kept.");
    HWND focus = GetFocus();
    if (!focus || (focus != g.hwnd && !IsWindowEnabled(focus))) SetFocus(g.loaded ? g.list : g.browse);
}

void UpdateAccountText() {
    std::wstring text;
    COLORREF color = kMuted;
    if (g.signingIn) {
        text = L"Waiting for you to sign in in your web browser\u2026";
        color = kAccent;
    } else if (!g.accountChecked) {
        text = L"Checking\u2026";
    } else if (!g.account.gitAvailable) {
        text = L"\u2716 Git isn't installed";
        color = kClosed;
    } else if (g.account.signedIn) {
        text = L"\u2714 Signed in as " + g.account.username + (g.account.verified ? L"" : L" (" + g.account.note + L")");
        color = kOpen;
    } else {
        text = L"\u2716 " + (g.account.note.empty() ? std::wstring(L"Not signed in \u2014 click \u201cSign in to GitHub\u201d")
                                                    : g.account.note);
        color = kClosed;
    }
    g.accountColor = color;
    SetWindowTextW(g.accountText, text.c_str());
    InvalidateRect(g.accountText, nullptr, TRUE);
    InvalidateRect(g.steps, nullptr, TRUE);
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

// "On GitHub" column: compares the gallery file with the last download from GitHub.
std::wstring GitHubColumn(const std::wstring& file) {
    if (!g.connected) return L"";
    if (!g.pub.remoteKnown) return L"\u2014";
    auto has = [&](const std::vector<std::wstring>& v) {
        return std::any_of(v.begin(), v.end(), [&](const std::wstring& f) { return _wcsicmp(f.c_str(), file.c_str()) == 0; });
    };
    if (!has(g.pub.remoteGallery)) return L"only on this PC";
    if (has(g.pub.changedGallery)) return L"changed here";
    return L"\u2714 on GitHub";
}

void RefreshGitHubColumn() {
    if (!g.loaded) return;
    for (size_t i = 0; i < g.data.Count() && (int)i < ListView_GetItemCount(g.list); ++i) {
        std::wstring text = GitHubColumn(g.data.Item(i).file);
        ListView_SetItemText(g.list, (int)i, 3, text.data());
    }
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
        RefreshGitHubColumn();
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

// Re-reads the folder's git state (a few quick local git queries) and updates steps, buttons and the list.
void UpdateGitState() {
    if (g.busy) return;
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    g.repoInfo = InspectRepo(g.repo);
    g.connected = g.repoInfo.isRepo && !g.repoInfo.originUrl.empty() && SameRemoteUrl(g.repoInfo.originUrl, GetText(g.remote));
    g.pub = g.connected ? GetPublishState(g.repo) : PublishState();
    SetCursor(old);
    RefreshGitHubColumn();
    InvalidateRect(g.steps, nullptr, TRUE);
    UpdateButtons();
}

// One line under the Push button saying what to do next.
void ShowStateHint() {
    const RepoInfo& i = g.repoInfo;
    if (!i.gitAvailable)
        SetPushResult(L"Git isn't installed \u2014 install Git for Windows (git-scm.com) to publish.", kClosed);
    else if (!i.isRepo || i.originUrl.empty())
        SetPushResult(L"Not connected to GitHub yet \u2014 click \u201cConnect folder\u2026\u201d (step 2).", kClosed);
    else if (!g.connected)
        SetPushResult(L"Linked to a different address (" + i.originUrl + L") \u2014 click \u201cConnect folder\u2026\u201d to change it.",
                      kClosed);
    else if (!g.pub.remoteKnown)
        SetPushResult(L"Connected. Click \u201cSync from GitHub\u201d to see what's on GitHub.", kMuted);
    else if (!g.pub.remoteHasSite)
        SetPushResult(L"GitHub repo has no site files yet \u2014 click \u201cPush changes\u201d to upload this folder.", kAccent);
    else if (g.pub.Unpublished())
        SetPushResult(L"Some changes here aren't on GitHub yet \u2014 click \u201cPush changes\u201d to publish them.", kMuted);
    else
        SetPushResult(L"\u2714 GitHub has everything in this folder.", kOpen);
}

void CancelLiveCheck();

// Loads site data from `path`. Shows a message box on failure when `interactive`.
bool LoadRepo(const std::wstring& path, bool interactive) {
    if (path != g.repo) {
        g.changes.clear();
        CancelLiveCheck();
    }
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
    }
    RefreshAll(0);
    UpdateGitState();
    ShowStateHint();
    if (!g.loaded && interactive)
        Message(err + (DirExists(path) ? L"\n\nIf your website is already on GitHub, you can download it into this folder: "
                                         L"connect the folder (step 2), then click \u201cSync from GitHub\u201d."
                                       : L""),
                MB_ICONERROR);
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

enum class GitTask { Connect, Push, Sync, SignIn, SaveToken, SignOut };

struct GitTaskResult {
    GitTask task;
    GitResult result;
    bool automatic; // started by the app itself (startup check): no pop-ups
};

// ---- Waiting for GitHub Pages ----------------------------------------------------------------------
struct LiveMsg {
    unsigned id;
    bool done;
    int attempt;
    std::wstring seen, problem;
    LiveOutcome outcome;
};

// Opens a web page. `fresh` adds "?t=<ms>", so the browser loads the newest copy of the website.
void OpenUrl(const std::wstring& url, bool fresh) {
    const std::wstring target = fresh ? WithCacheBuster(url, UnixTimeMs()) : url;
    ShellExecuteW(g.hwnd, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void OnOpenWebsite() {
    const std::wstring url = SiteUrlFor(SiteRemoteUrl());
    AppendLog(L"Opening " + url + L" (newest copy) in your web browser.");
    OpenUrl(url, true);
}

void CancelLiveCheck() {
    if (g.live) SetEvent(g.live->cancel);
    g.live.reset();
    ++g.liveId;
}

std::wstring ElapsedText() {
    const ULONGLONG s = (GetTickCount64() - g.liveStart) / 1000;
    wchar_t buf[32];
    swprintf_s(buf, L"%llu:%02llu", s / 60, s % 60);
    return buf;
}

// After a push: checks the website every 10 s (up to 5 min, on a worker thread) until it shows `version`.
void StartLiveCheck(const std::wstring& version) {
    CancelLiveCheck();
    auto check = std::make_shared<LiveCheck>();
    g.live = check;
    const unsigned id = g.liveId;
    g.liveVersion = version;
    g.liveSite = SiteUrlFor(SiteRemoteUrl());
    g.liveActions = ActionsUrlFor(SiteRemoteUrl());
    g.liveStart = GetTickCount64();
    g.liveNote.clear();
    const std::wstring waiting = L"Uploaded \u2714 \u2014 waiting for GitHub Pages to publish\u2026";
    SetPushResult(waiting, kAccent);
    SetStatus(waiting);
    AppendLog(waiting + L" (checking " + g.liveSite + L" for version " + version +
              L" every 10 seconds, for up to 5 minutes)");
    HWND hwnd = g.hwnd;
    const std::wstring site = g.liveSite;
    std::thread([check, id, hwnd, site, version] {
        auto post = [hwnd](LiveMsg* m) {
            if (!PostMessageW(hwnd, WM_APP_LIVE, 0, (LPARAM)m)) delete m;
        };
        LiveCheckResult r = WaitUntilLive(site, version, check->cancel, kLiveCheckIntervalMs, kLiveCheckTimeoutMs,
                                          [&](int attempt, const std::wstring& seen, const std::wstring& problem) {
                                              post(new LiveMsg{id, false, attempt, seen, problem, LiveOutcome::TimedOut});
                                          });
        if (r.outcome != LiveOutcome::Cancelled) post(new LiveMsg{id, true, r.attempts, r.seenVersion, L"", r.outcome});
    }).detach();
}

void OnLiveMsg(LiveMsg* raw) {
    std::unique_ptr<LiveMsg> m(raw);
    if (m->id != g.liveId || !g.live) return; // cancelled meanwhile
    const std::wstring elapsed = ElapsedText();
    if (!m->done) {
        if (m->problem.empty() && !m->seen.empty() && m->seen >= g.liveVersion) return; // "Live" follows
        std::wstring note = !m->problem.empty() ? L"the website isn't showing the new version yet (" + m->problem + L")"
                                                : L"the website still shows the previous version (" + m->seen + L")";
        SetPushResult(L"Uploaded \u2714 \u2014 waiting for GitHub Pages to publish\u2026 (" + elapsed + L")", kAccent);
        SetStatus(L"Waiting for GitHub Pages (" + elapsed + L", check " + std::to_wstring(m->attempt) + L"): " + note + L".");
        if (note != g.liveNote) AppendLog(L"  " + NowText() + L"  " + note);
        g.liveNote = note;
        return;
    }
    g.live.reset();
    if (m->outcome == LiveOutcome::Live) {
        const std::wstring live = L"Live on the website \u2714 (version " + m->seen + L")";
        SetPushResult(live + L" \u2014 click to open it", kOpen, g.liveSite);
        SetStatus(live + L" at " + NowText() + L".");
        AppendLog(live + L", " + elapsed + L" after uploading.");
        AppendLog(L"");
    } else {
        SetPushResult(L"Uploaded, but the website hasn't updated after 5 minutes \u2014 click to see GitHub's progress",
                      kAccent, g.liveActions);
        SetStatus(L"The website hasn't shown the new version yet (checked for 5 minutes).");
        AppendLog(L"GitHub has your changes, but after 5 minutes the website still showed " +
                  (m->seen.empty() ? std::wstring(L"an older copy") : L"the previous version (" + m->seen + L")") +
                  L". GitHub Pages is sometimes slow; it usually catches up within a few more minutes. You can watch "
                  L"its progress here (or click the message under \u201cPush changes\u201d):\n  " + g.liveActions +
                  L"\nLater, click \u201cOpen website\u201d to see the newest copy.");
        AppendLog(L"");
    }
}

// Runs `work` on a worker thread; output streams into the log, the result arrives as WM_APP_GITDONE.
void RunGitTask(GitTask task, const std::wstring& title, std::function<GitResult(const LogFn&)> work,
                bool automatic = false) {
    CancelLiveCheck(); // the line under the Push button is needed for this task now
    g.busy = true;
    UpdateButtons();
    UpdateAccountText();
    SetPushResult(title + L"\u2026 please wait.", kMuted);
    SetStatus(title + L"\u2026");
    AppendLog(L"=== " + title + L" (" + NowText() + L") ===");
    HWND hwnd = g.hwnd;
    std::thread([task, work = std::move(work), hwnd, automatic] {
        LogFn log = [hwnd](const std::wstring& line) {
            auto* p = new std::wstring(line);
            if (!PostMessageW(hwnd, WM_APP_LOG, 0, (LPARAM)p)) delete p;
        };
        auto* r = new GitTaskResult{task, work(log), automatic};
        if (!PostMessageW(hwnd, WM_APP_GITDONE, 0, (LPARAM)r)) delete r;
    }).detach();
}

// Checks the saved GitHub sign-in in the background; the result arrives as WM_APP_ACCOUNT.
void StartAccountCheck() {
    if (g.accountChecking) return;
    g.accountChecking = true;
    UpdateAccountText();
    UpdateButtons();
    const std::wstring preferred = LoadSetting(L"GitHubUser", L"");
    HWND hwnd = g.hwnd;
    std::thread([preferred, hwnd] {
        auto* a = new GitHubAccount(CheckGitHubAccount(preferred));
        if (!PostMessageW(hwnd, WM_APP_ACCOUNT, 0, (LPARAM)a)) delete a;
    }).detach();
}

std::wstring RemoteUrlText() {
    std::wstring url = Trim(GetText(g.remote));
    if (!url.empty()) SaveSetting(L"RemoteUrl", url);
    return url;
}

void OnSync(MergeChoice choice, bool automatic) {
    if (g.busy || !DirExists(g.repo)) return;
    const std::wstring repo = g.repo, summary = CommitSummary(), user = g.account.signedIn ? g.account.username : L"";
    RunGitTask(GitTask::Sync, automatic ? L"Checking GitHub for changes" : L"Syncing from GitHub",
               [=](const LogFn& log) { return SyncFromGitHub(repo, user, summary, choice, log); }, automatic);
}

void OnPush(MergeChoice choice) {
    if (!g.loaded || g.busy) return;
    const std::wstring repo = g.repo, summary = CommitSummary(), user = g.account.signedIn ? g.account.username : L"";
    RunGitTask(GitTask::Push, L"Publishing to GitHub",
               [=](const LogFn& log) { return PushChanges(repo, user, summary, choice, log); });
}

void OnConnect() {
    if (g.busy || !DirExists(g.repo)) return;
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
        UpdateGitState();
        Message(L"This folder is already connected to\n" + url +
                    L"\n(branch main). Use \u201cSync from GitHub\u201d to download and \u201cPush changes\u201d to publish.",
                MB_ICONINFORMATION);
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

// ---- Sign-in ----------------------------------------------------------------------------------
struct TokenDialogData {
    std::wstring owner, name; // repository the token must be allowed to change
    std::wstring token, login;
};

INT_PTR CALLBACK TokenDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* d = reinterpret_cast<TokenDialogData*>(GetWindowLongPtrW(dlg, DWLP_USER));
    switch (msg) {
    case WM_INITDIALOG: {
        d = reinterpret_cast<TokenDialogData*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        const std::wstring info =
            L"A token lets this app upload to GitHub for you. To make one:\r\n"
            L"1.  Click \u201cCreate a token on GitHub\u201d (sign in there if asked).\r\n"
            L"2.  Token name: e.g. \u201cCommission Manager\u201d. Expiration: your choice (e.g. 1 year).\r\n"
            L"3.  Repository access: \u201cOnly select repositories\u201d \u2192 " + d->name + L".\r\n"
            L"4.  Permissions \u2192 Repository permissions \u2192 Contents \u2192 \u201cRead and write\u201d.\r\n"
            L"5.  Click \u201cGenerate token\u201d, copy it, and paste it below.";
        SetDlgItemTextW(dlg, IDC_TOKEN_INFO, info.c_str());
        SetDlgItemTextW(dlg, IDC_TOKEN_OPEN, L"Create a token on GitHub\u2026");
        SetDlgItemTextW(dlg, IDC_TOKEN_STATUS, L"The token is checked with GitHub, then kept in Windows Credential Manager.");
        SetFocus(GetDlgItem(dlg, IDC_TOKEN_EDIT));
        return FALSE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_TOKEN_OPEN:
            ShellExecuteW(dlg, L"open", kNewTokenUrl, nullptr, nullptr, SW_SHOWNORMAL);
            return TRUE;
        case IDC_TOKEN_SHOW: {
            const bool show = IsDlgButtonChecked(dlg, IDC_TOKEN_SHOW) == BST_CHECKED;
            SendDlgItemMessageW(dlg, IDC_TOKEN_EDIT, EM_SETPASSWORDCHAR, show ? 0 : 0x25CF, 0);
            InvalidateRect(GetDlgItem(dlg, IDC_TOKEN_EDIT), nullptr, TRUE);
            return TRUE;
        }
        case IDOK: {
            std::wstring pasted = GetText(GetDlgItem(dlg, IDC_TOKEN_EDIT));
            std::wstring token = CleanToken(pasted);
            SecureZeroMemory(pasted.data(), pasted.size() * sizeof(wchar_t));
            if (token.empty()) {
                SetDlgItemTextW(dlg, IDC_TOKEN_STATUS, L"Paste the token into the box first.");
                SetFocus(GetDlgItem(dlg, IDC_TOKEN_EDIT));
                return TRUE;
            }
            SetDlgItemTextW(dlg, IDC_TOKEN_STATUS, L"Checking the token with GitHub\u2026");
            UpdateWindow(dlg);
            HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
            HttpResponse r = HttpGet(L"https://api.github.com/user", token);
            SetCursor(old);
            std::wstring login;
            if (r.status == 200 && ParseGitHubLogin(r.body, login)) {
                d->token = token;
                d->login = login;
                SecureZeroMemory(token.data(), token.size() * sizeof(wchar_t));
                EndDialog(dlg, IDOK);
                return TRUE;
            }
            SecureZeroMemory(token.data(), token.size() * sizeof(wchar_t));
            std::wstring problem =
                r.status == 401 ? L"GitHub didn't accept this token. Make sure you copied all of it (it starts with "
                                  L"\u201cgithub_pat_\u201d) and that it hasn't expired."
                : r.status == 0 ? L"Couldn't reach GitHub \u2014 check your internet connection. (" + r.error + L")"
                                : L"GitHub answered with an error (HTTP " + std::to_wstring(r.status) + L"). Try again in a minute.";
            SetDlgItemTextW(dlg, IDC_TOKEN_STATUS, problem.c_str());
            MessageBeep(MB_ICONWARNING);
            return TRUE;
        }
        case IDCANCEL: EndDialog(dlg, IDCANCEL); return TRUE;
        }
        break;
    }
    return FALSE;
}

void OnToken() {
    if (g.busy) return;
    TokenDialogData d;
    if (!GitHubRepoFromUrl(GetText(g.remote), d.owner, d.name)) GitHubRepoFromUrl(kDefaultRemoteUrl, d.owner, d.name);
    if (DialogBoxParamW(g.inst, MAKEINTRESOURCEW(IDD_TOKEN), g.hwnd, TokenDlgProc, (LPARAM)&d) != IDOK) return;
    const std::wstring login = d.login;
    auto token = std::make_shared<std::wstring>(std::move(d.token));
    RunGitTask(GitTask::SaveToken, L"Saving your GitHub sign-in", [login, token](const LogFn& log) {
        GitResult r = SaveSignIn(kGitHubHost, login, *token, log);
        SecureZeroMemory(token->data(), token->size() * sizeof(wchar_t));
        return r;
    });
}

void OnSignIn() {
    if (g.signingIn) {
        AppendLog(L"Cancelling the sign-in\u2026");
        CancelSignIn();
        return;
    }
    if (g.busy) return;
    if (g.accountChecked && g.account.gcmVersion.empty()) {
        // No Git Credential Manager (browser sign-in): use an access token instead.
        OnToken();
        return;
    }
    // Always confirm (Cancel is the default), so the browser sign-in only starts on purpose.
    if (Message(L"Sign in to GitHub?\n\nYour web browser will open GitHub. Sign in there (if asked) and click "
                L"\u201cAuthorize\u201d, then come back here. The sign-in is kept safely in Windows Credential Manager.",
                MB_ICONQUESTION | MB_OKCANCEL | MB_DEFBUTTON2) != IDOK)
        return;
    g.signingIn = true;
    RunGitTask(GitTask::SignIn, L"Signing in to GitHub", [](const LogFn& log) { return SignInWithBrowser(log); });
    SetPushResult(L"Your web browser opens GitHub: sign in there and click \u201cAuthorize\u201d.", kAccent);
}

void OnSignOut() {
    if (g.busy) return;
    const std::wstring user = g.account.username;
    if (Message(L"Sign out of GitHub" + (user.empty() ? std::wstring() : L" (" + user + L")") +
                    L" on this computer?\n\nThe saved sign-in is removed from Windows Credential Manager. You'll need "
                    L"to sign in again before you can push changes.",
                MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) != IDYES)
        return;
    RunGitTask(GitTask::SignOut, L"Signing out of GitHub",
               [user](const LogFn& log) { return SignOut(kGitHubHost, user, log); });
}

void OnAccountChecked(GitHubAccount* a) {
    const bool first = !g.accountChecked;
    g.account = *a;
    delete a;
    g.accountChecking = false;
    g.accountChecked = true;
    if (g.account.signedIn && !g.account.username.empty()) SaveSetting(L"GitHubUser", g.account.username);
    if (first)
        AppendLog(g.account.gcmVersion.empty() ? L"Git Credential Manager not found: sign in with an access token."
                                               : L"Git Credential Manager " + g.account.gcmVersion.substr(0, g.account.gcmVersion.find(L'+')) +
                                                     L" found (browser sign-in available).");
    AppendLog(g.account.signedIn ? L"GitHub: signed in as " + g.account.username
                                 : L"GitHub: not signed in" + (g.account.note.empty() ? L"" : L" \u2014 " + g.account.note));
    UpdateAccountText();
    UpdateButtons();
    if (g.startupSync) {
        g.startupSync = false;
        if (g.account.signedIn && g.connected && !g.busy) OnSync(MergeChoice::Ask, true);
    }
}

// Asks how to combine when GitHub and this folder both changed. False = cancelled.
bool AskMergeChoice(GitTask task, const GitResult& res, MergeChoice& choice) {
    std::wstring list;
    for (size_t i = 0; i < res.files.size(); ++i) {
        if (i == 8) {
            list += L"\n   \u2026 and " + std::to_wstring(res.files.size() - 8) + L" more";
            break;
        }
        list += L"\n   \u2022 " + res.files[i];
    }
    std::wstring instruction, content;
    std::vector<TASKDIALOG_BUTTON> buttons;
    if (res.outcome == GitOutcome::Unrelated && task == GitTask::Push) {
        instruction = L"Combine with GitHub and upload?";
        content = res.message + L"\n\nFiles that are only on GitHub are kept (and copied into this folder). Where a file "
                                L"exists in both places, this computer's version is kept. Then everything in this "
                                L"folder is uploaded." +
                  (list.empty() ? L"" : L"\n\nOnly on GitHub:" + list);
        buttons.push_back({1001, L"Combine and upload\nKeep my local site files"});
    } else if (res.outcome == GitOutcome::Unrelated) {
        instruction = L"GitHub has a different copy of the website";
        content = res.message + L"\n\nFiles that are only on GitHub are copied here either way. Which version should "
                                L"be kept where a file exists in both places?" +
                  (list.empty() ? L"" : L"\n\nOnly on GitHub:" + list);
        buttons.push_back({1001, L"Keep this computer's files\nGitHub's extra files are added; nothing here is replaced"});
        buttons.push_back({1002, L"Use GitHub's files\nFiles that differ are replaced with the copies from GitHub"});
    } else {
        instruction = L"Some files were changed both on GitHub and on this computer";
        content = L"Choose which version to keep for these files. All other changes from both sides are kept." + list;
        buttons.push_back({1001, L"Keep this computer's version\nYour edits here win for these files"});
        buttons.push_back({1002, L"Use GitHub's version\nYour edits to these files are replaced by GitHub's"});
    }
    TASKDIALOGCONFIG tc = {};
    tc.cbSize = sizeof(tc);
    tc.hwndParent = g.hwnd;
    tc.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    tc.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    tc.pszWindowTitle = kAppName;
    tc.pszMainIcon = TD_INFORMATION_ICON;
    tc.pszMainInstruction = instruction.c_str();
    tc.pszContent = content.c_str();
    tc.cButtons = (UINT)buttons.size();
    tc.pButtons = buttons.data();
    tc.nDefaultButton = 1001;
    int button = 0;
    if (FAILED(TaskDialogIndirect(&tc, &button, nullptr, nullptr))) return false;
    if (button == 1001) choice = MergeChoice::KeepLocal;
    else if (button == 1002) choice = MergeChoice::UseGitHub;
    else return false;
    return true;
}

void OnGitDone(GitTaskResult* r) {
    const GitTask task = r->task;
    const GitResult res = r->result;
    const bool automatic = r->automatic;
    delete r;
    g.busy = false;
    g.signingIn = false;
    if (!res.message.empty()) AppendLog(res.message);
    AppendLog(L"");

    if (task == GitTask::SignIn || task == GitTask::SaveToken || task == GitTask::SignOut) {
        if (res.outcome == GitOutcome::Done) {
            SaveSetting(L"GitHubUser", task == GitTask::SignOut ? std::wstring() : res.account);
            SetPushResult(L"\u2714 " + res.message, kOpen);
            g.startupSync = task != GitTask::SignOut; // download from GitHub once the sign-in is confirmed
        } else if (res.outcome == GitOutcome::Cancelled) {
            SetPushResult(res.message, kMuted);
        } else {
            SetPushResult(L"\u2716 " + res.message, kClosed);
            Message(res.message, MB_ICONWARNING);
        }
        g.accountChecked = false;
        UpdateGitState();
        StartAccountCheck();
        return;
    }

    const std::wstring what = task == GitTask::Connect ? L"Connect" : task == GitTask::Push ? L"Push" : L"Sync";
    switch (res.outcome) {
    case GitOutcome::Done:
        if (task == GitTask::Connect) {
            UpdateGitState();
            SetPushResult(L"\u2714 " + res.message, kOpen);
            SetStatus(L"Connected at " + NowText() + L".");
            OnSync(MergeChoice::Ask, false); // show what's on GitHub right away
            return;
        }
        g.changes.clear(); // committed (and, for Push, uploaded)
        LoadRepo(g.repo, !automatic); // GitHub may have changed the site data and images
        SetPushResult(L"\u2714 " + res.message, kOpen);
        SetStatus(what + L" finished at " + NowText() + L".");
        if (task == GitTask::Push && !res.version.empty()) StartLiveCheck(res.version);
        break;
    case GitOutcome::NothingToDo:
        if (task == GitTask::Push) g.changes.clear();
        UpdateGitState();
        SetPushResult(L"\u2714 " + res.message, kOpen);
        SetStatus(res.message);
        break;
    case GitOutcome::NoSiteOnGitHub:
        UpdateGitState();
        SetPushResult(res.message, kAccent);
        SetStatus(res.message);
        break;
    case GitOutcome::NoGit:
        UpdateGitState();
        SetPushResult(L"\u2716 Git isn't installed.", kClosed);
        if (!automatic) Message(kNoGitHelp, MB_ICONINFORMATION);
        break;
    case GitOutcome::NotRepo:
    case GitOutcome::NoRemote:
        UpdateGitState();
        SetPushResult(L"\u2716 Not connected to GitHub yet \u2014 click \u201cConnect folder\u2026\u201d (step 2).", kClosed);
        if (!automatic) Message(res.message + L"\n\nClick \u201cConnect folder\u2026\u201d (step 2) first.", MB_ICONINFORMATION);
        break;
    case GitOutcome::Unrelated:
    case GitOutcome::Conflict: {
        UpdateGitState();
        if (automatic) {
            SetPushResult(L"GitHub and this folder both have changes \u2014 click \u201cSync from GitHub\u201d to choose what to keep.",
                          kAccent);
            break;
        }
        MergeChoice choice;
        if (!AskMergeChoice(task, res, choice)) {
            SetPushResult(L"Cancelled \u2014 nothing was changed.", kMuted);
            break;
        }
        if (task == GitTask::Push) OnPush(choice);
        else OnSync(choice, false);
        break;
    }
    case GitOutcome::AuthFailed:
        UpdateGitState();
        SetPushResult(L"\u2716 GitHub didn't accept the sign-in \u2014 click \u201cSign in to GitHub\u201d (step 1).", kClosed);
        if (!automatic) Message(res.message, MB_ICONWARNING);
        g.accountChecked = false;
        StartAccountCheck();
        break;
    case GitOutcome::NoAccess:
        UpdateGitState();
        SetPushResult(L"\u2716 This GitHub account isn't allowed to change the repository.", kClosed);
        if (!automatic) Message(res.message, MB_ICONWARNING);
        break;
    case GitOutcome::NotFound:
        UpdateGitState();
        SetPushResult(L"\u2716 GitHub couldn't find the repository \u2014 check the address (step 2).", kClosed);
        if (!automatic) Message(res.message, MB_ICONWARNING);
        break;
    case GitOutcome::Offline:
        UpdateGitState();
        SetPushResult(L"\u2716 Can't reach GitHub \u2014 check your internet connection.", kClosed);
        if (!automatic) Message(res.message, MB_ICONWARNING);
        break;
    case GitOutcome::Cancelled:
        UpdateGitState();
        SetPushResult(res.message, kMuted);
        break;
    case GitOutcome::Failed:
        UpdateGitState();
        SetPushResult(L"\u2716 " + what + L" didn't work \u2014 see the details below.", kClosed);
        SetStatus(what + L" failed.");
        if (!automatic) Message(res.message, MB_ICONERROR);
        break;
    }
    UpdateAccountText();
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

// "1 Sign in → 2 Connect folder → 3 Push changes" with a check mark on each finished step.
void DrawSteps(const DRAWITEMSTRUCT* di) {
    HDC dc = di->hDC;
    RECT rc = di->rcItem;
    FillRect(dc, &rc, GetSysColorBrush(COLOR_WINDOW));
    const bool signedIn = g.account.signedIn;
    const bool published = g.connected && g.pub.remoteKnown && g.pub.remoteHasSite && !g.pub.Unpublished();
    struct Step {
        std::wstring label;
        bool done;
    } steps[] = {
        {signedIn ? L"Signed in as " + g.account.username : std::wstring(L"Sign in to GitHub"), signedIn},
        {g.connected ? std::wstring(L"Folder connected") : std::wstring(L"Connect folder"), g.connected},
        {published ? std::wstring(L"Published \u2014 GitHub is up to date") : std::wstring(L"Push changes"), published},
    };
    int current = 3;
    for (int i = 2; i >= 0; --i)
        if (!steps[i].done) current = i;

    SetBkMode(dc, TRANSPARENT);
    const int d = S(22), cy = (rc.top + rc.bottom) / 2;
    int x = rc.left;
    for (int i = 0; i < 3; ++i) {
        const COLORREF fill = steps[i].done ? kOpen : i == current ? kAccent : RGB(176, 176, 184);
        HBRUSH br = CreateSolidBrush(fill);
        HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, x, cy - d / 2, x + d + 1, cy + d / 2 + 1);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(br);
        HGDIOBJ of = SelectObject(dc, g.fontBold);
        SetTextColor(dc, RGB(255, 255, 255));
        std::wstring mark = steps[i].done ? L"\u2714" : std::to_wstring(i + 1);
        RECT cr = {x, cy - d / 2, x + d, cy + d / 2};
        DrawTextW(dc, mark.c_str(), -1, &cr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        x += d + S(6);

        SelectObject(dc, i == current ? g.fontBold : g.font);
        SetTextColor(dc, steps[i].done ? kOpen : i == current ? kAccent : kMuted);
        SIZE ts{};
        GetTextExtentPoint32W(dc, steps[i].label.c_str(), (int)steps[i].label.size(), &ts);
        RECT tr = {x, rc.top, std::min<LONG>(x + ts.cx, rc.right), rc.bottom};
        DrawTextW(dc, steps[i].label.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        x += ts.cx + S(12);
        if (i < 2) {
            SelectObject(dc, g.font);
            SetTextColor(dc, kMuted);
            RECT ar = {x, rc.top, x + S(24), rc.bottom};
            DrawTextW(dc, L"\u2192", -1, &ar, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            x += S(24) + S(4);
        }
        SelectObject(dc, of);
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
    const wchar_t* cols[] = {L"#", L"Name", L"Image file", L"On GitHub"};
    for (int i = 0; i < 4; ++i) {
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

    g.headPublish = Ctl(L"STATIC", L"Publish to the website (GitHub)", SS_LEFT, IDC_HEAD_PUBLISH);
    g.steps = Ctl(L"STATIC", L"", SS_OWNERDRAW, IDC_STEPS);
    g.accountLabel = Ctl(L"STATIC", L"GitHub account:", SS_LEFT, IDC_ACCOUNT_LABEL);
    g.accountText = Ctl(L"STATIC", L"Checking\u2026", SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX, IDC_ACCOUNT);
    g.signIn = Ctl(L"BUTTON", L"Sign in to GitHub\u2026", WS_TABSTOP | BS_PUSHBUTTON, IDC_SIGNIN);
    g.token = Ctl(L"BUTTON", L"Use access &token\u2026", WS_TABSTOP | BS_PUSHBUTTON, IDC_TOKEN);
    g.signOut = Ctl(L"BUTTON", L"Sign &out", WS_TABSTOP | BS_PUSHBUTTON, IDC_SIGNOUT);
    g.remoteLabel = Ctl(L"STATIC", L"GitHub repository:", SS_LEFT, IDC_REMOTE_LABEL);
    g.remote = Ctl(L"EDIT", LoadSetting(L"RemoteUrl", kDefaultRemoteUrl).c_str(), WS_TABSTOP | ES_AUTOHSCROLL,
                   IDC_REMOTE, WS_EX_CLIENTEDGE);
    g.connect = Ctl(L"BUTTON", L"&Connect folder\u2026", WS_TABSTOP | BS_PUSHBUTTON, IDC_CONNECT);
    g.push = Ctl(L"BUTTON", L"&Push changes", WS_TABSTOP | BS_PUSHBUTTON, IDC_PUSH);
    g.sync = Ctl(L"BUTTON", L"S&ync from GitHub", WS_TABSTOP | BS_PUSHBUTTON, IDC_SYNC);
    g.openSite = Ctl(L"BUTTON", L"Open &website", WS_TABSTOP | BS_PUSHBUTTON, IDC_OPEN_SITE);
    g.pushResult = Ctl(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX | SS_NOTIFY, IDC_PUSH_RESULT);
    g.log = Ctl(L"EDIT", L"", WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, IDC_LOG,
                WS_EX_CLIENTEDGE);
    SendMessageW(g.log, EM_SETLIMITTEXT, 0x7FFFFFFE, 0);
    g.status = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, g.hwnd,
                               (HMENU)(INT_PTR)IDC_STATUSBAR, g.inst, nullptr);

    g.tips = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT,
                             CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, g.hwnd, nullptr, g.inst, nullptr);
    SendMessageW(g.tips, TTM_SETMAXTIPWIDTH, 0, S(380));
    SendMessageW(g.tips, TTM_SETDELAYTIME, TTDT_AUTOPOP, 20000);
    for (HWND h : TipControls()) {
        TOOLINFOW ti = {sizeof(ti)};
        ti.hwnd = g.hwnd;
        ti.lpszText = const_cast<wchar_t*>(L"");
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS; // while enabled: the button itself
        ti.uId = (UINT_PTR)h;
        SendMessageW(g.tips, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        ti.uFlags = TTF_SUBCLASS; // while disabled: its rectangle on the main window (rect set in Layout)
        ti.uId = kDisabledTipOffset + GetDlgCtrlID(h);
        SendMessageW(g.tips, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    }
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
        StartAccountCheck(); // then syncs from GitHub if signed in and connected
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
        mmi->ptMinTrackSize = {S(760), S(700)};
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
        else if (ctl == g.accountText) SetTextColor(dc, g.accountColor);
        else SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }

    case WM_DRAWITEM: {
        auto* di = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (di->CtlID == IDC_TOGGLE) {
            DrawToggle(di);
            return TRUE;
        }
        if (di->CtlID == IDC_STEPS) {
            DrawSteps(di);
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
        case IDC_SIGNIN: OnSignIn(); return 0;
        case IDC_TOKEN: OnToken(); return 0;
        case IDC_SIGNOUT: OnSignOut(); return 0;
        case IDC_CONNECT: OnConnect(); return 0;
        case IDC_PUSH: OnPush(MergeChoice::Ask); return 0;
        case IDC_SYNC: OnSync(MergeChoice::Ask, false); return 0;
        case IDC_OPEN_SITE: OnOpenWebsite(); return 0;
        case IDC_PUSH_RESULT:
            if (HIWORD(wp) == STN_CLICKED && !g.pushResultLink.empty()) OpenUrl(g.pushResultLink, g.pushResultLink == g.liveSite);
            return 0;
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

    case WM_APP_ACCOUNT:
        OnAccountChecked(reinterpret_cast<GitHubAccount*>(lp));
        return 0;

    case WM_APP_LIVE:
        OnLiveMsg(reinterpret_cast<LiveMsg*>(lp));
        return 0;

    case WM_SETCURSOR:
        if ((HWND)wp == g.pushResult && !g.pushResultLink.empty()) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;

    case WM_CLOSE:
        if (g.busy && Message(L"Git is still working. Quit anyway?", MB_ICONWARNING | MB_YESNO) != IDYES)
            return 0;
        if (g.signingIn) CancelSignIn();
        CancelLiveCheck();
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
    int w = std::min(S(980), (int)(wa.right - wa.left)), h = std::min(S(880), (int)(wa.bottom - wa.top));
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
