// 20.10.2026: тикеты/очки и время раунда отдельно для каждого режима (окно «Мой сервер»).
// Значения выбранного режима — в TicketsBox/TimeBox; хранятся в friend-settings.json (ModeRules).
// Первая карта получает их через Startup.txt, следующие — через RCON перед загрузкой (RconRules).
using System.IO;
using System.Windows.Controls;

namespace EVBots.Launcher;

public partial class MainWindow
{
    private static readonly int[] TicketSteps = { 10, 20, 25, 30, 50, 75, 100, 125, 150, 200, 300, 400 };
    private static readonly int[] TimeSteps = { 25, 50, 75, 100, 150, 200, 300, 400 };
    private bool _ruleUi;
    private RconRules? _rcon;

    private static int Nearest(int[] steps, int v) =>
        Array.IndexOf(steps, steps.OrderBy(x => Math.Abs(x - v)).First());

    /// <summary>Показать правило режима в полях (без записи обратно).</summary>
    private void ShowRule(string mode)
    {
        var r = _s.RuleFor(mode);
        _ruleUi = true;
        TicketsBox.SelectedIndex = Nearest(TicketSteps, r.Tickets);
        TimeBox.SelectedIndex = Nearest(TimeSteps, r.Time);
        // время раунда в BF3 есть только у «Захвата флага» (vars.ctfRoundTimeModifier)
        TimeBox.IsEnabled = !SelectedProfile.IsRoyale && RconRules.IsCtf(mode);
        _ruleUi = false;
    }

    private void Rule_Changed(object sender, SelectionChangedEventArgs e)
    {
        if (SelectedProfile.IsRoyale || _ruleUi || TicketsBox.SelectedIndex < 0 || TimeBox.SelectedIndex < 0) return;
        var mode = CurMode();
        _s.ModeRules[mode] = new ModeRule
        {
            Tickets = TicketSteps[TicketsBox.SelectedIndex],
            Time = TimeSteps[TimeBox.SelectedIndex],
        };
        _s.Save();
    }

    /// <summary>Строки Startup.txt для режима первой карты: vars.gameModeCounter и (CTF) vars.ctfRoundTimeModifier.</summary>
    private static void SetStartupRule(List<string> lines, string mode, ModeRule r)
    {
        void Set(string name, int v)
        {
            var i = lines.FindIndex(l => l.TrimStart().StartsWith(name + " ", StringComparison.Ordinal));
            if (i >= 0) lines[i] = name + " " + v;
            else lines.Add(name + " " + v);
        }
        // vars.roundTimeLimit в BF3 нет (UnknownCommand) — убираем, если его записала прежняя версия
        lines.RemoveAll(l => l.TrimStart().StartsWith("vars.roundTimeLimit", StringComparison.Ordinal));
        Set("vars.gameModeCounter", r.Tickets);
        Set("vars.ctfRoundTimeModifier", RconRules.IsCtf(mode) ? r.Time : 100);
    }

    private void StartRules()
    {
        StopRules();
        if (SelectedProfile.IsRoyale) return;
        // пароль RCON — тот, с которым HostRunner запускает сервер (+admin.password)
        _rcon = new RconRules(HostRunner.RconPassword, m => _s.RuleFor(m), Log);
        _rcon.Start();
    }

    private void StopRules()
    {
        _rcon?.Stop();
        _rcon = null;
    }

    private bool _jetsUi;

    /// <summary>22.10: «Боты берут самолёты» — команда мода funbots.config.UseJets (сразу, если сервер поднят).</summary>
    private async void Jets_Changed(object sender, System.Windows.RoutedEventArgs e)
    {
        if (_jetsUi || SelectedProfile.IsRoyale) return;
        _s.BotsJets = JetsBox.IsChecked == true;
        _s.Save();
        if (_rcon is not null) await SendJetsAsync();
    }

    /// <summary>
    /// 28.10 (239): команда мода через RCON. Перехват команд модов в EVBots.dll встаёт через несколько секунд после
    /// LUA-MODE READY (журнал комплекта 26.10: READY 23:49:38.49, «команды модов доступны» 23:49:45.00) — до этого сервер
    /// отвечает UnknownCommand, и у друга 27.10 пропали права F12, «без самолётов» и число ботов. Повтор раз в секунду, до 60 с.
    /// </summary>
    private static async Task<string[]> SendModAsync(RconRules rcon, params string[] words)
    {
        var r = await rcon.SendAsync(words);
        for (var i = 0; i < 60 && r.Length > 0 && r[0] == "UnknownCommand"; i++)
        {
            await Task.Delay(1000);
            r = await rcon.SendAsync(words);
        }
        return r;
    }

    /// <summary>
    /// 01.11: «Сложность ботов» — уровни поверх настроек прицела мода fun-bots (ext\Shared\Config.lua, раздел DIFFICULTY):
    /// ухудшение прицела (0 — без разброса), разброс умения, доп. время реакции, прицел в технике. «Нормально» — значения
    /// мода по умолчанию (SettingsDefinition.lua Default). Мод читает их в момент прицеливания (Bot\BotAiming.lua), поэтому
    /// команда funbots.config.* действует сразу, без перезапуска. Пределы — Range из SettingsDefinition.lua.
    /// </summary>
    private static readonly (string Label, (string Name, double Value)[] Values)[] SkillLevels =
    {
        ("Очень легко", new[] { ("BotAimWorsening", 1.5), ("BotSniperAimWorsening", 0.8), ("BotSupportAimWorsening", 1.8),
            ("BotWorseningSkill", 0.8), ("BotSniperWorseningSkill", 0.7), ("ReactionTime", 0.8), ("VehicleAimWorsening", 0.6) }),
        ("Легко", new[] { ("BotAimWorsening", 0.6), ("BotSniperAimWorsening", 0.35), ("BotSupportAimWorsening", 0.8),
            ("BotWorseningSkill", 0.6), ("BotSniperWorseningSkill", 0.5), ("ReactionTime", 0.5), ("VehicleAimWorsening", 0.25) }),
        ("Нормально", new[] { ("BotAimWorsening", 0.2), ("BotSniperAimWorsening", 0.1), ("BotSupportAimWorsening", 0.3),
            ("BotWorseningSkill", 0.4), ("BotSniperWorseningSkill", 0.3), ("ReactionTime", 0.2), ("VehicleAimWorsening", 0.05) }),
        ("Сложно", new[] { ("BotAimWorsening", 0.1), ("BotSniperAimWorsening", 0.05), ("BotSupportAimWorsening", 0.15),
            ("BotWorseningSkill", 0.2), ("BotSniperWorseningSkill", 0.15), ("ReactionTime", 0.1), ("VehicleAimWorsening", 0.0) }),
        ("Очень сложно", new[] { ("BotAimWorsening", 0.0), ("BotSniperAimWorsening", 0.0), ("BotSupportAimWorsening", 0.0),
            ("BotWorseningSkill", 0.05), ("BotSniperWorseningSkill", 0.05), ("ReactionTime", 0.0), ("VehicleAimWorsening", 0.0) }),
    };

    private async void Skill_Changed(object sender, System.Windows.Controls.SelectionChangedEventArgs e)
    {
        if (_jetsUi || SelectedProfile.IsRoyale || SkillBox.SelectedIndex < 0) return;
        _s.BotsSkill = SkillBox.SelectedIndex;
        _s.Save();
        if (_rcon is not null) await SendSkillAsync();
    }

    private async Task SendSkillAsync()
    {
        if (SelectedProfile.IsRoyale) return;
        var rcon = _rcon;
        if (rcon is null) return;
        var level = SkillLevels[Math.Clamp(_s.BotsSkill, 0, SkillLevels.Length - 1)];
        var bad = new List<string>();
        try
        {
            foreach (var (name, value) in level.Values)
            {
                var r = await SendModAsync(rcon, "funbots.config." + name, value.ToString("0.0##", System.Globalization.CultureInfo.InvariantCulture));
                if (!RconRules.Ok(r)) bad.Add($"{name}: {string.Join(' ', r)}");
            }
            Log($"Сложность ботов: {level.Label}" + (bad.Count == 0 ? " — мод принял" : " — не принято: " + string.Join("; ", bad)));
        }
        catch (Exception ex) { Log("Сложность ботов: команда мода не прошла: " + ex.Message); }
    }

    /// <summary>
    /// 01.11: «Боты охраняют свои точки» — настройка мода DefendObjectives. Друг: на малых режимах после захвата всех точек
    /// часть ботов «бегает на месте друг за другом». По коду мода (GameDirector:OnEngineUpdate) так бывает, когда все точки
    /// свои: всех ботов команды мод шлёт в защиту и раз в 1,5 с перераспределяет защитников по точкам. Выкл. — без защиты, боты
    /// ходят по путям. Причина у друга прогоном не подтверждена.
    /// </summary>
    private async void Defend_Changed(object sender, System.Windows.RoutedEventArgs e)
    {
        if (_jetsUi || SelectedProfile.IsRoyale) return;
        _s.BotsDefend = DefendBox.IsChecked == true;
        _s.Save();
        if (_rcon is not null) await SendDefendAsync();
    }

    private async Task SendDefendAsync()
    {
        if (SelectedProfile.IsRoyale) return;
        var rcon = _rcon;
        if (rcon is null) return;
        try
        {
            var r = await SendModAsync(rcon, "funbots.config.DefendObjectives", _s.BotsDefend ? "true" : "false");
            Log($"Боты охраняют точки: {(_s.BotsDefend ? "да" : "нет")} — команда мода: {string.Join(' ', r)}");
        }
        catch (Exception ex) { Log("Охрана точек: команда мода не прошла: " + ex.Message); }
    }


    private async Task SendJetsAsync()
    {
        if (SelectedProfile.IsRoyale) return;
        var rcon = _rcon;
        if (rcon is null) return;
        try
        {
            var r = await SendModAsync(rcon, "funbots.config.UseJets", _s.BotsJets ? "true" : "false");
            Log($"Боты в самолётах: {(_s.BotsJets ? "да" : "нет")} — команда мода: {string.Join(' ', r)}");
        }
        catch (Exception ex) { Log("Боты в самолётах: команда мода не прошла: " + ex.Message); }
    }

    /// <summary>
    /// 26.10 (238-3): меню F12 мода (право UserInterface и др.) — хосту, автоматически при «Поднять сервер». Как на VU: владелец
    /// сервера выдаёт права RCON-командой мода <c>funbots.Permissions &lt;ник&gt; *</c>, мод сам хранит их в своей mod.db по имени
    /// игрока (ext\Server\PermissionManager.lua). Выдаём только нику этого лаунчера — гостям права не даются; у друга-хоста его
    /// лаунчер так же выдаёт их ему. Файлы мода не трогаем.
    /// </summary>
    private async Task GrantHostMenuAsync()
    {
        if (SelectedProfile.IsRoyale) return;
        var rcon = _rcon;
        if (rcon is null) return;
        var nick = (_s.Nick.Length > 0 ? _s.Nick : _s.Login).Trim();
        if (nick.Length == 0)
        {
            Log("Меню F12: ник хоста не задан — права мода не выданы (впиши ник, войди и подними сервер заново).");
            return;
        }
        try
        {
            var r = await SendModAsync(rcon, "funbots.Permissions", nick, "*");
            Log($"Меню F12 — права мода хосту «{nick}»: {string.Join(' ', r)}");
        }
        catch (Exception ex) { Log("Меню F12: команда мода не прошла: " + ex.Message); }
    }

    /// <summary>
    /// 22.10: «Карты по кругу» — работающему серверу через RCON (mapList.clear / add / save), без перезапуска. Текущий раунд
    /// доигрывается, следующая карта — первая из нового списка (mapList.setNextMapIndex 0); тикеты для неё — сразу.
    /// </summary>
    private async void RotApply_Click(object sender, System.Windows.RoutedEventArgs e)
    {
        if (SelectedProfile.IsRoyale) return;
        SaveUi();
        var rcon = _rcon;
        if (rcon is null) { SetStatus("Сервер не поднят — список возьмётся при «Поднять сервер»."); return; }
        var rotation = _s.Rotation.Count > 0 ? _s.Rotation.ToArray() : new[] { $"{_s.Map} {_s.Mode} 1" };
        try
        {
            var mapList = System.IO.Path.Combine(_p.InstanceDir, "Admin", "MapList.txt");
            System.IO.File.WriteAllLines(mapList, rotation);
            var r = await rcon.SendAsync("mapList.clear");
            if (!RconRules.Ok(r)) { SetStatus("Сервер не принял список: " + string.Join(' ', r)); return; }
            foreach (var line in rotation)
            {
                var p = line.Split(' ', StringSplitOptions.RemoveEmptyEntries);
                if (p.Length < 2) continue;
                var a = await rcon.SendAsync("mapList.add", p[0], p[1], p.Length >= 3 ? p[2] : "1");
                if (!RconRules.Ok(a)) { SetStatus($"Карта {p[0]} {p[1]} не принята: " + string.Join(' ', a)); return; }
            }
            await rcon.SendAsync("mapList.save");
            await rcon.SendAsync("mapList.setNextMapIndex", "0");
            rcon.ApplyNext();
            SetStatus($"Список карт ({rotation.Length}) отдан серверу. После этого раунда — {RotationLabel(rotation[0])}.");
            Log("RCON: список карт применён без перезапуска: " + string.Join(" | ", rotation));
        }
        catch (Exception ex) { SetStatus("Список не применён: " + ex.Message); }
    }

    /// <summary>
    /// 22.10: закончить раунд сейчас — mapList.endRound (победитель — у кого больше очков). Это обычный конец раунда движка:
    /// Server:RoundOver, таблица счёта, затем следующая карта списка; счёт игроков и ботов движок обнуляет сам
    /// (ServerRoundResetMessage → PointSystem). mapList.runNextRound так не делает: раунд не кончается, счёт остаётся.
    /// </summary>
    private async void NextMap_Click(object sender, System.Windows.RoutedEventArgs e)
    {
        if (SelectedProfile.IsRoyale) return;
        var rcon = _rcon;
        if (rcon is null) { SetStatus("Сервер не поднят."); return; }
        if (System.Windows.MessageBox.Show(this, "Закончить текущий раунд и перейти к следующей карте?", "EV Launcher",
                System.Windows.MessageBoxButton.YesNo, System.Windows.MessageBoxImage.Question) != System.Windows.MessageBoxResult.Yes)
            return;
        try
        {
            var winner = "1";
            var info = await rcon.SendAsync("serverInfo");
            // serverInfo: … [8] число команд, затем очки команд
            if (RconRules.Ok(info) && info.Length > 10 && int.TryParse(info[8], out var teams) && teams >= 2 && info.Length > 8 + teams)
            {
                var best = 0; var bestScore = double.MinValue;
                for (var t = 0; t < teams; t++)
                    if (double.TryParse(info[9 + t], System.Globalization.NumberStyles.Float,
                            System.Globalization.CultureInfo.InvariantCulture, out var sc) && sc > bestScore)
                    { bestScore = sc; best = t; }
                winner = (best + 1).ToString();
            }
            var r = await rcon.SendAsync("mapList.endRound", winner);
            SetStatus(RconRules.Ok(r) ? "Раунд закончен — сервер переходит к следующей карте." : "Сервер не закончил раунд: " + string.Join(' ', r));
            Log($"RCON: mapList.endRound {winner} -> {string.Join(' ', r)}");
        }
        catch (Exception ex) { SetStatus("Не получилось: " + ex.Message); }
    }
}
