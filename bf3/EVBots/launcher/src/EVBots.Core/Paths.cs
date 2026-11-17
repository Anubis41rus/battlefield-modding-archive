namespace EVBots.Core;

/// <summary>
/// Пути стенда. Совпадают со скриптами стенда stand\run\ (common.ps1, start.ps1, join.ps1; сверено 16.10.2026).
/// Менять только вместе с ними.
/// </summary>
public static class Paths
{
    // --- комплект BF3NativeBots (EVBots-dev\stand) ---
    public const string KitRoot = @"C:\Games\EVBots-dev\stand";
    /// <summary>23.10.2026: свой загрузчик EVBots (EVBots-dev\loader): несколько DLL, клиенту activation.dll первой.</summary>
    public const string LoaderName = "evbots-loader.exe";
    /// <summary>Загрузчик старого комплекта (одна DLL на процесс) — только если своего в папке нет.</summary>
    public const string LegacyLoaderName = "activation-loader.exe";
    public const string Loader = KitRoot + @"\activation\" + LoaderName;
    public const string EVBotsExtDeployed = KitRoot + @"\activation\EVBots.dll";
    public const string EVBotsExtBuild = @"C:\Games\EVBots-dev\evbots-dll\build\EVBots.dll";
    public const string EbxIndex = KitRoot + @"\activation\ebx_index.bin";
    public const string LicenseArtifact = KitRoot + @"\license\71067.dlf";
    public const string BackendExe = KitRoot + @"\backend\EV Backend.exe";
    public const string ProfilesDir = KitRoot + @"\profiles";
    public const string ProgressDir = KitRoot + @"\progress";
    public const string StatsCatalog = KitRoot + @"\progress\bf3-stats-catalog.json";
    public const string LuaLog = KitRoot + @"\logs\EVBots.log";

    // --- игра ---
    public const string ClientGameDir = @"C:\Games\Battlefield 3";
    public const string ServerDir = @"C:\Games\BF-Portable\Battlefield_3_Server";
    public const string ServerExe = ServerDir + @"\bf3_server_Final.exe";
    public const string InstanceDir = ServerDir + @"\Instance";
    public const string StartupTxt = InstanceDir + @"\Admin\Startup.txt";
    public const string MapListTxt = InstanceDir + @"\Admin\MapList.txt";
    public const string FunBotsConfig = InstanceDir + @"\Admin\Mods\fun-bots\ext\Shared\Config.lua";
    public const string RuntimeLogsDir = InstanceDir + @"\Logs";

    /// <summary>Папка лаунчера (рядом с exe): настройки и логи.</summary>
    public static string AppDir => AppContext.BaseDirectory;

    public static string LogsDir => Path.Combine(AppDir, "logs");

    /// <summary>
    /// Загрузчик в папке activation: свой evbots-loader.exe, а в старой установке без него — activation-loader.exe.
    /// null — нет ни того, ни другого.
    /// </summary>
    public static string? FindLoader(string activationDir)
    {
        foreach (var name in new[] { LoaderName, LegacyLoaderName })
        {
            var p = Path.Combine(activationDir, name);
            if (File.Exists(p)) return p;
        }
        return null;
    }
}
