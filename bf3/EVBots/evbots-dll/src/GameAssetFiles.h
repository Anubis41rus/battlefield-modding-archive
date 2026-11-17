#pragma once
#include <windows.h>
#include <shlwapi.h>
#include <cstring>
#include <iterator>
#include "GameAssetRoutes.h"

namespace evbots {
inline bool ReadCachedGameAsset(const std::string& route, std::string* body, std::string* err) {
    std::wstring relative;
    if (!GameAssetRelativePngPath(route, &relative)) { *err = "invalid icon route"; return false; }
    wchar_t root[32768];
    const DWORD count = ::GetEnvironmentVariableW(L"EVBOTS_UI_ASSETS", root, static_cast<DWORD>(std::size(root)));
    if (count == 0 || count >= std::size(root)) { *err = "EVBOTS_UI_ASSETS missing"; return false; }
    if (::PathIsRelativeW(root)) { *err = "icon cache must be absolute"; return false; }
    std::wstring path(root);
    if (path.back() != L'\\' && path.back() != L'/') path += L'\\';
    path += relative;
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { *err = "icon absent from cache"; return false; }
    LARGE_INTEGER size{};
    bool ok = ::GetFileSizeEx(file, &size) && size.QuadPart >= 8 && size.QuadPart <= 8 * 1024 * 1024;
    if (ok) {
        body->resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = ::ReadFile(file, body->data(), static_cast<DWORD>(body->size()), &read, nullptr) && read == body->size();
        static const unsigned char png[] = {137, 80, 78, 71, 13, 10, 26, 10};
        if (ok) ok = memcmp(body->data(), png, sizeof(png)) == 0;
    }
    ::CloseHandle(file);
    if (!ok) { body->clear(); *err = "invalid cached PNG"; }
    return ok;
}
}
