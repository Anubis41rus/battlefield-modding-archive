// InstanceCatalog.cs — инстанс сервера в структуре VU.
//
// Раскладка как у Venice Unleashed (hosting\config.md, hosting\mods.md,
// modding\structure.md):
//
//   <instance>\Admin\Startup.txt        vars.serverName "…", vars.gamePassword "…"
//   <instance>\Admin\MapList.txt        MapID GamemodeID Rounds
//   <instance>\Admin\ModList.txt        список модов, по имени папки
//   <instance>\Admin\Mods\<мод>\mod.json
//                        \ext\{client,server,shared}\__init__.lua
//                        \ui.vuic
//
// Зачем своя структура: чтобы работали существующие серверные конфиги и
// VU-моды без конверсии, и чтобы modList.* работали так же, как у VU.

using System.Globalization;
using System.Text;

namespace EVBots.Backend.Data;

/// <summary>Строка ротации карт.</summary>
internal sealed record MapListEntry(string MapId, string GamemodeId, int Rounds)
{
    public override string ToString() =>
        $"{MapId} {GamemodeId} {Rounds.ToString(CultureInfo.InvariantCulture)}";
}

/// <summary>
/// Чтение и запись Startup.txt / MapList.txt / ModList.txt плюс команды
/// modList.* из hosting\commands.md.
/// </summary>
internal sealed class InstanceCatalog(string instanceDir, string modsDir, Log log)
{
    public string InstanceDir { get; } = instanceDir;

    public string AdminDir => Path.Combine(InstanceDir, "Admin");

    public string StartupPath => Path.Combine(AdminDir, "Startup.txt");

    public string MapListPath => Path.Combine(AdminDir, "MapList.txt");

    public string ModListPath => Path.Combine(AdminDir, "ModList.txt");

    public string ModsDir { get; } = modsDir;

    /// <summary>Отладочный режим модов (аналог modList.Debug).</summary>
    public bool DebugMode { get; private set; }

    public void EnsureLayout()
    {
        Directory.CreateDirectory(AdminDir);
        Directory.CreateDirectory(ModsDir);

        if (!File.Exists(MapListPath))
        {
            WriteMapList(new List<MapListEntry>
            {
                new("MP_001", "ConquestSmall0", 1),
            });
            log.Info($"создан MapList.txt с картой по умолчанию: {MapListPath}");
        }

        if (!File.Exists(StartupPath))
        {
            WriteStartup(new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
            {
                ["vars.serverName"] = "EVBots LAN Server",
                ["vars.maxPlayers"] = "64",
            });
            log.Info($"создан Startup.txt: {StartupPath}");
        }

        if (!File.Exists(ModListPath))
        {
            WriteModList(new List<string>());
            log.Info($"создан ModList.txt: {ModListPath}");
        }
    }

    // ── Startup.txt ─────────────────────────────────────────────────────────

    /// <summary>
    /// Читает параметры из Startup.txt. Поддерживает форму
    /// <c>vars.serverName "значение"</c> и <c>vars.maxPlayers 64</c>.
    /// Кавычки снимаются, строки-комментарии (#) пропускаются.
    /// </summary>
    public Dictionary<string, string> ReadStartup()
    {
        var result = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        if (!File.Exists(StartupPath))
        {
            return result;
        }

        foreach (var line in File.ReadAllLines(StartupPath, Encoding.UTF8))
        {
            var s = line.Trim();
            if (s.Length == 0 || s.StartsWith('#'))
            {
                continue;
            }

            var sp = s.IndexOf(' ');
            if (sp <= 0)
            {
                result[s] = "";
                continue;
            }

            var key = s[..sp];
            var val = s[(sp + 1)..].Trim();
            if (val.Length >= 2 && val[0] == '"' && val[^1] == '"')
            {
                val = val[1..^1];
            }

            result[key] = val;
        }

        return result;
    }

    public string? StartupValue(string key) =>
        ReadStartup().TryGetValue(key, out var v) ? v : null;

    public void WriteStartup(Dictionary<string, string> values)
    {
        Directory.CreateDirectory(AdminDir);

        var sb = new StringBuilder();
        sb.AppendLine("# Startup.txt — параметры выделенного сервера EVBots.");
        sb.AppendLine("# Формат как у Venice Unleashed: <переменная> <значение>,");
        sb.AppendLine("# строковые значения в кавычках. Строки с # — комментарии.");
        sb.AppendLine();

        foreach (var (k, v) in values)
        {
            var needsQuote = v.Length > 0 &&
                             (v.Contains(' ') || v.Contains('"'));
            sb.AppendLine(needsQuote ? $"{k} \"{v}\"" : $"{k} {v}");
        }

        File.WriteAllText(StartupPath, sb.ToString(), new UTF8Encoding(false));
    }

    public void SetStartupValue(string key, string value)
    {
        var values = ReadStartup();
        values[key] = value;
        WriteStartup(values);
        log.Info($"startup: {key} = {value}");
    }

    public string ServerNameFromInstance() =>
        StartupValue("vars.serverName") ?? "";

    // ── MapList.txt ─────────────────────────────────────────────────────────

    public List<MapListEntry> ReadMapList()
    {
        var result = new List<MapListEntry>();
        if (!File.Exists(MapListPath))
        {
            return result;
        }

        foreach (var line in File.ReadAllLines(MapListPath, Encoding.UTF8))
        {
            var s = line.Trim();
            if (s.Length == 0 || s.StartsWith('#'))
            {
                continue;
            }

            var parts = s.Split(' ', StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length < 2)
            {
                continue;
            }

            var rounds = parts.Length >= 3 &&
                         int.TryParse(parts[2], NumberStyles.Integer,
                             CultureInfo.InvariantCulture, out var r)
                ? r
                : 1;
            result.Add(new MapListEntry(parts[0], parts[1], rounds));
        }

        return result;
    }

    public void WriteMapList(List<MapListEntry> entries)
    {
        Directory.CreateDirectory(AdminDir);

        var sb = new StringBuilder();
        sb.AppendLine("# MapList.txt — ротация карт: MapID GamemodeID Rounds");
        sb.AppendLine();

        foreach (var e in entries)
        {
            sb.AppendLine(e.ToString());
        }

        File.WriteAllText(MapListPath, sb.ToString(), new UTF8Encoding(false));
    }

    // ── ModList.txt и команды modList.* ─────────────────────────────────────

    public List<string> ReadModList()
    {
        if (!File.Exists(ModListPath))
        {
            return new List<string>();
        }

        return File.ReadAllLines(ModListPath, Encoding.UTF8)
            .Select(l => l.Trim())
            .Where(l => l.Length > 0 && !l.StartsWith('#'))
            .ToList();
    }

    public void WriteModList(List<string> mods)
    {
        Directory.CreateDirectory(AdminDir);

        var sb = new StringBuilder();
        sb.AppendLine("# ModList.txt — моды, которые сервер загрузит при старте.");
        sb.AppendLine("# По одному имени папки в Admin\\Mods на строку.");
        sb.AppendLine("# Изменения вступают в силу после перезапуска сервера.");
        sb.AppendLine();

        foreach (var m in mods)
        {
            sb.AppendLine(m);
        }

        File.WriteAllText(ModListPath, sb.ToString(), new UTF8Encoding(false));
    }

    /// <summary>Все моды, которые можно добавить: папки в Admin\Mods.</summary>
    public List<string> AvailableMods() =>
        Directory.Exists(ModsDir)
            ? Directory.EnumerateDirectories(ModsDir)
                .Select(Path.GetFileName)
                .Where(n => !string.IsNullOrEmpty(n))
                .Select(n => n!)
                .Order(StringComparer.OrdinalIgnoreCase)
                .ToList()
            : new List<string>();

    /// <summary>
    /// Выполняет команду modList.* из hosting\commands.md.
    /// Возвращает строки ответа (как показывает RCON).
    /// </summary>
    public List<string> ExecuteModListCommand(string cmd, string? arg,
        IReadOnlyCollection<string> running)
    {
        var mod = arg ?? "";
        var list = ReadModList();

        switch (cmd)
        {
            case "modList.List":
                return list.Count == 0
                    ? new List<string> { "ModList.txt пуст" }
                    : new List<string> { "Будет загружено при старте:" }
                        .Concat(list.Select(m => "  " + m)).ToList();

            case "modList.ListRunning":
                return running.Count == 0
                    ? new List<string> { "Сейчас не загружено ни одного мода" }
                    : new List<string> { "Загружено сейчас:" }
                        .Concat(running.Order().Select(m => "  " + m)).ToList();

            case "modList.Available":
            {
                var avail = AvailableMods();
                return avail.Count == 0
                    ? new List<string>
                    {
                        $"В {ModsDir} нет папок модов",
                        "Скопируйте мод (например fun-bots) в эту папку",
                    }
                    : new List<string> { "Доступно в Admin\\Mods:" }
                        .Concat(avail.Select(m => "  " + m +
                            (list.Contains(m, StringComparer.OrdinalIgnoreCase)
                                ? "  [уже в списке]"
                                : ""))).ToList();
            }

            case "modList.Add":
            {
                if (string.IsNullOrWhiteSpace(mod))
                {
                    return new List<string> { "modList.Add: нужно имя мода" };
                }

                if (list.Contains(mod, StringComparer.OrdinalIgnoreCase))
                {
                    return new List<string> { $"Мод '{mod}' уже в списке" };
                }

                if (!AvailableMods().Contains(mod, StringComparer.OrdinalIgnoreCase))
                {
                    return new List<string>
                    {
                        $"Мод '{mod}' не найден в Admin\\Mods",
                        "Список доступных: modList.Available",
                    };
                }

                list.Add(mod);
                WriteModList(list);
                return new List<string>
                {
                    $"Мод '{mod}' добавлен в ModList.txt",
                    "Вступит в силу после перезапуска сервера",
                };
            }

            case "modList.Remove":
            {
                var hit = list.FirstOrDefault(
                    x => x.Equals(mod, StringComparison.OrdinalIgnoreCase));
                if (hit is null)
                {
                    return new List<string> { $"Мод '{mod}' не найден в ModList.txt" };
                }

                list.Remove(hit);
                WriteModList(list);
                return new List<string>
                {
                    $"Мод '{hit}' убран из ModList.txt",
                    "Вступит в силу после перезапуска сервера",
                };
            }

            case "modList.Clear":
                WriteModList(new List<string>());
                return new List<string>
                {
                    "ModList.txt очищен",
                    "Уже загруженные моды продолжат работать до перезапуска",
                };

            case "modList.Debug":
                DebugMode = !DebugMode;
                return new List<string>
                {
                    $"modList.Debug = {(DebugMode ? "true" : "false")}",
                    DebugMode
                        ? "Скрипты модов будут собраны с отладочными символами"
                        : "Обычный режим сборки",
                };

            case "modList.ReloadExtensions":
                return new List<string>
                {
                    "ReloadExtensions: перечитывание модов на живом сервере",
                    "пока не реализовано — перезапустите выделенный сервер",
                };

            default:
                return new List<string>
                {
                    $"Неизвестная команда '{cmd}'",
                    "Доступны: List, ListRunning, Available, Add, Remove, Clear, Debug",
                };
        }
    }

    /// <summary>Проверка mod.json мода: возвращает (ok, сообщение).</summary>
    public (bool Ok, string Message) ValidateMod(string modName)
    {
        var dir = Path.Combine(ModsDir, modName);
        if (!Directory.Exists(dir))
        {
            return (false, $"мод '{modName}' не найден");
        }

        var modJson = Path.Combine(dir, "mod.json");
        if (!File.Exists(modJson))
        {
            return (false, $"у мода '{modName}' нет mod.json — VeniceEXT его не загрузит");
        }

        var ext = Path.Combine(dir, "ext");
        if (!Directory.Exists(ext))
        {
            return (false, $"у мода '{modName}' нет папки ext");
        }

        var inits = Directory
            .EnumerateFiles(ext, "__init__.lua", SearchOption.AllDirectories)
            .Select(f => Path.GetRelativePath(ext, f).Replace('\\', '/'))
            .Order()
            .ToList();

        if (inits.Count == 0)
        {
            return (false,
                $"у мода '{modName}' нет ни одного __init__.lua " +
                $"(ext/client, ext/server, ext/shared)");
        }

        return (true, $"mod.json и скрипты на месте: {string.Join(", ", inits)}");
    }
}