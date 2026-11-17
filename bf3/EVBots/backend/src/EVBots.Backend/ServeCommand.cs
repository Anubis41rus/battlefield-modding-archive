// ServeCommand.cs — запуск сервиса. Каркас фазы 2, шаг 1.

using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using EVBots.Backend.Blaze;
using EVBots.Backend.Data;
using EVBots.Backend.Http;

namespace EVBots.Backend;

internal static class ServeCommand
{
    public static int Run(string[] args)
    {
        var cfg = ServeConfig.FromArgs(args);
        var log = new Log(cfg.Debug);

        log.Info("EV Backend - starting");
        log.Info($"  Blaze redirector : TCP {cfg.RedirectorPort}");
        log.Info($"  Blaze core       : TCP {cfg.BlazePort}");
        log.Info($"  HTTP (browser)   : TCP {cfg.HttpPort}");
        log.Info($"  QoS              : UDP {cfg.QosPort}");
        log.Info($"  EV channel       : {(cfg.EvPort > 0 ? "TCP " + cfg.EvPort : "off")}");
        log.Info($"  game server      : {cfg.GameServerAddress}");
        log.Info($"  profiles         : {cfg.ProfilesDir}");
        log.Info($"  progress         : {cfg.ProgressDir}");
        log.Info($"  stats catalog    : {cfg.StatsCatalogPath}" +
                 (File.Exists(cfg.StatsCatalogPath) ? "" : "  [NOT FOUND]"));
        log.Info($"  server instance  : {cfg.InstanceDir}");
        log.Info($"  mods             : {cfg.ModsDir}");
        log.Info($"  advertised IP    : {cfg.AdvertisedIp}");
        log.Info("");

        Directory.CreateDirectory(cfg.ProfilesDir);
        Directory.CreateDirectory(cfg.ProgressDir);

        // ── хранилище ──────────────────────────────────────────────────────
        var store = new ProfileStore(cfg, log);
        store.Load();
        var accounts = new AccountsStore(
            Path.Combine(cfg.ProfilesDir, "accounts.json"), store, log);
        accounts.Load();
        var stats = new StatsCatalog(cfg.StatsCatalogPath, log);
        stats.Load();
        log.Info($"  stats catalog    : {stats.StatCount} stats, " +
                 $"group '{stats.Group}', payload {stats.PayloadLength:N0} bytes");

        // ── инстанс сервера (Admin\Startup.txt и т.д.) ─────────────────────
        var catalog = new InstanceCatalog(cfg.InstanceDir, cfg.ModsDir, log);
        catalog.EnsureLayout();

        // имя сервера: приоритет у явного --server-name (лаунчер берёт его из
        // server.config), иначе Startup.txt — как у настоящего BF3.
        var serverName = cfg.ServerNameExplicit
            ? cfg.ServerName
            : catalog.ServerNameFromInstance();
        if (string.IsNullOrWhiteSpace(serverName))
        {
            serverName = cfg.ServerName;
        }

        // синхронизируем instance, чтобы Startup.txt не врал в списке серверов
        if (!string.Equals(catalog.ServerNameFromInstance(), serverName,
                StringComparison.Ordinal))
        {
            catalog.SetStartupValue("vars.serverName", serverName);
        }

        var mapList = catalog.ReadMapList();
        log.Info($"  instance         : {mapList.Count} maps in rotation, " +
                 $"{catalog.ReadModList().Count} mods in ModList.txt, " +
                 $"{catalog.AvailableMods().Count} mods available");
        log.Info("");

        // ── TLS ────────────────────────────────────────────────────────────
        //
        // Клиент dirtysock (bf3.exe) на рукопожатии принимает только лист
        // структуры настоящего EA: issuer «OTG3 Certificate Authority»,
        // subject gosredirector.ea.com, RSA 1024, SHA1, без расширений
        // (эталон снят с рабочего бэкенда 09.10.2026). Наш pfx с issuer
        // «Root Agency» клиент рвёт сразу после Certificate, поэтому pfx
        // из каталога сертификатов больше не используем. Подпись клиент не
        // проверяет — рабочий бэкенд генерирует свежий лист на каждом
        // старте и ставит ОДИН сертификат и на редиректор, и на ядро.
        var otg3Cert = Blaze.Otg3Cert.Generate(log);

        // ── компоненты Blaze ───────────────────────────────────────────────
        var services = new BlazeServices(cfg, store, accounts, stats, catalog, log);

        var redirector = new BlazeListener(
            cfg.RedirectorPort, otg3Cert, services, isRedirector: true, log);
        var blaze = new BlazeListener(
            cfg.BlazePort, otg3Cert, services, isRedirector: false, log);

        var qos = new QosListener(cfg.QosPort, log);
        // 20.10: server.xml для сервера (bf3.gos.ea.com -> 127.0.0.1:80), как Apache у BF-Portable, без ScoreMultiplier
        var editorial = cfg.Editorial
            ? new Http.EditorialHttp(cfg.EditorialFile ?? Http.EditorialHttp.FindServerXml(cfg.InstanceDir), log)
            : null;
        var web = new WebHost(cfg, services, store, accounts, stats, catalog,
            serverName, log);
        // 23.10: EV-канал — функции Zeus для EVBots.dll и лаунчера, та же база аккаунтов (backend\EV-PROTOCOL.md)
        var ev = cfg.EvPort > 0 ? new Ev.EvListener(cfg.EvPort, new Ev.EvService(accounts, log), log) : null;

        try
        {
            redirector.Start();
            blaze.Start();
            qos.Start();
            web.Start();
            editorial?.Start();
            ev?.Start();
        }
        catch (Exception ex)
        {
            log.Error($"cannot bind port: {ex.Message}");
            return 1;
        }

        log.Info("all listeners started, waiting for connections");
        log.Info($"  guest connects: EVBots.exe -> {cfg.RedirectorPort}, " +
                 $"or directly {cfg.BlazePort}");
        log.Info("");

        // Lua-статус: окно frost-сервера (Win32-GUI, без консоли) принять
        // чужие строки не умеет, поэтому единственное место, где видно и
        // бэкенд, и Lua — это консоль бэкенда (её показывает лаунчер).
        // Тайлим EVBots.log с конца файла, только значимые маркеры.
        if (cfg.LuaLogPath.Length > 0)
        {
            log.Info($"  Lua log (tail): {cfg.LuaLogPath}");
            log.Info("");
        }

        var luaTail = ServeConfig.TailLuaLog(cfg, log);

        using var stop = new CancellationTokenSource();
        Console.CancelKeyPress += (_, e) =>
        {
            e.Cancel = true;
            log.Info("Ctrl+C received, stopping...");
            stop.Cancel();
        };

        // Уборка «призраков»: игра, по которой давно не приходило Blaze-
        // трафика (сервер убит/упал), больше не показывается в /servers.
        // Иначе гость видит сервер, которого нет, и клиент заходит в мёртвый
        // GID. Порог 90 с: живой сервер шлёт Blaze постоянно (createGame,
        // finalizeGameCreation, advanceGameState, heartbeat-подобные вызовы).
        var sweeper = new Timer(
            _ =>
            {
                var dead = services.Games.Sweep(TimeSpan.FromSeconds(90));
                foreach (var gid in dead)
                {
                    log.Warn($"game GID={gid} removed from list: server silent " +
                             $"for more than 90s (crashed or killed)");
                }
            },
            null, TimeSpan.FromSeconds(15), TimeSpan.FromSeconds(15));

        try
        {
            Task.Delay(Timeout.Infinite, stop.Token).GetAwaiter().GetResult();
        }
        catch (OperationCanceledException)
        {
            // штатная остановка
        }

        sweeper.Dispose();
        log.Info("");
        log.Info("stopping...");
        redirector.Stop();
        blaze.Stop();
        qos.Stop();
        web.Stop();
        log.Info("остановлено");
        return 0;
    }

}

/// <summary>Параметры запуска.</summary>
internal sealed class ServeConfig
{
    public int RedirectorPort { get; init; } = 42127;
    public int BlazePort { get; init; } = 10041;
    public int HttpPort { get; init; } = 8080;
    public int QosPort { get; init; } = 17554;

    /// <summary>EV-канал (Zeus-функции), TCP; 0 — выключен. Порт как у Zeus.</summary>
    public int EvPort { get; init; } = 7828;
    public string GameServerAddress { get; init; } = "127.0.0.1:25300";

    /// <summary>Адрес, который мы отдаём клиенту в preAuth (QOSS) и редиректоре.</summary>
    public IPAddress QosIp { get; init; } = IPAddress.Loopback;
    public string ProfilesDir { get; init; } = "profiles";
    public string ProgressDir { get; init; } = "progress";
    public string StatsCatalogPath { get; init; } = "";

    /// <summary>Инстанс выделенного сервера: Admin\Startup.txt и т.д.</summary>
    public string InstanceDir { get; init; } = "";
    public string ModsDir { get; init; } = "";

    /// <summary>EVBots.log — Lua-статус сервера (тайлим в консоль бэкенда).</summary>
    public string LuaLogPath { get; init; } = "";
    public string? AdvertiseIp { get; init; }
    public string ServerName { get; init; } = "EVBots LAN Server";

    /// <summary>--server-name передан явно (лаунчер читает его из server.config).</summary>
    public bool ServerNameExplicit { get; init; }
    public string? ServerId { get; init; }
    public bool Debug { get; init; }

    /// <summary>
    /// -unlisted: сервер не показывается в списке /servers, но остаётся
    /// доступным по id и адресу. Как в VU (docs general\args.md):
    /// «Makes the server not show up in the server list. Unlisted servers
    /// can only be joined by the connect command or via vu://join/server-id».
    /// </summary>
    public bool Unlisted { get; init; }

    /// <summary>
    /// 26.10: частота движка сервера (30, 60 или 120 Гц) — как у VU (-high60/-high120, «vu.FrequencyMode»).
    /// Отдаётся в /servers полем frequency: клиент друга при входе ставит ту же частоту (у VU клиент берёт её
    /// из списка серверов). Задаёт лаунчер --frequency; другое значение — 30.
    /// </summary>
    public int Frequency { get; init; } = 30;

    /// <summary>
    /// 20.10: при входе игрока слать игровому серверу UserAdded/UserUpdated о нём (до уведомления 1) и повторять
    /// уведомление 1 после PlayerJoining. Разбор bf3_server_Final.exe: обработчик уведомления 1 (0x0151D580)
    /// пишет расширенные данные только уже известному пользователю, а onPlayerAuthenticated (0x011F1AE0) без них
    /// даёт ассерт «Joined player has no blaze extended data». Выключить: --no-join-user-to-server.
    /// </summary>
    public bool JoinUserToServer { get; init; } = true;

    /// <summary>
    /// 20.10: коды GOS-достижений в FetchClientConfig. По умолчанию выкл.: с ними сервер ходит в интернет-сервис
    /// достижений EA и пишет «GosAchievementsGetRequest failed. DirtyHttp error = 1». Включить: --gos-achievements.
    /// </summary>
    public bool GosAchievements { get; init; }

    /// <summary>20.10: отдавать серверу server.xml (EditorialHttp). Выключить: --no-editorial.</summary>
    public bool Editorial { get; init; } = true;

    /// <summary>Свой путь к server.xml (--editorial-file); иначе ищем в BF-Portable\htdocs рядом с инстансом.</summary>
    public string? EditorialFile { get; init; }

    /// <summary>IP, который мы рекламируем в серверном браузере.</summary>
    public string AdvertisedIp => AdvertiseIp ?? DetectLanIp();

    /// <summary>
    /// Автодетект адреса в LAN: берём первый не-loopback IPv4. Аналог
    /// «backend auto-detect IP» в Zeus (server arg -joinaddr).
    /// </summary>
    private static string DetectLanIp()
    {
        try
        {
            foreach (var ni in System.Net.NetworkInformation
                         .NetworkInterface.GetAllNetworkInterfaces())
            {
                if (ni.OperationalStatus !=
                    System.Net.NetworkInformation.OperationalStatus.Up ||
                    ni.NetworkInterfaceType ==
                    System.Net.NetworkInformation.NetworkInterfaceType.Loopback)
                {
                    continue;
                }

                foreach (var ip in ni.GetIPProperties().UnicastAddresses)
                {
                    if (ip.Address.AddressFamily ==
                        System.Net.Sockets.AddressFamily.InterNetwork &&
                        !IPAddress.IsLoopback(ip.Address))
                    {
                        return ip.Address.ToString();
                    }
                }
            }
        }
        catch
        {
            // определить не удалось — останется 127.0.0.1
        }

        return "127.0.0.1";
    }

    public string DataDir { get; init; } =
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
                     "EVBots");

    public IPAddress GameServerIp { get; init; } = IPAddress.Loopback;
    public int GameServerPort { get; init; } = 25300;

    public static ServeConfig FromArgs(string[] args)
    {
        var dataDir = Program.GetOption(args, "--data-dir")
                      ?? Path.Combine(
                          Environment.GetFolderPath(
                              Environment.SpecialFolder.ApplicationData),
                          "EVBots");

        var progressDir = Program.GetOption(args, "--progress-dir")
                          ?? Path.Combine(Environment.CurrentDirectory, "progress");
        var catalog = Program.GetOption(args, "--stats-catalog")
                      ?? Path.Combine(progressDir, "bf3-stats-catalog.json");
        // адрес игрового сервера по умолчанию — авто-детект LAN IP: у любого
        // хоста всё работает без правки конфига (раньше тут был 127.0.0.1,
        // а в скриптах — захардкоженный IP одной машины).
        // Явная опция --game-server-address имеет приоритет.
        var advertiseOpt = Program.GetOption(args, "--advertise-ip");
        var gs = Program.GetOption(args, "--game-server-address")
                 ?? $"{advertiseOpt ?? DetectLanIp()}:25300";

        var (ip, port) = ParseAddress(gs);

        // инстанс сервера: сначала явная опция, потом server.config
        // существующего комплекта, потом каталог данных
        var instance = Program.GetOption(args, "--instance");
        if (string.IsNullOrEmpty(instance))
        {
            instance = FindInstanceInServerConfig() ??
                       Path.Combine(dataDir, "server");
        }

        var modsDir = Program.GetOption(args, "--mods-dir") ??
                      Path.Combine(instance, "Admin", "Mods");

        var serverId = Program.GetOption(args, "--server-id");
        if (string.IsNullOrEmpty(serverId))
        {
            // стабильный ID на основе пути к данным: не меняется между запусками
            serverId = StableServerId(dataDir);
        }

        // путь к логу Lua (EVBots.log) — чтобы консоль бэкенда показывала
        // Lua-статус (аналог VU: мастер-консоль видит, что мод запущен).
        var luaLog = Program.GetOption(args, "--evbots-log") ?? FindLuaLog();

        return new ServeConfig
        {
            RedirectorPort = Int(args, "--redirector-port", 42127),
            BlazePort = Int(args, "--blaze-port", 10041),
            HttpPort = Int(args, "--http-port", 8080),
            QosPort = Int(args, "--qos-port", 17554),
            EvPort = Int(args, "--ev-port", 7828),
            GameServerAddress = gs,
            GameServerIp = ip,
            GameServerPort = port,
            QosIp = IPAddress.TryParse(Program.GetOption(args, "--qos-ip") ?? "",
                out var qip) ? qip : ip,
            ProfilesDir = Program.GetOption(args, "--profiles-dir")
                          ?? Path.Combine(Environment.CurrentDirectory, "profiles"),
            ProgressDir = progressDir,
            StatsCatalogPath = catalog,
            InstanceDir = instance,
            ModsDir = modsDir,
            AdvertiseIp = advertiseOpt,
            ServerName = Program.GetOption(args, "--server-name")
                         ?? "EVBots LAN Server",
            // Явный --server-name (лаунчер берёт его из server.config) должен
            // побеждать Startup.txt: иначе переименование в конфиге молча
            // игнорировалось, потому что vars.serverName был записан однажды
            // и всегда выигрывал. Правку instance делаем, чтобы не расходились.
            ServerNameExplicit = Program.GetOption(args, "--server-name") is not null,
            ServerId = serverId,
            LuaLogPath = luaLog,
            Debug = Program.HasFlag(args, "--debug"),
            Unlisted = Program.HasFlag(args, "--unlisted"),
            Frequency = Int(args, "--frequency", 30) is 60 or 120 ? Int(args, "--frequency", 30) : 30,
            JoinUserToServer = !Program.HasFlag(args, "--no-join-user-to-server"),
            GosAchievements = Program.HasFlag(args, "--gos-achievements"),
            Editorial = !Program.HasFlag(args, "--no-editorial"),
            EditorialFile = Program.GetOption(args, "--editorial-file"),
            DataDir = dataDir,
        };
    }

    private static int Int(string[] args, string name, int fallback) =>
        int.TryParse(Program.GetOption(args, name), out var v) ? v : fallback;

    private static string FindLuaLog()
    {
        // бэкенд лежит в <kit>\backend, лог Lua — <kit>\logs\EVBots.log
        var candidates = new[]
        {
            Path.Combine(Environment.CurrentDirectory, "..", "logs", "EVBots.log"),
            Path.Combine(Environment.CurrentDirectory, "logs", "EVBots.log"),
        };

        var dir = Path.GetDirectoryName(Environment.ProcessPath ??
                                        Environment.CurrentDirectory);
        if (!string.IsNullOrEmpty(dir))
        {
            candidates =
                [.. candidates, Path.Combine(dir, "logs", "EVBots.log")];
        }

        return candidates.FirstOrDefault(File.Exists) ?? candidates[0];
    }

    /// <summary>
    /// Значимые Lua-маркеры — их тайлим в консоль бэкенда (её показывает
    /// лаунчер). Технический шум (установка хуков, сигнатуры, реестры) —
    /// нет. Ловим и английские (новые), и русские (старые сборки).
    /// </summary>
    private static bool IsLuaNotable(string line)
    {
        // технические строки установки хуков — не про мод/ботов, пропускаем
        if (line.Contains("сигнатура:", StringComparison.OrdinalIgnoreCase) ||
            line.Contains("signature:", StringComparison.OrdinalIgnoreCase) ||
            line.Contains("установлен", StringComparison.OrdinalIgnoreCase) ||
            line.Contains("hook installed", StringComparison.OrdinalIgnoreCase) ||
            line.Contains("hooks installed", StringComparison.OrdinalIgnoreCase) ||
            line.Contains("target ", StringComparison.OrdinalIgnoreCase) ||
            line.Contains("already hooked", StringComparison.OrdinalIgnoreCase) ||
            line.Contains("RegisterInstanceLoadHandler", StringComparison.OrdinalIgnoreCase) ||
            line.Contains("SearchForDataContainer", StringComparison.OrdinalIgnoreCase) ||
            line.Contains("SearchForInstanceByGuid", StringComparison.OrdinalIgnoreCase) ||
            line.Contains("No fireData", StringComparison.OrdinalIgnoreCase) ||
            line.Contains("Path not found", StringComparison.OrdinalIgnoreCase) ||
            line.Contains("CreatePlayer(", StringComparison.OrdinalIgnoreCase) ||
            line.Contains("SpawnSoldierAt", StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }

        if (line.Contains("LUA-MODE READY", StringComparison.OrdinalIgnoreCase) ||
            line.Contains("LUA-РЕЖИМ", StringComparison.OrdinalIgnoreCase) ||
            line.Contains("Lua ready", StringComparison.OrdinalIgnoreCase) ||
            line.Contains("test live player", StringComparison.OrdinalIgnoreCase) ||
            line.Contains("живой игрок", StringComparison.OrdinalIgnoreCase) ||
            line.Contains("onNewRound", StringComparison.OrdinalIgnoreCase) ||
            line.Contains("new round", StringComparison.OrdinalIgnoreCase) ||
            line.Contains("Level:Loaded", StringComparison.OrdinalIgnoreCase) ||
            line.Contains("BotCreator", StringComparison.OrdinalIgnoreCase) ||
            line.Contains("BotSpawner", StringComparison.OrdinalIgnoreCase) ||
            line.Contains("bot", StringComparison.OrdinalIgnoreCase) ||
            line.Contains("heartbeat", StringComparison.OrdinalIgnoreCase) ||
            line.Contains("[VeniceEXT]", StringComparison.OrdinalIgnoreCase) ||
            line.Contains("Loading VeniceEXT module", StringComparison.OrdinalIgnoreCase) ||
            line.Contains("Successfully loaded", StringComparison.OrdinalIgnoreCase) ||
            line.Contains("Engine:Update timer", StringComparison.OrdinalIgnoreCase))
        {
            return true;
        }

        return line.Contains("ERROR", StringComparison.OrdinalIgnoreCase) ||
               line.Contains("WARN", StringComparison.OrdinalIgnoreCase) ||
               line.Contains("unloaded", StringComparison.OrdinalIgnoreCase) ||
               line.Contains("выгружен", StringComparison.OrdinalIgnoreCase);
    }

    internal static Thread TailLuaLog(ServeConfig cfg, Log log)
    {
        var tail = new Thread(() =>
        {
            // старт «сейчас»: показываем только новые строки, а не историю.
            var pos = File.Exists(cfg.LuaLogPath)
                ? new FileInfo(cfg.LuaLogPath).Length
                : 0L;
            while (true)
            {
                try
                {
                    if (File.Exists(cfg.LuaLogPath))
                    {
                        using var fs = new FileStream(cfg.LuaLogPath,
                            FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
                        if (fs.Length > pos)
                        {
                            fs.Seek(pos, SeekOrigin.Begin);
                            using var r = new StreamReader(fs, Encoding.UTF8);
                            var text = r.ReadToEnd();
                            pos = fs.Position;
                            foreach (var line in text.Split('\n'))
                            {
                                var t = line.TrimEnd('\r');
                                if (t.Length == 0 || !IsLuaNotable(t))
                                {
                                    continue;
                                }

                                log.Info("[lua] " + t);
                            }
                        }
                    }
                }
                catch
                {
                    // файл может быть занят — пробуем дальше
                }

                Thread.Sleep(1000);
            }
        })
        {
            IsBackground = true,
        };
        tail.Start();
        return tail;
    }
    /// <summary>
    /// Ищет game.dedicatedInstance в server.config существующего комплекта,
    /// чтобы по умолчанию работать с тем же инстансом, что и раньше.
    /// Ищем от каталога данных вверх по дереву и в фиксированных местах.
    /// </summary>
    private static string? FindInstanceInServerConfig()
    {
        string[] roots =
        {
            Directory.GetCurrentDirectory(),
            @"C:\Games",
        };

        foreach (var root in roots)
        {
            var cfg = Path.Combine(root, "server.config");
            if (!File.Exists(cfg))
            {
                continue;
            }

            foreach (var line in File.ReadAllLines(cfg))
            {
                var s = line.Trim();
                if (!s.StartsWith("game.dedicatedInstance", StringComparison.Ordinal))
                {
                    continue;
                }

                var eq = s.IndexOf('=');
                if (eq > 0 && s[(eq + 1)..].Trim() is { Length: > 0 } v)
                {
                    return v;
                }
            }
        }

        return null;
    }

    private static (IPAddress, int) ParseAddress(string s)
    {
        var idx = s.LastIndexOf(':');
        if (idx > 0 &&
            int.TryParse(s[(idx + 1)..], out var port) &&
            IPAddress.TryParse(s[..idx], out var ip))
        {
            return (ip, port);
        }

        return (IPAddress.Loopback, 25300);
    }

    /// <summary>
    /// Детерминированный ID сервера без dashes — формат как у VU server.key.
    /// Считается от каталога данных, поэтому переживает перезапуск.
    /// </summary>
    private static string StableServerId(string dataDir)
    {
        var seed = System.Text.Encoding.UTF8.GetBytes(
            Path.GetFullPath(dataDir).ToLowerInvariant());
        var hash = System.Security.Cryptography.MD5.HashData(seed);
        return Convert.ToHexString(hash).ToLowerInvariant();
    }
}

/// <summary>Простой логгер в консоль.</summary>
internal sealed class Log(bool debug)
{
    public bool DebugEnabled { get; } = debug;

    public void Info(string msg) => Write("INFO", msg, ConsoleColor.Gray);
    public void Ok(string msg) => Write("OK  ", msg, ConsoleColor.Green);
    public void Warn(string msg) => Write("WARN", msg, ConsoleColor.Yellow);
    public void Error(string msg) => Write("ERR ", msg, ConsoleColor.Red);
    public void Debug(string msg)
    {
        if (DebugEnabled)
        {
            Write("DBG ", msg, ConsoleColor.DarkGray);
        }
    }

    private static readonly object Gate = new();

    private void Write(string level, string msg, ConsoleColor color)
    {
        var stamp = DateTime.Now.ToString("HH:mm:ss.fff");
        lock (Gate)
        {
            var prev = Console.ForegroundColor;
            Console.ForegroundColor = color;
            Console.Write($"[{stamp}] {level} ");
            Console.ForegroundColor = prev;
            Console.WriteLine(msg);
        }
    }
}