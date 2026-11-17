using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Threading;
using EVBots.Core;

// WPF также содержит System.Windows.Controls.Validation: явный алиас снимает неоднозначность
using Validation = EVBots.Core.Validation;

namespace EVBots.Gui;

/// <summary>
/// Окно лаунчера. Вкладки: Сервер, Играть, Аккаунты, Серверы, Настройки.
/// Все сетевые и файловые действия — асинхронно; UI меняется только в потоке окна.
/// </summary>
public partial class MainWindow : Window
{
    private readonly LauncherSettings _s = LauncherSettings.Load();
    private readonly Runtime _rt = new();
    private readonly LanDiscovery _lan = new();
    private readonly BackendClient _local = new("127.0.0.1", Ports.Http);
    private readonly DispatcherTimer _tick = new() { Interval = TimeSpan.FromSeconds(2) };
    private readonly DispatcherTimer _logTick = new() { Interval = TimeSpan.FromSeconds(1) };
    private readonly LogTail _tail = new();
    private readonly List<string> _launcherLines = new();
    private readonly List<AccountDto> _accounts = new();
    private readonly ObservableCollection<AccountRow> _accountRows = new();
    private readonly ObservableCollection<ServerRow> _localRows = new();
    private readonly ObservableCollection<ServerRow> _networkRows = new();
    private readonly Button[] _navButtons;
    private readonly ScrollViewer[] _panels;
    private readonly Button[] _logButtons;
    private int _tickBusy;
    private bool _ready;
    private bool _accountsLoaded;
    private bool _backendAlive;
    private int _logSource;
    private string _localSig = "";
    private string _networkSig = "";
    private ServerRow? _target;

    public MainWindow()
    {
        InitializeComponent();

        _navButtons = new[] { NavServer, NavPlay, NavAccounts, NavServers, NavSettings };
        _panels = new[] { PanelServer, PanelPlay, PanelAccounts, PanelServers, PanelSettings };
        _logButtons = new[] { LogSrc0, LogSrc1, LogSrc2, LogSrc3, LogSrc4 };

        LocalList.ItemsSource = _localRows;
        NetworkList.ItemsSource = _networkRows;
        AccountsList.ItemsSource = _accountRows;

        _lan.Changed += list => Dispatcher.BeginInvoke(new Action(() => ShowNetwork(list)));
        _tick.Tick += async (_, _) => await RunTickAsync();
        _logTick.Tick += (_, _) => RunLogTick();

        Loaded += OnLoaded;
        SourceInitialized += (_, _) => ApplyGlass();
        Closing += OnClosing;

        FillSettingsUi();
        SelectTab(0);
        SelectLogSource(0);
    }

    // ───────────── жизненный цикл ─────────────

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        _ready = true;
        if (_s.DiscoveryEnabled)
        {
            StartDiscovery();
        }

        _tick.Start();
        _logTick.Start();
        Notify("Готово. Запустите сервер на вкладке «Сервер» или войдите в игру.");
        _ = RunTickAsync();
    }

    private void OnClosing(object? sender, CancelEventArgs e)
    {
        if (_rt.ServerOwned || _rt.BackendOwned)
        {
            var answer = MessageBox.Show(this,
                "Остановить сервер и бэкенд, запущенные этим лаунчером?\n\n" +
                "Да: остановить.\nНет: оставить работать (вывод пишется в файлы логов).\nОтмена: вернуться в окно.",
                "Выход", MessageBoxButton.YesNoCancel, MessageBoxImage.Question);
            if (answer == MessageBoxResult.Cancel)
            {
                e.Cancel = true;
                return;
            }

            if (answer == MessageBoxResult.Yes)
            {
                _rt.Stop(includeBackend: true);
            }
        }

        _tick.Stop();
        _logTick.Stop();
        _lan.Dispose();
    }

    /// <summary>Acrylic, если доступен; иначе полупрозрачные градиенты. Вызывается при старте и после сохранения.</summary>
    private void ApplyGlass()
    {
        var hwnd = new WindowInteropHelper(this).Handle;
        var on = _s.AcrylicEnabled && Acrylic.TryEnable(hwnd, Color.FromArgb(0xB8, 0x0B, 0x10, 0x20));
        if (!on)
        {
            Acrylic.Disable(hwnd);
        }

        Resources["WindowBgBrush"] = on ? Brushes.Transparent : FindResource("WindowBgFallback");
        GlassText.Text = on
            ? "Сейчас: Acrylic (фон окна размыт)."
            : "Сейчас: полупрозрачные градиенты (Acrylic выключен или недоступен в этой Windows).";
    }

    // ───────────── навигация и окно ─────────────

    private void MinBtn_Click(object sender, RoutedEventArgs e) => WindowState = WindowState.Minimized;

    private void MaxBtn_Click(object sender, RoutedEventArgs e) =>
        WindowState = WindowState == WindowState.Maximized ? WindowState.Normal : WindowState.Maximized;

    private void CloseBtn_Click(object sender, RoutedEventArgs e) => Close();

    private void Nav_Click(object sender, RoutedEventArgs e)
    {
        if (sender is Button { Tag: string tag } && int.TryParse(tag, out var index))
        {
            SelectTab(index);
        }
    }

    private void SelectTab(int index)
    {
        for (var i = 0; i < _panels.Length; i++)
        {
            _panels[i].Visibility = i == index ? Visibility.Visible : Visibility.Collapsed;
            _navButtons[i].Background = i == index ? (Brush)FindResource("AccentSoftBrush") : Brushes.Transparent;
            _navButtons[i].Foreground = i == index ? (Brush)FindResource("TextBrush") : (Brush)FindResource("TextDimBrush");
        }

        if (index == 2 && _ready)
        {
            _ = ReloadAccountsAsync();
        }
    }

    // ───────────── настройки формы ─────────────

    private void FillSettingsUi()
    {
        ServerNameBox.Text = _s.ServerName;
        MaxPlayersBox.Text = _s.MaxPlayers.ToString();
        BotsBox.Text = _s.BotCount.ToString();
        MapBox.Text = _s.Map;
        ModeBox.Text = _s.Mode;
        OpenLanCheck.IsChecked = _s.OpenForLan;
        BridgeCheck.IsChecked = _s.UseBridge;
        ManagerCheck.IsChecked = _s.UseManager;
        TestsCheck.IsChecked = _s.EngineTests;
        WindowedCheck.IsChecked = _s.Windowed;
        JoinLoginBox.Text = _s.LastLogin;
        LanIpBox.Text = _s.LanIp;
        AcrylicCheck.IsChecked = _s.AcrylicEnabled;
        DiscoveryCheck.IsChecked = _s.DiscoveryEnabled;

        var addresses = LanInfo.Addresses();
        LanHintText.Text = addresses.Count == 0
            ? "Адаптеры с IPv4 не найдены."
            : "Адреса этого ПК: " + string.Join(", ", addresses.Select(a => a.Ip + (a.Preferred ? " (основной)" : "")));

        var current = FunBotsConfig.ReadInitBots(Paths.FunBotsConfig);
        ConfigBotsText.Text = current is null
            ? "Config.lua мода не найден."
            : $"В Config.lua сейчас: InitNumberOfBots = {current}.";

        FillPaths();
    }

    private void FillPaths()
    {
        PathsPanel.Children.Clear();
        foreach (var (label, location) in PathList())
        {
            var exists = File.Exists(location) || Directory.Exists(location);
            PathsPanel.Children.Add(new TextBlock
            {
                Text = (exists ? "есть   " : "НЕТ    ") + label + ": " + location,
                Style = (Style)FindResource("Mono"),
                Foreground = exists ? (Brush)FindResource("TextDimBrush") : (Brush)FindResource("WarnBrush"),
                Margin = new Thickness(0, 2, 0, 2),
            });
        }
    }

    private static IEnumerable<(string Label, string Location)> PathList() =>
    [
        ("evbots-loader", Paths.Loader),
        ("лицензия", Paths.LicenseArtifact),
        ("бэкенд", Paths.BackendExe),
        ("EVBots.dll (стенд)", Paths.EVBotsExtDeployed),
        ("клиент BF3", Paths.ClientGameDir + @"\bf3.exe"),
        ("сервер BF3", Paths.ServerExe),
        ("инстанс сервера", Paths.InstanceDir),
        ("Config.lua мода", Paths.FunBotsConfig),
        ("логи сервера", Paths.RuntimeLogsDir),
    ];

    /// <summary>Переносит значения формы в настройки и проверяет их. false = есть ошибка (текст в error).</summary>
    private bool ReadAllFromUi(out string error)
    {
        error = "";
        var name = ServerNameBox.Text.Trim();
        var map = MapBox.Text.Trim();
        var mode = ModeBox.Text.Trim();
        if (!int.TryParse(MaxPlayersBox.Text.Trim(), out var max) || !int.TryParse(BotsBox.Text.Trim(), out var bots))
        {
            error = "Макс. игроков и боты — целые числа.";
            return false;
        }

        var lanText = LanIpBox.Text.Trim();
        if (lanText.Length > 0 && !Validation.TryIPv4(lanText, out _))
        {
            error = "LAN-IP: адрес вида 192.168.1.10 или пусто (определить автоматически).";
            return false;
        }

        error = Validation.ServerName(name)
                ?? Validation.MaxPlayers(max)
                ?? Validation.Bots(bots)
                ?? Validation.MapOrMode(map, "Карта")
                ?? Validation.MapOrMode(mode, "Режим")
                ?? "";
        if (error.Length > 0)
        {
            return false;
        }

        _s.ServerName = name;
        _s.MaxPlayers = max;
        _s.BotCount = bots;
        _s.Map = map;
        _s.Mode = mode;
        _s.OpenForLan = OpenLanCheck.IsChecked == true;
        _s.UseBridge = BridgeCheck.IsChecked == true;
        _s.UseManager = ManagerCheck.IsChecked == true;
        _s.EngineTests = TestsCheck.IsChecked == true;
        _s.Windowed = WindowedCheck.IsChecked == true;
        _s.LanIp = lanText;
        _s.AcrylicEnabled = AcrylicCheck.IsChecked == true;
        _s.DiscoveryEnabled = DiscoveryCheck.IsChecked == true;
        _s.LastLogin = JoinLoginBox.Text.Trim();
        return true;
    }

    // ───────────── сообщения и лог ─────────────

    private void Notify(string text, bool warn = false)
    {
        NotifyText.Text = text;
        NotifyText.Foreground = warn ? (Brush)FindResource("WarnBrush") : (Brush)FindResource("TextDimBrush");
        var line = $"{DateTime.Now:HH:mm:ss}  {text}";
        _launcherLines.Add(line);
        if (_launcherLines.Count > 2000)
        {
            _launcherLines.RemoveRange(0, 500);
        }

        if (_logSource == 0)
        {
            AppendLogText(line + Environment.NewLine);
        }
    }

    private void AppendLogText(string text)
    {
        LogBox.AppendText(text);
        if (LogBox.Text.Length > 300_000)
        {
            LogBox.Text = LogBox.Text[^200_000..];
        }

        LogBox.ScrollToEnd();
    }

    private void LogSource_Click(object sender, RoutedEventArgs e)
    {
        if (sender is Button { Tag: string tag } && int.TryParse(tag, out var index))
        {
            SelectLogSource(index);
        }
    }

    private void SelectLogSource(int index)
    {
        _logSource = index;
        for (var i = 0; i < _logButtons.Length; i++)
        {
            _logButtons[i].Background = i == index ? (Brush)FindResource("AccentSoftBrush") : (Brush)FindResource("PillBrush");
        }

        _tail.Attach(null, 0);
        if (index == 0)
        {
            LogHeaderText.Text = "Лог лаунчера: действия и итоги запуска";
            LogBox.Text = string.Join(Environment.NewLine, _launcherLines) + Environment.NewLine;
            LogBox.ScrollToEnd();
        }
    }

    private void OpenLogs_Click(object sender, RoutedEventArgs e) => OpenFolder(Paths.LogsDir);

    private void OpenLauncherDir_Click(object sender, RoutedEventArgs e) => OpenFolder(Paths.AppDir);

    private static void OpenFolder(string dir)
    {
        try
        {
            Directory.CreateDirectory(dir);
            Process.Start(new ProcessStartInfo { FileName = dir, UseShellExecute = true })?.Dispose();
        }
        catch (Exception)
        {
            // проводник не открылся — не критично
        }
    }

    private void RunLogTick()
    {
        if (!_ready || _logSource == 0)
        {
            return;
        }

        var path = CurrentLogPath();
        if (path is null)
        {
            LogHeaderText.Text = "Файл лога ещё не создан (появится после запуска).";
            return;
        }

        if (path != _tail.Path)
        {
            var lines = _tail.Attach(path, 400);
            LogBox.Text = string.Join(Environment.NewLine, lines) + (lines.Count > 0 ? Environment.NewLine : "");
            LogBox.ScrollToEnd();
            LogHeaderText.Text = path;
            return;
        }

        var added = _tail.ReadNew();
        if (added.Count > 0)
        {
            AppendLogText(string.Join(Environment.NewLine, added) + Environment.NewLine);
        }
    }

    private string? CurrentLogPath() => _logSource switch
    {
        1 => _rt.BackendLogPath ?? ProcessUtil.NewestFile(Paths.LogsDir, "backend-*.log"),
        2 => _rt.ServerLogPath ?? ProcessUtil.NewestFile(Paths.LogsDir, "server-*.log"),
        3 => Paths.LuaLog,
        4 => ProcessUtil.NewestRuntimeLog(Paths.RuntimeLogsDir),
        _ => null,
    };

    // ───────────── опрос состояния ─────────────

    private async Task RunTickAsync()
    {
        if (Interlocked.Exchange(ref _tickBusy, 1) == 1)
        {
            return;
        }

        try
        {
            _backendAlive = await _local.IsLauncherBackendAsync();
            var games = new List<GameDto>();
            if (_backendAlive)
            {
                var r = await _local.ServersAsync();
                if (r.Ok)
                {
                    games = r.Items;
                }

                if (!_accountsLoaded)
                {
                    await ReloadAccountsAsync();
                }
            }

            UpdateStatus(games);
            RebuildLocalRows(games);
            _lan.SetLocal(BuildAnnouncement(games));
        }
        catch (Exception ex)
        {
            Notify("Обновление состояния: " + ex.Message, warn: true);
        }
        finally
        {
            Interlocked.Exchange(ref _tickBusy, 0);
        }
    }

    private void UpdateStatus(List<GameDto> games)
    {
        var serverProcs = ProcessUtil.CountByName("bf3_server_Final");
        var bf3Procs = ProcessUtil.CountByName("bf3");
        var players = games.Sum(g => g.Players);
        var slots = games.Sum(g => g.MaxPlayers);

        BackendPill.Text = _backendAlive
            ? (_rt.BackendOwned ? "Бэкенд: работает" : "Бэкенд: работает (не лаунчером)")
            : "Бэкенд: не запущен";
        ServerPill.Text = serverProcs > 0
            ? (_rt.ServerOwned ? "Сервер: работает" : "Сервер: работает (не лаунчером)")
            : "Сервер: не запущен";
        ClientPill.Text = bf3Procs > 0 ? "Игра: bf3 запущен" : "Игра: не запущена";
        LanPill.Text = _s.OpenForLan ? "LAN: открыт, адрес " + AdvertiseIp() : "LAN: только этот ПК";

        BackendStateText.Text = (_backendAlive
                ? $"Бэкенд отвечает на 127.0.0.1:{Ports.Http}"
                : $"Бэкенд не отвечает на 127.0.0.1:{Ports.Http}")
            + (_rt.BackendOwned ? $" · PID {_rt.Backend?.Id}, запущен лаунчером" : "");
        ServerStateText.Text = $"Сервер: процессов bf3_server_Final {serverProcs}; игр в бэкенде {games.Count}"
            + (games.Count > 0 ? $"; игроков {players} из {slots}" : "")
            + (_rt.ServerOwned ? $" · загрузчик PID {_rt.ServerLoader?.Id}" : "");
        ClientStateText.Text = bf3Procs > 0
            ? "Клиент: bf3 запущен; второй клиент лаунчер не стартует."
            : "Клиент: bf3 не запущен.";
        LanStateText.Text = (_s.OpenForLan ? "LAN-режим: сервер объявляется в сети. " : "LAN-режим выключен: сервер виден только этому ПК. ")
            + (_lan.IsRunning ? "Объявления LAN работают." : "Объявления LAN выключены.");
    }

    private void RebuildLocalRows(List<GameDto> games)
    {
        var rows = games.Select(g => new ServerRow
        {
            IsLocal = true,
            Title = g.Name,
            Details = $"{g.Map} · {g.Mode} · игроков {g.Players}/{g.MaxPlayers}",
            Address = g.Address,
            Badge = _rt.ServerOwned ? "ЛАУНЧЕР" : "ЭТОТ ПК",
            Ip = "127.0.0.1",
            HttpPort = Ports.Http,
            GamePort = PortOf(g.Address),
            GameId = g.GameId,
        }).ToList();

        var sig = string.Join("\n", rows.Select(r => r.Signature));
        if (sig == _localSig)
        {
            return;
        }

        _localSig = sig;
        _localRows.Clear();
        foreach (var row in rows)
        {
            _localRows.Add(row);
        }

        LocalEmptyText.Visibility = rows.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
    }

    private void ShowNetwork(IReadOnlyList<NetworkServer> list)
    {
        var rows = list.Select(n => new ServerRow
        {
            IsLocal = false,
            Title = n.Server.Name,
            Details = $"{n.Server.Map} · {n.Server.Mode} · игроков {n.Server.Players}/{n.Server.MaxPlayers}"
                      + (n.Server.Bots >= 0 ? $" · ботов {n.Server.Bots}" : ""),
            Address = $"{n.HostIp}:{n.Server.GamePort}",
            Badge = "LAN · " + n.HostName,
            Ip = n.HostIp,
            HttpPort = n.HttpPort,
            GamePort = n.Server.GamePort,
            GameId = n.Server.GameId,
        }).ToList();

        var sig = string.Join("\n", rows.Select(r => r.Signature));
        if (sig != _networkSig)
        {
            _networkSig = sig;
            _networkRows.Clear();
            foreach (var row in rows)
            {
                _networkRows.Add(row);
            }
        }

        NetStatusText.Text = rows.Count == 0
            ? "В LAN серверов не найдено. Хост должен открыть сервер для LAN (вкладка «Сервер») и запустить лаунчер."
            : $"Найдено в LAN: {rows.Count}. Выберите строку, чтобы войти.";
    }

    /// <summary>Объявление для других ПК: только если сервер открыт для LAN и бэкенд отдаёт игры.</summary>
    private LanAnnouncement? BuildAnnouncement(List<GameDto> games)
    {
        if (!_s.OpenForLan || games.Count == 0)
        {
            return null;
        }

        return new LanAnnouncement
        {
            Host = Environment.MachineName,
            Ip = AdvertiseIp(),
            HttpPort = Ports.Http,
            Servers = games.Select(g => new AnnouncedServer
            {
                Name = g.Name,
                GameId = g.GameId,
                GamePort = PortOf(g.Address),
                Map = g.Map,
                Mode = g.Mode,
                Players = g.Players,
                MaxPlayers = g.MaxPlayers,
                Bots = _rt.ServerOwned ? _s.BotCount : -1,
            }).ToList(),
        };
    }

    private void StartDiscovery()
    {
        try
        {
            if (!_lan.IsRunning)
            {
                _lan.Start();
            }
        }
        catch (Exception ex)
        {
            Notify("LAN-объявления не запущены (UDP 47990): " + ex.Message, warn: true);
        }
    }

    private static int PortOf(string address)
    {
        var i = address.LastIndexOf(':');
        return i > 0 && int.TryParse(address[(i + 1)..], out var port) ? port : Ports.Game;
    }

    /// <summary>Адрес, который объявляет бэкенд: LAN-IP (если открыт для LAN) или 127.0.0.1.</summary>
    private string AdvertiseIp() => _s.OpenForLan ? ResolveLanIp() : "127.0.0.1";

    private string ResolveLanIp()
    {
        if (Validation.TryIPv4(_s.LanIp, out var ip) && ip is not null)
        {
            return ip.ToString();
        }

        return LanInfo.DefaultIp() ?? "127.0.0.1";
    }

    // ───────────── бэкенд и сервер ─────────────

    /// <summary>Бэкенд отвечает? Если нет — запускает свой (после проверки портов). true = готов.</summary>
    private async Task<bool> EnsureBackendAsync()
    {
        if (await _local.IsLauncherBackendAsync())
        {
            return true;
        }

        if (!File.Exists(Paths.BackendExe))
        {
            Notify("Не найден бэкенд: " + Paths.BackendExe, warn: true);
            return false;
        }

        var busy = await Task.Run(() => PortsBusy(new[]
        {
            (Ports.Http, true), (Ports.Blaze, true), (Ports.Redirector, true), (Ports.Qos, false),
        }));
        if (busy.Count > 0)
        {
            Notify("Порты бэкенда заняты: " + string.Join(", ", busy) +
                   ". Возможно, запущен другой бэкенд (стенд stand\\run или BF3NativeBots). Остановите его.", warn: true);
            return false;
        }

        var (ok, message) = _rt.StartBackend(_s, AdvertiseIp());
        Notify(message, warn: !ok);
        if (!ok)
        {
            return false;
        }

        var deadline = DateTime.UtcNow.AddSeconds(25);
        while (DateTime.UtcNow < deadline)
        {
            await Task.Delay(500);
            if (!ProcessUtil.IsAlive(_rt.Backend))
            {
                Notify("Бэкенд завершился сразу после запуска. Смотрите лог «Бэкенд».", warn: true);
                return false;
            }

            if (await _local.IsLauncherBackendAsync())
            {
                return true;
            }
        }

        Notify("Бэкенд не ответил за 25 с. Смотрите лог «Бэкенд».", warn: true);
        return false;
    }

    private async void StartServer_Click(object sender, RoutedEventArgs e) => await StartServerAsync();

    private async Task StartServerAsync()
    {
        if (!ReadAllFromUi(out var error))
        {
            Notify(error, warn: true);
            return;
        }

        _s.Save();
        SetBusy(true);
        try
        {
            if (ProcessUtil.CountByName("bf3_server_Final") > 0)
            {
                Notify(_rt.ServerOwned
                    ? "Сервер уже запущен этим лаунчером."
                    : "Сервер уже работает на этом ПК (запущен не лаунчером). Второй не запускаю.", warn: true);
                return;
            }

            var missing = MissingForServer();
            if (missing.Count > 0)
            {
                Notify("Не найдено: " + string.Join("; ", missing), warn: true);
                return;
            }

            var busy = await Task.Run(() => PortsBusy(new[] { (Ports.Game, true), (Ports.Game, false), (Ports.Rcon, true) }));
            if (busy.Count > 0)
            {
                Notify("Порты сервера заняты: " + string.Join(", ", busy) + ". Освободите их.", warn: true);
                return;
            }

            if (!await EnsureBackendAsync())
            {
                return;
            }

            if (_s.OpenForLan && !_rt.BackendOwned)
            {
                Notify("Внимание: бэкенд запущен не лаунчером, его адрес не LAN. Для LAN остановите его и запустите снова.", warn: true);
            }

            // имя, лимит и карта — в Startup.txt и MapList.txt инстанса через админ-API бэкенда
            var startupName = await _local.SetStartupAsync("vars.serverName", _s.ServerName);
            var startupPlayers = await _local.SetStartupAsync("vars.maxPlayers", _s.MaxPlayers.ToString());
            var mapList = await _local.SetMapListAsync(_s.Map, _s.Mode, 1);
            if (!startupName.Ok || !startupPlayers.Ok || !mapList.Ok)
            {
                var reason = !startupName.Ok ? startupName.Error : !startupPlayers.Ok ? startupPlayers.Error : mapList.Error;
                Notify("Бэкенд не принял настройки: " + reason, warn: true);
                return;
            }

            // число ботов — InitNumberOfBots в Config.lua мода (побайтово, с копией файла)
            try
            {
                var backup = await Task.Run(() => FunBotsConfig.Apply(
                    Paths.FunBotsConfig, _s.BotCount, Path.Combine(Paths.LogsDir, "config-backup")));
                Notify(backup.Length > 0
                    ? $"Боты: InitNumberOfBots = {_s.BotCount} (копия Config.lua: {Path.GetFileName(backup)})."
                    : $"Боты: InitNumberOfBots = {_s.BotCount} (без изменений).");
            }
            catch (Exception ex)
            {
                Notify("Config.lua мода не обновлён: " + ex.Message, warn: true);
                return;
            }

            var (ok, message) = _rt.StartServer(_s);
            Notify(message, warn: !ok);
            if (!ok)
            {
                return;
            }

            var deadline = DateTime.UtcNow.AddSeconds(150);
            while (DateTime.UtcNow < deadline)
            {
                await Task.Delay(2000);
                if (!ProcessUtil.IsAlive(_rt.ServerLoader))
                {
                    Notify("Сервер завершился сразу после запуска. Смотрите лог «Сервер (загрузчик)».", warn: true);
                    return;
                }

                var games = await _local.ServersAsync();
                if (games.Ok && games.Items.Count > 0)
                {
                    Notify("Сервер работает и зарегистрирован в бэкенде. Можно входить.");
                    return;
                }
            }

            Notify("Сервер запущен, но не зарегистрирован в бэкенде за 150 с. Смотрите логи.", warn: true);
        }
        finally
        {
            SetBusy(false);
        }
    }

    private async void StopServer_Click(object sender, RoutedEventArgs e) => await StopAsync(includeBackend: false);

    private async void StopAll_Click(object sender, RoutedEventArgs e) => await StopAsync(includeBackend: true);

    private async void StartBackend_Click(object sender, RoutedEventArgs e)
    {
        SetBusy(true);
        try
        {
            if (await EnsureBackendAsync())
            {
                Notify("Бэкенд работает.");
            }
        }
        finally
        {
            SetBusy(false);
        }
    }

    private async Task StopAsync(bool includeBackend)
    {
        SetBusy(true);
        try
        {
            var messages = await Task.Run(() => _rt.Stop(includeBackend));
            var text = string.Join("; ", messages);

            // проверка честная: если bf3_server_Final не дочерний процесс загрузчика, он может остаться
            var left = ProcessUtil.CountByName("bf3_server_Final");
            if (left > 0)
            {
                text += $". Внимание: bf3_server_Final ещё работает ({left} шт.). Лаунчер чужие процессы по имени не снимает, остановите вручную";
                Notify(text + ".", warn: true);
                return;
            }

            Notify(text + ".");
        }
        finally
        {
            SetBusy(false);
        }
    }

    private List<string> MissingForServer()
    {
        var list = new List<string>();
        if (!File.Exists(Paths.ServerExe))
        {
            list.Add("bf3_server_Final.exe");
        }

        if (!File.Exists(Paths.Loader))
        {
            list.Add(Paths.LoaderName);
        }

        if (!File.Exists(Paths.LicenseArtifact))
        {
            list.Add("license\\71067.dlf");
        }

        if (!File.Exists(Paths.EVBotsExtDeployed))
        {
            list.Add("EVBots.dll в комплекте");
        }

        if (!Directory.Exists(Paths.InstanceDir))
        {
            list.Add("инстанс сервера");
        }

        if (!File.Exists(Paths.FunBotsConfig))
        {
            list.Add("Config.lua мода fun-bots");
        }

        return list;
    }

    /// <summary>Занятые порты из списка (пробный бинд). Ничего не трогает.</summary>
    private static List<string> PortsBusy(IEnumerable<(int Port, bool Tcp)> ports)
    {
        var busy = new List<string>();
        foreach (var (port, tcp) in ports)
        {
            if (Ports.Busy(port, tcp) is not null)
            {
                busy.Add($"{(tcp ? "TCP" : "UDP")} {port}");
            }
        }

        return busy;
    }

    // ───────────── вход в игру ─────────────

    private void LocalList_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (LocalList.SelectedItem is ServerRow row)
        {
            NetworkList.SelectedItem = null;
            SetTarget(row);
        }
    }

    private void NetworkList_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (NetworkList.SelectedItem is ServerRow row)
        {
            LocalList.SelectedItem = null;
            SetTarget(row);
        }
    }

    private void SetTarget(ServerRow row)
    {
        _target = row;
        TargetText.Text = $"{row.Title} · {row.Address} · {(row.IsLocal ? "этот ПК" : "LAN: " + row.Ip)}";
        JoinStatusText.Text = "";
    }

    private async void Join_Click(object sender, RoutedEventArgs e) => await JoinAsync();

    /// <summary>Тот же путь, что run\join.ps1: gameId из GET /servers, personaref аккаунта, serveraddr хоста.</summary>
    private async Task JoinAsync()
    {
        var target = _target;
        if (target is null)
        {
            JoinStatusText.Text = "Сначала выберите сервер во вкладке «Серверы».";
            return;
        }

        var login = JoinLoginBox.Text.Trim();
        var password = JoinPasswordBox.Password;
        var loginError = Validation.Login(login);
        if (loginError is not null)
        {
            JoinStatusText.Text = loginError;
            return;
        }

        if (ProcessUtil.CountByName("bf3") > 0)
        {
            JoinStatusText.Text = "Игра bf3 уже запущена. Второй клиент не стартую.";
            return;
        }

        SetBusy(true);
        try
        {
            if (target.IsLocal && !await EnsureBackendAsync())
            {
                return;
            }

            var host = new BackendClient(target.Ip, target.HttpPort);
            var games = await host.ServersAsync();
            if (!games.Ok)
            {
                JoinStatusText.Text = $"Нет связи с хостом {target.Ip}:{target.HttpPort} ({games.Error}). " +
                                      "Для LAN хост должен открыть порты (вкладка «Настройки», кнопка портов).";
                return;
            }

            var game = games.Items.FirstOrDefault(g => g.GameId == target.GameId) ?? games.Items.FirstOrDefault();
            if (game is null)
            {
                JoinStatusText.Text = "На хосте сервер не зарегистрирован в бэкенде. Он ещё запускается или остановлен.";
                return;
            }

            ulong personaref;
            if (target.IsLocal && password.Length == 0)
            {
                if (_accounts.Count == 0)
                {
                    await ReloadAccountsAsync();
                }

                var account = _accounts.FirstOrDefault(a => string.Equals(a.Login, login, StringComparison.OrdinalIgnoreCase));
                if (account is null)
                {
                    JoinStatusText.Text = $"Аккаунт «{login}» не найден на этом ПК. Создайте его во вкладке «Аккаунты».";
                    return;
                }

                personaref = account.Personaref;
            }
            else
            {
                var passwordError = Validation.Password(password);
                if (passwordError is not null)
                {
                    JoinStatusText.Text = passwordError;
                    return;
                }

                // аккаунт на хосте: создаётся, если его нет, иначе пароль должен совпасть
                var ensured = await host.EnsureAccountAsync(login, password, null);
                if (!ensured.Ok)
                {
                    JoinStatusText.Text = "Вход отклонён хостом: " + ensured.Error;
                    return;
                }

                personaref = ensured.Personaref;
            }

            var plan = ClientPlan.Join(_s, target.Ip, game.GameId, personaref, target.GamePort);
            var (ok, message) = _rt.StartClient(plan);
            JoinStatusText.Text = message;
            Notify(message, warn: !ok);
            if (ok)
            {
                _s.LastLogin = login;
                _s.Save();
            }
        }
        finally
        {
            SetBusy(false);
        }
    }

    // ───────────── аккаунты ─────────────

    private async Task ReloadAccountsAsync()
    {
        var r = await _local.AccountsAsync();
        if (!r.Ok)
        {
            AccountsStatusText.Text = "Бэкенд не отвечает: " + r.Error + ". Запустите бэкенд (вкладка «Сервер»).";
            return;
        }

        _accountsLoaded = true;
        _accounts.Clear();
        _accounts.AddRange(r.Items);
        _accountRows.Clear();
        foreach (var a in r.Items.OrderBy(x => x.Login, StringComparer.OrdinalIgnoreCase))
        {
            _accountRows.Add(new AccountRow
            {
                Login = a.Login,
                Personaref = a.Personaref.ToString(),
                Soldier = a.Name,
                Soldiers = a.Soldiers.ToString(),
            });
        }

        AccountsStatusText.Text = $"Аккаунтов на этом ПК: {r.Items.Count}.";
    }

    private async void RefreshAccounts_Click(object sender, RoutedEventArgs e) => await ReloadAccountsAsync();

    private void AccountsList_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (AccountsList.SelectedItem is AccountRow row)
        {
            JoinLoginBox.Text = row.Login;
        }
    }

    private async void CreateAccount_Click(object sender, RoutedEventArgs e) => await CreateAccountAsync();

    private async Task CreateAccountAsync()
    {
        var login = NewLoginBox.Text.Trim();
        var password = NewPasswordBox.Password;
        var name = NewNameBox.Text.Trim();
        var error = Validation.Login(login) ?? Validation.Password(password);
        if (error is not null)
        {
            AccountsStatusText.Text = error;
            return;
        }

        SetBusy(true);
        try
        {
            if (!await EnsureBackendAsync())
            {
                return;
            }

            var r = await _local.EnsureAccountAsync(login, password, name.Length > 0 ? name : null);
            if (!r.Ok)
            {
                AccountsStatusText.Text = "Не создан: " + r.Error;
                return;
            }

            NewPasswordBox.Clear();
            Notify($"Аккаунт «{login}»: personaref {r.Personaref}.");
            await ReloadAccountsAsync();
        }
        finally
        {
            SetBusy(false);
        }
    }

    private async void DeleteAccount_Click(object sender, RoutedEventArgs e) => await DeleteAccountAsync();

    private async Task DeleteAccountAsync()
    {
        if (AccountsList.SelectedItem is not AccountRow row)
        {
            AccountsStatusText.Text = "Выберите аккаунт в списке.";
            return;
        }

        var keep = KeepSoldierCheck.IsChecked == true;
        var question = keep
            ? $"Удалить аккаунт «{row.Login}»? Солдат и его прогресс сохранятся."
            : $"Удалить аккаунт «{row.Login}» вместе с солдатом и прогрессом?";
        if (MessageBox.Show(this, question + "\n\nОтменить нельзя.", "Удаление аккаунта",
                MessageBoxButton.YesNo, MessageBoxImage.Warning) != MessageBoxResult.Yes)
        {
            return;
        }

        SetBusy(true);
        try
        {
            if (!await EnsureBackendAsync())
            {
                return;
            }

            var r = await _local.DeleteAccountAsync(row.Login, keep);
            if (!r.Ok)
            {
                AccountsStatusText.Text = "Не удалён: " + r.Error;
                return;
            }

            Notify($"Аккаунт «{row.Login}» удалён.");
            await ReloadAccountsAsync();
        }
        finally
        {
            SetBusy(false);
        }
    }

    private void SetBusy(bool busy)
    {
        StartServerBtn.IsEnabled = !busy;
        JoinBtn.IsEnabled = !busy;
        CreateAccountBtn.IsEnabled = !busy;
        DeleteAccountBtn.IsEnabled = !busy;
    }

    // ───────────── настройки и LAN ─────────────

    private void SaveSettings_Click(object sender, RoutedEventArgs e)
    {
        if (!ReadAllFromUi(out var error))
        {
            SettingsStatusText.Text = error;
            Notify(error, warn: true);
            return;
        }

        _s.Save();
        ApplyGlass();
        if (_s.DiscoveryEnabled)
        {
            StartDiscovery();
        }
        else if (_lan.IsRunning)
        {
            _lan.Dispose();
        }

        SettingsStatusText.Text = "Сохранено: " + LauncherSettings.FilePath;
        Notify("Настройки сохранены.");
        _ = RunTickAsync();
    }

    private async void LanCheck_Click(object sender, RoutedEventArgs e)
    {
        var ip = ResolveLanIp();
        SettingsStatusText.Text = $"Проверяю http://{ip}:{Ports.Http}/ ...";
        var ok = await new BackendClient(ip, Ports.Http).IsLauncherBackendAsync();
        SettingsStatusText.Text = ok
            ? $"HTTP на {ip}:{Ports.Http} отвечает: гости смогут войти и зарегистрироваться."
            : $"HTTP на {ip}:{Ports.Http} не отвечает. Нужны права URL-ACL (кнопка ниже) и запущенный бэкенд.";
    }

    /// <summary>Команды для хоста: URL-ACL и правила брандмауэра. Выполняются в окне cmd с запросом UAC.</summary>
    private void LanPorts_Click(object sender, RoutedEventArgs e)
    {
        var ip = ResolveLanIp();
        var user = Environment.UserName;
        var text =
            "Будут выполнены команды (нужны права администратора, появится запрос UAC):\n\n" +
            $"1) netsh http add urlacl url=http://{ip}:{Ports.Http}/ user={user}\n" +
            "2) правила брандмауэра: TCP 8080, 10041, 42127, 25300 и UDP 47990, 17554 (профиль «частная сеть»).\n\nВыполнить?";
        if (MessageBox.Show(this, text, "Порты для LAN", MessageBoxButton.YesNo, MessageBoxImage.Question) != MessageBoxResult.Yes)
        {
            return;
        }

        var script =
            $"netsh http add urlacl url=http://{ip}:{Ports.Http}/ user={user} & " +
            "netsh advfirewall firewall add rule name=\"EVBots LAN TCP\" dir=in action=allow protocol=TCP " +
            "localport=8080,10041,42127,25300 profile=private & " +
            "netsh advfirewall firewall add rule name=\"EVBots LAN UDP\" dir=in action=allow protocol=UDP " +
            "localport=47990,17554 profile=private & echo. & echo Готово. Закройте окно.";
        try
        {
            Process.Start(new ProcessStartInfo("cmd.exe", "/k " + script) { Verb = "runas", UseShellExecute = true })?.Dispose();
            SettingsStatusText.Text = "Окно с командами открыто. Подтвердите UAC.";
        }
        catch (Exception ex)
        {
            SettingsStatusText.Text = "Не выполнено: " + ex.Message;
        }
    }
}
