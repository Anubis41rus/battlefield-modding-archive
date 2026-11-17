using System.Net;
using System.Net.Sockets;
using System.Text;
using EVBots.Core;

namespace EVBots.Tests;

/// <summary>
/// Проверки без запуска процессов стенда: аргументы сверяются с stand\run\start.ps1 и join.ps1,
/// разбор JSON — с форматом бэкенда (WebHost.cs), правка Config.lua — на временной копии.
/// Запуск: dotnet run --project launcher\src\EVBots.Tests
/// </summary>
internal static class Program
{
    private static int _passed;
    private static int _failed;

    private static int Main()
    {
        Run("Логин и пароль (правила AccountsStore.cs)", LoginAndPassword);
        Run("Имя сервера, макс. игроков, боты", ServerFormRules);
        Run("Выбор загрузчика (evbots-loader, иначе activation-loader)", LoaderChoice);
        Run("Аргументы сервера (run\\start.ps1)", ServerArgs);
        Run("Аргументы бэкенда (run\\start.ps1)", BackendArgs);
        Run("Вход клиента (run\\join.ps1)", ClientArgs);
        Run("LAN: broadcast и объявления", DiscoveryRules);
        Run("Config.lua: InitNumberOfBots (побайтово)", FunBotsConfigRules);
        Run("Хвост лога (LogTail)", LogTailRules);
        Run("Разбор ответов бэкенда", BackendParsing);
        Run("Занятость портов и процессы", PortsAndProcesses);

        Console.WriteLine();
        Console.WriteLine($"Итог: прошло {_passed}, упало {_failed}");
        return _failed == 0 ? 0 : 1;
    }

    private static void Run(string title, Action test)
    {
        Console.WriteLine(title);
        try
        {
            test();
        }
        catch (Exception ex)
        {
            _failed++;
            Console.WriteLine("  EXCEPTION " + ex.GetType().Name + ": " + ex.Message);
        }
    }

    private static void Check(bool condition, string what)
    {
        if (condition)
        {
            _passed++;
            Console.WriteLine("  ok    " + what);
        }
        else
        {
            _failed++;
            Console.WriteLine("  FAIL  " + what);
        }
    }

    /// <summary>Значение после ключа в списке аргументов (или null).</summary>
    private static string? Pair(IReadOnlyList<string> args, string key)
    {
        var i = args.ToList().IndexOf(key);
        return i >= 0 && i + 1 < args.Count ? args[i + 1] : null;
    }

    private static void LoginAndPassword()
    {
        Check(Validation.Login("ab") is not null, "логин короче 3 символов отклоняется");
        Check(Validation.Login("abc") is null, "логин из 3 символов допустим");
        Check(Validation.Login("bf3_1") is null, "логин с «_» и цифрами допустим");
        Check(Validation.Login("a b") is not null, "логин с пробелом отклоняется");
        Check(Validation.Login("кот") is not null, "логин кириллицей отклоняется");
        Check(Validation.Login(new string('a', 33)) is not null, "логин длиннее 32 отклоняется");
        Check(Validation.Login("") is not null, "пустой логин отклоняется");
        Check(Validation.Password("123") is not null, "пароль короче 4 отклоняется");
        Check(Validation.Password("1234") is null, "пароль из 4 символов допустим");
    }

    private static void ServerFormRules()
    {
        Check(Validation.ServerName("EVBots") is null, "имя сервера допустимо");
        Check(Validation.ServerName("") is not null, "пустое имя отклоняется");
        Check(Validation.ServerName("a\"b") is not null, "кавычки в имени отклоняются");
        Check(Validation.ServerName("a&b") is not null && Validation.ServerName("a%b") is not null, "символы cmd (& %) в имени отклоняются");
        Check(Validation.MaxPlayers(64) is null && Validation.MaxPlayers(65) is not null, "макс. игроков: 64 ок, 65 нет");
        Check(Validation.Bots(0) is null && Validation.Bots(129) is not null, "боты: 0 ок, 129 нет");
        Check(Validation.MapOrMode("MP_001", "Карта") is null, "карта MP_001 допустима");
        Check(Validation.MapOrMode("MP 001", "Карта") is not null, "карта с пробелом отклоняется");
    }

    private static void LoaderChoice()
    {
        var dir = Path.Combine(Path.GetTempPath(), "evbots-loader-choice-" + Guid.NewGuid().ToString("N")[..8]);
        Directory.CreateDirectory(dir);
        try
        {
            Check(Paths.FindLoader(dir) is null, "нет загрузчика — null");
            File.WriteAllText(Path.Combine(dir, Paths.LegacyLoaderName), "");
            Check(Paths.FindLoader(dir) == Path.Combine(dir, Paths.LegacyLoaderName), "старая установка — activation-loader.exe");
            File.WriteAllText(Path.Combine(dir, Paths.LoaderName), "");
            Check(Paths.FindLoader(dir) == Path.Combine(dir, Paths.LoaderName), "есть свой — evbots-loader.exe важнее");
        }
        finally
        {
            Directory.Delete(dir, true);
        }
    }

    private static void ServerArgs()
    {
        var s = new LauncherSettings();
        var plan = ServerPlan.Server(s, @"C:\x\loader.log");

        Check(plan.FileName == Paths.Loader && plan.FileName.EndsWith(@"\evbots-loader.exe", StringComparison.Ordinal),
            "запуск через evbots-loader (как $Loader в run\\common.ps1)");
        Check(plan.WorkDir == Paths.KitRoot, "рабочая папка — комплект");
        Check(plan.Args.Contains("--dedicated") && plan.Args.Contains("--wait"), "флаги --dedicated и --wait");
        Check(Pair(plan.Args, "--exe") == "bf3_server_Final.exe", "exe сервера");
        Check(Pair(plan.Args, "--dll") == "EVBots.dll", "--dll EVBots.dll");
        Check(Pair(plan.Args, "--log") == @"C:\x\loader.log", "--log — файл вывода загрузчика");
        Check(Pair(plan.Args, "-serverInstancePath") == Paths.InstanceDir, "-serverInstancePath = Instance");
        Check(Pair(plan.Args, "-Server.ServerName") == "EVBots", "имя сервера");
        Check(Pair(plan.Args, "-GamePort") == "25300", "игровой порт 25300");
        Check(Pair(plan.Args, "-Network.MaxClientCount") == "64", "макс. игроков 64");
        Check(Pair(plan.Args, "-RemoteAdminPort") == "127.0.0.1:47900", "RCON только на 127.0.0.1:47900");
        Check(Pair(plan.Args, "+admin.password") == "change-this-rcon-password", "пароль RCON как в run\\start.ps1");
        Check(Pair(plan.Args, "-Login") == "bf3_1", "логин сервера bf3_1");
        Check((Pair(plan.Args, "-Server.ActiveLicenses") ?? "").StartsWith("premium_xp4,xp2_ks", StringComparison.Ordinal), "список лицензий как в run\\start.ps1");

        Check(plan.Env["BF3ACT_MODE"] == "dedicated" && plan.Env["BF3ACT_DRM"] == "0", "BF3ACT_MODE и BF3ACT_DRM");
        Check(plan.Env["BF3ACT_LOG"] == Paths.LuaLog, "BF3ACT_LOG = EVBots.log");
        Check(plan.Env["BF3ACT_DNSREDIR"] == Ports.ServerSinkhole, "редирект EA-хостов (сервер: + bf3.gos.ea.com)");
        Check(plan.Env.ContainsKey("BF3ACT_DNSREDIR_IP") && plan.Env["BF3ACT_DNSREDIR_IP"] is null, "BF3ACT_DNSREDIR_IP на сервере убирается");
        Check(plan.Env["EVBOTS_LIVE_SEARCH"] == "1", "EVBOTS_LIVE_SEARCH=1 (стенд; SPM_CUSTOM по умолчанию ВКЛ)");
        Check(!plan.Env.ContainsKey("EVBOTS_SPM_SPAWN") && !plan.Env.ContainsKey("EVBOTS_AI_PLAYER"), "мёртвые флаги 168–169 не задаются");
        Check(plan.Env["EVBOTS_BRIDGE"] == "1" && plan.Env["EVBOTS_SPM_MGR"] == "1", "мост и менеджер включены (run\\common.ps1)");
        Check(plan.Env["EVBOTS_SPM_PROBE"] == "0" && plan.Env["EVBOTS_SPM_CREATE"] == "0", "пробник и createPlayer-хук выключены");
        Check(plan.Env["EVBOTS_EBX_INDEX"] == Paths.EbxIndex, "индекс EBX из комплекта");
        Check(!plan.Env.ContainsKey("EVBOTS_TESTS"), "тестов по умолчанию нет");

        var tests = ServerPlan.Server(new LauncherSettings { EngineTests = true }, "x");
        Check(tests.Env["EVBOTS_TESTS"] == "1" && tests.Env["EVBOTS_PTS_REG"] == "1", "тесты движка: EVBOTS_TESTS и PTS_REG (run\\start.ps1)");

        var off = ServerPlan.Server(new LauncherSettings { UseBridge = false, UseManager = false }, "x");
        Check(off.Env["EVBOTS_BRIDGE"] == "0" && off.Env["EVBOTS_SPM_TICK"] == "0", "мост и менеджер выключаются");

        var display = plan.Display();
        Check(display.Contains("***") && !display.Contains("change-this-rcon-password"), "пароль RCON маскируется в логе");
    }

    private static void BackendArgs()
    {
        var local = ServerPlan.Backend(new LauncherSettings(), "127.0.0.1");
        Check(local.Args[0] == "serve", "первый аргумент — serve");
        Check(Pair(local.Args, "--http-port") == "8080", "HTTP 8080");
        Check(Pair(local.Args, "--redirector-port") == "42127", "редиректор 42127");
        Check(Pair(local.Args, "--blaze-port") == "10041", "Blaze 10041");
        Check(Pair(local.Args, "--instance") == Paths.InstanceDir, "--instance = инстанс сервера");
        Check(Pair(local.Args, "--evbots-log") == Paths.LuaLog, "--evbots-log = EVBots.log");
        Check(Pair(local.Args, "--advertise-ip") == "127.0.0.1", "по умолчанию адрес 127.0.0.1");
        Check(Pair(local.Args, "--game-server-address") == "127.0.0.1:25300", "игровой адрес 127.0.0.1:25300");

        var lan = ServerPlan.Backend(new LauncherSettings { ServerName = "Мой сервер" }, "192.168.1.5");
        Check(Pair(lan.Args, "--game-server-address") == "192.168.1.5:25300", "LAN: игровой адрес = LAN-IP");
        Check(Pair(lan.Args, "--advertise-ip") == "192.168.1.5", "LAN: advertise-ip = LAN-IP");
        Check(Pair(lan.Args, "--server-name") == "Мой сервер", "имя сервера передаётся бэкенду");
        Check(lan.WorkDir == Paths.KitRoot + @"\backend", "рабочая папка — backend комплекта");
    }

    private static void ClientArgs()
    {
        var params0 = ClientPlan.RequestStateParams(1, 234471775, "127.0.0.1:25300");
        Check(params0 == "<data putinsquad=\"false\" gameid=\"1\" personaref=\"234471775\" levelmode=\"mp\" serveraddr=\"127.0.0.1:25300\"/>",
            "requestStateParams совпадает с run\\join.ps1");

        var plan = ClientPlan.Join(new LauncherSettings(), "192.168.1.5", 3, 234471782, 25300);
        Check(plan.FileName == Paths.Loader, "вход через evbots-loader");
        Check(plan.Args.Contains("--client") && plan.Args.Contains("--drm-strict"), "флаги --client и --drm-strict");
        Check(Pair(plan.Args, "--game") == Paths.ClientGameDir, "клиентская игра C:\\Games\\Battlefield 3");
        Check((Pair(plan.Args, "--license-artifact") ?? "").EndsWith(@"license\71067.dlf", StringComparison.Ordinal), "лицензия 71067.dlf");
        Check(Pair(plan.Args, "-requestState") == "State_ClaimReservation", "State_ClaimReservation");
        Check(Pair(plan.Args, "-requestStateParams") == ClientPlan.RequestStateParams(3, 234471782, "192.168.1.5:25300"),
            "параметры входа: gameid, personaref, serveraddr хоста");
        Check(plan.Env["BF3ACT_DNSREDIR_IP"] == "192.168.1.5", "BF3ACT_DNSREDIR_IP = хост");
        Check(plan.Env["BF3ACT_DNSREDIR"] == Ports.Sinkhole, "редирект EA-хостов на клиенте");
        Check(plan.Args.Contains("-windowed") && Pair(plan.Args, "-w") == "1280", "оконный режим 1280x720 по умолчанию");

        var full = ClientPlan.Join(new LauncherSettings { Windowed = false }, "127.0.0.1", 1, 1, 25300);
        Check(!full.Args.Contains("-windowed"), "полноэкранный режим, если выключено");
    }

    private static void DiscoveryRules()
    {
        Check(LanInfo.Broadcast(IPAddress.Parse("192.168.1.10"), IPAddress.Parse("255.255.255.0")).ToString() == "192.168.1.255",
            "broadcast для 192.168.1.10/24");
        Check(LanInfo.Broadcast(IPAddress.Parse("10.6.7.1"), IPAddress.Parse("255.0.0.0")).ToString() == "10.255.255.255",
            "broadcast для 10.6.7.1/8");
        Check(LanInfo.BroadcastTargets().Any(a => a.ToString() == "255.255.255.255"), "255.255.255.255 входит в цели");

        var ann = new LanAnnouncement
        {
            Instance = "abc",
            Host = "PC1",
            Ip = "192.168.1.5",
            HttpPort = 8080,
            Servers = { new AnnouncedServer { Name = "Тест Сервер", GameId = 7, GamePort = 25300, Map = "MP_001", Mode = "ConquestSmall0", Players = 2, MaxPlayers = 64, Bots = 8 } },
        };
        var bytes = Encoding.UTF8.GetBytes(System.Text.Json.JsonSerializer.Serialize(ann));
        var back = LanDiscovery.Parse(bytes);
        Check(back is not null && back.Servers.Count == 1 && back.Servers[0].Name == "Тест Сервер", "объявление: кириллица в имени сохраняется");
        Check(back is not null && back.Servers[0].GameId == 7 && back.Ip == "192.168.1.5" && back.HttpPort == 8080, "объявление: gameId, IP, HTTP-порт");
        Check(LanDiscovery.Parse(Encoding.UTF8.GetBytes("{\"app\":\"other\",\"v\":1}")) is null, "чужое приложение отбрасывается");
        Check(LanDiscovery.Parse(Encoding.UTF8.GetBytes("не json")) is null, "битый пакет отбрасывается");
        Check(LanInfo.Addresses().Count >= 0, "перечисление адаптеров не падает");
    }

    private static void FunBotsConfigRules()
    {
        var dir = NewTempDir();
        try
        {
            var cfg = Path.Combine(dir, "Config.lua");
            var original = "\uFEFF-- комментарий\r\n\tSpawnMode = SpawnModes.balanced_teams,\r\n\tInitNumberOfBots = 8,\t\t\t\t-- Bots for spawnmode\r\n\tMaxBots = 2,\r\n";
            File.WriteAllBytes(cfg, new UTF8Encoding(true).GetBytes(original));

            Check(FunBotsConfig.ReadInitBots(cfg) == 8, "чтение InitNumberOfBots = 8");

            var backupDir = Path.Combine(dir, "backup");
            var backup = FunBotsConfig.Apply(cfg, 12, backupDir);
            Check(backup.Length > 0 && File.Exists(backup), "перед записью создана копия в backup");

            var after = File.ReadAllBytes(cfg);
            Check(after.Length >= 3 && after[0] == 0xEF && after[1] == 0xBB && after[2] == 0xBF, "BOM сохранён");
            var text = new UTF8Encoding(false).GetString(after, 3, after.Length - 3);
            Check(text == original.Substring(1).Replace("= 8,", "= 12,"), "изменились только цифры, остальное байт в байт");
            Check(FunBotsConfig.ReadInitBots(cfg) == 12, "новое значение читается");
            Check(FunBotsConfig.Apply(cfg, 12, backupDir) == "", "то же значение — файл не трогается");

            var noKey = Path.Combine(dir, "NoKey.lua");
            File.WriteAllText(noKey, "x = 1\n");
            var threw = false;
            try
            {
                FunBotsConfig.Apply(noKey, 5, backupDir);
            }
            catch (InvalidOperationException)
            {
                threw = true;
            }

            Check(threw, "файл без InitNumberOfBots не портится, а даёт ошибку");
            Check(File.ReadAllText(noKey) == "x = 1\n", "файл без ключа не изменён");
        }
        finally
        {
            Directory.Delete(dir, true);
        }
    }

    private static void LogTailRules()
    {
        var dir = NewTempDir();
        try
        {
            var file = Path.Combine(dir, "EVBots.log");
            File.WriteAllText(file, "a\nb\nc\n");

            var tail = new LogTail();
            Check(tail.Attach(file, 10).SequenceEqual(new[] { "a", "b", "c" }), "хвост: три строки");

            File.AppendAllText(file, "d\ne");
            Check(tail.ReadNew().SequenceEqual(new[] { "d" }), "новые строки; неполная последняя ждёт");

            File.AppendAllText(file, "\n");
            Check(tail.ReadNew().SequenceEqual(new[] { "e" }), "неполная строка дописывается при переводе строки");

            Check(tail.ReadNew().Count == 0, "без новых данных — пусто");
        }
        finally
        {
            Directory.Delete(dir, true);
        }
    }

    private static void BackendParsing()
    {
        var accounts = BackendClient.ParseAccounts(
            "{\"count\":2,\"accounts\":[{\"login\":\"KOTuk1\",\"personaref\":234471788,\"name\":\"KOTuk1\",\"createdUtc\":\"x\",\"soldiers\":1}," +
            "{\"login\":\"bf3_1\",\"personaref\":234471782,\"name\":\"bf3_1\",\"createdUtc\":\"y\",\"soldiers\":2}]}");
        Check(accounts.Count == 2 && accounts[0].Login == "KOTuk1" && accounts[0].Personaref == 234471788, "разбор GET /accounts");
        Check(accounts[1].Soldiers == 2, "число солдат");
        Check(BackendClient.ParseAccounts("{}").Count == 0, "пустой ответ — пустой список");

        var games = BackendClient.ParseServers(
            "{\"count\":1,\"servers\":[{\"id\":\"3\",\"name\":\"EVBots\",\"gameId\":3,\"state\":2,\"players\":2," +
            "\"address\":\"127.0.0.1:25300\",\"map\":\"MP_001\",\"mode\":\"ConquestSmall0\",\"maxPlayers\":64}]}");
        Check(games.Count == 1 && games[0].GameId == 3, "разбор GET /servers: gameId");
        Check(games[0].Address == "127.0.0.1:25300" && games[0].Map == "MP_001" && games[0].Mode == "ConquestSmall0", "адрес, карта, режим");
        Check(games[0].Players == 2 && games[0].MaxPlayers == 64 && games[0].State == "2", "игроки, лимит, состояние (число)");

        Check(BackendClient.ErrorText("{\"error\":\"пароль минимум 4 символов\"}", 400) == "пароль минимум 4 символов",
            "текст ошибки берётся из поля error");
        Check(BackendClient.ErrorText("<html>", 500) == "HTTP 500", "не JSON — код HTTP");
    }

    private static void PortsAndProcesses()
    {
        var listener = new TcpListener(IPAddress.Any, 0);
        listener.Start();
        var port = ((IPEndPoint)listener.LocalEndpoint).Port;
        try
        {
            Check(Ports.Busy(port, true) is not null, "занятый TCP-порт распознаётся");
        }
        finally
        {
            listener.Stop();
        }

        Check(Ports.Busy(port, true) is null, "после остановки слушателя порт свободен");
        Check(ProcessUtil.CountByName("no_such_process_evbots_test") == 0, "несуществующий процесс — 0");

        var dir = NewTempDir();
        try
        {
            File.WriteAllText(Path.Combine(dir, "RuntimeLog_PC1_server.1.log"), "old\n");
            File.WriteAllText(Path.Combine(dir, "RuntimeLog_PC1_server.2.log"), "older\n");
            Thread.Sleep(20);
            File.WriteAllText(Path.Combine(dir, "RuntimeLog_PC1_server.log"), "new\n");
            var newest = ProcessUtil.NewestRuntimeLog(dir) ?? "";
            Check(Path.GetFileName(newest) == "RuntimeLog_PC1_server.log", "свежий RuntimeLog без номера ротации");
        }
        finally
        {
            Directory.Delete(dir, true);
        }
    }

    private static string NewTempDir()
    {
        var dir = Path.Combine(Path.GetTempPath(), "evbots-launcher-tests-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        return dir;
    }
}
