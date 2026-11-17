using EVBots.Core;

namespace EVBots.Gui;

/// <summary>Строка списка серверов: локальный (бэкенд этого ПК) или сетевой (объявление LAN).</summary>
public sealed class ServerRow
{
    public string Title { get; init; } = "";
    public string Details { get; init; } = "";
    public string Address { get; init; } = "";
    public string Badge { get; init; } = "";
    public bool IsLocal { get; init; }
    public string Ip { get; init; } = "127.0.0.1";
    public int HttpPort { get; init; } = Ports.Http;
    public int GamePort { get; init; } = Ports.Game;
    public ulong GameId { get; init; }

    /// <summary>Подпись содержимого: список перерисовываем только при изменении.</summary>
    public string Signature => $"{Title}|{Details}|{Address}|{Badge}|{Ip}|{HttpPort}|{GamePort}|{GameId}";
}

/// <summary>Строка таблицы аккаунтов этого ПК.</summary>
public sealed class AccountRow
{
    public string Login { get; init; } = "";
    public string Personaref { get; init; } = "";
    public string Soldier { get; init; } = "";
    public string Soldiers { get; init; } = "";
}
