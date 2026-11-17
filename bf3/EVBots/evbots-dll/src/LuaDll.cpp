// LuaDll.cpp — Lua 5.4.2 в отдельной lua.dll, как у VU (vubin\lua.dll; veniceext.dll берёт из неё 74 функции).
//
// Решение пользователя 23.10.2026: вынести интерпретатор из EVBots.dll — как у VU, EVBots.dll чище, Lua пригодится и
// другим модулям (ev-core). EVBots.dll связана с lua.dll отложенно (/DELAYLOAD:lua.dll): загрузчик внедряет нас по полному
// пути, а обычный поиск зависимостей смотрит в папку игры — поэтому lua.dll грузим сами из папки EVBots.dll (крючок
// отложенной загрузки). Загрузчик и лаунчер менять не нужно; lua.dll лежит рядом с EVBots.dll.

#include "LuaDll.h"

#include <windows.h>
#include <delayimp.h>

#include <string>

#include "EVBotsLog.h"

namespace evbots {
namespace {

std::wstring LuaDllPath() {
    wchar_t self[MAX_PATH]{};
    ::GetModuleFileNameW(gModule, self, MAX_PATH);
    std::wstring dir(self);
    const size_t slash = dir.find_last_of(L"\\/");
    dir = slash == std::wstring::npos ? L"." : dir.substr(0, slash);
    return dir + L"\\lua.dll";
}

FARPROC WINAPI DelayHook(unsigned notify, PDelayLoadInfo info) {
    if (notify == dliNotePreLoadLibrary && info != nullptr && info->szDll != nullptr &&
        _stricmp(info->szDll, "lua.dll") == 0) {
        return reinterpret_cast<FARPROC>(::LoadLibraryW(LuaDllPath().c_str()));
    }
    return nullptr;
}

/// SEH отдельно от C++-объектов: при неудаче отложенная загрузка бросает исключение.
HRESULT LoadAllLuaImports() {
    __try {
        return __HrLoadAllImportsForDll("lua.dll");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return E_FAIL;
    }
}

}  // namespace

bool EnsureLuaDll() {
    static const bool ok = [] {
        const HRESULT hr = LoadAllLuaImports();
        if (FAILED(hr)) {
            LOG_ERR("EVBots: lua.dll не загрузилась (ждём рядом с EVBots.dll: %ls) — Lua-режим не запускаем",
                    LuaDllPath().c_str());
            return false;
        }
        LOG_INFO("EVBots: Lua 5.4.2 — отдельная lua.dll (как у VU): %ls", LuaDllPath().c_str());
        return true;
    }();
    return ok;
}

}  // namespace evbots

extern "C" const PfnDliHook __pfnDliNotifyHook2 = evbots::DelayHook;
