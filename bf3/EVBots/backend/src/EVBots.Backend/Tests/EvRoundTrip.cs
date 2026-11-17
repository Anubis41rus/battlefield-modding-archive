// EvRoundTrip.cs — самопроверка EV-канала (E1): кадр байт в байт, порядок сессии, вход по паролю и по токену через
// настоящий TCP. Данные — во временном каталоге, рабочие профили не трогаем.
//
//   EV Backend ev-selftest

using System.Net;
using System.Net.Sockets;
using System.Text.Json.Nodes;
using EVBots.Backend.Data;
using EVBots.Backend.Ev;

namespace EVBots.Backend.Tests;

internal static class EvRoundTrip
{
    private static int _failures;

    public static int Run()
    {
        Console.WriteLine("=== самопроверка EV-канала ===");
        var tmp = Path.Combine(Path.GetTempPath(), "evbots-ev-selftest-" + Guid.NewGuid().ToString("N")[..8]);
        Directory.CreateDirectory(tmp);
        try
        {
            FrameCases();
            ServiceCases(tmp);
        }
        finally
        {
            try
            {
                Directory.Delete(tmp, recursive: true);
            }
            catch
            {
                // временный каталог — не страшно
            }
        }

        Console.WriteLine(_failures == 0 ? "=== EV: всё прошло ===" : $"=== EV: провалено {_failures} ===");
        return _failures == 0 ? 0 : 1;
    }

    private static void Check(bool ok, string what)
    {
        Console.WriteLine($"  [{(ok ? " OK " : "FAIL")}] {what}");
        if (!ok)
        {
            _failures++;
        }
    }

    private static void FrameCases()
    {
        var f = new EvFrame(EvIds.Utility, EvIds.Ping, 0x01020304, new JsonObject { ["a"] = "я" });
        var bytes = f.Encode();
        Check(bytes.Length == 12 + "{\"a\":\"я\"}"u8.Length, "кадр: длина = 4 + 8 + JSON");
        Check(bytes[0] == bytes.Length - 4 && bytes[4] == 0x7E && bytes[5] == 0xC5 && bytes[6] == 0x74 && bytes[7] == 0x88
              && bytes[8] == 0x04 && bytes[11] == 0x01, "кадр: LE длина/module/type/request_id");
        var back = EvFrame.Decode(bytes.AsSpan(4));
        Check(back.Module == f.Module && back.Type == f.Type && back.RequestId == f.RequestId
              && (string?)back.Body["a"] == "я", "кадр: разбор обратно");
        Check(EvFrame.Decode(bytes.AsSpan(4, 8)).Body.Count == 0, "кадр: пустое тело = {}");
        var threw = false;
        try
        {
            EvFrame.Decode(bytes.AsSpan(4, 7));
        }
        catch (InvalidDataException)
        {
            threw = true;
        }

        Check(threw, "кадр: короче заголовка — ошибка");
    }

    private static void ServiceCases(string tmp)
    {
        var log = new Log(false);
        var cfg = new ServeConfig
        {
            ProfilesDir = Path.Combine(tmp, "profiles"),
            ProgressDir = Path.Combine(tmp, "progress"),
            DataDir = tmp,
        };
        Directory.CreateDirectory(cfg.ProfilesDir);
        Directory.CreateDirectory(cfg.ProgressDir);
        var store = new ProfileStore(cfg, log);
        store.Load();
        var accounts = new AccountsStore(Path.Combine(cfg.ProfilesDir, "accounts.json"), store, log);
        accounts.Load();
        Check(accounts.Ensure("evtest", "pass123", "EvTester", out var acc, out var err), $"аккаунт создан ({err})");

        var service = new EvService(accounts, log);
        var listener = new EvListener(0, service, log);
        listener.Start();
        try
        {
            using var tcp = new TcpClient();
            tcp.Connect(IPAddress.Loopback, listener.Port);
            var s = tcp.GetStream();
            uint req = 0;

            JsonObject Call(ushort module, ushort type, JsonObject body, out ushort respType)
            {
                var r = ++req;
                s.Write(new EvFrame(module, type, r, body).Encode());
                var resp = EvFrame.ReadAsync(s, CancellationToken.None).GetAwaiter().GetResult()
                           ?? throw new IOException("EV: соединение закрыто");
                if (resp.RequestId != r)
                {
                    throw new InvalidDataException("EV: чужой request_id");
                }

                respType = resp.Type;
                return resp.Body;
            }

            var b = Call(EvIds.Authentication, EvIds.Login, new JsonObject { ["login"] = "evtest", ["password"] = "pass123" }, out var t);
            Check(t == EvIds.Error && (string?)b["code"] == "session-not-initialized", "до InitSession вход запрещён");

            b = Call(EvIds.Utility, EvIds.InitSession, new JsonObject { ["version"] = 99 }, out t);
            Check(t == EvIds.Error && (string?)b["code"] == "version", "чужая версия протокола — отказ");

            b = Call(EvIds.Utility, EvIds.InitSession, new JsonObject { ["version"] = EvService.ProtocolVersion }, out t);
            Check(t == EvIds.InitSession && (int?)b["version"] == EvService.ProtocolVersion, "InitSession");

            b = Call(EvIds.Manager, EvIds.RegisterSession, new JsonObject { ["role"] = "client" }, out t);
            Check(t == EvIds.RegisterSession && (string?)b["role"] == "client", "RegisterSession(client)");

            b = Call(EvIds.Authentication, EvIds.Login, new JsonObject { ["login"] = "evtest", ["password"] = "wrong" }, out t);
            Check(t == EvIds.Error && (string?)b["code"] == "login", "Login: неверный пароль — отказ");

            b = Call(EvIds.Authentication, EvIds.Login, new JsonObject { ["login"] = "evtest", ["password"] = "pass123" }, out t);
            var token = (string?)b["token"] ?? "";
            Check(t == EvIds.Login && token.Length == 32 && (ulong?)b["personaref"] == acc.Personaref,
                "Login: токен и та же персона, что у Blaze");

            // токен EV = токен Blaze: тот же AccountsStore разрешает его
            Check(accounts.ResolveToken(token)?.Login == "evtest", "токен EV понимает общая база (Blaze)");

            b = Call(EvIds.Authentication, EvIds.TokenLogin, new JsonObject { ["token"] = token }, out t);
            Check(t == EvIds.TokenLogin && (string?)b["login"] == "evtest", "TokenLogin");

            b = Call(EvIds.Authentication, EvIds.TokenLogin, new JsonObject { ["token"] = "00" }, out t);
            Check(t == EvIds.Error && (string?)b["code"] == "token", "TokenLogin: неизвестный токен — отказ");

            b = Call(EvIds.Utility, EvIds.Ping, new JsonObject(), out t);
            Check(t == EvIds.Ping && b["time"] is not null, "Ping");

            b = Call(0x1234, 0x5678, new JsonObject(), out t);
            Check(t == EvIds.Error && (string?)b["code"] == "unknown-message", "неизвестное сообщение — ошибка, не обрыв");

            ServerCases(listener.Port, token, service);
        }
        finally
        {
            listener.Stop();
        }
    }

    /// <summary>E2: сервер входит без ключа, игрок видит его в списке, входит; сервер разрешает/отказывает; выход.</summary>
    private static void ServerCases(int port, string token, EvService service)
    {
        using var srv = new TestClient(port);
        using var cli = new TestClient(port);
        srv.Call(EvIds.Utility, EvIds.InitSession, new JsonObject { ["version"] = EvService.ProtocolVersion }, out _);
        cli.Call(EvIds.Utility, EvIds.InitSession, new JsonObject { ["version"] = EvService.ProtocolVersion }, out _);

        var guid = Guid.NewGuid().ToString();
        var b = srv.Call(EvIds.Server, EvIds.ServerLogin, new JsonObject { ["guid"] = guid }, out var t);
        Check(t == EvIds.Error && (string?)b["code"] == "role", "ServerLogin без role=server — отказ");
        srv.Call(EvIds.Manager, EvIds.RegisterSession, new JsonObject { ["role"] = "server" }, out _);
        b = srv.Call(EvIds.Server, EvIds.ServerLogin, new JsonObject
        {
            ["guid"] = guid, ["name"] = "Тест-сервер", ["address"] = "127.0.0.1:25200", ["channelPort"] = 25201,
            ["mods"] = new JsonArray(new JsonObject { ["name"] = "fun-bots", ["version"] = "3.0", ["hash"] = "ab" }),
        }, out t);
        Check(t == EvIds.ServerLogin && (string?)b["guid"] == guid, "ServerLogin: по GUID инстанса, без ключа");

        cli.Call(EvIds.Manager, EvIds.RegisterSession, new JsonObject { ["role"] = "client" }, out _);
        b = cli.Call(EvIds.Server, EvIds.JoinServer, new JsonObject { ["guid"] = guid }, out t);
        Check(t == EvIds.Error && (string?)b["code"] == "auth", "JoinServer без входа — отказ");
        cli.Call(EvIds.Authentication, EvIds.TokenLogin, new JsonObject { ["token"] = token }, out _);

        b = cli.Call(EvIds.Server, EvIds.FetchServers, new JsonObject(), out t);
        var list = b["servers"] as JsonArray;
        Check(t == EvIds.FetchServers && list?.Count == 1 && (string?)list[0]!["name"] == "Тест-сервер"
              && (int?)list[0]!["channelPort"] == 25201 && (list[0]!["mods"] as JsonArray)?.Count == 1,
            "FetchServers: сервер с адресом канала и модами");

        // вход 1: сервер отказывает (как «Player disallowed … by an extension»)
        b = cli.Call(EvIds.Server, EvIds.JoinServer, new JsonObject { ["guid"] = guid }, out t);
        var join1 = (long?)b["joinId"] ?? 0;
        var n = srv.Next();
        Check(t == EvIds.JoinServer && join1 > 0 && n.Type == EvIds.PlayerJoiningNotification
              && (long?)n.Body["joinId"] == join1 && (string?)n.Body["login"] == "evtest",
            "JoinServer: серверу уходит PlayerJoiningNotification");
        srv.Call(EvIds.Server, EvIds.DenyPlayer, new JsonObject { ["joinId"] = join1, ["reason"] = "мод не пустил" }, out t);
        n = cli.Next();
        Check(t == EvIds.DenyPlayer && n.Type == EvIds.JoinDeniedNotification && (string?)n.Body["reason"] == "мод не пустил",
            "DenyPlayer -> клиенту JoinDeniedNotification с причиной");

        // вход 2: разрешено, затем выход
        b = cli.Call(EvIds.Server, EvIds.JoinServer, new JsonObject { ["guid"] = guid }, out _);
        var join2 = (long?)b["joinId"] ?? 0;
        srv.Next();
        srv.Call(EvIds.Server, EvIds.AllowPlayer, new JsonObject { ["joinId"] = join2 }, out t);
        n = cli.Next();
        Check(t == EvIds.AllowPlayer && n.Type == EvIds.JoinAllowedNotification
              && (string?)n.Body["server"]!["address"] == "127.0.0.1:25200",
            "AllowPlayer -> клиенту JoinAllowedNotification с адресом");
        Check(service.Servers.First().Players.Count == 1, "игрок числится на сервере");
        b = srv.Call(EvIds.Server, EvIds.AllowPlayer, new JsonObject { ["joinId"] = join2 }, out t);
        Check(t == EvIds.Error && (string?)b["code"] == "join", "повторное решение по тому же входу — отказ");
        cli.Call(EvIds.Server, EvIds.LeaveServer, new JsonObject(), out _);
        n = srv.Next();
        Check(n.Type == EvIds.PlayerLeftNotification && (string?)n.Body["reason"] == "leave", "LeaveServer -> серверу PlayerLeft");

        // вход 3 и обрыв клиента: сервер получает PlayerLeft(disconnected)
        cli.Call(EvIds.Server, EvIds.JoinServer, new JsonObject { ["guid"] = guid }, out _);
        var join3 = (long?)srv.Next().Body["joinId"] ?? 0;
        srv.Call(EvIds.Server, EvIds.AllowPlayer, new JsonObject { ["joinId"] = join3 }, out _);
        cli.Next();
        cli.Dispose();
        n = srv.Next();
        Check(n.Type == EvIds.PlayerLeftNotification && (string?)n.Body["reason"] == "disconnected",
            "обрыв клиента -> серверу PlayerLeft(disconnected)");
        srv.Dispose();
        for (var i = 0; i < 50 && service.Servers.Count > 0; i++)
        {
            Thread.Sleep(20);
        }

        Check(service.Servers.Count == 0, "обрыв сервера -> сервер ушёл из списка");
    }

    /// <summary>Тестовый клиент EV: ответы по request_id, уведомления (request_id = 0) — в очередь.</summary>
    private sealed class TestClient : IDisposable
    {
        private readonly TcpClient _tcp = new();
        private readonly NetworkStream _s;
        private readonly System.Collections.Concurrent.BlockingCollection<EvFrame> _replies = new();
        private readonly System.Collections.Concurrent.BlockingCollection<EvFrame> _notes = new();
        private uint _req;

        public TestClient(int port)
        {
            _tcp.Connect(IPAddress.Loopback, port);
            _s = _tcp.GetStream();
            _ = Task.Run(async () =>
            {
                try
                {
                    while (await EvFrame.ReadAsync(_s, CancellationToken.None) is { } f)
                    {
                        (f.RequestId == 0 ? _notes : _replies).Add(f);
                    }
                }
                catch (Exception)
                {
                    // закрыто
                }
            });
        }

        public JsonObject Call(ushort module, ushort type, JsonObject body, out ushort respType)
        {
            var r = ++_req;
            _s.Write(new EvFrame(module, type, r, body).Encode());
            if (!_replies.TryTake(out var resp, 5000) || resp.RequestId != r)
            {
                throw new IOException("EV: нет ответа");
            }

            respType = resp.Type;
            return resp.Body;
        }

        public EvFrame Next() =>
            _notes.TryTake(out var f, 5000) ? f : throw new IOException("EV: нет уведомления");

        public void Dispose()
        {
            try
            {
                _tcp.Close();
            }
            catch
            {
                // уже закрыт
            }
        }
    }
}
