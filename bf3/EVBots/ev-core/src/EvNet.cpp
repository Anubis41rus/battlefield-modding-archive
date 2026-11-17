// EvNet.cpp — канал клиент↔сервер для NetEvents (К4), аналог Harmony у VU.
//
// У VU канал живёт в vu-core (RTTI MonitoredHarmony, RakNet, «Monitored Harmony server listening on 0.0.0.0:7948» в окне
// сервера VU). У нас — в ev-core.dll, свой простой транспорт: TCP (надёжно и по порядку — как NetEvents по умолчанию у VU;
// Unreliable-варианты идут тем же каналом, в LAN это не мешает), кадр «u32 LE длина | данные». Что в данных — решает
// EVBots.dll (имя события и аргументы NetEvents, приветствие клиента); ev-core только возит байты.
//
// Сервер (bf3_server_Final.exe): слушает 0.0.0.0:7948 (EVCORE_HARMONY_PORT), каждому клиенту — номер соединения 1, 2, …
// Клиент (bf3.exe): EvNetClientConnect — подключиться к серверу (адрес из -requestStateParams serveraddr=…), при обрыве —
// переподключение. Приём — в потоках ev-core, обработчик EVBots.dll (EvNetSetHandler) сам кладёт данные в очередь и отдаёт
// их Lua на тике движка.
//
// ★ ev-core 4 (25.10): отправка — через очередь. До этого EvNetSend звал send() прямо в потоке движка (тик сервера, кадр
// клиента) с таймаутом 3 с: клиент с плохим Wi-Fi мог остановить тик сервера для всех (гипотеза жалобы друга «колом у
// всех», MODLOG «Жалоба друга 24–25.10»). Теперь у каждого соединения своя очередь кадров и поток-отправитель (у VU —
// RakNet со своей очередью, реестр Г1). EvNetSend/EvNetBroadcast только кладут кадр и возвращаются; порядок кадров прежний.
// Очередь больше kMaxQueued байт (клиент не читает) или ошибка send — соединение закрывается, клиент переподключится.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "EvCore.h"

namespace evcore {
namespace {

constexpr uint32_t kMaxFrame = 4u << 20;  // 4 МБ — с запасом для таблиц NetEvents
constexpr size_t kMaxQueued = 16u << 20;  // 16 МБ в очереди одного соединения — дальше клиент считается зависшим

std::atomic<EvNetHandler> g_handler{nullptr};
std::once_flag g_wsaOnce;
bool g_wsaOk = false;

bool WsaInit() {
    std::call_once(g_wsaOnce, [] {
        WSADATA d;
        g_wsaOk = ::WSAStartup(MAKEWORD(2, 2), &d) == 0;
    });
    return g_wsaOk;
}

void Notify(int role, uint32_t conn, int kind, const char* data, uint32_t len) {
    if (EvNetHandler h = g_handler.load()) h(role, conn, kind, data, len);
}

bool SendAll(SOCKET s, const char* p, int n) {
    while (n > 0) {
        const int r = ::send(s, p, n, 0);
        if (r <= 0) return false;
        p += r;
        n -= r;
    }
    return true;
}

bool RecvAll(SOCKET s, char* p, int n) {
    while (n > 0) {
        const int r = ::recv(s, p, n, 0);
        if (r <= 0) return false;
        p += r;
        n -= r;
    }
    return true;
}

bool SendFrame(SOCKET s, const std::string& frame) {
    const uint32_t le = static_cast<uint32_t>(frame.size());  // x86 — little-endian
    return SendAll(s, reinterpret_cast<const char*>(&le), 4) &&
           (frame.empty() || SendAll(s, frame.data(), static_cast<int>(frame.size())));
}

/// Очередь отправки одного соединения; кадры шлёт свой поток (SenderLoop), вызывающий не ждёт сеть.
struct Outbox {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::string> q;
    size_t bytes = 0;
    bool closed = false;
    SOCKET sock = INVALID_SOCKET;
    std::string who;  // для журнала
};

/// Закрыть очередь (кадры выбрасываются) и разбудить отправителя. shutdown + CancelIoEx — чтобы ReadLoop и send этого
/// сокета вышли сразу (shutdown в Windows не прерывает уже идущие блокирующие recv/send — тест ev-core 4).
void CloseOutbox(Outbox& ob, bool shutdownSocket) {
    {
        std::lock_guard<std::mutex> lock(ob.m);
        ob.closed = true;
        ob.q.clear();
        ob.bytes = 0;
    }
    ob.cv.notify_all();
    if (shutdownSocket) {
        ::shutdown(ob.sock, SD_BOTH);
        ::CancelIoEx(reinterpret_cast<HANDLE>(ob.sock), nullptr);
    }
}

/// Положить кадр в очередь. false — кадр слишком велик, очередь закрыта или переполнена (тогда соединение закрывается).
bool Enqueue(Outbox& ob, const char* data, uint32_t len) {
    if (len > kMaxFrame) return false;
    bool overflow = false;
    {
        std::lock_guard<std::mutex> lock(ob.m);
        if (ob.closed) return false;
        if (ob.bytes + len + 4 > kMaxQueued) {
            overflow = true;
        } else {
            ob.q.emplace_back(data != nullptr ? data : "", len);
            ob.bytes += len + 4;
        }
    }
    if (overflow) {
        FileLog("Harmony: %s — очередь отправки больше %u МБ (не читает), соединение закрываем", ob.who.c_str(),
                static_cast<unsigned>(kMaxQueued >> 20));
        CloseOutbox(ob, true);
        return false;
    }
    ob.cv.notify_one();
    return true;
}

void SenderLoop(std::shared_ptr<Outbox> ob) {
    for (;;) {
        std::string frame;
        {
            std::unique_lock<std::mutex> lock(ob->m);
            ob->cv.wait(lock, [&] { return ob->closed || !ob->q.empty(); });
            if (ob->closed) return;
            frame.swap(ob->q.front());
            ob->q.pop_front();
            ob->bytes -= frame.size() + 4;
        }
        if (!SendFrame(ob->sock, frame)) {
            FileLog("Harmony: %s — отправка не прошла (WSA %d), соединение закрываем", ob->who.c_str(), ::WSAGetLastError());
            CloseOutbox(*ob, true);
            return;
        }
    }
}

/// Новая очередь с потоком-отправителем для сокета; поток возвращается, его надо дождаться (join) до closesocket.
std::shared_ptr<Outbox> StartOutbox(SOCKET s, const std::string& who, std::thread& sender) {
    auto ob = std::make_shared<Outbox>();
    ob->sock = s;
    ob->who = who;
    sender = std::thread(SenderLoop, ob);
    return ob;
}

/// Читает кадры, пока соединение живо; каждый — обработчику.
void ReadLoop(SOCKET s, int role, uint32_t conn) {
    std::vector<char> buf;
    for (;;) {
        uint32_t len = 0;
        if (!RecvAll(s, reinterpret_cast<char*>(&len), 4) || len > kMaxFrame) break;
        buf.resize(len);
        if (len > 0 && !RecvAll(s, buf.data(), static_cast<int>(len))) break;
        Notify(role, conn, kEvNetData, buf.data(), len);
    }
}

void TuneSocket(SOCKET s) {
    BOOL on = TRUE;
    ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
    // send идёт в потоке-отправителе (ev-core 4), движок его не ждёт; дольше 15 с не уходит кадр — клиент считается потерянным
    DWORD sndTimeout = 15000;
    ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&sndTimeout), sizeof(sndTimeout));
}

// ── сервер ───────────────────────────────────────────────────────────────────────────────────────────────────────

struct Peer {
    SOCKET sock = INVALID_SOCKET;
    std::string addr;
    std::shared_ptr<Outbox> out;
    std::thread sender;
};

std::mutex g_peersGate;
std::map<uint32_t, std::shared_ptr<Peer>> g_peers;
std::atomic<uint32_t> g_nextConn{1};
SOCKET g_listen = INVALID_SOCKET;
std::atomic<int> g_listenPort{0};

void ServePeer(uint32_t conn, std::shared_ptr<Peer> peer) {
    Notify(kEvNetRoleServer, conn, kEvNetOpen, peer->addr.data(), static_cast<uint32_t>(peer->addr.size()));
    ReadLoop(peer->sock, kEvNetRoleServer, conn);
    {
        std::lock_guard<std::mutex> lock(g_peersGate);
        g_peers.erase(conn);
    }
    CloseOutbox(*peer->out, true);
    if (peer->sender.joinable()) peer->sender.join();
    ::closesocket(peer->sock);
    Notify(kEvNetRoleServer, conn, kEvNetClose, nullptr, 0);
    FileLog("Harmony: соединение %u (%s) закрыто", conn, peer->addr.c_str());
}

void AcceptLoop() {
    for (;;) {
        sockaddr_in from{};
        int fl = sizeof(from);
        SOCKET s = ::accept(g_listen, reinterpret_cast<sockaddr*>(&from), &fl);
        if (s == INVALID_SOCKET) {
            if (g_listen == INVALID_SOCKET) return;
            ::Sleep(100);
            continue;
        }
        TuneSocket(s);
        auto peer = std::make_shared<Peer>();
        peer->sock = s;
        char ip[64] = "?";
        ::inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
        peer->addr = std::string(ip) + ":" + std::to_string(ntohs(from.sin_port));
        const uint32_t conn = g_nextConn.fetch_add(1);
        peer->out = StartOutbox(s, "соединение " + std::to_string(conn) + " (" + peer->addr + ")", peer->sender);
        {
            std::lock_guard<std::mutex> lock(g_peersGate);
            g_peers[conn] = peer;
        }
        FileLog("Harmony: соединение %u от %s", conn, peer->addr.c_str());
        std::thread(ServePeer, conn, peer).detach();
    }
}

// ── клиент ───────────────────────────────────────────────────────────────────────────────────────────────────────

std::mutex g_clientGate;  // защищает g_clientOut
std::shared_ptr<Outbox> g_clientOut;  // очередь текущего соединения с сервером; nullptr — не подключены
std::atomic<bool> g_clientStarted{false};
std::atomic<bool> g_clientUp{false};

void ClientLoop(std::string host, int port) {
    int attempt = 0;
    for (;;) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        SOCKET s = INVALID_SOCKET;
        if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) == 0 && res != nullptr) {
            s = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
            if (s != INVALID_SOCKET && ::connect(s, res->ai_addr, static_cast<int>(res->ai_addrlen)) != 0) {
                ::closesocket(s);
                s = INVALID_SOCKET;
            }
            ::freeaddrinfo(res);
        }
        if (s == INVALID_SOCKET) {
            if (attempt++ % 15 == 0) FileLog("Harmony: клиент не подключился к %s:%d — повтор", host.c_str(), port);
            ::Sleep(2000);  // сеть: сервер мог ещё не подняться — ждём и повторяем
            continue;
        }
        TuneSocket(s);
        attempt = 0;
        std::thread sender;
        auto out = StartOutbox(s, "клиент → " + host, sender);
        {
            std::lock_guard<std::mutex> lock(g_clientGate);
            g_clientOut = out;
        }
        g_clientUp = true;
        FileLog("Harmony: клиент подключён к %s:%d", host.c_str(), port);
        Notify(kEvNetRoleClient, 0, kEvNetOpen, host.data(), static_cast<uint32_t>(host.size()));
        ReadLoop(s, kEvNetRoleClient, 0);
        g_clientUp = false;
        {
            std::lock_guard<std::mutex> lock(g_clientGate);
            g_clientOut.reset();
        }
        CloseOutbox(*out, true);
        sender.join();
        ::closesocket(s);
        Notify(kEvNetRoleClient, 0, kEvNetClose, nullptr, 0);
        FileLog("Harmony: клиент потерял соединение с %s:%d — переподключаемся", host.c_str(), port);
        ::Sleep(1000);
    }
}

/// Адрес сервера из командной строки игры: -requestStateParams "<data … serveraddr=\"IP:PORT\" …/>" — берём IP.
std::string ServerHostFromCommandLine() {
    const std::string cmd = ::GetCommandLineA();
    const size_t k = cmd.find("serveraddr=");
    if (k == std::string::npos) return {};
    size_t i = k + std::strlen("serveraddr=");
    while (i < cmd.size() && (cmd[i] == '"' || cmd[i] == '\\')) ++i;
    size_t j = i;
    while (j < cmd.size() && cmd[j] != ':' && cmd[j] != '"' && cmd[j] != '\\' && cmd[j] != ' ') ++j;
    return cmd.substr(i, j - i);
}

int HarmonyPort() {
    char v[16]{};
    const DWORD n = ::GetEnvironmentVariableA("EVCORE_HARMONY_PORT", v, sizeof(v));
    const int p = (n > 0 && n < sizeof(v)) ? std::atoi(v) : 0;
    return p > 0 && p < 65536 ? p : kHarmonyDefaultPort;
}

}  // namespace

int NetServerStart(int port) {
    if (g_listen != INVALID_SOCKET) return g_listenPort.load();
    if (!WsaInit()) return -1;
    if (port < 0) port = HarmonyPort();
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return -1;
    BOOL reuse = FALSE;
    ::setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(static_cast<u_short>(port));
    if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || ::listen(s, 16) != 0) {
        FileLog("Harmony: не удалось слушать порт %d (WSA %d)", port, ::WSAGetLastError());
        ::closesocket(s);
        return -1;
    }
    sockaddr_in bound{};
    int bl = sizeof(bound);
    ::getsockname(s, reinterpret_cast<sockaddr*>(&bound), &bl);
    g_listenPort = ntohs(bound.sin_port);
    g_listen = s;
    std::thread(AcceptLoop).detach();
    FileLog("Harmony: сервер слушает 0.0.0.0:%d", g_listenPort.load());
    return g_listenPort.load();
}

int NetClientConnect(const char* host, int port) {
    if (g_clientStarted.exchange(true)) return 1;
    if (!WsaInit()) return -1;
    std::string h = (host != nullptr && *host != '\0') ? host : ServerHostFromCommandLine();
    if (h.empty()) {
        FileLog("Harmony: адрес сервера не найден (нет serveraddr в командной строке) — канала не будет");
        g_clientStarted = false;
        return -1;
    }
    if (port <= 0) port = HarmonyPort();
    FileLog("Harmony: клиент подключается к %s:%d", h.c_str(), port);
    std::thread(ClientLoop, h, port).detach();
    return 0;
}

// 1 — кадр поставлен в очередь соединения (уйдёт потоком-отправителем), 0 — соединения нет / закрыто / переполнено.
int NetSend(uint32_t conn, const char* data, uint32_t len) {
    std::shared_ptr<Outbox> out;
    if (conn == 0) {  // клиент -> сервер
        std::lock_guard<std::mutex> lock(g_clientGate);
        out = g_clientOut;
    } else {
        std::lock_guard<std::mutex> lock(g_peersGate);
        auto it = g_peers.find(conn);
        if (it != g_peers.end()) out = it->second->out;
    }
    return out != nullptr && Enqueue(*out, data, len) ? 1 : 0;
}

int NetBroadcast(const char* data, uint32_t len) {
    std::vector<std::shared_ptr<Outbox>> outs;
    {
        std::lock_guard<std::mutex> lock(g_peersGate);
        for (auto& kv : g_peers) outs.push_back(kv.second->out);
    }
    int n = 0;
    for (auto& o : outs) n += Enqueue(*o, data, len) ? 1 : 0;
    return n;
}

void NetSetHandler(EvNetHandler h) { g_handler = h; }

bool NetClientConnected() { return g_clientUp.load(); }

}  // namespace evcore

// ── экспорт для EVBots.dll ───────────────────────────────────────────────────────────────────────────────────────

extern "C" __declspec(dllexport) void __cdecl EvNetSetHandler(evcore::EvNetHandler h) { evcore::NetSetHandler(h); }
/// Сервер: начать слушать (port < 0 — EVCORE_HARMONY_PORT или 7948, 0 — любой свободный); вернёт порт или -1.
extern "C" __declspec(dllexport) int __cdecl EvNetServerStart(int port) { return evcore::NetServerStart(port); }
/// Клиент: подключиться (host = nullptr — из командной строки игры; port <= 0 — по умолчанию).
extern "C" __declspec(dllexport) int __cdecl EvNetClientConnect(const char* host, int port) {
    return evcore::NetClientConnect(host, port);
}
/// conn = 0 — клиент -> сервер; иначе сервер -> соединение conn. 1 — отправлено.
extern "C" __declspec(dllexport) int __cdecl EvNetSend(uint32_t conn, const char* data, uint32_t len) {
    return evcore::NetSend(conn, data, len);
}
/// Сервер -> все соединения; вернёт, скольким ушло.
extern "C" __declspec(dllexport) int __cdecl EvNetBroadcast(const char* data, uint32_t len) {
    return evcore::NetBroadcast(data, len);
}
extern "C" __declspec(dllexport) int __cdecl EvNetClientConnected() { return evcore::NetClientConnected() ? 1 : 0; }
