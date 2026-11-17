using System.Text.Json;

namespace EVBots.Core;

/// <summary>
/// Настройки лаунчера. Хранятся в launcher-settings.json рядом с exe.
/// Пароли сюда НЕ пишутся: пароль аккаунта вводится в окне входа каждый раз.
/// </summary>
public sealed class LauncherSettings
{
    // --- сервер (значения по умолчанию = текущий стенд) ---
    public string ServerName { get; set; } = "EVBots";
    public int MaxPlayers { get; set; } = 64;

    /// <summary>InitNumberOfBots в Admin\Mods\fun-bots\ext\Shared\Config.lua. 8 даёт ~15 ботов (balanced_teams).</summary>
    public int BotCount { get; set; } = 8;

    public string Map { get; set; } = "MP_001";
    public string Mode { get; set; } = "ConquestSmall0";

    /// <summary>true = сервер рекламирует LAN-адрес и объявляется в сети; false = только этот ПК.</summary>
    public bool OpenForLan { get; set; }

    /// <summary>IPv4 для LAN. Пусто = определить автоматически.</summary>
    public string LanIp { get; set; } = "";

    /// <summary>EVBOTS_BRIDGE (run\common.ps1).</summary>
    public bool UseBridge { get; set; } = true;

    /// <summary>EVBOTS_SPM_MGR / TICK / SOLDIER (run\common.ps1).</summary>
    public bool UseManager { get; set; } = true;

    /// <summary>EVBOTS_TESTS=1 + EVBOTS_PTS_REG=1 (как run\start.ps1).</summary>
    public bool EngineTests { get; set; }

    /// <summary>Клиент в оконном режиме 1280x720 (как run\join.ps1).</summary>
    public bool Windowed { get; set; } = true;

    public bool AcrylicEnabled { get; set; } = true;
    public bool DiscoveryEnabled { get; set; } = true;

    public string LastLogin { get; set; } = "";

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        PropertyNameCaseInsensitive = true,
    };

    public static string FilePath => Path.Combine(Paths.AppDir, "launcher-settings.json");

    public static LauncherSettings Load()
    {
        try
        {
            if (File.Exists(FilePath))
            {
                var loaded = JsonSerializer.Deserialize<LauncherSettings>(File.ReadAllText(FilePath), JsonOptions);
                if (loaded is not null)
                {
                    return loaded;
                }
            }
        }
        catch (Exception)
        {
            // битый файл — начинаем с умолчаний, старый файл не трогаем
        }

        return new LauncherSettings();
    }

    public void Save()
    {
        Directory.CreateDirectory(Paths.AppDir);
        File.WriteAllText(FilePath, JsonSerializer.Serialize(this, JsonOptions));
    }
}
