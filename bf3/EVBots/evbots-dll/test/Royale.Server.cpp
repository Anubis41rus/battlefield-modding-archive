// Запуск только серверной Lua в изолированной папке; не запускает Battlefield.
#include <Windows.h>
#include <cstdio>

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) { puts("Usage: Royale.Server.exe mods-root"); return 2; }
    SetEnvironmentVariableW(L"EVBOTS_MODS", argv[1]);
    SetEnvironmentVariableW(L"EVBOTS_NO_UPDATE_TIMER", L"1");
    SetEnvironmentVariableW(L"EVBOTS_TESTS", L"0");
    SetEnvironmentVariableW(L"EVBOTS_LOG_VERBOSE", L"1");
    HMODULE dll = LoadLibraryW(L"EVBots.dll");
    if (!dll) { printf("LoadLibrary failed %lu\n", GetLastError()); return 2; }
    auto get = reinterpret_cast<void*(__cdecl*)()>(GetProcAddress(dll, "GetModdingInterface"));
    if (!get) return 2;
    void* object = get();
    auto table = *reinterpret_cast<void***>(object);
    auto initialize = reinterpret_cast<bool(__thiscall*)(void*)>(table[0]);
    auto shutdown = reinterpret_cast<void(__thiscall*)(void*)>(table[4]);
    const bool started = initialize(object);
    printf("SERVER INIT %s\n", started ? "PASS" : "FAIL");
    shutdown(object);
    FreeLibrary(dll);
    return started ? 0 : 1;
}
