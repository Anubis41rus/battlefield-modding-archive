// AccountsStore.cs — аккаунты LAN: логин + пароль -> профиль (personaref).
//
// Зачем: как в VU — у каждого свой аккаунт, и зайти можно с любого ПК в LAN.
// Регистрация/вход идут через HTTP (WebHost: /accounts/*), join.bat вызывает
// POST /accounts/ensure перед запуском игры и подставляет personaref в
// requestStateParams — гость играет ПОД СВОИМ профилем со всей статистикой.
//
// Файл: <profiles>\accounts.json. Пароли не хранятся в открытом виде:
// PBKDF2-SHA256, 100 000 итераций, соль 16 байт (RFC 2898). Токены входа —
// в памяти процесса, 24 часа; перезапуск бэкенда их сбрасывает, join.bat
// просто входит заново.
//
// Аккаунт привязан к профилю двумя способами:
//   - регистрация нового: имя профиля = логин (или переиспользуется
//     существующий профиль с тем же именем — так KOTuk сохраняет стату);
//   - Blaze login (cmd 40) с неизвестным логином: первичная регистрация
//     на уже созданный профиль (выделенный сервер bf3_1 и т.п.).

using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace EVBots.Backend.Data;

/// <summary>Солдат аккаунта (для «моих солдат» в лаунчере).</summary>
internal sealed class SoldierInfo
{
    public ulong Personaref { get; init; }
    public string Name { get; init; } = "";

    /// <summary>Профиль на диске ещё существует (иначе солдат удалён).</summary>
    public bool Exists { get; init; }

    public bool Selected { get; init; }
}

/// <summary>Публичная карточка аккаунта (без секретов).</summary>
internal sealed class AccountInfo
{
    public string Login { get; init; } = "";
    public ulong Personaref { get; init; }
    public string Name { get; init; } = "";
    public string CreatedUtc { get; init; } = "";

    /// <summary>Все солдаты аккаунта; Personaref — выбранный.</summary>
    public IReadOnlyList<SoldierInfo> Soldiers { get; init; } = [];
}

internal sealed class AccountsStore
{
    private static readonly JsonSerializerOptions Json = new()
    {
        WriteIndented = true,
        DefaultIgnoreCondition = JsonIgnoreCondition.Never,
    };

    private const int Pbkdf2Iterations = 100_000;
    private const int MinPasswordLength = 4;
    private const int MaxLoginLength = 32;
    private const int TokenLifetimeHours = 24;

    private readonly string _path;
    private readonly ProfileStore _profiles;
    private readonly Log _log;
    private readonly Lock _gate = new();

    private readonly Dictionary<string, AccountRecord> _byLogin =
        new(StringComparer.OrdinalIgnoreCase);

    private readonly Dictionary<string, (string Login, DateTime ExpiresUtc)> _tokens =
        new();

    public AccountsStore(string path, ProfileStore profiles, Log log)
    {
        _path = path;
        _profiles = profiles;
        _log = log;
    }

    private sealed class AccountRecord
    {
        [JsonPropertyName("login")] public string Login { get; set; } = "";
        [JsonPropertyName("salt")] public string Salt { get; set; } = "";
        [JsonPropertyName("hash")] public string Hash { get; set; } = "";

        /// <summary>
        /// Солдаты аккаунта (как в VU: у аккаунта их несколько, выбираешь
        /// одного). Старые файлы держали одно поле personaref — при
        /// загрузке оно переносится в Soldiers + Selected.
        /// </summary>
        [JsonPropertyName("soldiers")] public List<ulong> Soldiers { get; set; } = [];

        /// <summary>Выбранный солдат (за него заходим в игру).</summary>
        [JsonPropertyName("selected")] public ulong Selected { get; set; }

        [JsonPropertyName("personaref")]
        public ulong Personaref
        {
            get => Selected;
            set => Selected = value;
        }

        [JsonPropertyName("createdUtc")] public string CreatedUtc { get; set; } = "";

        /// <summary>Привести запись к новой схеме (старые файлы).</summary>
        public void Migrate()
        {
            Soldiers ??= [];
            if (Soldiers.Count == 0 && Selected != 0)
            {
                Soldiers.Add(Selected);
            }

            if (Selected == 0 && Soldiers.Count > 0)
            {
                Selected = Soldiers[0];
            }
        }
    }

    private sealed class AccountsFile
    {
        [JsonPropertyName("schema")] public int Schema { get; set; } = 1;
        [JsonPropertyName("accounts")] public List<AccountRecord> Accounts { get; set; } = new();
    }

    // ── загрузка/сохранение ────────────────────────────────────────────────

    public void Load()
    {
        lock (_gate)
        {
            _byLogin.Clear();

            if (!File.Exists(_path))
            {
                _log.Info($"аккаунтов: 0 (файл {Path.GetFileName(_path)} появится " +
                          "при первой регистрации)");
                return;
            }

            try
            {
                var f = JsonSerializer.Deserialize<AccountsFile>(
                    File.ReadAllText(_path));
                if (f is not null)
                {
                    foreach (var a in f.Accounts)
                    {
                        if (!string.IsNullOrEmpty(a.Login))
                        {
                            a.Migrate();
                            _byLogin[a.Login] = a;
                        }
                    }
                }

                _log.Info($"accounts loaded: {_byLogin.Count}");
            }
            catch (Exception ex)
            {
                _log.Warn($"accounts.json не прочитан ({ex.Message}) — " +
                          "начинаем с пустого списка");
            }
        }
    }

    private void Save()
    {
        // вызывается под _gate
        var f = new AccountsFile
        {
            Accounts = _byLogin.Values
                .OrderBy(a => a.Login, StringComparer.OrdinalIgnoreCase)
                .ToList(),
        };

        var tmp = _path + ".tmp";
        File.WriteAllText(tmp, JsonSerializer.Serialize(f, Json),
            new UTF8Encoding(false));
        File.Move(tmp, _path, overwrite: true);
    }

    // ── валидация ──────────────────────────────────────────────────────────

    private static bool ValidLogin(string login)
    {
        if (login.Length < 3 || login.Length > MaxLoginLength)
        {
            return false;
        }

        foreach (var c in login)
        {
            if (!char.IsAsciiLetterOrDigit(c) && c is not ('_' or '-' or '.'))
            {
                return false;
            }
        }

        return true;
    }

    private static bool ValidPassword(string password) =>
        password.Length >= MinPasswordLength;

    private static bool CheckPassword(AccountRecord rec, string password)
    {
        try
        {
            var salt = Convert.FromBase64String(rec.Salt);
            var expected = Convert.FromBase64String(rec.Hash);
            var actual = Rfc2898DeriveBytes.Pbkdf2(
                password, salt, Pbkdf2Iterations, HashAlgorithmName.SHA256,
                expected.Length);
            return CryptographicOperations.FixedTimeEquals(expected, actual);
        }
        catch
        {
            return false;
        }
    }

    private static AccountRecord MakeRecord(string login, string password,
        ulong personaref)
    {
        var salt = RandomNumberGenerator.GetBytes(16);
        var hash = Rfc2898DeriveBytes.Pbkdf2(
            password, salt, Pbkdf2Iterations, HashAlgorithmName.SHA256, 32);

        return new AccountRecord
        {
            Login = login,
            Salt = Convert.ToBase64String(salt),
            Hash = Convert.ToBase64String(hash),
            Personaref = personaref,
            CreatedUtc = DateTime.UtcNow.ToString("o"),
        };
    }

    private AccountInfo InfoOf(AccountRecord rec)
    {
        var profile = _profiles.Find(rec.Selected);
        return new AccountInfo
        {
            Login = rec.Login,
            Personaref = rec.Selected,
            Name = profile?.Name ?? rec.Login,
            CreatedUtc = rec.CreatedUtc,
            Soldiers = rec.Soldiers
                .Select(id => new SoldierInfo
                {
                    Personaref = id,
                    Name = _profiles.Find(id)?.Name ?? "?",
                    Exists = _profiles.Find(id) is not null,
                    Selected = id == rec.Selected,
                })
                .ToList(),
        };
    }

    // ── операции ───────────────────────────────────────────────────────────

    public bool Exists(string login)
    {
        lock (_gate)
        {
            return !string.IsNullOrEmpty(login) && _byLogin.ContainsKey(login);
        }
    }

    public IReadOnlyList<AccountInfo> All
    {
        get
        {
            lock (_gate)
            {
                return _byLogin.Values.Select(InfoOf).ToList();
            }
        }
    }

    /// <summary>
    /// Вход или регистрация в одном шаге (join.bat): аккаунта нет —
    /// создаём (профиль с именем-логином или существующий с тем же именем);
    /// есть — проверяем пароль и чиним привязку, если профиль удаляли.
    /// displayName — ник из join.bat: он становится ИМЕНЕМ СОЛДАТА
    /// в игре для нового аккаунта; для существующего ник не меняется.
    /// </summary>
    public bool Ensure(string login, string password, string? displayName,
        out AccountInfo info, out string error)
    {
        info = new AccountInfo();
        error = "";

        login = login.Trim();

        if (!ValidLogin(login))
        {
            error = $"логин: 3..{MaxLoginLength} символов, только буквы/цифры" +
                    " _ - .";
            return false;
        }

        if (!ValidPassword(password))
        {
            error = $"пароль минимум {MinPasswordLength} символов";
            return false;
        }

        lock (_gate)
        {
            if (_byLogin.TryGetValue(login, out var rec))
            {
                if (!CheckPassword(rec, password))
                {
                    error = "неверный пароль";
                    return false;
                }

                // профиль могли удалить — восстанавливаем привязку
                if (_profiles.Find(rec.Personaref) is null)
                {
                    var rebuilt = _profiles.Create(login);
                    if (rebuilt is null)
                    {
                        error = "профиль аккаунта удалён, а имя занято";
                        return false;
                    }

                    rec.Personaref = rebuilt.Id;
                    Save();
                }

                info = InfoOf(rec);
                error = "";
                return true;
            }

            // новая регистрация: сначала ищем профиль с именем-логином
            // (старая стата сохраняется), иначе создаём новый: имя =
            // ник из join.bat, а если оно занято — сам логин
            var profile = _profiles.FindByName(login);
            if (profile is null)
            {
                var want = string.IsNullOrWhiteSpace(displayName)
                    ? login
                    : displayName.Trim();
                profile = _profiles.Create(want) ?? _profiles.Create(login);
            }

            if (profile is null)
            {
                error = "имя занято другим профилем";
                return false;
            }

            rec = MakeRecord(login, password, profile.Id);
            _byLogin[login] = rec;
            Save();

            _log.Info($"аккаунт зарегистрирован: {login} -> " +
                      $"personaref {profile.Id}");
            info = InfoOf(rec);
            error = "";
            return true;
        }
    }

    /// <summary>Проверка пароля существующего аккаунта (Blaze login).</summary>
    public bool Verify(string login, string password,
        out AccountInfo info, out string error)
    {
        info = new AccountInfo();
        error = "";

        lock (_gate)
        {
            if (string.IsNullOrEmpty(login) ||
                !_byLogin.TryGetValue(login.Trim(), out var rec))
            {
                error = "аккаунт не найден";
                return false;
            }

            if (!CheckPassword(rec, password))
            {
                error = "неверный пароль";
                return false;
            }

            info = InfoOf(rec);
            error = "";
            return true;
        }
    }

    /// <summary>
    /// Первичная привязка: Blaze login с неизвестным логином заводит аккаунт
    /// на УЖЕ созданный профиль (выделенный сервер bf3_1). Если логин уже
    /// есть — пароль обязан совпасть.
    /// </summary>
    public bool EnsureForProfile(string login, string password,
        ulong personaref, out string error)
    {
        error = "";

        login = login.Trim();
        if (!ValidLogin(login))
        {
            error = $"логин недопустим: '{login}'";
            return false;
        }

        lock (_gate)
        {
            if (_byLogin.TryGetValue(login, out var rec))
            {
                if (!CheckPassword(rec, password))
                {
                    error = "неверный пароль";
                    return false;
                }

                return true;
            }

            if (!ValidPassword(password))
            {
                // без пароля аккаунт не заводим — остаётся старое поведение
                // (профиль по имени), но защищённого аккаунта не будет
                error = "пароль слишком короткий — аккаунт не создан";
                return false;
            }

            _byLogin[login] = MakeRecord(login, password, personaref);
            Save();
            _log.Info($"аккаунт привязан через Blaze login: {login} -> " +
                      $"personaref {personaref}");
            return true;
        }
    }

    /// <summary>
    /// Удалить аккаунт (логин) и его солдата вместе с прогрессом —
    /// «ничего не должно остаться». Логин можно будет завести заново, но
    /// статистика и ник пропадут безвозвратно.
    /// Возвращает удалённого аккаунта либо null, если логина нет.
    /// </summary>
    public AccountInfo? Remove(string login, bool withSoldier)
    {
        login = (login ?? "").Trim();
        if (login.Length == 0)
        {
            return null;
        }

        lock (_gate)
        {
            if (!_byLogin.TryGetValue(login, out var rec))
            {
                return null;
            }

            _byLogin.Remove(login);

            // токены этого логина больше недействительны
            foreach (var t in _tokens
                         .Where(t => t.Value.Login == login)
                         .Select(t => t.Key)
                         .ToList())
            {
                _tokens.Remove(t);
            }

            var soldierDeleted = false;
            if (withSoldier)
            {
                soldierDeleted = _profiles.Delete(rec.Personaref);
            }

            Save();
            _log.Info($"аккаунт удалён: {login} (personaref " +
                      $"{rec.Personaref}, солдат " +
                      $"{(soldierDeleted ? "удалён" : "не найден")})");
            return InfoOf(rec);
        }
    }

    /// <summary>
    /// Солдаты аккаунта (все personaref). Как в VU: у аккаунта несколько
    /// солдат, заходишь за выбранного.
    /// </summary>
    public IReadOnlyList<ulong> SoldiersOf(string login)
    {
        lock (_gate)
        {
            return _byLogin.TryGetValue((login ?? "").Trim(), out var rec)
                ? rec.Soldiers.ToList()
                : [];
        }
    }

    /// <summary>
    /// Создать ещё одного солдата для аккаунта (или переключиться на
    /// существующего, если он уже есть у этого логина). cloneFrom — донор
    /// статистики, иначе клиент не рисует ранг в табе.
    /// </summary>
    public (ulong Personaref, bool Created, string Error) AddSoldier(
        string login, string name, ulong cloneFrom)
    {
        name = (name ?? "").Trim();
        lock (_gate)
        {
            if (!_byLogin.TryGetValue((login ?? "").Trim(), out var rec))
            {
                return (0, false, "аккаунт не найден");
            }

            if (name.Length == 0)
            {
                return (0, false, "имя солдата пустое");
            }

            // уже есть солдат с таким именем — просто выбираем его
            var sameName = _profiles.FindByName(name);
            if (sameName is not null)
            {
                if (!rec.Soldiers.Contains(sameName.Id))
                {
                    return (0, false,
                        $"солдат «{name}» уже есть, но он не привязан " +
                        "к этому аккаунту");
                }

                rec.Selected = sameName.Id;
                Save();
                return (sameName.Id, false, "");
            }

            var profile = _profiles.CreateBot(name, cloneFrom) ??
                _profiles.Create(name);
            if (profile is null)
            {
                return (0, false, "имя занято другим профилем");
            }

            rec.Soldiers.Add(profile.Id);
            rec.Selected = profile.Id;
            Save();

            _log.Info($"аккаунт {login}: добавлен солдат «{profile.Name}» " +
                      $"personaref={profile.Id}" +
                      (cloneFrom > 0 ? $" (стата скопирована с {cloneFrom})" : ""));
            return (profile.Id, true, "");
        }
    }

    /// <summary>Выбрать солдата аккаунта (за него пойдём в игру).</summary>
    public string SelectSoldier(string login, ulong personaref)
    {
        lock (_gate)
        {
            if (!_byLogin.TryGetValue((login ?? "").Trim(), out var rec))
            {
                return "аккаунт не найден";
            }

            if (!rec.Soldiers.Contains(personaref))
            {
                return "этот солдат не привязан к аккаунту";
            }

            if (_profiles.Find(personaref) is null)
            {
                return "солдат удалён — создайте нового";
            }

            rec.Selected = personaref;
            Save();
            return "";
        }
    }

    /// <summary>
    /// Убрать солдата из аккаунта (не удаляя его профиль) — «мои солдаты»
    /// в лаунчере: можно отвязать, чтобы выбор был чистым.
    /// </summary>
    public string DetachSoldier(string login, ulong personaref)
    {
        lock (_gate)
        {
            if (!_byLogin.TryGetValue((login ?? "").Trim(), out var rec))
            {
                return "аккаунт не найден";
            }

            if (!rec.Soldiers.Remove(personaref))
            {
                return "солдат не привязан к этому аккаунту";
            }

            if (rec.Selected == personaref)
            {
                rec.Selected = rec.Soldiers.Count > 0 ? rec.Soldiers[0] : 0;
            }

            Save();
            return "";
        }
    }

    /// <summary>Найти аккаунт по логину (без пароля).</summary>
    public AccountInfo? Find(string login)
    {
        lock (_gate)
        {
            return _byLogin.TryGetValue((login ?? "").Trim(), out var rec)
                ? InfoOf(rec)
                : null;
        }
    }

    public AccountInfo? FindByPersonaref(ulong personaref)
    {
        lock (_gate)
        {
            var rec = _byLogin.Values.FirstOrDefault(
                a => a.Personaref == personaref);
            return rec is null ? null : InfoOf(rec);
        }
    }

    // ── токены (память процесса, 24 ч) ─────────────────────────────────────

    public string IssueToken(AccountInfo account)
    {
        lock (_gate)
        {
            var now = DateTime.UtcNow;

            foreach (var dead in _tokens
                         .Where(t => t.Value.ExpiresUtc < now)
                         .Select(t => t.Key)
                         .ToList())
            {
                _tokens.Remove(dead);
            }

            var token = Convert.ToHexString(
                RandomNumberGenerator.GetBytes(16));
            _tokens[token] = (account.Login,
                now.AddHours(TokenLifetimeHours));
            return token;
        }
    }

    public AccountInfo? ResolveToken(string token)
    {
        if (string.IsNullOrEmpty(token))
        {
            return null;
        }

        lock (_gate)
        {
            if (!_tokens.TryGetValue(token, out var t) ||
                t.ExpiresUtc < DateTime.UtcNow)
            {
                _tokens.Remove(token);
                return null;
            }

            return _byLogin.TryGetValue(t.Login, out var rec)
                ? InfoOf(rec)
                : null;
        }
    }
}
