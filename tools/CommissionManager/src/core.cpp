#include "core.h"

#include <objbase.h>
#include <oleauto.h>
#include <propidl.h>
#include <wincodec.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <mutex>

// ===========================================================================
// Text helpers
// ===========================================================================
std::string ToUtf8(std::wstring_view s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring FromUtf8(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

std::wstring Trim(const std::wstring& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::iswspace(s[b])) ++b;
    while (e > b && std::iswspace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

bool FileExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring RepoPath(const std::wstring& repo, const std::wstring& rel) {
    std::wstring r = rel;
    std::replace(r.begin(), r.end(), L'/', L'\\');
    std::wstring base = repo;
    while (!base.empty() && (base.back() == L'\\' || base.back() == L'/')) base.pop_back();
    return base + L"\\" + r;
}

static std::wstring LowerAscii(std::wstring s) {
    for (auto& c : s)
        if (c >= L'A' && c <= L'Z') c = c - L'A' + L'a';
    return s;
}

static std::wstring ExtOf(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return {};
    return LowerAscii(path.substr(dot + 1));
}

static std::wstring SystemMessage(DWORD code) {
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, (LPWSTR)&buf, 0, nullptr);
    std::wstring s = buf ? Trim(buf) : L"error " + std::to_wstring(code);
    if (buf) LocalFree(buf);
    return s;
}

// ===========================================================================
// JSON
// ===========================================================================
namespace json {

Value Value::String(std::string s) { Value v; v.type = Type::String; v.text = std::move(s); return v; }
Value Value::Bool(bool b) { Value v; v.type = Type::Bool; v.boolean = b; return v; }
Value Value::Array() { Value v; v.type = Type::Array; return v; }
Value Value::Object() { Value v; v.type = Type::Object; return v; }

Value* Value::Get(std::string_view key) {
    for (auto& m : members)
        if (m.first == key) return &m.second;
    return nullptr;
}

const Value* Value::Get(std::string_view key) const {
    for (auto& m : members)
        if (m.first == key) return &m.second;
    return nullptr;
}

void Value::Set(const std::string& key, Value v) {
    if (Value* existing = Get(key)) *existing = std::move(v);
    else members.emplace_back(key, std::move(v));
}

namespace {

class Parser {
public:
    Parser(const std::string& s, size_t pos) : s_(s), i_(pos) {}

    bool Run(Value& out) { return ParseValue(out, 0); }
    size_t Pos() const { return i_; }
    const std::string& Error() const { return err_; }

private:
    bool Fail(const char* what) {
        if (err_.empty()) err_ = what;
        return false;
    }
    void Ws() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) ++i_;
    }
    bool Literal(const char* lit) {
        size_t n = strlen(lit);
        if (s_.compare(i_, n, lit) != 0) return Fail("invalid literal");
        i_ += n;
        return true;
    }

    bool ParseValue(Value& out, int depth) {
        if (depth > 256) return Fail("nesting too deep");
        Ws();
        if (i_ >= s_.size()) return Fail("unexpected end of data");
        char c = s_[i_];
        switch (c) {
        case '{': return ParseObject(out, depth);
        case '[': return ParseArray(out, depth);
        case '"': out = Value(); out.type = Value::Type::String; return ParseString(out.text);
        case 't': out = Value::Bool(true); return Literal("true");
        case 'f': out = Value::Bool(false); return Literal("false");
        case 'n': out = Value(); return Literal("null");
        default:
            if (c == '-' || (c >= '0' && c <= '9')) return ParseNumber(out);
            return Fail("unexpected character");
        }
    }

    bool ParseObject(Value& out, int depth) {
        out = Value::Object();
        ++i_; // {
        Ws();
        if (i_ < s_.size() && s_[i_] == '}') { ++i_; return true; }
        for (;;) {
            Ws();
            if (i_ >= s_.size() || s_[i_] != '"') return Fail("expected a \"key\" in object");
            std::string key;
            if (!ParseString(key)) return false;
            Ws();
            if (i_ >= s_.size() || s_[i_] != ':') return Fail("expected ':' after key");
            ++i_;
            Value v;
            if (!ParseValue(v, depth + 1)) return false;
            out.members.emplace_back(std::move(key), std::move(v));
            Ws();
            if (i_ >= s_.size()) return Fail("unexpected end of data in object");
            if (s_[i_] == ',') { ++i_; continue; }
            if (s_[i_] == '}') { ++i_; return true; }
            return Fail("expected ',' or '}' in object");
        }
    }

    bool ParseArray(Value& out, int depth) {
        out = Value::Array();
        ++i_; // [
        Ws();
        if (i_ < s_.size() && s_[i_] == ']') { ++i_; return true; }
        for (;;) {
            Value v;
            if (!ParseValue(v, depth + 1)) return false;
            out.items.push_back(std::move(v));
            Ws();
            if (i_ >= s_.size()) return Fail("unexpected end of data in array");
            if (s_[i_] == ',') { ++i_; continue; }
            if (s_[i_] == ']') { ++i_; return true; }
            return Fail("expected ',' or ']' in array");
        }
    }

    static void AppendUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out += (char)cp;
        } else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }

    bool Hex4(unsigned& v) {
        if (i_ + 4 > s_.size()) return Fail("truncated \\u escape");
        v = 0;
        for (int k = 0; k < 4; ++k) {
            char h = s_[i_++];
            v <<= 4;
            if (h >= '0' && h <= '9') v |= h - '0';
            else if (h >= 'a' && h <= 'f') v |= h - 'a' + 10;
            else if (h >= 'A' && h <= 'F') v |= h - 'A' + 10;
            else return Fail("invalid hex digit in \\u escape");
        }
        return true;
    }

    bool ParseString(std::string& out) {
        out.clear();
        ++i_; // opening quote
        for (;;) {
            if (i_ >= s_.size()) return Fail("unterminated string");
            unsigned char c = (unsigned char)s_[i_++];
            if (c == '"') return true;
            if (c < 0x20) return Fail("control character in string");
            if (c != '\\') { out += (char)c; continue; }
            if (i_ >= s_.size()) return Fail("unterminated escape");
            char e = s_[i_++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned cp;
                if (!Hex4(cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    unsigned lo = 0;
                    if (i_ + 1 < s_.size() && s_[i_] == '\\' && s_[i_ + 1] == 'u') {
                        size_t save = i_;
                        i_ += 2;
                        if (!Hex4(lo)) return false;
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else {
                            i_ = save;      // not a pair; re-read the next escape on its own
                            cp = 0xFFFD;
                        }
                    } else {
                        cp = 0xFFFD; // lone high surrogate
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    cp = 0xFFFD; // lone low surrogate
                }
                AppendUtf8(out, cp);
                break;
            }
            default: return Fail("invalid escape in string");
            }
        }
    }

    bool ParseNumber(Value& out) {
        size_t start = i_;
        auto digit = [&](size_t k) { return k < s_.size() && s_[k] >= '0' && s_[k] <= '9'; };
        if (s_[i_] == '-') ++i_;
        if (!digit(i_)) return Fail("invalid number");
        if (s_[i_] == '0') ++i_;
        else while (digit(i_)) ++i_;
        if (i_ < s_.size() && s_[i_] == '.') {
            ++i_;
            if (!digit(i_)) return Fail("invalid number");
            while (digit(i_)) ++i_;
        }
        if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
            ++i_;
            if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) ++i_;
            if (!digit(i_)) return Fail("invalid number");
            while (digit(i_)) ++i_;
        }
        out = Value();
        out.type = Value::Type::Number;
        out.text = s_.substr(start, i_ - start);
        return true;
    }

    const std::string& s_;
    size_t i_;
    std::string err_;
};

void WriteString(std::string& o, const std::string& s) {
    static const char* hex = "0123456789abcdef";
    o += '"';
    for (size_t k = 0; k < s.size(); ++k) {
        unsigned char c = (unsigned char)s[k];
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\b': o += "\\b"; break;
        case '\f': o += "\\f"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) {
                o += "\\u00";
                o += hex[c >> 4];
                o += hex[c & 15];
            } else if (c == 0xE2 && k + 2 < s.size() && (unsigned char)s[k + 1] == 0x80 &&
                       ((unsigned char)s[k + 2] == 0xA8 || (unsigned char)s[k + 2] == 0xA9)) {
                // U+2028 / U+2029: valid JSON but historically not valid in JS string literals.
                o += (unsigned char)s[k + 2] == 0xA8 ? "\\u2028" : "\\u2029";
                k += 2;
            } else {
                o += (char)c;
            }
        }
    }
    o += '"';
}

bool IsFlat(const Value& v) {
    for (auto& k : v.items) if (!k.IsScalar()) return false;
    for (auto& m : v.members) if (!m.second.IsScalar()) return false;
    return true;
}

void WriteValue(std::string& o, const Value& v, int indent, bool inArray) {
    switch (v.type) {
    case Value::Type::Null: o += "null"; return;
    case Value::Type::Bool: o += v.boolean ? "true" : "false"; return;
    case Value::Type::Number: o += v.text; return;
    case Value::Type::String: WriteString(o, v.text); return;
    case Value::Type::Array: {
        if (v.items.empty()) { o += "[]"; return; }
        if (inArray && IsFlat(v)) {
            o += '[';
            for (size_t k = 0; k < v.items.size(); ++k) {
                if (k) o += ", ";
                WriteValue(o, v.items[k], indent, true);
            }
            o += ']';
            return;
        }
        o += "[\n";
        for (size_t k = 0; k < v.items.size(); ++k) {
            o.append(indent + 2, ' ');
            WriteValue(o, v.items[k], indent + 2, true);
            if (k + 1 < v.items.size()) o += ',';
            o += '\n';
        }
        o.append(indent, ' ');
        o += ']';
        return;
    }
    case Value::Type::Object: {
        if (v.members.empty()) { o += "{}"; return; }
        if (inArray && IsFlat(v)) {
            o += "{ ";
            for (size_t k = 0; k < v.members.size(); ++k) {
                if (k) o += ", ";
                WriteString(o, v.members[k].first);
                o += ": ";
                WriteValue(o, v.members[k].second, indent, true);
            }
            o += " }";
            return;
        }
        o += "{\n";
        for (size_t k = 0; k < v.members.size(); ++k) {
            o.append(indent + 2, ' ');
            WriteString(o, v.members[k].first);
            o += ": ";
            WriteValue(o, v.members[k].second, indent + 2, false);
            if (k + 1 < v.members.size()) o += ',';
            o += '\n';
        }
        o.append(indent, ' ');
        o += '}';
        return;
    }
    }
}

} // namespace

bool Parse(const std::string& src, size_t& pos, Value& out, std::string& error) {
    Parser p(src, pos);
    if (!p.Run(out)) {
        error = p.Error();
        pos = p.Pos();
        return false;
    }
    pos = p.Pos();
    return true;
}

std::string Serialize(const Value& v) {
    std::string o;
    WriteValue(o, v, 0, false);
    return o;
}

} // namespace json

// ===========================================================================
// SiteData
// ===========================================================================
static const char kMarker[] = "window.SITE_DATA";

static bool ReadWholeFile(const std::wstring& path, std::string& out, std::wstring& error) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        error = L"Couldn't open " + path + L":\n" + SystemMessage(GetLastError());
        return false;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    if (size.QuadPart > 64 * 1024 * 1024) {
        CloseHandle(h);
        error = path + L" is unexpectedly large.";
        return false;
    }
    out.resize((size_t)size.QuadPart);
    DWORD read = 0;
    BOOL ok = out.empty() || ReadFile(h, out.data(), (DWORD)out.size(), &read, nullptr);
    DWORD gle = GetLastError();
    CloseHandle(h);
    if (!ok) {
        error = L"Couldn't read " + path + L":\n" + SystemMessage(gle);
        return false;
    }
    out.resize(read);
    return true;
}

static std::wstring LineCol(const std::string& text, size_t pos) {
    size_t line = 1, col = 1;
    for (size_t k = 0; k < pos && k < text.size(); ++k) {
        if (text[k] == '\n') { ++line; col = 1; }
        else ++col;
    }
    return L"line " + std::to_wstring(line) + L", column " + std::to_wstring(col);
}

// Parses a whole data/site-data.js text ("<prefix>window.SITE_DATA = {…};<suffix>").
static bool ParseSiteText(const std::string& text, json::Value& root, std::string& prefix, std::string& suffix,
                          std::wstring& error) {
    size_t bom = (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
                  (unsigned char)text[2] == 0xBF) ? 3 : 0;

    size_t at = text.find(kMarker, bom);
    if (at == std::string::npos) {
        error = L"data\\site-data.js doesn't contain \"window.SITE_DATA = …;\". It may be damaged.";
        return false;
    }
    size_t p = at + strlen(kMarker);
    while (p < text.size() && isspace((unsigned char)text[p])) ++p;
    if (p >= text.size() || text[p] != '=') {
        error = L"data\\site-data.js: expected '=' after window.SITE_DATA (" + LineCol(text, p) + L").";
        return false;
    }
    ++p;

    std::string perr;
    if (!json::Parse(text, p, root, perr)) {
        error = L"data\\site-data.js has a formatting error at " + LineCol(text, p) + L": " + FromUtf8(perr) +
                L".\nFix the file (or restore it) and try again.";
        return false;
    }
    if (root.type != json::Value::Type::Object) {
        error = L"data\\site-data.js: SITE_DATA must be a { … } object.";
        return false;
    }
    while (p < text.size() && isspace((unsigned char)text[p])) ++p;
    if (p >= text.size() || text[p] != ';') {
        error = L"data\\site-data.js: expected ';' after the data (" + LineCol(text, p) + L").";
        return false;
    }
    ++p;

    if (const json::Value* g = root.Get("gallery")) {
        if (g->type != json::Value::Type::Array) {
            error = L"data\\site-data.js: \"gallery\" must be a list [ … ].";
            return false;
        }
        for (auto& item : g->items) {
            if (item.type != json::Value::Type::Object) {
                error = L"data\\site-data.js: every gallery entry must be an object like { \"name\": …, \"file\": … }.";
                return false;
            }
        }
    }
    if (const json::Value* c = root.Get("commissionsOpen")) {
        if (c->type != json::Value::Type::Bool) {
            error = L"data\\site-data.js: \"commissionsOpen\" must be true or false.";
            return false;
        }
    }

    prefix = text.substr(bom, at - bom);
    suffix = text.substr(p);
    return true;
}

bool SiteData::Load(const std::wstring& repo, std::wstring& error) {
    repo_ = repo;
    while (!repo_.empty() && (repo_.back() == L'\\' || repo_.back() == L'/')) repo_.pop_back();
    dataPath_ = RepoPath(repo_, L"data/site-data.js");

    if (!DirExists(repo_)) {
        error = L"The website folder doesn't exist:\n" + repo_ + L"\n\nUse Browse… to choose your website folder.";
        return false;
    }
    if (!FileExists(dataPath_)) {
        error = L"Couldn't find data\\site-data.js in this folder:\n" + repo_ +
                L"\n\nThis doesn't look like your website folder. Use Browse… to choose the folder that "
                L"contains index.html and the data folder.";
        return false;
    }

    std::string text;
    if (!ReadWholeFile(dataPath_, text, error)) return false;
    json::Value root;
    std::string prefix, suffix;
    if (!ParseSiteText(text, root, prefix, suffix, error)) return false;
    prefix_ = std::move(prefix);
    suffix_ = std::move(suffix);
    root_ = std::move(root);
    return true;
}

std::string SiteData::ToText() const {
    std::string out = prefix_;
    out += kMarker;
    out += " = ";
    out += json::Serialize(root_);
    out += ';';
    out += suffix_.empty() ? std::string("\n") : suffix_;
    return out;
}

// Atomically replaces `path` with `text` (temp file + rename).
static bool WriteWholeFile(const std::wstring& path, const std::string& text, std::wstring& error) {
    const std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        error = L"Couldn't write " + tmp + L":\n" + SystemMessage(GetLastError());
        return false;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(h, text.data(), (DWORD)text.size(), &written, nullptr) && written == text.size();
    DWORD gle = GetLastError();
    if (ok) ok = FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        error = L"Couldn't write " + tmp + L":\n" + SystemMessage(gle);
        return false;
    }
    // Replace atomically; retry briefly in case an editor/antivirus holds the file.
    for (int attempt = 0;; ++attempt) {
        if (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
        gle = GetLastError();
        if (attempt >= 10) break;
        Sleep(100);
    }
    DeleteFileW(tmp.c_str());
    error = L"Couldn't replace " + path + L":\n" + SystemMessage(gle) + L"\n\nIs the file open in another program?";
    return false;
}

bool SiteData::Save(std::wstring& error, const std::wstring& version) {
    SetVersion(version.empty() ? NewVersionStamp() : version);
    return WriteWholeFile(dataPath_, ToText(), error);
}

bool SiteData::CommissionsOpen() const {
    const json::Value* c = root_.Get("commissionsOpen");
    return c && c->type == json::Value::Type::Bool && c->boolean;
}

void SiteData::SetCommissionsOpen(bool open) {
    if (json::Value* c = root_.Get("commissionsOpen")) *c = json::Value::Bool(open);
    else root_.members.insert(root_.members.begin() + (root_.Get("version") ? 1 : 0),
                              json::Member("commissionsOpen", json::Value::Bool(open)));
}

std::wstring SiteData::Version() const {
    const json::Value* v = root_.Get("version");
    return v && v->type == json::Value::Type::String ? FromUtf8(v->text) : std::wstring();
}

void SiteData::SetVersion(const std::wstring& version) {
    auto& m = root_.members;
    m.erase(std::remove_if(m.begin(), m.end(), [](const json::Member& x) { return x.first == "version"; }), m.end());
    m.insert(m.begin(), json::Member("version", json::Value::String(ToUtf8(version))));
}

// ---------------------------------------------------------------------------
// Version stamps
// ---------------------------------------------------------------------------
std::wstring FormatVersionStamp(const SYSTEMTIME& utc) {
    wchar_t buf[32];
    swprintf_s(buf, L"%04u%02u%02uT%02u%02u%02uZ", utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond);
    return buf;
}

std::wstring NewVersionStamp() {
    SYSTEMTIME st;
    GetSystemTime(&st);
    return FormatVersionStamp(st);
}

static bool IsVersionStamp(const std::wstring& v) {
    if (v.size() != 16 || v[8] != L'T' || v[15] != L'Z') return false;
    for (size_t i = 0; i < 15; ++i)
        if (i != 8 && (v[i] < L'0' || v[i] > L'9')) return false;
    return true;
}

std::wstring ExtractSiteVersion(const std::string& jsText) {
    json::Value root;
    std::string prefix, suffix;
    std::wstring error;
    if (ParseSiteText(jsText, root, prefix, suffix, error)) {
        const json::Value* v = root.Get("version");
        return v && v->type == json::Value::Type::String ? FromUtf8(v->text) : std::wstring();
    }
    // Not strict site data (e.g. edited by hand): look for "version": "…" directly.
    for (size_t at = jsText.find("\"version\""); at != std::string::npos; at = jsText.find("\"version\"", at + 1)) {
        size_t p = at + 9;
        while (p < jsText.size() && isspace((unsigned char)jsText[p])) ++p;
        if (p >= jsText.size() || jsText[p] != ':') continue;
        ++p;
        json::Value v;
        std::string perr;
        if (json::Parse(jsText, p, v, perr) && v.type == json::Value::Type::String) return FromUtf8(v.text);
    }
    return {};
}

bool SameExceptVersion(const std::string& a, const std::string& b) {
    json::Value ra, rb;
    std::string pa, sa, pb, sb;
    std::wstring error;
    if (!ParseSiteText(a, ra, pa, sa, error) || !ParseSiteText(b, rb, pb, sb, error)) return false;
    auto strip = [](json::Value& root, std::string& prefix, std::string& suffix) {
        auto& m = root.members;
        m.erase(std::remove_if(m.begin(), m.end(), [](const json::Member& x) { return x.first == "version"; }), m.end());
        for (std::string* s : {&prefix, &suffix}) s->erase(std::remove(s->begin(), s->end(), '\r'), s->end());
    };
    strip(ra, pa, sa);
    strip(rb, pb, sb);
    return pa == pb && sa == sb && json::Serialize(ra) == json::Serialize(rb);
}

namespace {

// A top-level "version" member line as written by json::Serialize ("  \"version\": …").
bool IsVersionLine(const std::string& line) { return line.compare(0, 12, "  \"version\":") == 0; }

std::vector<std::string> WithoutVersionLines(const std::vector<std::string>& lines) {
    std::vector<std::string> out;
    for (auto& l : lines)
        if (!IsVersionLine(l)) out.push_back(l);
    return out;
}

// One conflict: picks the result when the sides only clash on the version line. Lines keep their '\n'.
bool ResolveHunk(const std::vector<std::string>& ours, const std::vector<std::string>* base,
                 const std::vector<std::string>& theirs, std::string& out) {
    auto hasVersion = [](const std::vector<std::string>& v) { return std::any_of(v.begin(), v.end(), IsVersionLine); };
    if (!hasVersion(ours) && !hasVersion(theirs)) return false; // a real conflict elsewhere in the file
    const auto o = WithoutVersionLines(ours), t = WithoutVersionLines(theirs);
    auto emit = [&out](const std::vector<std::string>& v) {
        for (auto& l : v) out += l;
    };
    if (o == t) return emit(ours), true;
    if (!base) return false;
    const auto b = WithoutVersionLines(*base);
    if (t == b) return emit(ours), true;   // only we changed something besides the version
    if (o == b) return emit(theirs), true; // only they did
    if (o.size() != b.size() || t.size() != b.size()) return false;
    // Both changed different lines: take each changed line from the side that changed it.
    std::vector<std::string> merged;
    for (size_t i = 0; i < b.size(); ++i) {
        if (o[i] == t[i] || t[i] == b[i]) merged.push_back(o[i]);
        else if (o[i] == b[i]) merged.push_back(t[i]);
        else return false;
    }
    size_t k = 0;
    for (auto& l : ours) out += IsVersionLine(l) ? l : merged[k++];
    return true;
}

} // namespace

bool ResolveVersionConflicts(const std::string& text, std::string& out) {
    std::vector<std::string> lines;
    for (size_t b = 0; b < text.size();) {
        size_t e = text.find('\n', b);
        e = e == std::string::npos ? text.size() : e + 1;
        lines.push_back(text.substr(b, e - b));
        b = e;
    }
    auto marker = [](const std::string& l, const char* m) {
        return l.compare(0, 7, m) == 0 && (l.size() == 7 || l[7] == ' ' || l[7] == '\r' || l[7] == '\n');
    };
    std::string result;
    bool any = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (!marker(lines[i], "<<<<<<<")) {
            if (marker(lines[i], "=======") || marker(lines[i], ">>>>>>>") || marker(lines[i], "|||||||")) return false;
            result += lines[i];
            continue;
        }
        std::vector<std::string> ours, base, theirs;
        bool hasBase = false;
        std::vector<std::string>* cur = &ours;
        size_t j = i + 1;
        for (; j < lines.size() && !marker(lines[j], ">>>>>>>"); ++j) {
            if (marker(lines[j], "|||||||") && cur == &ours) { cur = &base; hasBase = true; }
            else if (marker(lines[j], "=======") && cur != &theirs) cur = &theirs;
            else if (marker(lines[j], "<<<<<<<")) return false;
            else cur->push_back(lines[j]);
        }
        if (j >= lines.size() || cur != &theirs) return false; // unterminated
        if (!ResolveHunk(ours, hasBase ? &base : nullptr, theirs, result)) return false;
        any = true;
        i = j;
    }
    if (!any) return false;
    out = std::move(result);
    return true;
}

json::Value& SiteData::Gallery() {
    if (json::Value* g = root_.Get("gallery")) return *g;
    root_.Set("gallery", json::Value::Array());
    return *root_.Get("gallery");
}

const json::Value* SiteData::GalleryConst() const { return root_.Get("gallery"); }

size_t SiteData::Count() const {
    const json::Value* g = GalleryConst();
    return g ? g->items.size() : 0;
}

GalleryItem SiteData::Item(size_t i) const {
    GalleryItem it;
    const json::Value& v = GalleryConst()->items.at(i);
    if (const json::Value* n = v.Get("name"); n && n->type == json::Value::Type::String) it.name = FromUtf8(n->text);
    if (const json::Value* f = v.Get("file"); f && f->type == json::Value::Type::String) it.file = FromUtf8(f->text);
    return it;
}

std::vector<std::wstring> SiteData::Files() const {
    std::vector<std::wstring> out;
    for (size_t i = 0; i < Count(); ++i) out.push_back(Item(i).file);
    return out;
}

void SiteData::Add(const std::wstring& name, const std::wstring& file) {
    json::Value item = json::Value::Object();
    item.Set("name", json::Value::String(ToUtf8(name)));
    item.Set("file", json::Value::String(ToUtf8(file)));
    Gallery().items.push_back(std::move(item));
}

void SiteData::Rename(size_t i, const std::wstring& name) {
    Gallery().items.at(i).Set("name", json::Value::String(ToUtf8(name)));
}

void SiteData::Remove(size_t i) {
    auto& items = Gallery().items;
    items.erase(items.begin() + (ptrdiff_t)i);
}

void SiteData::Move(size_t from, size_t to) {
    auto& items = Gallery().items;
    if (from >= items.size() || to >= items.size() || from == to) return;
    json::Value v = std::move(items[from]);
    items.erase(items.begin() + (ptrdiff_t)from);
    items.insert(items.begin() + (ptrdiff_t)to, std::move(v));
}

// ===========================================================================
// Images (WIC)
// ===========================================================================
std::wstring Slugify(const std::wstring& name) {
    std::wstring out;
    bool dash = false;
    for (wchar_t c : name) {
        if (c >= L'A' && c <= L'Z') c = c - L'A' + L'a';
        if ((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9')) {
            if (dash && !out.empty()) out += L'-';
            dash = false;
            out += c;
        } else {
            dash = true;
        }
        if (out.size() >= 60) break;
    }
    return out.empty() ? L"image" : out;
}

bool IsSupportedImageExt(const std::wstring& path) {
    std::wstring e = ExtOf(path);
    return e == L"png" || e == L"jpg" || e == L"jpeg" || e == L"webp" || e == L"bmp";
}

namespace {

template <class T>
class Com {
public:
    Com() = default;
    ~Com() { Reset(); }
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    T** Put() { Reset(); return &p_; }
    void Reset() { if (p_) { p_->Release(); p_ = nullptr; } }
    void Assign(T* p) { if (p) p->AddRef(); Reset(); p_ = p; }
    T* Get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
private:
    T* p_ = nullptr;
};

bool CreateFactory(Com<IWICImagingFactory>& f) {
    return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory,
                                      reinterpret_cast<void**>(f.Put())));
}

UINT ReadOrientation(IWICBitmapFrameDecode* frame) {
    Com<IWICMetadataQueryReader> q;
    if (FAILED(frame->GetMetadataQueryReader(q.Put())) || !q) return 1;
    for (const wchar_t* query : {L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}"}) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(q->GetMetadataByName(query, &v))) {
            UINT o = v.vt == VT_UI2 ? v.uiVal : 1;
            PropVariantClear(&v);
            return o;
        }
    }
    return 1;
}

// Decodes frame 0, applies EXIF orientation, scales to fit maxW x maxH (never upscales)
// and returns 32bpp BGRA pixels (premultiplied if requested).
bool Decode(IWICImagingFactory* f, const std::wstring& path, UINT maxW, UINT maxH, bool premultiplied, UINT& w,
            UINT& h, std::vector<BYTE>& px) {
    Com<IWICBitmapDecoder> dec;
    if (FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                            dec.Put())))
        return false;
    Com<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, frame.Put()))) return false;

    Com<IWICBitmapSource> src;
    src.Assign(frame.Get());

    WICBitmapTransformOptions xf = WICBitmapTransformRotate0;
    switch (ReadOrientation(frame.Get())) {
    case 2: xf = WICBitmapTransformFlipHorizontal; break;
    case 3: xf = WICBitmapTransformRotate180; break;
    case 4: xf = WICBitmapTransformFlipVertical; break;
    case 6: xf = WICBitmapTransformRotate90; break;
    case 8: xf = WICBitmapTransformRotate270; break;
    default: break;
    }
    Com<IWICBitmapFlipRotator> rot;
    if (xf != WICBitmapTransformRotate0 && SUCCEEDED(f->CreateBitmapFlipRotator(rot.Put())) &&
        SUCCEEDED(rot->Initialize(src.Get(), xf)))
        src.Assign(rot.Get());

    // Scale in premultiplied space to avoid dark fringes around transparent edges.
    Com<IWICFormatConverter> pre;
    if (FAILED(f->CreateFormatConverter(pre.Put())) ||
        FAILED(pre->Initialize(src.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                               WICBitmapPaletteTypeCustom)))
        return false;
    src.Assign(pre.Get());

    if (FAILED(src->GetSize(&w, &h)) || w == 0 || h == 0) return false;
    Com<IWICBitmapScaler> scaler;
    if (w > maxW || h > maxH) {
        double s = std::min((double)maxW / w, (double)maxH / h);
        UINT nw = std::max<UINT>(1, (UINT)(w * s + 0.5)), nh = std::max<UINT>(1, (UINT)(h * s + 0.5));
        if (FAILED(f->CreateBitmapScaler(scaler.Put()))) return false;
        // WICBitmapInterpolationModeHighQualityCubic (= 4, Windows 10+); spelled numerically because
        // MinGW headers lack the name. Falls back to Fant on older systems.
        const auto highQualityCubic = static_cast<WICBitmapInterpolationMode>(4);
        if (FAILED(scaler->Initialize(src.Get(), nw, nh, highQualityCubic)) &&
            FAILED(scaler->Initialize(src.Get(), nw, nh, WICBitmapInterpolationModeFant)))
            return false;
        src.Assign(scaler.Get());
        w = nw;
        h = nh;
    }

    Com<IWICFormatConverter> straight;
    if (!premultiplied) {
        if (FAILED(f->CreateFormatConverter(straight.Put())) ||
            FAILED(straight->Initialize(src.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr,
                                        0.0, WICBitmapPaletteTypeCustom)))
            return false;
        src.Assign(straight.Get());
    }
    px.resize((size_t)w * h * 4);
    return SUCCEEDED(src->CopyPixels(nullptr, w * 4, (UINT)px.size(), px.data()));
}

bool Encode(IWICImagingFactory* f, const std::wstring& out, bool png, UINT w, UINT h, const std::vector<BYTE>& bgra) {
    std::vector<BYTE> buf;
    UINT stride;
    WICPixelFormatGUID fmt;
    if (png) {
        stride = w * 4;
        fmt = GUID_WICPixelFormat32bppBGRA;
        buf = bgra;
    } else {
        stride = (w * 3 + 3) & ~3u;
        fmt = GUID_WICPixelFormat24bppBGR;
        buf.assign((size_t)stride * h, 0);
        for (UINT y = 0; y < h; ++y) {
            const BYTE* s = &bgra[(size_t)y * w * 4];
            BYTE* d = &buf[(size_t)y * stride];
            for (UINT x = 0; x < w; ++x, s += 4, d += 3) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; }
        }
    }

    Com<IWICStream> stream;
    if (FAILED(f->CreateStream(stream.Put())) || FAILED(stream->InitializeFromFilename(out.c_str(), GENERIC_WRITE)))
        return false;
    Com<IWICBitmapEncoder> enc;
    if (FAILED(f->CreateEncoder(png ? GUID_ContainerFormatPng : GUID_ContainerFormatJpeg, nullptr, enc.Put())) ||
        FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)))
        return false;
    Com<IWICBitmapFrameEncode> frame;
    Com<IPropertyBag2> props;
    if (FAILED(enc->CreateNewFrame(frame.Put(), props.Put()))) return false;
    if (!png && props) {
        PROPBAG2 opt = {};
        opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = kJpegQuality;
        props->Write(1, &opt, &v);
    }
    if (FAILED(frame->Initialize(props.Get())) || FAILED(frame->SetSize(w, h))) return false;
    WICPixelFormatGUID want = fmt;
    if (FAILED(frame->SetPixelFormat(&fmt)) || !IsEqualGUID(fmt, want)) return false;
    if (FAILED(frame->WritePixels(h, stride, (UINT)buf.size(), buf.data()))) return false;
    return SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
}

bool EnsureDir(const std::wstring& dir) {
    if (DirExists(dir)) return true;
    size_t slash = dir.find_last_of(L'\\');
    if (slash != std::wstring::npos && slash > 2 && !EnsureDir(dir.substr(0, slash))) return false;
    return CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

} // namespace

bool ImportImage(const std::wstring& repo, const std::wstring& source, const std::wstring& displayName,
                 const std::vector<std::wstring>& takenFiles, std::wstring& relPathOut, std::wstring& error) {
    if (!FileExists(source)) {
        error = L"The image file doesn't exist:\n" + source;
        return false;
    }
    if (!IsSupportedImageExt(source)) {
        error = L"Please choose a .png, .jpg, .jpeg, .webp or .bmp image.";
        return false;
    }
    const std::wstring galleryDir = RepoPath(repo, L"assets/gallery");
    if (!EnsureDir(galleryDir)) {
        error = L"Couldn't create the folder " + galleryDir + L":\n" + SystemMessage(GetLastError());
        return false;
    }

    Com<IWICImagingFactory> f;
    if (!CreateFactory(f)) {
        error = L"Windows Imaging Component is not available.";
        return false;
    }
    UINT w = 0, h = 0;
    std::vector<BYTE> px;
    bool decoded = Decode(f.Get(), source, kMaxImageSide, kMaxImageSide, false, w, h, px);
    std::wstring ext;
    bool png = false;
    if (decoded) {
        for (size_t k = 3; k < px.size(); k += 4)
            if (px[k] != 255) { png = true; break; }
        ext = png ? L"png" : L"jpg";
    } else {
        // No decoder (e.g. WebP codec missing): web-ready formats can still be copied as-is.
        ext = ExtOf(source);
        if (ext == L"bmp") {
            error = L"Windows couldn't read this image:\n" + source;
            return false;
        }
    }

    // Unique slug: not used by any existing file (any image extension) or gallery entry.
    std::vector<std::wstring> taken;
    for (auto& t : takenFiles) taken.push_back(LowerAscii(t));
    const std::wstring base = Slugify(displayName);
    std::wstring slug = base;
    for (int n = 2;; ++n) {
        bool clash = false;
        for (const wchar_t* e : {L"jpg", L"jpeg", L"png", L"webp", L"bmp", L"gif", L"tmp"}) {
            std::wstring rel = L"assets/gallery/" + slug + L"." + e;
            if (FileExists(RepoPath(repo, rel)) || std::find(taken.begin(), taken.end(), rel) != taken.end()) {
                clash = true;
                break;
            }
        }
        if (!clash) break;
        slug = base + L"-" + std::to_wstring(n);
    }

    const std::wstring rel = L"assets/gallery/" + slug + L"." + ext;
    const std::wstring dest = RepoPath(repo, rel);
    const std::wstring tmp = RepoPath(repo, L"assets/gallery/" + slug + L".tmp");

    bool ok = decoded ? Encode(f.Get(), tmp, png, w, h, px) : CopyFileW(source.c_str(), tmp.c_str(), TRUE) != FALSE;
    if (ok) ok = MoveFileExW(tmp.c_str(), dest.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!ok) {
        DeleteFileW(tmp.c_str());
        error = L"Couldn't save the image to " + dest;
        return false;
    }
    relPathOut = rel;
    return true;
}

HBITMAP LoadThumbnail(const std::wstring& path, UINT maxW, UINT maxH, UINT& w, UINT& h) {
    Com<IWICImagingFactory> f;
    std::vector<BYTE> px;
    if (maxW == 0 || maxH == 0 || !CreateFactory(f) || !Decode(f.Get(), path, maxW, maxH, true, w, h, px))
        return nullptr;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = (LONG)w;
    bi.bmiHeader.biHeight = -(LONG)h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp && bits) memcpy(bits, px.data(), px.size());
    return bmp;
}

// ===========================================================================
// Git & GitHub
// ===========================================================================
namespace {

constexpr DWORD kLocalTimeoutMs = 5 * 60 * 1000;
constexpr DWORD kNetworkTimeoutMs = 10 * 60 * 1000;
constexpr DWORD kCredentialTimeoutMs = 60 * 1000;
constexpr DWORD kLoginTimeoutMs = 15 * 60 * 1000;

const wchar_t kNoSiteOnGitHub[] =
    L"GitHub repo has no site files yet \u2014 click \u201cPush changes\u201d to upload this folder.";

// Quotes one argument per the MSVCRT/CommandLineToArgvW rules.
std::wstring QuoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring q = L"\"";
    size_t slashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'"') { q.append(slashes * 2 + 1, L'\\'); q += L'"'; }
        else { q.append(slashes, L'\\'); q += c; }
        slashes = 0;
    }
    q.append(slashes * 2, L'\\');
    q += L'"';
    return q;
}

struct RunOptions {
    bool echo = true;                   // log the command and its output
    bool interactive = false;           // allow Git Credential Manager's sign-in UI (Sign-in button only)
    const std::string* input = nullptr; // written to stdin; otherwise stdin is NUL
    DWORD timeoutMs = kLocalTimeoutMs;  // the whole process tree is killed after this long
    bool cancellable = false;           // CancelSignIn() can stop it
};

struct RunResult {
    bool started = false;
    bool killed = false;    // stopped because it took too long
    bool cancelled = false; // stopped by CancelSignIn()
    DWORD exitCode = 1;
    std::string output; // stdout + stderr
};

struct Watchdog {
    HANDLE job = nullptr;
    HANDLE process = nullptr;
    std::atomic<bool> fired{false};
    std::atomic<bool> cancelled{false};
};

VOID CALLBACK StopProcessTree(PVOID p, BOOLEAN) {
    auto* w = static_cast<Watchdog*>(p);
    w->fired = true;
    if (!w->job || !TerminateJobObject(w->job, 1)) TerminateProcess(w->process, 1);
}

std::mutex g_cancelMutex;
Watchdog* g_cancelWatch = nullptr; // the running cancellable command, if any

// The environment for git: never wait for a prompt nobody can see. Git Credential Manager may only show
// its sign-in window when `interactive` (the explicit Sign-in button); everything else fails fast instead.
std::wstring GitEnvironment(bool interactive) {
    const std::pair<const wchar_t*, const wchar_t*> overrides[] = {
        {L"GIT_TERMINAL_PROMPT", L"0"},
        {L"GCM_INTERACTIVE", interactive ? L"auto" : L"never"},
        {L"GIT_EDITOR", L":"}, // ":" = never open an editor
        {L"GIT_SEQUENCE_EDITOR", L":"},
        {L"GIT_MERGE_AUTOEDIT", L"no"},
        {L"LANGUAGE", L"en"}, // messages are matched in English below
        {L"GIT_ASKPASS", nullptr},
        {L"SSH_ASKPASS", nullptr},
    };
    std::wstring block;
    if (wchar_t* env = GetEnvironmentStringsW()) {
        for (const wchar_t* p = env; *p; p += wcslen(p) + 1) {
            std::wstring entry = p;
            std::wstring name = LowerAscii(entry.substr(0, entry.find(L'=', 1)));
            bool replaced = false;
            for (auto& o : overrides) replaced = replaced || name == LowerAscii(o.first);
            if (!replaced) block.append(entry).push_back(L'\0');
        }
        FreeEnvironmentStringsW(env);
    }
    for (auto& o : overrides)
        if (o.second) block.append(o.first).append(L"=").append(o.second).push_back(L'\0');
    block.push_back(L'\0');
    return block;
}

RunResult RunGit(const std::wstring& dir, const std::vector<std::wstring>& args, const LogFn& log, const RunOptions& opt) {
    RunResult r;
    std::wstring cmd = L"git";
    for (auto& a : args) cmd += L" " + QuoteArg(a);
    if (opt.echo && log) log(L"> " + cmd);

    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE outRd = nullptr, outWr = nullptr, inRd = nullptr, inWr = nullptr;
    if (!CreatePipe(&outRd, &outWr, &sa, 0)) return r;
    SetHandleInformation(outRd, HANDLE_FLAG_INHERIT, 0);
    if (opt.input) {
        if (CreatePipe(&inRd, &inWr, &sa, 0)) SetHandleInformation(inWr, HANDLE_FLAG_INHERIT, 0);
        else inRd = inWr = nullptr;
    }
    if (!inRd) {
        inRd = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
        if (inRd == INVALID_HANDLE_VALUE) inRd = nullptr;
    }
    // A job object lets a timeout or Cancel stop git *and* anything it started (credential helpers, ssh…).
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li));
    }

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = inRd;
    si.hStdOutput = outWr;
    si.hStdError = outWr;
    PROCESS_INFORMATION pi = {};
    std::wstring env = GitEnvironment(opt.interactive);
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(0);
    r.started = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                               CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, env.data(),
                               dir.empty() ? nullptr : dir.c_str(), &si, &pi) != FALSE;
    CloseHandle(outWr);
    if (inRd) CloseHandle(inRd);
    if (!r.started) {
        CloseHandle(outRd);
        if (inWr) CloseHandle(inWr);
        if (job) CloseHandle(job);
        return r;
    }
    if (job && !AssignProcessToJobObject(job, pi.hProcess)) {
        CloseHandle(job);
        job = nullptr;
    }
    ResumeThread(pi.hThread);

    Watchdog watch;
    watch.job = job;
    watch.process = pi.hProcess;
    HANDLE timer = nullptr;
    if (opt.timeoutMs)
        CreateTimerQueueTimer(&timer, nullptr, StopProcessTree, &watch, opt.timeoutMs, 0, WT_EXECUTEONLYONCE);
    if (opt.cancellable) {
        std::lock_guard<std::mutex> lock(g_cancelMutex);
        g_cancelWatch = &watch;
    }
    if (inWr) {
        DWORD n = 0;
        WriteFile(inWr, opt.input->data(), (DWORD)opt.input->size(), &n, nullptr);
        CloseHandle(inWr);
    }

    std::string pending;
    char chunk[4096];
    DWORD n = 0;
    while (ReadFile(outRd, chunk, sizeof(chunk), &n, nullptr) && n > 0) {
        r.output.append(chunk, n);
        if (!opt.echo || !log) continue;
        pending.append(chunk, n);
        size_t nl;
        while ((nl = pending.find_first_of("\r\n")) != std::string::npos) {
            std::string line = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            if (!line.empty()) log(L"  " + FromUtf8(line));
        }
    }
    if (opt.echo && log && !pending.empty()) log(L"  " + FromUtf8(pending));
    CloseHandle(outRd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    if (timer) DeleteTimerQueueTimer(nullptr, timer, INVALID_HANDLE_VALUE); // waits for a running callback
    if (opt.cancellable) {
        std::lock_guard<std::mutex> lock(g_cancelMutex);
        g_cancelWatch = nullptr;
    }
    GetExitCodeProcess(pi.hProcess, &r.exitCode);
    r.cancelled = watch.cancelled;
    r.killed = watch.fired && !watch.cancelled;
    if (r.killed && opt.echo && log) log(L"  (stopped: git took longer than " + std::to_wstring(opt.timeoutMs / 60000) + L" minutes)");
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (job) CloseHandle(job);
    return r;
}

RunResult RunGit(const std::wstring& dir, const std::vector<std::wstring>& args, const LogFn& log, bool echo = true) {
    RunOptions o;
    o.echo = echo;
    return RunGit(dir, args, log, o);
}

// A command that talks to GitHub. `user` (the signed-in GitHub login) tells the credential helper which
// saved account to use, so several saved accounts never cause an account-picker prompt.
RunResult RunNetwork(const std::wstring& dir, const std::wstring& user, std::vector<std::wstring> args, const LogFn& log) {
    if (!user.empty()) args.insert(args.begin(), {L"-c", L"credential.username=" + user});
    RunOptions o;
    o.timeoutMs = kNetworkTimeoutMs;
    return RunGit(dir, args, log, o);
}

std::string TrimA(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

std::vector<std::wstring> SplitZ(const std::string& out) {
    std::vector<std::wstring> items;
    for (size_t b = 0; b < out.size();) {
        size_t e = out.find('\0', b);
        if (e == std::string::npos) e = out.size();
        std::string item = TrimA(out.substr(b, e - b));
        if (!item.empty()) items.push_back(FromUtf8(item));
        b = e + 1;
    }
    return items;
}

void Wipe(std::string& s) {
    if (!s.empty()) SecureZeroMemory(s.data(), s.size());
    s.clear();
}
void Wipe(std::wstring& s) {
    if (!s.empty()) SecureZeroMemory(s.data(), s.size() * sizeof(wchar_t));
    s.clear();
}

std::wstring NormPath(std::wstring p) {
    std::replace(p.begin(), p.end(), L'/', L'\\');
    wchar_t full[4096], longer[4096];
    if (GetFullPathNameW(p.c_str(), ARRAYSIZE(full), full, nullptr)) p = full;
    if (GetLongPathNameW(p.c_str(), longer, ARRAYSIZE(longer))) p = longer;
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    return LowerAscii(p);
}

const wchar_t kIdentityHelp[] = L"Git doesn't know your name/email yet. Sign in to GitHub (step 1) and try again, or run once "
                                L"in a command prompt:\n  git config --global user.name \"Your Name\"\n"
                                L"  git config --global user.email \"you@example.com\"";

// When git has no user.name/user.email, commits are made as the signed-in GitHub user (GitHub's
// private "noreply" address), so a fresh computer can publish without any extra setup.
std::vector<std::wstring> IdentityArgs(const std::wstring& repo, const std::wstring& githubUser) {
    std::vector<std::wstring> a;
    if (githubUser.empty()) return a;
    const LogFn quiet;
    if (TrimA(RunGit(repo, {L"config", L"user.name"}, quiet, false).output).empty())
        a.insert(a.end(), {L"-c", L"user.name=" + githubUser});
    if (TrimA(RunGit(repo, {L"config", L"user.email"}, quiet, false).output).empty())
        a.insert(a.end(), {L"-c", L"user.email=" + githubUser + L"@users.noreply.github.com"});
    return a;
}

std::vector<std::wstring> Concat(std::vector<std::wstring> a, std::initializer_list<std::wstring> b) {
    a.insert(a.end(), b);
    return a;
}

GitResult CheckBasics(const RepoInfo& info) {
    if (!info.gitAvailable) return {GitOutcome::NoGit, L"Git is not installed (or not on PATH)."};
    if (!info.isRepo) return {GitOutcome::NotRepo, L"This folder is not connected to GitHub yet (it isn't a git repository)."};
    if (info.originUrl.empty()) return {GitOutcome::NoRemote, L"This folder has no GitHub address (remote \"origin\") set yet."};
    return {GitOutcome::Done, L""};
}

bool HasHead(const std::wstring& repo) {
    return RunGit(repo, {L"rev-parse", L"-q", L"--verify", L"HEAD"}, LogFn(), false).exitCode == 0;
}

bool IsAncestor(const std::wstring& repo, const wchar_t* a, const wchar_t* b) {
    return RunGit(repo, {L"merge-base", L"--is-ancestor", a, b}, LogFn(), false).exitCode == 0;
}

bool RemoteHasSite(const std::wstring& repo) {
    return RunGit(repo, {L"cat-file", L"-e", L"origin/main:data/site-data.js"}, LogFn(), false).exitCode == 0;
}

bool RebaseInProgress(const std::wstring& repo) {
    for (const wchar_t* dir : {L"rebase-merge", L"rebase-apply"}) {
        RunResult gp = RunGit(repo, {L"rev-parse", L"--git-path", dir}, LogFn(), false);
        std::wstring path = FromUtf8(TrimA(gp.output));
        if (gp.exitCode == 0 && !path.empty() && (DirExists(path) || DirExists(RepoPath(repo, path)))) return true;
    }
    return false;
}

// A merge or rebase interrupted earlier (e.g. by an older version of this app or another tool) would
// make every later command fail, so it is rolled back first. Committed work is never lost by this.
void CleanUpUnfinished(const std::wstring& repo, const LogFn& log) {
    if (RunGit(repo, {L"rev-parse", L"-q", L"--verify", L"MERGE_HEAD"}, LogFn(), false).exitCode == 0) {
        log(L"An unfinished combine (merge) was left over from before; cancelling it.");
        RunGit(repo, {L"merge", L"--abort"}, log);
    }
    if (RebaseInProgress(repo)) {
        log(L"An unfinished rebase was left over from before; cancelling it.");
        RunGit(repo, {L"rebase", L"--abort"}, log);
    }
}

// Stages every change in the repository (git add -A, honoring .gitignore), lists the changed files
// in the log and commits them. `committed` stays false when there is nothing to commit.
GitResult CommitAll(const std::wstring& repo, const std::wstring& message, const std::vector<std::wstring>& ident,
                    const LogFn& log, bool& committed) {
    committed = false;
    if (RunGit(repo, {L"add", L"-A"}, log).exitCode != 0) return {GitOutcome::Failed, L"git add failed \u2014 see the log."};
    RunResult d = RunGit(repo, {L"diff", L"--cached", L"--quiet"}, log, false);
    if (d.exitCode == 0) return {GitOutcome::Done, L""};
    if (d.exitCode != 1 && HasHead(repo)) return {GitOutcome::Failed, L"git diff failed \u2014 see the log."};
    RunResult list = RunGit(repo, {L"-c", L"core.quotepath=false", L"diff", L"--cached", L"--name-status"}, log, false);
    log(L"Changed files (A = added, M = modified, D = deleted, R = renamed):");
    std::wstring files = FromUtf8(list.output);
    for (size_t b = 0; b < files.size();) {
        size_t e = files.find(L'\n', b);
        std::wstring line = Trim(files.substr(b, e == std::wstring::npos ? std::wstring::npos : e - b));
        if (!line.empty()) log(L"  " + line);
        if (e == std::wstring::npos) break;
        b = e + 1;
    }
    RunResult c = RunGit(repo, Concat(ident, {L"commit", L"-q", L"-m", message}), log);
    if (c.exitCode != 0) {
        if (ClassifyGitOutput(c.output) == GitError::Identity) return {GitOutcome::Failed, kIdentityHelp};
        return {GitOutcome::Failed, L"git commit failed \u2014 see the log."};
    }
    committed = true;
    return {GitOutcome::Done, L""};
}

std::wstring CommitMessage(bool hasCommits, const std::wstring& summary) {
    if (!hasCommits) return L"Initial site upload (Commission Manager)";
    return L"Update site: " + (summary.empty() ? std::wstring(L"website changes") : summary);
}

// Downloads GitHub's main branch into refs/remotes/origin/main.
RunResult Fetch(const std::wstring& repo, const std::wstring& user, const LogFn& log) {
    return RunNetwork(repo, user, {L"fetch", L"--no-tags", L"origin", L"+refs/heads/main:refs/remotes/origin/main"}, log);
}

bool RemoteBranchMissing(const RunResult& f) {
    return f.started && !f.killed && ClassifyGitOutput(f.output) == GitError::NoRemoteBranch;
}

GitResult NetworkFailure(const RunResult& r, const std::wstring& action) {
    if (!r.started) return {GitOutcome::NoGit, L"Git is not installed (or not on PATH)."};
    if (r.killed)
        return {GitOutcome::Offline, L"GitHub didn't answer for a long time, so the " + action +
                                         L" was stopped. Check your internet connection and try again."};
    switch (ClassifyGitOutput(r.output)) {
    case GitError::Offline:
        return {GitOutcome::Offline, L"Can't reach GitHub. Check your internet connection and try again."};
    case GitError::Auth:
        return {GitOutcome::AuthFailed,
                L"GitHub asked for a sign-in and didn't accept this computer's (or there is none). Click "
                L"\u201cSign in to GitHub\u201d (step 1) and try again. If you are signed in, check the repository "
                L"address in step 2 \u2014 GitHub answers this way for addresses that don't exist, too."};
    case GitError::NoAccess:
        return {GitOutcome::NoAccess,
                L"GitHub says this account isn't allowed to change the repository. Sign in with the account that owns "
                L"it, or \u2014 if you used an access token \u2014 make a new token that includes this repository with "
                L"\u201cContents: Read and write\u201d."};
    case GitError::NotFound:
        return {GitOutcome::NotFound, L"GitHub couldn't find the repository. Check the address in step 2 (and, if the "
                                      L"repository is private, that you're signed in with an account that can see it)."};
    default: return {GitOutcome::Failed, L"The " + action + L" didn't work \u2014 see the details in the log."};
    }
}

std::vector<std::wstring> UnmergedFiles(const std::wstring& repo) {
    return SplitZ(RunGit(repo, {L"-c", L"core.quotepath=false", L"diff", L"--name-only", L"--diff-filter=U", L"-z"},
                         LogFn(), false)
                      .output);
}

size_t CountChangedFiles(const std::wstring& repo, const std::wstring& from) {
    if (from.empty()) return SplitZ(RunGit(repo, {L"ls-files", L"-z"}, LogFn(), false).output).size();
    return SplitZ(RunGit(repo, {L"diff", L"--name-only", L"-z", from, L"HEAD"}, LogFn(), false).output).size();
}

std::wstring HeadId(const std::wstring& repo) {
    RunResult r = RunGit(repo, {L"rev-parse", L"-q", L"--verify", L"HEAD"}, LogFn(), false);
    return r.exitCode == 0 ? FromUtf8(TrimA(r.output)) : std::wstring();
}

const wchar_t kSiteDataFile[] = L"data/site-data.js";

// The committed data/site-data.js in `rev`; false if `rev` has none.
bool CommittedSiteData(const std::wstring& repo, const std::wstring& rev, std::string& text) {
    RunResult r = RunGit(repo, {L"show", rev + L":" + kSiteDataFile}, LogFn(), false);
    if (r.exitCode != 0) return false;
    text = std::move(r.output);
    return true;
}

// Sets "version" in this folder's data/site-data.js. False if it is missing or can't be read/written.
bool StampSiteData(const std::wstring& repo, const std::wstring& version, const LogFn& log) {
    if (!FileExists(RepoPath(repo, kSiteDataFile))) return false;
    SiteData d;
    std::wstring error;
    if (!d.Load(repo, error) || !d.Save(error, version)) {
        log(L"Couldn't update the version stamp in data/site-data.js: " + error.substr(0, error.find(L'\n')));
        return false;
    }
    return true;
}

// True when the only uncommitted change is a new "version" stamp in data/site-data.js.
bool OnlyVersionChanged(const std::wstring& repo) {
    RunResult st = RunGit(repo, {L"status", L"--porcelain"}, LogFn(), false);
    if (st.exitCode != 0) return false;
    std::vector<std::string> lines;
    for (size_t b = 0; b < st.output.size();) {
        size_t e = st.output.find('\n', b);
        if (e == std::string::npos) e = st.output.size();
        std::string line = st.output.substr(b, e - b);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(line);
        b = e + 1;
    }
    if (lines.size() != 1 || lines[0].size() < 4 || lines[0].substr(3) != ToUtf8(kSiteDataFile) ||
        (lines[0][0] != 'M' && lines[0][0] != ' ') || (lines[0][1] != 'M' && lines[0][1] != ' '))
        return false;
    std::string committed, current;
    std::wstring error;
    return CommittedSiteData(repo, L"HEAD", committed) &&
           ReadWholeFile(RepoPath(repo, kSiteDataFile), current, error) && SameExceptVersion(committed, current);
}

// During a conflicted merge: combines data/site-data.js when the only clash is the version stamp.
bool ResolveSiteDataMerge(const std::wstring& repo, const LogFn& log) {
    const std::wstring path = RepoPath(repo, kSiteDataFile);
    std::string text, resolved, prefix, suffix;
    std::wstring error;
    json::Value root;
    if (!ReadWholeFile(path, text, error) || !ResolveVersionConflicts(text, resolved) ||
        !ParseSiteText(resolved, root, prefix, suffix, error) || !WriteWholeFile(path, resolved, error))
        return false;
    if (RunGit(repo, {L"add", L"--", kSiteDataFile}, log).exitCode != 0) return false;
    log(L"data/site-data.js was changed on both sides, but only its version stamps clashed: combined automatically.");
    return true;
}

// Combines GitHub's main branch (origin/main, already fetched) into this folder. Local changes must be
// committed first. With MergeChoice::Ask, separate histories return Unrelated and files changed on both
// sides return Conflict (nothing is changed); otherwise such files are taken whole from the chosen side.
GitResult Integrate(const std::wstring& repo, MergeChoice choice, const std::vector<std::wstring>& ident,
                    const LogFn& log) {
    const LogFn quiet;
    if (!HasHead(repo)) {
        // Nothing in this folder yet: simply take GitHub's files.
        if (RunGit(repo, {L"merge", L"--ff-only", L"origin/main"}, log).exitCode != 0)
            return {GitOutcome::Failed, L"Couldn't download GitHub's files into this folder \u2014 see the log."};
        return {GitOutcome::Done, L""};
    }
    if (IsAncestor(repo, L"origin/main", L"HEAD")) return {GitOutcome::NothingToDo, L""};
    if (IsAncestor(repo, L"HEAD", L"origin/main")) {
        if (RunGit(repo, {L"merge", L"--ff-only", L"origin/main"}, log).exitCode != 0)
            return {GitOutcome::Failed, L"Couldn't update this folder from GitHub \u2014 see the log."};
        return {GitOutcome::Done, L""};
    }

    const bool unrelated = RunGit(repo, {L"merge-base", L"HEAD", L"origin/main"}, quiet, false).exitCode != 0;
    if (unrelated && choice == MergeChoice::Ask) {
        GitResult r{GitOutcome::Unrelated, L"This folder and the GitHub repository were started separately "
                                           L"(they don't share any history yet)."};
        r.files = SplitZ(RunGit(repo, {L"-c", L"core.quotepath=false", L"diff", L"--name-only", L"--diff-filter=A",
                                       L"-z", L"HEAD", L"origin/main"},
                                quiet, false)
                             .output);
        return r;
    }
    // diff3 conflicts include the common version, so clashes of only the "version" stamp can be resolved.
    std::vector<std::wstring> merge = Concat(ident, {L"-c", L"merge.conflictStyle=diff3", L"merge", L"--no-edit", L"-m",
                                                     unrelated ? L"Combine this folder with the files on GitHub"
                                                               : L"Combine changes from GitHub"});
    if (unrelated) merge.push_back(L"--allow-unrelated-histories");
    merge.push_back(L"origin/main");
    RunResult m = RunGit(repo, merge, log);
    if (m.exitCode == 0) return {GitOutcome::Done, L""};

    std::vector<std::wstring> files = UnmergedFiles(repo);
    auto abort = [&] {
        if (RunGit(repo, {L"rev-parse", L"-q", L"--verify", L"MERGE_HEAD"}, quiet, false).exitCode == 0)
            RunGit(repo, {L"merge", L"--abort"}, log);
    };
    if (files.empty()) {
        abort();
        if (ClassifyGitOutput(m.output) == GitError::Identity) return {GitOutcome::Failed, kIdentityHelp};
        return {GitOutcome::Failed, L"Couldn't combine GitHub's files with this folder \u2014 see the log. Nothing was changed."};
    }
    auto siteData = std::find(files.begin(), files.end(), std::wstring(kSiteDataFile));
    if (siteData != files.end() && ResolveSiteDataMerge(repo, log)) files.erase(siteData);
    if (files.empty()) {
        if (RunGit(repo, Concat(ident, {L"commit", L"-q", L"--no-edit"}), log).exitCode != 0) {
            abort();
            return {GitOutcome::Failed, L"Couldn't finish combining with GitHub \u2014 see the log. Nothing was changed."};
        }
        return {GitOutcome::Done, L""};
    }
    if (choice == MergeChoice::Ask) {
        abort();
        GitResult r{GitOutcome::Conflict, L"Some files were changed both on GitHub and on this computer."};
        r.files = files;
        return r;
    }
    // Take each conflicting file whole from the chosen side (or delete it if that side deleted it).
    const std::wstring side = choice == MergeChoice::KeepLocal ? L"HEAD" : L"origin/main";
    log(choice == MergeChoice::KeepLocal ? L"Keeping this computer's version of files changed in both places."
                                         : L"Using GitHub's version of files changed in both places.");
    for (const auto& f : files) {
        const bool exists = RunGit(repo, {L"cat-file", L"-e", side + L":" + f}, quiet, false).exitCode == 0;
        RunResult rr = exists ? RunGit(repo, {L"checkout", side, L"--", f}, log)
                              : RunGit(repo, {L"rm", L"-q", L"-f", L"--", f}, log);
        if (rr.exitCode != 0) {
            abort();
            return {GitOutcome::Failed, L"Couldn't combine " + f + L" \u2014 see the log. Nothing was changed."};
        }
    }
    if (RunGit(repo, Concat(ident, {L"commit", L"-q", L"--no-edit"}), log).exitCode != 0) {
        abort();
        return {GitOutcome::Failed, L"Couldn't finish combining with GitHub \u2014 see the log. Nothing was changed."};
    }
    return {GitOutcome::Done, L""};
}

std::vector<std::wstring> GcmAccounts() {
    RunOptions o;
    o.echo = false;
    o.timeoutMs = kCredentialTimeoutMs;
    RunResult r = RunGit(L"", {L"credential-manager", L"github", L"list"}, LogFn(), o);
    return r.exitCode == 0 ? ParseAccountList(r.output) : std::vector<std::wstring>();
}

} // namespace

// ---------------------------------------------------------------------------
// Pure helpers (unit-tested)
// ---------------------------------------------------------------------------
GitError ClassifyGitOutput(std::string_view output) {
    std::string s(output);
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    auto has = [&](const char* needle) { return s.find(needle) != std::string::npos; };
    if (has("could not resolve host") || has("failed to connect") || has("couldn't connect to server") ||
        has("connection timed out") || has("operation timed out") || has("connection was reset") ||
        has("connection reset") || has("network is unreachable") || has("recv failure") || has("ssl connect error") ||
        has("schannel: failed") || has("could not resolve proxy"))
        return GitError::Offline;
    if (has("returned error: 403") || (has("permission to ") && has(" denied")) || has("write access to repository not granted"))
        return GitError::NoAccess;
    if (has("authentication failed") || has("could not read username") || has("could not read password") ||
        has("cannot prompt because user interactivity has been disabled") || has("returned error: 401") ||
        has("invalid username or password") || has("terminal prompts disabled"))
        return GitError::Auth;
    if (has("repository not found") || has("returned error: 404") || has("does not appear to be a git repository"))
        return GitError::NotFound;
    if (has("couldn't find remote ref")) return GitError::NoRemoteBranch;
    if (has("[rejected]") || has("non-fast-forward") || has("(fetch first)")) return GitError::NonFastForward;
    if (has("please tell me who you are") || has("unable to auto-detect email") || has("empty ident name"))
        return GitError::Identity;
    return GitError::Other;
}

bool ParseCredentialOutput(const std::string& out, std::wstring& username, std::wstring& password) {
    username.clear();
    password.clear();
    for (size_t b = 0; b < out.size();) {
        size_t e = out.find('\n', b);
        if (e == std::string::npos) e = out.size();
        std::string line = out.substr(b, e - b);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("username=", 0) == 0) username = FromUtf8(line.substr(9));
        else if (line.rfind("password=", 0) == 0) password = FromUtf8(line.substr(9));
        Wipe(line);
        b = e + 1;
    }
    return !password.empty();
}

std::vector<std::wstring> ParseAccountList(const std::string& out) {
    std::vector<std::wstring> accounts;
    for (size_t b = 0; b < out.size();) {
        size_t e = out.find('\n', b);
        if (e == std::string::npos) e = out.size();
        std::string line = TrimA(out.substr(b, e - b));
        const bool message = line.rfind("warning:", 0) == 0 || line.rfind("error:", 0) == 0 || line.rfind("fatal:", 0) == 0;
        if (!line.empty() && !message && line.find(' ') == std::string::npos) accounts.push_back(FromUtf8(line));
        b = e + 1;
    }
    return accounts;
}

std::string CredentialRequest(const std::wstring& host, const std::wstring& username, const std::wstring& secret) {
    auto clean = [](const std::wstring& v) {
        std::wstring s;
        for (wchar_t c : v)
            if (c != L'\r' && c != L'\n' && c != L'\0') s += c;
        return ToUtf8(s);
    };
    std::string req = "protocol=https\nhost=" + clean(host) + "\n";
    if (!username.empty()) req += "username=" + clean(username) + "\n";
    if (!secret.empty()) req += "password=" + clean(secret) + "\n";
    return req + "\n";
}

std::wstring CleanToken(const std::wstring& pasted) {
    std::wstring t;
    for (wchar_t c : pasted)
        if (!std::iswspace(c) && c != L'"' && c != L'\'') t += c;
    return t;
}

bool ParseGitHubLogin(const std::string& body, std::wstring& login) {
    json::Value v;
    size_t pos = 0;
    std::string err;
    if (!json::Parse(body, pos, v, err) || v.type != json::Value::Type::Object) return false;
    const json::Value* l = v.Get("login");
    if (!l || l->type != json::Value::Type::String || l->text.empty()) return false;
    login = FromUtf8(l->text);
    return true;
}

bool GitHubRepoFromUrl(const std::wstring& url, std::wstring& owner, std::wstring& name) {
    std::wstring u = Trim(url);
    std::wstring lower = LowerAscii(u);
    size_t start = std::wstring::npos;
    for (const wchar_t* prefix : {L"https://github.com/", L"http://github.com/", L"git@github.com:", L"ssh://git@github.com/"})
        if (lower.rfind(prefix, 0) == 0) start = wcslen(prefix);
    if (start == std::wstring::npos) {
        // https://user@github.com/...
        size_t at = lower.find(L"@github.com/");
        if (lower.rfind(L"https://", 0) != 0 || at == std::wstring::npos) return false;
        start = at + wcslen(L"@github.com/");
    }
    std::wstring path = u.substr(start);
    while (!path.empty() && path.back() == L'/') path.pop_back();
    if (path.size() > 4 && LowerAscii(path.substr(path.size() - 4)) == L".git") path.resize(path.size() - 4);
    size_t slash = path.find(L'/');
    if (slash == std::wstring::npos || slash == 0 || slash + 1 >= path.size() ||
        path.find(L'/', slash + 1) != std::wstring::npos)
        return false;
    owner = path.substr(0, slash);
    name = path.substr(slash + 1);
    return true;
}

std::wstring GitHubPagesUrl(const std::wstring& remoteUrl) {
    std::wstring owner, name;
    if (!GitHubRepoFromUrl(remoteUrl, owner, name)) return {};
    owner = LowerAscii(owner);
    if (LowerAscii(name) == owner + L".github.io") return L"https://" + owner + L".github.io/";
    return L"https://" + owner + L".github.io/" + name + L"/";
}

std::wstring WithCacheBuster(const std::wstring& url, unsigned long long ms) {
    std::wstring u = url;
    size_t hash = u.find(L'#');
    std::wstring fragment = hash == std::wstring::npos ? L"" : u.substr(hash);
    if (hash != std::wstring::npos) u.resize(hash);
    return u + (u.find(L'?') == std::wstring::npos ? L"?t=" : L"&t=") + std::to_wstring(ms) + fragment;
}

unsigned long long UnixTimeMs() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    const unsigned long long t = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (t - 116444736000000000ULL) / 10000; // 100 ns ticks since 1601 → ms since 1970
}

std::wstring SiteUrlFor(const std::wstring& remoteUrl) {
    wchar_t env[1024];
    DWORD n = GetEnvironmentVariableW(L"COMMISSION_MANAGER_PAGES_URL", env, ARRAYSIZE(env));
    std::wstring url = n > 0 && n < ARRAYSIZE(env) ? Trim(env) : GitHubPagesUrl(remoteUrl);
    if (url.empty()) url = kDefaultPagesUrl;
    if (url.back() != L'/') url += L'/';
    return url;
}

std::wstring ActionsUrlFor(const std::wstring& remoteUrl) {
    std::wstring owner, name;
    if (!GitHubRepoFromUrl(remoteUrl, owner, name)) return kDefaultActionsUrl;
    return L"https://github.com/" + owner + L"/" + name + L"/actions";
}

// ---------------------------------------------------------------------------
// HTTPS (WinHTTP)
// ---------------------------------------------------------------------------
namespace {

struct HttpOptions {
    std::wstring headers; // "Name: value\r\n" lines
    DWORD flags = 0;      // extra WinHttpOpenRequest flags
    int resolveMs = 15000, connectMs = 15000, sendMs = 20000, receiveMs = 20000;
};

HttpResponse HttpRequest(const std::wstring& url, HttpOptions opt) {
    HttpResponse res;
    URL_COMPONENTS uc = {sizeof(uc)};
    wchar_t host[256] = {}, path[2048] = {}, extra[2048] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = ARRAYSIZE(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = ARRAYSIZE(path);
    uc.lpszExtraInfo = extra; // "?query"
    uc.dwExtraInfoLength = ARRAYSIZE(extra);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
        res.error = L"Invalid address: " + url;
        return res;
    }
    const std::wstring object = std::wstring(path) + extra;
    HINTERNET session = WinHttpOpen(L"ZelRaccyCommissionManager/1.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
        session = WinHttpOpen(L"ZelRaccyCommissionManager/1.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET conn = nullptr, req = nullptr;
    std::wstring& headers = opt.headers;
    bool ok = session != nullptr;
    if (ok) {
        WinHttpSetTimeouts(session, opt.resolveMs, opt.connectMs, opt.sendMs, opt.receiveMs);
        conn = WinHttpConnect(session, host, uc.nPort, 0);
        ok = conn != nullptr;
    }
    if (ok) {
        req = WinHttpOpenRequest(conn, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 (uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) | opt.flags);
        ok = req != nullptr;
    }
    ok = ok &&
         WinHttpSendRequest(req, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), (DWORD)headers.size(),
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
         WinHttpReceiveResponse(req, nullptr);
    if (ok) {
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &size, WINHTTP_NO_HEADER_INDEX);
        res.status = status;
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(req, &avail) && avail > 0 && res.body.size() < (4u << 20)) {
            std::string chunk(avail, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(req, chunk.data(), avail, &read) || read == 0) break;
            res.body.append(chunk.data(), read);
        }
    } else {
        res.error = L"Couldn't connect to " + std::wstring(host) + L" (error " + std::to_wstring(GetLastError()) + L").";
    }
    Wipe(headers);
    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    if (session) WinHttpCloseHandle(session);
    return res;
}

} // namespace

HttpResponse HttpGet(const std::wstring& url, const std::wstring& bearerToken) {
    HttpOptions o;
    o.headers = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    if (!bearerToken.empty()) o.headers += L"Authorization: Bearer " + bearerToken + L"\r\n";
    return HttpRequest(url, std::move(o));
}

HttpResponse HttpGetFresh(const std::wstring& url) {
    HttpOptions o;
    o.headers = L"Cache-Control: no-cache\r\nPragma: no-cache\r\n";
    o.flags = WINHTTP_FLAG_REFRESH; // never answer from a cache
    o.resolveMs = o.connectMs = o.sendMs = o.receiveMs = 8000;
    return HttpRequest(url, std::move(o));
}

// ---------------------------------------------------------------------------
// Waiting for GitHub Pages
// ---------------------------------------------------------------------------
LiveCheckResult WaitUntilLive(const std::wstring& siteUrl, const std::wstring& version, HANDLE cancelEvent,
                              DWORD intervalMs, DWORD timeoutMs, const LiveProgressFn& progress) {
    LiveCheckResult r;
    std::wstring dataUrl = siteUrl;
    if (dataUrl.empty() || dataUrl.back() != L'/') dataUrl += L'/';
    dataUrl += L"data/site-data.js";
    auto cancelled = [cancelEvent](DWORD waitMs) {
        if (!cancelEvent) {
            Sleep(waitMs);
            return false;
        }
        return WaitForSingleObject(cancelEvent, waitMs) == WAIT_OBJECT_0;
    };
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        if (cancelled(0)) {
            r.outcome = LiveOutcome::Cancelled;
            return r;
        }
        ++r.attempts;
        HttpResponse resp = HttpGetFresh(WithCacheBuster(dataUrl, UnixTimeMs()));
        std::wstring seen, problem;
        if (resp.status == 200) {
            seen = r.seenVersion = ExtractSiteVersion(resp.body);
            if (seen.empty()) problem = L"its site data has no version stamp yet";
        } else {
            problem = resp.status ? L"HTTP " + std::to_wstring(resp.status) : resp.error;
        }
        // A newer stamp (a later upload, e.g. from another computer) includes this one too.
        const bool live = !seen.empty() && (seen == version || (IsVersionStamp(seen) && IsVersionStamp(version) && seen > version));
        if (progress) progress(r.attempts, seen, problem);
        if (live) {
            r.outcome = LiveOutcome::Live;
            return r;
        }
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) {
            r.outcome = LiveOutcome::TimedOut;
            return r;
        }
        if (cancelled((DWORD)std::min<ULONGLONG>(intervalMs, deadline - now))) {
            r.outcome = LiveOutcome::Cancelled;
            return r;
        }
    }
}

// ---------------------------------------------------------------------------
// GitHub sign-in (credentials live in Git's credential helper, i.e. Windows Credential Manager)
// ---------------------------------------------------------------------------
bool ReadSavedCredential(const std::wstring& host, const std::wstring& username, std::wstring& userOut,
                         std::wstring& secretOut) {
    std::string in = CredentialRequest(host, username, L"");
    RunOptions o;
    o.echo = false; // the output contains the secret
    o.input = &in;
    o.timeoutMs = kCredentialTimeoutMs;
    RunResult r = RunGit(L"", {L"credential", L"fill"}, LogFn(), o);
    std::wstring u, p;
    const bool ok = r.exitCode == 0 && !r.killed && ParseCredentialOutput(r.output, u, p) &&
                    (username.empty() || u.empty() || _wcsicmp(u.c_str(), username.c_str()) == 0);
    Wipe(r.output);
    if (ok) {
        userOut = u.empty() ? username : u;
        secretOut = p;
    }
    Wipe(p);
    return ok;
}

GitHubAccount CheckGitHubAccount(const std::wstring& preferredUser) {
    GitHubAccount a;
    RunOptions o;
    o.echo = false;
    o.timeoutMs = kCredentialTimeoutMs;
    RunResult v = RunGit(L"", {L"--version"}, LogFn(), o);
    a.gitAvailable = v.started && v.exitCode == 0;
    if (!a.gitAvailable) {
        a.note = L"Git isn't installed.";
        return a;
    }
    RunResult gcm = RunGit(L"", {L"credential-manager", L"--version"}, LogFn(), o);
    if (gcm.exitCode == 0) a.gcmVersion = FromUtf8(TrimA(gcm.output));

    std::wstring user, secret;
    bool found = ReadSavedCredential(kGitHubHost, preferredUser, user, secret);
    if (!found && !a.gcmVersion.empty())
        for (const auto& acct : GcmAccounts())
            if ((found = ReadSavedCredential(kGitHubHost, acct, user, secret))) break;
    if (!found) return a;

    HttpResponse resp = HttpGet(L"https://api.github.com/user", secret);
    Wipe(secret);
    a.username = user;
    std::wstring login;
    if (resp.status == 200 && ParseGitHubLogin(resp.body, login)) {
        a.signedIn = a.verified = true;
        a.username = login;
    } else if (resp.status == 401) {
        a.note = L"GitHub no longer accepts the saved sign-in" + (user.empty() ? std::wstring() : L" for " + user) +
                 L" (it expired or was revoked). Please sign in again.";
    } else {
        a.signedIn = true; // saved, but GitHub couldn't confirm right now
        a.note = resp.status == 0 ? std::wstring(L"couldn't reach GitHub to double-check")
                                  : L"GitHub didn't confirm the sign-in (HTTP " + std::to_wstring(resp.status) + L")";
    }
    return a;
}

GitResult SignInWithBrowser(const LogFn& log) {
    const std::vector<std::wstring> before = GcmAccounts();
    log(L"Opening GitHub in your web browser. Sign in there and click \u201cAuthorize\u201d, then come back here.");
    RunOptions o;
    o.interactive = true;
    o.timeoutMs = kLoginTimeoutMs;
    o.cancellable = true;
    RunResult r = RunGit(L"", {L"credential-manager", L"github", L"login", L"--url", L"https://github.com", L"--browser"},
                         log, o);
    if (!r.started) return {GitOutcome::NoGit, L"Git is not installed (or not on PATH)."};
    if (r.cancelled) return {GitOutcome::Cancelled, L"Sign-in cancelled."};
    if (r.killed) return {GitOutcome::Failed, L"Sign-in took too long and was stopped. Please try again."};
    if (r.exitCode != 0) {
        if (ClassifyGitOutput(r.output) == GitError::Offline)
            return {GitOutcome::Offline, L"Can't reach GitHub. Check your internet connection and try again."};
        return {GitOutcome::Failed, L"GitHub sign-in didn't finish. Try again, or use \u201cUse access token\u2026\u201d instead."};
    }
    const std::vector<std::wstring> after = GcmAccounts();
    GitResult res{GitOutcome::Done, L""};
    for (const auto& a : after)
        if (std::find(before.begin(), before.end(), a) == before.end()) res.account = a;
    if (res.account.empty() && after.size() == 1) res.account = after[0];
    res.message = L"Signed in to GitHub" + (res.account.empty() ? std::wstring(L".") : L" as " + res.account + L".");
    return res;
}

void CancelSignIn() {
    std::lock_guard<std::mutex> lock(g_cancelMutex);
    if (g_cancelWatch) {
        g_cancelWatch->cancelled = true;
        StopProcessTree(g_cancelWatch, FALSE);
    }
}

GitResult SaveSignIn(const std::wstring& host, const std::wstring& username, const std::wstring& secret, const LogFn& log) {
    if (username.empty() || secret.empty()) return {GitOutcome::Failed, L"Missing user name or token."};
    // Git needs a credential helper to remember the sign-in; Git for Windows normally sets "manager".
    RunResult helper = RunGit(L"", {L"config", L"--get-all", L"credential.helper"}, LogFn(), false);
    if (!helper.started) return {GitOutcome::NoGit, L"Git is not installed (or not on PATH)."};
    if (TrimA(helper.output).empty()) {
        const bool gcm = RunGit(L"", {L"credential-manager", L"--version"}, LogFn(), false).exitCode == 0;
        const wchar_t* name = gcm ? L"manager" : L"wincred";
        log(L"Git has no credential helper configured; setting credential.helper=" + std::wstring(name) +
            L" (Windows Credential Manager).");
        if (RunGit(L"", {L"config", L"--global", L"credential.helper", name}, log).exitCode != 0)
            return {GitOutcome::Failed, L"Couldn't set up Git's credential helper \u2014 see the log."};
    }
    std::string in = CredentialRequest(host, username, secret);
    RunOptions o;
    o.echo = false;
    o.input = &in;
    o.timeoutMs = kCredentialTimeoutMs;
    RunResult r = RunGit(L"", {L"credential", L"approve"}, LogFn(), o);
    Wipe(in);
    std::wstring u, p;
    const bool saved = r.exitCode == 0 && ReadSavedCredential(host, username, u, p) && p == secret;
    Wipe(p);
    if (!saved) {
        if (!TrimA(r.output).empty()) log(L"  " + FromUtf8(TrimA(r.output)));
        return {GitOutcome::Failed, L"Couldn't save the sign-in in Windows Credential Manager \u2014 see the log."};
    }
    log(L"Saved the sign-in for " + username + L" (" + host + L") in Windows Credential Manager.");
    GitResult res{GitOutcome::Done, L"Signed in as " + username + L"."};
    res.account = username;
    return res;
}

GitResult SignOut(const std::wstring& host, const std::wstring& username, const LogFn& log) {
    std::string in = CredentialRequest(host, username, L"");
    RunOptions o;
    o.echo = false;
    o.input = &in;
    o.timeoutMs = kCredentialTimeoutMs;
    RunResult r = RunGit(L"", {L"credential", L"reject"}, LogFn(), o);
    if (!r.started) return {GitOutcome::NoGit, L"Git is not installed (or not on PATH)."};
    std::wstring u, p;
    if (ReadSavedCredential(host, username, u, p)) {
        Wipe(p);
        return {GitOutcome::Failed, L"The saved sign-in couldn't be removed. You can delete it in Windows "
                                    L"\u201cCredential Manager\u201d \u2192 Windows Credentials (git:https://" + host + L")."};
    }
    log(L"Removed the saved sign-in" + (username.empty() ? std::wstring() : L" for " + username) + L" (" + host + L").");
    return {GitOutcome::Done, L"Signed out of GitHub on this computer."};
}

// ---------------------------------------------------------------------------
// Repository workflows
// ---------------------------------------------------------------------------
bool SameRemoteUrl(const std::wstring& a, const std::wstring& b) {
    auto norm = [](std::wstring s) {
        s = LowerAscii(Trim(s));
        while (!s.empty() && s.back() == L'/') s.pop_back();
        if (s.size() > 4 && s.compare(s.size() - 4, 4, L".git") == 0) s.resize(s.size() - 4);
        return s;
    };
    return norm(a) == norm(b);
}

RepoInfo InspectRepo(const std::wstring& repo) {
    RepoInfo info;
    const LogFn quiet;
    info.gitAvailable = RunGit(repo, {L"--version"}, quiet, false).started;
    if (!info.gitAvailable || !DirExists(repo)) return info;
    // Only a repository rooted exactly at this folder counts (not a parent folder's repository).
    RunResult top = RunGit(repo, {L"rev-parse", L"--show-toplevel"}, quiet, false);
    info.isRepo = top.exitCode == 0 && NormPath(FromUtf8(TrimA(top.output))) == NormPath(repo);
    if (!info.isRepo) return info;
    info.hasCommits = HasHead(repo);
    RunResult br = RunGit(repo, {L"symbolic-ref", L"--short", L"-q", L"HEAD"}, quiet, false);
    if (br.exitCode == 0) info.branch = FromUtf8(TrimA(br.output));
    RunResult origin = RunGit(repo, {L"remote", L"get-url", L"origin"}, quiet, false);
    if (origin.exitCode == 0) info.originUrl = FromUtf8(TrimA(origin.output));
    info.hasUpstream =
        RunGit(repo, {L"rev-parse", L"--abbrev-ref", L"--symbolic-full-name", L"@{u}"}, quiet, false).exitCode == 0;
    return info;
}

PublishState GetPublishState(const std::wstring& repo) {
    PublishState s;
    const LogFn quiet;
    s.localChanges = !TrimA(RunGit(repo, {L"status", L"--porcelain"}, quiet, false).output).empty();
    const bool head = HasHead(repo);
    s.remoteKnown = RunGit(repo, {L"rev-parse", L"-q", L"--verify", L"refs/remotes/origin/main"}, quiet, false).exitCode == 0;
    if (!s.remoteKnown) {
        s.ahead = head ? 1 : 0;
        return s;
    }
    s.remoteHasSite = RemoteHasSite(repo);
    if (head) {
        RunResult c = RunGit(repo, {L"rev-list", L"--left-right", L"--count", L"HEAD...refs/remotes/origin/main"}, quiet, false);
        if (c.exitCode == 0) sscanf_s(c.output.c_str(), "%d %d", &s.ahead, &s.behind);
    }
    s.remoteGallery = SplitZ(RunGit(repo, {L"-c", L"core.quotepath=false", L"ls-tree", L"-r", L"--name-only", L"-z",
                                           L"refs/remotes/origin/main", L"--", L"assets/gallery/"},
                                    quiet, false)
                                 .output);
    s.changedGallery = SplitZ(RunGit(repo, {L"-c", L"core.quotepath=false", L"diff", L"--name-only", L"-z",
                                            L"refs/remotes/origin/main", L"--", L"assets/gallery/"},
                                     quiet, false)
                                  .output);
    return s;
}

GitResult ConnectRepo(const std::wstring& repo, const std::wstring& url, bool allowChangeOrigin, const LogFn& log) {
    RepoInfo info = InspectRepo(repo);
    if (!info.gitAvailable) return {GitOutcome::NoGit, L"Git is not installed (or not on PATH)."};
    if (Trim(url).empty()) return {GitOutcome::Failed, L"Please enter the GitHub repository address first."};

    if (!info.isRepo) {
        if (RunGit(repo, {L"init"}, log).exitCode != 0) return {GitOutcome::Failed, L"git init failed \u2014 see the log."};
        info.hasCommits = false;
        info.branch.clear();
    }
    if (info.branch != kBranch) {
        RunResult b = info.hasCommits ? RunGit(repo, {L"branch", L"-M", kBranch}, log)
                                      : RunGit(repo, {L"symbolic-ref", L"HEAD", L"refs/heads/main"}, log);
        if (b.exitCode != 0) return {GitOutcome::Failed, L"Couldn't set the branch to main \u2014 see the log."};
    }
    if (info.originUrl.empty()) {
        if (RunGit(repo, {L"remote", L"add", L"origin", Trim(url)}, log).exitCode != 0)
            return {GitOutcome::Failed, L"Couldn't add the GitHub address \u2014 see the log."};
    } else if (!SameRemoteUrl(info.originUrl, url)) {
        if (!allowChangeOrigin)
            return {GitOutcome::Failed, L"This folder is already linked to a different address: " + info.originUrl};
        if (RunGit(repo, {L"remote", L"set-url", L"origin", Trim(url)}, log).exitCode != 0)
            return {GitOutcome::Failed, L"Couldn't change the GitHub address \u2014 see the log."};
        // The old address's downloaded branch says nothing about the new repository.
        RunGit(repo, {L"update-ref", L"-d", L"refs/remotes/origin/main"}, LogFn(), false);
    }
    return {GitOutcome::Done, L"Connected to " + Trim(url) + L" (branch main)."};
}

GitResult SyncFromGitHub(const std::wstring& repo, const std::wstring& githubUser, const std::wstring& summary,
                         MergeChoice choice, const LogFn& log) {
    RepoInfo info = InspectRepo(repo);
    if (GitResult b = CheckBasics(info); b.outcome != GitOutcome::Done) return b;
    CleanUpUnfinished(repo, log);

    RunResult f = Fetch(repo, githubUser, log);
    if (f.exitCode != 0 || f.killed) {
        if (RemoteBranchMissing(f)) {
            RunGit(repo, {L"update-ref", L"-d", L"refs/remotes/origin/main"}, LogFn(), false);
            return {GitOutcome::NoSiteOnGitHub, kNoSiteOnGitHub};
        }
        return NetworkFailure(f, L"download");
    }
    if (!RemoteHasSite(repo)) return {GitOutcome::NoSiteOnGitHub, kNoSiteOnGitHub};
    if (info.hasCommits && IsAncestor(repo, L"origin/main", L"HEAD")) {
        const bool pending = !IsAncestor(repo, L"HEAD", L"origin/main") ||
                             !TrimA(RunGit(repo, {L"status", L"--porcelain"}, LogFn(), false).output).empty();
        return {GitOutcome::NothingToDo, pending ? L"Already up to date with GitHub. Your newer changes here are waiting "
                                                   L"for \u201cPush changes\u201d."
                                                 : L"Already up to date with GitHub."};
    }

    // Local changes are committed first, so nothing in this folder can be lost while combining.
    const std::vector<std::wstring> ident = IdentityArgs(repo, githubUser);
    bool committed = false;
    if (GitResult r = CommitAll(repo, CommitMessage(info.hasCommits, summary), ident, log, committed);
        r.outcome != GitOutcome::Done)
        return r;
    const std::wstring before = HeadId(repo);
    GitResult r = Integrate(repo, choice, ident, log);
    if (r.outcome != GitOutcome::Done && r.outcome != GitOutcome::NothingToDo) return r;
    const size_t n = CountChangedFiles(repo, before);
    return {GitOutcome::Done, L"Downloaded the latest from GitHub (" + std::to_wstring(n) +
                                  (n == 1 ? L" file" : L" files") + L" updated)."};
}

GitResult PushChanges(const std::wstring& repo, const std::wstring& githubUser, const std::wstring& summary,
                      MergeChoice choice, const LogFn& log) {
    RepoInfo info = InspectRepo(repo);
    if (GitResult b = CheckBasics(info); b.outcome != GitOutcome::Done) return b;
    CleanUpUnfinished(repo, log);
    const std::vector<std::wstring> ident = IdentityArgs(repo, githubUser);

    // Every upload carries a fresh "version" in data/site-data.js, so the website (and the live check
    // after the push) can tell the new copy from a cached one. A new stamp alone is never uploaded.
    const std::wstring stamp = NewVersionStamp();
    if (!TrimA(RunGit(repo, {L"status", L"--porcelain"}, LogFn(), false).output).empty()) {
        if (info.hasCommits && OnlyVersionChanged(repo)) {
            log(L"Only the version stamp in data/site-data.js changed, so there is nothing new to upload from this folder.");
            RunGit(repo, {L"checkout", L"-q", L"HEAD", L"--", kSiteDataFile}, log);
        } else if (StampSiteData(repo, stamp, log)) {
            log(L"Version stamp for this upload: " + stamp);
        }
    }

    bool committed = false;
    if (GitResult r = CommitAll(repo, CommitMessage(info.hasCommits, summary), ident, log, committed);
        r.outcome != GitOutcome::Done)
        return r;
    if (!HasHead(repo)) return {GitOutcome::NothingToDo, L"This folder is empty, so there is nothing to upload yet."};

    // Combine with whatever is on GitHub first, so the upload is never refused.
    RunResult f = Fetch(repo, githubUser, log);
    bool downloaded = false;
    if (f.exitCode != 0 || f.killed) {
        if (!RemoteBranchMissing(f)) return NetworkFailure(f, L"upload");
        RunGit(repo, {L"update-ref", L"-d", L"refs/remotes/origin/main"}, LogFn(), false); // GitHub is empty
    } else {
        const std::wstring before = HeadId(repo);
        GitResult r = Integrate(repo, choice, ident, log);
        if (r.outcome == GitOutcome::Unrelated)
            r.message = RemoteHasSite(repo)
                            ? L"GitHub already has a different copy of the website, started separately from this folder."
                            : L"GitHub already has a few files (like a LICENSE, or images uploaded on github.com) but "
                              L"not the website itself yet" +
                                  (r.files.empty() ? std::wstring(L".")
                                                   : L"; " + std::to_wstring(r.files.size()) +
                                                         (r.files.size() == 1 ? L" of them isn't" : L" of them aren't") +
                                                         L" in this folder.");
        if (r.outcome != GitOutcome::Done && r.outcome != GitOutcome::NothingToDo) return r;
        downloaded = r.outcome == GitOutcome::Done && CountChangedFiles(repo, before) > 0;
        RunResult ahead = RunGit(repo, {L"rev-list", L"--count", L"refs/remotes/origin/main..HEAD"}, LogFn(), false);
        if (ahead.exitCode == 0 && TrimA(ahead.output) == "0") {
            if (!info.hasUpstream) RunGit(repo, {L"branch", L"--set-upstream-to=origin/main"}, LogFn(), false);
            return {GitOutcome::NothingToDo, L"Nothing to push \u2014 GitHub already has everything in this folder."};
        }
    }

    // What goes up must carry this upload's stamp, also when the newest commit was made earlier (a push that
    // failed) or GitHub's copy of the file won while combining. The unpublished newest commit is amended,
    // so the stamp never makes a commit of its own.
    std::string head;
    if (CommittedSiteData(repo, L"HEAD", head) && ExtractSiteVersion(head) != stamp && StampSiteData(repo, stamp, log)) {
        log(L"Version stamp for this upload: " + stamp);
        if (RunGit(repo, {L"add", L"--", kSiteDataFile}, log).exitCode != 0 ||
            RunGit(repo, Concat(ident, {L"commit", L"-q", L"--amend", L"--no-edit"}), log).exitCode != 0)
            return {GitOutcome::Failed, L"Couldn't add the version stamp to the upload \u2014 see the log."};
    }
    std::wstring version;
    if (CommittedSiteData(repo, L"HEAD", head)) version = ExtractSiteVersion(head);

    RunResult p = RunNetwork(repo, githubUser, {L"push", L"-u", L"origin", L"HEAD:refs/heads/main"}, log);
    if (!p.started || p.exitCode != 0 || p.killed) {
        if (!p.killed && ClassifyGitOutput(p.output) == GitError::NonFastForward)
            return {GitOutcome::Failed, L"GitHub changed while uploading. Click \u201cPush changes\u201d again."};
        return NetworkFailure(p, L"upload");
    }
    GitResult done{GitOutcome::Done, std::wstring(L"Uploaded to GitHub.") +
                                         (downloaded ? L" (GitHub's newer files were downloaded and combined first.)" : L"")};
    done.version = version;
    return done;
}
