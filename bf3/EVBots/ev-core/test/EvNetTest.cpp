// EvNetTest.cpp — проверка канала ev-core (К4) в одном процессе: сервер на свободном порту, клиент к 127.0.0.1,
// кадры в обе стороны, рассылка, кадр с нулевыми байтами и пустой кадр. Запуск: build\test\EvNetTest.exe build\ev-core.dll
// ev-core 4: порядок 2000 кадров подряд; «зависший» клиент (сокет без чтения) не держит отправку, переполнение его очереди
// закрывает только его соединение.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

using Handler = void(__cdecl*)(int, uint32_t, int, const char*, uint32_t);
using SetHandlerFn = void(__cdecl*)(Handler);
using ServerStartFn = int(__cdecl*)(int);
using ClientConnectFn = int(__cdecl*)(const char*, int);
using SendFn = int(__cdecl*)(uint32_t, const char*, uint32_t);
using BroadcastFn = int(__cdecl*)(const char*, uint32_t);

struct Event {
    int role;
    uint32_t conn;
    int kind;
    std::string data;
};

std::mutex g_gate;
std::vector<Event> g_events;
int g_fail = 0;

void __cdecl OnNet(int role, uint32_t conn, int kind, const char* data, uint32_t len) {
    std::lock_guard<std::mutex> lock(g_gate);
    g_events.push_back({role, conn, kind, std::string(data != nullptr ? data : "", len)});
}

bool WaitFor(int role, int kind, const std::string* data, Event* out, int ms = 5000) {
    for (int t = 0; t < ms; t += 10) {
        {
            std::lock_guard<std::mutex> lock(g_gate);
            for (size_t i = 0; i < g_events.size(); ++i) {
                const Event& e = g_events[i];
                if (e.role == role && e.kind == kind && (data == nullptr || e.data == *data)) {
                    if (out != nullptr) *out = e;
                    g_events.erase(g_events.begin() + static_cast<long>(i));
                    return true;
                }
            }
        }
        ::Sleep(10);
    }
    return false;
}

void Check(bool ok, const char* what) {
    printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
    if (!ok) ++g_fail;
}

}  // namespace

int main(int argc, char** argv) {
    const char* dllPath = argc > 1 ? argv[1] : "build\\ev-core.dll";
    printf("=== канал ev-core (К4) ===\n");
    HMODULE m = ::LoadLibraryA(dllPath);
    Check(m != nullptr, "ev-core.dll загружен");
    if (m == nullptr) return 1;
    auto setHandler = reinterpret_cast<SetHandlerFn>(::GetProcAddress(m, "EvNetSetHandler"));
    auto serverStart = reinterpret_cast<ServerStartFn>(::GetProcAddress(m, "EvNetServerStart"));
    auto clientConnect = reinterpret_cast<ClientConnectFn>(::GetProcAddress(m, "EvNetClientConnect"));
    auto send = reinterpret_cast<SendFn>(::GetProcAddress(m, "EvNetSend"));
    auto broadcast = reinterpret_cast<BroadcastFn>(::GetProcAddress(m, "EvNetBroadcast"));
    Check(setHandler && serverStart && clientConnect && send && broadcast, "экспорт EvNet* найден");
    if (!(setHandler && serverStart && clientConnect && send && broadcast)) return 1;

    setHandler(&OnNet);
    const int port = serverStart(0);
    Check(port > 0, "сервер слушает свободный порт");
    Check(clientConnect("127.0.0.1", port) == 0, "клиент начал подключение");

    Event srvOpen{}, cliOpen{};
    Check(WaitFor(0, 1, nullptr, &srvOpen), "сервер: соединение открыто");
    Check(WaitFor(1, 1, nullptr, &cliOpen), "клиент: соединение открыто");
    const uint32_t conn = srvOpen.conn;
    Check(conn >= 1, "номер соединения на сервере >= 1");

    const std::string hello("hello\0мир", 13);
    Check(send(0, hello.data(), static_cast<uint32_t>(hello.size())) == 1, "клиент -> сервер отправлено");
    Check(WaitFor(0, 2, &hello, nullptr), "сервер получил кадр байт в байт (с нулевым байтом)");

    const std::string reply = "reply";
    Check(send(conn, reply.data(), static_cast<uint32_t>(reply.size())) == 1, "сервер -> клиент отправлено");
    Check(WaitFor(1, 2, &reply, nullptr), "клиент получил ответ");

    const std::string all = "broadcast";
    Check(broadcast(all.data(), static_cast<uint32_t>(all.size())) == 1, "рассылка ушла одному соединению");
    Check(WaitFor(1, 2, &all, nullptr), "клиент получил рассылку");

    const std::string empty;
    Check(send(0, empty.data(), 0) == 1, "пустой кадр отправлен");
    Check(WaitFor(0, 2, &empty, nullptr), "сервер получил пустой кадр");

    std::string big(200000, 'x');
    big[123456] = 'y';
    Check(send(0, big.data(), static_cast<uint32_t>(big.size())) == 1, "большой кадр 200 КБ отправлен");
    Check(WaitFor(0, 2, &big, nullptr), "сервер получил большой кадр целиком");

    Check(send(999, reply.data(), 5) == 0, "неизвестное соединение — 0, без падения");

    // ev-core 4: порядок кадров через очередь
    bool sentAll = true;
    for (int i = 0; i < 2000; ++i) {
        const std::string f = "seq" + std::to_string(i);
        sentAll = sentAll && send(0, f.data(), static_cast<uint32_t>(f.size())) == 1;
    }
    Check(sentAll, "2000 кадров подряд поставлены в очередь");
    bool inOrder = true;
    for (int i = 0; i < 2000 && inOrder; ++i) {
        Event e{};
        if (!WaitFor(0, 2, nullptr, &e)) { inOrder = false; break; }
        inOrder = e.data == "seq" + std::to_string(i);
    }
    Check(inOrder, "сервер получил 2000 кадров по порядку");

    // ev-core 4: «зависший» клиент — подключился и не читает. Отправка ему не должна ждать сеть.
    WSADATA wsa{};
    ::WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET stuck = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<u_short>(port));
    ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    Check(stuck != INVALID_SOCKET && ::connect(stuck, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0,
          "«зависший» клиент подключился");
    Event stuckOpen{};
    Check(WaitFor(0, 1, nullptr, &stuckOpen), "сервер: соединение «зависшего» открыто");
    const std::string mb(1u << 20, 'z');
    const ULONGLONG t0 = ::GetTickCount64();
    int queued = 0;
    for (int i = 0; i < 40; ++i) queued += send(stuckOpen.conn, mb.data(), static_cast<uint32_t>(mb.size()));
    const ULONGLONG dt = ::GetTickCount64() - t0;
    printf("        40 МБ «зависшему»: в очереди %d кадров, вызовы заняли %llu мс\n", queued, dt);
    Check(dt < 1000, "40 кадров по 1 МБ «зависшему» не ждут сеть (< 1 с)");
    Check(queued > 0 && queued < 40, "очередь «зависшего» ограничена (часть кадров отклонена)");
    Event stuckClose{};
    bool closed = false;
    for (int t = 0; t < 5000 && !closed; t += 10) {
        if (WaitFor(0, 3, nullptr, &stuckClose, 10)) closed = stuckClose.conn == stuckOpen.conn;
    }
    Check(closed, "соединение «зависшего» закрыто сервером");
    Check(send(stuckOpen.conn, reply.data(), 5) == 0, "после закрытия отправка «зависшему» — 0");
    Check(send(conn, reply.data(), static_cast<uint32_t>(reply.size())) == 1 && WaitFor(1, 2, &reply, nullptr),
          "живой клиент после этого получает кадры");
    ::closesocket(stuck);

    printf(g_fail == 0 ? "=== ИТОГ: всё прошло ===\n" : "=== ИТОГ: провалено %d ===\n", g_fail);
    fflush(stdout);
    ::ExitProcess(g_fail == 0 ? 0 : 1);  // потоки ev-core не останавливаются — выходим сразу
}
