// marker.cpp — тестовая DLL для проверки загрузчика: при загрузке дописывает строку в файл из EVBOTS_MARKER_FILE
// (имя своей DLL, PID, имя процесса, были ли уже загружены activation.dll / marker1.dll). Только для test.cmd.
#include <windows.h>
#include <cstdio>

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    wchar_t out[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"EVBOTS_MARKER_FILE", out, MAX_PATH) == 0) return TRUE;
    wchar_t me[MAX_PATH] = {}, exe[MAX_PATH] = {}, mode[32] = {};
    GetModuleFileNameW(self, me, MAX_PATH);
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    GetEnvironmentVariableW(L"BF3ACT_MODE", mode, 32);
    const wchar_t* meName = wcsrchr(me, L'\\') ? wcsrchr(me, L'\\') + 1 : me;
    const bool m1 = GetModuleHandleW(L"marker1.dll") != nullptr;
    char line[1024];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "%S pid=%lu exe=%S BF3ACT_MODE=%S marker1_loaded=%d\r\n", meName,
                GetCurrentProcessId(), exe, mode, m1 ? 1 : 0);
    HANDLE h = CreateFileW(out, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        WriteFile(h, line, (DWORD)strlen(line), &wr, nullptr);
        CloseHandle(h);
    }
    return TRUE;
}
