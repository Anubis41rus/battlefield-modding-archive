// BlazeComponents.cs — идентификаторы компонентов и команд Blaze.
//
// Значения компонентов подтверждены строкой «Blaze 3.15.08.0 (CL# 1060080)»
// в New-Blaze-Emulator (Commands\UtilComponent\PreAuthCommand.cs:79) и
// перекрестно сверены с ним же. Это то же значение, что мы реально отдаём
// клиенту (Payloads.cs:38). Раньше здесь стоял неверный номер 944265.
// Идентификаторы команд — из диспетчеров New-Blaze-Emulator
// (Blaze/Components/*.cs): это идентификаторы в протоколе, а не код.

namespace EVBots.Backend.Blaze;

/// <summary>Номера компонентов Blaze (BCP/Blaze 3.x).</summary>
internal static class Component
{
    public const ushort Authentication = 0x0001;
    public const ushort GameManager = 0x0004;
    public const ushort Redirector = 0x0005;
    public const ushort Stats = 0x0007;
    public const ushort Util = 0x0009;
    public const ushort Clubs = 0x000B;
    public const ushort GameReporting = 0x001C;
    public const ushort Rsp = 0x0801;
    public const ushort UserSessions = 0x7802;

    public static string Name(ushort id) => id switch
    {
        Authentication => "Authentication",
        GameManager => "GameManager",
        Redirector => "Redirector",
        Stats => "Stats",
        Util => "Util",
        Clubs => "Clubs",
        GameReporting => "GameReporting",
        Rsp => "RSP",
        UserSessions => "UserSessions",
        _ => $"Component(0x{id:X4})",
    };
}

/// <summary>Коды ошибок Blaze, которые встречаются на пути входа в игру.</summary>
internal static class BlazeError
{
    public const int None = 0;
    public const int Pending = 100;
    public const int AuthFailed = 101;
    public const int CommandNotFound = 1003;
    public const int ServiceUnavailable = 1001;
    public const int NameInUse = 1009;
    public const int GameNotFound = 2001;
    public const int GameFull = 2002;
}

/// <summary>Идентификаторы команд Util-компонента.</summary>
// cmd 2 = Ping, cmd 8 = PostAuth — подтверждено sizes'ом эталонного бэкенда
// (BF3NativeBots.Backend.exe, лог lfl.out): на 0009:0002 он отвечает 9 байтами
// (STIM), а PostAuth-ответ на 0009:0008 — 411 байтами. Раньше эти числа были
// прочитаны как hex («9B» = 155), из-за чего cmd 2 ошибочно объявили PostAuth.
// Сервер bf3_server шлёт PostAuth именно как 0009:0008 — сходится.
internal static class UtilCmd
{
    public const ushort FetchClientConfig = 1;
    public const ushort Ping = 2;
    public const ushort GetTelemetryServer = 5;
    public const ushort PreAuth = 7;
    public const ushort PostAuth = 8;
    public const ushort UserSettingsSave = 0x0B;
    public const ushort UserSettingsLoadAll = 0x0C;
    public const ushort SetClientMetrics = 0x16;
}

/// <summary>Идентификаторы команд Authentication-компонента.</summary>
internal static class AuthCmd
{
    public const ushort Login = 0x28;
    public const ushort SilentLogin = 0x32;
    public const ushort LoginPersona = 0x6E;
    public const ushort ListUserEntitlements2 = 0x1D;
}

/// <summary>Идентификаторы команд Stats-компонента.</summary>
internal static class StatsCmd
{
    public const ushort GetStatGroup = 4;
    public const ushort GetStatsByGroupAsync = 0x10;
    public const ushort StatsAsyncNotification = 0x32;
}

/// <summary>Идентификаторы команд Redirector-компонента.</summary>
internal static class RedirectorCmd
{
    public const ushort GetServerInstance = 1;
}

/// <summary>Идентификаторы команд UserSessions-компонента.</summary>
internal static class UserSessionsCmd
{
    public const ushort UpdateNetworkInfo = 0x14;
    public const ushort UserSessionExtendedDataUpdate = 1;
    public const ushort UserAddedNotification = 2;
    public const ushort UserUpdatedNotification = 5;
}

/// <summary>
/// Идентификаторы УВЕДОМЛЕНИЙ GameManager. Важно: они отличаются от номеров
/// команд-запросов. Значения взяты из протокола.
/// </summary>
internal static class GameManagerNotify
{
    public const ushort GameSetup = 0x14;
    public const ushort PlayerJoining = 0x15;
    public const ushort InitiateConnections = 0x16;
    public const ushort PlayerClaimingReservation = 0x19;
    public const ushort PlayerJoinCompleted = 0x1E;
    public const ushort PlayerRemoved = 0x28;
    public const ushort GameStateChange = 0x64;
    public const ushort GameSettingsChange = 0x6E;

    /// <summary>4:116 — состояние игрока изменилось (GID, PID, STAT).</summary>
    public const ushort PlayerStateChanged = 0x74;
}

/// <summary>Идентификаторы команд GameManager-компонента.</summary>
internal static class GameManagerCmd
{
    public const ushort CreateGame = 1;
    public const ushort DestroyGame = 2;
    public const ushort AdvanceGameState = 3;
    public const ushort SetGameSettings = 4;
    public const ushort SetPlayerCapacity = 5;
    public const ushort SetGameAttributes = 7;
    public const ushort JoinGame = 9;
    public const ushort RemovePlayer = 0x0B;
    public const ushort FinalizeGameCreation = 0x0F;
    /// <summary>4:19 — конец раунда, игра POST_GAME → PRE_GAME (рабочий бэкенд: router.On(4, 19, HandleReplayGame)).</summary>
    public const ushort ReplayGame = 0x13;
    public const ushort UpdateMeshConnection = 0x1D;
}