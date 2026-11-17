using System.Text;

namespace EVBots.Core;

/// <summary>Готовый запуск процесса: файл, аргументы (по одному элементу на аргумент), переменные окружения.</summary>
public sealed class ProcessPlan
{
    public string FileName { get; init; } = "";
    public string WorkDir { get; init; } = "";
    public List<string> Args { get; init; } = new();

    /// <summary>Переменные окружения. Значение null = убрать переменную из окружения процесса.</summary>
    public Dictionary<string, string?> Env { get; init; } = new();

    /// <summary>Командная строка для лога. Пароли маскируются.</summary>
    public string Display()
    {
        var sb = new StringBuilder(FileName);
        string? prev = null;
        foreach (var arg in Args)
        {
            var masked = prev is "+admin.password" or "-Password";
            sb.Append(' ');
            sb.Append(masked ? "***" : arg.Contains(' ') ? $"\"{arg}\"" : arg);
            prev = arg;
        }

        return sb.ToString();
    }
}

/// <summary>Сервер и бэкенд. Аргументы и переменные — как в stand\run\start.ps1 и common.ps1.</summary>
public static class ServerPlan
{
    /// <summary>RCON и пароль Startup.txt (server.config rcon.password, как в run\start.ps1).</summary>
    public const string RconPassword = "change-this-rcon-password";

    public static string Flag(bool on) => on ? "1" : "0";

    /// <summary>
    /// Бэкенд: serve с портами 42127/10041/8080 и адресом, который видят клиенты.
    /// advertiseIp = 127.0.0.1 (только этот ПК) или LAN-IP (открыт для LAN).
    /// </summary>
    public static ProcessPlan Backend(LauncherSettings settings, string advertiseIp) => new()
    {
        FileName = Paths.BackendExe,
        WorkDir = Path.GetDirectoryName(Paths.BackendExe) ?? Paths.KitRoot,
        Args =
        [
            "serve",
            "--redirector-port", Ports.Redirector.ToString(),
            "--blaze-port", Ports.Blaze.ToString(),
            "--http-port", Ports.Http.ToString(),
            "--profiles-dir", Paths.ProfilesDir,
            "--progress-dir", Paths.ProgressDir,
            "--stats-catalog", Paths.StatsCatalog,
            "--instance", Paths.InstanceDir,
            "--game-server-address", $"{advertiseIp}:{Ports.Game}",
            "--advertise-ip", advertiseIp,
            "--server-name", settings.ServerName,
            "--evbots-log", Paths.LuaLog,
        ],
    };

    /// <summary>
    /// Выделенный сервер через evbots-loader --dedicated. loaderLog — файл вывода загрузчика
    /// (как --log в run\start.ps1: там же пишется вывод сервера).
    /// </summary>
    public static ProcessPlan Server(LauncherSettings settings, string loaderLog)
    {
        var env = new Dictionary<string, string?>
        {
            ["BF3ACT_MODE"] = "dedicated",
            ["BF3ACT_DRM"] = "0",
            ["BF3ACT_LOG"] = Paths.LuaLog,
            ["BF3ACT_DNSREDIR"] = Ports.ServerSinkhole,
            // DNSREDIR_IP на сервере не задаём (как в run\start.ps1): убираем, если унаследовали от окружения
            ["BF3ACT_DNSREDIR_IP"] = null,
            // 16.10.2026: только флаги, которые код ещё читает (SPM_POINTS, SPM_SPAWN, AI_PLAYER, DUMP_INPUT удалены
            // в сборках 168–169; SPM_CUSTOM и PTS_REG включены по умолчанию).
            ["EVBOTS_SPM_PROBE"] = "0",
            ["EVBOTS_EBX_INDEX"] = Paths.EbxIndex,
            ["EVBOTS_LIVE_SEARCH"] = "1",
            ["EVBOTS_BRIDGE_ENGINE"] = "1",
            ["EVBOTS_BRIDGE"] = Flag(settings.UseBridge),
            ["EVBOTS_SPM_MGR"] = Flag(settings.UseManager),
            ["EVBOTS_SPM_SQUAD"] = "0",
            ["EVBOTS_SPM_TICK"] = Flag(settings.UseManager),
            ["EVBOTS_SPM_SOLDIER"] = Flag(settings.UseManager),
            ["EVBOTS_SPM_ONCREATE"] = "0",
            ["EVBOTS_SPM_CREATE"] = "0",
        };

        if (settings.EngineTests)
        {
            env["EVBOTS_PTS_REG"] = "1";
            env["EVBOTS_TESTS"] = "1";
        }

        return new ProcessPlan
        {
            FileName = Paths.Loader,
            WorkDir = Paths.KitRoot,
            Env = env,
            Args =
            [
                "--dedicated",
                "--game", Paths.ServerDir,
                "--exe", "bf3_server_Final.exe",
                "--dll", "EVBots.dll",
                "--log", loaderLog,
                "--wait",
                "--",
                "-serverInstancePath", Paths.InstanceDir,
                "-Server.ServerName", settings.ServerName,
                "-GamePort", Ports.Game.ToString(),
                "-Network.MaxClientCount", settings.MaxPlayers.ToString(),
                "-RemoteAdminPort", "127.0.0.1:" + Ports.Rcon,
                "+admin.password", RconPassword,
                "-PingSite", "ams",
                "-Region", "EU",
                "-Login", "bf3_1",
                "-Password", "password",
                "-Server.ActiveLicenses", "premium_xp4,xp2_ks,xp3_ks,xp4_ks,xp5_ks,ea_nfs12,le,premium,pwp",
            ],
        };
    }
}

/// <summary>Вход клиента. Аргументы и переменные — как в stand\run\join.ps1.</summary>
public static class ClientPlan
{
    /// <summary>Значение -requestStateParams (без внешних кавычек командной строки).</summary>
    public static string RequestStateParams(ulong gameId, ulong personaref, string serverAddr) =>
        $"<data putinsquad=\"false\" gameid=\"{gameId}\" personaref=\"{personaref}\" levelmode=\"mp\" serveraddr=\"{serverAddr}\"/>";

    /// <summary>
    /// hostIp — адрес хоста: 127.0.0.1 у себя, LAN-IP чужого сервера.
    /// Он же идёт в BF3ACT_DNSREDIR_IP: клиент перенаправляет gosredirector на хост.
    /// </summary>
    public static ProcessPlan Join(LauncherSettings settings, string hostIp, ulong gameId, ulong personaref, int gamePort)
    {
        var target = $"{hostIp}:{gamePort}";
        var args = new List<string>
        {
            "--client",
            "--game", Paths.ClientGameDir,
            "--drm-strict",
            "--license-artifact", Paths.LicenseArtifact,
            "--",
            "-webMode", "MP",
            "-Origin_NoAppFocus",
            "-onlineEnvironment", "prod",
            "-loginToken", "0",
            "-AuthToken", "0",
            "-requestState", "State_ClaimReservation",
            "-requestStateParams", RequestStateParams(gameId, personaref, target),
            "-Online.BlazeLogLevel", "2",
            "-Online.DirtysockLogLevel", "2",
        };

        if (settings.Windowed)
        {
            args.AddRange(["-windowed", "-w", "1280", "-h", "720"]);
        }

        return new ProcessPlan
        {
            FileName = Paths.Loader,
            WorkDir = Paths.KitRoot,
            Args = args,
            Env = new Dictionary<string, string?>
            {
                ["BF3ACT_DNSREDIR"] = Ports.Sinkhole,
                ["BF3ACT_DNSREDIR_IP"] = hostIp,
                ["BF3ACT_DRM"] = "0",
            },
        };
    }
}
