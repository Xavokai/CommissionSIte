#include "core.h"

#include <objbase.h>
#include <oleauto.h>
#include <propidl.h>
#include <wincodec.h>

#include <algorithm>
#include <cwctype>

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

    json::Value root;
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

    prefix_ = text.substr(bom, at - bom);
    suffix_ = text.substr(p);
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

bool SiteData::Save(std::wstring& error) const {
    const std::string text = ToText();
    const std::wstring tmp = dataPath_ + L".tmp";

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
        if (MoveFileExW(tmp.c_str(), dataPath_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
        gle = GetLastError();
        if (attempt >= 10) break;
        Sleep(100);
    }
    DeleteFileW(tmp.c_str());
    error = L"Couldn't replace " + dataPath_ + L":\n" + SystemMessage(gle) +
            L"\n\nIs the file open in another program?";
    return false;
}

bool SiteData::CommissionsOpen() const {
    const json::Value* c = root_.Get("commissionsOpen");
    return c && c->type == json::Value::Type::Bool && c->boolean;
}

void SiteData::SetCommissionsOpen(bool open) {
    if (json::Value* c = root_.Get("commissionsOpen")) *c = json::Value::Bool(open);
    else root_.members.insert(root_.members.begin(), json::Member("commissionsOpen", json::Value::Bool(open)));
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
// Git
// ===========================================================================
namespace {

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

struct RunResult {
    bool started = false;
    DWORD exitCode = 1;
    std::string output;
};

RunResult RunGit(const std::wstring& dir, const std::vector<std::wstring>& args, const LogFn& log, bool echo = true) {
    RunResult r;
    std::wstring cmd = L"git";
    for (auto& a : args) cmd += L" " + QuoteArg(a);
    if (echo) log(L"> " + cmd);

    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return r;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(0);
    r.started = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                               dir.c_str(), &si, &pi) != FALSE;
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!r.started) {
        CloseHandle(rd);
        return r;
    }

    std::string pending;
    char chunk[4096];
    DWORD n = 0;
    while (ReadFile(rd, chunk, sizeof(chunk), &n, nullptr) && n > 0) {
        r.output.append(chunk, n);
        pending.append(chunk, n);
        size_t nl;
        while ((nl = pending.find_first_of("\r\n")) != std::string::npos) {
            std::string line = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            if (echo && !line.empty()) log(L"  " + FromUtf8(line));
        }
    }
    if (echo && !pending.empty()) log(L"  " + FromUtf8(pending));
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &r.exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return r;
}

std::string TrimA(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

bool Contains(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

void PrepareGitEnv() {
    // Never let git wait for a console prompt nobody can see (GUI credential helpers still work).
    SetEnvironmentVariableW(L"GIT_TERMINAL_PROMPT", L"0");
}

std::wstring NormPath(std::wstring p) {
    std::replace(p.begin(), p.end(), L'/', L'\\');
    wchar_t full[4096], longer[4096];
    if (GetFullPathNameW(p.c_str(), ARRAYSIZE(full), full, nullptr)) p = full;
    if (GetLongPathNameW(p.c_str(), longer, ARRAYSIZE(longer))) p = longer;
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    return LowerAscii(p);
}

bool NoIdentity(const RunResult& r) { return Contains(r.output, "tell me who you are") || Contains(r.output, "user.email"); }

GitResult CommitFailed(const RunResult& c) {
    if (NoIdentity(c))
        return {GitOutcome::Failed, L"Git doesn't know your name/email yet. Run once in a command prompt:\n"
                                    L"  git config --global user.name \"Your Name\"\n"
                                    L"  git config --global user.email \"you@example.com\""};
    return {GitOutcome::Failed, L"git commit failed — see the log."};
}

bool LooksLikeAuthError(const std::string& out) {
    return Contains(out, "Authentication") || Contains(out, "could not read Username") ||
           Contains(out, "Permission denied") || Contains(out, "403") || Contains(out, "Repository not found");
}

const wchar_t kAuthHint[] = L"GitHub refused the connection. Check the repository address, sign in to GitHub "
                            L"(e.g. with GitHub Desktop or the Git Credential Manager window) and try again.";

GitResult CheckBasics(const RepoInfo& info) {
    if (!info.gitAvailable) return {GitOutcome::NoGit, L"Git is not installed (or not on PATH)."};
    if (!info.isRepo) return {GitOutcome::NotRepo, L"This folder is not connected to GitHub yet (it isn't a git repository)."};
    if (info.originUrl.empty()) return {GitOutcome::NoRemote, L"This folder has no GitHub address (remote \"origin\") set yet."};
    return {GitOutcome::Done, L""};
}

// Stages every change in the repository (git add -A, honoring .gitignore), lists the changed files
// in the log and commits them. `committed` stays false when there is nothing to commit.
GitResult CommitAll(const std::wstring& repo, const std::wstring& message, const LogFn& log, bool& committed) {
    committed = false;
    if (RunGit(repo, {L"add", L"-A"}, log).exitCode != 0) return {GitOutcome::Failed, L"git add failed — see the log."};
    RunResult d = RunGit(repo, {L"diff", L"--cached", L"--quiet"}, log, false);
    if (d.exitCode == 0) {
        log(L"No new changes to commit.");
        return {GitOutcome::Done, L""};
    }
    if (d.exitCode != 1) return {GitOutcome::Failed, L"git diff failed — see the log."};
    RunResult list = RunGit(repo, {L"-c", L"core.quotepath=false", L"diff", L"--cached", L"--name-status"}, log, false);
    log(L"Changed files to publish (A = added, M = modified, D = deleted, R = renamed):");
    std::wstring files = FromUtf8(list.output);
    for (size_t b = 0; b < files.size();) {
        size_t e = files.find(L'\n', b);
        std::wstring line = Trim(files.substr(b, e == std::wstring::npos ? std::wstring::npos : e - b));
        if (!line.empty()) log(L"  " + line);
        if (e == std::wstring::npos) break;
        b = e + 1;
    }
    RunResult c = RunGit(repo, {L"commit", L"-m", message}, log);
    if (c.exitCode != 0) return CommitFailed(c);
    committed = true;
    return {GitOutcome::Done, L""};
}

} // namespace

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
    PrepareGitEnv();
    RepoInfo info;
    LogFn quiet = [](const std::wstring&) {};
    info.gitAvailable = RunGit(repo, {L"--version"}, quiet, false).started;
    if (!info.gitAvailable || !DirExists(repo)) return info;
    // Only a repository rooted exactly at this folder counts (not a parent folder's repository).
    RunResult top = RunGit(repo, {L"rev-parse", L"--show-toplevel"}, quiet, false);
    info.isRepo = top.exitCode == 0 && NormPath(FromUtf8(TrimA(top.output))) == NormPath(repo);
    if (!info.isRepo) return info;
    info.hasCommits = RunGit(repo, {L"rev-parse", L"--verify", L"-q", L"HEAD"}, quiet, false).exitCode == 0;
    RunResult br = RunGit(repo, {L"symbolic-ref", L"--short", L"-q", L"HEAD"}, quiet, false);
    if (br.exitCode == 0) info.branch = FromUtf8(TrimA(br.output));
    RunResult origin = RunGit(repo, {L"remote", L"get-url", L"origin"}, quiet, false);
    if (origin.exitCode == 0) info.originUrl = FromUtf8(TrimA(origin.output));
    info.hasUpstream =
        RunGit(repo, {L"rev-parse", L"--abbrev-ref", L"--symbolic-full-name", L"@{u}"}, quiet, false).exitCode == 0;
    return info;
}

GitResult ConnectRepo(const std::wstring& repo, const std::wstring& url, bool allowChangeOrigin, const LogFn& log) {
    PrepareGitEnv();
    RepoInfo info = InspectRepo(repo);
    if (!info.gitAvailable) return {GitOutcome::NoGit, L"Git is not installed (or not on PATH)."};
    if (Trim(url).empty()) return {GitOutcome::Failed, L"Please enter the GitHub repository address first."};

    if (!info.isRepo) {
        if (RunGit(repo, {L"init"}, log).exitCode != 0) return {GitOutcome::Failed, L"git init failed — see the log."};
        info.hasCommits = false;
        info.branch.clear();
    }
    if (info.branch != kBranch) {
        RunResult b = info.hasCommits ? RunGit(repo, {L"branch", L"-M", kBranch}, log)
                                      : RunGit(repo, {L"symbolic-ref", L"HEAD", L"refs/heads/main"}, log);
        if (b.exitCode != 0) return {GitOutcome::Failed, L"Couldn't set the branch to main — see the log."};
    }
    if (info.originUrl.empty()) {
        if (RunGit(repo, {L"remote", L"add", L"origin", Trim(url)}, log).exitCode != 0)
            return {GitOutcome::Failed, L"Couldn't add the GitHub address — see the log."};
    } else if (!SameRemoteUrl(info.originUrl, url)) {
        if (!allowChangeOrigin)
            return {GitOutcome::Failed, L"This folder is already linked to a different address: " + info.originUrl};
        if (RunGit(repo, {L"remote", L"set-url", L"origin", Trim(url)}, log).exitCode != 0)
            return {GitOutcome::Failed, L"Couldn't change the GitHub address — see the log."};
    }
    return {GitOutcome::Done, L"Connected to " + Trim(url) + L" (branch main). Click \u201cPush changes\u201d to upload."};
}

GitResult PushChanges(const std::wstring& repo, const std::wstring& summary, const LogFn& log) {
    PrepareGitEnv();
    RepoInfo info = InspectRepo(repo);
    if (GitResult b = CheckBasics(info); b.outcome != GitOutcome::Done) return b;

    bool committed = false;
    const std::wstring message =
        !info.hasCommits ? std::wstring(L"Initial site upload (Commission Manager)")
                         : L"Update site: " + (summary.empty() ? std::wstring(L"website changes") : summary);
    if (GitResult r = CommitAll(repo, message, log, committed); r.outcome != GitOutcome::Done) return r;

    if (!committed && info.hasUpstream) {
        RunResult ahead = RunGit(repo, {L"rev-list", L"--count", L"@{u}..HEAD"}, log, false);
        if (ahead.exitCode == 0 && TrimA(ahead.output) == "0")
            return {GitOutcome::NothingToDo, L"Nothing to push — the website is already up to date."};
    }

    std::vector<std::wstring> push = {L"push"};
    if (!info.hasUpstream) push.insert(push.end(), {L"-u", L"origin", kBranch});
    RunResult p = RunGit(repo, push, log);
    if (!p.started || p.exitCode != 0) {
        if (Contains(p.output, "src refspec"))
            return {GitOutcome::Failed, L"There is no \u201cmain\u201d branch here. Click \u201cConnect to GitHub\u2026\u201d to fix the setup."};
        if (Contains(p.output, "[rejected]") || Contains(p.output, "fetch first") || Contains(p.output, "non-fast-forward"))
            return {GitOutcome::Rejected,
                    L"GitHub has changes this folder doesn't have yet, so the push was refused. "
                    L"Click \u201cPull\u201d first, then \u201cPush changes\u201d again."};
        if (LooksLikeAuthError(p.output)) return {GitOutcome::Failed, kAuthHint};
        return {GitOutcome::Failed, L"git push failed — see the log for details."};
    }
    if (!committed && Contains(p.output, "Everything up-to-date"))
        return {GitOutcome::NothingToDo, L"Nothing to push — the website is already up to date."};
    return {GitOutcome::Done, L"Pushed! GitHub Pages usually updates the site within a minute or two."};
}

GitResult PullChanges(const std::wstring& repo, bool allowUnrelated, const LogFn& log) {
    PrepareGitEnv();
    RepoInfo info = InspectRepo(repo);
    if (GitResult b = CheckBasics(info); b.outcome != GitOutcome::Done) return b;

    RunResult f = RunGit(repo, {L"fetch", L"origin", kBranch}, log);
    if (f.exitCode != 0) {
        if (Contains(f.output, "couldn't find remote ref"))
            return {GitOutcome::NothingToDo, L"GitHub has no \u201cmain\u201d branch yet, so there is nothing to pull. "
                                             L"Click \u201cPush changes\u201d to upload."};
        if (LooksLikeAuthError(f.output)) return {GitOutcome::Failed, kAuthHint};
        return {GitOutcome::Failed, L"Couldn't download from GitHub — see the log."};
    }

    const bool unrelated =
        !info.hasCommits || RunGit(repo, {L"merge-base", L"HEAD", L"FETCH_HEAD"}, log, false).exitCode != 0;
    if (unrelated && !allowUnrelated)
        return {GitOutcome::Unrelated, L"This folder and the GitHub repository have separate histories."};
    if (!info.hasCommits) {
        bool committed;
        if (GitResult r = CommitAll(repo, L"Initial site upload (Commission Manager)", log, committed);
            r.outcome != GitOutcome::Done)
            return r;
    }

    std::vector<std::wstring> pull = {L"pull", L"--rebase", L"--autostash"};
    // Combining unrelated histories: GitHub's files are kept, and where both sides have the same
    // file this folder's version wins (during a rebase, "theirs" = the local commits being replayed).
    if (unrelated) pull.insert(pull.end(), {L"--allow-unrelated-histories", L"-X", L"theirs"});
    pull.insert(pull.end(), {L"origin", kBranch});
    RunResult p = RunGit(repo, pull, log);
    if (p.exitCode != 0) {
        bool rebasing = false;
        for (const wchar_t* dir : {L"rebase-merge", L"rebase-apply"}) {
            RunResult gp = RunGit(repo, {L"rev-parse", L"--git-path", dir}, log, false);
            std::wstring path = FromUtf8(TrimA(gp.output));
            if (gp.exitCode == 0 && !path.empty() && (DirExists(path) || DirExists(RepoPath(repo, path)))) rebasing = true;
        }
        if (rebasing) {
            RunGit(repo, {L"rebase", L"--abort"}, log);
            return {GitOutcome::Conflict,
                    L"Some files were changed both on GitHub and here, so they couldn't be combined automatically. "
                    L"Nothing was changed. Open the folder in GitHub Desktop to resolve the conflict."};
        }
        if (NoIdentity(p)) return CommitFailed(p);
        if (LooksLikeAuthError(p.output)) return {GitOutcome::Failed, kAuthHint};
        return {GitOutcome::Failed, L"git pull failed — see the log."};
    }
    return {GitOutcome::Done, L"Pulled the latest changes from GitHub. Now click \u201cPush changes\u201d."};
}
