// GameRegistry.cs — игры, созданные на бэкенде.
//
// Аналог Zeus-функции «реестр серверов», но локальный: игра = один
// запущенный выделенный BF3-сервер. GID — локальный идентификатор вместо
// VU server.key. Атрибуты (ATTR) приходят от клиента в createGame:
// level, levellocation, gameMode и прочие.

using System;
using System.Net;
using EVBots.Backend.Tdf;

namespace EVBots.Backend.Blaze;

internal sealed class GameInfo
{
    public ulong GameId { get; init; }
    public string Name { get; set; } = "";
    public BlazeSession? Host { get; set; }
    public bool Finalized { get; set; }
    public ulong State { get; set; } = 1; // Initializing
    public IReadOnlyDictionary<string, string> Attributes { get; init; } =
        new Dictionary<string, string>();

    /// <summary>
    /// UUID игры — реальный GUID, как у рабочего бэкенда
    /// (Guid.Empty клиент не принимает: висит на загрузке).
    /// </summary>
    public Guid Uuid { get; init; } = Guid.NewGuid();

    /// <summary>PGID — второй GUID игры (persisted game id).</summary>
    public string Pgid { get; init; } = Guid.NewGuid().ToString();

    /// <summary>SEED — большое случайное число, как у рабочего (1249645194).</summary>
    public ulong Seed { get; init; } =
        (ulong)Random.Shared.NextInt64(1_000_000_000, int.MaxValue);

    /// <summary>GSET — битовая маска настроек игры из createGame (у рабочего 1325).</summary>
    public ulong Gset { get; set; } = 1325;

    /// <summary>Игроки по слотам. Индекс в списке — номер слота.</summary>
    public List<BlazeSession> Players { get; } = new();

    /// <summary>
    /// Когда сервер последний раз был слышен по Blaze. Нужно, чтобы убитый
    /// сервер не висел в списке вечно: раньше игра жила в памяти бэкенда,
    /// пока тот не перезапустят, и гость видел «сервер», которого нет.
    /// </summary>
    public DateTime LastSeenUtc { get; private set; } = DateTime.UtcNow;

    public void Touch() => LastSeenUtc = DateTime.UtcNow;

    public Lock Gate { get; } = new();

    /// <summary>
    /// 28.10 (239): слот игрока хранится отдельно, а не берётся позицией в списке. Раньше слот = индекс в Players: после
    /// ухода игроков (и ошибочного удаления сервера по removePlayer) новый вход получал слот, занятый на сервере, или слот
    /// сервера 0 — у друга 27.10 после падения клиента бесконечная загрузка до перезапуска сервера (гипотеза, не прогнано).
    /// </summary>
    private readonly Dictionary<BlazeSession, int> _slots = new();

    /// <summary>
    /// Слот игрока: прежний, если сессия уже в игре, иначе наименьший свободный начиная с 1 (0 — сервер). Старая
    /// сессия того же игрока (клиент упал, вошёл заново) из игры убирается.
    /// </summary>
    public int Attach(BlazeSession s)
    {
        lock (Gate)
        {
            if (_slots.TryGetValue(s, out var have))
            {
                return have;
            }

            Players.RemoveAll(p => !ReferenceEquals(p, s) && !ReferenceEquals(p, Host)
                                   && p.Personaref == s.Personaref);
            foreach (var stale in _slots.Keys.Where(p => !Players.Contains(p)).ToList())
            {
                _slots.Remove(stale);
            }

            var slot = 1;
            while (_slots.ContainsValue(slot))
            {
                slot++;
            }

            if (!Players.Contains(s))
            {
                Players.Add(s);
            }
            _slots[s] = slot;
            return slot;
        }
    }

    public void RemovePlayer(BlazeSession s)
    {
        lock (Gate)
        {
            Players.Remove(s);
            _slots.Remove(s);
        }
    }

    /// <summary>
    /// 28.10 (239): сервер прислал removePlayer о игроке PID — убрать этого игрока (не сам сервер). Возвращает убранного
    /// или null.
    /// </summary>
    public BlazeSession? RemovePlayerById(ulong personaref)
    {
        lock (Gate)
        {
            var gone = Players.FirstOrDefault(p => !ReferenceEquals(p, Host) && p.Personaref == personaref);
            if (gone is not null)
            {
                Players.Remove(gone);
                _slots.Remove(gone);
            }
            return gone;
        }
    }

    /// <summary>
    /// Сколько ИГРОКОВ в игре. Собственный логин выделенного сервера
    /// (bf3_1 из `-Login bf3_1`, слот 0) НЕ считается игроком: иначе в
    /// списке серверов всегда «1 игрок», даже когда никто не заходил.
    /// Хост остаётся в Players — от него зависит нумерация слотов
    /// (клиент получает slot=1).
    /// </summary>
    public int PlayerCount
    {
        get
        {
            lock (Gate)
            {
                return Host is null
                    ? Players.Count
                    : Players.Count(p => !ReferenceEquals(p, Host));
            }
        }
    }

    /// <summary>Игроки без учёта сервера — для списка в интерфейсе.</summary>
    public IReadOnlyList<BlazeSession> HumanPlayers
    {
        get
        {
            lock (Gate)
            {
                return Host is null
                    ? Players.ToList()
                    : Players.Where(p => !ReferenceEquals(p, Host)).ToList();
            }
        }
    }

    /// <summary>Реальный адрес игрока — из его updateNetworkInfo (INIP).</summary>
    private static IPAddress AddrOf(BlazeSession s)
    {
        if (s.InternalIp == 0)
        {
            return IPAddress.Loopback;
        }

        // InternalIp хранится в формате IpToUInt (b0<<24|b1<<16|b2<<8|b3)
        var v = s.InternalIp;
        return new IPAddress(new[]
        {
            (byte)(v >> 24), (byte)(v >> 16), (byte)(v >> 8), (byte)v,
        });
    }

    /// <summary>
    /// Уведомление 0x15 хост-серверу: игрок заходит в игру.
    /// STAT = 0 — «входит». Адрес игрока — из его updateNetworkInfo:
    /// сервер сопоставляет приходящее mesh-подключение с этим адресом
    /// (Loopback:0 не подходит — сервер отвергает игрока, STAT=0).
    /// </summary>
    public void NotifyPlayerJoining(BlazeSession s, int slot) =>
        Host?.Notify(Component.GameManager, GameManagerNotify.PlayerJoining,
            Payloads.PlayerJoining(GameId, s.Personaref, s.SoldierName,
                s.Localization, slot, 0,
                AddrOf(s), s.InternalPort));

    /// <summary>
    /// Уведомление 0x19 хост-серверу: игрок занял слот.
    /// STAT = 2 — «подтверждён».
    /// </summary>
    public void NotifyPlayerClaimingReservation(BlazeSession s, int slot) =>
        Host?.Notify(Component.GameManager,
            GameManagerNotify.PlayerClaimingReservation,
            Payloads.PlayerJoining(GameId, s.Personaref, s.SoldierName,
                s.Localization, slot, 2,
                AddrOf(s), s.InternalPort));
}

internal sealed class GameRegistry
{
    private readonly Dictionary<ulong, GameInfo> _games = new();
    private readonly Lock _gate = new();
    private ulong _next;

    public GameInfo Create(string name, BlazeSession host,
        IReadOnlyDictionary<string, string> attributes,
        Data.InstanceCatalog catalog)
    {
        lock (_gate)
        {
            // GID с 1 — как в рабочем бэкенде BF3NativeBots: join.bat клиента
            // жёстко передаёт gameid=1 (см. комментарий в join.bat)
            var gid = ++_next;

            var attrs = new Dictionary<string, string>(attributes,
                StringComparer.Ordinal);

            // подставляем карту и режим из ротации, если клиент не прислал
            if (!attrs.ContainsKey("level"))
            {
                var first = catalog.ReadMapList().FirstOrDefault();
                if (first is not null)
                {
                    attrs["level"] = first.MapId;
                    attrs["levellocation"] = first.GamemodeId;
                    attrs["gameMode"] = first.GamemodeId;
                }
            }

            var game = new GameInfo
            {
                GameId = gid,
                Name = name,
                Host = host,
                Attributes = attrs,
            };

            // хост занимает слот 0 — как у рабочего бэкенда, где клиент
            // при joinGame получает slot=1 (см. backend-bat.log)
            game.Players.Add(host);

            _games[gid] = game;
            return game;
        }
    }

    public GameInfo? Find(ulong gid)
    {
        lock (_gate)
        {
            return _games.TryGetValue(gid, out var g) ? g : null;
        }
    }

    public IReadOnlyList<GameInfo> All
    {
        get
        {
            lock (_gate)
            {
                return _games.Values.ToList();
            }
        }
    }

    public void Remove(ulong gid)
    {
        lock (_gate)
        {
            _games.Remove(gid);
        }
    }

    /// <summary>
    /// Найти игру и отметить, что сервер только что был слышен.
    /// Вызывается из обработчиков Blaze-команд — НЕ из HTTP
    /// (/servers иначе «оживлял» бы умершие серверы опросом списка).
    /// </summary>
    public GameInfo? Touch(ulong gid)
    {
        lock (_gate)
        {
            if (_games.TryGetValue(gid, out var g))
            {
                g.Touch();
                return g;
            }

            return null;
        }
    }

    /// <summary>
    /// Убрать серверы, которых давно не слышно (сервер убит/упал).
    /// Возвращает список удалённых GID — для лога.
    /// </summary>
    public IReadOnlyList<ulong> Sweep(TimeSpan idle)
    {
        lock (_gate)
        {
            var dead = _games.Values
                .Where(g => DateTime.UtcNow - g.LastSeenUtc > idle)
                .Select(g => g.GameId)
                .ToList();

            foreach (var gid in dead)
            {
                _games.Remove(gid);
            }

            return dead;
        }
    }

    /// <summary>Игрок отвалился — убираем из всех игр.</summary>
    public void Detach(BlazeSession s)
    {
        foreach (var g in All)
        {
            g.RemovePlayer(s);
        }
    }

    public int TotalPlayers => All.Sum(g => g.PlayerCount);
}