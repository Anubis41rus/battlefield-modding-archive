// Настройки друга: логин/пароль/ник, карта, режим, боты — рядом с exe.
// Пароль ХРАНИМ (друг просил не вводить каждый раз). Файл только на его ПК.
using System.IO;
using System.Text.Json;

namespace EVBots.Launcher;

/// <summary>Тикеты/очки и время раунда одного режима, в процентах от стандарта BF3 (100 = как в игре).</summary>
internal sealed class ModeRule
{
    public int Tickets { get; set; } = 100;
    public int Time { get; set; } = 100;
}

internal sealed class FriendSettings
{
    public string Login { get; set; } = "";
    public string Password { get; set; } = "";
    public string Nick { get; set; } = "";
    public string ModProfileId { get; set; } = "vu-battleroyale";
    public string Map { get; set; } = "MP_001";
    public string Mode { get; set; } = "ConquestSmall0";
    public int Bots { get; set; } = 8;

    /// <summary>22.10: боты садятся в самолёты (мод fun-bots, UseJets; ставится RCON-командой мода, файлы мода не трогаем).</summary>
    public bool BotsJets { get; set; } = true;

    /// <summary>01.11: сложность ботов 0..4 (очень легко … очень сложно); 2 — значения мода по умолчанию. Через RCON мода.</summary>
    public int BotsSkill { get; set; } = 2;

    /// <summary>01.11: боты охраняют захваченные точки (мод fun-bots DefendObjectives, через RCON). Выкл. — после захвата всех точек ходят по путям.</summary>
    public bool BotsDefend { get; set; } = true;

    /// <summary>01.11: меню F12 мода на русском (ModPatch: файлы перевода в моде этого ПК).</summary>
    public bool RussianMenu { get; set; } = true;

    /// <summary>
    /// 26.10: частота движка сервера, Гц: 30 (обычная), 60 или 120 — как у VU (-high60/-high120). Клиенты друзей
    /// берут её из списка серверов бэкенда. Меняется перезапуском сервера.
    /// </summary>
    public int Frequency { get; set; } = 30;
    public string HostIp { get; set; } = "127.0.0.1";
    public string ServerName { get; set; } = "EVBots";

    /// <summary>Карты по кругу (MapList): строки «карта режим раундов». Пусто — одна карта Map/Mode.</summary>
    public List<string> Rotation { get; set; } = new();

    /// <summary>Раундов на карту при добавлении в список.</summary>
    public int Rounds { get; set; } = 1;

    /// <summary>Папка EVBots, если друг выбрал её вручную (пусто = искать автоматически).</summary>
    public string StandDir { get; set; } = "";

    /// <summary>
    /// 20.10: правила по режимам: тикеты/очки (vars.gameModeCounter, %) и время раунда (vars.roundTimeLimit, %).
    /// Ключ — режим (ConquestSmall0, TeamDeathMatch0, …). Нет записи — 100/100, как у BF3 по умолчанию.
    /// </summary>
    public Dictionary<string, ModeRule> ModeRules { get; set; } = new();

    public ModeRule RuleFor(string mode) => ModeRules.TryGetValue(mode, out var r) ? r : new ModeRule();

    private static string FilePath =>
        Path.Combine(PathFinder.ExeDir, "friend-settings.json");

    private static readonly JsonSerializerOptions Opts = new()
    {
        WriteIndented = true,
        PropertyNameCaseInsensitive = true,
    };

    public static FriendSettings Load()
    {
        try
        {
            if (File.Exists(FilePath))
            {
                var s = JsonSerializer.Deserialize<FriendSettings>(File.ReadAllText(FilePath), Opts);
                if (s is not null) return s;
            }
        }
        catch (Exception) { }
        return new FriendSettings();
    }

    public void Save()
    {
        try { File.WriteAllText(FilePath, JsonSerializer.Serialize(this, Opts)); }
        catch (Exception) { }
    }
}
