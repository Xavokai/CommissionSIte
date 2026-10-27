// Console tests for the non-UI core. Never point these at the real website folder.
//   core_tests json
//   core_tests logic                            (pure helpers: error classification, credential parsing…)
//   core_tests version                          (pure helpers: version stamps, website address, conflicts)
//   core_tests site <repo-copy> <image-dir>    (image-dir: big.png, alpha.png, photo.jpg)
//   core_tests git  <sandbox-dir>               (creates its own repos + local bare "GitHub")
//   core_tests cred                             (saves + removes a credential for a fake host)
//   core_tests api                              (read-only, unauthenticated api.github.com checks)
//   core_tests live <base-url> <served-dir>     (<served-dir> is served at <base-url>, e.g. by python -m http.server)
#include "../src/core.h"

#include <objbase.h>
#include <wincodec.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <thread>

static int g_failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (cond) std::printf("  ok   %s\n", #cond);                           \
        else { std::printf("  FAIL %s  (line %d)\n", #cond, __LINE__); ++g_failures; } \
    } while (0)

static void Log(const std::wstring& line) { std::printf("    | %s\n", ToUtf8(line).c_str()); }
static std::string ReadText(const std::wstring& path);

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
    CHECK(d.Version().size() == 16 && d.ToText().rfind("window.SITE_DATA = {\n  \"version\": \"", 0) == 0);
    d.SetCommissionsOpen(open0);
    d.SetCommissionsOpen(!open0); // re-setting keeps "version" first
    CHECK(d.Save(err, L"20261027T214100Z") && d.Version() == L"20261027T214100Z");
    CHECK(ExtractSiteVersion(ReadText(d.DataPath())) == L"20261027T214100Z");

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
static void TestLogic() {
    std::printf("[logic]\n");
    CHECK(ClassifyGitOutput("fatal: unable to access 'https://github.com/x/y.git/': Could not resolve host: github.com") ==
          GitError::Offline);
    CHECK(ClassifyGitOutput("fatal: unable to access 'https://github.com/x/y.git/': Failed to connect to github.com port 443") ==
          GitError::Offline);
    CHECK(ClassifyGitOutput("fatal: Cannot prompt because user interactivity has been disabled.\n"
                            "fatal: could not read Username for 'https://github.com': terminal prompts disabled") ==
          GitError::Auth);
    CHECK(ClassifyGitOutput("remote: Invalid username or token.\nfatal: Authentication failed for 'https://github.com/x/y.git/'") ==
          GitError::Auth);
    CHECK(ClassifyGitOutput("remote: Permission to Xavokai/CommissionSIte.git denied to someone.\n"
                            "fatal: unable to access '...': The requested URL returned error: 403") == GitError::NoAccess);
    CHECK(ClassifyGitOutput("remote: Repository not found.\nfatal: repository 'https://github.com/x/y.git/' not found") ==
          GitError::NotFound);
    CHECK(ClassifyGitOutput("fatal: couldn't find remote ref refs/heads/main") == GitError::NoRemoteBranch);
    CHECK(ClassifyGitOutput(" ! [rejected]        main -> main (fetch first)\nerror: failed to push some refs") ==
          GitError::NonFastForward);
    CHECK(ClassifyGitOutput("Author identity unknown\n*** Please tell me who you are.") == GitError::Identity);
    CHECK(ClassifyGitOutput("error: something else") == GitError::Other);

    std::wstring u, p;
    CHECK(ParseCredentialOutput("protocol=https\r\nhost=github.com\r\nusername=Xavokai\r\npassword=gho_abc=def\r\n", u, p));
    CHECK(u == L"Xavokai" && p == L"gho_abc=def");
    CHECK(!ParseCredentialOutput("fatal: Cannot prompt because user interactivity has been disabled.\n", u, p));
    CHECK(ParseAccountList("Xavokai\nsecond-account\r\n\nwarning: something\n") ==
          (std::vector<std::wstring>{L"Xavokai", L"second-account"}));
    CHECK(ParseAccountList("").empty());
    CHECK(CredentialRequest(L"github.com", L"me", L"tok\nhost=evil") ==
          "protocol=https\nhost=github.com\nusername=me\npassword=tokhost=evil\n\n");
    CHECK(CredentialRequest(L"github.com", L"", L"") == "protocol=https\nhost=github.com\n\n");
    CHECK(CleanToken(L"  github_pat_11AB\r\n CD \"") == L"github_pat_11ABCD");
    std::wstring login;
    CHECK(ParseGitHubLogin(R"({"login":"Xavokai","id":229278996,"type":"User"})", login) && login == L"Xavokai");
    CHECK(!ParseGitHubLogin(R"({"message":"Bad credentials"})", login));
    CHECK(!ParseGitHubLogin("<html>", login));
    std::wstring owner, name;
    CHECK(GitHubRepoFromUrl(L"https://github.com/Xavokai/CommissionSIte.git", owner, name) && owner == L"Xavokai" &&
          name == L"CommissionSIte");
    CHECK(GitHubRepoFromUrl(L"https://Xavokai@github.com/Xavokai/CommissionSIte/", owner, name) && name == L"CommissionSIte");
    CHECK(GitHubRepoFromUrl(L"git@github.com:Xavokai/CommissionSIte.git", owner, name) && owner == L"Xavokai");
    CHECK(!GitHubRepoFromUrl(L"https://example.com/a/b.git", owner, name));
    CHECK(!GitHubRepoFromUrl(L"https://github.com/Xavokai", owner, name));
    PublishState s;
    CHECK(!s.Unpublished());
    s.ahead = 1;
    CHECK(s.Unpublished());
}

// ---------------------------------------------------------------------------
static void TestVersion() {
    std::printf("[version]\n");
    SYSTEMTIME t = {};
    t.wYear = 2026; t.wMonth = 10; t.wDay = 27; t.wHour = 21; t.wMinute = 41; t.wSecond = 0;
    CHECK(FormatVersionStamp(t) == L"20261027T214100Z");
    t.wYear = 2027; t.wMonth = 1; t.wDay = 2; t.wHour = 3; t.wMinute = 4; t.wSecond = 5;
    CHECK(FormatVersionStamp(t) == L"20270102T030405Z");
    const std::wstring now = NewVersionStamp();
    CHECK(now.size() == 16 && now[8] == L'T' && now[15] == L'Z' && now > L"20260101T000000Z");

    CHECK(GitHubPagesUrl(L"https://github.com/Xavokai/CommissionSIte.git") == L"https://xavokai.github.io/CommissionSIte/");
    CHECK(GitHubPagesUrl(L"git@github.com:Xavokai/CommissionSIte.git") == L"https://xavokai.github.io/CommissionSIte/");
    CHECK(GitHubPagesUrl(L"https://Xavokai@github.com/Xavokai/CommissionSIte/") == L"https://xavokai.github.io/CommissionSIte/");
    CHECK(GitHubPagesUrl(L"https://github.com/Xavokai/xavokai.github.io") == L"https://xavokai.github.io/");
    CHECK(GitHubPagesUrl(L"C:\\temp\\github.git").empty());
    CHECK(GitHubPagesUrl(L"https://example.com/a/b.git").empty());
    CHECK(ActionsUrlFor(L"https://github.com/Someone/Site.git") == L"https://github.com/Someone/Site/actions");
    CHECK(ActionsUrlFor(L"C:\\temp\\github.git") == kDefaultActionsUrl);
    SetEnvironmentVariableW(L"COMMISSION_MANAGER_PAGES_URL", nullptr);
    CHECK(SiteUrlFor(L"https://github.com/Someone/Site.git") == L"https://someone.github.io/Site/");
    CHECK(SiteUrlFor(L"C:\\temp\\github.git") == kDefaultPagesUrl);
    SetEnvironmentVariableW(L"COMMISSION_MANAGER_PAGES_URL", L"http://127.0.0.1:8765");
    CHECK(SiteUrlFor(L"https://github.com/Someone/Site.git") == L"http://127.0.0.1:8765/");
    SetEnvironmentVariableW(L"COMMISSION_MANAGER_PAGES_URL", nullptr);

    CHECK(WithCacheBuster(L"https://xavokai.github.io/CommissionSIte/", 1761601260000ULL) ==
          L"https://xavokai.github.io/CommissionSIte/?t=1761601260000");
    CHECK(WithCacheBuster(L"https://x.github.io/s/data/site-data.js?a=1", 5) == L"https://x.github.io/s/data/site-data.js?a=1&t=5");
    CHECK(WithCacheBuster(L"https://x.github.io/s/#gallery", 7) == L"https://x.github.io/s/?t=7#gallery");
    const unsigned long long ms = UnixTimeMs();
    CHECK(ms > 1761600000000ULL && ms < 4102444800000ULL);

    const std::string stamped = "window.SITE_DATA = {\n  \"version\": \"20261027T214100Z\",\n  \"commissionsOpen\": true,\n"
                                "  \"gallery\": []\n};\n";
    CHECK(ExtractSiteVersion(stamped) == L"20261027T214100Z");
    CHECK(ExtractSiteVersion("\xEF\xBB\xBF// data\r\nwindow.SITE_DATA = {\"gallery\":[],\"version\":\"v2\"};") == L"v2");
    CHECK(ExtractSiteVersion("window.SITE_DATA = { commissionsOpen: true, \"version\": \"20261027T214100Z\" };") ==
          L"20261027T214100Z"); // not strict JSON: found directly
    CHECK(ExtractSiteVersion("window.SITE_DATA = {\n  \"commissionsOpen\": true\n};\n").empty());
    CHECK(ExtractSiteVersion("<!doctype html><h1>404</h1>").empty());

    const std::string other = "window.SITE_DATA = {\n  \"version\": \"20261028T000000Z\",\n  \"commissionsOpen\": true,\n"
                              "  \"gallery\": []\n};\r\n";
    const std::string unstamped = "window.SITE_DATA = {\n  \"commissionsOpen\": true,\n  \"gallery\": []\n};\n";
    CHECK(SameExceptVersion(stamped, other));
    CHECK(SameExceptVersion(stamped, unstamped));
    CHECK(!SameExceptVersion(stamped, "window.SITE_DATA = {\n  \"version\": \"20261027T214100Z\",\n  \"commissionsOpen\": false,\n"
                                      "  \"gallery\": []\n};\n"));
    CHECK(!SameExceptVersion(stamped, "not site data"));

    // Merge conflicts (diff3 style) where only the version clashes are combined; real ones are not.
    const std::string toggledHere =
        "window.SITE_DATA = {\n"
        "<<<<<<< HEAD\n  \"version\": \"20261027T220000Z\",\n  \"commissionsOpen\": false,\n"
        "||||||| base\n  \"version\": \"20261027T210000Z\",\n  \"commissionsOpen\": true,\n"
        "=======\n  \"version\": \"20261027T215000Z\",\n  \"commissionsOpen\": true,\n"
        ">>>>>>> origin/main\n"
        "  \"gallery\": [\n    { \"name\": \"Ember\", \"file\": \"assets/gallery/ember.webp\" },\n"
        "    { \"name\": \"New\", \"file\": \"assets/gallery/new.webp\" }\n  ]\n};\n";
    std::string out;
    CHECK(ResolveVersionConflicts(toggledHere, out));
    CHECK(out == "window.SITE_DATA = {\n  \"version\": \"20261027T220000Z\",\n  \"commissionsOpen\": false,\n"
                 "  \"gallery\": [\n    { \"name\": \"Ember\", \"file\": \"assets/gallery/ember.webp\" },\n"
                 "    { \"name\": \"New\", \"file\": \"assets/gallery/new.webp\" }\n  ]\n};\n");
    CHECK(ExtractSiteVersion(out) == L"20261027T220000Z");
    const std::string toggledThere =
        "window.SITE_DATA = {\r\n<<<<<<< HEAD\r\n  \"version\": \"A\",\r\n  \"commissionsOpen\": true,\r\n"
        "||||||| base\r\n  \"version\": \"B\",\r\n  \"commissionsOpen\": true,\r\n"
        "=======\r\n  \"version\": \"C\",\r\n  \"commissionsOpen\": false,\r\n>>>>>>> theirs\r\n  \"gallery\": []\r\n};\r\n";
    CHECK(ResolveVersionConflicts(toggledThere, out) && out.find("\"commissionsOpen\": false") != std::string::npos &&
          out.find("<<<") == std::string::npos);
    const std::string bothDifferentLines = // each side changed a different line next to the version
        "window.SITE_DATA = {\n<<<<<<< HEAD\n  \"version\": \"A\",\n  \"commissionsOpen\": false,\n  \"title\": \"x\",\n"
        "||||||| base\n  \"version\": \"B\",\n  \"commissionsOpen\": true,\n  \"title\": \"x\",\n"
        "=======\n  \"version\": \"C\",\n  \"commissionsOpen\": true,\n  \"title\": \"y\",\n>>>>>>> theirs\n"
        "  \"gallery\": []\n};\n";
    CHECK(ResolveVersionConflicts(bothDifferentLines, out) &&
          out == "window.SITE_DATA = {\n  \"version\": \"A\",\n  \"commissionsOpen\": false,\n  \"title\": \"y\",\n"
                 "  \"gallery\": []\n};\n");
    const std::string realConflict =
        "window.SITE_DATA = {\n<<<<<<< HEAD\n  \"version\": \"A\",\n  \"commissionsOpen\": false,\n"
        "||||||| base\n  \"version\": \"B\",\n  \"commissionsOpen\": true,\n"
        "=======\n  \"version\": \"C\",\n  \"commissionsOpen\": null,\n>>>>>>> theirs\n  \"gallery\": []\n};\n";
    out = "unchanged";
    CHECK(!ResolveVersionConflicts(realConflict, out) && out == "unchanged");
    const std::string renameConflict = // a conflict without the version line stays a conflict
        "window.SITE_DATA = {\n  \"version\": \"A\",\n  \"gallery\": [\n<<<<<<< HEAD\n    { \"name\": \"Mine\" }\n"
        "||||||| base\n    { \"name\": \"Old\" }\n=======\n    { \"name\": \"Theirs\" }\n>>>>>>> theirs\n  ]\n};\n";
    CHECK(!ResolveVersionConflicts(renameConflict, out));
    CHECK(!ResolveVersionConflicts(stamped, out)); // nothing to resolve
    CHECK(!ResolveVersionConflicts("<<<<<<< HEAD\n  \"version\": \"A\",\n=======\n", out)); // unterminated
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

static std::string ReadText(const std::wstring& path) {
    std::string s;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return s;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n) s.append(buf, n);
    CloseHandle(h);
    return s;
}

static const char kFixture[] =
    "window.SITE_DATA = {\n  \"commissionsOpen\": true,\n  \"gallery\": [\n"
    "    { \"name\": \"Ember\", \"file\": \"assets/gallery/ember.webp\" }\n  ]\n};\n";
static const char kLicense[] = "GNU GENERAL PUBLIC LICENSE (same text on both sides)\n";

static void MakeDirs(const std::wstring& dir) {
    for (const wchar_t* sub : {L"", L"\\data", L"\\assets", L"\\assets\\gallery"}) CreateDirectoryW((dir + sub).c_str(), nullptr);
}

static void MakeSite(const std::wstring& dir) {
    MakeDirs(dir);
    WriteText(dir + L"\\index.html", "<!doctype html><title>site</title>\n");
    WriteText(dir + L"\\script.js", "console.log('site');\n");
    WriteText(dir + L"\\README.md", "# site\n");
    WriteText(dir + L"\\LICENSE", kLicense);
    WriteText(dir + L"\\data\\site-data.js", kFixture);
    WriteText(dir + L"\\assets\\gallery\\ember.webp", "ember-local");
    WriteText(dir + L"\\assets\\gallery\\nyxen.webp", "nyxen-same");
}

static const char* Name(GitOutcome o) {
    switch (o) {
    case GitOutcome::Done: return "Done";
    case GitOutcome::NothingToDo: return "NothingToDo";
    case GitOutcome::NoGit: return "NoGit";
    case GitOutcome::NotRepo: return "NotRepo";
    case GitOutcome::NoRemote: return "NoRemote";
    case GitOutcome::NoSiteOnGitHub: return "NoSiteOnGitHub";
    case GitOutcome::Unrelated: return "Unrelated";
    case GitOutcome::Conflict: return "Conflict";
    case GitOutcome::AuthFailed: return "AuthFailed";
    case GitOutcome::NoAccess: return "NoAccess";
    case GitOutcome::NotFound: return "NotFound";
    case GitOutcome::Offline: return "Offline";
    case GitOutcome::Cancelled: return "Cancelled";
    case GitOutcome::Failed: return "Failed";
    }
    return "?";
}

static GitResult Step(const char* what, GitResult r) {
    std::printf("  -> %s: %s  (%s)\n", what, Name(r.outcome), ToUtf8(r.message).c_str());
    for (auto& f : r.files) std::printf("       file: %s\n", ToUtf8(f).c_str());
    return r;
}

static bool Has(const std::vector<std::wstring>& v, const wchar_t* item) {
    return std::find(v.begin(), v.end(), std::wstring(item)) != v.end();
}

static std::wstring Head(const std::wstring& dir) {
    std::wstring tmp = dir + L"\\..\\head.txt";
    _wsystem((L"cmd /c \"cd /d \"" + dir + L"\" && git rev-parse HEAD > \"" + tmp + L"\" 2>nul\"").c_str());
    std::string s = ReadText(tmp);
    DeleteFileW(tmp.c_str());
    return FromUtf8(s);
}

// Output of a shell command run in `dir` (stdout only).
static std::string Capture(const std::wstring& dir, const std::wstring& cmd) {
    std::wstring tmp = dir + L"\\..\\capture.txt";
    _wsystem((L"cmd /c \"cd /d \"" + dir + L"\" && " + cmd + L" > \"" + tmp + L"\" 2>nul\"").c_str());
    std::string s = ReadText(tmp);
    DeleteFileW(tmp.c_str());
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    return s;
}

static void TestGit(const std::wstring& sandbox) {
    std::printf("[git] %s\n", ToUtf8(sandbox).c_str());
    CreateDirectoryW(sandbox.c_str(), nullptr);
    const std::wstring bare = sandbox + L"\\github.git", web = sandbox + L"\\web", site = sandbox + L"\\site",
                       fresh = sandbox + L"\\fresh";
    std::wstring err;
    const std::wstring me = L"";

    // ---- The owner's real situation ----------------------------------------------------------------
    // "GitHub": re-created on github.com with a LICENSE, then images uploaded in the browser (no site yet).
    CHECK(Sh(sandbox, L"git init -q --bare -b main github.git") == 0);
    CHECK(Sh(sandbox, L"git clone -q github.git web") == 0);
    MakeDirs(web);
    WriteText(web + L"\\LICENSE", kLicense);
    CHECK(Sh(web, L"git add LICENSE && git commit -qm \"Initial commit\" && git push -q origin main") == 0);
    WriteText(web + L"\\assets\\paw.svg", "<svg/>");
    WriteText(web + L"\\assets\\gallery\\ember.webp", "ember-uploaded-on-github"); // differs from the local copy
    WriteText(web + L"\\assets\\gallery\\nyxen.webp", "nyxen-same");               // identical
    WriteText(web + L"\\assets\\gallery\\death.webp", "only-on-github");
    CHECK(Sh(web, L"git add -A && git commit -qm \"Add files via upload\" && git push -q origin main") == 0);

    // This folder: its own, older history (no shared commit), no upstream, a stale origin/main from the
    // deleted repository, uncommitted edits and new untracked files that must be published.
    MakeSite(site);
    // An older date, so this commit can never be identical to GitHub's "Initial commit" made in the same second.
    CHECK(Sh(site, L"git init -q -b main && git add LICENSE && git commit -qm \"Initial commit\" --date=2024-01-01T12:00:00") == 0);
    CHECK(Sh(site, L"git add -A && git commit -qm \"added tingy\" && git commit -q --allow-empty -m \"Initial site upload\"") == 0);
    CHECK(Sh(site, L"git remote add origin \"" + bare + L"\" && git update-ref refs/remotes/origin/main HEAD~1") == 0);
    WriteText(site + L"\\README.md", "# site (edited)\n");
    WriteText(site + L"\\script.js", "console.log('edited');\n");
    WriteText(site + L"\\web.config", "<configuration/>");
    WriteText(site + L"\\song.mp3", "fake mp3");
    RepoInfo info = InspectRepo(site);
    CHECK(info.isRepo && info.hasCommits && !info.hasUpstream && SameRemoteUrl(info.originUrl, bare));
    const std::wstring head0 = Head(site);

    CHECK(Step("sync (GitHub has no site yet)", SyncFromGitHub(site, me, L"", MergeChoice::Ask, Log)).outcome ==
          GitOutcome::NoSiteOnGitHub);
    CHECK(Head(site) == head0 && ReadText(site + L"\\README.md") == "# site (edited)\n");
    PublishState ps = GetPublishState(site);
    CHECK(ps.remoteKnown && !ps.remoteHasSite && ps.localChanges && ps.Unpublished());
    CHECK(Has(ps.remoteGallery, L"assets/gallery/ember.webp") && Has(ps.changedGallery, L"assets/gallery/ember.webp") &&
          !Has(ps.changedGallery, L"assets/gallery/nyxen.webp"));

    GitResult r = Step("push (needs the one confirmation)", PushChanges(site, me, L"first", MergeChoice::Ask, Log));
    CHECK(r.outcome == GitOutcome::Unrelated && Has(r.files, L"assets/paw.svg") && Has(r.files, L"assets/gallery/death.webp"));
    CHECK(!FileExists(site + L"\\.git\\MERGE_HEAD") && !FileExists(site + L"\\assets\\paw.svg"));
    CHECK(Step("push (combine, keep local site)", PushChanges(site, me, L"first", MergeChoice::KeepLocal, Log)).outcome ==
          GitOutcome::Done);
    for (const wchar_t* f : {L"data/site-data.js", L"index.html", L"web.config", L"song.mp3", L"LICENSE", L"assets/paw.svg",
                             L"assets/gallery/death.webp", L"assets/gallery/nyxen.webp"})
        CHECK(Sh(sandbox, L"git --git-dir=github.git cat-file -e main:" + std::wstring(f)) == 0);
    CHECK(Sh(sandbox, L"git --git-dir=github.git show main:assets/gallery/ember.webp | findstr /c:\"ember-local\"") == 0);
    CHECK(Sh(sandbox, L"git --git-dir=github.git show main:README.md | findstr /c:\"edited\"") == 0);
    CHECK(FileExists(site + L"\\assets\\paw.svg") && FileExists(site + L"\\assets\\gallery\\death.webp"));
    CHECK(ReadText(site + L"\\assets\\gallery\\ember.webp") == "ember-local");
    info = InspectRepo(site);
    CHECK(info.hasUpstream);
    ps = GetPublishState(site);
    CHECK(ps.remoteHasSite && !ps.Unpublished() && ps.changedGallery.empty());
    CHECK(Step("push again", PushChanges(site, me, L"", MergeChoice::Ask, Log)).outcome == GitOutcome::NothingToDo);
    CHECK(Step("sync when up to date", SyncFromGitHub(site, me, L"", MergeChoice::Ask, Log)).outcome == GitOutcome::NothingToDo);

    // ---- Version stamp: every upload carries a fresh one; a new stamp alone is never uploaded -----
    const wchar_t kShowData[] = L"git --git-dir=github.git show main:data/site-data.js";
    const std::wstring ver0 = ExtractSiteVersion(Capture(sandbox, kShowData));
    CHECK(ver0.size() == 16 && ExtractSiteVersion(ReadText(site + L"\\data\\site-data.js")) == ver0);
    Sleep(1100);
    SiteData sd;
    CHECK(sd.Load(site, err) && sd.Save(err) && sd.Version() > ver0); // only the stamp changes
    CHECK(Step("push only a new stamp", PushChanges(site, me, L"", MergeChoice::Ask, Log)).outcome == GitOutcome::NothingToDo);
    CHECK(!GetPublishState(site).localChanges && ExtractSiteVersion(ReadText(site + L"\\data\\site-data.js")) == ver0);
    Sleep(1100);
    WriteText(site + L"\\index.html", "<!doctype html><title>edited by hand</title>\n"); // not by the app
    GitResult pv = Step("push a hand-edited page", PushChanges(site, me, L"page", MergeChoice::Ask, Log));
    const std::wstring ver1 = ExtractSiteVersion(Capture(sandbox, kShowData));
    CHECK(pv.outcome == GitOutcome::Done && pv.version == ver1 && ver1 > ver0);
    CHECK(Capture(sandbox, L"git --git-dir=github.git log -1 --format=%s main") == "Update site: page\n");
    CHECK(Capture(sandbox, L"git --git-dir=github.git diff --name-only main~1 main") == "data/site-data.js\nindex.html\n");
    CHECK(!GetPublishState(site).Unpublished());
    Sleep(1100);
    WriteText(site + L"\\script.js", "console.log('committed by hand');\n");
    CHECK(Sh(site, L"git commit -qam \"hand commit\"") == 0); // e.g. a push that failed earlier
    pv = Step("push a commit made by hand", PushChanges(site, me, L"", MergeChoice::Ask, Log));
    const std::wstring ver2 = ExtractSiteVersion(Capture(sandbox, kShowData));
    CHECK(pv.outcome == GitOutcome::Done && pv.version == ver2 && ver2 > ver1);
    CHECK(Capture(sandbox, L"git --git-dir=github.git log -2 --format=%s main") == "hand commit\nUpdate site: page\n");
    CHECK(!GetPublishState(site).Unpublished());

    // ---- Another computer: empty folder → connect → sync downloads the site -----------------------
    CreateDirectoryW(fresh.c_str(), nullptr);
    CHECK(Step("connect fresh", ConnectRepo(fresh, bare, false, Log)).outcome == GitOutcome::Done);
    CHECK(Step("sync fresh", SyncFromGitHub(fresh, me, L"", MergeChoice::Ask, Log)).outcome == GitOutcome::Done);
    SiteData d;
    CHECK(d.Load(fresh, err) && d.Count() == 1 && d.CommissionsOpen());
    CHECK(FileExists(fresh + L"\\assets\\gallery\\death.webp"));

    // Edit there, push; the first folder fast-forwards on sync.
    d.SetCommissionsOpen(false);
    d.Add(L"Death", L"assets/gallery/death.webp");
    CHECK(d.Save(err));
    CHECK(Step("push from fresh", PushChanges(fresh, me, L"commissions closed", MergeChoice::Ask, Log)).outcome == GitOutcome::Done);
    CHECK(Sh(sandbox, L"git --git-dir=github.git log -1 --format=%s main | findstr /c:\"Update site: commissions closed\"") == 0);
    CHECK(Step("sync site (fast-forward)", SyncFromGitHub(site, me, L"", MergeChoice::Ask, Log)).outcome == GitOutcome::Done);
    CHECK(d.Load(site, err) && !d.CommissionsOpen() && d.Count() == 2);

    // ---- Both sides changed different files: push combines automatically ------------------------
    WriteText(fresh + L"\\index.html", "<!doctype html><title>from fresh</title>\n");
    CHECK(Step("push index from fresh", PushChanges(fresh, me, L"page", MergeChoice::Ask, Log)).outcome == GitOutcome::Done);
    CHECK(d.Load(site, err));
    d.SetCommissionsOpen(true);
    CHECK(d.Save(err));
    CHECK(Step("push while behind", PushChanges(site, me, L"commissions open", MergeChoice::Ask, Log)).outcome == GitOutcome::Done);
    CHECK(ReadText(site + L"\\index.html").find("from fresh") != std::string::npos);
    CHECK(Sh(sandbox, L"git --git-dir=github.git show main:data/site-data.js | findstr /c:\"\\\"commissionsOpen\\\": true\"") == 0);

    // ---- Both sides changed data/site-data.js in different places: their version stamps clash, but
    //      that is combined automatically, without asking ----------------------------------------------
    CHECK(Step("sync fresh", SyncFromGitHub(fresh, me, L"", MergeChoice::Ask, Log)).outcome == GitOutcome::Done);
    CHECK(d.Load(fresh, err));
    d.SetCommissionsOpen(false);
    CHECK(d.Save(err));
    CHECK(Step("push toggle from fresh", PushChanges(fresh, me, L"commissions closed", MergeChoice::Ask, Log)).outcome ==
          GitOutcome::Done);
    Sleep(1100);
    CHECK(d.Load(site, err));
    const size_t count0 = d.Count();
    d.Add(L"Nyxen", L"assets/gallery/nyxen.webp");
    CHECK(d.Save(err));
    const std::wstring mine = d.Version();
    r = Step("push gallery change (stamps clash)", PushChanges(site, me, L"added Nyxen", MergeChoice::Ask, Log));
    const std::string remoteData = Capture(sandbox, kShowData);
    CHECK(r.outcome == GitOutcome::Done && r.version == ExtractSiteVersion(remoteData) && r.version >= mine);
    CHECK(remoteData.find("\"commissionsOpen\": false") != std::string::npos && remoteData.find("Nyxen") != std::string::npos);
    CHECK(remoteData.rfind("window.SITE_DATA = {\n  \"version\": ", 0) == 0);
    CHECK(d.Load(site, err) && d.Count() == count0 + 1 && !d.CommissionsOpen() && d.Version() == r.version);
    CHECK(!GetPublishState(site).Unpublished());

    // ---- Same line changed on both sides: ask, nothing changes; then each choice -----------------
    CHECK(Step("sync fresh", SyncFromGitHub(fresh, me, L"", MergeChoice::Ask, Log)).outcome == GitOutcome::Done);
    CHECK(d.Load(fresh, err));
    d.Rename(0, L"Theirs");
    CHECK(d.Save(err));
    CHECK(Step("push rename from fresh", PushChanges(fresh, me, L"renamed", MergeChoice::Ask, Log)).outcome == GitOutcome::Done);
    CHECK(d.Load(site, err));
    d.Rename(0, L"Mine");
    CHECK(d.Save(err));
    WriteText(site + L"\\assets\\gallery\\new.webp", "new local image");
    const std::wstring head1 = Head(site);
    r = Step("sync conflicting", SyncFromGitHub(site, me, L"renamed", MergeChoice::Ask, Log));
    CHECK(r.outcome == GitOutcome::Conflict && Has(r.files, L"data/site-data.js"));
    CHECK(!FileExists(site + L"\\.git\\MERGE_HEAD") && d.Load(site, err) && d.Item(0).name == L"Mine");
    CHECK(Step("sync, use GitHub's", SyncFromGitHub(site, me, L"renamed", MergeChoice::UseGitHub, Log)).outcome ==
          GitOutcome::Done);
    CHECK(d.Load(site, err) && d.Item(0).name == L"Theirs");
    CHECK(FileExists(site + L"\\assets\\gallery\\new.webp")); // non-conflicting local work survives
    CHECK(Head(site) != head1);
    CHECK(Step("push after choosing", PushChanges(site, me, L"", MergeChoice::Ask, Log)).outcome == GitOutcome::Done);

    CHECK(Step("sync fresh", SyncFromGitHub(fresh, me, L"", MergeChoice::Ask, Log)).outcome == GitOutcome::Done);
    CHECK(d.Load(fresh, err));
    d.Rename(0, L"Fresh name");
    CHECK(d.Save(err));
    CHECK(Step("push from fresh", PushChanges(fresh, me, L"renamed", MergeChoice::Ask, Log)).outcome == GitOutcome::Done);
    CHECK(d.Load(site, err));
    d.Rename(0, L"Keep me");
    CHECK(d.Save(err));
    CHECK(Step("push conflicting", PushChanges(site, me, L"renamed", MergeChoice::Ask, Log)).outcome == GitOutcome::Conflict);
    CHECK(Step("push, keep mine", PushChanges(site, me, L"renamed", MergeChoice::KeepLocal, Log)).outcome == GitOutcome::Done);
    CHECK(Sh(sandbox, L"git --git-dir=github.git show main:data/site-data.js | findstr /c:\"Keep me\"") == 0);

    // ---- A merge left half-done by another tool is cleaned up -------------------------------------
    CHECK(Step("sync fresh", SyncFromGitHub(fresh, me, L"", MergeChoice::Ask, Log)).outcome == GitOutcome::Done);
    CHECK(Sh(fresh, L"git checkout -q -b side") == 0);
    WriteText(fresh + L"\\index.html", "side\n");
    CHECK(Sh(fresh, L"git commit -qam side && git checkout -q main") == 0);
    WriteText(fresh + L"\\index.html", "main\n");
    CHECK(Sh(fresh, L"git commit -qam main-edit") == 0);
    CHECK(Sh(fresh, L"git merge side") != 0);
    CHECK(FileExists(fresh + L"\\.git\\MERGE_HEAD"));
    CHECK(Step("sync with leftover merge", SyncFromGitHub(fresh, me, L"", MergeChoice::Ask, Log)).outcome ==
          GitOutcome::NothingToDo);
    CHECK(!FileExists(fresh + L"\\.git\\MERGE_HEAD") && d.Load(fresh, err) && d.Item(0).name == L"Keep me");
    CHECK(ReadText(fresh + L"\\index.html").find("main") == 0);

    // ---- Empty GitHub repository ----------------------------------------------------------------
    const std::wstring empty = sandbox + L"\\empty", emptyBare = sandbox + L"\\empty.git";
    CHECK(Sh(sandbox, L"git init -q --bare -b main empty.git") == 0);
    MakeSite(empty);
    CHECK(Step("connect", ConnectRepo(empty, emptyBare, false, Log)).outcome == GitOutcome::Done);
    CHECK(Step("sync empty GitHub", SyncFromGitHub(empty, me, L"", MergeChoice::Ask, Log)).outcome == GitOutcome::NoSiteOnGitHub);
    CHECK(Step("push to empty GitHub", PushChanges(empty, me, L"", MergeChoice::Ask, Log)).outcome == GitOutcome::Done);
    CHECK(Sh(sandbox, L"git --git-dir=empty.git log -1 --format=%s main | findstr /c:\"Initial site upload\"") == 0);

    // ---- No git identity: commits use the GitHub login ------------------------------------------
    CHECK(Sh(empty, L"git config user.useConfigOnly true && git config user.name \"\" && git config user.email \"\"") == 0);
    WriteText(empty + L"\\index.html", "<!doctype html><title>identity</title>\n");
    CHECK(Step("push without identity", PushChanges(empty, L"tester", L"page", MergeChoice::Ask, Log)).outcome == GitOutcome::Done);
    CHECK(Sh(sandbox, L"git --git-dir=empty.git log -1 --format=%ae main | findstr /c:\"tester@users.noreply.github.com\"") == 0);

    // ---- Errors: not connected, no origin, offline, private/missing repo (never hangs) ----------
    const std::wstring loose = sandbox + L"\\loose";
    MakeSite(loose);
    CHECK(Step("push before connect", PushChanges(loose, me, L"x", MergeChoice::Ask, Log)).outcome == GitOutcome::NotRepo);
    CHECK(Sh(loose, L"git init -q") == 0);
    CHECK(Step("sync without origin", SyncFromGitHub(loose, me, L"", MergeChoice::Ask, Log)).outcome == GitOutcome::NoRemote);
    CHECK(Step("connect offline url", ConnectRepo(loose, L"https://commission-manager-test.invalid/x.git", false, Log)).outcome ==
          GitOutcome::Done);
    CHECK(Step("sync offline", SyncFromGitHub(loose, me, L"", MergeChoice::Ask, Log)).outcome == GitOutcome::Offline);
    CHECK(Step("change url (no consent)", ConnectRepo(loose, L"https://github.com/Xavokai/commission-manager-no-such-repo.git", false, Log))
              .outcome == GitOutcome::Failed);
    CHECK(Step("change url (consent)", ConnectRepo(loose, L"https://github.com/Xavokai/commission-manager-no-such-repo.git", true, Log))
              .outcome == GitOutcome::Done);
    const ULONGLONG t0 = GetTickCount64();
    GitOutcome o = Step("sync private/missing GitHub repo", SyncFromGitHub(loose, me, L"", MergeChoice::Ask, Log)).outcome;
    CHECK(o == GitOutcome::AuthFailed || o == GitOutcome::NotFound);
    CHECK(GetTickCount64() - t0 < 60000);
}

// `served` is the folder a local web server (e.g. python -m http.server) publishes at `base`.
static void TestLive(const std::wstring& base, const std::wstring& served) {
    std::printf("[live] %s -> %s\n", ToUtf8(base).c_str(), ToUtf8(served).c_str());
    CreateDirectoryW((served + L"\\data").c_str(), nullptr);
    const std::wstring file = served + L"\\data\\site-data.js";
    auto publish = [&](const char* version) {
        WriteText(file, std::string("window.SITE_DATA = {\n  \"version\": \"") + version +
                            "\",\n  \"commissionsOpen\": true,\n  \"gallery\": []\n};\n");
    };
    auto progress = [](int attempt, const std::wstring& seen, const std::wstring& problem) {
        std::printf("    | check %d: version=%s %s\n", attempt, ToUtf8(seen).c_str(), ToUtf8(problem).c_str());
    };
    publish("20261027T210000Z");
    LiveCheckResult r = WaitUntilLive(base, L"20261027T210000Z", nullptr, 200, 2000, progress);
    CHECK(r.outcome == LiveOutcome::Live && r.attempts == 1 && r.seenVersion == L"20261027T210000Z");
    // The website already shows a later upload (e.g. from another computer): that includes ours.
    r = WaitUntilLive(base, L"20261027T200000Z", nullptr, 200, 2000, progress);
    CHECK(r.outcome == LiveOutcome::Live && r.attempts == 1);

    // Published while waiting.
    HANDLE cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread later([&] {
        Sleep(1500);
        publish("20261027T220000Z");
    });
    ULONGLONG t0 = GetTickCount64();
    r = WaitUntilLive(base, L"20261027T220000Z", cancel, 500, 10000, progress);
    later.join();
    CHECK(r.outcome == LiveOutcome::Live && r.attempts >= 2 && r.seenVersion == L"20261027T220000Z");
    CHECK(GetTickCount64() - t0 < 6000);

    // Never published: gives up after the timeout.
    t0 = GetTickCount64();
    r = WaitUntilLive(base, L"20261027T230000Z", cancel, 300, 1500, progress);
    CHECK(r.outcome == LiveOutcome::TimedOut && r.seenVersion == L"20261027T220000Z" && r.attempts >= 3);
    CHECK(GetTickCount64() - t0 < 4000);

    // Cancelled (window closed) while waiting between checks: stops at once.
    std::thread closer([&] {
        Sleep(700);
        SetEvent(cancel);
    });
    t0 = GetTickCount64();
    r = WaitUntilLive(base, L"20261027T230000Z", cancel, kLiveCheckIntervalMs, kLiveCheckTimeoutMs, progress);
    closer.join();
    CHECK(r.outcome == LiveOutcome::Cancelled && r.attempts == 1 && GetTickCount64() - t0 < 2000);
    CloseHandle(cancel);

    // No site there (404) / server unreachable: keeps trying, then times out.
    r = WaitUntilLive(base + L"no-site-here/", L"20261027T230000Z", nullptr, 200, 700, progress);
    CHECK(r.outcome == LiveOutcome::TimedOut && r.seenVersion.empty());
    r = WaitUntilLive(L"http://127.0.0.1:9/", L"20261027T230000Z", nullptr, 200, 700, progress);
    CHECK(r.outcome == LiveOutcome::TimedOut && r.seenVersion.empty());

    HttpResponse fresh = HttpGetFresh(WithCacheBuster(base + L"data/site-data.js", UnixTimeMs()));
    CHECK(fresh.status == 200 && ExtractSiteVersion(fresh.body) == L"20261027T220000Z");
}

// Saves and removes a credential for a host that can't exist, through the user's real credential helper.
static void TestCred() {
    std::printf("[cred]\n");
    const std::wstring host = L"commission-manager-test.invalid";
    std::wstring u, p;
    CHECK(!ReadSavedCredential(host, L"tester", u, p));
    CHECK(Step("save", SaveSignIn(host, L"tester", L"not-a-real-token-123", Log)).outcome == GitOutcome::Done);
    CHECK(ReadSavedCredential(host, L"tester", u, p) && u == L"tester" && p == L"not-a-real-token-123");
    CHECK(Step("sign out", SignOut(host, L"tester", Log)).outcome == GitOutcome::Done);
    CHECK(!ReadSavedCredential(host, L"tester", u, p));
    GitHubAccount a = CheckGitHubAccount(L"");
    std::printf("  github.com account: git=%d gcm=%s signedIn=%d user=%s note=%s\n", a.gitAvailable,
                ToUtf8(a.gcmVersion).c_str(), a.signedIn, ToUtf8(a.username).c_str(), ToUtf8(a.note).c_str());
    CHECK(a.gitAvailable);
}

static void TestApi() {
    std::printf("[api]\n");
    HttpResponse repo = HttpGet(L"https://api.github.com/repos/Xavokai/CommissionSIte", L"");
    CHECK(repo.status == 200 && repo.body.find("Xavokai/CommissionSIte") != std::string::npos);
    HttpResponse user = HttpGet(L"https://api.github.com/user", L"");
    CHECK(user.status == 401);
    HttpResponse bad = HttpGet(L"https://api.github.com/user", L"github_pat_invalid_token_for_testing");
    CHECK(bad.status == 401);
    HttpResponse offline = HttpGet(L"https://commission-manager-test.invalid/", L"");
    CHECK(offline.status == 0 && !offline.error.empty());
}

int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::wstring mode = argc > 1 ? argv[1] : L"";
    if (mode == L"json") TestJson();
    else if (mode == L"logic") TestLogic();
    else if (mode == L"version") TestVersion();
    else if (mode == L"site" && argc > 3) TestSite(argv[2], argv[3]);
    else if (mode == L"git" && argc > 2) TestGit(argv[2]);
    else if (mode == L"cred") TestCred();
    else if (mode == L"api") TestApi();
    else if (mode == L"live" && argc > 3) TestLive(argv[2], argv[3]);
    else {
        std::printf("usage: core_tests json | logic | version | site <repo-copy> <image-dir> | git <sandbox-dir> | cred | api"
                    " | live <base-url> <served-dir>\n");
        return 2;
    }
    std::printf(g_failures ? "\n%d FAILURE(S)\n" : "\nALL PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
