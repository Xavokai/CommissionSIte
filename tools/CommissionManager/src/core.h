// Core (non-UI) logic for ZelRaccy Commission Manager:
// JSON, site-data.js load/save, image import (WIC), GitHub sign-in and git publishing.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00 // Windows 10 (per-monitor DPI v2, WIC high-quality cubic scaling)
#endif
#ifndef WINVER
#define WINVER _WIN32_WINNT
#endif
#include <windows.h>

#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// Text helpers
// ---------------------------------------------------------------------------
std::string ToUtf8(std::wstring_view s);
std::wstring FromUtf8(std::string_view s);
std::wstring Trim(const std::wstring& s);
bool FileExists(const std::wstring& path);
bool DirExists(const std::wstring& path);
// "assets/gallery/x.jpg" -> "<repo>\assets\gallery\x.jpg"
std::wstring RepoPath(const std::wstring& repo, const std::wstring& rel);

// ---------------------------------------------------------------------------
// JSON (order-preserving; numbers kept as their original text)
// ---------------------------------------------------------------------------
namespace json {

struct Value;
using Member = std::pair<std::string, Value>;

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    std::string text;            // String: UTF-8 contents; Number: literal text
    std::vector<Value> items;    // Array
    std::vector<Member> members; // Object

    static Value String(std::string s);
    static Value Bool(bool b);
    static Value Array();
    static Value Object();

    bool IsScalar() const { return type != Type::Array && type != Type::Object; }
    Value* Get(std::string_view key);
    const Value* Get(std::string_view key) const;
    void Set(const std::string& key, Value v); // replace in place or append
};

// Parses one JSON value starting at `pos` (leading whitespace allowed).
// On success `pos` points just past the value.
bool Parse(const std::string& src, size_t& pos, Value& out, std::string& error);
// Pretty-printed, 2-space indent; flat objects/scalar arrays inside arrays stay on one line.
std::string Serialize(const Value& v);

} // namespace json

// ---------------------------------------------------------------------------
// data/site-data.js
// ---------------------------------------------------------------------------
struct GalleryItem {
    std::wstring name;
    std::wstring file;
};

class SiteData {
public:
    // Loads <repo>\data\site-data.js. On failure returns false with a user-readable message.
    bool Load(const std::wstring& repo, std::wstring& error);
    // Atomically rewrites data/site-data.js (temp file + replace).
    bool Save(std::wstring& error) const;

    const std::wstring& Repo() const { return repo_; }
    const std::wstring& DataPath() const { return dataPath_; }

    bool CommissionsOpen() const;
    void SetCommissionsOpen(bool open);

    size_t Count() const;
    GalleryItem Item(size_t i) const;
    std::vector<std::wstring> Files() const;
    void Add(const std::wstring& name, const std::wstring& file);
    void Rename(size_t i, const std::wstring& name);
    void Remove(size_t i);
    void Move(size_t from, size_t to);

    std::string ToText() const;

private:
    json::Value& Gallery();
    const json::Value* GalleryConst() const;

    std::wstring repo_;
    std::wstring dataPath_;
    std::string prefix_; // text before "window.SITE_DATA" (e.g. comments), preserved
    std::string suffix_; // text after the final ';', preserved
    json::Value root_;
};

// ---------------------------------------------------------------------------
// Images (Windows Imaging Component). COM must be initialized on the calling thread.
// ---------------------------------------------------------------------------
constexpr UINT kMaxImageSide = 1920;
constexpr float kJpegQuality = 0.88f;

// Lowercase ASCII letters/digits kept; every run of other characters becomes '-'.
std::wstring Slugify(const std::wstring& name);
bool IsSupportedImageExt(const std::wstring& path);

// Copies `source` into <repo>\assets\gallery\<slug>.<ext>, downscaled to kMaxImageSide and
// re-encoded as JPEG (PNG if the image has transparency). The source file is never modified.
// `takenFiles` = repo-relative files already used by the gallery (for de-duplication).
bool ImportImage(const std::wstring& repo, const std::wstring& source, const std::wstring& displayName,
                 const std::vector<std::wstring>& takenFiles, std::wstring& relPathOut, std::wstring& error);

// Top-down 32bpp premultiplied-BGRA DIB scaled to fit maxW x maxH, or nullptr.
HBITMAP LoadThumbnail(const std::wstring& path, UINT maxW, UINT maxH, UINT& w, UINT& h);

// ---------------------------------------------------------------------------
// Git publishing & GitHub sign-in. Functions that run git block; call them from a worker thread.
// Git never waits for an invisible prompt: every command runs with GIT_TERMINAL_PROMPT=0 and
// GCM_INTERACTIVE=never, except SignInWithBrowser (the explicit Sign-in button).
// ---------------------------------------------------------------------------
constexpr wchar_t kDefaultRemoteUrl[] = L"https://github.com/Xavokai/CommissionSIte.git";
constexpr wchar_t kBranch[] = L"main";
constexpr wchar_t kGitHubHost[] = L"github.com";
constexpr wchar_t kNewTokenUrl[] = L"https://github.com/settings/personal-access-tokens/new";

enum class GitOutcome {
    Done,           // connected / pushed / synced / signed in
    NothingToDo,    // already up to date
    NoGit,          // git.exe not found
    NotRepo,        // folder is not the root of a git repository
    NoRemote,       // no "origin" remote
    NoSiteOnGitHub, // GitHub has no data/site-data.js yet (or no main branch at all)
    Unrelated,      // GitHub and this folder were started separately: ask before combining
    Conflict,       // files changed on both sides: ask which version to keep (nothing was changed)
    AuthFailed,     // not signed in, or GitHub rejected the sign-in (401)
    NoAccess,       // signed in, but not allowed to write to the repository (403)
    NotFound,       // repository doesn't exist (or is private and invisible to this account)
    Offline,        // GitHub unreachable / timed out
    Cancelled,      // the user cancelled (sign-in)
    Failed,
};

struct GitResult {
    GitOutcome outcome = GitOutcome::Failed;
    std::wstring message;            // short, user-readable
    std::vector<std::wstring> files; // Conflict: files changed on both sides; Unrelated: files only on GitHub
    std::wstring account;            // sign-in: the GitHub login, when known
};

// How to combine when GitHub and this folder both changed: ask (return Conflict/Unrelated), or take
// whole files from one side for the files changed in both places.
enum class MergeChoice { Ask, KeepLocal, UseGitHub };

struct RepoInfo {
    bool gitAvailable = false;
    bool isRepo = false; // a repository rooted exactly at the folder
    bool hasCommits = false;
    bool hasUpstream = false;
    std::wstring branch;    // current branch ("" if none/detached)
    std::wstring originUrl; // "" if no origin
};

// Compared with the last download from GitHub (refs/remotes/origin/main). Local git commands only.
struct PublishState {
    bool remoteKnown = false;   // GitHub's main branch has been downloaded
    bool remoteHasSite = false; // ...and it contains data/site-data.js
    bool localChanges = false;  // changed/new files not committed yet
    int ahead = 0, behind = 0;  // commits only here / only on GitHub
    std::vector<std::wstring> remoteGallery;  // files under assets/gallery/ on GitHub
    std::vector<std::wstring> changedGallery; // files under assets/gallery/ that differ from GitHub
    bool Unpublished() const { return localChanges || ahead > 0; }
};

using LogFn = std::function<void(const std::wstring& line)>;

RepoInfo InspectRepo(const std::wstring& repo);
PublishState GetPublishState(const std::wstring& repo);
// Equal ignoring case, trailing '/' and ".git".
bool SameRemoteUrl(const std::wstring& a, const std::wstring& b);

// git init (if needed), branch "main", origin = url (set-url only if allowChangeOrigin).
GitResult ConnectRepo(const std::wstring& repo, const std::wstring& url, bool allowChangeOrigin, const LogFn& log);
// Downloads GitHub's main branch and brings this folder up to date with it (fast-forward, or a merge
// after committing local changes as "Update site: <summary>"). NoSiteOnGitHub when GitHub has no
// data/site-data.js. `githubUser` picks the saved account and is the commit identity fallback.
GitResult SyncFromGitHub(const std::wstring& repo, const std::wstring& githubUser, const std::wstring& summary,
                         MergeChoice choice, const LogFn& log);
// git add -A + commit; combines with GitHub's newer commits first (like SyncFromGitHub, but also when
// GitHub has no site yet); then git push -u origin HEAD:main.
GitResult PushChanges(const std::wstring& repo, const std::wstring& githubUser, const std::wstring& summary,
                      MergeChoice choice, const LogFn& log);

struct GitHubAccount {
    bool gitAvailable = false;
    std::wstring gcmVersion; // Git Credential Manager version ("" = not installed)
    bool signedIn = false;
    bool verified = false; // GitHub confirmed the saved sign-in just now
    std::wstring username;
    std::wstring note; // extra explanation (expired sign-in, offline…)
};

// Looks up the saved github.com credential (non-interactive) and checks it with api.github.com/user.
GitHubAccount CheckGitHubAccount(const std::wstring& preferredUser);
// `git credential-manager github login --browser` (OAuth in the web browser). Cancellable.
GitResult SignInWithBrowser(const LogFn& log);
void CancelSignIn();
// Stores username + token with `git credential approve` (so git push uses it) and verifies it was saved.
GitResult SaveSignIn(const std::wstring& host, const std::wstring& username, const std::wstring& secret, const LogFn& log);
// `git credential reject` for the account, then verifies it is gone.
GitResult SignOut(const std::wstring& host, const std::wstring& username, const LogFn& log);
// Non-interactive `git credential fill`.
bool ReadSavedCredential(const std::wstring& host, const std::wstring& username, std::wstring& userOut,
                         std::wstring& secretOut);

struct HttpResponse {
    DWORD status = 0; // 0 = no response (offline, DNS, TLS…)
    std::string body;
    std::wstring error;
};
HttpResponse HttpGet(const std::wstring& url, const std::wstring& bearerToken);

// Pure helpers (unit-tested).
enum class GitError { Offline, Auth, NoAccess, NotFound, NoRemoteBranch, NonFastForward, Identity, Other };
GitError ClassifyGitOutput(std::string_view output);
bool ParseCredentialOutput(const std::string& out, std::wstring& username, std::wstring& password);
std::vector<std::wstring> ParseAccountList(const std::string& out); // `git credential-manager github list`
std::string CredentialRequest(const std::wstring& host, const std::wstring& username, const std::wstring& secret);
std::wstring CleanToken(const std::wstring& pasted); // drops whitespace/quotes picked up while copying
bool ParseGitHubLogin(const std::string& body, std::wstring& login);
bool GitHubRepoFromUrl(const std::wstring& url, std::wstring& owner, std::wstring& name);
