// ProfileStore.cs — профили игроков: аккаунты, солдаты, прогресс.
//
// Формат ВЗЯТ из существующего комплекта BF3NativeBots без изменений, чтобы
// новая и старая установки читали одни и те же данные:
//
//   profiles\<personaref>\profile.json     { schema, id, name, createdUtc }
//   profiles\<personaref>\progress.json    { schema, updatedUtc, stats{имя:значение} }
//   profiles\next-id.json                  { schema, nextId }
//
// Идентификатор personaref = и есть ключ: клиент присылает его в
// getStatsByGroupAsync(EID=…), и по нему мы отдаём статистику. Это ровно
// тот механизм, по которому клиент рисует ранг. Поэтому всё, что нужно для
// «у бота есть уровень», — создать для него запись прогресса.

using System.Globalization;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace EVBots.Backend.Data;

/// <summary>Запись прогресса игрока.</summary>
internal sealed class ProgressFile
{
    [JsonPropertyName("schema")] public int Schema { get; set; } = 1;
    [JsonPropertyName("updatedUtc")] public string UpdatedUtc { get; set; } = "";
    [JsonPropertyName("stats")] public Dictionary<string, double> Stats { get; set; } = new();
}

/// <summary>Профиль (аккаунт + солдат).</summary>
internal sealed class ProfileFile
{
    [JsonPropertyName("schema")] public int Schema { get; set; } = 1;
    [JsonPropertyName("id")] public ulong Id { get; set; }
    [JsonPropertyName("name")] public string Name { get; set; } = "";
    [JsonPropertyName("createdUtc")] public string CreatedUtc { get; set; } = "";
}

internal sealed class NextIdFile
{
    [JsonPropertyName("schema")] public int Schema { get; set; } = 1;
    [JsonPropertyName("nextId")] public ulong NextId { get; set; }
}

/// <summary>
/// Хранилище профилей. Один экземпляр на запуск; синхронизация через
/// общий лок, потому что к одному профилю может обращаться и HTTP-морда,
/// и Blaze-обработчик одновременно.
/// </summary>
internal sealed class ProfileStore(ServeConfig cfg, Log log)
{
    private static readonly JsonSerializerOptions Json = new()
    {
        WriteIndented = true,
        DefaultIgnoreCondition = JsonIgnoreCondition.Never,
    };

    private readonly Lock _gate = new();
    private readonly Dictionary<ulong, ProfileFile> _profiles = new();
    private ulong _nextId;

    public string Root => cfg.ProfilesDir;

    public void Load()
    {
        lock (_gate)
        {
            _profiles.Clear();

            var nextIdPath = Path.Combine(Root, "next-id.json");
            _nextId = 1;

            // Идентификаторы игр в реальном BF3 — крупные числа (напр. 234471774).
            // Стартуем с того же диапазона, чтобы не конфликтовать с EA-диапазоном.
            const ulong FirstId = 234471774;

            if (File.Exists(nextIdPath))
            {
                try
                {
                    var f = JsonSerializer.Deserialize<NextIdFile>(
                        File.ReadAllText(nextIdPath));
                    if (f is not null && f.NextId > 0)
                    {
                        _nextId = f.NextId;
                    }
                }
                catch (Exception ex)
                {
                    log.Warn($"next-id.json не прочитан ({ex.Message}), " +
                             $"начинаем с {FirstId}");
                    _nextId = FirstId;
                }
            }
            else
            {
                _nextId = FirstId;
                log.Info($"next-id.json не найден, новые personaref с {FirstId}");
            }

            var found = 0;
            foreach (var dir in Directory.EnumerateDirectories(Root))
            {
                var pj = Path.Combine(dir, "profile.json");
                if (!File.Exists(pj))
                {
                    continue;
                }

                try
                {
                    var p = JsonSerializer.Deserialize<ProfileFile>(File.ReadAllText(pj));
                    if (p is not null && p.Id > 0)
                    {
                        _profiles[p.Id] = p;
                        found++;
                        if (p.Id >= _nextId)
                        {
                            _nextId = p.Id + 1;
                        }
                    }
                }
                catch (Exception ex)
                {
                    log.Warn($"профиль {dir}: {ex.Message}");
                }
            }

            log.Info($"profiles loaded: {found}, next personaref = {_nextId}");
        }
    }

    public IReadOnlyList<ProfileFile> All
    {
        get
        {
            lock (_gate)
            {
                return _profiles.Values.OrderBy(p => p.Id).ToList();
            }
        }
    }

    public ProfileFile? Find(ulong id)
    {
        lock (_gate)
        {
            return _profiles.TryGetValue(id, out var p) ? p : null;
        }
    }

    public ProfileFile? FindByName(string name)
    {
        lock (_gate)
        {
            return _profiles.Values.FirstOrDefault(
                p => string.Equals(p.Name, name, StringComparison.OrdinalIgnoreCase));
        }
    }

    /// <summary>Создаёт солдата. Возвращает null, если имя уже занято.</summary>
    public ProfileFile? Create(string name)
    {
        if (string.IsNullOrWhiteSpace(name))
        {
            return null;
        }

        lock (_gate)
        {
            name = name.Trim();

            var clash = _profiles.Values.Any(
                p => string.Equals(p.Name, name, StringComparison.OrdinalIgnoreCase));
            if (clash)
            {
                return null;
            }

            var id = _nextId++;
            var profile = new ProfileFile
            {
                Id = id,
                Name = name,
                CreatedUtc = DateTime.UtcNow.ToString("o", CultureInfo.InvariantCulture),
            };

            var dir = Path.Combine(Root, id.ToString(CultureInfo.InvariantCulture));
            Directory.CreateDirectory(dir);
            SaveProfile(dir, profile);
            SaveNextId();

            _profiles[id] = profile;
            log.Ok($"создан солдат '{name}' personaref={id}");
            return profile;
        }
    }

    /// <summary>
    /// Переименовывает солдата. Возвращает null, если имя пустое,
    /// занято или солдата нет. Правит profile.json тем же SaveProfile,
    /// что и Create.
    /// </summary>
    public ProfileFile? Rename(ulong id, string name)
    {
        if (string.IsNullOrWhiteSpace(name))
        {
            return null;
        }

        lock (_gate)
        {
            if (!_profiles.TryGetValue(id, out var profile))
            {
                return null;
            }

            name = name.Trim();
            if (!string.Equals(profile.Name, name, StringComparison.Ordinal))
            {
                var clash = _profiles.Values.Any(p =>
                    p.Id != id &&
                    string.Equals(p.Name, name, StringComparison.OrdinalIgnoreCase));
                if (clash)
                {
                    return null;
                }
            }

            var old = profile.Name;
            profile.Name = name;
            var dir = Path.Combine(Root, id.ToString(CultureInfo.InvariantCulture));
            SaveProfile(dir, profile);

            log.Ok($"солдат personaref={id} переименован '{old}' -> '{name}'");
            return profile;
        }
    }

    public bool Delete(ulong id)
    {
        lock (_gate)
        {
            if (!_profiles.Remove(id))
            {
                return false;
            }

            var dir = Path.Combine(Root, id.ToString(CultureInfo.InvariantCulture));
            try
            {
                if (Directory.Exists(dir))
                {
                    Directory.Delete(dir, recursive: true);
                }
            }
            catch (Exception ex)
            {
                log.Warn($"не удалось удалить каталог {dir}: {ex.Message}");
            }

            log.Ok($"солдат personaref={id} удалён");
            return true;
        }
    }

    /// <summary>Возвращает прогресс игрока, создавая пустой при первом обращении.</summary>
    public ProgressFile GetProgress(ulong personaref)
    {
        lock (_gate)
        {
            var path = Path.Combine(
                Root, personaref.ToString(CultureInfo.InvariantCulture), "progress.json");

            if (File.Exists(path))
            {
                try
                {
                    var pf = JsonSerializer.Deserialize<ProgressFile>(
                        File.ReadAllText(path));
                    if (pf is not null)
                    {
                        return pf;
                    }
                }
                catch (Exception ex)
                {
                    log.Warn($"progress.json personaref={personaref}: {ex.Message}");
                }
            }

            return new ProgressFile();
        }
    }

    public void SaveProgress(ulong personaref, ProgressFile progress)
    {
        lock (_gate)
        {
            progress.Schema = 1;
            progress.UpdatedUtc = DateTime.UtcNow.ToString("o",
                CultureInfo.InvariantCulture);

            var dir = Path.Combine(
                Root, personaref.ToString(CultureInfo.InvariantCulture));
            Directory.CreateDirectory(dir);
            // Сбой записи не должен оставлять обрезанный единственный файл прогресса.
            var path = Path.Combine(dir, "progress.json");
            var pending = path + ".tmp";
            File.WriteAllText(pending, JsonSerializer.Serialize(progress, Json),
                new System.Text.UTF8Encoding(false));
            File.Move(pending, path, overwrite: true);
        }
    }

    /// <summary>Чтение, изменение и запись под одним локом: HTTP и отчёт матча не затирают друг друга.</summary>
    public ProgressFile UpdateProgress(ulong personaref, Action<ProgressFile> update)
    {
        lock (_gate)
        {
            var progress = GetProgress(personaref);
            update(progress);
            SaveProgress(personaref, progress);
            return progress;
        }
    }

    /// <summary>
    /// Копирует стартовый набор статистики существующего игрока.
    /// Нужно, чтобы у бота был ранг: без записи прогресса клиент ранга не рисует.
    /// </summary>
    public bool CloneProgress(ulong from, ulong to)
    {
        lock (_gate)
        {
            var src = GetProgress(from);
            var dst = GetProgress(to);
            dst.Stats = new Dictionary<string, double>(src.Stats);
            SaveProgress(to, dst);
            log.Info($"прогресс скопирован: {from} -> {to} ({src.Stats.Count} стат)");
            return true;
        }
    }

    /// <summary>Создаёт бота: солдат + стартовый прогресс.</summary>
    public ProfileFile? CreateBot(string name, ulong cloneFrom)
    {
        var p = Create(name);
        if (p is null)
        {
            return null;
        }

        if (cloneFrom > 0 && Find(cloneFrom) is not null)
        {
            CloneProgress(cloneFrom, p.Id);
        }

        return p;
    }

    public ulong ReserveId()
    {
        lock (_gate)
        {
            return _nextId++;
        }
    }

    public void SaveNextId()
    {
        File.WriteAllText(Path.Combine(Root, "next-id.json"),
            JsonSerializer.Serialize(new NextIdFile { NextId = _nextId }, Json));
    }

    private static void SaveProfile(string dir, ProfileFile profile) =>
        File.WriteAllText(Path.Combine(dir, "profile.json"),
            JsonSerializer.Serialize(profile, Json));
}
