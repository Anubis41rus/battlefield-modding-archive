using System.Diagnostics;
using System.IO;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;

namespace EVBots.Launcher;

public partial class MainWindow
{
    private bool _profileUi;
    private ModProfile? _remoteProfile;
    private Process? _clientLoader;
    private ModProfile SelectedProfile => ModProfiles.Royale;
    private ModProfile? EffectiveProfile => _authIp.Length > 0 && _authIp != LocalIp ? _remoteProfile : SelectedProfile;
    private bool ClientLaunchRunning => _clientLoader is not null && !_clientLoader.HasExited;

    private void InitializeProfiles()
    {
        _s.ModProfileId = SelectedProfile.Id;
        _profileUi = true;
        ProfileBox.ItemsSource = ModProfiles.All;
        ProfileBox.DisplayMemberPath = "Label";
        ProfileBox.SelectedItem = SelectedProfile;
        _profileUi = false;
    }

    private void Profile_Changed(object sender, SelectionChangedEventArgs e)
    {
        if (_profileUi || ProfileBox.SelectedItem is not ModProfile next || next.Id == SelectedProfile.Id) return;
        if (_busy || ClientLaunchRunning || _host.ServerAlive || HostRunner.AnyGameRunning())
        {
            _profileUi = true;
            ProfileBox.SelectedItem = SelectedProfile;
            _profileUi = false;
            SetStatus("Набор модов можно сменить после выхода из игры и остановки сервера. Запущенная игра сохранена.");
            return;
        }
        SaveUi(); // Map/Mode/Rotation остаются настройками fun-bots.
        StopRules();
        _s.ModProfileId = next.Id;
        _s.Save();
        FillMaps();
        Log("Выбран набор: " + next.Label);
        SetStatus(ModProfiles.RoyaleNotice);
    }

    private void UpdateProfileControls()
    {
        var royale = SelectedProfile.IsRoyale;
        ProfileNote.Text = "Battle Royale по локальной сети. Полный игровой матч пока проходит проверку.";
        BotsBox.IsEnabled = !royale;
        JetsBox.IsEnabled = DefendBox.IsEnabled = SkillBox.IsEnabled = !royale;
        TicketsBox.IsEnabled = !royale;
        TimeBox.IsEnabled = !royale && RconRules.IsCtf(CurMode());
        MapBox.IsEnabled = ModeBox.IsEnabled = !royale;
        RotationList.IsEnabled = RoundsBox.IsEnabled = RotAddBtn.IsEnabled = RotDelBtn.IsEnabled = RotUpBtn.IsEnabled = !royale;
        RotApplyBtn.IsEnabled = NextMapBtn.IsEnabled = !royale;
        ActiveModsText.Text = EffectiveProfile is { } profile
            ? "Набор: " + profile.Label + (profile.IsRoyale ? " · работа матча ещё проверяется" : "")
            : "Набор модов определяет хост. Он будет проверен перед входом.";
    }

    private IEnumerable<string> LocalModsDirectories(ModProfile profile)
    {
        if (profile.IsRoyale && _p.StandDir.Length > 0) yield return Path.Combine(_p.StandDir, "Mods");
        if (_p.InstanceDir.Length > 0) yield return Path.Combine(_p.InstanceDir, "Admin", "Mods");
        if (!profile.IsRoyale && _p.StandDir.Length > 0) yield return Path.Combine(_p.StandDir, "Mods");
    }

    private ActiveModSet CheckHostMods(ModProfile profile)
    {
        var errors = new List<string>();
        foreach (var dir in LocalModsDirectories(profile).Distinct(StringComparer.OrdinalIgnoreCase))
        {
            try { return ModProfiles.CheckLocal(dir, profile, true, profile.IsRoyale ? ModProfiles.RoyaleFolders : null); }
            catch (Exception ex) when (ex is IOException or InvalidDataException or UnauthorizedAccessException or JsonException or ArgumentException)
            { errors.Add(ex.Message); }
        }
        throw new InvalidDataException("Набор сервера не готов: " + string.Join(" ", errors));
    }

    private ActiveModSet CheckClientMods(ActiveModSet host)
    {
        var errors = new List<string>();
        foreach (var dir in LocalModsDirectories(host.Profile).Distinct(StringComparer.OrdinalIgnoreCase))
        {
            try
            {
                var local = ModProfiles.CheckLocal(dir, host.Profile, false, host.Roots);
                ModProfiles.MatchHost(local, host);
                return local;
            }
            catch (Exception ex) when (ex is IOException or InvalidDataException or UnauthorizedAccessException or JsonException or ArgumentException)
            { errors.Add(ex.Message); }
        }
        throw new InvalidDataException("Локальный набор модов не совпадает с хостом. " + string.Join(" ", errors) +
            " Положите полный комплект хоста в EVBots\\Mods либо Instance\\Admin\\Mods. Автораздачи пока нет.");
    }

    private async Task ReadRemoteProfileLabelAsync()
    {
        if (_authIp.Length == 0 || _authIp == LocalIp) { UpdateProfileControls(); return; }
        var ip = _authIp;
        ModProfile? found;
        try
        {
            using var doc = JsonDocument.Parse(await Http.GetStringAsync($"http://{ip}:8080/mods"));
            var names = doc.RootElement.GetProperty("modList").EnumerateArray().Select(v => v.GetString() ?? "");
            found = ModProfiles.ProfileForRoots(names);
        }
        catch (Exception) { found = null; }
        if (_authIp != ip) return;
        _remoteProfile = found;
        UpdateProfileControls();
    }
}
