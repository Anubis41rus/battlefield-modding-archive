// Карты и режимы из mapfiles мода fun-bots + русские названия.
using System.IO;

namespace EVBots.Launcher;

internal sealed record MapMode(string Map, string Mode);

internal static class GameData
{
    public static List<MapMode> ForProfile(ModProfile profile, string funBotsDir) => profile.IsRoyale
        ? new List<MapMode> { new(ModProfiles.RoyaleMap, ModProfiles.RoyaleMode) }
        : Available(funBotsDir);

    public static readonly Dictionary<string, string> MapNames = new()
    {
        ["MP_001"] = "Гранд Базар", ["MP_003"] = "Тегеранское шоссе",
        ["MP_007"] = "Каспийская граница", ["MP_011"] = "Переправа через Сену",
        ["MP_012"] = "Огненный шторм", ["MP_013"] = "Пик Дамаванд",
        ["MP_017"] = "Каналы Ношахра", ["MP_018"] = "Остров Харг",
        ["MP_Subway"] = "Метро",
        ["XP1_001"] = "Удар по Каркенду", ["XP1_002"] = "Оманский залив",
        ["XP1_003"] = "Полуостров Шарки", ["XP1_004"] = "Остров Уэйк",
        ["XP2_Factory"] = "Металлолом", ["XP2_Office"] = "Операция 925",
        ["XP2_Palace"] = "Крепость Донья", ["XP2_Skybar"] = "Башня Зиба",
        ["XP3_Alborz"] = "Горы Альборз", ["XP3_Desert"] = "Пустыня Бандар",
        ["XP3_Shield"] = "Бронированный щит", ["XP3_Valley"] = "Долина смерти",
        ["XP4_FD"] = "Монолит Марказ", ["XP4_Parl"] = "Дворец Азади",
        ["XP4_Quake"] = "Эпицентр", ["XP4_Rubble"] = "Рынок Талах",
        ["XP5_001"] = "Риверсайд", ["XP5_002"] = "Плато Небандан",
        ["XP5_003"] = "Железная дорога", ["XP5_004"] = "Трубопровод Сабалан",
    };

    public static readonly string[] KnownModes = new[]
    {
        "ConquestSmall0", "ConquestLarge0", "RushLarge0", "SquadRush0",
        "SquadDeathMatch0", "TeamDeathMatch0", "GunMaster0", "BFLAG",
        "CaptureTheFlag0", "Domination0", "AirSuperiority0", "TankSuperiority0",
        "Scavenger0", "ConquestAssaultSmall0", "ConquestAssaultLarge0",
        "ConquestAssaultSmall1",
    };

    public static readonly Dictionary<string, string> ModeNames = new()
    {
        ["ConquestSmall0"] = "Захват", ["ConquestLarge0"] = "Захват (большой)",
        ["ConquestAssaultSmall0"] = "Штурм-захват", ["ConquestAssaultSmall1"] = "Штурм-захват 2",
        ["ConquestAssaultLarge0"] = "Штурм-захват (большой)", ["RushLarge0"] = "Штурм",
        ["SquadRush0"] = "Штурм отрядов", ["SquadDeathMatch0"] = "Схватка отрядов",
        ["TeamDeathMatch0"] = "Командный бой", ["GunMaster0"] = "Мастер оружия (Gun Master)",
        ["BFLAG"] = "BFLAG", ["CaptureTheFlag0"] = "Захват флага", ["Domination0"] = "Превосходство",
        ["AirSuperiority0"] = "Превосходство в воздухе", ["TankSuperiority0"] = "Танковое превосходство",
        ["Scavenger0"] = "Мусорщик (Scavenger)",
    };

    public static string MapLabel(string map) =>
        MapNames.TryGetValue(map, out var n) ? $"{map} — {n}" : map;

    public static string ModeLabel(string mode) =>
        ModeNames.TryGetValue(mode, out var n) ? $"{n} — {mode}" : mode;

    // 19.10: сначала режим, потом карты под него (просьба пользователя: список карт был общий для всех режимов).
    public static List<string> AllModes(List<MapMode> all) =>
        all.Select(x => x.Mode).Distinct().OrderBy(x => Array.IndexOf(KnownModes, x)).ToList();

    public static List<string> MapsForMode(List<MapMode> all, string mode) =>
        all.Where(x => x.Mode == mode).Select(x => x.Map).Distinct().OrderBy(x => x).ToList();

    public static List<MapMode> Available(string funBotsDir)
    {
        var list = new List<MapMode>();
        var dir = Path.Combine(funBotsDir, "mapfiles");
        if (!Directory.Exists(dir)) return list;
        foreach (var f in Directory.GetFiles(dir, "*.map"))
        {
            var b = Path.GetFileNameWithoutExtension(f);
            foreach (var m in KnownModes)
            {
                if (!b.EndsWith("_" + m)) continue;
                var map = b[..(b.Length - m.Length - 1)];
                if (!list.Any(x => x.Map == map && x.Mode == m))
                    list.Add(new MapMode(map, m));
                break;
            }
        }
        list.Sort((a, b) => string.Compare(a.Map + a.Mode, b.Map + b.Mode, StringComparison.Ordinal));
        return list;
    }

    public static List<string> MapsFor(List<MapMode> all) =>
        all.Select(x => x.Map).Distinct().OrderBy(x => x).ToList();

    public static List<string> ModesFor(List<MapMode> all, string map) =>
        all.Where(x => x.Map == map).Select(x => x.Mode).Distinct().OrderBy(x => x).ToList();
}
