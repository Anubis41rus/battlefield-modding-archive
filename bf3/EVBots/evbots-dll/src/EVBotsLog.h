// EVBotsLog.h — лог DLL в файл и в отладочный вывод.
//
// Пишем в <комплект>\logs\EVBots.log (каталог считается от места DLL,
// которая лежит в <комплект>\activation\). Плюс OutputDebugString, чтобы
// видеть из отладчика.

#pragma once

#include <Windows.h>

namespace evbots {

extern HMODULE gModule;

/// Инициализация. Вызывается из DllMain при загрузке.
void LogInit();

/// Закрытие лог-файла.
void LogClose();

/// Запись строки в лог.
void LogWrite(const char* level, const char* message);

void LogPrintf(const char* level, const char* fmt, ...);
// ★ Сборка 139: тестовые строки диагностики — в основной лог EVBots.log, как и остальные.
void LogTestWrite(const char* message);
void LogTestPrintf(const char* fmt, ...);

/// ★ Сборка 206: вывод мода (print, Console) — в отдельный файл logs\<мод>.log (fun-bots -> logs\fun-bots.log),
/// а не в EVBots.log. Пишет тот же поток-писатель; в консоль сервера строка идёт как раньше.
void LogModWrite(const char* mod, const char* level, const char* message);

/// ★ Сборка 217: строка в консоль окна сервера через ev-core.dll (EvCoreConsoleWrite) — как у VU, где veniceext берёт
/// логгер у vu-core (GetConsoleLogger). level: "info" | "warning" | "error"; source — "EVBots" или имя мода; текст UTF-8.
/// Нет ev-core.dll (клиент, тест-хост) — в консоль ничего; alsoLog — продублировать строку в EVBots.log (★).
void ConsoleWrite(const char* level, const char* source, const char* text, bool alsoLog = true);
void ConsolePrintf(const char* level, const char* source, const char* fmt, ...);

/// ★ Сборка 168: GetEnvironmentVariableA с кэшем (окружение за прогон не меняется). Та же семантика: длина значения,
/// 0 — переменной нет; значение длиннее cap-1 — возвращает длину, buf пустой. Для флагов, которые читаются на тиках.
DWORD EnvGetCached(const char* name, char* buf, DWORD cap);

}  // namespace evbots

#define LOG_INFO(...) ::evbots::LogPrintf("INFO", __VA_ARGS__)
#define LOG_WARN(...) ::evbots::LogPrintf("WARN", __VA_ARGS__)
#define LOG_ERR(...) ::evbots::LogPrintf("ERR ", __VA_ARGS__)