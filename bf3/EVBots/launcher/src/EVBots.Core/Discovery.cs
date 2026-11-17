using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace EVBots.Core;

/// <summary>Сервер в объявлении: одна запись из GET /servers бэкенда хоста.</summary>
public sealed class AnnouncedServer
{
    [JsonPropertyName("name")] public string Name { get; set; } = "";
    [JsonPropertyName("gameId")] public ulong GameId { get; set; }
    [JsonPropertyName("gamePort")] public int GamePort { get; set; } = Ports.Game;
    [JsonPropertyName("map")] public string Map { get; set; } = "";
    [JsonPropertyName("mode")] public string Mode { get; set; } = "";
    [JsonPropertyName("players")] public int Players { get; set; }
    [JsonPropertyName("maxPlayers")] public int MaxPlayers { get; set; }

    /// <summary>Число ботов из настроек запуска; -1 = неизвестно (сервер запущен не лаунчером).</summary>
    [JsonPropertyName("bots")] public int Bots { get; set; } = -1;
}

/// <summary>Объявление лаунчера: JSON в UDP-пакете (UTF-8), рассылка на broadcast-адреса LAN.</summary>
public sealed class LanAnnouncement
{
    [JsonPropertyName("app")] public string App { get; set; } = LanDiscovery.AppTag;
    [JsonPropertyName("v")] public int Version { get; set; } = 1;

    /// <summary>Случайный id запуска лаунчера: свои объявления пропускаем.</summary>
    [JsonPropertyName("inst")] public string Instance { get; set; } = "";
    [JsonPropertyName("host")] public string Host { get; set; } = "";

    /// <summary>LAN-адрес хоста: по нему клиент идёт на редиректор, Blaze и HTTP.</summary>
    [JsonPropertyName("ip")] public string Ip { get; set; } = "";
    [JsonPropertyName("httpPort")] public int HttpPort { get; set; } = Ports.Http;
    [JsonPropertyName("servers")] public List<AnnouncedServer> Servers { get; set; } = new();
}

/// <summary>Сетевой сервер, найденный по объявлению другого ПК.</summary>
public sealed class NetworkServer
{
    public string HostIp { get; init; } = "";
    public int HttpPort { get; init; } = Ports.Http;
    public string HostName { get; init; } = "";
    public AnnouncedServer Server { get; init; } = new();
    public DateTime LastSeenUtc { get; set; }

    public string Key => $"{HostIp}:{Server.GamePort}:{Server.GameId}";

    /// <summary>Подпись содержимого: изменилась — список надо перерисовать.</summary>
    public string Signature =>
        $"{HostName}|{HttpPort}|{Server.Name}|{Server.Map}|{Server.Mode}|{Server.Players}|{Server.MaxPlayers}|{Server.Bots}";
}

/// <summary>
/// LAN-объявления лаунчеров. Каждый лаунчер, у которого есть серверы, раз в несколько секунд
/// рассылает их на UDP 47990 (broadcast каждой LAN-сети) и слушает объявления других.
/// Объявления старше 12 секунд удаляются. Бэкенд не нужен и не меняется.
/// </summary>
public sealed class LanDiscovery : IDisposable
{
    public const string AppTag = "evbots-launcher";
    public static readonly TimeSpan SendEvery = TimeSpan.FromSeconds(3);
    public static readonly TimeSpan Ttl = TimeSpan.FromSeconds(12);

    private readonly object _gate = new();
    private readonly Dictionary<string, NetworkServer> _seen = new();
    private UdpClient? _udp;
    private CancellationTokenSource? _cts;
    private System.Threading.Timer? _timer;
    private LanAnnouncement? _local;

    public string InstanceId { get; } = Guid.NewGuid().ToString("N");

    public bool IsRunning => _udp is not null;

    /// <summary>Список сетевых серверов изменился. Вызывается из фоновых потоков.</summary>
    public event Action<IReadOnlyList<NetworkServer>>? Changed;

    public void Start()
    {
        if (_udp is not null)
        {
            return;
        }

        var udp = new UdpClient(AddressFamily.InterNetwork);
        try
        {
            // SO_REUSEADDR: два лаунчера на одном ПК могут слушать один порт
            udp.Client.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true);
            udp.Client.Bind(new IPEndPoint(IPAddress.Any, Ports.Discovery));
            udp.EnableBroadcast = true;
        }
        catch (Exception)
        {
            udp.Dispose();
            throw;
        }

        _udp = udp;
        _cts = new CancellationTokenSource();
        _ = Task.Run(() => ReceiveLoopAsync(udp, _cts.Token));
        _timer = new System.Threading.Timer(_ => Tick(), null, TimeSpan.FromSeconds(1), SendEvery);
    }

    /// <summary>Свои серверы для рассылки. null = сейчас объявлять нечего.</summary>
    public void SetLocal(LanAnnouncement? announcement)
    {
        if (announcement is not null)
        {
            announcement.Instance = InstanceId;
        }

        lock (_gate)
        {
            _local = announcement;
        }
    }

    /// <summary>Разбор объявления. Возвращает null, если пакет чужой или битый.</summary>
    public static LanAnnouncement? Parse(byte[] data)
    {
        try
        {
            var ann = JsonSerializer.Deserialize<LanAnnouncement>(data.AsSpan());
            return ann is not null && ann.App == AppTag && ann.Version == 1 ? ann : null;
        }
        catch (JsonException)
        {
            return null;
        }
    }

    private void Tick()
    {
        LanAnnouncement? local;
        lock (_gate)
        {
            local = _local;
        }

        var udp = _udp;
        if (udp is not null && local is not null && local.Servers.Count > 0)
        {
            var bytes = Encoding.UTF8.GetBytes(JsonSerializer.Serialize(local));
            foreach (var target in LanInfo.BroadcastTargets())
            {
                try
                {
                    udp.Send(bytes, bytes.Length, new IPEndPoint(target, Ports.Discovery));
                }
                catch (SocketException)
                {
                    // этот адаптер сейчас недоступен — пробуем остальные
                }
                catch (ObjectDisposedException)
                {
                    return;
                }
            }
        }

        if (Prune())
        {
            RaiseChanged();
        }
    }

    private async Task ReceiveLoopAsync(UdpClient udp, CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            UdpReceiveResult res;
            try
            {
                res = await udp.ReceiveAsync(ct);
            }
            catch (OperationCanceledException)
            {
                return;
            }
            catch (ObjectDisposedException)
            {
                return;
            }
            catch (SocketException)
            {
                await Task.Delay(500);
                continue;
            }

            var ann = Parse(res.Buffer);
            if (ann is null || ann.Instance == InstanceId)
            {
                continue;
            }

            var ip = string.IsNullOrWhiteSpace(ann.Ip) ? res.RemoteEndPoint.Address.ToString() : ann.Ip;

            // сервер на этом же ПК уже виден в локальном списке (бэкенд 127.0.0.1)
            if (LanInfo.LocalIPv4().Contains(ip))
            {
                continue;
            }

            var now = DateTime.UtcNow;
            var changed = false;
            lock (_gate)
            {
                foreach (var s in ann.Servers)
                {
                    var item = new NetworkServer
                    {
                        HostIp = ip,
                        HttpPort = ann.HttpPort,
                        HostName = ann.Host,
                        Server = s,
                        LastSeenUtc = now,
                    };

                    if (!_seen.TryGetValue(item.Key, out var old) || old.Signature != item.Signature)
                    {
                        changed = true;
                    }

                    _seen[item.Key] = item;
                }
            }

            if (changed)
            {
                RaiseChanged();
            }
        }
    }

    private bool Prune()
    {
        var removed = false;
        lock (_gate)
        {
            var now = DateTime.UtcNow;
            foreach (var key in _seen.Where(kv => now - kv.Value.LastSeenUtc > Ttl).Select(kv => kv.Key).ToList())
            {
                _seen.Remove(key);
                removed = true;
            }
        }

        return removed;
    }

    private void RaiseChanged()
    {
        List<NetworkServer> snapshot;
        lock (_gate)
        {
            snapshot = _seen.Values.OrderBy(s => s.Server.Name, StringComparer.CurrentCultureIgnoreCase).ToList();
        }

        Changed?.Invoke(snapshot);
    }

    public void Dispose()
    {
        _timer?.Dispose();
        _timer = null;
        _cts?.Cancel();
        _udp?.Close();
        _udp = null;
    }
}

/// <summary>Адрес IPv4 на сетевом адаптере.</summary>
public sealed record LanAddress(string Ip, string Mask, string Broadcast, string Adapter, bool Preferred);

/// <summary>Сетевые адреса этого ПК: для выбора LAN-IP, broadcast и отсечения своих объявлений.</summary>
public static class LanInfo
{
    private static readonly string[] VirtualHints =
        ["vpn", "virtual", "hyper-v", "vethernet", "tap-", "tunnel", "wsl", "docker", "bluetooth", "loopback"];

    /// <summary>Адреса поднятых IPv4-адаптеров. Сначала физические с основным шлюзом.</summary>
    public static IReadOnlyList<LanAddress> Addresses()
    {
        var result = new List<LanAddress>();
        try
        {
            foreach (var ni in NetworkInterface.GetAllNetworkInterfaces())
            {
                if (ni.OperationalStatus != OperationalStatus.Up || ni.NetworkInterfaceType == NetworkInterfaceType.Loopback)
                {
                    continue;
                }

                var props = ni.GetIPProperties();
                var hasGateway = props.GatewayAddresses.Any(g =>
                    g.Address.AddressFamily == AddressFamily.InterNetwork && !g.Address.Equals(IPAddress.Any));
                var label = ni.Name + " " + ni.Description;
                var virtualAdapter = VirtualHints.Any(h => label.Contains(h, StringComparison.OrdinalIgnoreCase));

                foreach (var u in props.UnicastAddresses)
                {
                    if (u.Address.AddressFamily != AddressFamily.InterNetwork)
                    {
                        continue;
                    }

                    var b = u.Address.GetAddressBytes();
                    if (b[0] == 127 || (b[0] == 169 && b[1] == 254))
                    {
                        continue;
                    }

                    var mask = u.IPv4Mask ?? IPAddress.Parse("255.255.255.0");
                    result.Add(new LanAddress(
                        u.Address.ToString(),
                        mask.ToString(),
                        Broadcast(u.Address, mask).ToString(),
                        ni.Name,
                        hasGateway && !virtualAdapter));
                }
            }
        }
        catch (Exception)
        {
            // не смогли перечислить адаптеры — вернём то, что есть
        }

        return result.OrderByDescending(a => a.Preferred).ToList();
    }

    /// <summary>Основной LAN-адрес или null.</summary>
    public static string? DefaultIp() => Addresses().Select(a => a.Ip).FirstOrDefault();

    /// <summary>Адреса, которые считаются «этим ПК»: loopback и все IPv4 адаптеров.</summary>
    public static HashSet<string> LocalIPv4()
    {
        var set = new HashSet<string> { "127.0.0.1" };
        foreach (var a in Addresses())
        {
            set.Add(a.Ip);
        }

        return set;
    }

    /// <summary>Адреса для broadcast: 255.255.255.255 и направленные broadcast каждой сети.</summary>
    public static IReadOnlyList<IPAddress> BroadcastTargets()
    {
        var set = new HashSet<string> { "255.255.255.255" };
        foreach (var a in Addresses())
        {
            set.Add(a.Broadcast);
        }

        return set.Select(s => IPAddress.Parse(s)).ToList();
    }

    public static IPAddress Broadcast(IPAddress ip, IPAddress mask)
    {
        var a = ip.GetAddressBytes();
        var m = mask.GetAddressBytes();
        var r = new byte[4];
        for (var i = 0; i < 4; i++)
        {
            r[i] = (byte)(a[i] | (~m[i] & 0xFF));
        }

        return new IPAddress(r);
    }
}
