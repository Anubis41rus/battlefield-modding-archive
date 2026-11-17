using System.IO;
using System.Net.Http;
using System.Numerics;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using EVBots.Installation;

namespace EVBots.Launcher;

internal sealed record ModProfile(string Id, string Label, string Root, bool IsRoyale);
internal sealed record ModDescriptor(string Folder, string Version, bool HasExt, bool HasWebUi,
    Dictionary<string, string> Dependencies);
internal sealed record ActiveModSet(ModProfile Profile, string[] Roots, IReadOnlyList<ModDescriptor> Mods, string Directory = "")
{
    public string EnvironmentValue => string.Join(';', Roots);
}

/// <summary>Только чтение комплекта. Наличие файлов не означает совместимость мода с движком.</summary>
internal static class ModProfiles
{
    public const string RoyaleMap = RoyaleInstanceConfig.Map;
    public const string RoyaleMode = RoyaleInstanceConfig.Mode;
    public const string VeniceExtVersion = "1.12.0"; // declareVeniceExt в EVBots.dll
    public const string RoyaleNotice = "Тест оригинального VU Battle Royale: работа полного матча в EVBots ещё не подтверждена. " +
        "Нужны одинаковые версии мода, VEManager и VU-BattleRoyale-Compass у всех игроков.";
    public static readonly ModProfile FunBots = new("fun-bots", "fun-bots · боты", "fun-bots", false);
    public static readonly ModProfile Royale = new("vu-battleroyale", "Battle Royale · LAN", "VU-BattleRoyale", true);
    public static readonly ModProfile[] All = { Royale };
    public static readonly string[] RoyaleFolders = RoyaleInstanceConfig.Mods;
    public static ModProfile Get(string id) => All.FirstOrDefault(p => p.Id == id) ?? Royale;

    public static bool ValidFolder(string name)
    {
        if (string.IsNullOrWhiteSpace(name) || name.Length > 255 || name != name.Trim() || name is "." or ".." ||
            name.EndsWith('.') || name.Any(c => c < 32 || "\\/:*?\"<>|;".Contains(c)) || name.Equals("@none", StringComparison.OrdinalIgnoreCase)) return false;
        var stem = name.Split('.')[0];
        return !Regex.IsMatch(stem, @"^(CON|PRN|AUX|NUL|CONIN\$|CONOUT\$|COM[1-9¹²³]|LPT[1-9¹²³])$", RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);
    }

    private static ModDescriptor Parse(string folder, JsonElement value)
    {
        if (!ValidFolder(folder) || value.ValueKind != JsonValueKind.Object) throw new InvalidDataException($"Неверный mod.json: {folder}.");
        var fields = new HashSet<string>(StringComparer.Ordinal);
        foreach (var field in value.EnumerateObject())
            if (!fields.Add(field.Name)) throw new InvalidDataException($"{folder}: повторное поле {field.Name}.");
        if (!value.TryGetProperty("Version", out var v) || v.ValueKind != JsonValueKind.String || !SemVersion.TryParse(v.GetString()!, out _))
            throw new InvalidDataException($"{folder}: Version должен быть полной SemVer-версией.");
        bool Flag(string name)
        {
            if (!value.TryGetProperty(name, out var flag)) return false;
            if (flag.ValueKind is not (JsonValueKind.True or JsonValueKind.False)) throw new InvalidDataException($"{folder}: неверный {name}.");
            return flag.GetBoolean();
        }
        var deps = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        if (value.TryGetProperty("Dependencies", out var dependencies))
        {
            if (dependencies.ValueKind != JsonValueKind.Object) throw new InvalidDataException($"{folder}: неверный Dependencies.");
            foreach (var dep in dependencies.EnumerateObject())
            {
                if (!ValidFolder(dep.Name) || dep.Value.ValueKind != JsonValueKind.String ||
                    !SemVersion.ValidRequirement(dep.Value.GetString()!) || !deps.TryAdd(dep.Name, dep.Value.GetString()!))
                    throw new InvalidDataException($"{folder}: неверная зависимость {dep.Name}.");
            }
        }
        return new ModDescriptor(folder, v.GetString()!, Flag("HasVeniceEXT"), Flag("HasWebUI"), deps);
    }

    public static ModProfile ProfileForRoots(IEnumerable<string> roots)
    {
        var names = roots.ToHashSet(StringComparer.OrdinalIgnoreCase);
        if (names.Contains(Royale.Root) && names.IsSubsetOf(RoyaleFolders)) return Royale;
        throw new InvalidDataException("Этот лаунчер подключается только к локальному Battle Royale. Запустите на хосте сервер Battle Royale.");
    }

    private static ActiveModSet Resolve(string[] roots, Func<string, ModDescriptor> read)
    {
        var profile = ProfileForRoots(roots);
        var loaded = new Dictionary<string, ModDescriptor>(StringComparer.OrdinalIgnoreCase);
        var visiting = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var ordered = new List<ModDescriptor>();
        ModDescriptor Visit(string folder)
        {
            if (!ValidFolder(folder)) throw new InvalidDataException($"Неверное имя папки мода: {folder}.");
            if (visiting.Contains(folder)) throw new InvalidDataException($"Цикл зависимостей модов: {folder}.");
            if (loaded.TryGetValue(folder, out var done)) return done;
            if (loaded.Count + visiting.Count >= 64) throw new InvalidDataException("Слишком много зависимостей модов.");
            visiting.Add(folder);
            var mod = read(folder);
            foreach (var dep in mod.Dependencies)
            {
                var version = dep.Key.Equals("veniceext", StringComparison.OrdinalIgnoreCase) ? VeniceExtVersion : Visit(dep.Key).Version;
                if (!SemVersion.Matches(version, dep.Value))
                    throw new InvalidDataException($"{folder} требует {dep.Key} {dep.Value}; найдена версия {version}.");
            }
            visiting.Remove(folder);
            loaded.Add(folder, mod);
            ordered.Add(mod);
            return mod;
        }
        foreach (var root in roots) Visit(root);
        if (profile.IsRoyale && !loaded.Keys.ToHashSet(StringComparer.OrdinalIgnoreCase).SetEquals(RoyaleFolders))
            throw new InvalidDataException("Оригинальный профиль требует VU-BattleRoyale, VEManager и VU-BattleRoyale-Compass с объявленными зависимостями.");
        if (profile.IsRoyale && (ordered.Any(m => !m.HasExt) ||
            ordered.Any(m => !m.Folder.Equals("VEManager", StringComparison.OrdinalIgnoreCase) && !m.HasWebUi)))
            throw new InvalidDataException("Оригинальный профиль требует HasVeniceEXT у всех трёх модов и HasWebUI у Battle Royale и Compass.");
        return new ActiveModSet(profile, roots.Distinct(StringComparer.OrdinalIgnoreCase).ToArray(), ordered);
    }

    public static ActiveModSet CheckLocal(string modsRoot, ModProfile profile, bool server, string[]? roots = null)
    {
        if (string.IsNullOrEmpty(modsRoot) || !System.IO.Directory.Exists(modsRoot))
            throw new FileNotFoundException($"Нет папки модов: {modsRoot}. Автоматической загрузки с хоста пока нет.");
        var set = Resolve(roots ?? new[] { profile.Root }, folder =>
        {
            var file = Path.Combine(modsRoot, folder, "mod.json");
            if (!File.Exists(file)) throw new FileNotFoundException($"Нет {folder}\\mod.json в {modsRoot}. Нужен тот же комплект модов, что у хоста; автораздачи пока нет.");
            using var doc = JsonDocument.Parse(File.ReadAllText(file, new UTF8Encoding(false, true)));
            return Parse(folder, doc.RootElement);
        });
        if (set.Profile.Id != profile.Id) throw new InvalidDataException("Набор модов не соответствует выбранному профилю.");
        foreach (var mod in set.Mods)
        {
            var dir = Path.Combine(modsRoot, mod.Folder);
            if (mod.HasExt && !new[] { "shared", "client", "server" }.Any(side => NonEmpty(Path.Combine(dir, "ext", side, "__init__.lua"))))
                throw new FileNotFoundException($"{mod.Folder}: нет Lua-инициализаторов в ext.");
            if (mod.HasWebUi || mod.Folder.Equals(Royale.Root, StringComparison.OrdinalIgnoreCase)) CheckUiArchive(Path.Combine(dir, "ui.vuic"));
        }
        if (profile.IsRoyale)
        {
            foreach (var folder in RoyaleFolders)
            {
                var side = server ? "server" : "client";
                if (!NonEmpty(Path.Combine(modsRoot, folder, "ext", side, "__init__.lua")))
                    throw new FileNotFoundException($"{folder}: нет ext\\{side}\\__init__.lua.");
            }
            if (!NonEmpty(Path.Combine(modsRoot, Royale.Root, "ext", "shared", "__init__.lua")))
                throw new FileNotFoundException("VU-BattleRoyale: нет ext\\shared\\__init__.lua.");
        }
        else if (server && !NonEmpty(Path.Combine(modsRoot, FunBots.Root, "mod.db")))
            throw new FileNotFoundException("fun-bots: нет mod.db с путями ботов.");
        return set with { Directory = modsRoot };
    }

    private static bool NonEmpty(string path) => File.Exists(path) && new FileInfo(path).Length > 0;

    // Та же структура заголовка/индекса, что читает evbots-dll/src/Vuic.cpp.
    private static void CheckUiArchive(string file)
    {
        if (!NonEmpty(file)) throw new FileNotFoundException($"Нет собранного UI: {file}. Исходники ui/ его не заменяют.");
        using var stream = File.OpenRead(file);
        using var reader = new BinaryReader(stream);
        if (stream.Length < 12 || stream.Length > 256L * 1024 * 1024 || Encoding.ASCII.GetString(reader.ReadBytes(4)) != "CIUV" || reader.ReadUInt32() != 2)
            throw new InvalidDataException($"Неподдерживаемый ui.vuic (нужен CIUV v2): {file}.");
        var count = reader.ReadUInt32();
        if (count == 0 || count > 100000 || 12UL + count * 20UL > (ulong)stream.Length) throw new InvalidDataException($"Повреждён индекс ui.vuic: {file}.");
        for (var i = 0U; i < count; ++i)
        {
            reader.ReadUInt32(); var stored = reader.ReadUInt32(); reader.ReadUInt32(); reader.ReadUInt32(); var offset = reader.ReadUInt32();
            if ((ulong)offset + stored > (ulong)stream.Length) throw new InvalidDataException($"Повреждён ui.vuic: {file}.");
        }
    }

    public static async Task<ActiveModSet> ReadHostAsync(HttpClient http, string baseUrl)
    {
        using var list = JsonDocument.Parse(await http.GetStringAsync(baseUrl + "/mods"));
        if (!list.RootElement.TryGetProperty("modList", out var configured) || configured.ValueKind != JsonValueKind.Array)
            throw new InvalidDataException("Хост не сообщает активный набор модов. Обновите EV Launcher у хоста.");
        var roots = configured.EnumerateArray().Select(v => v.ValueKind == JsonValueKind.String ? v.GetString()! : "").ToArray();
        ProfileForRoots(roots); // Пустой старый ModList не является доказательством активного fun-bots.
        if (roots.Any(n => !ValidFolder(n))) throw new InvalidDataException("Хост передал неверный список модов.");
        var descriptors = new Dictionary<string, ModDescriptor>(StringComparer.OrdinalIgnoreCase);
        var queue = new Queue<string>(roots);
        while (queue.TryDequeue(out var folder))
        {
            if (descriptors.ContainsKey(folder)) continue;
            if (!ValidFolder(folder) || descriptors.Count >= 64) throw new InvalidDataException("Неверные зависимости модов хоста.");
            using var response = JsonDocument.Parse(await http.GetStringAsync(baseUrl + "/mods/" + Uri.EscapeDataString(folder)));
            if (!response.RootElement.TryGetProperty("modJson", out var json)) throw new InvalidDataException($"Хост не отдал mod.json для {folder}.");
            var mod = Parse(folder, json);
            if ((mod.HasWebUi || folder.Equals(Royale.Root, StringComparison.OrdinalIgnoreCase)) &&
                (!response.RootElement.TryGetProperty("hasWebUi", out var ui) || ui.ValueKind != JsonValueKind.True))
                throw new InvalidDataException($"У хоста не собран ui.vuic мода {folder}.");
            descriptors.Add(folder, mod);
            foreach (var dep in mod.Dependencies.Keys.Where(n => !n.Equals("veniceext", StringComparison.OrdinalIgnoreCase))) queue.Enqueue(dep);
        }
        return Resolve(roots, name => descriptors[name]);
    }

    public static void MatchHost(ActiveModSet local, ActiveModSet host)
    {
        var mine = local.Mods.ToDictionary(m => m.Folder, StringComparer.OrdinalIgnoreCase);
        if (!mine.Keys.ToHashSet(StringComparer.OrdinalIgnoreCase).SetEquals(host.Mods.Select(m => m.Folder)))
            throw new InvalidDataException("Локальные зависимости отличаются от набора хоста. Нужен одинаковый комплект модов.");
        foreach (var mod in host.Mods)
        {
            if (!mine.TryGetValue(mod.Folder, out var found) || found.Version != mod.Version ||
                found.HasExt != mod.HasExt || found.HasWebUi != mod.HasWebUi || found.Dependencies.Count != mod.Dependencies.Count ||
                mod.Dependencies.Any(d => !found.Dependencies.TryGetValue(d.Key, out var requirement) || requirement != d.Value))
                throw new InvalidDataException($"{mod.Folder}: хост использует {mod.Version}, локальная версия {found?.Version ?? "отсутствует"} или её зависимости отличаются. Нужен комплект хоста; автораздачи пока нет.");
        }
    }

    private sealed record SemVersion(BigInteger Major, BigInteger Minor, BigInteger Patch, string[] Pre) : IComparable<SemVersion>
    {
        private static readonly Regex Pattern = new(@"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$", RegexOptions.CultureInvariant);
        public static bool TryParse(string value, out SemVersion? result)
        {
            result = null;
            var m = Pattern.Match(value);
            if (!m.Success || m.Length != value.Length) return false;
            var pre = m.Groups[4].Success ? m.Groups[4].Value.Split('.') : Array.Empty<string>();
            if (pre.Any(p => p.All(char.IsAsciiDigit) && p.Length > 1 && p[0] == '0')) return false;
            result = new(BigInteger.Parse(m.Groups[1].Value), BigInteger.Parse(m.Groups[2].Value), BigInteger.Parse(m.Groups[3].Value), pre);
            return true;
        }
        public int CompareTo(SemVersion? b)
        {
            if (b is null) return 1;
            var c = Major.CompareTo(b.Major); if (c != 0) return c;
            c = Minor.CompareTo(b.Minor); if (c != 0) return c;
            c = Patch.CompareTo(b.Patch); if (c != 0) return c;
            if (Pre.Length == 0 || b.Pre.Length == 0) return Pre.Length == b.Pre.Length ? 0 : (Pre.Length == 0 ? 1 : -1);
            for (var i = 0; i < Math.Min(Pre.Length, b.Pre.Length); ++i)
            {
                var an = Pre[i].All(char.IsAsciiDigit); var bn = b.Pre[i].All(char.IsAsciiDigit);
                if (an != bn) return an ? -1 : 1;
                c = an ? BigInteger.Parse(Pre[i]).CompareTo(BigInteger.Parse(b.Pre[i])) : string.CompareOrdinal(Pre[i], b.Pre[i]);
                if (c != 0) return c;
            }
            return Pre.Length.CompareTo(b.Pre.Length);
        }
        private static (string Op, string Version) Split(string requirement)
        {
            var r = requirement.Trim();
            var op = r.StartsWith(">=") ? ">=" : r.Length > 0 && "^~>".Contains(r[0]) ? r[..1] : "";
            return (op, r[op.Length..].Trim());
        }
        public static bool ValidRequirement(string requirement) => TryParse(Split(requirement).Version, out _);
        public static bool Matches(string value, string requirement)
        {
            var (op, text) = Split(requirement);
            if (!TryParse(value, out var v) || !TryParse(text, out var b)) return false;
            var c = v!.CompareTo(b);
            return op switch { "^" => v.Major == b!.Major && c >= 0, "~" => v.Major == b!.Major && v.Minor == b.Minor && c >= 0,
                ">" => c > 0, ">=" => c >= 0, "" => c == 0, _ => false };
        }
    }
}
