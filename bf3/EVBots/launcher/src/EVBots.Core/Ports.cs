using System.Net;
using System.Net.Sockets;

namespace EVBots.Core;

/// <summary>Порты стенда. Менять только синхронно с бэкендом и server args.</summary>
public static class Ports
{
    /// <summary>Игровой сервер (BF3 -GamePort).</summary>
    public const int Game = 25300;

    /// <summary>HTTP-API бэкенда (аккаунты, серверы, админка инстанса).</summary>
    public const int Http = 8080;

    /// <summary>Blaze (логин, лобби).</summary>
    public const int Blaze = 10041;

    /// <summary>Редиректор gosredirector.</summary>
    public const int Redirector = 42127;

    /// <summary>QoS бэкенда (значение по умолчанию --qos-port).</summary>
    public const int Qos = 17554;

    /// <summary>Remote admin (RCON), только 127.0.0.1.</summary>
    public const int Rcon = 47900;

    /// <summary>UDP-объявления лаунчеров в LAN (широковещательная рассылка).</summary>
    public const int Discovery = 47990;

    /// <summary>Хосты EA, которые и сервер, и клиент перенаправляют на бэкенд.</summary>
    public const string Sinkhole =
        "gosredirector.ea.com,gosredirector.online.ea.com,gosredirector.scert.ea.com,gosredirector.stest.ea.com";

    /// <summary>
    /// 20.10: серверу ещё bf3.gos.ea.com — за ним server.xml (бэкенд отдаёт его на 127.0.0.1:80, EditorialHttp).
    /// </summary>
    public const string ServerSinkhole = Sinkhole + ",bf3.gos.ea.com";

    /// <summary>
    /// Проверка занятости порта пробным биндом. Возвращает null, если порт свободен,
    /// иначе — короткую причину. Ничего не запускает и не останавливает.
    /// </summary>
    public static string? Busy(int port, bool tcp)
    {
        try
        {
            if (tcp)
            {
                var listener = new TcpListener(IPAddress.Any, port);
                listener.Start();
                listener.Stop();
            }
            else
            {
                using var udp = new UdpClient(port);
            }

            return null;
        }
        catch (SocketException)
        {
            return "занят";
        }
    }
}
