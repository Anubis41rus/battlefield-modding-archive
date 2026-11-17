// StatsCatalog.cs — каталог прогресса: определения (лестница рангов).
//
// Источник: BF3NativeBots\progress\bf3-stats-catalog.json — 271 КБ,
// group = "player_mpdefault2", 1432 стата {name, mode} плюс готовый
// getStatGroupPayloadBase64 (135 690 байт) — реальный TDF-payload ответа.
//
// Зачем: клиент BF3 по getStatGroup("player_mpdefault2") получает ОПРЕДЕЛЕНИЯ
// прогресса (лестницу рангов, что за что открывается). Payload уже готов,
// поэтому мы просто отдаём его байт в байт, а не собираем заново.
//
// getStatsByGroupAsync(EID) отдаёт ЗНАЧЕНИЯ конкретного игрока — это уже
// ProfileStore.

using System.Globalization;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace EVBots.Backend.Data;

internal sealed class StatEntry
{
    [JsonPropertyName("name")] public string Name { get; set; } = "";
    [JsonPropertyName("mode")] public string Mode { get; set; } = "";
}

internal sealed class CatalogFile
{
    [JsonPropertyName("schema")] public int Schema { get; set; }
    [JsonPropertyName("group")] public string Group { get; set; } = "";
    [JsonPropertyName("stats")] public List<StatEntry> Stats { get; set; } = new();
    [JsonPropertyName("getStatGroupPayloadBase64")]
    public string PayloadBase64 { get; set; } = "";
}

internal sealed class StatsCatalog(string path, Log log)
{
    public string Group { get; private set; } = "player_mpdefault2";
    public byte[] Payload { get; private set; } = [];
    public int StatCount { get; private set; }
    public IReadOnlyList<StatEntry> Stats { get; private set; } = [];
    public bool Ok { get; private set; }

    public int PayloadLength => Payload.Length;

    public string Path => path;

    public void Load()
    {
        if (!File.Exists(path))
        {
            log.Warn($"каталог статов не найден: {path}");
            log.Warn("ранги у игроков не появятся, пока каталог не подложен");
            return;
        }

        try
        {
            using var doc = JsonDocument.Parse(
                File.ReadAllText(path),
                new JsonDocumentOptions
                {
                    AllowTrailingCommas = true,
                    CommentHandling = JsonCommentHandling.Skip,
                });

            var root = doc.RootElement;
            Group = root.TryGetProperty("group", out var g)
                ? g.GetString() ?? Group
                : Group;

            var stats = new List<StatEntry>();
            if (root.TryGetProperty("stats", out var st) &&
                st.ValueKind == JsonValueKind.Array)
            {
                foreach (var e in st.EnumerateArray())
                {
                    var name = e.TryGetProperty("name", out var n)
                        ? n.GetString() ?? ""
                        : "";
                    var mode = e.TryGetProperty("mode", out var m)
                        ? m.GetString() ?? ""
                        : "";
                    if (name.Length > 0)
                    {
                        stats.Add(new StatEntry { Name = name, Mode = mode });
                    }
                }
            }

            Stats = stats;
            StatCount = stats.Count;

            if (root.TryGetProperty("getStatGroupPayloadBase64", out var pb) &&
                pb.GetString() is { Length: > 0 } b64)
            {
                Payload = Convert.FromBase64String(b64);
            }

            Ok = StatCount > 0;
            if (!Ok)
            {
                log.Warn("каталог прочитан, но список стат пуст");
            }
        }
        catch (Exception ex)
        {
            log.Error($"каталог статов не разобран: {ex.Message}");
            Ok = false;
        }
    }

    /// <summary>
    /// Значения прогресса игрока в виде TDF-списка — то, что уходит в
    /// ответ на getStatsByGroupAsync.
    ///
    /// Структура (Blaze Stats, "stats" лист map): имя стата -> значение.
    /// Клиент читает те же имена, что перечислены в getStatGroup, поэтому
    /// значения фильтруем по каталогу.
    /// </summary>
    public Dictionary<string, double> Filter(ProfileStore store, ulong personaref)
    {
        var known = new HashSet<string>(StringComparer.Ordinal);
        foreach (var s in Stats)
        {
            known.Add(s.Name);
        }

        var progress = store.GetProgress(personaref);
        var result = new Dictionary<string, double>(StringComparer.Ordinal);

        foreach (var (k, v) in progress.Stats)
        {
            // если каталог пуст — отдаём всё, иначе только известные имена
            if (known.Count == 0 || known.Contains(k))
            {
                result[k] = v;
            }
        }

        return result;
    }

    /// <summary>
    /// Значения ВСЕХ статов каталога строго в порядке каталога — ровно то,
    /// что уходит в notify 0x32 (StatsAsyncNotification) на
    /// getStatsByGroupAsync. Порядок обязан совпадать с getStatGroup:
    /// клиент читает значения по позициям дескрипторов.
    ///
    /// Нет сохранённого значения — "0.00" (как zeros у рабочего бэкенда).
    /// Формат: invariant "F2" (как StatsService.FormatValue рабочего).
    /// </summary>
    public List<object> FormatValuesOrdered(ProfileStore store, ulong personaref)
    {
        var progress = store.GetProgress(personaref).Stats;
        var result = new List<object>(Stats.Count);

        foreach (var s in Stats)
        {
            var v = progress.TryGetValue(s.Name, out var raw) ? raw : 0.0;
            if (v == 0.0)
            {
                v = 0.0; // нормализация -0.0
            }

            result.Add(v.ToString("F2", CultureInfo.InvariantCulture));
        }

        return result;
    }
}