// Core (non-UI) logic for ZelRaccy Commission Manager:
// JSON, site-data.js load/save, image import (WIC), git push.
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
// Git publishing. All functions block; call them from a worker thread.
// ---------------------------------------------------------------------------
constexpr wchar_t kDefaultRemoteUrl[] = L"https://github.com/Xavokai/CommissionSIte.git";
constexpr wchar_t kBranch[] = L"main";

enum class GitOutcome {
    Done,        // connected / pushed / pulled
    NothingToDo, // nothing new to push or pull
    NoGit,       // git.exe not found
    NotRepo,     // folder is not the root of a git repository
    NoRemote,    // no "origin" remote
    Rejected,    // push refused: GitHub has commits we don't
    Unrelated,   // pull needs --allow-unrelated-histories (ask the user first)
    Conflict,    // pull hit conflicts; it was rolled back
    Failed,
};

struct GitResult {
    GitOutcome outcome = GitOutcome::Failed;
    std::wstring message; // short, user-readable
};

struct RepoInfo {
    bool gitAvailable = false;
    bool isRepo = false; // a repository rooted exactly at the folder
    bool hasCommits = false;
    bool hasUpstream = false;
    std::wstring branch;    // current branch ("" if none/detached)
    std::wstring originUrl; // "" if no origin
};

using LogFn = std::function<void(const std::wstring& line)>;

RepoInfo InspectRepo(const std::wstring& repo);
// Equal ignoring case, trailing '/' and ".git".
bool SameRemoteUrl(const std::wstring& a, const std::wstring& b);

// git init (if needed), branch "main", origin = url (set-url only if allowChangeOrigin).
GitResult ConnectRepo(const std::wstring& repo, const std::wstring& url, bool allowChangeOrigin, const LogFn& log);
// git add -A (whole repository, honoring .gitignore); lists the changed files in the log;
// git commit -m "Update site: <summary>"; git push (-u origin main without upstream).
GitResult PushChanges(const std::wstring& repo, const std::wstring& summary, const LogFn& log);
// git pull --rebase --autostash origin main. Unrelated histories return GitOutcome::Unrelated unless
// allowUnrelated, which adds --allow-unrelated-histories -X theirs (this folder wins on conflicts).
GitResult PullChanges(const std::wstring& repo, bool allowUnrelated, const LogFn& log);
