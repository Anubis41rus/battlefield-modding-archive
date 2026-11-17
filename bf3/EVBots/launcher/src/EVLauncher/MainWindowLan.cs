// Поиск серверов в сети без ввода IP.
// Хост с готовым сервером объявляет его на UDP 47990 (протокол EVBots.Core, класс LanDiscovery),
// гость видит такие серверы в списке «Сервер» на первом экране и входит, IP не вводя.
// Тот же протокол, что у вкладки «Серверы → Сеть LAN» в EVBots: два лаунчера видят друг друга.
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Threading;
using EVBots.Core;

namespace EVBots.Launcher;

public partial class MainWindow
{
    /// <summary>Адрес бэкенда на этом ПК.</summary>
    private const string LocalIp = "127.0.0.1";

    /// <summary>Пункт списка «Сервер»: этот ПК, хост из сети или ручной ввод.</summary>
    private sealed record PickItem(string Title, string Ip, bool Manual)
    {
        public override string ToString() => Title;
    }

    private LanDiscovery? _lan;
    private readonly DispatcherTimer _lanLocal = new() { Interval = TimeSpan.FromSeconds(3) };
    private IReadOnlyList<NetworkServer> _found = Array.Empty<NetworkServer>();
    private readonly HashSet<string> _seenHosts = new();
    private bool _localBackendUp;
    private bool _pickedByUser;
    private bool _pickUpdating;
    private string _pickSig = "";

    /// <summary>Старт: слушаем объявления и раз в 3 с объявляем свой бэкенд, если он поднят и есть серверы.</summary>
    private void StartLan()
    {
        try
        {
            _lan = new LanDiscovery();
            // Changed приходит из фонового потока: список перестраиваем в потоке окна
            _lan.Changed += found => Dispatcher.BeginInvoke(new Action(() =>
            {
                _found = found;
                RebuildPicks();
            }));
            _lan.Start();
        }
        catch (Exception ex)
        {
            _lan = null;
            Log("Поиск серверов в сети не работает (" + ex.Message + "). Впиши IP вручную.");
        }

        _lanLocal.Tick += async (_, _) => await PublishLocalAsync();
        _lanLocal.Start();
        _ = PublishLocalAsync();
        RebuildPicks();
    }

    /// <summary>Раз в 3 с: бэкенд на этом ПК отвечает и есть готовые серверы — объявляем их в сеть.</summary>
    private async Task PublishLocalAsync()
    {
        LanAnnouncement? ann = null;
        var up = false;
        try
        {
            var txt = await Http.GetStringAsync($"http://{LocalIp}:8080/servers");
            up = true;
            var servers = ReadFinalized(txt);
            if (servers.Count > 0)
            {
                ann = new LanAnnouncement
                {
                    Host = Environment.MachineName,
                    Ip = LanInfo.DefaultIp() ?? "",
                    Servers = servers,
                };
            }
        }
        catch (Exception)
        {
            // бэкенда на этом ПК нет — объявлять нечего
        }

        _lan?.SetLocal(ann);
        if (up != _localBackendUp)
        {
            _localBackendUp = up;
            RebuildPicks();
        }
    }

    /// <summary>Готовые серверы (finalized) из GET /servers бэкенда, в формате объявления.</summary>
    private static List<AnnouncedServer> ReadFinalized(string json)
    {
        var res = new List<AnnouncedServer>();
        using var doc = JsonDocument.Parse(json);
        if (!doc.RootElement.TryGetProperty("servers", out var arr)) return res;
        foreach (var s in arr.EnumerateArray())
        {
            if (!(s.TryGetProperty("finalized", out var f) && f.ValueKind == JsonValueKind.True)) continue;
            var name = JsonField(s, "name");
            res.Add(new AnnouncedServer
            {
                Name = name.Length > 0 ? name : "Сервер",
                GameId = ulong.TryParse(JsonField(s, "gameId"), out var gid) ? gid : 0,
                Map = JsonField(s, "map"),
                Mode = JsonField(s, "mode"),
                Players = int.TryParse(JsonField(s, "players"), out var p) ? p : 0,
                MaxPlayers = int.TryParse(JsonField(s, "maxPlayers"), out var mp) ? mp : 0,
            });
        }
        return res;
    }

    /// <summary>Поле JSON как строка: число и строка читаются одинаково.</summary>
    private static string JsonField(JsonElement s, string name) =>
        s.TryGetProperty(name, out var v)
            ? (v.ValueKind == JsonValueKind.String ? v.GetString() ?? "" : v.GetRawText())
            : "";

    /// <summary>
    /// Список «Сервер»: этот ПК, хосты из сети, ручной ввод. Перерисовываем только при изменении,
    /// иначе раскрытый список схлопывается.
    /// </summary>
    private void RebuildPicks()
    {
        var hosts = _found.Where(n => n.HostIp.Length > 0).GroupBy(n => n.HostIp).ToList();
        var items = new List<PickItem>
        {
            new PickItem($"Этот ПК — я хост ({LanInfo.DefaultIp() ?? LocalIp})", LocalIp, false),
        };
        foreach (var g in hosts)
        {
            var first = g.First();
            var sv = first.Server;
            items.Add(new PickItem(
                $"{first.HostName} · {sv.Name} · {sv.Map} · {sv.Players}/{sv.MaxPlayers} ({g.Key})",
                g.Key, false));
        }
        items.Add(new PickItem("Ввести IP вручную…", "", true));

        var sig = _localBackendUp + "|" + string.Join("|", items.Select(i => i.Ip + "=" + i.Title));
        if (sig == _pickSig) return;
        _pickSig = sig;

        foreach (var g in hosts)
            if (_seenHosts.Add(g.Key))
                Log($"В сети найден сервер: {g.First().HostName} ({g.Key})");

        var prev = ServerPick.SelectedItem as PickItem;
        var keep = _pickedByUser && prev is not null
            ? items.FirstOrDefault(i => i.Ip == prev.Ip && i.Manual == prev.Manual)
            : null;
        _pickUpdating = true;
        ServerPick.ItemsSource = items;
        ServerPick.SelectedItem = keep ?? DefaultPick(items);
        _pickUpdating = false;
        ApplyPick();
    }

    /// <summary>
    /// По умолчанию: бэкенда на этом ПК нет, а в сети есть хост — входим к нему (гость);
    /// иначе выбран «Этот ПК» (хост).
    /// </summary>
    private PickItem DefaultPick(List<PickItem> items)
    {
        var remote = items.FirstOrDefault(i => !i.Manual && i.Ip != LocalIp);
        return !_localBackendUp && remote is not null ? remote : items[0];
    }

    private void ServerPick_Changed(object sender, SelectionChangedEventArgs e)
    {
        if (_pickUpdating) return;
        _pickedByUser = true;
        ApplyPick();
    }

    /// <summary>Поле «IP вручную» видно только при выборе «Ввести IP вручную…».</summary>
    private void ApplyPick()
    {
        var manual = (ServerPick.SelectedItem as PickItem)?.Manual == true;
        ManualLabel.Visibility = manual ? Visibility.Visible : Visibility.Collapsed;
        HostBox.Visibility = manual ? Visibility.Visible : Visibility.Collapsed;
    }
}
