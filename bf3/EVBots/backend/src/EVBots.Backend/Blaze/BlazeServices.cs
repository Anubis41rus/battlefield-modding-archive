// BlazeServices.cs — диспетчер и обработчики компонентов.
//
// Имена полей в ответах и уведомлениях — из Payloads.cs, где они взяты из
// протокола (New-Blaze-Emulator) и подтверждены строками рабочего бэкенда.
// Ничего не выдумывается: см. Payloads.cs и VU-HOWITWORKS.md.

using System.Net;
using EVBots.Backend.Data;
using EVBots.Backend.Tdf;

namespace EVBots.Backend.Blaze;

internal sealed class BlazeServices(
    ServeConfig cfg,
    ProfileStore store,
    AccountsStore accounts,
    StatsCatalog stats,
    InstanceCatalog catalog,
    Log log)
{
    private readonly Dictionary<long, BlazeSession> _sessions = new();
    private readonly Lock _gate = new();

    public GameRegistry Games { get; } = new();

    public IReadOnlyCollection<BlazeSession> Sessions
    {
        get
        {
            lock (_gate)
            {
                return _sessions.Values.ToList();
            }
        }
    }

    public ServeConfig Config => cfg;
    public ProfileStore Store => store;
    public StatsCatalog Stats => stats;
    public InstanceCatalog Catalog => catalog;
    public Log Logger => log;

    /// <summary>Моды, объявленные в ModList.txt (загрузятся при старте).</summary>
    public IReadOnlyList<string> ConfiguredMods => catalog.ReadModList();

    /// <summary>Моды, реально загруженные этим запуском.</summary>
    public IReadOnlyList<string> RunningMods { get; } = new List<string>();

    public void Register(BlazeSession s)
    {
        lock (_gate)
        {
            _sessions[s.Id] = s;
        }
    }

    public void Unregister(BlazeSession s)
    {
        lock (_gate)
        {
            _sessions.Remove(s.Id);
        }

        Games.Detach(s);
    }

    public void Dispatch(BlazeSession s, ushort component, ushort command,
        int messageId, TdfFields fields)
    {
        // Диагностика 13.10: каждый входящий кадр — в лог (компонент/команда/id),
        // чтобы понять, на какой команде бэкенд замолкает после выхода клиента
        // (сервер уходил в OFFLINE с RPC_TIMEOUT на removePlayer/MidGameReport).
        s.Log.Info($"[frame] {Component.Name(component)} cmd=0x{command:X} " +
                   $"id={messageId} сессия={s.Id} GID={s.GameId}");

        // отложенные join-уведомления: клиент готов, когда после дедлайна
        // пришёл первый пакет (пинг каждые 20 с — как у рабочего бэкенда)
        TryFlushJoinNotify(s);

        // живой сервер: тажим игру на ЛЮБОМ Blaze-трафике хоста (в т.ч. пинг).
        // Иначе sweeper (90 с простоя) решит, что сервер умер, и уберёт его
        // из /servers, хотя сервер жив — просто не дошёл до раунда.
        if (s.GameId != 0)
        {
            Games.Touch(s.GameId);
        }

        try
        {
            switch (component)
            {
                case Component.Util:
                    HandleUtil(s, command, messageId, fields);
                    break;

                case Component.Authentication:
                    HandleAuthentication(s, command, messageId, fields);
                    break;

                case Component.Stats:
                    HandleStats(s, command, messageId, fields);
                    break;

                case Component.Redirector:
                    HandleRedirector(s, command, messageId, fields);
                    break;

                case Component.UserSessions:
                    HandleUserSessions(s, command, messageId, fields);
                    break;

                case Component.GameManager:
                    HandleGameManager(s, command, messageId, fields);
                    break;

                default:
                    Unknown(s, component, command, messageId,
                        $"компонент {Component.Name(component)} не реализован");
                    break;
            }
        }
        catch (Exception ex)
        {
            log.Error($"{Component.Name(component)} cmd={command}: {ex.Message}");
            log.Debug(ex.ToString());

            // клиент ждёт ответа на каждый запрос: без него он зависнет
            if (!s.Replied)
            {
                s.ReplyError(component, command, messageId,
                    BlazeError.ServiceUnavailable);
            }
        }
    }

    /// <summary>
    /// Ответ на неподдерживаемую команду. Молчать нельзя: клиент BF3 ждёт
    /// ответа на каждый запрос и иначе зависает на этом кадре.
    /// </summary>
    private static void Unknown(BlazeSession s, ushort component, ushort command,
        int messageId, string why)
    {
        s.Log.Debug($"{Component.Name(component)} cmd={command}: {why}");
        // рабочий бэкенд на неизвестное отвечает пустым OK (fallback),
        // ошибка (1003) ломает клиентский join-флоу
        s.Reply(component, command, messageId, Array.Empty<TdfNode>());
    }

    // ── Util ────────────────────────────────────────────────────────────────

    private void HandleUtil(BlazeSession s, ushort cmd, int messageId, TdfFields f)
    {
        switch (cmd)
        {
            case UtilCmd.FetchClientConfig:
            {
                var cfid = f.Str("CFID", "");
                log.Info($"[blaze] fetchClientConfig CFID=\"{cfid}\" " +
                         $"(сессия {s.Id})");
                s.Reply(Component.Util, cmd, messageId,
                    Payloads.FetchClientConfigReply(cfg.GosAchievements));
                break;
            }

            case UtilCmd.GetTelemetryServer:
            {
                // телеметрия EA не нужна: отдаём loopback с пустыми фильтрами
                log.Debug("[blaze] getTelemetryServer -> loopback, opt-out");
                s.Reply(Component.Util, cmd, messageId,
                    Payloads.TelemetryReply(s.Localization));
                break;
            }

            case UtilCmd.PreAuth:
            {
                // запрос: CDAT{TYPE, SVCN} и CINF{LOC}
                var service = s.Service;
                var cdats = f.Struct("CDAT");
                if (cdats is not null)
                {
                    service = cdats["SVCN"] is TdfStr s2 ? s2.Value : service;
                    if (cdats["TYPE"] is TdfInt t)
                    {
                        s.ClientType = (int)t.Value;
                    }
                }

                var cinf = f.Struct("CINF");
                if (cinf?["LOC"] is TdfInt loc)
                {
                    s.Localization = loc.Value;
                }

                log.Info($"[blaze] preAuth SVCN=\"{service}\" (сессия {s.Id})");

                s.Service = service;
                s.Reply(Component.Util, cmd, messageId,
                    Payloads.PreAuthReply(service, cfg.QosIp, cfg.QosPort));
                break;
            }

            case UtilCmd.Ping:
            {
                // ответ на пинг: серверное время
                s.Reply(Component.Util, cmd, messageId,
                    Payloads.PingReply(UnixMillis()));
                break;
            }

            case UtilCmd.PostAuth:
            {
                log.Info($"[blaze] postAuth (сессия {s.Id})");
                s.Reply(Component.Util, cmd, messageId,
                    Payloads.PostAuthReply(s.Localization,
                        s.Personaref != 0 ? s.Personaref : (ulong)s.Id,
                        cfg.QosIp, cfg.QosPort));
                break;
            }

            case UtilCmd.UserSettingsLoadAll:
            {
                var pid = f.UInt("PID", s.Personaref);
                var settings = LoadUserSettings(pid);
                s.Reply(Component.Util, cmd, messageId,
                    Payloads.UserSettingsLoadAllReply(settings));
                log.Debug($"[blaze] userSettingsLoadAll PID={pid} -> " +
                          $"{settings.Count} ключей");
                break;
            }

            case UtilCmd.UserSettingsSave:
            {
                // DATA приходит СТРОКОЙ, а не блобом
                var pid = f.UInt("PID", s.Personaref);
                var key = f.Str("KEY", "");
                var value = f.Str("DATA", "");
                if (value.Length > 0)
                {
                    SaveUserSetting(pid, key, value);
                }

                s.Reply(Component.Util, cmd, messageId, Array.Empty<TdfNode>());
                break;
            }

            case UtilCmd.SetClientMetrics:
                s.Reply(Component.Util, cmd, messageId, Array.Empty<TdfNode>());
                break;

            default:
                Unknown(s, Component.Util, cmd, messageId,
                    "команда Util не реализована");
                break;
        }
    }

    /// <summary>
    /// Настройки игрока: ключи из файлов settings\*.bin, которые мы уже
    /// сохраняем (cust.bin, sdt.bin). Отдаём строкой — так ждёт клиент.
    /// </summary>
    private Dictionary<string, string> LoadUserSettings(ulong pid)
    {
        var result = new Dictionary<string, string>(StringComparer.Ordinal);
        if (pid == 0)
        {
            return result;
        }

        var dir = Path.Combine(cfg.ProfilesDir,
            pid.ToString(System.Globalization.CultureInfo.InvariantCulture),
            "settings");

        if (!Directory.Exists(dir))
        {
            return result;
        }

        foreach (var file in Directory.EnumerateFiles(dir, "*.bin"))
        {
            var key = Path.GetFileNameWithoutExtension(file);
            try
            {
                result[key] = Convert.ToBase64String(File.ReadAllBytes(file));
            }
            catch (IOException)
            {
                // файл занят — пропустим
            }
        }

        return result;
    }

    private void SaveUserSetting(ulong pid, string key, string value)
    {
        if (pid == 0 || string.IsNullOrEmpty(key))
        {
            return;
        }

        var dir = Path.Combine(cfg.ProfilesDir,
            pid.ToString(System.Globalization.CultureInfo.InvariantCulture),
            "settings");
        Directory.CreateDirectory(dir);

        var safe = string.Concat(key.Select(ch =>
            char.IsLetterOrDigit(ch) || ch is '_' or '-' ? ch : '_'));

        byte[] bytes;
        try
        {
            bytes = Convert.FromBase64String(value);
        }
        catch (FormatException)
        {
            bytes = System.Text.Encoding.UTF8.GetBytes(value);
        }

        File.WriteAllBytes(Path.Combine(dir, safe + ".bin"), bytes);
        log.Debug($"[blaze] userSettingsSave PID={pid} KEY=\"{key}\" " +
                  $"({bytes.Length} байт)");
    }

    // ── Authentication ──────────────────────────────────────────────────────

    private void HandleAuthentication(BlazeSession s, ushort cmd, int messageId,
        TdfFields f)
    {
        switch (cmd)
        {
            case AuthCmd.Login:
            {
                var mail = f.Str("MAIL", "");
                var pass = f.Str("PASS", "");
                log.Info($"[blaze] login MAIL=\"{mail}\" (сессия {s.Id})");

                ProfileFile? profile;

                if (accounts.Exists(mail))
                {
                    // аккаунт зарегистрирован — пароль обязан совпасть
                    if (!accounts.Verify(mail, pass, out var acc, out var err))
                    {
                        log.Warn($"login отклонён ({err}): MAIL=\"{mail}\"");
                        s.ReplyError(Component.Authentication, cmd, messageId,
                            BlazeError.AuthFailed);
                        return;
                    }

                    profile = store.Find(acc.Personaref) ??
                              store.FindByName(acc.Login) ??
                              store.Create(acc.Login);
                }
                else
                {
                    // первого раза — автоматическая регистрация аккаунта на
                    // уже созданный профиль (выделенный сервер:
                    // -Login bf3_1 -Password password)
                    profile = store.FindByName(mail);
                    if (profile is null)
                    {
                        profile = store.Create(mail.Length > 0 ? mail : "Player");
                    }

                    if (profile is not null && pass.Length > 0)
                    {
                        accounts.EnsureForProfile(mail, pass, profile.Id, out _);
                    }
                }

                if (profile is null)
                {
                    s.ReplyError(Component.Authentication, cmd, messageId,
                        BlazeError.NameInUse);
                    return;
                }

                AdoptSession(s, profile.Id, profile.Name, $"{profile.Name}@evbots.local");
                EnsureProgress(s);

                s.Reply(Component.Authentication, cmd, messageId,
                    Payloads.LoginReply((ulong)s.Id, profile.Id, profile.Name,
                        $"{profile.Name}@evbots.local"));
                break;
            }

            case AuthCmd.SilentLogin:
            {
                var pid = f.UInt("PID");
                log.Info($"[blaze] silentLogin PID={pid} (сессия {s.Id})");

                // PID = 0 — это не человек: заводить «Player0» нельзя,
                // иначе мусорные профили появятся при каждом запросе
                if (pid == 0)
                {
                    log.Warn("silentLogin без PID — отклонено");
                    s.ReplyError(Component.Authentication, cmd, messageId,
                        BlazeError.AuthFailed);
                    return;
                }

                var profile = store.Find(pid);
                if (profile is null)
                {
                    // раньше тут молча создавался Player{pid} — мусорный
                    // профиль при каждом чужом PID. Аккаунт должен быть
                    // зарегистрирован ДО запуска игры (join.bat вызывает
                    // POST /accounts/ensure); иначе — отказ.
                    log.Warn($"silentLogin: персона {pid} не зарегистрирована — " +
                             "отклонено (нужен аккаунт /accounts/ensure)");
                    s.ReplyError(Component.Authentication, cmd, messageId,
                        BlazeError.AuthFailed);
                    return;
                }

                AdoptSession(s, profile.Id, profile.Name, $"{profile.Name}@evbots.local");
                EnsureProgress(s);

                s.Reply(Component.Authentication, cmd, messageId,
                    Payloads.SilentLoginReply(profile.Id, profile.Name,
                        $"{profile.Name}@evbots.local"));

                NotifyUserAdded(s);
                break;
            }

            case AuthCmd.LoginPersona:
            {
                var name = f.Str("PNAM", "");
                log.Info($"[blaze] loginPersona PNAM=\"{name}\" (сессия {s.Id})");

                var profile = store.FindByName(name);
                if (profile is null)
                {
                    // VU требует, чтобы солдат существовал; мы тоже
                    profile = store.Create(name);
                }

                if (profile is null)
                {
                    s.ReplyError(Component.Authentication, cmd, messageId,
                        BlazeError.NameInUse);
                    return;
                }

                AdoptSession(s, profile.Id, profile.Name, $"{profile.Name}@evbots.local");
                EnsureProgress(s);

                s.Reply(Component.Authentication, cmd, messageId,
                    Payloads.LoginPersonaReply((ulong)s.Id, profile.Id,
                        profile.Name, $"{profile.Name}@evbots.local"));

                NotifyUserAdded(s);
                break;
            }

            case AuthCmd.ListUserEntitlements2:
            {
                // Как в рабочем бэкенде: ETAG="ONLINE_ACCESS" -> пустой ответ,
                // любой другой -> прогресс-список QueryGeneral (58 штук).
                // Без прогресс-списка клиент не запрашивает статы и не
                // показывает ранг.
                var tag = f.Str("ETAG", "");
                if (tag == "ONLINE_ACCESS")
                {
                    s.Reply(Component.Authentication, cmd, messageId,
                        Array.Empty<TdfNode>());
                    log.Debug("[blaze] listUserEntitlements2 ETAG=\"ONLINE_ACCESS\" " +
                              "-> пусто (Online Pass не нужен на PC)");
                }
                else
                {
                    s.Reply(Component.Authentication, cmd, messageId,
                        Payloads.EntitlementsProgressReply());
                    log.Debug($"[blaze] listUserEntitlements2 ETAG=\"{tag}\" -> " +
                              $"{Payloads.ProgressEntitlements.Length} " +
                              "entitlements (progress on: QueryGeneral)");
                }
                break;
            }

            default:
                Unknown(s, Component.Authentication, cmd, messageId,
                    "команда Authentication не реализована");
                break;
        }
    }

    private static void AdoptSession(BlazeSession s, ulong personaref,
        string soldierName, string mail)
    {
        s.Personaref = personaref;
        s.SoldierName = soldierName;
        s.Mail = mail;
        s.Authenticated = true;
    }

    /// <summary>
    /// Прогресс должен существовать, иначе клиент не покажет ранг.
    /// Пустой набор копируем с первого игрока, у которого он есть.
    /// </summary>
    private void EnsureProgress(BlazeSession s)
    {
        var progress = store.GetProgress(s.Personaref);
        if (progress.Stats.Count > 0)
        {
            return;
        }

        var donor = store.All.FirstOrDefault(p =>
            p.Id != s.Personaref && store.GetProgress(p.Id).Stats.Count > 0);

        if (donor is not null)
        {
            store.CloneProgress(donor.Id, s.Personaref);
            log.Info($"прогресс нового солдата '{s.SoldierName}' " +
                     $"скопирован с p={donor.Id} (иначе ранг не покажется)");
        }
    }

    /// <summary>
    /// Отложенные join-уведомления (полный блок, как у рабочего бэкенда):
    /// UserAdded/UserUpdated о хосте — клиенту, PlayerJoining/Claiming —
    /// хост-серверу, InitiateConnections — клиенту. Отправляются на первом
    /// же пакете клиента после дедлайна (+20 с от joinGame): во время
    /// загрузки карты клиент не поднимает mesh и уведомления пропадают.
    /// </summary>
    private void TryFlushJoinNotify(BlazeSession s)
    {
        var due = s.JoinNotifyDue;
        if (due is null || DateTime.UtcNow < due.Value)
        {
            return;
        }

        s.JoinNotifyDue = null;

        var game = Games.Find(s.GameId);
        if (game is null)
        {
            return;
        }

        log.Info("[blaze] join-уведомления: клиент готов, отправляю " +
                 $"(GID={game.GameId}, слот={s.PendingJoinSlot})");

        if (game.Host is { } host)
        {
            s.Notify(Component.UserSessions,
                UserSessionsCmd.UserAddedNotification,
                Payloads.UserAdded(host.Personaref, host.SoldierName,
                    s.Localization));
            s.Notify(Component.UserSessions,
                UserSessionsCmd.UserUpdatedNotification,
                Payloads.UserUpdated(host.Personaref));
        }

        // Сервер (хост игры) проверяет расширенные данные каждого вошедшего игрока:
        // без уведомления UserSessionExtendedDataUpdate (1) движок пишет
        // «Joined player has no blaze extended data» и не выдаёт игрока в мир.
        // 20.10: уведомление 1 сервер применяет только к уже известному пользователю (0x0151D580), поэтому сначала
        // UserAdded о входящем игроке — его обработчик (0x0151DBF0) создаёт пользователя сразу с расширенными данными.
        if (game.Host is { } srv)
        {
            if (cfg.JoinUserToServer)
            {
                srv.Notify(Component.UserSessions,
                    UserSessionsCmd.UserAddedNotification,
                    Payloads.UserAdded(s.Personaref, s.SoldierName, s.Localization));
                srv.Notify(Component.UserSessions,
                    UserSessionsCmd.UserUpdatedNotification,
                    Payloads.UserUpdated(s.Personaref));
                log.Info($"[blaze] серверу: UserAdded/UserUpdated о p={s.Personaref} ({s.SoldierName})");
            }
            srv.Notify(Component.UserSessions,
                UserSessionsCmd.UserSessionExtendedDataUpdate,
                Payloads.SessionExtended(s.Personaref, cfg.GameServerIp,
                    cfg.GameServerPort, game.GameId, false));
        }

        // порядок уведомлений важен: сначала хост узнаёт о игроке,
        // затем игрок получает адрес игры
        game.NotifyPlayerJoining(s, s.PendingJoinSlot);
        game.NotifyPlayerClaimingReservation(s, s.PendingJoinSlot);

        // 20.10: после PlayerJoining пользователь у сервера точно есть — повторяем уведомление 1
        if (cfg.JoinUserToServer && game.Host is { } srv2)
        {
            srv2.Notify(Component.UserSessions,
                UserSessionsCmd.UserSessionExtendedDataUpdate,
                Payloads.SessionExtended(s.Personaref, cfg.GameServerIp,
                    cfg.GameServerPort, game.GameId, false));
        }
        s.Notify(Component.GameManager, GameManagerNotify.InitiateConnections,
            InitiatePayload(game, s, s.PendingJoinSlot));
    }

    /// <summary>
    /// После входа клиент ждёт UserAdded и UserUpdated — без них он не
    /// считает сессию established.
    /// </summary>
    private void NotifyUserAdded(BlazeSession s)
    {
        if (s.Personaref == 0)
        {
            return;
        }

        s.Notify(Component.UserSessions, UserSessionsCmd.UserAddedNotification,
            Payloads.UserAdded(s.Personaref, s.SoldierName, s.Localization));
        s.Notify(Component.UserSessions, UserSessionsCmd.UserUpdatedNotification,
            Payloads.UserUpdated(s.Personaref));
    }

    // ── GameReporting (18.10) ───────────────────────────────────────────────

    public void HandleGameReport(BlazeSession s, ushort cmd, int messageId, byte[] payload)
    {
        s.Log.Info($"[frame] GameReporting cmd=0x{cmd:X} id={messageId} сессия={s.Id} GID={s.GameId}");
        if (s.GameId != 0)
        {
            Games.Touch(s.GameId);
        }

        GameReporting.Handle(s, cmd, messageId, payload, store, stats, log);
    }

    // ── Stats ───────────────────────────────────────────────────────────────

    private void HandleStats(BlazeSession s, ushort cmd, int messageId, TdfFields f)
    {
        switch (cmd)
        {
            case StatsCmd.GetStatGroup:
            {
                var name = f.Str("NAME", "");
                log.Info($"[blaze] getStatGroup NAME=\"{name}\" (сессия {s.Id})");

                if (!stats.Ok || stats.Payload.Length == 0)
                {
                    log.Warn("каталог прогресса не загружен — отдаём пустой ответ");
                    s.Reply(Component.Stats, cmd, messageId, Array.Empty<TdfNode>());
                    return;
                }

                // payload из каталога — это НЕ TDF, а бинарный блок DICE
                // statgroups/bf3/bf3_player_mpdefault2.bin. Отдаём байт в байт.
                s.SendRaw(Component.Stats, cmd, messageId, stats.Payload);
                log.Debug($"[blaze] getStatGroup отдан payload " +
                          $"{stats.PayloadLength:N0} байт, {stats.StatCount} стат");
                break;
            }

            case StatsCmd.GetStatsByGroupAsync:
            {
                // Формат снят с рабочего StatsService (эталон):
                //   запрос  EID — СПИСОК Integer (несколько персон за раз);
                //   ответ   — пустой реплай ПЛЮС уведомление 0x32 вида
                //             {GRNM, KEY="No_Scope_Defined", LAST=1,
                //              STS{STAT: []entity}, VID},
                //   entity  — {EID, ETYP=ObjectType(30722,1), POFF=0,
                //              STAT: список строк "F2" в порядке каталога}.
                // Прежний формат (STATS map + SNAM + EID) клиент игнорировал,
                // из-за чего ранг не отображался.
                var name = f.Str("NAME", stats.Group);
                var vid = f.UInt("VID");
                var entityIds = new List<ulong>();
                if (f.List("EID") is { } eidList)
                {
                    foreach (var item in eidList.Items)
                    {
                        entityIds.Add(Convert.ToUInt64(item));
                    }
                }

                if (entityIds.Count == 0)
                {
                    // без списка — запрос о самом игроке
                    entityIds.Add(s.Personaref);
                }

                var entities = new List<object>(entityIds.Count);
                var summary = new List<string>(entityIds.Count);
                foreach (var id in entityIds)
                {
                    entities.Add(new List<TdfNode>
                    {
                        TdfNode.Int("EID", id),
                        new TdfObjectType("ETYP", 30722, 1),
                        TdfNode.Int("POFF", 0),
                        new TdfList("STAT", TdfType.String,
                            stats.FormatValuesOrdered(store, id)),
                    });
                    summary.Add($"{id}:{store.GetProgress(id).Stats.Count}");
                }

                log.Info($"[blaze] getStatsByGroupAsync NAME=\"{name}\" VID={vid} " +
                         $"EID=[{string.Join(",", entityIds)}] -> пустой реплай " +
                         $"+ notify 0x32 [{string.Join(" ", summary)}]");

                // порядок как AfterReply рабочего: сначала реплай, потом 0x32
                s.Reply(Component.Stats, cmd, messageId, Array.Empty<TdfNode>());
                s.Notify(Component.Stats, StatsCmd.StatsAsyncNotification,
                    new TdfNode[]
                    {
                        TdfNode.Str("GRNM", name),
                        TdfNode.Str("KEY", "No_Scope_Defined"),
                        TdfNode.Int("LAST", 1),
                        TdfNode.Struct("STS",
                            new TdfList("STAT", TdfType.Struct, entities)),
                        TdfNode.Int("VID", vid),
                    });
                break;
            }

            default:
                Unknown(s, Component.Stats, cmd, messageId,
                    "команда Stats не реализована");
                break;
        }
    }

    // ── Redirector ──────────────────────────────────────────────────────────

    private void HandleRedirector(BlazeSession s, ushort cmd, int messageId,
        TdfFields f)
    {
        if (cmd != RedirectorCmd.GetServerInstance)
        {
            Unknown(s, Component.Redirector, cmd, messageId,
                "команда Redirector не реализована");
            return;
        }

        var clnt = f.UInt("CLNT");
        log.Info($"[blaze] getServerInstance CLNT={clnt} -> " +
                 $"{cfg.AdvertisedIp}:{cfg.BlazePort} (сессия {s.Id})");

        // ADDR — union NetworkAddress: клиент идёт по этому адресу за Blaze
        s.Reply(Component.Redirector, cmd, messageId,
            Payloads.ServerInstanceReply(cfg.QosIp, cfg.BlazePort,
                "gosredirector.ea.com"));
    }

    // ── UserSessions ────────────────────────────────────────────────────────

    private void HandleUserSessions(BlazeSession s, ushort cmd, int messageId,
        TdfFields f)
    {
        if (cmd == UserSessionsCmd.UpdateNetworkInfo)
        {
            // ADDR -> VALU -> INIP{IP, PORT}
            var addr = f["ADDR"] as TdfUnion;
            var valu = addr?.Data as TdfStruct;
            var inip = valu?["INIP"] as TdfStruct;

            if (inip?["IP"] is TdfInt ip && inip["PORT"] is TdfInt port)
            {
                s.InternalIp = ip.Value;
                s.InternalPort = (int)port.Value;
                log.Debug($"[blaze] updateNetworkInfo INIP=" +
                          $"{ip.Value}:{port.Value} (сессия {s.Id})");
            }

            s.Reply(Component.UserSessions, cmd, messageId, Array.Empty<TdfNode>());
            return;
        }

        Unknown(s, Component.UserSessions, cmd, messageId,
            "команда UserSessions не реализована");
    }

    // ── GameManager ─────────────────────────────────────────────────────────

    private void HandleGameManager(BlazeSession s, ushort cmd, int messageId,
        TdfFields f)
    {
        switch (cmd)
        {
            case GameManagerCmd.CreateGame:
            {
                // запрос: ATTR (map), GNAM (имя), PCAP, PMAX, NRES, NTOP,
                // VOIP, PRES, QCAP, IGNO, GSET
                var name = f.Str("GNAM", "EVBots Server");
                var attrs = ReadAttrMap(f);

                var game = Games.Create(name, s, attrs, catalog);
                game.Gset = f.UInt("GSET", 1325);
                s.GameId = game.GameId;
                s.GameHost = s;

                log.Info($"[blaze-server] createGame \"{name}\" GID={game.GameId}" +
                         $" attributes {attrs.Count}");

                s.Reply(Component.GameManager, cmd, messageId,
                    Payloads.CreateGameReply(game.GameId));

                // GameSetup создаёт локальный объект игры с начальным состоянием.
                s.Notify(Component.GameManager, GameManagerNotify.GameSetup,
                    GameSetupPayload(game, s));
                break;
            }

            case GameManagerCmd.FinalizeGameCreation:
            {
                var gid = f.UInt("GID", s.GameId);
                var game = Games.Touch(gid);
                if (game is not null)
                {
                    game.Finalized = true;
                    log.Info($"[blaze-server] finalizeGameCreation GID={gid}");
                }

                s.Reply(Component.GameManager, cmd, messageId,
                    Payloads.FinalizeGameReply(gid));
                break;
            }

            case GameManagerCmd.AdvanceGameState:
            {
                var gid = f.UInt("GID", s.GameId);
                var state = f.UInt("GSTA");
                var game = Games.Touch(gid);
                if (game is not null)
                {
                    game.State = state;
                }

                log.Info($"[blaze-server] advanceGameState GID={gid} GSTA={state}");
                s.Reply(Component.GameManager, cmd, messageId, Array.Empty<TdfNode>());

                // Эталон AdvanceGameStateCommand: после пустого ответа клиент
                // ждёт подтверждение смены состояния уведомлением 0x64
                // (GameStateChangeNotification.Notify). Без него выделенный
                // сервер остаётся в инициализации — проверено живьём 09.10.
                // 19.10: как рабочий бэкенд (BroadcastAsync) — хосту и всем игрокам игры.
                NotifyGame(game, s, GameManagerNotify.GameStateChange,
                    Payloads.GameStateChange(gid, state));
                break;
            }

            case GameManagerCmd.ReplayGame:
            {
                // 19.10 (прогон 195): после первого конца раунда игра застревала в POST_GAME (4) — replayGame
                // уходил в Unknown без смены состояния. Сервер в тике BlazeServerGameManager (0x011E6B40) считает время
                // вне IN_GAME (0x83) и через 600 с пишет «Game stuck in some state but InGame. Attempting to restart»,
                // пересоздаёт игру и кикает людей (02:48:52). Рабочий бэкенд: HandleReplayGame → PRE_GAME (130) +
                // GameStateChange (0x64) хосту и игрокам; на старте раунда сервер сам шлёт advanceGameState 131.
                var gid = f.UInt("GID", s.GameId);
                var game = Games.Touch(gid);
                const ulong preGame = 130;
                if (game is not null)
                {
                    game.State = preGame;
                }

                log.Info($"[blaze-server] replayGame GID={gid} -> PRE_GAME");
                s.Reply(Component.GameManager, cmd, messageId, Array.Empty<TdfNode>());
                NotifyGame(game, s, GameManagerNotify.GameStateChange,
                    Payloads.GameStateChange(gid, preGame));
                break;
            }

            case GameManagerCmd.JoinGame:
            {
                var gid = f.UInt("GID");
                var game = Games.Touch(gid);

                if (game is null)
                {
                    log.Warn($"[blaze] joinGame GID={gid} — такой игры нет");
                    s.ReplyError(Component.GameManager, cmd, messageId,
                        BlazeError.GameNotFound);
                    return;
                }

                s.GameId = gid;
                s.GameHost = game.Host;

                var slot = game.Attach(s);

                log.Info($"[blaze] joinGame GID={gid} GVER=\"{f.Str("GVER")}\" " +
                         $"игрок={s.SoldierName} p={s.Personaref} слот={slot}");

                s.Reply(Component.GameManager, cmd, messageId,
                    Payloads.JoinGameReply(gid));

                // как в эталоне (JoinGameCommand) клиенту нужны UserAdded/
                // UserUpdated о хосте — но отправляем всё отложенно, когда
                // клиент догрузил карту (см. TryFlushJoinNotify)
                s.JoinNotifyDue = DateTime.UtcNow.AddSeconds(20);
                s.PendingJoinSlot = slot;
                break;
            }

            case GameManagerCmd.DestroyGame:
            {
                var gid = f.UInt("GID", s.GameId);
                Games.Remove(gid);
                log.Info($"[blaze-server] destroyGame GID={gid}");
                s.Reply(Component.GameManager, cmd, messageId, Array.Empty<TdfNode>());
                break;
            }

            case GameManagerCmd.SetGameSettings:
            case GameManagerCmd.SetPlayerCapacity:
            case GameManagerCmd.SetGameAttributes:
                log.Debug($"[blaze-server] GameManager cmd={cmd} " +
                          $"(принято, GID={f.UInt("GID", s.GameId)})");
                s.Reply(Component.GameManager, cmd, messageId, Array.Empty<TdfNode>());
                break;

            case GameManagerCmd.UpdateMeshConnection:
            {
                // Зеркало HandleUpdateMeshConnection рабочего бэкенда:
                // на STAT=2 (подключился) отправителю 0x1D после респонса
                // уходят 4:116 {GID,PID,STAT=4} + 4:30 {GID,PID} — клиент
                // ждёт state=4, прежде чем грузить карту. На STAT=0 —
                // удаляем игрока и рассылаем 4:40 PlayerRemoved остальным.
                var gid = f.UInt("GID", s.GameId);
                var game = Games.Touch(gid);
                var after = new List<(BlazeSession Target, ushort NotifyCmd,
                    TdfNode[] Payload)>();

                if (f.List("TARG") is { } targ)
                {
                    // элемент struct-списка у нас — список членов (не TdfStruct)
                    foreach (var entry in targ.Items
                                 .OfType<IReadOnlyList<TdfNode>>())
                    {
                        var item = new TdfStruct("", entry);
                        var pid = (item["PID"] as TdfInt)?.Value ?? 0UL;
                        var statNode = item["STAT"] as TdfInt;
                        var stat = statNode is null ? -1 : (int)statNode.Value;

                        // от сервера речь о цели (TARG.PID), от клиента — о нём
                        var playerPid = ReferenceEquals(game?.Host, s)
                            ? pid : s.Personaref;

                        // Info (было Debug): диагностика зависания после выхода клиента
                        log.Info($"[blaze] updateMeshConnection GID={gid} " +
                                 $"от p={s.Personaref} о p={pid} STAT={stat}" +
                                 $" -> p={playerPid}");

                        if (stat == 2)
                        {
                            after.Add((s, GameManagerNotify.PlayerStateChanged,
                                Payloads.PlayerStateChanged(gid, playerPid, 4)
                                    .ToArray()));
                            after.Add((s, GameManagerNotify.PlayerJoinCompleted,
                                Payloads.PlayerJoinCompleted(gid, playerPid)
                                    .ToArray()));
                        }
                        else if (stat == 0 && game is not null)
                        {
                            var gone = game.Players.FirstOrDefault(
                                p => p.Personaref == playerPid);
                            if (gone is not null)
                            {
                                game.RemovePlayer(gone);
                                var payload = Payloads.PlayerRemoved(
                                    game.GameId, playerPid, 1).ToArray();

                                var targets = new List<BlazeSession>();
                                if (game.Host is not null &&
                                    !ReferenceEquals(game.Host, gone))
                                {
                                    targets.Add(game.Host);
                                }

                                foreach (var p in game.Players)
                                {
                                    if (!ReferenceEquals(p, gone) &&
                                        !targets.Contains(p))
                                    {
                                        targets.Add(p);
                                    }
                                }

                                foreach (var t in targets)
                                {
                                    after.Add((t,
                                        GameManagerNotify.PlayerRemoved,
                                        payload));
                                }
                            }
                        }
                    }
                }

                s.Reply(Component.GameManager, cmd, messageId, Array.Empty<TdfNode>());
                foreach (var (target, notifyCmd, payload) in after)
                {
                    target.Notify(Component.GameManager, notifyCmd, payload);
                }

                break;
            }

            case GameManagerCmd.RemovePlayer:
            {
                var gid = f.UInt("GID", s.GameId);
                // Диагностика 16.10 (прогон 159): все поля запроса со значениями — видно причину выхода клиента из игры.
                var fieldsText = new System.Text.StringBuilder();
                foreach (var node in f.All)
                {
                    fieldsText.Append(node.ToString()).Append(' ');
                }
                // 28.10 (239): сервер шлёт removePlayer о другом игроке (PID, журналы стенда 26.10: «игрок=bf3_1 …
                // PID=234471788 REAS=8») — раньше здесь из игры удалялся сам сервер. Убираем того, о ком запрос.
                var rgame = Games.Touch(gid);
                var rpid = f.UInt("PID", s.Personaref);
                string removed;
                if (rgame is not null && ReferenceEquals(rgame.Host, s))
                {
                    removed = rgame.RemovePlayerById(rpid)?.SoldierName ?? "нет в игре";
                }
                else
                {
                    rgame?.RemovePlayer(s);
                    removed = s.SoldierName;
                }
                log.Info($"[blaze-server] removePlayer GID={gid} " +
                         $"от={s.SoldierName} убран={removed} поля: {fieldsText.ToString().TrimEnd()}");
                s.Reply(Component.GameManager, cmd, messageId, Array.Empty<TdfNode>());

                // ★ 28.10 (239, прогон: снятый клиент → перезаход висит): в Blaze участник уходит из игры у всех по
                // уведомлению 4:40 PlayerRemoved, в т.ч. у сервера после его же removePlayer. Мы его слали только из
                // updateMeshConnection STAT=0 и только если игрок ещё в списке — а при обрыве клиента сессия уже убрана
                // (Detach за 20 мс до STAT=0), и сервер так и держал игрока: при повторном входе того же PID соединение
                // не создаётся (оба STAT=0 через 10 с) — бесконечная загрузка до перезапуска сервера (жалоба друга 27.10).
                if (rgame is not null)
                {
                    var payload = Payloads.PlayerRemoved(rgame.GameId, rpid, f.UInt("REAS", 1)).ToArray();
                    var targets = new List<BlazeSession>();
                    lock (rgame.Gate)
                    {
                        if (rgame.Host is not null) targets.Add(rgame.Host);
                        targets.AddRange(rgame.Players.Where(p => p.Personaref != rpid));
                    }
                    foreach (var t in targets.Distinct())
                    {
                        t.Notify(Component.GameManager, GameManagerNotify.PlayerRemoved, payload);
                    }
                    log.Info($"[blaze-server] PlayerRemoved p={rpid} REAS={f.UInt("REAS", 1)} — {targets.Distinct().Count()} получателям " +
                             "(сервер и игроки)");
                }
                break;
            }

            default:
                Unknown(s, Component.GameManager, cmd, messageId,
                    "команда GameManager не реализована");
                break;
        }
    }

    /// <summary>Уведомление всем в игре: хосту, игрокам и отправителю (как BroadcastAsync рабочего бэкенда).
    /// Игры нет — только отправителю.</summary>
    private static void NotifyGame(GameInfo? game, BlazeSession sender, ushort notifyCmd,
        IEnumerable<TdfNode> payload)
    {
        var body = payload.ToArray();
        var targets = new List<BlazeSession> { sender };
        if (game is not null)
        {
            lock (game.Gate)
            {
                if (game.Host is not null) targets.Add(game.Host);
                targets.AddRange(game.Players);
            }
        }

        foreach (var t in targets.Where(t => t is not null).Distinct())
        {
            t.Notify(Component.GameManager, notifyCmd, body);
        }
    }

    /// <summary>ATTR из createGame: карта строк с level, levellocation и т.п.</summary>
    private static Dictionary<string, string> ReadAttrMap(TdfFields f)
    {
        var result = new Dictionary<string, string>(StringComparer.Ordinal);

        if (f["ATTR"] is not TdfMap map)
        {
            return result;
        }

        foreach (var (k, v) in map.Pairs)
        {
            result[k.ToString() ?? ""] = v.ToString() ?? "";
        }

        return result;
    }

    /// <summary>GAME/PROS/QUEU/REAS для GameSetup (0x14, серверу).
    /// Зеркало BuildGameSetup(game, [], 0, null) рабочего бэкенда.</summary>
    private IEnumerable<TdfNode> GameSetupPayload(GameInfo game,
        BlazeSession owner) => Payloads.GameSetupNotification(
        game.GameId,
        owner.Personaref,
        game.Name,
        64,
        game.State,
        cfg.GameServerIp,
        cfg.GameServerPort,
        0, 0,
        game.Attributes,
        game.Uuid.ToString(),
        game.Pgid,
        game.Seed,
        game.Gset,
        0,
        null);

    /// <summary>GAME/PROS/QUEU/REAS для InitiateConnections (0x16).</summary>
    private IEnumerable<TdfNode> InitiatePayload(GameInfo game, BlazeSession s,
        int slot)
    {
        // адрес игрока — из его updateNetworkInfo
        IPAddress playerIp;
        if (s.InternalIp != 0)
        {
            var v = s.InternalIp;
            playerIp = new IPAddress(new[]
            {
                (byte)(v >> 24), (byte)(v >> 16), (byte)(v >> 8), (byte)v,
            });
        }
        else
        {
            playerIp = cfg.GameServerIp;
        }

        var playerPort = s.InternalPort != 0 ? s.InternalPort : 0;

        return Payloads.GameSetupNotification(
        game.GameId,
        game.Host?.Personaref ?? 0,
        game.Name,
        64,
        game.State,
        cfg.GameServerIp,
        cfg.GameServerPort,
        s.Personaref,
        slot,
        game.Attributes,
        game.Uuid.ToString(),
        game.Pgid,
        game.Seed,
        game.Gset,
        3,
        new List<object>
        {
            // PROS = данные зашедшего игрока, STAT=2 — как в BuildPlayer
            (object)Payloads.PlayerData(game.GameId, s.Personaref, s.SoldierName,
                s.Localization, slot, 2, playerIp, playerPort,
                withBlob: true),
        });
    }

    internal static ulong UnixMillis() =>
        (ulong)DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
}
