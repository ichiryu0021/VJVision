// UTF-8 <-> filesystem path helpers. All strings stored in the DB / QML /
// JSON are UTF-8. On Windows, std::filesystem's narrow-string API uses the
// ANSI codepage and mangles non-ASCII paths, so always cross through
// u8path()/u8string() or explicit wide conversion.
#pragma once
#include <filesystem>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace vj::pathutil {

inline std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

inline std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                        s.data(), n, nullptr, nullptr);
    return s;
}

inline std::filesystem::path fromUtf8(const std::string& s) {
    return std::filesystem::u8path(s);
}

inline std::string toUtf8(const std::filesystem::path& p) {
    return wideToUtf8(p.wstring());
}

} // namespace vj::pathutil
