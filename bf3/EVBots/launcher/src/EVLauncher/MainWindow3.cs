using System.Diagnostics;
using System.IO;
using System.Text;
using System.Windows;
using EVBots.Installation;

namespace EVBots.Launcher;

public partial class MainWindow
{
    private string CurMap()
    {
        if (SelectedProfile.IsRoyale) return ModProfiles.RoyaleMap;
        var i = MapBox.SelectedIndex;
        return i >= 0 && i < _mapsShown.Count ? _mapsShown[i] : _s.Map;
    }

    private string CurMode()
    {
        if (SelectedProfile.IsRoyale) return ModProfiles.RoyaleMode;
        var i = ModeBox.SelectedIndex;
        return i >= 0 && i < _modesShown.Count ? _modesShown[i] : _s.Mode;
    }

    private static string RotationLabel(string line)
    {
        var p = line.Split(' ', StringSplitOptions.RemoveEmptyEntries);
        return p.Length >= 3 ? $"{GameData.MapLabel(p[0])} · {p[1]} · раундов {p[2]}" : line;
    }

    private void RefreshRotation()
    {
        RotationList.ItemsSource = null;
        RotationList.ItemsSource = _s.Rotation.Select(RotationLabel).ToList();
    }

    private void RotAdd_Click(object sender, RoutedEventArgs e)
    {
        if (SelectedProfile.IsRoyale) return;
        var rounds = RoundsBox.SelectedIndex >= 0 ? RoundsBox.SelectedIndex + 1 : 1;
        _s.Rounds = rounds;
        _s.Rotation.Add($"{CurMap()} {CurMode()} {rounds}");
        RefreshRotation();
        RotationList.SelectedIndex = _s.Rotation.Count - 1;
        SaveUi();
    }

    private void RotDel_Click(object sender, RoutedEventArgs e)
    {
        if (SelectedProfile.IsRoyale) return;
        var i = RotationList.SelectedIndex;
        if (i < 0 || i >= _s.Rotation.Count) return;
        _s.Rotation.RemoveAt(i);
        RefreshRotation();
        RotationList.SelectedIndex = Math.Min(i, _s.Rotation.Count - 1);
        SaveUi();
    }

    private void RotUp_Click(object sender, RoutedEventArgs e)
    {
        if (SelectedProfile.IsRoyale) return;
        var i = RotationList.SelectedIndex;
        if (i <= 0 || i >= _s.Rotation.Count) return;
        (_s.Rotation[i - 1], _s.Rotation[i]) = (_s.Rotation[i], _s.Rotation[i - 1]);
        RefreshRotation();
        RotationList.SelectedIndex = i - 1;
        SaveUi();
    }

    private void SaveUi()
    {
        if (!SelectedProfile.IsRoyale) { _s.Map = CurMap(); _s.Mode = CurMode(); }
        _s.Bots = BotsBox.SelectedIndex >= 0 ? BotsBox.SelectedIndex : _s.Bots;
        if (FreqBox.SelectedIndex >= 0) _s.Frequency = Frequencies[FreqBox.SelectedIndex];
        _s.ServerName = ServerNameBox.Text.Trim();
        if (_s.ServerName.Length == 0) _s.ServerName = "EVBots";
        _s.Login = LoginBox.Text.Trim();
        _s.Password = PasswordBox.Password;
        _s.Nick = NickBox.Text.Trim();
        _s.Save();
    }

    private async void Host_Click(object sender, RoutedEventArgs e)
    {
        if (_busy) return;
        if (ClientLaunchRunning || _host.ServerAlive || HostRunner.AnyGameRunning())
        { SetStatus("Сначала закрой игру и останови сервер. Работающий матч не будет прерван."); return; }
        SaveUi();
        if (_p.ServerDir.Length == 0)
        { SetStatus("BF-Portable не найден — хостить нельзя. Ставь BF-Portable рядом с игрой."); return; }
        _busy = true;
        _hosting = true;
        _joinAfterHost = false;
        var ready = false;
        var profile = SelectedProfile;
        var map = CurMap();
        var mode = CurMode();
        var hostPaths = _p;
        try
        {
            SetStatus("Проверяю моды: " + profile.Label);
            var active = await Task.Run(() => CheckHostMods(profile));
            Log("Моды сервера: " + string.Join(", ", active.Mods.Select(m => m.Folder + " " + m.Version)));
            if (profile.IsRoyale)
            {
                SetStatus("Готовлю отдельный сервер Royale без ботов...");
                var layout = await Task.Run(() => RoyaleServerLayout.Prepare(_p.ServerDir, _p.InstanceDir,
                    RoyaleInstanceConfig.ServerDirectory(_p.StandDir), Log, serverName: _s.ServerName));
                hostPaths = new FoundPaths { Root = _p.Root, KitDir = _p.KitDir, ClientGameDir = _p.ClientGameDir,
                    StandDir = _p.StandDir, ServerDir = layout.ServerDir, InstanceDir = layout.InstanceDir };
            }
            if (profile.IsRoyale) Log(ModProfiles.RoyaleNotice);
            SetStatus("Поднимаю сервер: " + profile.Label);
            await Task.Run(() =>
            {
                if (HostRunner.AnyGameRunning()) throw new InvalidOperationException("Игра или сервер уже запущены.");
                _host.StopBackend(_p, Log);
                if (!profile.IsRoyale)
                {
                    var mapList = Path.Combine(hostPaths.InstanceDir, "Admin", "MapList.txt");
                    var rotation = _s.Rotation.Count > 0 ? _s.Rotation.ToArray() : new[] { $"{map} {mode} 1" };
                    File.WriteAllLines(mapList, rotation);
                    File.WriteAllLines(Path.Combine(hostPaths.InstanceDir, "Admin", "ModList.txt"), active.Roots, new UTF8Encoding(false));
                    var first = rotation[0].Split(' ', StringSplitOptions.RemoveEmptyEntries);
                    if (first.Length >= 2) { map = first[0]; mode = first[1]; }
                    _s.Map = map;
                    _s.Mode = mode;
                    var startup = Path.Combine(hostPaths.InstanceDir, "Admin", "Startup.txt");
                    if (File.Exists(startup))
                    {
                        var lines = File.ReadAllLines(startup).ToList();
                        var nameLine = lines.FindIndex(line => line.StartsWith("vars.serverName", StringComparison.Ordinal));
                        var name = "vars.serverName " + _s.ServerName;
                        if (nameLine < 0) lines.Add(name);
                        else lines[nameLine] = name;
                        if (!lines.Any(l => l.TrimStart().StartsWith("vars.unlockMode", StringComparison.Ordinal)))
                            lines.Add("vars.unlockMode all");
                        SetStartupRule(lines, mode, _s.RuleFor(mode));
                        File.WriteAllLines(startup, lines);
                    }
                }
                Dispatcher.Invoke(StopRules);
                _host.StartBackend(hostPaths, MyLanIp() ?? "", _s.ServerName, Log, _s.Frequency, active.Directory,
                    sourceServerDir: _p.ServerDir);
            });
            var up = false;
            for (var i = 0; i < 20; i++)
            {
                await Task.Delay(1000);
                try { await Http.GetStringAsync("http://127.0.0.1:8080/"); up = true; break; }
                catch (Exception) { }
            }
            if (!up) { SetStatus("Бэкенд не ответил (нет .NET 10 или занят 8080)."); return; }
            var lanIp = MyLanIp();
            if (lanIp is not null && !UrlAclExists(lanIp))
                Log($"ВНИМАНИЕ: нет URL-ACL для http://{lanIp}:8080/ — гости не смогут войти. Нажми «Фаервол (UAC)» и «Поднять сервер» заново.");
            var logPath = Path.Combine(_p.StandDir, "logs", "EVBots.log");
            var off = File.Exists(logPath) ? new FileInfo(logPath).Length : 0;
            await Task.Run(() => _host.StartServer(hostPaths, _s.ServerName, map, mode, Log, _s.Frequency, active.EnvironmentValue, active.Directory));
            Log($"Частота сервера: {HostRunner.NormFrequency(_s.Frequency)} Гц");
            SetStatus(profile.IsRoyale ? "Сервер Royale загружает карту..." : "Сервер запускается, жду LUA-MODE READY...");
            var res = await WaitReadyAsync(logPath, off, 150, profile.IsRoyale);
            if (res == "ready")
            {
                ready = true;
                SetStatus(profile.IsRoyale
                    ? "VU Battle Royale: карта загружена. Тестовый режим — полный матч ещё не подтверждён."
                    : "Сервер готов. Скажи друзьям свой IP.");
                Log("server ready");
                if (!profile.IsRoyale)
                {
                var r = _s.RuleFor(_s.Mode);
                Log($"Режим {_s.Mode}: тикеты/очки {r.Tickets}%" + (RconRules.IsCtf(_s.Mode) ? $", время CTF {r.Time}%" : ""));
                StartRules();
                // 20.10: число ботов — команда мода fun-bots (ext\Server\Commands\RCON.lua: funbots.config.InitNumberOfBots,
                // применяется сразу), как RCON у VU. Config.lua мода больше не правим.
                try
                {
                    var r2 = await SendModAsync(_rcon!, "funbots.config.InitNumberOfBots", _s.Bots.ToString());
                    Log($"Ботов: {_s.Bots} — команда мода: {string.Join(' ', r2)}");
                }
                catch (Exception ex) { Log("Ботов: команда мода не прошла: " + ex.Message); }
                await SendJetsAsync();
                await SendSkillAsync();      // 01.11: сложность ботов из лаунчера
                await SendDefendAsync();     // 01.11: охрана точек
                await GrantHostMenuAsync();  // 26.10: меню F12 — хосту без ручного RCON
                }
            }
            else { SetStatus("Сервер не готов: " + res); Log("ERROR: " + res); }
        }
        catch (Exception ex) { SetStatus("Сервер не запущен: " + ex.Message); Log("ERROR: " + ex.Message); }
        finally { _busy = false; _hosting = false; UpdateProfileControls(); }
        if (_joinAfterHost)
        {
            _joinAfterHost = false;
            if (ready) { Log("вход по клику во время подъёма сервера"); Join_Click(sender, e); }
        }
    }

    private async Task<string> WaitReadyAsync(string luaLog, long off, int sec, bool requireLevel = false)
    {
        var end = DateTime.UtcNow.AddSeconds(sec);
        var luaReady = false;
        var levelReady = false;
        while (DateTime.UtcNow < end)
        {
            var lines = new List<string>();
            try
            {
                using var fs = new FileStream(luaLog, FileMode.Open, FileAccess.Read,
                    FileShare.ReadWrite | FileShare.Delete);
                if (off > fs.Length) off = 0;
                fs.Seek(off, SeekOrigin.Begin);
                using var sr = new StreamReader(fs, Encoding.UTF8);
                lines.AddRange((await sr.ReadToEndAsync()).Split('\n'));
                off = fs.Position;
            }
            catch (Exception) { }
            foreach (var l in lines)
            {
                if (l.Contains("VEH #")) return "падение " + l.Trim();
                if (l.Contains("LUA-MODE FAILED") || l.Contains("LEVEL-MODE FAILED") || (requireLevel && l.Contains("] ERR ")))
                    return "ошибка загрузки мода — смотри журнал: " + l.Trim();
            }
            luaReady |= lines.Any(l => l.Contains("LUA-MODE READY"));
            levelReady |= lines.Any(l => l.Contains("LEVEL-MODE READY"));
            if (luaReady && (!requireLevel || levelReady)) return "ready";
            await Task.Delay(1000);
        }
        return "timeout";
    }

    private void Stop_Click(object sender, RoutedEventArgs e)
    {
        SaveUi();
        StopRules();
        _host.StopAll(Log);
        SetStatus("Остановлен.");
    }

    private void Refresh_Click(object sender, RoutedEventArgs e) => _ = TickAsync();
}
