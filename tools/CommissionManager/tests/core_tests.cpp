// Console tests for the non-UI core. Never point these at the real website folder.
//   core_tests json
//   core_tests site <repo-copy> <image-dir>    (image-dir: big.png, alpha.png, photo.jpg)
//   core_tests git  <sandbox-dir>               (creates its own repos + local bare "GitHub")
#include "../src/core.h"

#include <objbase.h>
#include <wincodec.h>

#include <cstdio>
#include <cstdlib>

static int g_failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (cond) std::printf("  ok   %s\n", #cond);                           \
        else { std::printf("  FAIL %s  (line %d)\n", #cond, __LINE__); ++g_failures; } \
    } while (0)

static void Log(const std::wstring& line) { std::printf("    | %s\n", ToUtf8(line).c_str()); }

static bool ParseAll(const std::string& s, json::Value& v) {
    size_t pos = 0;
    std::string err;
    if (!json::Parse(s, pos, v, err)) return false;
    while (pos < s.size() && isspace((unsigned char)s[pos])) ++pos;
    return pos == s.size();
}

static void TestJson() {
    std::printf("[json]\n");
    json::Value v;
    CHECK(ParseAll(R"({"a":"q\"b\\s\/\b\f\n\r\t","u":"\u00e9\ud83d\ude00","n":[0,-1.5e+3,12],"t":true,"f":false,"z":null,"o":{}})", v));
    CHECK(v.Get("a") && v.Get("a")->text == "q\"b\\s/\b\f\n\r\t");
    CHECK(v.Get("u") && v.Get("u")->text == "\xC3\xA9\xF0\x9F\x98\x80");
    CHECK(v.Get("n") && v.Get("n")->items.size() == 3 && v.Get("n")->items[1].text == "-1.5e+3");
    CHECK(v.Get("t")->boolean && !v.Get("f")->boolean && v.Get("z")->type == json::Value::Type::Null);
    json::Value back;
    CHECK(ParseAll(json::Serialize(v), back) && json::Serialize(back) == json::Serialize(v));
    for (const char* bad : {"[1,]", "{\"a\" 1}", "01", "\"\x01\"", "[1 2]", "{\"a\":}", "tru", "\"\\x\"", "-", "1.", "[", "\"\\u12G4\""})
        CHECK(!ParseAll(bad, v));
    json::Value ctl = json::Value::String("a\x01" "b\xE2\x80\xA8");
    CHECK(json::Serialize(ctl) == "\"a\\u0001b\\u2028\"");
}

static bool ImageSize(const std::wstring& path, UINT& w, UINT& h) {
    HBITMAP b = LoadThumbnail(path, 100000, 100000, w, h);
    if (b) DeleteObject(b);
    return b != nullptr;
}

static void TestSite(const std::wstring& repo, const std::wstring& images) {
    std::printf("[site] %s\n", ToUtf8(repo).c_str());
    SiteData d;
    std::wstring err;
    CHECK(d.Load(repo, err));
    if (!err.empty()) Log(err);
    const size_t n0 = d.Count();
    const bool open0 = d.CommissionsOpen();

    d.SetCommissionsOpen(!open0);
    std::wstring rel1, rel2, rel3, rel4;
    const std::wstring tricky = L"Fox \"Quoted\" Name \u00e9";
    CHECK(ImportImage(repo, images + L"\\big.png", tricky, d.Files(), rel1, err));
    CHECK(rel1 == L"assets/gallery/fox-quoted-name.jpg");
    d.Add(tricky, rel1);
    CHECK(ImportImage(repo, images + L"\\big.png", tricky, d.Files(), rel2, err));
    CHECK(rel2 == L"assets/gallery/fox-quoted-name-2.jpg");
    d.Add(L"Second copy", rel2);
    CHECK(ImportImage(repo, images + L"\\alpha.png", L"Sticker", d.Files(), rel3, err));
    CHECK(rel3 == L"assets/gallery/sticker.png");
    d.Add(L"Sticker", rel3);
    CHECK(ImportImage(repo, images + L"\\photo.jpg", L"Small photo", d.Files(), rel4, err));
    d.Add(L"Small photo", rel4);
    UINT w = 0, h = 0;
    CHECK(ImageSize(RepoPath(repo, rel1), w, h) && w == 1920 && h == 1280);
    CHECK(ImageSize(RepoPath(repo, rel3), w, h) && w == 300 && h == 200);
    CHECK(ImageSize(RepoPath(repo, rel4), w, h) && w == 640 && h == 480);
    CHECK(FileExists(images + L"\\big.png"));
    CHECK(!ImportImage(repo, images + L"\\missing.png", L"x", d.Files(), rel4, err));

    d.Rename(n0 + 1, L"Renamed \"two\"");
    d.Move(n0, 0);                 // new tricky item becomes featured
    d.Move(1, 2);
    d.Remove(d.Count() - 1);       // drop "Small photo"
    CHECK(d.Save(err));
    CHECK(!FileExists(d.DataPath() + L".tmp"));

    SiteData r;
    CHECK(r.Load(repo, err));
    CHECK(r.ToText() == d.ToText());
    CHECK(r.CommissionsOpen() == !open0);
    CHECK(r.Count() == n0 + 3);
    CHECK(r.Item(0).name == tricky && r.Item(0).file == rel1);
    CHECK(r.Item(r.Count() - 1).name == L"Sticker");
    bool renamed = false;
    for (size_t i = 0; i < r.Count(); ++i) renamed |= r.Item(i).name == L"Renamed \"two\"";
    CHECK(renamed);
    std::printf("--- data/site-data.js ---\n%s--- end ---\n", r.ToText().c_str());
}

// ---------------------------------------------------------------------------
static int Sh(const std::wstring& dir, const std::wstring& cmd) {
    std::wstring full = L"cmd /c \"cd /d \"" + dir + L"\" && " + cmd + L" >nul 2>nul\"";
    return _wsystem(full.c_str());
}

static void WriteText(const std::wstring& path, const std::string& text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD n;
    WriteFile(h, text.data(), (DWORD)text.size(), &n, nullptr);
    CloseHandle(h);
}

static const char kFixture[] =
    "window.SITE_DATA = {\n  \"commissionsOpen\": true,\n  \"gallery\": [\n"
    "    { \"name\": \"Ember\", \"file\": \"assets/gallery/ember.webp\" }\n  ]\n};\n";

static void MakeSite(const std::wstring& dir) {
    CreateDirectoryW(dir.c_str(), nullptr);
    CreateDirectoryW((dir + L"\\data").c_str(), nullptr);
    CreateDirectoryW((dir + L"\\assets").c_str(), nullptr);
    CreateDirectoryW((dir + L"\\assets\\gallery").c_str(), nullptr);
    WriteText(dir + L"\\index.html", "<!doctype html><title>site</title>\n");
    WriteText(dir + L"\\LICENSE", "local license text\n");
    WriteText(dir + L"\\data\\site-data.js", kFixture);
    WriteText(dir + L"\\assets\\gallery\\ember.webp", "fake");
}

static const char* Name(GitOutcome o) {
    switch (o) {
    case GitOutcome::Done: return "Done";
    case GitOutcome::NothingToDo: return "NothingToDo";
    case GitOutcome::NoGit: return "NoGit";
    case GitOutcome::NotRepo: return "NotRepo";
    case GitOutcome::NoRemote: return "NoRemote";
    case GitOutcome::Rejected: return "Rejected";
    case GitOutcome::Unrelated: return "Unrelated";
    case GitOutcome::Conflict: return "Conflict";
    case GitOutcome::Failed: return "Failed";
    }
    return "?";
}

static GitOutcome Step(const char* what, GitResult r) {
    std::printf("  -> %s: %s  (%s)\n", what, Name(r.outcome), ToUtf8(r.message).c_str());
    return r.outcome;
}

static void TestGit(const std::wstring& sandbox) {
    std::printf("[git] %s\n", ToUtf8(sandbox).c_str());
    const std::wstring bare = sandbox + L"\\github.git", seed = sandbox + L"\\seed", site = sandbox + L"\\site",
                       other = sandbox + L"\\other";
    const std::wstring url = bare;
    CreateDirectoryW(sandbox.c_str(), nullptr);

    // "GitHub": a repository created online with only a LICENSE commit.
    CHECK(Sh(sandbox, L"git init -q --bare -b main github.git") == 0);
    CHECK(Sh(sandbox, L"git clone -q github.git seed") == 0);
    WriteText(seed + L"\\LICENSE", "MIT License from GitHub\n");
    CHECK(Sh(seed, L"git add LICENSE && git commit -qm \"Initial commit\" && git push -q origin main") == 0);

    // The local website folder: not a git repository yet.
    MakeSite(site);
    RepoInfo info = InspectRepo(site);
    CHECK(info.gitAvailable && !info.isRepo);
    CHECK(Step("push before connect", PushChanges(site, L"x", Log)) == GitOutcome::NotRepo);
    CHECK(Step("pull before connect", PullChanges(site, false, Log)) == GitOutcome::NotRepo);

    CHECK(Step("connect", ConnectRepo(site, url, false, Log)) == GitOutcome::Done);
    info = InspectRepo(site);
    CHECK(info.isRepo && info.branch == L"main" && SameRemoteUrl(info.originUrl, url) && !info.hasCommits);

    CHECK(Step("first push (GitHub has LICENSE)", PushChanges(site, L"first", Log)) == GitOutcome::Rejected);
    CHECK(Step("pull without consent", PullChanges(site, false, Log)) == GitOutcome::Unrelated);
    CHECK(Step("pull with consent", PullChanges(site, true, Log)) == GitOutcome::Done);
    CHECK(Step("push after pull", PushChanges(site, L"first", Log)) == GitOutcome::Done);
    CHECK(Sh(sandbox, L"git --git-dir=github.git cat-file -e main:data/site-data.js") == 0);
    CHECK(Sh(sandbox, L"git --git-dir=github.git cat-file -e main:index.html") == 0);
    CHECK(Sh(sandbox, L"git --git-dir=github.git show main:LICENSE | findstr /c:\"local license\"") == 0);
    CHECK(Step("push with nothing new", PushChanges(site, L"", Log)) == GitOutcome::NothingToDo);

    // Normal edit → commit with summary → push.
    SiteData d;
    std::wstring err;
    CHECK(d.Load(site, err));
    d.SetCommissionsOpen(false);
    CHECK(d.Save(err));
    CHECK(Step("push edit", PushChanges(site, L"commissions closed", Log)) == GitOutcome::Done);
    CHECK(Sh(sandbox, L"git --git-dir=github.git log -1 --format=%s main | findstr /c:\"Update site: commissions closed\"") == 0);

    // Push publishes every change in the folder (index.html, new song.mp3, deletions) but honors
    // .gitignore, e.g. the app's own build outputs.
    WriteText(site + L"\\index.html", "<!doctype html><title>edited</title>\n");
    WriteText(site + L"\\song.mp3", "fake mp3");
    DeleteFileW((site + L"\\LICENSE").c_str());
    CreateDirectoryW((site + L"\\tools").c_str(), nullptr);
    CreateDirectoryW((site + L"\\tools\\CommissionManager").c_str(), nullptr);
    CreateDirectoryW((site + L"\\tools\\CommissionManager\\build").c_str(), nullptr);
    WriteText(site + L"\\tools\\CommissionManager\\.gitignore", "/build/\n/CommissionManager.exe\n");
    WriteText(site + L"\\tools\\CommissionManager\\CommissionManager.exe", "exe");
    WriteText(site + L"\\tools\\CommissionManager\\build\\main.obj", "obj");
    WriteText(site + L"\\tools\\CommissionManager\\README.md", "tool readme\n");
    CHECK(Step("push everything", PushChanges(site, L"edited page", Log)) == GitOutcome::Done);
    CHECK(Sh(sandbox, L"git --git-dir=github.git show main:index.html | findstr /c:\"edited\"") == 0);
    CHECK(Sh(sandbox, L"git --git-dir=github.git cat-file -e main:song.mp3") == 0);
    CHECK(Sh(sandbox, L"git --git-dir=github.git cat-file -e main:tools/CommissionManager/README.md") == 0);
    CHECK(Sh(sandbox, L"git --git-dir=github.git cat-file -e main:LICENSE") != 0);
    CHECK(Sh(sandbox, L"git --git-dir=github.git cat-file -e main:tools/CommissionManager/CommissionManager.exe") != 0);
    CHECK(Sh(sandbox, L"git --git-dir=github.git cat-file -e main:tools/CommissionManager/build/main.obj") != 0);
    CHECK(Sh(sandbox, L"git --git-dir=github.git log -1 --format=%s main | findstr /c:\"Update site: edited page\"") == 0);

    // Someone else pushes; our push is rejected; pull (related history) then push works.
    CHECK(Sh(sandbox, L"git clone -q github.git other") == 0);
    WriteText(other + L"\\README.md", "readme\n");
    CHECK(Sh(other, L"git add README.md && git commit -qm readme && git push -q origin main") == 0);
    d.SetCommissionsOpen(true);
    CHECK(d.Save(err));
    CHECK(Step("push while behind", PushChanges(site, L"commissions open", Log)) == GitOutcome::Rejected);
    CHECK(Step("pull", PullChanges(site, false, Log)) == GitOutcome::Done);
    CHECK(FileExists(site + L"\\README.md"));
    CHECK(Step("push after pull", PushChanges(site, L"commissions open", Log)) == GitOutcome::Done);

    // Conflicting edit to the same line: pull reports Conflict and leaves the repo clean.
    CHECK(Sh(other, L"git pull -q origin main") == 0);
    WriteText(other + L"\\data\\site-data.js", std::string(kFixture).replace(std::string(kFixture).find("Ember"), 5, "Other"));
    CHECK(Sh(other, L"git commit -qam other && git push -q origin main") == 0);
    CHECK(d.Load(site, err));
    d.Rename(0, L"Mine");
    CHECK(d.Save(err));
    CHECK(Step("push conflicting", PushChanges(site, L"renamed", Log)) == GitOutcome::Rejected);
    CHECK(Step("pull conflicting", PullChanges(site, false, Log)) == GitOutcome::Conflict);
    CHECK(!DirExists(site + L"\\.git\\rebase-merge") && !DirExists(site + L"\\.git\\rebase-apply"));
    CHECK(d.Load(site, err) && d.Item(0).name == L"Mine");

    // Changing origin requires consent.
    CHECK(Step("connect other url (no consent)", ConnectRepo(site, L"https://example.com/x.git", false, Log)) == GitOutcome::Failed);
    CHECK(Step("connect other url (consent)", ConnectRepo(site, L"https://example.com/x.git", true, Log)) == GitOutcome::Done);
    CHECK(InspectRepo(site).originUrl == L"https://example.com/x.git");

    // A repository without origin.
    const std::wstring noRemote = sandbox + L"\\noremote";
    MakeSite(noRemote);
    CHECK(Sh(noRemote, L"git init -q") == 0);
    CHECK(Step("push without origin", PushChanges(noRemote, L"", Log)) == GitOutcome::NoRemote);
}

int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::wstring mode = argc > 1 ? argv[1] : L"";
    if (mode == L"json") TestJson();
    else if (mode == L"site" && argc > 3) TestSite(argv[2], argv[3]);
    else if (mode == L"git" && argc > 2) TestGit(argv[2]);
    else {
        std::printf("usage: core_tests json | site <repo-copy> <image-dir> | git <sandbox-dir>\n");
        return 2;
    }
    std::printf(g_failures ? "\n%d FAILURE(S)\n" : "\nALL PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
