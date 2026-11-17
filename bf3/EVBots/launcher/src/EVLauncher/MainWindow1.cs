using System.Diagnostics;
using System.IO;
using System.Net.Http;
using System.Text;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Threading;
using EVBots.Core;

namespace EVBots.Launcher;

public partial class MainWindow : Window
{
    private static readonly HttpClient Http = new(new HttpClientHandler { UseProxy = false })
    {
        Timeout = TimeSpan.FromSeconds(5),
    };

    private readonly FriendSettings _s = FriendSettings.Load();
    private readonly HostRunner _host = new();
    private FoundPaths _p = new();
    private List<MapMode> _all = new();
    private List<string> _modesShown = new();  // 19.10: режимы в ModeBox (по индексу)
    private List<string> _mapsShown = new();   // 19.10: карты выбранного режима в MapBox (по индексу)
    private readonly DispatcherTimer _tick = new() { Interval = TimeSpan.FromSeconds(3) };
    private bool _busy;
    private bool _hosting;        // 29.10: идёт «Поднять сервер»
    private bool _joinAfterHost;  // 29.10: «Войти в игру» нажали во время подъёма — войти, когда сервер готов
    private long _logOffset;

    public MainWindow(bool preview = false)
    {
        InitializeComponent();
        InitializeProfiles();
        HostBox.Text = _s.HostIp;
        LoginBox.Text = _s.Login;
        PasswordBox.Password = _s.Password;
        NickBox.Text = _s.Nick;
        ServerNameBox.Text = _s.ServerName;
        _tick.Tick += async (_, _) => await TickAsync();
        FitToScreen();
        if (preview)
        {
            ServerPick.Items.Add("Этот ПК");
            ServerPick.SelectedIndex = 0;
            SetStatus("Предпросмотр оформления · без запуска игры и сетевых служб");
            AuthPanel.IsHitTestVisible = false;
            System.Windows.Input.KeyboardNavigation.SetTabNavigation(AuthPanel, System.Windows.Input.KeyboardNavigationMode.None);
        }
        else Loaded += (_, _) => _ = StartupAsync();
    }

    /// <summary>
    /// 19.10: окно не выше и не шире рабочей области экрана. У друга (ноутбук, масштаб Windows) окно 680 выходило
    /// за экран, блок «Войти в игру» сплющивался и кнопка пропадала.
    /// </summary>
    private void FitToScreen()
    {
        var wa = SystemParameters.WorkArea;
        if (Height > wa.Height) Height = wa.Height;
        if (Width > wa.Width) Width = wa.Width;
        if (MinHeight > Height) MinHeight = Height;
        if (MinWidth > Width) MinWidth = Width;
    }

    private void Log(string line)
    {
        Dispatcher.Invoke(() =>
        {
            LogBox.AppendText($"[{DateTime.Now:HH:mm:ss}] {line}{Environment.NewLine}");
            if (LogBox.Text.Length > 300_000) LogBox.Text = LogBox.Text[^200_000..];
            LogBox.ScrollToEnd();
        });
    }

    private void SetStatus(string t) => Dispatcher.Invoke(() => StatusText.Text = t);

    /// <summary>
    /// 26.10 (238-6): фон окна — картинка BF3 из BF-Portable\htdocs\img этого ПК (в комплект не кладём: у хоста и гостей
    /// BF-Portable свой). Нет BF-Portable или файла — фон прежний. Читаем в память (файл не держим).
    /// </summary>
    private void LoadBackground()
    {
        // Встроенное оформление имеет приоритет над прежними внешними обоями.
        if (BgImage.Source != null) return;
        try
        {
            var roots = new[] { _p.ServerDir.Length > 0 ? Path.GetDirectoryName(_p.ServerDir) ?? "" : "", _p.Root };
            var names = new[] { "Wallpaper-04-1080p.jpg", "4182771-battlefield-3-HD.jpg", "BF3_Operation_Metro.jpg", "19068-oboi-battlefield-3.jpg" };
            foreach (var root in roots)
            {
                if (root.Length == 0) continue;
                foreach (var dir in new[] { Path.Combine(root, "htdocs", "img"), Path.Combine(root, "BF-Portable", "htdocs", "img"),
                                            Path.Combine(root, "Battlefield Portable", "htdocs", "img") })
                {
                    foreach (var n in names)
                    {
                        var f = Path.Combine(dir, n);
                        if (!File.Exists(f)) continue;
                        var bmp = new System.Windows.Media.Imaging.BitmapImage();
                        bmp.BeginInit();
                        bmp.CacheOption = System.Windows.Media.Imaging.BitmapCacheOption.OnLoad;
                        bmp.DecodePixelWidth = 1600;
                        bmp.UriSource = new Uri(f);
                        bmp.EndInit();
                        bmp.Freeze();
                        BgImage.Source = bmp;
                        return;
                    }
                }
            }
        }
        catch (Exception ex) { Log("Фон окна не загружен: " + ex.Message); }
    }

    private async Task StartupAsync()
    {
        await Task.Run(() => _p = PathFinder.Find());
        // Папку EVBots друг мог выбрать вручную раньше: берём её в первую очередь
        if (_s.StandDir.Length > 0 && Directory.Exists(_s.StandDir)) _p.StandDir = _s.StandDir;
        LoadBackground();
        Log("Корень: " + _p.Root);
        if (!File.Exists(Path.Combine(_p.ClientGameDir, "bf3.exe")))
        {
            SetStatus("Не нашёл Battlefield 3 рядом. Положите лаунчер туда, где Battlefield 3.");
            return;
        }
        if (_p.ServerDir.Length == 0)
        {
            Log("BF-Portable не найден — это нормально: ты будешь входить к другу, а не хостить.");
            HostBtn.IsEnabled = false;
            HostBtn.ToolTip = "Нужен BF-Portable рядом с игрой — это только для хоста.";
        }
        // 20.10: файлы ставит только установщик EVBots-Setup — лаунчер ничего не копирует (старая установка из
        // payload убрана: она могла класть файлы в мод сервера).
        Log("Папка EVBots: " + _p.StandDir);
        FillMaps();
        ShowMyIp();
        StartLan();
        // 18.10: у друга не было правил брандмауэра (окно разрешения не появлялось) — нет правил, сразу предлагаем настроить
        var fwOk = await Task.Run(() => FirewallRuleExists("EVBots-TCP") && FirewallRuleExists("EVBots-UDP"));
        if (!fwOk)
        {
            Log("Брандмауэр: правил EVBots нет — предлагаю настроить (окно UAC).");
            Firewall_Click(this, new RoutedEventArgs());
        }
        // Тик и список моих серверов стартуют только после входа (EnterMain).
        SetStatus("Выбери сервер в списке, введи логин и пароль (или зарегистрируйся).");
    }

    private void FillMaps()
    {
        Dispatcher.Invoke(() =>
        {
            // 20.10: значения раньше режимов — Mode_Changed сразу показывает правило выбранного режима
            _ruleUi = true;
            TicketsBox.ItemsSource = TicketSteps.Select(v => v + " %").ToList();
            TimeBox.ItemsSource = TimeSteps.Select(v => v + " %").ToList();
            _ruleUi = false;
            _all = GameData.ForProfile(SelectedProfile, Path.Combine(_p.InstanceDir, "Admin", "Mods", "fun-bots"));
            _modesShown = GameData.AllModes(_all);
            if (_modesShown.Count == 0)
            {
                _modesShown = new List<string> { _s.Mode };
                _mapsShown = new List<string> { _s.Map };
                ModeBox.ItemsSource = _modesShown.Select(GameData.ModeLabel).ToList();
                MapBox.ItemsSource = _mapsShown.Select(GameData.MapLabel).ToList();
                ModeBox.SelectedIndex = 0; MapBox.SelectedIndex = 0;
                Log("ВНИМАНИЕ: mapfiles мода не найдены.");
            }
            else
            {
                // 19.10: сначала режим, карты — только с путями мода под этот режим.
                ModeBox.ItemsSource = _modesShown.Select(GameData.ModeLabel).ToList();
                ModeBox.SelectedIndex = Math.Max(0, _modesShown.IndexOf(_s.Mode));  // Mode_Changed заполнит карты
                if (_mapsShown.Count == 0) FillMapsForMode(_modesShown[ModeBox.SelectedIndex]);
            }
            BotsBox.ItemsSource = Enumerable.Range(0, 33).Select(n => n.ToString()).ToList();
            BotsBox.SelectedIndex = Math.Clamp(_s.Bots, 0, 32);
            FreqBox.ItemsSource = Frequencies.Select(hz => hz + " Гц").ToList();
            FreqBox.SelectedIndex = Math.Max(0, Array.IndexOf(Frequencies, HostRunner.NormFrequency(_s.Frequency)));
            _jetsUi = true;
            JetsBox.IsChecked = _s.BotsJets;
            SkillBox.ItemsSource = SkillLevels.Select(l => l.Label).ToList();
            SkillBox.SelectedIndex = Math.Clamp(_s.BotsSkill, 0, SkillLevels.Length - 1);
            DefendBox.IsChecked = _s.BotsDefend;
            _jetsUi = false;
            RoundsBox.ItemsSource = Enumerable.Range(1, 9).Select(n => n.ToString()).ToList();
            RoundsBox.SelectedIndex = Math.Clamp(_s.Rounds, 1, 9) - 1;
            RotationList.ItemsSource = null;
            RotationList.ItemsSource = _s.Rotation.Select(RotationLabel).ToList();
            if (SelectedProfile.IsRoyale) RotationList.ItemsSource = new[] { RotationLabel($"{ModProfiles.RoyaleMap} {ModProfiles.RoyaleMode} 1") };
            UpdateProfileControls();
        });
    }

    /// <summary>26.10: частоты движка в выборе «Частота» (режимы VU regular/high60/high120).</summary>
    private static readonly int[] Frequencies = { 30, 60, 120 };

    private void FillMapsForMode(string mode)
    {
        _mapsShown = GameData.MapsForMode(_all, mode);
        if (_mapsShown.Count == 0) _mapsShown = new List<string> { _s.Map };
        MapBox.ItemsSource = _mapsShown.Select(GameData.MapLabel).ToList();
        MapBox.SelectedIndex = Math.Max(0, _mapsShown.IndexOf(_s.Map));
    }

    private void Mode_Changed(object sender, SelectionChangedEventArgs e)
    {
        if (ModeBox.SelectedIndex >= 0 && ModeBox.SelectedIndex < _modesShown.Count) ShowRule(_modesShown[ModeBox.SelectedIndex]);
        if (_all.Count == 0 || ModeBox.SelectedIndex < 0 || ModeBox.SelectedIndex >= _modesShown.Count) return;
        FillMapsForMode(_modesShown[ModeBox.SelectedIndex]);
    }

    private void ShowMyIp()
    {
        var ip = MyLanIp() ?? "127.0.0.1";
        Dispatcher.Invoke(() => MyIpText.Text = $"Мой IP для друзей: {ip}");
    }

    /// <summary>
    /// LAN-адрес этого ПК: физический адаптер с основным шлюзом. VPN и туннели (happ, TAP) отсекаются:
    /// бэкенд рекламирует именно этот адрес, и клиент до VPN-адреса не достучивается.
    /// </summary>
    private static string? MyLanIp() => LanInfo.DefaultIp();
}
