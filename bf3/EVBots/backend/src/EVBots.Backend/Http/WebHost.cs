// WebHost.cs — HTTP-морда: список серверов, статистика, раздача модов.
//
// Идея взята из BF-Portable\Xampp\htdocs\getServerBrowser.php и
// getServerInfo.php — там PHP+MySQL решали ровно эту задачу для BattleLog.
// Здесь то же самое, но на C# и без MySQL: данные лежат в profiles\.
//
// Маршруты:
//   GET  /                     обзор состояния
//   GET  /servers              список серверов (JSON)
//   GET  /servers/{id}         один сервер
//   GET  /profiles             список солдат
//   GET  /profiles/{id}        один солдат (имя, ранг-статы)
//   POST /profiles             создать солдата  {"name":"..."}
//   DELETE /profiles/{id}      удалить солдата
//   GET  /stats                каталог прогресса (информация)
//   GET  /accounts             список аккаунтов (логины, без секретов)
//   POST /accounts/ensure      {"login","password"} — войти или зарегистрироваться
//   POST /accounts/login       {"login","password"} — вход -> {token, personaref}
//   GET  /mods                 список модов в Admin\Mods
//   GET  /mods/{name}/...      скачать файл мода (раздача клиентам)

using System.Net;
using System.Net.NetworkInformation;
using System.Text;
using System.Text.Json;
using EVBots.Backend.Blaze;
using EVBots.Backend.Data;

namespace EVBots.Backend.Http;

internal sealed class WebHost(ServeConfig cfg, Blaze.BlazeServices services,
    ProfileStore store, AccountsStore accounts, StatsCatalog stats,
    InstanceCatalog catalog, string serverName, Log log)
{
    private readonly CancellationTokenSource _stop = new();
    HttpListener? _listener;

    public void Start()
    {
        // HttpListener не даёт префикс «+» без прав администратора, поэтому
        // перечисляем локальные адреса и вешаем префикс на каждый — так HTTP
        // доступен гостям из LAN без повышения привилегий.
        //
        // Важно: ОДИН HttpListener со всеми префиксами. Если заводить по
        // отдельному слушателю на префикс (как было раньше), второй падает
        // с «conflicts with an existing registration on the machine».
        // Проверено пробой: один слушатель с двумя префиксами — ok.
        //
        // Права http.sys проверяются на каждом префиксе отдельно, поэтому
        // сначала пробуем каждый «пробным» слушателем, оставляем только
        // разрешённые; запрещённый LAN не мешает работать loopback.
        var wanted = new List<string> { $"http://127.0.0.1:{cfg.HttpPort}/" };
        foreach (var addr in LocalIPv4())
        {
            wanted.Add($"http://{addr}:{cfg.HttpPort}/");
        }

        var accepted = new List<string>();
        var failed = new List<string>();
        foreach (var p in wanted)
        {
            var probe = new HttpListener();
            probe.Prefixes.Add(p);
            try
            {
                probe.Start();
                probe.Stop();
                probe.Close();
                accepted.Add(p);
            }
            catch (Exception ex)
            {
                // отказ URL-ACL показываем своими словами: системное
                // «Отказано в доступе» зависит от локали Windows
                var reason = ex.Message.Contains("Отказано", StringComparison.OrdinalIgnoreCase)
                    ? "Access denied (URL-ACL)"
                    : ex.Message;
                failed.Add($"{p} : {reason}");
                try
                {
                    probe.Close();
                }
                catch
                {
                    // игнорируем
                }
            }
        }

        if (accepted.Count == 0)
        {
            log.Error($"HTTP failed on port {cfg.HttpPort}");
            foreach (var f in failed)
            {
                log.Error($"  {f}");
            }

            return;
        }

        var listener = new HttpListener();
        foreach (var p in accepted)
        {
            listener.Prefixes.Add(p);
        }

        try
        {
            listener.Start();
            _listener = listener;
        }
        catch (Exception ex)
        {
            log.Error($"HTTP failed on port {cfg.HttpPort}: {ex.Message}");
            return;
        }

        _ = Task.Run(() => LoopAsync(_stop.Token));
        log.Ok($"HTTP: port {cfg.HttpPort}, accepted prefixes " +
               $"{accepted.Count}: {string.Join(", ", accepted)}");

        if (failed.Count > 0)
        {
            log.Warn("some addresses unavailable, URL-ACL needed (as admin):");
            foreach (var f in failed)
            {
                log.Warn($"  {f}");
            }

            log.Warn("  to make HTTP visible in LAN, run once:");
            foreach (var p in failed)
            {
                // p = "http://10.6.7.1:8080/ : Access denied (URL-ACL)": адрес с портом и слешем до « : »
                var url = p[..p.IndexOf(" : ", StringComparison.Ordinal)];
                log.Warn($"    netsh http add urlacl url={url} " +
                         $"user={Environment.UserName}");
            }

            log.Warn("  check port usage: netstat -ano | findstr " +
                     $":{cfg.HttpPort}");
        }
    }

    /// <summary>Локальные IPv4-адреса, кроме loopback.</summary>
    private static IEnumerable<string> LocalIPv4()
    {
        var result = new List<string>();

        try
        {
            foreach (var ni in NetworkInterface.GetAllNetworkInterfaces())
            {
                if (ni.OperationalStatus != OperationalStatus.Up ||
                    ni.NetworkInterfaceType == NetworkInterfaceType.Loopback)
                {
                    continue;
                }

                foreach (var ip in ni.GetIPProperties().UnicastAddresses)
                {
                    if (ip.Address.AddressFamily ==
                        System.Net.Sockets.AddressFamily.InterNetwork)
                    {
                        result.Add(ip.Address.ToString());
                    }
                }
            }
        }
        catch
        {
            // не смогли перечислить — останется только loopback
        }

        return result;
    }

    public void Stop()
    {
        try
        {
            _stop.Cancel();
            _listener?.Stop();
            _listener?.Close();
        }
        catch
        {
            // игнорируем
        }
    }

    private async Task LoopAsync(CancellationToken ct)
    {
        var listener = _listener;
        if (listener is null)
        {
            return;
        }

        while (!ct.IsCancellationRequested)
        {
            HttpListenerContext ctx;
            try
            {
                ctx = await listener.GetContextAsync();
            }
            catch (Exception ex) when (ct.IsCancellationRequested ||
                                       ex is ObjectDisposedException or
                                       HttpListenerException)
            {
                return;
            }
            catch (Exception ex)
            {
                log.Warn($"HTTP: {ex.Message}");
                continue;
            }

            _ = Task.Run(() => Handle(ctx), ct);
        }
    }

    private void Handle(HttpListenerContext ctx)
    {
        try
        {
            var path = ctx.Request.Url?.AbsolutePath?.TrimEnd('/') ?? "/";
            var method = ctx.Request.HttpMethod;
            log.Debug($"HTTP {method} {path} от " +
                      $"{ctx.Request.RemoteEndPoint}");

            Route(ctx, path, method);
        }
        catch (Exception ex)
        {
            log.Error($"HTTP {ctx.Request.Url}: {ex.Message}");
            SendJson(ctx, 500, new { error = ex.Message });
        }
        finally
        {
            try
            {
                ctx.Response.Close();
            }
            catch
            {
                // игнорируем
            }
        }
    }

    private void Route(HttpListenerContext ctx, string path, string method)
    {
        // корень: "/" и "" — обзор состояния
        if (path.Length <= 1)
        {
            if (method == "GET")
            {
                SendJson(ctx, 200, Overview());
            }
            else
            {
                SendJson(ctx, 405, new { error = "метод не поддерживается" });
            }

            return;
        }

        var parts = path.Split('/', StringSplitOptions.RemoveEmptyEntries);

        if (parts.Length == 0)
        {
            SendJson(ctx, 404, new { error = "не найдено" });
            return;
        }

        switch (parts[0])
        {
            case "servers":
                HandleServers(ctx, parts, method);
                return;

            case "profiles":
                HandleProfiles(ctx, parts, method);
                return;

            case "accounts":
                HandleAccounts(ctx, parts, method);
                return;

            case "stats" when method == "GET":
                SendJson(ctx, 200, new
                {
                    ok = stats.Ok,
                    group = stats.Group,
                    stats = stats.StatCount,
                    payloadBytes = stats.PayloadLength,
                    path = stats.Path,
                });
                return;

            case "mods":
                ModServer.Handle(ctx, cfg, catalog, parts, method);
                return;

            case "admin":
                HandleAdmin(ctx, parts, method);
                return;

            case "rcon" when method == "POST":
                HandleRcon(ctx);
                return;

            default:
                SendJson(ctx, 404, new { error = "неизвестный маршрут", path });
                return;
        }
    }

    /// <summary>
    /// Админка инстанса сервера в структуре VU: Startup.txt, MapList.txt,
    /// ModList.txt и команды modList.* из hosting\commands.md.
    ///
    ///   GET  /admin/startup              все параметры
    ///   POST /admin/startup {key,value}  задать параметр
    ///   GET  /admin/maplist              ротация карт
    ///   POST /admin/maplist {entries}    задать ротацию
    ///   GET  /admin/modlist              что загрузится при старте
    ///   POST /admin/modlist {command,argument}  команда modList.*
    /// </summary>
    private void HandleAdmin(HttpListenerContext ctx, string[] parts, string method)
    {
        if (parts.Length < 2)
        {
            SendJson(ctx, 404, new
            {
                error = "нужен раздел: startup, maplist или modlist",
            });
            return;
        }

        switch (parts[1])
        {
            case "startup" when method == "GET":
                SendJson(ctx, 200, new
                {
                    path = catalog.StartupPath,
                    values = catalog.ReadStartup(),
                });
                return;

            case "startup" when method == "POST":
            {
                var body = ReadJson(ctx);
                if (body.ValueKind == JsonValueKind.Undefined ||
                    !body.TryGetProperty("key", out var k) ||
                    !body.TryGetProperty("value", out var v))
                {
                    SendJson(ctx, 400,
                        new { error = "ожидались поля key и value" });
                    return;
                }

                catalog.SetStartupValue(k.GetString() ?? "", v.ValueKind switch
                {
                    JsonValueKind.String => v.GetString() ?? "",
                    JsonValueKind.Number => v.GetRawText(),
                    _ => v.ToString(),
                });
                SendJson(ctx, 200, new { ok = true, values = catalog.ReadStartup() });
                return;
            }

            case "maplist" when method == "GET":
            {
                var list = catalog.ReadMapList();
                SendJson(ctx, 200, new
                {
                    path = catalog.MapListPath,
                    count = list.Count,
                    entries = list.Select(e => new
                    {
                        map = e.MapId,
                        gamemode = e.GamemodeId,
                        rounds = e.Rounds,
                    }).ToList(),
                });
                return;
            }

            case "maplist" when method == "POST":
            {
                var body = ReadJson(ctx);
                if (body.ValueKind == JsonValueKind.Undefined ||
                    !body.TryGetProperty("entries", out var arr) ||
                    arr.ValueKind != JsonValueKind.Array)
                {
                    SendJson(ctx, 400,
                        new { error = "ожидалось поле entries: [{map,gamemode,rounds}]" });
                    return;
                }

                var entries = new List<MapListEntry>();
                foreach (var it in arr.EnumerateArray())
                {
                    var map = it.TryGetProperty("map", out var m) ? m.GetString() : null;
                    var mode = it.TryGetProperty("gamemode", out var g)
                        ? g.GetString()
                        : null;
                    if (string.IsNullOrEmpty(map) || string.IsNullOrEmpty(mode))
                    {
                        continue;
                    }

                    var rounds = it.TryGetProperty("rounds", out var r) &&
                                 r.ValueKind == JsonValueKind.Number
                        ? r.GetInt32()
                        : 1;
                    entries.Add(new MapListEntry(map!, mode!, rounds));
                }

                catalog.WriteMapList(entries);
                log.Info($"MapList.txt обновлён: {entries.Count} записей");
                SendJson(ctx, 200, new
                {
                    ok = true,
                    count = entries.Count,
                    entries = entries.Select(e => e.ToString()).ToList(),
                });
                return;
            }

            case "modlist" when method == "GET":
                SendJson(ctx, 200, new
                {
                    path = catalog.ModListPath,
                    modsDir = catalog.ModsDir,
                    configured = catalog.ReadModList(),
                    running = services.RunningMods,
                    available = catalog.AvailableMods(),
                    debug = catalog.DebugMode,
                });
                return;

            case "modlist" when method == "POST":
            {
                var body = ReadJson(ctx);
                var command = body.ValueKind != JsonValueKind.Undefined &&
                              body.TryGetProperty("command", out var c)
                    ? c.GetString() ?? ""
                    : "";
                var argument = body.ValueKind != JsonValueKind.Undefined &&
                               body.TryGetProperty("argument", out var a)
                    ? a.GetString()
                    : null;

                var lines = catalog.ExecuteModListCommand(command, argument,
                    services.RunningMods);
                SendJson(ctx, 200, new
                {
                    command,
                    argument,
                    lines,
                    configured = catalog.ReadModList(),
                    debug = catalog.DebugMode,
                });
                return;
            }

            default:
                SendJson(ctx, 404,
                    new { error = "неизвестный раздел или метод", section = parts[1] });
                return;
        }
    }

    /// <summary>
    /// RCON-подобный ввод команд в HTTP. Нужен для лаунчера и для проверки
    /// команд modList.* без подключения к RCON порту.
    /// </summary>
    private void HandleRcon(HttpListenerContext ctx)
    {
        var body = ReadJson(ctx);
        var line = body.ValueKind != JsonValueKind.Undefined &&
                   body.TryGetProperty("command", out var c)
            ? c.GetString() ?? ""
            : "";

        if (line.Length == 0)
        {
            SendJson(ctx, 400, new { error = "ожидалось поле command" });
            return;
        }

        var space = line.IndexOf(' ');
        var cmd = space > 0 ? line[..space] : line;
        var arg = space > 0 ? line[(space + 1)..] : null;

        // поддерживаем пока только modList.* — остальное отдаёт RCON игрового
        // сервера на 47900
        if (cmd.StartsWith("modList", StringComparison.OrdinalIgnoreCase))
        {
            var lines = catalog.ExecuteModListCommand(cmd, arg,
                services.RunningMods);
            SendJson(ctx, 200, new { command = line, lines });
            return;
        }

        SendJson(ctx, 501, new
        {
            error = $"команда '{cmd}' обрабатывается игровым сервером через " +
                    $"RCON {cfg.GameServerAddress}",
            hint = "управление игрой идёт на порт 47900 (RCON)",
        });
    }

    /// <summary>Читает тело запроса. ValueKind == Undefined, если тела нет.</summary>
    private static JsonElement ReadJson(HttpListenerContext ctx)
    {
        using var reader = new StreamReader(ctx.Request.InputStream, Encoding.UTF8);
        var raw = reader.ReadToEnd();
        if (string.IsNullOrWhiteSpace(raw))
        {
            return default;
        }

        try
        {
            using var doc = JsonDocument.Parse(raw);
            return doc.RootElement.Clone();
        }
        catch (JsonException)
        {
            return default;
        }
    }

    private void HandleServers(HttpListenerContext ctx, string[] parts,
        string method)
    {
        // GET /servers
        if (parts.Length == 1 && method == "GET")
        {
            // Список — только ЖИВЫЕ серверы, как у VU: сервер появляется,
            // когда он зарегистрировал игру в Blaze, и исчезает, когда
            // остановился (docs: general\playing.md — браузер показывает
            // серверы, к которым можно подключиться; -unlisted прячет живой
            // сервер из списка, но он остаётся доступен по id).
            // Раньше сюда подставлялся StandaloneView() — «сервер из
            // конфига», которого на самом деле нет; из-за этого гость видел
            // сервер, который ещё не запущен. Конфигурация хоста доступна
            // отдельно: GET / (секция ownServer).
            var list = services.Games.All
                .Where(g => !cfg.Unlisted)
                .Select(ServerView)
                .ToList();

            SendJson(ctx, 200, new
            {
                count = list.Count,
                servers = list,
            });
            return;
        }

        // GET /servers/{id}
        if (parts.Length == 2 && method == "GET")
        {
            if (ulong.TryParse(parts[1], out var gid))
            {
                var game = services.Games.Find(gid);
                if (game is not null)
                {
                    SendJson(ctx, 200, ServerView(game));
                    return;
                }
            }

            var cfgId = cfg.ServerId ?? "";
            if (parts[1].Equals(cfgId, StringComparison.OrdinalIgnoreCase))
            {
                // Свой сервер по его id — как `connect <guid>` / vu://join у VU
                // для -unlisted: в списке его нет, но по id войти можно.
                // running:false честно говорит, что процесс сервера не поднят.
                SendJson(ctx, 200, OwnServerView());
                return;
            }

            SendJson(ctx, 404, new { error = "сервер не найден", id = parts[1] });
            return;
        }

        SendJson(ctx, 405, new { error = "метод не поддерживается" });
    }

    /// <summary>
    /// Аккаунты LAN (как в VU): вход и регистрация с любого ПК.
    ///
    ///   GET  /accounts                  список (логин, personaref, имя)
    ///   POST /accounts/ensure  {login,password,name?}
    ///   POST /accounts/login   {login,password}
    ///
    /// ensure идемпотентен — его зовёт join.bat перед запуском игры:
    /// аккаунта нет — создаётся (профиль: имя = логин, либо ник name из
    /// join.bat — он станет именем солдата в игре), есть — проверяется
    /// пароль; в ответе personaref для requestStateParams.
    /// login дополнительно выдаёт токен (в памяти, 24 ч) — для будущего
    /// Launcher и Blaze AUTH.
    /// </summary>
    /// <summary>
    /// 01.11: «ник в игре» из лаунчера. Раньше он брался только при первой регистрации, потом в игре оставалось старое имя,
    /// а права меню F12 лаунчер выдавал новому нику — не тому, с которым солдат в игре. Теперь ник после проверки пароля
    /// переименовывает выбранного солдата аккаунта (тот же Rename, что и POST /profiles/{id}/rename). Имя занято — остаётся
    /// прежнее, причина уходит лаунчеру в renameError.
    /// </summary>
    private (string Name, string? Error) ApplyNick(ulong personaref, string current, string? nick)
    {
        if (string.IsNullOrWhiteSpace(nick)) return (current, null);
        nick = nick.Trim();
        if (string.Equals(nick, current, StringComparison.Ordinal)) return (current, null);
        var renamed = store.Rename(personaref, nick);
        return renamed is null
            ? (current, $"ник «{nick}» занят другим солдатом — в игре остаётся «{current}»")
            : (renamed.Name, null);
    }

    private void HandleAccounts(HttpListenerContext ctx, string[] parts,
        string method)
    {
        // GET /accounts
        if (parts.Length == 1 && method == "GET")
        {
            var list = accounts.All.Select(a => new
            {
                login = a.Login,
                personaref = a.Personaref,
                name = a.Name,
                createdUtc = a.CreatedUtc,
                soldiers = a.Soldiers.Count,
            }).ToList();

            SendJson(ctx, 200, new { count = list.Count, accounts = list });
            return;
        }

        // POST /accounts/ensure | /accounts/login
        if (parts.Length == 2 && method == "POST" &&
            parts[1] is "ensure" or "login")
        {
            string login;
            string password;
            string? displayName;

            try
            {
                using var reader = new StreamReader(ctx.Request.InputStream,
                    Encoding.UTF8);
                using var doc = JsonDocument.Parse(reader.ReadToEnd());
                var root = doc.RootElement;
                login = root.TryGetProperty("login", out var lg)
                    ? lg.GetString() ?? ""
                    : "";
                password = root.TryGetProperty("password", out var pw)
                    ? pw.GetString() ?? ""
                    : "";
                displayName = root.TryGetProperty("name", out var nm)
                    ? nm.GetString()
                    : null;
            }
            catch (JsonException)
            {
                SendJson(ctx, 400,
                    new { error = "ожидался JSON {login,password}" });
                return;
            }

            if (parts[1] == "login")
            {
                if (!accounts.Verify(login, password, out var acc, out var err))
                {
                    SendJson(ctx, 401, new { error = err });
                    return;
                }

                var (accName, renameError) = ApplyNick(acc.Personaref, acc.Name, displayName);
                SendJson(ctx, 200, new
                {
                    token = accounts.IssueToken(acc),
                    personaref = acc.Personaref,
                    login = acc.Login,
                    name = accName,
                    renameError,
                });
                return;
            }

            if (!accounts.Ensure(login, password, displayName, out var info,
                out var err2))
            {
                SendJson(ctx, 400, new { error = err2 });
                return;
            }

            var (infoName, renameError2) = ApplyNick(info.Personaref, info.Name, displayName);
            SendJson(ctx, 200, new
            {
                personaref = info.Personaref,
                login = info.Login,
                name = infoName,
                renameError = renameError2,
            });
            return;
        }

        // GET /accounts/{login}            карточка аккаунта с солдатами
        // POST /accounts/{login}/soldiers  {"name":"...","cloneFrom":N}
        // POST /accounts/{login}/soldier/select {"personaref":N}
        // POST /accounts/{login}/soldier/detach  {"personaref":N}
        if (parts.Length >= 2 && parts[1] is not ("ensure" or "login")
            && method == "GET")
        {
            var acc = accounts.Find(Uri.UnescapeDataString(parts[1]));
            if (acc is null)
            {
                SendJson(ctx, 404, new { error = "аккаунт не найден" });
                return;
            }

            SendJson(ctx, 200, new
            {
                login = acc.Login,
                personaref = acc.Personaref,
                name = acc.Name,
                soldiers = acc.Soldiers,
            });
            return;
        }

        // 18.10: POST /accounts/{login}/progress {"password":"...","stats":{имя:число}} — синхронизация статистики между
        // ПК друзей. Лаунчер шлёт свою копию, бэкенд сливает и возвращает итог: {"personaref":N,"stats":{...}}.
        // Слияние — максимум по каждому имени: значения каталога только растут (increment — накопление, max — рекорд),
        // поэтому копии с разных хостов сходятся без конфликтов в любом порядке. set (2 имени) — значение хоста.
        if (parts.Length == 3 && parts[2] == "progress" && method == "POST")
        {
            var login = Uri.UnescapeDataString(parts[1]);
            var body = ReadJson(ctx);
            var password = body.ValueKind == JsonValueKind.Object &&
                           body.TryGetProperty("password", out var pw2)
                ? pw2.GetString() ?? ""
                : "";
            if (!accounts.Verify(login, password, out var acc, out var verr))
            {
                SendJson(ctx, 401, new { error = verr });
                return;
            }

            var modes = stats.Stats.ToDictionary(e => e.Name, e => e.Mode, StringComparer.Ordinal);
            var changed = 0;
            var progress = store.UpdateProgress(acc.Personaref, current =>
            {
                if (!body.TryGetProperty("stats", out var inc) || inc.ValueKind != JsonValueKind.Object) return;
                foreach (var kv in inc.EnumerateObject())
                {
                    if (kv.Value.ValueKind != JsonValueKind.Number ||
                        !modes.TryGetValue(kv.Name, out var mode) || mode == "set")
                    {
                        continue;
                    }

                    var v = kv.Value.GetDouble();
                    if (!double.IsFinite(v))
                    {
                        continue;
                    }

                    current.Stats.TryGetValue(kv.Name, out var old);
                    if (v > old)
                    {
                        current.Stats[kv.Name] = v;
                        changed++;
                    }
                }
            });

            if (changed > 0)
            {
                log.Info($"[http] progress {login} ({acc.Personaref}): из копии игрока взято {changed} значений");
            }

            SendJson(ctx, 200, new { personaref = acc.Personaref, changed, stats = progress.Stats });
            return;
        }

        if (parts.Length == 3 && parts[2] == "soldiers" && method == "POST")
        {
            var login = Uri.UnescapeDataString(parts[1]);
            var body = ReadJson(ctx);
            var name = body.ValueKind == JsonValueKind.Object &&
                       body.TryGetProperty("name", out var n)
                ? n.GetString() ?? ""
                : "";
            var clone = body.ValueKind == JsonValueKind.Object &&
                        body.TryGetProperty("cloneFrom", out var cf) &&
                        cf.ValueKind == JsonValueKind.Number
                ? cf.GetUInt64()
                : 0UL;

            var (pid, created, err) = accounts.AddSoldier(login, name, clone);
            if (pid == 0)
            {
                SendJson(ctx, 400, new { error = err });
                return;
            }

            SendJson(ctx, 200, new
            {
                personaref = pid,
                created,
                login,
                soldiers = accounts.Find(login)?.Soldiers,
            });
            return;
        }

        if (parts.Length == 4 && parts[2] == "soldier" && method == "POST")
        {
            var login = Uri.UnescapeDataString(parts[1]);
            var body = ReadJson(ctx);
            var pid = body.ValueKind == JsonValueKind.Object &&
                      body.TryGetProperty("personaref", out var pv) &&
                      pv.ValueKind == JsonValueKind.Number
                ? pv.GetUInt64()
                : 0UL;

            var err = parts[3] switch
            {
                "select" => accounts.SelectSoldier(login, pid),
                "detach" => accounts.DetachSoldier(login, pid),
                _ => "неизвестное действие",
            };

            if (err.Length > 0)
            {
                SendJson(ctx, 400, new { error = err });
                return;
            }

            var acc = accounts.Find(login);
            SendJson(ctx, 200, new
            {
                login,
                personaref = acc?.Personaref ?? 0,
                name = acc?.Name ?? "",
                soldiers = acc?.Soldiers,
            });
            return;
        }

        // DELETE /accounts/{login}[?keepSoldier=1]
        // Удаляет логин и (по умолчанию) его солдата с прогрессом.
        if (parts.Length == 2 && parts[1] is not ("ensure" or "login")
            && method == "DELETE")
        {
            var login = Uri.UnescapeDataString(parts[1]);
            var keepSoldier = string.Equals(
                ctx.Request.Url?.Query, "?keepSoldier=1",
                StringComparison.OrdinalIgnoreCase);

            var acc = accounts.Find(login);
            if (acc is null)
            {
                SendJson(ctx, 404, new { error = "аккаунт не найден", login });
                return;
            }

            // солдат самого выделенного сервера трогать нельзя: на нём
            // держится Blaze-сессия хоста (слот 0) и создание игры
            foreach (var g in services.Games.All)
            {
                if (g.Host is not null && g.Host.Personaref == acc.Personaref)
                {
                    SendJson(ctx, 409, new
                    {
                        error = "это солдат работающего сервера — " +
                                "сначала останови сервер",
                        login,
                        personaref = acc.Personaref,
                    });
                    return;
                }
            }

            var removed = accounts.Remove(login, !keepSoldier);
            SendJson(ctx, 200, new
            {
                deleted = removed is not null,
                login = acc.Login,
                personaref = acc.Personaref,
                soldierDeleted = !keepSoldier,
            });
            return;
        }

        SendJson(ctx, 404, new { error = "не найдено" });
    }

    private void HandleProfiles(HttpListenerContext ctx, string[] parts,
        string method)
    {
        // GET /profiles
        if (parts.Length == 1 && method == "GET")
        {
            var list = store.All.Select(p =>
            {
                var progress = store.GetProgress(p.Id);
                // rank — индекс ранга в каталоге; лаунчеру нужен для вкладки
                // «Профили», чтобы не дёргать GET /profiles/{id} на каждый профиль
                progress.Stats.TryGetValue("rank", out var rank);
                return new
                {
                    personaref = p.Id,
                    name = p.Name,
                    createdUtc = p.CreatedUtc,
                    stats = progress.Stats.Count,
                    kills = progress.Stats.GetValueOrDefault("c___k_g"),
                    score = progress.Stats.GetValueOrDefault("c___sa_g"),
                    rank,
                };
            }).ToList();

            SendJson(ctx, 200, new { count = list.Count, profiles = list });
            return;
        }

        // GET /profiles/{id}
        if (parts.Length == 2 && method == "GET")
        {
            if (!ulong.TryParse(parts[1], out var pid))
            {
                SendJson(ctx, 400, new { error = "personaref должен быть числом" });
                return;
            }

            var profile = store.Find(pid);
            if (profile is null)
            {
                SendJson(ctx, 404, new { error = "солдат не найден" });
                return;
            }

            var progress = store.GetProgress(pid);
            SendJson(ctx, 200, new
            {
                personaref = profile.Id,
                name = profile.Name,
                createdUtc = profile.CreatedUtc,
                updatedUtc = progress.UpdatedUtc,
                stats = progress.Stats,
            });
            return;
        }

        // POST /profiles  {"name":"...","cloneFrom":234471775}
        if (parts.Length == 1 && method == "POST")
        {
            using var reader = new StreamReader(ctx.Request.InputStream,
                Encoding.UTF8);
            var body = reader.ReadToEnd();

            string name;
            ulong cloneFrom = 0;

            try
            {
                using var doc = JsonDocument.Parse(body);
                var root = doc.RootElement;
                name = root.TryGetProperty("name", out var n)
                    ? n.GetString() ?? ""
                    : "";
                cloneFrom = root.TryGetProperty("cloneFrom", out var cf) &&
                            cf.ValueKind == JsonValueKind.Number
                    ? cf.GetUInt64()
                    : 0;
            }
            catch (JsonException)
            {
                SendJson(ctx, 400, new
                {
                    error = "ожидался JSON с полем name",
                });
                return;
            }

            // cloneFrom — стартовая статистика для бота. Без записи прогресса
            // клиент не рисует ранг, поэтому при создании бота это обязательно.
            var created = cloneFrom > 0
                ? store.CreateBot(name, cloneFrom)
                : store.Create(name);

            if (created is null)
            {
                SendJson(ctx, 409,
                    new { error = "имя занято или пустое", name });
                return;
            }

            var progress = store.GetProgress(created.Id);
            SendJson(ctx, 201, new
            {
                personaref = created.Id,
                name = created.Name,
                createdUtc = created.CreatedUtc,
                clonedFrom = cloneFrom > 0 ? cloneFrom : (ulong?)null,
                stats = progress.Stats.Count,
                hasProgress = progress.Stats.Count > 0,
            });
            return;
        }

        // POST /profiles/{id}/clone  {"from":234471775}
        if (parts.Length == 3 && parts[2] == "clone" && method == "POST")
        {
            if (!ulong.TryParse(parts[1], out var target))
            {
                SendJson(ctx, 400, new { error = "personaref должен быть числом" });
                return;
            }

            if (store.Find(target) is null)
            {
                SendJson(ctx, 404,
                    new { error = "солдат не найден", personaref = target });
                return;
            }

            using var rd = new StreamReader(ctx.Request.InputStream, Encoding.UTF8);
            var raw = rd.ReadToEnd();

            ulong from = 0;
            try
            {
                using var d2 = JsonDocument.Parse(raw);
                if (d2.RootElement.TryGetProperty("from", out var f) &&
                    f.ValueKind == JsonValueKind.Number)
                {
                    from = f.GetUInt64();
                }
            }
            catch (JsonException)
            {
                // пустое тело — берём первого игрока с прогрессом
            }

            if (from == 0)
            {
                from = store.All
                    .Where(p => store.GetProgress(p.Id).Stats.Count > 0)
                    .Select(p => p.Id)
                    .FirstOrDefault();
            }

            if (from == 0 || store.Find(from) is null)
            {
                SendJson(ctx, 404, new { error = "нет игрока с прогрессом" });
                return;
            }

            store.CloneProgress(from, target);
            var prog = store.GetProgress(target);
            SendJson(ctx, 200, new
            {
                personaref = target,
                clonedFrom = from,
                stats = prog.Stats.Count,
            });
            return;
        }

        // POST /profiles/{id}/rename  {"name":"..."}
        // Переименование солдата (вкладка «Профили» лаунчера). Имя — как
        // у Create: уникальное, без пустых.
        if (parts.Length == 3 && parts[2] == "rename" && method == "POST")
        {
            if (!ulong.TryParse(parts[1], out var rid))
            {
                SendJson(ctx, 400, new { error = "personaref должен быть числом" });
                return;
            }

            using (var rd = new StreamReader(ctx.Request.InputStream, Encoding.UTF8))
            {
                var raw = rd.ReadToEnd();
                string name;
                try
                {
                    using var d = JsonDocument.Parse(raw);
                    name = d.RootElement.TryGetProperty("name", out var n)
                        ? n.GetString() ?? ""
                        : "";
                }
                catch (JsonException)
                {
                    SendJson(ctx, 400, new { error = "ожидался JSON с полем name" });
                    return;
                }

                var renamed = store.Rename(rid, name);
                if (renamed is null)
                {
                    SendJson(ctx, 409, new { error = "имя занято, пустое или солдат не найден" });
                    return;
                }

                SendJson(ctx, 200, new
                {
                    personaref = renamed.Id,
                    name = renamed.Name,
                });
            }
            return;
        }

        // DELETE /profiles/{id}
        if (parts.Length == 2 && method == "DELETE")
        {
            if (!ulong.TryParse(parts[1], out var pid))
            {
                SendJson(ctx, 400, new { error = "personaref должен быть числом" });
                return;
            }

            var ok = store.Delete(pid);
            SendJson(ctx, ok ? 200 : 404,
                ok ? new { deleted = pid } : new { error = "солдат не найден" });
            return;
        }

        SendJson(ctx, 405, new { error = "метод не поддерживается" });
    }

    private object Overview() => new
    {
        service = "EV Backend",
        version = "0.1.0",
        purpose = "локальный аналог Zeus в Venice Unleashed",
        internet = false,
        ports = new
        {
            redirector = cfg.RedirectorPort,
            blaze = cfg.BlazePort,
            http = cfg.HttpPort,
            qos = cfg.QosPort,
            gameServer = cfg.GameServerAddress,
            rcon = 47900,
        },
        server = new
        {
            id = cfg.ServerId,
            name = serverName,
            instance = cfg.InstanceDir,
            advertisedIp = cfg.AdvertisedIp,
            configuredMods = services.ConfiguredMods.Count,
            mapList = catalog.ReadMapList().Count,
        },

        // конфигурация СВОЕГО сервера отдельно от списка /servers:
        // в списке — только живые, здесь — «я хочу поднять вот это»
        ownServer = OwnServerView(),
        sessions = services.Sessions.Count,
        games = services.Games.All.Count,
        players = services.Games.TotalPlayers,
        stats = new
        {
            ok = stats.Ok,
            group = stats.Group,
            count = stats.StatCount,
        },
        profiles = store.All.Count,
        accounts = accounts.All.Count,
    };

    private object ServerView(GameInfo g) => new
    {
        id = g.GameId.ToString(),
        name = serverName,
        gameId = g.GameId,
        state = g.State,
        finalized = g.Finalized,
        players = g.PlayerCount,
        address = $"{cfg.AdvertisedIp}:{cfg.GameServerPort}",
        instance = cfg.InstanceDir,
        map = catalog.ReadMapList().FirstOrDefault()?.MapId ?? "?",
        mode = catalog.ReadMapList().FirstOrDefault()?.GamemodeId ?? "?",
        maps = catalog.ReadMapList().Count,
        maxPlayers = ReadMaxPlayers(),
        mods = services.ConfiguredMods,
        unlisted = cfg.Unlisted,
        // 26.10: частота движка сервера (30/60/120) — клиент при входе ставит ту же, как у VU
        frequency = cfg.Frequency,
        // без собственного логина сервера (bf3_1) — он не игрок
        players_detail = g.HumanPlayers.Select(p => new
        {
            personaref = p.Personaref,
            name = p.SoldierName,
        }).ToList(),
    };

    /// <summary>vars.maxPlayers из Startup.txt, иначе 64.</summary>
    private int ReadMaxPlayers() =>
        int.TryParse(catalog.StartupValue("vars.maxPlayers"), out var v)
            ? v
            : 64;

    /// <summary>
    /// Конфигурация СВОЕГО сервера (по параметрам запуска), для вкладки
    /// «Сервер» лаунчера и GET /. Это НЕ запись в списке серверов: в
    /// /servers попадают только живые, зарегистрировавшие игру.
    /// </summary>
    internal object OwnServerView() => new
    {
        id = cfg.ServerId ?? "",
        name = serverName,
        configured = true,
        running = services.Games.All.Count > 0,
        address = $"{cfg.AdvertisedIp}:{cfg.GameServerPort}",
        instance = cfg.InstanceDir,
        map = catalog.ReadMapList().FirstOrDefault()?.MapId ?? "?",
        mode = catalog.ReadMapList().FirstOrDefault()?.GamemodeId ?? "?",
        maxPlayers = ReadMaxPlayers(),
    };

    private static readonly JsonSerializerOptions JsonOut = new()
    {
        WriteIndented = true,
        // читаемый вывод: кириллица не как \uXXXX
        Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
    };

    internal static void SendJson(HttpListenerContext ctx, int status, object body)
    {
        var json = JsonSerializer.Serialize(body, JsonOut);

        var bytes = Encoding.UTF8.GetBytes(json);
        ctx.Response.StatusCode = status;
        ctx.Response.ContentType = "application/json; charset=utf-8";
        ctx.Response.ContentLength64 = bytes.Length;
        ctx.Response.OutputStream.Write(bytes, 0, bytes.Length);
    }

    internal static void SendError(HttpListenerContext ctx, int status, string message)
    {
        ctx.Response.StatusCode = status;
        var bytes = Encoding.UTF8.GetBytes(message);
        ctx.Response.ContentType = "text/plain; charset=utf-8";
        ctx.Response.ContentLength64 = bytes.Length;
        ctx.Response.OutputStream.Write(bytes, 0, bytes.Length);
    }
}
