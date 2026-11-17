// EvCore.h — общее для файлов ev-core.dll.
#pragma once

#include <cstdint>

void FileLog(const char* fmt, ...);

namespace evcore {

constexpr int kHarmonyDefaultPort = 7948;  // как Harmony у VU

constexpr int kEvNetRoleServer = 0;
constexpr int kEvNetRoleClient = 1;
constexpr int kEvNetOpen = 1;
constexpr int kEvNetData = 2;
constexpr int kEvNetClose = 3;

/// Обработчик канала: роль (0 сервер, 1 клиент), соединение (у клиента 0), вид (1 открыто, 2 данные, 3 закрыто),
/// данные (для «открыто» — адрес собеседника). Зовётся из потоков ev-core — не трогать Lua прямо в нём.
using EvNetHandler = void(__cdecl*)(int role, uint32_t conn, int kind, const char* data, uint32_t len);

int NetServerStart(int port);
int NetClientConnect(const char* host, int port);
int NetSend(uint32_t conn, const char* data, uint32_t len);
int NetBroadcast(const char* data, uint32_t len);
void NetSetHandler(EvNetHandler h);
bool NetClientConnected();

/// Частота 30/60/120 Гц как у VU (EvFreq.cpp). Из DllMain: сервер — сразу (процесс подвешен), клиент — после расшифровки.
void FreqInit();

}  // namespace evcore
