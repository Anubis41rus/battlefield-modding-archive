// Payloads.cs — ответы и уведомления Blaze с НАСТОЯЩИМИ именами полей.
//
// Источник истины: New-Blaze-Emulator\Commands\* и Notifications\* — это
// публичная реализация протокола Blaze 3.x, плюс строки рабочего бэкенда
// BF3NativeBots.Backend.exe (C:\Games\re\backend-utf16.txt), где реальные
// метки встречаются в строках лога:
//
//   [blaze] login MAIL=        [blaze] loginPersona PNAM=
//   [blaze] silentLogin PID=   [blaze] getStatGroup NAME=
//   [blaze] getStatsByGroupAsync EID=    [blaze] listUserEntitlements2 ETAG=
//   [blaze] joinGame GID= GVER= [blaze] getServerInstance CLNT=
//   [blaze] -> notify ... JoiningPlayerInitiateConnections HNET=
//   [blaze-server] advanceGameState GID=
//
// Ничего не выдумывается: каждое поле ниже взято из протокола.

using System.Globalization;
using System.Net;
using EVBots.Backend.Tdf;

namespace EVBots.Backend.Blaze;

/// <summary>Активный член union NetworkAddress (Blaze).</summary>
internal enum AddrMember : byte
{
    XboxClientAddress = 0,
    XboxServerAddress = 1,
    IPPAddress = 2,
    IPPAddress2 = 3,
    IPAddress = 4,
    HostnameAddress = 5,
    Unset = 0x7F,
}

/// <summary>Версия протокола, которую мы сообщаем клиенту.</summary>
internal static class Protocol
{
    public const string BlazeVersion = "Blaze 3.15.08.0 (CL# 1060080)";
    public const string GameType = "frostbite_multiplayer";
    public const string Platform = "pc";
    public const string QosServer = "ams";
    public const string HostId = "300294";

    /// <summary>
    /// Идентификаторы компонентов, поддерживаемые клиентом
    /// (PreAuth -> CIDS). Пустой список означает «все», но список из
    /// протокола безопаснее.
    /// </summary>
    public static ulong[] SupportedIds { get; } =
    {
        1, 25, 4, 27, 28, 6, 7, 9, 10, 11,
        30720, 30721, 30722, 30723, 20, 30725, 30726, 2000,
    };
}

internal static class Payloads
{
    // ── утилиты ────────────────────────────────────────────────────────────

    /// <summary>IP-адрес как 32-битное целое (порядок байт как в протоколе).</summary>
    public static ulong IpToUInt(IPAddress ip)
    {
        var b = ip.GetAddressBytes();
        return ((ulong)b[0] << 24) | ((ulong)b[1] << 16) |
               ((ulong)b[2] << 8) | b[3];
    }

    private static TdfList IntList(string label, params ulong[] values) =>
        new(label, TdfType.Integer, values.Cast<object>().ToList());

    private static TdfMap StrMap(string label, params (string Key, string Value)[] pairs)
    {
        var list = new List<(object, object)>(pairs.Length);
        foreach (var (k, v) in pairs)
        {
            list.Add((k, v));
        }

        return new TdfMap(label, TdfType.String, TdfType.String, list);
    }

    private static TdfMap StrIntMap(string label, params (string Key, ulong Value)[] pairs)
    {
        var list = new List<(object, object)>(pairs.Length);
        foreach (var (k, v) in pairs)
        {
            list.Add((k, v));
        }

        return new TdfMap(label, TdfType.String, TdfType.Integer, list);
    }

    private static TdfMap IntIntMap(string label, params (ulong Key, ulong Value)[] pairs)
    {
        var list = new List<(object, object)>(pairs.Length);
        foreach (var (k, v) in pairs)
        {
            list.Add((k, v));
        }

        return new TdfMap(label, TdfType.Integer, TdfType.Integer, list);
    }

    /// <summary>NetworkAddress union: VALU{HOST, IP, PORT}.</summary>
    public static TdfUnion NetworkAddress(string host, IPAddress ip, int port,
        AddrMember member = AddrMember.IPAddress) =>
        new("ADDR", (byte)member, TdfNode.Struct("VALU",
            TdfNode.Str("HOST", host),
            TdfNode.Int("IP", IpToUInt(ip)),
            TdfNode.Int("PORT", port)));

    /// <summary>Внутренний/внешний адрес игрока в union PNET.</summary>
    public static TdfUnion PlayerNetwork(IPAddress extIp, int extPort,
        IPAddress intIp, int intPort) =>
        // IPPairAddress (2): реальный клиент шлёт ADDR[2] в updateNetworkInfo,
        // эталон использует NetworkAddressMember.IPPAirAddress = 2.
        new("PNET", (byte)AddrMember.IPPAddress, TdfNode.Struct("VALU",
            TdfNode.Struct("EXIP",
                TdfNode.Int("IP", IpToUInt(extIp)),
                TdfNode.Int("PORT", extPort)),
            TdfNode.Struct("INIP",
                TdfNode.Int("IP", IpToUInt(intIp)),
                TdfNode.Int("PORT", intPort))));

    // ── Util ────────────────────────────────────────────────────────────────

    /// <summary>
    /// Util::FetchClientConfig -> CONF: карта строк.
    /// Реальные ключи из протокола: Achievements (коды достижений) и
    /// WinCodes (коды условий победы). Пустые значения допустимы.
    /// 20.10: по умолчанию значения пустые. С кодами сервер включает GOS Achievements («service is active»,
    /// fb::GosAchievementsService::FetchClientConfigCb 0x01120AC0: active, если список Achievements или WinCodes
    /// не пуст) и на каждого игрока ходит https://&lt;хост&gt;:443/achievements/personas/… (0x0111A590) — без
    /// интернета это «GosAchievementsGetRequest failed. DirtyHttp error = 1» в логе. Коды — ключ --gos-achievements.
    /// </summary>
    public static IEnumerable<TdfNode> FetchClientConfigReply(bool withCodes) => withCodes
        ? FetchClientConfigWithCodes()
        : new[] { StrMap("CONF", ("Achievements", ""), ("WinCodes", "")) };

    private static IEnumerable<TdfNode> FetchClientConfigWithCodes() => new[]
    {
        StrMap("CONF",
            ("Achievements",
                "ACH32_00,ACH33_00,ACH34_00,ACH35_00,ACH36_00,ACH37_00," +
                "ACH38_00,ACH39_00,ACH40_00,XPACH01_00,XPACH02_00,XPACH03_00," +
                "XPACH04_00,XPACH05_00,XP2ACH01_00,XP2ACH02_00,XP2ACH03_00," +
                "XP2ACH04_00,XP2ACH05_00,XP3ACH01_00,XP3ACH02_00,XP3ACH03_00," +
                "XP3ACH04_00,XP3ACH05_00,XP4ACH01_00,XP4ACH02_00,XP4ACH03_00," +
                "XP4ACH04_00,XP4ACH05_00,XP5ACH01_00,XP5ACH02_00,XP5ACH03_00," +
                "XP5ACH04_00,XP5ACH05_00"),
            ("WinCodes",
                "r01_00,r05_00,r04_00,r03_00,r02_00,r10_00,r08_00,r07_00," +
                "r06_00,r09_00,r11_00,r12_00,r13_00,r14_00,r15_00,r16_00," +
                "r17_00,r18_00,r19_00,r20_00,r21_00,r22_00,r23_00,r24_00," +
                "r25_00,r26_00,r27_00,r28_00,r29_00,r30_00,r31_00,r32_00," +
                "r33_00,r34_00,r35_00,r36_00,r37_00,r38_00,r39_00,r40_00," +
                "r41_00,r42_00,r43_00,r44_00,r45_00")),
    };

    /// <summary>
    /// Util::PreAuth -> состав сервера: CIDS (поддерживаемые компоненты),
    /// CONF (таймауты), INST (имя службы), PLAT, SVER, QOSS.
    /// </summary>
    public static IEnumerable<TdfNode> PreAuthReply(string service,
        IPAddress qosIp, int qosPort) => new TdfNode[]
    {
        TdfNode.Int("ANON", 0),
        TdfNode.Str("ASRC", Protocol.HostId),
        IntList("CIDS", Protocol.SupportedIds),
        TdfNode.Str("CNGN", ""),
        TdfNode.Struct("CONF",
            StrMap("CONF",
                ("connIdleTimeout", "90s"),
                ("defaultRequestTimeout", "80s"),
                ("pingPeriod", "20s"),
                ("voipHeadsetUpdateRate", "1000"),
                ("xlspConnectionIdleTimeout", "300"))),
        TdfNode.Str("INST", service),
        TdfNode.Int("MINR", 0),
        TdfNode.Str("NASP", "cem_ea_id"),
        TdfNode.Str("PILD", ""),
        TdfNode.Str("PLAT", Protocol.Platform),
        TdfNode.Str("PTAG", ""),
        TdfNode.Struct("QOSS",
            TdfNode.Struct("BWPS",
                TdfNode.Str("PSA", qosIp.ToString()),
                TdfNode.Int("PSP", qosPort),
                TdfNode.Str("SNA", Protocol.QosServer)),
            TdfNode.Int("LNP", 10),
            TdfNode.Map("LTPS", TdfType.String, TdfType.Struct,
                new List<(object, object)>
                {
                    (Protocol.QosServer, (object)new List<TdfNode>
                    {
                        TdfNode.Str("PSA", qosIp.ToString()),
                        TdfNode.Int("PSP", qosPort),
                        TdfNode.Str("SNA", Protocol.QosServer),
                    }),
                }),
            TdfNode.Int("SVID", 1161889797)),
        TdfNode.Str("RSRC", Protocol.HostId),
        TdfNode.Str("SVER", Protocol.BlazeVersion),
    };

    /// <summary>
    /// Util::GetTelemetryServer -> адрес, на который клиент шлёт телеметрию.
    /// Отдаём loopback и пустые фильтры: телеметрия EA нам не нужна.
    /// Поля: ADRS, ANON, DISA, FILT, LOC, NOOK, PORT, SDLY, SESS, SKEY,
    /// SPCT, STIM.
    /// </summary>
    public static IEnumerable<TdfNode> TelemetryReply(ulong localization) => new TdfNode[]
    {
        TdfNode.Str("ADRS", "127.0.0.1"),
        TdfNode.Int("ANON", 0),
        TdfNode.Str("DISA", ""),
        TdfNode.Str("FILT", ""),
        TdfNode.Int("LOC", localization),
        TdfNode.Str("NOOK", ""),
        TdfNode.Int("PORT", 9988),
        TdfNode.Int("SDLY", 15000),
        TdfNode.Str("SESS", ""),
        TdfNode.Str("SKEY", ""),
        TdfNode.Int("SPCT", 0),
        TdfNode.Str("STIM", "Default"),
    };

    /// <summary>Util::PostAuth -> PSS, TELE, TICK, UROP.</summary>
    public static IEnumerable<TdfNode> PostAuthReply(ulong localization,
        ulong userId, IPAddress tickIp, int tickPort) => new TdfNode[]
    {
        TdfNode.Struct("PSS",
            TdfNode.Str("ADRS", "127.0.0.1"),
            TdfNode.Bin("CSIG", []),
            TdfNode.Str("PJID", userId.ToString(CultureInfo.InvariantCulture)),
            TdfNode.Int("PORT", 8443),
            TdfNode.Int("RPRT", 9),
            TdfNode.Int("TIID", 0)),
        TdfNode.Struct("TELE",
            TdfNode.Str("ADRS", "127.0.0.1"),
            TdfNode.Int("ANON", 0),
            TdfNode.Str("DISA", ""),
            TdfNode.Str("FILT", ""),
            TdfNode.Int("LOC", localization),
            TdfNode.Str("NOOK", ""),
            TdfNode.Int("PORT", 9988),
            TdfNode.Int("SDLY", 15000),
            TdfNode.Str("SESS", ""),
            TdfNode.Str("SKEY", ""),
            TdfNode.Int("SPCT", 0),
            TdfNode.Str("STIM", "Default")),
        TdfNode.Struct("TICK",
            TdfNode.Str("ADRS", tickIp.ToString()),
            TdfNode.Int("PORT", tickPort),
            TdfNode.Str("SKEY",
                $"{userId},127.0.0.1:{tickPort},battlefield-3-pc,10,50,50,50,50,0,12")),
        TdfNode.Struct("UROP",
            TdfNode.Int("TMOP", 0),
            TdfNode.Int("UID", userId)),
    };

    /// <summary>Util::Ping -> STIM.</summary>
    public static IEnumerable<TdfNode> PingReply(ulong serverTime) => new TdfNode[]
    {
        TdfNode.Int("STIM", serverTime),
    };

    /// <summary>
    /// Util::UserSettingsLoadAll -> SMAP: карта строк
    /// (настройки игрока приходят строкой, а не блобом).
    /// </summary>
    public static IEnumerable<TdfNode> UserSettingsLoadAllReply(
        IReadOnlyDictionary<string, string> settings)
    {
        var pairs = new List<(object, object)>(settings.Count);
        foreach (var (k, v) in settings)
        {
            pairs.Add((k, v));
        }

        return new TdfNode[]
        {
            new TdfMap("SMAP", TdfType.String, TdfType.String, pairs),
        };
    }

    // ── Authentication ──────────────────────────────────────────────────────

    /// <summary>PersonaData: DSNM, LAST, PID, STAS, XREF, XTYP.</summary>
    private static TdfNode PersonaData(string displayName, ulong personaref,
        ulong lastSeen) =>
        TdfNode.Struct("PDTL",
            TdfNode.Str("DSNM", displayName),
            TdfNode.Int("LAST", lastSeen),
            TdfNode.Int("PID", personaref),
            TdfNode.Int("STAS", 0),
            TdfNode.Int("XREF", 0),
            TdfNode.Int("XTYP", 0));

    /// <summary>Authentication::Login (по почте) -> PLST со списком солдат.</summary>
    public static IEnumerable<TdfNode> LoginReply(ulong sessionId, ulong personaref,
        string displayName, string mail) => new TdfNode[]
    {
        TdfNode.Str("LDHT", ""),
        TdfNode.Int("NTOS", 0),
        TdfNode.Str("PCTK", ""),
        new TdfList("PLST", TdfType.Struct, new List<object>
        {
            (object)new List<TdfNode>
            {
                TdfNode.Str("DSNM", displayName),
                TdfNode.Int("LAST", 0),
                TdfNode.Int("PID", personaref),
                TdfNode.Int("STAS", 2),
                TdfNode.Int("XREF", 0),
                TdfNode.Int("XTYP", 0),
            },
        }),
        TdfNode.Str("PRIV", ""),
        TdfNode.Str("SKEY", ""),
        TdfNode.Int("SPAM", 1),
        TdfNode.Str("THST", ""),
        TdfNode.Str("TSUI", ""),
        TdfNode.Str("TURI", ""),
        TdfNode.Int("UID", sessionId),
    };

    /// <summary>Authentication::LoginPersona -> BUID, FRST, KEY, LLOG, MAIL, PDTL, UID.</summary>
    public static IEnumerable<TdfNode> LoginPersonaReply(ulong sessionId,
        ulong personaref, string displayName, string mail) => new TdfNode[]
    {
        TdfNode.Int("BUID", personaref),
        TdfNode.Int("FRST", 0),
        TdfNode.Str("KEY", ""),
        TdfNode.Int("LLOG", 0),
        TdfNode.Str("MAIL", mail),
        PersonaData(displayName, personaref, 0),
        TdfNode.Int("UID", sessionId),
    };

    /// <summary>Authentication::SilentLogin -> AGUP и SESS.</summary>
    public static IEnumerable<TdfNode> SilentLoginReply(ulong personaref,
        string displayName, string mail) => new TdfNode[]
    {
        TdfNode.Int("AGUP", 0),
        TdfNode.Str("LDHT", ""),
        TdfNode.Int("NTOS", 0),
        TdfNode.Str("PCTK", ""),
        TdfNode.Str("PRIV", ""),
        TdfNode.Struct("SESS",
            TdfNode.Int("BUID", personaref),
            TdfNode.Int("FRST", 0),
            TdfNode.Str("KEY", ""),
            TdfNode.Int("LLOG", 0),
            TdfNode.Str("MAIL", mail),
            PersonaData(displayName, personaref, 0),
            TdfNode.Int("UID", personaref)),
        TdfNode.Int("SPAM", 0),
        TdfNode.Str("THST", ""),
        TdfNode.Str("TSUI", ""),
        TdfNode.Str("TURI", ""),
    };

    /// <summary>
    /// Authentication::ListUserEntitlements2 -> NLST: список прав.
    /// Поля: DEVI, GDAY, GNAM, ID, ISCO, PID, PJID, PRCA, PRID, STAT,
    /// STRC, TAG, TDAY, TYPE, UCNT, VER.
    ///
    /// TAG = "ONLINE_ACCESS" даёт клиенту право зайти на сервер без
    /// проверки EA. Остальные права (DLC) пустые: у нас своя экономика.
    /// </summary>
    public static IEnumerable<TdfNode> EntitlementsReply() => new TdfNode[]
    {
        new TdfList("NLST", TdfType.Struct, new List<object>
        {
            (object)new List<TdfNode>
            {
                TdfNode.Str("DEVI", ""),
                TdfNode.Str("GDAY", ""),
                TdfNode.Str("GNAM", "BF3PC"),
                TdfNode.Int("ID", 1),
                TdfNode.Int("ISCO", 0),
                TdfNode.Int("PID", 0),
                TdfNode.Str("PJID", ""),
                TdfNode.Int("PRCA", 2),
                TdfNode.Str("PRID", ""),
                TdfNode.Int("STAT", 1),
                TdfNode.Int("STRC", 0),
                TdfNode.Str("TAG", "ONLINE_ACCESS"),
                TdfNode.Str("TDAY", ""),
                TdfNode.Int("TYPE", 1),
                TdfNode.Int("UCNT", 0),
                TdfNode.Int("VER", 0),
            },
        }),
    };

    /// <summary>
    /// Прогресс-набор: ровно те 58 entitlement'ов, которые отдаёт рабочий
    /// бэкенд при "progress on" (QueryGeneral без shortcut-паков).
    ///
    /// Без этого списка клиент НЕ запрашивает getStatsByGroupAsync и не
    /// рисует ранг — проверено на живом прогоне (см. MODLOG).
    /// </summary>
    public static readonly (string Tag, string Group)[] ProgressEntitlements =
    [
        ("PROJECT10_CODE_CONSUMED_LE1", "BF3PC"),
        ("BF3:PC:B2K_PURCHASE", "BF3PC"),
        ("BF3:PC:B2K_CODE_CONSUMED", "BF3PC"),
        ("BF3:PROMO:PWP", "BF3PC"),
        ("BF3:PROMO:SPECA", "BF3PC"),
        ("BF3:PROMO:PEPPER1", "BF3PC"),
        ("BF3:PROMO:PEPPER2", "BF3PC"),
        ("BF3:PROMO:PEPPER3", "BF3PC"),
        ("BF3:PROMO:PEPPER4", "BF3PC"),
        ("BF3:PROMO:PEPPER5", "BF3PC"),
        ("BF3:PROMO:PEPPER6", "BF3PC"),
        ("BF3:PROMO:DOGTAGS", "BF3PC"),
        ("BF3:PROMO:ALIENWARE", "BF3PC"),
        ("BF3:PROMO:RAZER", "BF3PC"),
        ("BF3:PROMO:TOURNAMENT1", "BF3PC"),
        ("BF3:PROMO:TOURNAMENT2", "BF3PC"),
        ("BF3:PROMO:TOURNAMENT3", "BF3PC"),
        ("BF3:PROMO:VALOR", "BF3PC"),
        ("BF3MOBILE:DOGTAG", "Battlefield3"),
        ("BF1942:PC:ADDSVETRANK", "AddsVetRank"),
        ("BF2:PC:ADDSVETRANK", "AddsVetRank"),
        ("BF2142:PC:ADDSVETRANK", "AddsVetRank"),
        ("BFVIETNAM:PC:ADDSVETRANK", "AddsVetRank"),
        ("BFBC:PS3:ADDSVETRANK", "AddsVetRank"),
        ("BFBC:360:ADDSVETRANK", "AddsVetRank"),
        ("BFBCGOLD:PS3:ADDSVETRANK", "AddsVetRank"),
        ("BFBCGOLD:360:ADDSVETRANK", "AddsVetRank"),
        ("BF1943:PS3:ADDSVETRANK", "AddsVetRank"),
        ("BF1943:360:ADDSVETRANK", "AddsVetRank"),
        ("BFBC2:PC:ADDSVETRANK", "AddsVetRank"),
        ("BFBC2:PS3:ADDSVETRANK", "AddsVetRank"),
        ("BFBC2:360:ADDSVETRANK", "AddsVetRank"),
        ("BFP4F:PC:ADDSVETRANK", "AddsVetRank"),
        ("BFHEROES:PC:ADDSVETRANK", "AddsVetRank"),
        ("BFMC:PS2:ADDSVETRANK", "AddsVetRank"),
        ("BFMC:XBOX:ADDSVETRANK", "AddsVetRank"),
        ("BFMC:360:ADDSVETRANK", "AddsVetRank"),
        ("BF3MOBILE:MOBILE:ADDSVETRANK", "AddsVetRank"),
        ("GUNCLUB:WEB:MEMBER", "GunClub"),
        ("DICE", "Battlefield3"),
        ("POSH", "Battlefield3"),
        ("BF3:PREMIUM_ACCESS", "BF3PC"),
        ("BF3:PC:XPACK2_PURCHASE", "BF3PC"),
        ("BF3:DICE:COMMUNITY", "BF3PC"),
        ("BF3:PC:XPACK3_PURCHASE", "BF3PC"),
        ("BF3:DICE:XP3PROMO1", "BF3PC"),
        ("BF3:DICE:XP3PROMO2", "BF3PC"),
        ("BF3:DICE:XP3PROMO3", "BF3PC"),
        ("BF3:PC:XPACK4_PURCHASE", "BF3PC"),
        ("BF3:DICE:XP4PROMO1", "BF3PC"),
        ("BF3:DICE:XP4PROMO2", "BF3PC"),
        ("BF3:DICE:XP4PROMO3", "BF3PC"),
        ("BF3:DICE:XP4PROMO4", "BF3PC"),
        ("BF3:DICE:XP4PROMO5", "BF3PC"),
        ("BF3:PC:XPACK5_PURCHASE", "BF3PC"),
        ("BF3:DICE:XP5ENLISTED", "BF3PC"),
        ("BF3:DICE:XP5IWASTHERE", "BF3PC"),
        ("BF3:DICE:XP5PVZ", "BF3PC"),
    ];

    /// <summary>Ответ listUserEntitlements2 с прогресс-списком (58 штук).</summary>
    public static IEnumerable<TdfNode> EntitlementsProgressReply()
    {
        var items = new List<object>(ProgressEntitlements.Length);
        for (var i = 0; i < ProgressEntitlements.Length; i++)
        {
            var (tag, group) = ProgressEntitlements[i];
            items.Add(new List<TdfNode>
            {
                TdfNode.Str("DEVI", ""),
                TdfNode.Str("GDAY", ""),
                TdfNode.Str("GNAM", group),
                TdfNode.Int("ID", i + 1),
                TdfNode.Int("ISCO", 0),
                TdfNode.Int("PID", 0),
                TdfNode.Str("PJID", ""),
                TdfNode.Int("PRCA", 2),
                TdfNode.Str("PRID", ""),
                TdfNode.Int("STAT", 1),
                TdfNode.Int("STRC", 0),
                TdfNode.Str("TAG", tag),
                TdfNode.Str("TDAY", ""),
                // TYPE=1 только у PROJECT10 (как в Granted рабочего), 5 — у прочих
                TdfNode.Int("TYPE", tag == "PROJECT10_CODE_CONSUMED_LE1" ? 1 : 5),
                TdfNode.Int("UCNT", 0),
                TdfNode.Int("VER", 0),
            });
        }

        return new TdfNode[]
        {
            new TdfList("NLST", TdfType.Struct, items),
        };
    }

    // ── уведомления UserSessions ────────────────────────────────────────────

    /// <summary>
    /// UserSessions уведомление 2 (UserAdded): DATA + USER{AID,ALOC,ID,NAME}.
    /// DMAP и QDAT — то, что сервер сообщает о клиенте.
    /// </summary>
    public static IEnumerable<TdfNode> UserAdded(ulong userId, string userName,
        ulong localization) => new TdfNode[]
    {
        TdfNode.Struct("DATA",
            new TdfUnion("ADDR", (byte)AddrMember.Unset, null),
            TdfNode.Str("BPS", Protocol.QosServer),
            TdfNode.Str("CTY", ""),
            IntIntMap("DMAP", (0x70001, 55), (0x70002, 707)),
            TdfNode.Int("HWFG", 0),
            TdfNode.Struct("QDAT",
                TdfNode.Int("DBPS", 0),
                TdfNode.Int("NATT", 0),
                TdfNode.Int("UBPS", 0)),
            TdfNode.Int("UATT", 0)),
        TdfNode.Struct("USER",
            TdfNode.Int("AID", userId),
            TdfNode.Int("ALOC", localization),
            TdfNode.Int("ID", userId),
            TdfNode.Str("NAME", userName)),
    };

    /// <summary>UserSessions уведомление 5 (UserUpdated): FLGS, ID.</summary>
    public static IEnumerable<TdfNode> UserUpdated(ulong userId) => new TdfNode[]
    {
        TdfNode.Int("FLGS", 3),
        TdfNode.Int("ID", userId),
    };

    /// <summary>
    /// UserSessions уведомление 1 (расширенные данные сессии): DATA + USID.
    /// Здесь клиент узнаёт наш IP и порт — по сути адрес игрового сервера.
    /// </summary>
    public static IEnumerable<TdfNode> SessionExtended(ulong sessionUserId,
        IPAddress ip, int port, ulong gameId, bool withGameList) => new TdfNode[]
    {
        TdfNode.Struct("DATA",
            NetworkAddress(ip.ToString(), ip, port),
            TdfNode.Str("BPS", Protocol.QosServer),
            TdfNode.Str("CTY", ""),
            TdfNode.Int("HWFG", 0),
            IntList("PSLM", 268374015, 268374015, 268374015, 268374015, 268374015),
            TdfNode.Int("UATT", 0)),
        TdfNode.Int("USID", sessionUserId),
    };

    // ── Redirector ──────────────────────────────────────────────────────────

    /// <summary>
    /// Redirector::GetServerInstance -> ADDR (union c VALU{HOST,IP,PORT}),
    /// AMAP, MSGS, NMAP (пустые списки), SECU, XDNS.
    ///
    /// Структура снята байт-в-байт с рабочего бэкенда (09.10.2026) и
    /// проверена нашим же декодером: без пустых AMAP/MSGS/NMAP клиент
    /// ответ не принимает. IP=0 — клиент идёт по HOST (DNS-редирект).
    /// </summary>
    public static IEnumerable<TdfNode> ServerInstanceReply(IPAddress ip, int port,
        string host) => new TdfNode[]
    {
        new TdfUnion("ADDR", (byte)AddrMember.XboxClientAddress,
            TdfNode.Struct("VALU",
                TdfNode.Str("HOST", host),
                TdfNode.Int("IP", 0),
                TdfNode.Int("PORT", port))),
        TdfNode.ListOf("AMAP", TdfType.Struct),
        TdfNode.ListOf("MSGS", TdfType.String),
        TdfNode.ListOf("NMAP", TdfType.Struct),
        TdfNode.Int("SECU", 1),
        TdfNode.Int("XDNS", 0),
    };

    // ── GameManager ─────────────────────────────────────────────────────────

    /// <summary>GameManager::CreateGame -> GID.</summary>
    public static IEnumerable<TdfNode> CreateGameReply(ulong gameId) => new TdfNode[]
    {
        TdfNode.Int("GID", gameId),
    };

    /// <summary>GameManager::FinalizeGameCreation -> GID.</summary>
    public static IEnumerable<TdfNode> FinalizeGameReply(ulong gameId) => new TdfNode[]
    {
        TdfNode.Int("GID", gameId),
    };

    /// <summary>GameManager::JoinGame -> GID, JGS.</summary>
    public static IEnumerable<TdfNode> JoinGameReply(ulong gameId) => new TdfNode[]
    {
        TdfNode.Int("GID", gameId),
        TdfNode.Int("JGS", 0),
    };

    /// <summary>GameManager уведомление 0x64: смена состояния игры.</summary>
    public static IEnumerable<TdfNode> GameStateChange(ulong gameId,
        ulong state) => new TdfNode[]
    {
        TdfNode.Int("GID", gameId),
        TdfNode.Int("GSTA", state),
    };

    /// <summary>Данные игрока в уведомлениях матча (PDAT / PROS).</summary>
    public static List<TdfNode> PlayerData(ulong gameId, ulong personaref,
        string name, ulong localization, int slot,
        ulong state, IPAddress ip, int port, bool withBlob)
    {
        var items = new List<TdfNode>();

        if (withBlob)
        {
            items.Add(TdfNode.Bin("BLOB", []));
        }

        items.Add(TdfNode.Int("EXID", 0));
        items.Add(TdfNode.Int("GID", gameId));
        items.Add(TdfNode.Int("LOC", localization));
        items.Add(TdfNode.Str("NAME", name));
        // ключ/значение в нижнем регистре — как в рабочем бэкенде
        items.Add(StrMap("PATT", ("premium", "false")));
        items.Add(TdfNode.Int("PID", personaref));
        items.Add(PlayerNetwork(ip, port, ip, port));
        items.Add(TdfNode.Int("SID", (ulong)slot));
        items.Add(TdfNode.Int("SLOT", 0));
        items.Add(TdfNode.Int("STAT", state));
        items.Add(TdfNode.Int("TIDX", 65535));
        items.Add(TdfNode.Int("TIME", 0));
        items.Add(TdfNode.ObjectId("UGID", 0, 0, 0));
        items.Add(TdfNode.Int("UID", personaref));

        return items;
    }

    /// <summary>
    /// GameManager уведомление 0x15 (PlayerJoining) и 0x19
    /// (PlayerClaimingReservation): отличаются только значением STAT.
    /// STAT = 0 — игрок заходит, STAT = 2 — занял слот.
    /// </summary>
    public static IEnumerable<TdfNode> PlayerJoining(ulong gameId,
        ulong personaref, string name, ulong localization,
        int slot, ulong state, IPAddress ip, int port) => new TdfNode[]
    {
        TdfNode.Int("GID", gameId),
        TdfNode.Struct("PDAT",
            PlayerData(gameId, personaref, name, localization, slot,
                state, ip, port, withBlob: false).ToArray()),
    };

    /// <summary>
    /// GameManager уведомление 4:116 (PlayerStateChanged): {GID, PID, STAT}.
    /// Рабочий бэкенд шлёт его отправителю 0x1D сразу после респонса,
    /// когда STAT=2 — клиент ждёт свой state=4 (Connected), прежде чем
    /// перейти к загрузке карты и подключению к игровому порту.
    /// </summary>
    public static IEnumerable<TdfNode> PlayerStateChanged(ulong gameId,
        ulong pid, ulong state) => new TdfNode[]
    {
        TdfNode.Int("GID", gameId),
        TdfNode.Int("PID", pid),
        TdfNode.Int("STAT", state),
    };

    /// <summary>GameManager уведомление 4:30 (PlayerJoinCompleted): {GID, PID}.</summary>
    public static IEnumerable<TdfNode> PlayerJoinCompleted(ulong gameId,
        ulong pid) => new TdfNode[]
    {
        TdfNode.Int("GID", gameId),
        TdfNode.Int("PID", pid),
    };

    /// <summary>
    /// GameManager уведомление 4:40 (PlayerRemoved): {CNTX, GID, PID, REAS}
    /// — зеркало BuildPlayerRemoved рабочего бэкенда (REAS: 1 — отключился,
    /// 2 — кикнули).
    /// </summary>
    public static IEnumerable<TdfNode> PlayerRemoved(ulong gameId,
        ulong pid, ulong reason) => new TdfNode[]
    {
        TdfNode.Int("CNTX", 0),
        TdfNode.Int("GID", gameId),
        TdfNode.Int("PID", pid),
        TdfNode.Int("REAS", reason),
    };

    /// <summary>
    /// GameManager уведомления 0x14 (GameSetup, серверу) и 0x16
    /// (JoiningPlayerInitiateConnections, клиенту) — общее зеркало
    /// BuildGameSetup рабочего бэкенда BF3NativeBots (декомпиляция,
    /// формат подтверждён живым клиентом): GAME/PROS/QUEU/REAS-union.
    /// </summary>
    public static IEnumerable<TdfNode> GameSetupNotification(ulong gameId,
        ulong hostPid, string gameName, int maxPlayers, ulong gameState,
        IPAddress ip, int port, ulong joinerPid, int joinerSlot,
        IReadOnlyDictionary<string, string> attributes,
        string uuid, string pgid, ulong seed, ulong gset, int dctx,
        List<object>? prosPlayers) => new TdfNode[]
    {
        TdfNode.Struct("GAME",
            // ADMN — как в рабочем BuildGame: HostPersonaId
            IntList("ADMN", hostPid),
            StrMap("ATTR", attributes.Select(kv =>
                (kv.Key, kv.Value)).ToArray()),
            // CAP = [MaxPlayers, 0] — как в рабочем бэкенде
            IntList("CAP", (ulong)maxPlayers, 0UL),
            TdfNode.Int("GID", gameId),
            TdfNode.Str("GNAM", gameName),
            TdfNode.Int("GPVH", 666),
            // GSET стоит ПЕРЕД GSID — строгий алфавитный порядок TDF
            TdfNode.Int("GSET", gset),
            TdfNode.Int("GSID", 1),
            TdfNode.Int("GSTA", gameState),
            TdfNode.Str("GTYP", Protocol.GameType),
            HnetListClient(ip, port),
            TdfNode.Int("HSES", 13666),
            TdfNode.Int("IGNO", 0),
            TdfNode.Int("MCAP", maxPlayers),
            // NQOS (не NQQS), NATT=4 — как в рабочем бэкенде
            TdfNode.Struct("NQOS",
                TdfNode.Int("DBPS", 0),
                TdfNode.Int("NATT", 4),
                TdfNode.Int("UBPS", 0)),
            TdfNode.Int("NRES", 1),
            TdfNode.Int("NTOP", 1),
            TdfNode.Str("PGID", pgid),
            TdfNode.Bin("PGSR", []),
            // PHST — для 0x16: зашедший игрок; для 0x14: хост
            TdfNode.Struct("PHST",
                TdfNode.Int("HPID", joinerPid != 0 ? joinerPid : hostPid),
                TdfNode.Int("HSLT", joinerPid != 0 ? joinerSlot : 0)),
            TdfNode.Int("PRES", 1),
            TdfNode.Str("PSAS", Protocol.QosServer),
            TdfNode.Int("QCAP", 0),
            TdfNode.Int("SEED", seed),
            TdfNode.Int("TCAP", 0),
            TdfNode.Struct("THST",
                TdfNode.Int("HPID", hostPid),
                TdfNode.Int("HSLT", 0)),
            TdfNode.Str("UUID", uuid),
            TdfNode.Int("VOIP", 0),
            TdfNode.Str("VSTR", "67"),
            TdfNode.Bin("XNNC", []),
            TdfNode.Bin("XSES", [])),
        new TdfList("PROS", TdfType.Struct, prosPlayers ?? new List<object>()),
        new TdfList("QUEU", TdfType.Struct, []),
        new TdfUnion("REAS", 0,
            TdfNode.Struct("VALU", TdfNode.Int("DCTX", dctx))),
    };

    /// <summary>
    /// HNET — список IPPairAddress-union: на проводе itemType ВСЕГДА 03
    /// (WireElementType маппит Union→Struct), элемент = байт member=02
    /// (IPPairAddress) + «голая» структура с полями EXIP/INIP + терминатор.
    /// Байт «02» — это НЕ стаб, а номер члена union'а. Снято с рабочего
    /// бэкенда (TdfWriter.WriteList + WireElementType) и сверено байт-в-байт
    /// с createGame реального bf3-сервера.
    /// Рабочий бэкенд шлёт EXIP = INIP = реальный адрес игры (IpPair).
    /// </summary>
    public static TdfList HnetListClient(IPAddress ip, int port)
    {
        var u = IpToUInt(ip);

        var exip = TdfNode.Struct("EXIP", TdfNode.Int("IP", u), TdfNode.Int("PORT", port));
        var inip = TdfNode.Struct("INIP", TdfNode.Int("IP", u), TdfNode.Int("PORT", port));

        return new TdfList("HNET", TdfType.Struct, new List<object>
        {
            (object)new List<TdfNode> { exip, inip },
        }, hnetStub: true);
    }
}