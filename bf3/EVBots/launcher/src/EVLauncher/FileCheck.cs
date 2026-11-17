// Проверка файлов: что найдено, что не нашлось и что надо сделать. Ничего на диске не меняет.
// Каждая строка лога: [OK] найдено; [НЕТ] не нашлось, с подсказкой, что положить; [инфо] — сведения.
using System.IO;
using System.Security.Cryptography;
using System.Text.Json;
using EVBots.Core;

namespace EVBots.Launcher;

/// <summary>Строка отчёта: Mark = «OK», «НЕТ» или «инфо».</summary>
internal sealed record CheckLine(string Mark, string Text);

internal static class FileCheck
{
    /// <summary>Файлы активации в папке EVBots\activation.</summary>
    private static readonly string[] ActivationFiles =
    {
        @"activation\evbots-loader.exe", @"activation\activation.dll",
        @"activation\EVBots.dll", @"activation\ebx_index.bin",
        @"activation\ev-core.dll", @"activation\lua.dll",  // 23.10: ядро (аналог vu-core) и Lua (как vubin\lua.dll у VU)
    };

    /// <summary>Файлы бэкенда в папке EVBots\backend.</summary>
    private static readonly string[] BackendFiles =
    {
        @"backend\EV Backend.exe", @"backend\EV Backend.dll", @"backend\BouncyCastle.Crypto.dll",
        @"backend\EV Backend.deps.json", @"backend\EV Backend.runtimeconfig.json",
    };

    /// <summary>Полный отчёт по всем пунктам. Вызывать в фоне: здесь чтение файлов и netsh.</summary>
    public static List<CheckLine> Build(FoundPaths p, ModProfile profile, bool server)
    {
        var res = new List<CheckLine>();

        // Игра и сервер (ищутся рядом с лаунчером)
        var bf3 = Sub(p.ClientGameDir, "bf3.exe");
        res.Add(Need(Has(bf3),
            $"Battlefield 3 (игра) найдена: {bf3}",
            "Battlefield 3 (игра) НЕ найдена: нет bf3.exe. Положи EVBots-Setup.exe рядом с папками «Battlefield 3» и «BF-Portable»."));

        var srv = Sub(p.ServerDir, "bf3_server_Final.exe");
        res.Add(Need(Has(srv),
            $"Сервер Battlefield 3 найден (BF-Portable): {srv}",
            "Сервер НЕ найден: нет BF-Portable\\Battlefield_3_Server\\bf3_server_Final.exe. Без него хостить нельзя, входить к другу можно."));

        var candidates = new List<string>();
        if (p.InstanceDir.Length > 0) candidates.Add(Path.Combine(p.InstanceDir, "Admin", "Mods"));
        if (!server && p.StandDir.Length > 0) candidates.Add(Path.Combine(p.StandDir, "Mods"));
        var modErrors = new List<string>();
        ActiveModSet? active = null;
        foreach (var dir in candidates.Distinct(StringComparer.OrdinalIgnoreCase))
        {
            try { active = ModProfiles.CheckLocal(dir, profile, server); break; }
            catch (Exception ex) when (ex is IOException or InvalidDataException or UnauthorizedAccessException or JsonException or ArgumentException)
            { modErrors.Add(ex.Message); }
        }
        res.Add(active is null
            ? new CheckLine("НЕТ", profile.Label + ": " + string.Join(" ", modErrors) + " Нужен полный комплект модов; автораздачи пока нет.")
            : new CheckLine("OK", "Файлы и зависимости: " + string.Join(", ", active.Mods.Select(m => m.Folder + " " + m.Version)) + " — " + active.Directory));
        if (profile.IsRoyale) res.Add(new CheckLine("инфо", ModProfiles.RoyaleNotice));

        var opts = Sub(p.InstanceDir, "ServerOptions.cfg");
        res.Add(Need(Has(opts),
            $"Настройки сервера найдены: {opts}",
            "НЕ найден Instance\\ServerOptions.cfg в BF-Portable: без него сервер не поднимется. Проверь папку BF-Portable."));

        // Папка EVBots: активация, бэкенд, лицензия, статистика
        var stand = p.StandDir;
        if (!HasDir(stand))
        {
            res.Add(new CheckLine("НЕТ",
                "Папка EVBots НЕ найдена (ищу её рядом с «Battlefield 3»). Нажми «Указать папку EVBots» и выбери её."));
        }
        else
        {
            res.Add(new CheckLine("OK", $"Папка EVBots найдена: {stand}"));
            Group(res, "Активация (папка activation)", stand, ActivationFiles, "переустанови EVBots-Setup.exe");
            Group(res, "Бэкенд (папка backend)", stand, BackendFiles, "переустанови EVBots-Setup.exe");
            res.Add(Need(Has(Sub(stand, @"license\71067.dlf")),
                "Лицензия найдена: license\\71067.dlf",
                "НЕ нашлось: license\\71067.dlf. Переустанови EVBots-Setup.exe."));
            res.Add(Need(Has(Sub(stand, @"progress\bf3-stats-catalog.json")),
                "Каталог статистики найден: progress\\bf3-stats-catalog.json",
                "НЕ нашлось: progress\\bf3-stats-catalog.json. Переустанови EVBots-Setup.exe."));
            Versions(res, stand);
        }

        // .NET и фаервол
        res.Add(Need(HasDotNet10(),
            "Среда .NET 10 найдена (нужна бэкенду)",
            "НЕ найден .NET 10: бэкенд не запустится. Установи «.NET 10 Runtime (x64)» с dotnet.microsoft.com и повтори проверку."));

        var fw = MainWindow.FirewallRuleExists("EVBots-TCP") && MainWindow.FirewallRuleExists("EVBots-UDP");
        res.Add(Need(fw,
            "Фаервол: правила EVBots-TCP и EVBots-UDP открыты",
            "Фаервол: правил нет. Нажми «Фаервол (UAC)» на каждом ПК, и хосту, и гостю. Без него друзья не увидят сервер."));

        // HTTP бэкенда для гостей: без URL-ACL на LAN-адрес бэкенд слушает только 127.0.0.1
        var lanIp = LanInfo.DefaultIp();
        if (lanIp is null)
            res.Add(new CheckLine("НЕТ", "LAN-адрес не найден: подключи ПК к сети (VPN для игры в LAN не подходит)."));
        else
            res.Add(Need(MainWindow.UrlAclExists(lanIp),
                $"HTTP для гостей: URL-ACL на http://{lanIp}:8080/ есть",
                $"HTTP для гостей: нет URL-ACL на http://{lanIp}:8080/ — гости не смогут войти. Нажми «Фаервол (UAC)» и перезапусти сервер."));
        res.Add(Need(MainWindow.FirewallRuleExists("EVBots client"),
            "Разрешение bf3.exe в брандмауэре есть",
            "Нет разрешения bf3.exe в брандмауэре: игра может не получить данные от сервера. Нажми «Фаервол (UAC)»."));

        // 19.10: база мода с путями ботов. Без неё мод пишет «No paths found» и ботов нет (у друга мод из git — без mod.db).
        if (!profile.IsRoyale && server && p.InstanceDir.Length > 0 && Has(Path.Combine(p.InstanceDir, "Admin", "Mods", "fun-bots", "mod.json")))
            res.Add(Need(Has(Path.Combine(p.InstanceDir, "Admin", "Mods", "fun-bots", "mod.db")),
                "Мод fun-bots: база путей mod.db есть",
                "Мод fun-bots: нет mod.db (пути ботов по картам) — ботов не будет. mod.db должен стоять в BF-Portable вместе с модом."));

        // 18.10: язык игры — реестр EA Games\Battlefield 3 «Locale» и русские файлы Data\Win32\Loc\ru.sb
        if (p.ClientGameDir.Length > 0)
        {
            string locale = "";
            try
            {
                using var k = Microsoft.Win32.RegistryKey.OpenBaseKey(Microsoft.Win32.RegistryHive.LocalMachine, Microsoft.Win32.RegistryView.Registry32)
                    .OpenSubKey(@"SOFTWARE\EA Games\Battlefield 3");
                locale = k?.GetValue("Locale") as string ?? "";
            }
            catch (Exception) { }
            var ru = Has(Sub(p.ClientGameDir, @"Data\Win32\Loc\ru.sb"));
            res.Add(Need(locale == "ru_RU" && ru,
                "Язык игры: русский (Locale = ru_RU, файлы ru.sb есть)",
                $"Язык игры не русский (Locale = «{locale}», ru.sb {(ru ? "есть" : "нет")}). Включи русский язык игры вручную (Locale = ru_RU, файлы ru.sb в Data\\Win32\\Loc)."));
        }
        return res;
    }

    /// <summary>Группа файлов: если всё на месте — одна строка OK; иначе по строке на каждый недостающий файл.</summary>
    private static void Group(List<CheckLine> res, string title, string stand, string[] files, string hint)
    {
        var missing = files.Where(f => !File.Exists(Path.Combine(stand, f))).ToList();
        if (missing.Count == 0)
        {
            var names = string.Join(", ", files.Select(f => Path.GetFileName(f)));
            res.Add(new CheckLine("OK", $"{title}: все файлы на месте ({names})"));
            return;
        }
        foreach (var f in missing)
            res.Add(new CheckLine("НЕТ", $"НЕ нашлось: {f} — {title}. Что сделать: {hint}."));
    }

    /// <summary>20.10: какая сборка стоит — md5 DLL и бэкенда (первые 8 знаков), чтобы сверить с тем, что прислали. Сведения.</summary>
    private static void Versions(List<CheckLine> res, string stand)
    {
        foreach (var rel in new[] { @"activation\EVBots.dll", @"backend\EV Backend.dll" })
        {
            var mine = Path.Combine(stand, rel);
            if (!File.Exists(mine)) continue;
            res.Add(new CheckLine("инфо", $"{rel}: md5 {Md5(mine)[..8].ToLowerInvariant()}, файл от {File.GetLastWriteTime(mine):dd.MM HH:mm}"));
        }
    }

    /// <summary>« (версия X)», если версия мода известна; иначе пусто.</summary>
    private static string ModNote(string modJson)
    {
        var v = ModVersion(modJson);
        return v == "?" ? "" : " (версия " + v + ")";
    }

    /// <summary>Версия мода из mod.json (если поле есть), иначе «?».</summary>
    private static string ModVersion(string modJson)
    {
        try
        {
            using var d = JsonDocument.Parse(File.ReadAllText(modJson));
            return d.RootElement.TryGetProperty("version", out var v) ? v.ToString() : "?";
        }
        catch (Exception) { return "?"; }
    }

    /// <summary>Есть ли .NET 10 (Microsoft.NETCore.App 10.x) в стандартной папке dotnet.</summary>
    private static bool HasDotNet10()
    {
        try
        {
            var dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "dotnet", "shared", "Microsoft.NETCore.App");
            return Directory.Exists(dir)
                && Directory.GetDirectories(dir).Any(d => Path.GetFileName(d).StartsWith("10.", StringComparison.Ordinal));
        }
        catch (Exception) { return false; }
    }

    private static CheckLine Need(bool ok, string okText, string missText) =>
        new(ok ? "OK" : "НЕТ", ok ? okText : missText);

    private static string Sub(string dir, string rel) => dir.Length == 0 ? "" : Path.Combine(dir, rel);
    private static bool Has(string path) => path.Length > 0 && File.Exists(path);
    private static bool HasDir(string path) => path.Length > 0 && Directory.Exists(path);

    private static string Md5(string path)
    {
        using var fs = File.OpenRead(path);
        return Convert.ToHexString(MD5.HashData(fs));
    }
}
