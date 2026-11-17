// EvService.cs — обработчики EV-канала, backend\EV-PROTOCOL.md §3–4.
//   E1: Utility, Manager, Authentication. E2: Server (ServerLogin без ключа, FetchServers/GetServerInfo, JoinServer с
//   допуском сервера Allow/DenyPlayer, LeaveServer, уведомления Join*/PlayerJoining/PlayerLeft).
//
// Одна база аккаунтов с Blaze (AccountsStore): токен EV = токен, который лаунчер передаёт BF3 для входа в Blaze, —
// так оба протокола видят одну сессию/персону (§2.2). Лицензий и ключей сервера нет (§2.4).

using System.Collections.Concurrent;
using System.Text.Json.Nodes;
using EVBots.Backend.Data;

namespace EVBots.Backend.Ev;

/// <summary>Состояние одного соединения EV.</summary>
internal sealed class EvSession
{
    private static long _next;
    private readonly SemaphoreSlim _sendGate = new(1, 1);

    public long Id { get; } = Interlocked.Increment(ref _next);
    public bool Initialized { get; set; }
    public string Role { get; set; } = "";        // "client" | "server" | "launcher"
    public AccountInfo? Account { get; set; }
    public string Token { get; set; } = "";

    /// <summary>Зарегистрированный сервер (роль server после ServerLogin).</summary>
    public EvServer? Server { get; set; }

    /// <summary>Сервер, на который игрок вошёл или входит.</summary>
    public string JoinedServer { get; set; } = "";

    /// <summary>Канал записи (поток TCP); null — сессия только для тестов без сети.</summary>
    public Stream? Output { get; set; }

    public async Task SendAsync(EvFrame f, CancellationToken ct = default)
    {
        if (Output is null)
        {
            return;
        }

        await _sendGate.WaitAsync(ct);
        try
        {
            await Output.WriteAsync(f.Encode(), ct);
        }
        finally
        {
            _sendGate.Release();
        }
    }
}

/// <summary>Сервер, вошедший в EV (ServerLogin). Идентичность — GUID инстанса, ключ не нужен.</summary>
internal sealed class EvServer
{
    public string Guid { get; init; } = "";
    public string Name { get; set; } = "";
    public string Address { get; set; } = "";     // адрес игры (IP:порт BF3)
    public int ChannelPort { get; set; }          // порт игрового канала К4 (аналог Harmony), 0 — нет
    public JsonArray Mods { get; set; } = [];     // [{name, version, hash}]
    public EvSession Session { get; init; } = null!;
    public ConcurrentDictionary<long, EvSession> Players { get; } = new();
}

internal sealed class EvService(AccountsStore accounts, Log log)
{
    public const int ProtocolVersion = 1;

    private readonly ConcurrentDictionary<string, EvServer> _servers = new(StringComparer.OrdinalIgnoreCase);
    private readonly ConcurrentDictionary<long, (EvSession Client, EvServer Server)> _pendingJoins = new();
    private long _nextJoin;

    public IReadOnlyCollection<EvServer> Servers => _servers.Values.ToList();

    public async Task<EvFrame> HandleAsync(EvSession s, EvFrame f)
    {
        // До InitSession — только InitSession и Ping (как у Zeus: сначала Utility.InitSession).
        if (!s.Initialized && !(f.Module == EvIds.Utility && f.Type is EvIds.InitSession or EvIds.Ping))
        {
            return Fail(f, "session-not-initialized", "сначала Utility.InitSession");
        }

        return (f.Module, f.Type) switch
        {
            (EvIds.Utility, EvIds.InitSession) => InitSession(s, f),
            (EvIds.Utility, EvIds.Ping) => Reply(f, new JsonObject { ["time"] = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() }),
            (EvIds.Manager, EvIds.RegisterSession) => RegisterSession(s, f),
            (EvIds.Authentication, EvIds.Login) => Login(s, f),
            (EvIds.Authentication, EvIds.TokenLogin) => TokenLogin(s, f),
            (EvIds.Authentication, EvIds.Logout) => Logout(s, f),
            (EvIds.Server, EvIds.ServerLogin) => ServerLogin(s, f),
            (EvIds.Server, EvIds.FetchServers) => FetchServers(f),
            (EvIds.Server, EvIds.GetServerInfo) => GetServerInfo(f),
            (EvIds.Server, EvIds.JoinServer) => await JoinServerAsync(s, f),
            (EvIds.Server, EvIds.AllowPlayer) => await DecideJoinAsync(s, f, allow: true),
            (EvIds.Server, EvIds.DenyPlayer) => await DecideJoinAsync(s, f, allow: false),
            (EvIds.Server, EvIds.LeaveServer) => await LeaveServerAsync(s, f),
            _ => Fail(f, "unknown-message", $"EV: {EvIds.Name(f.Module, f.Type)} 0x{f.Module:X4}/0x{f.Type:X4} не поддерживается"),
        };
    }

    /// <summary>Соединение закрыто: сервер уходит из списка, игрок — с сервера.</summary>
    public async Task DisconnectedAsync(EvSession s)
    {
        if (s.Server is { } srv && _servers.TryGetValue(srv.Guid, out var cur) && ReferenceEquals(cur, srv))
        {
            _servers.TryRemove(srv.Guid, out _);
            log.Info($"EV: сервер {srv.Name} ({srv.Guid}) ушёл");
            foreach (var p in srv.Players.Values)
            {
                p.JoinedServer = "";
                await SafeSend(p, Notify(EvIds.Server, EvIds.RemovedFromServerNotification,
                    new JsonObject { ["server"] = srv.Guid, ["reason"] = "server-gone" }));
            }
        }

        foreach (var (id, j) in _pendingJoins.Where(kv => ReferenceEquals(kv.Value.Client, s) || ReferenceEquals(kv.Value.Server.Session, s)).ToList())
        {
            _pendingJoins.TryRemove(id, out _);
        }

        await LeaveCurrentAsync(s, "disconnected");
    }

    // ── E1 ─────────────────────────────────────────────────────────────────

    private EvFrame InitSession(EvSession s, EvFrame f)
    {
        var ver = (int?)f.Body["version"] ?? 0;
        if (ver != ProtocolVersion)
        {
            return Fail(f, "version", $"версия протокола EV {ver}, нужна {ProtocolVersion}");
        }

        s.Initialized = true;
        return Reply(f, new JsonObject { ["version"] = ProtocolVersion, ["session"] = s.Id, ["service"] = "EV Backend" });
    }

    private EvFrame RegisterSession(EvSession s, EvFrame f)
    {
        var role = (string?)f.Body["role"] ?? "";
        if (role is not ("client" or "server" or "launcher"))
        {
            return Fail(f, "role", "role: client | server | launcher");
        }

        s.Role = role;
        log.Info($"EV: сессия {s.Id} — {role}");
        return Reply(f, new JsonObject { ["session"] = s.Id, ["role"] = role });
    }

    private EvFrame Login(EvSession s, EvFrame f)
    {
        var login = (string?)f.Body["login"] ?? "";
        var password = (string?)f.Body["password"] ?? "";
        if (!accounts.Verify(login, password, out var info, out var error))
        {
            return Fail(f, "login", error);
        }

        s.Account = info;
        s.Token = accounts.IssueToken(info);
        log.Info($"EV: сессия {s.Id} вошла как {info.Login} (persona {info.Personaref})");
        return Reply(f, AccountJson(info, s.Token));
    }

    private EvFrame TokenLogin(EvSession s, EvFrame f)
    {
        var token = (string?)f.Body["token"] ?? "";
        var info = accounts.ResolveToken(token);
        if (info is null)
        {
            return Fail(f, "token", "токен не найден или истёк");
        }

        s.Account = info;
        s.Token = token;
        return Reply(f, AccountJson(info, token));
    }

    private EvFrame Logout(EvSession s, EvFrame f)
    {
        s.Account = null;
        s.Token = "";
        return Reply(f, new JsonObject());
    }

    // ── E2: Server ─────────────────────────────────────────────────────────

    private EvFrame ServerLogin(EvSession s, EvFrame f)
    {
        if (s.Role != "server")
        {
            return Fail(f, "role", "ServerLogin — только для сессии role=server");
        }

        var guid = (string?)f.Body["guid"] ?? "";
        if (!System.Guid.TryParse(guid, out _))
        {
            return Fail(f, "guid", "guid сервера — GUID инстанса (serverid), ключ не нужен");
        }

        var srv = new EvServer
        {
            Guid = guid,
            Name = (string?)f.Body["name"] ?? "EVBots server",
            Address = (string?)f.Body["address"] ?? "",
            ChannelPort = (int?)f.Body["channelPort"] ?? 0,
            Mods = f.Body["mods"] as JsonArray is { } m ? (JsonArray)m.DeepClone() : [],
            Session = s,
        };
        _servers[guid] = srv;   // повторный вход того же GUID (перезапуск) заменяет запись
        s.Server = srv;
        log.Info($"EV: сервер вошёл: {srv.Name} ({guid}) {srv.Address}, модов {srv.Mods.Count}");
        return Reply(f, new JsonObject { ["guid"] = guid });
    }

    private static JsonObject ServerJson(EvServer srv) => new()
    {
        ["guid"] = srv.Guid,
        ["name"] = srv.Name,
        ["address"] = srv.Address,
        ["players"] = srv.Players.Count,
        ["channelPort"] = srv.ChannelPort,
        ["mods"] = srv.Mods.DeepClone(),
    };

    private EvFrame FetchServers(EvFrame f)
    {
        var list = new JsonArray();
        foreach (var srv in _servers.Values.OrderBy(x => x.Name, StringComparer.OrdinalIgnoreCase))
        {
            list.Add(ServerJson(srv));
        }

        return Reply(f, new JsonObject { ["servers"] = list });
    }

    private EvFrame GetServerInfo(EvFrame f)
    {
        var guid = (string?)f.Body["guid"] ?? "";
        return _servers.TryGetValue(guid, out var srv)
            ? Reply(f, ServerJson(srv))
            : Fail(f, "server", "сервер не найден");
    }

    /// <summary>
    /// Клиент просит вход. Как у VU: сервер решает (Lua «Player disallowed … by an extension») — ему уходит
    /// PlayerJoiningNotification, ответ клиенту — сразу {joinId, pending}, итог — JoinAllowed/JoinDeniedNotification.
    /// </summary>
    private async Task<EvFrame> JoinServerAsync(EvSession s, EvFrame f)
    {
        if (s.Account is null)
        {
            return Fail(f, "auth", "сначала Authentication.Login/TokenLogin");
        }

        var guid = (string?)f.Body["guid"] ?? "";
        if (!_servers.TryGetValue(guid, out var srv))
        {
            return Fail(f, "server", "сервер не найден");
        }

        await LeaveCurrentAsync(s, "switch");
        var joinId = Interlocked.Increment(ref _nextJoin);
        _pendingJoins[joinId] = (s, srv);
        await SafeSend(srv.Session, Notify(EvIds.Server, EvIds.PlayerJoiningNotification, new JsonObject
        {
            ["joinId"] = joinId,
            ["login"] = s.Account.Login,
            ["name"] = s.Account.Name,
            ["personaref"] = s.Account.Personaref,
        }));
        log.Info($"EV: {s.Account.Login} входит на {srv.Name} (join {joinId})");
        return Reply(f, new JsonObject { ["joinId"] = joinId, ["pending"] = true });
    }

    private async Task<EvFrame> DecideJoinAsync(EvSession s, EvFrame f, bool allow)
    {
        if (s.Server is null)
        {
            return Fail(f, "role", "Allow/DenyPlayer — только для вошедшего сервера");
        }

        var joinId = (long?)f.Body["joinId"] ?? 0;
        if (!_pendingJoins.TryGetValue(joinId, out var j) || !ReferenceEquals(j.Server, s.Server))
        {
            return Fail(f, "join", "нет такого ожидающего входа");
        }

        _pendingJoins.TryRemove(joinId, out _);
        if (allow)
        {
            j.Client.JoinedServer = s.Server.Guid;
            s.Server.Players[j.Client.Id] = j.Client;
            await SafeSend(j.Client, Notify(EvIds.Server, EvIds.JoinAllowedNotification, new JsonObject
            {
                ["joinId"] = joinId,
                ["server"] = ServerJson(s.Server),
            }));
        }
        else
        {
            var reason = (string?)f.Body["reason"] ?? "";
            await SafeSend(j.Client, Notify(EvIds.Server, EvIds.JoinDeniedNotification, new JsonObject
            {
                ["joinId"] = joinId,
                ["reason"] = reason,
            }));
            log.Info($"EV: {j.Client.Account?.Login} — отказ сервера {s.Server.Name}: {reason}");
        }

        return Reply(f, new JsonObject { ["joinId"] = joinId });
    }

    private async Task<EvFrame> LeaveServerAsync(EvSession s, EvFrame f)
    {
        await LeaveCurrentAsync(s, "leave");
        return Reply(f, new JsonObject());
    }

    private async Task LeaveCurrentAsync(EvSession s, string reason)
    {
        if (s.JoinedServer.Length == 0)
        {
            return;
        }

        if (_servers.TryGetValue(s.JoinedServer, out var srv) && srv.Players.TryRemove(s.Id, out _))
        {
            await SafeSend(srv.Session, Notify(EvIds.Server, EvIds.PlayerLeftNotification, new JsonObject
            {
                ["login"] = s.Account?.Login ?? "",
                ["personaref"] = s.Account?.Personaref ?? 0,
                ["reason"] = reason,
            }));
        }

        s.JoinedServer = "";
    }

    // ── общее ──────────────────────────────────────────────────────────────

    private async Task SafeSend(EvSession target, EvFrame f)
    {
        try
        {
            await target.SendAsync(f);
        }
        catch (Exception ex) when (ex is IOException or ObjectDisposedException or InvalidOperationException)
        {
            log.Warn($"EV: уведомление сессии {target.Id} не ушло: {ex.Message}");
        }
    }

    private static JsonObject AccountJson(AccountInfo info, string token) => new()
    {
        ["login"] = info.Login,
        ["name"] = info.Name,
        ["personaref"] = info.Personaref,
        ["token"] = token,
    };

    private static EvFrame Notify(ushort module, ushort type, JsonObject body) => new(module, type, 0, body);

    private static EvFrame Reply(EvFrame req, JsonObject body) => new(req.Module, req.Type, req.RequestId, body);

    private static EvFrame Fail(EvFrame req, string code, string message) =>
        new(req.Module, EvIds.Error, req.RequestId,
            new JsonObject { ["code"] = code, ["message"] = message, ["type"] = req.Type });
}
