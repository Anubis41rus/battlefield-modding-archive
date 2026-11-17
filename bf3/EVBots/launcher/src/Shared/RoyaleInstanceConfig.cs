using System;
using System.IO;
using System.Linq;
using System.Text;

namespace EVBots.Installation;

internal static class RoyaleInstanceConfig
{
    internal const string Map = "XP5_003";
    internal const string Mode = "ConquestLarge0";
    internal static readonly string[] Mods = { "VEManager", "VU-BattleRoyale-Compass", "VU-BattleRoyale" };
    private const string Runtime = @"runtime\royale\server-parity";
    private static readonly string[] Files = { "ServerOptions.cfg", @"Admin\Startup.txt", @"Admin\MapList.txt", @"Admin\ModList.txt" };
    private static readonly UTF8Encoding Utf8 = new UTF8Encoding(false);

    internal static string ServerDirectory(string kit) => Path.Combine(kit, Runtime);
    internal static string InstanceDirectory(string kit) => Path.Combine(ServerDirectory(kit), "Instance");
    internal static string[] InstallationPaths() => Files.Select(file => Path.Combine(Runtime, "Instance", file)).ToArray();

    internal static void Write(string instance, string name = "EVBots")
    {
        instance = Path.GetFullPath(instance);
        ValidatePaths(instance);
        name = (name ?? "").Trim();
        if (name.Length == 0) name = "EVBots";
        if (name.Any(char.IsControl)) throw new InvalidDataException("Название сервера должно быть одной строкой.");

        Directory.CreateDirectory(Path.Combine(instance, "Admin"));
        WriteFile(instance, Files[0], "# EVBots Battle Royale. Provider options are set on the command line.");
        WriteFile(instance, Files[1], "# EVBots Battle Royale", "vars.serverName " + name,
            "vars.maxPlayers 64", "vars.idleTimeout 0", "vars.autoBalance false", "vars.unlockMode all",
            "vars.gameModeCounter 100", "vars.ctfRoundTimeModifier 100");
        WriteFile(instance, Files[2], Map + " " + Mode + " 1");
        WriteFile(instance, Files[3], Mods);
    }

    internal static void ValidatePaths(string instance)
    {
        instance = Path.GetFullPath(instance);
        CheckPath(instance);
        foreach (var file in Files) CheckPath(Path.Combine(instance, file));
    }

    private static void WriteFile(string instance, string relative, params string[] lines)
    {
        var path = Path.Combine(instance, relative);
        var content = string.Join("\r\n", lines) + "\r\n";
        if (File.Exists(path) && File.ReadAllBytes(path).SequenceEqual(Utf8.GetBytes(content))) return;
        var temp = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            File.WriteAllText(temp, content, Utf8);
            if (File.Exists(path)) File.Replace(temp, path, null);
            else File.Move(temp, path);
        }
        finally { if (File.Exists(temp)) File.Delete(temp); }
    }

    private static void CheckPath(string path)
    {
        for (var current = path; current != null; current = Path.GetDirectoryName(current))
        {
            try
            {
                if ((File.GetAttributes(current) & FileAttributes.ReparsePoint) != 0)
                    throw new IOException("Путь конфигурации Royale проходит через ссылку: " + current);
            }
            catch (FileNotFoundException) { }
            catch (DirectoryNotFoundException) { }
        }
    }
}
