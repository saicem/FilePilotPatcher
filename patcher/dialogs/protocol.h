#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <algorithm>
#include <cstring>
#include <shobjidl.h>
#include <stdexcept>
#include <string>
#include <vector>
#include <windows.h>

namespace fpp {
constexpr DWORD Magic = 0x31505046, Version = 1, Capacity = 512 * 1024;
enum class Mode : DWORD { Open, Save, Folder };
enum class Event : LONG { Selection = 1, Folder, Type, Accept, Cancel };
constexpr UINT ControlMessage = WM_APP + 31;
enum Control : DWORD {
    Query = 0,
    Name = 1,
    Title = 2,
    OkLabel = 4,
    TypeIndex = 8,
    Extension = 16,
    Folder = 32
};
struct Shared {
    DWORD magic, version, ownerPid, helperPid;
    UINT64 ownerWindow, helperWindow;
    DWORD requestBytes, eventBytes, replyBytes;
    LONG kind;
    HRESULT reply;
    DWORD controlBytes;
    BYTE request[Capacity], event[Capacity], replyText[Capacity], control[Capacity];
};
struct Filter {
    std::wstring label, pattern;
};
struct Request {
    Mode mode = Mode::Open;
    DWORD options = FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST | FOS_NOCHANGEDIR, index = 1;
    std::wstring title, folder, filename, extension, okLabel;
    std::vector<Filter> filters;
    bool showReadOnly = false, readOnly = false;
    // Used only by the API conformance harness, never set by intercepted apps.
    std::vector<std::wstring> testPaths;
    bool testCancel = false;
};
struct Result {
    std::vector<std::wstring> paths;
    std::wstring folder;
    DWORD index = 1;
    bool readOnly = false;
};
struct Writer {
    std::vector<BYTE> data;
    void number(DWORD n) {
        if (data.size() + 4 > Capacity)
            throw std::runtime_error("request too large");
        auto p = (BYTE *)&n;
        data.insert(data.end(), p, p + 4);
    }
    void string(const std::wstring &s) {
        if (s.size() > 32767 || data.size() + 4 + s.size() * 2 > Capacity)
            throw std::runtime_error("string too large");
        number((DWORD)s.size());
        auto p = (const BYTE *)s.data();
        data.insert(data.end(), p, p + s.size() * 2);
    }
};
struct Reader {
    const BYTE *data;
    size_t size, pos = 0;
    Reader(const BYTE *d, size_t n) : data(d), size(n) {
        if (n > Capacity)
            throw std::runtime_error("packet too large");
    }
    DWORD number() {
        if (pos + 4 > size)
            throw std::runtime_error("short packet");
        DWORD n;
        memcpy(&n, data + pos, 4);
        pos += 4;
        return n;
    }
    std::wstring string() {
        auto n = number();
        if (n > 32767 || pos + n * 2 > size)
            throw std::runtime_error("bad string");
        std::wstring s(n, L'\0');
        memcpy(s.data(), data + pos, n * 2);
        pos += n * 2;
        if (s.find(L'\0') != s.npos)
            throw std::runtime_error("embedded null");
        return s;
    }
    void end() {
        if (pos != size)
            throw std::runtime_error("trailing data");
    }
};
inline std::vector<BYTE> encode(const Request &q) {
    Writer w;
    w.number((DWORD)q.mode);
    w.number(q.options);
    w.number(q.index);
    w.string(q.title);
    w.string(q.folder);
    w.string(q.filename);
    w.string(q.extension);
    w.string(q.okLabel);
    if (q.filters.size() > 128)
        throw std::runtime_error("too many filters");
    w.number((DWORD)q.filters.size());
    for (auto &f : q.filters) {
        w.string(f.label);
        w.string(f.pattern);
    }
    if (q.testPaths.size() > 128)
        throw std::runtime_error("too many paths");
    w.number((DWORD)q.testPaths.size());
    for (auto &s : q.testPaths)
        w.string(s);
    w.number(q.testCancel);
    w.number(q.showReadOnly);
    w.number(q.readOnly);
    return w.data;
}
inline Request decodeRequest(const BYTE *p, size_t n) {
    Reader r(p, n);
    Request q;
    q.mode = (Mode)r.number();
    if (q.mode > Mode::Folder)
        throw std::runtime_error("bad mode");
    q.options = r.number();
    q.index = r.number();
    q.title = r.string();
    q.folder = r.string();
    q.filename = r.string();
    q.extension = r.string();
    q.okLabel = r.string();
    auto count = r.number();
    if (count > 128)
        throw std::runtime_error("too many filters");
    while (count--) {
        Filter f;
        f.label = r.string();
        f.pattern = r.string();
        q.filters.push_back(f);
    }
    count = r.number();
    if (count > 128)
        throw std::runtime_error("too many paths");
    while (count--)
        q.testPaths.push_back(r.string());
    q.testCancel = r.number() != 0;
    q.showReadOnly = r.number() != 0;
    q.readOnly = r.number() != 0;
    r.end();
    return q;
}
inline std::vector<BYTE> encode(const Result &q) {
    Writer w;
    w.number(q.index);
    w.string(q.folder);
    if (q.paths.size() > 128)
        throw std::runtime_error("too many paths");
    w.number((DWORD)q.paths.size());
    for (auto &p : q.paths)
        w.string(p);
    w.number(q.readOnly);
    return w.data;
}
inline Result decodeResult(const BYTE *p, size_t n) {
    Reader r(p, n);
    Result q;
    q.index = r.number();
    q.folder = r.string();
    auto count = r.number();
    if (count > 128)
        throw std::runtime_error("too many paths");
    while (count--)
        q.paths.push_back(r.string());
    q.readOnly = r.number() != 0;
    r.end();
    return q;
}
inline std::wstring parent(const std::wstring &path) {
    auto i = path.find_last_of(L"\\/");
    if (i == path.npos)
        return {};
    if (i == 2 && path[1] == L':')
        return path.substr(0, 3);
    return path.substr(0, i);
}
inline std::wstring leaf(const std::wstring &path) {
    auto i = path.find_last_of(L"\\/");
    return path.substr(i == path.npos ? 0 : i + 1);
}
inline std::wstring quote(const std::wstring &s) {
    std::wstring out = L"\"";
    unsigned slashes = 0;
    for (auto ch : s) {
        if (ch == L'\\') {
            ++slashes;
            continue;
        }
        if (ch == L'\"') {
            out.append(slashes * 2 + 1, L'\\');
            out += ch;
        } else {
            out.append(slashes, L'\\');
            out += ch;
        }
        slashes = 0;
    }
    out.append(slashes * 2, L'\\');
    out += L'\"';
    return out;
}
inline std::wstring full(const std::wstring &s, const std::wstring &folder) {
    auto input = s;
    if (!(s.size() > 1 && s[1] == L':') && s.substr(0, 2) != L"\\\\")
        input = folder + (folder.empty() || folder.back() == L'\\' ? L"" : L"\\") + s;
    std::vector<wchar_t> b(32768);
    auto n = GetFullPathNameW(input.c_str(), (DWORD)b.size(), b.data(), nullptr);
    if (!n || n >= b.size())
        throw std::runtime_error("invalid path");
    return {b.data(), n};
}
inline bool directory(const std::wstring &s) {
    auto a = GetFileAttributesW(s.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
inline DWORD envNumber(const wchar_t *name) {
    wchar_t b[32]{};
    GetEnvironmentVariableW(name, b, 32);
    return wcstoul(b, nullptr, 10);
}
struct Handles {
    HANDLE mapping = nullptr, ready = nullptr, event = nullptr, decision = nullptr,
           cancel = nullptr;
    Shared *shared = nullptr;
    ~Handles() {
        if (shared)
            UnmapViewOfFile(shared);
        for (auto h : {mapping, ready, event, decision, cancel})
            if (h)
                CloseHandle(h);
    }
};
} // namespace fpp
