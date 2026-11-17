// EditorialHttp.cs — server.xml для игрового сервера (20.10.2026).
//
// Сервер BF3 при старте раунда качает http://bf3.gos.ea.com/easo/editorial/Battlefield/3/config/pc/server.xml
// (BF/Server/Online/Blaze/BlazeServerGameConfiguration.cpp(96); без интернета — «Failed to download server
// configuration»). BF-Portable отдаёт его своим Apache из htdocs\easo\…; у нас Apache нет — отдаём этот же файл сами.
// Имя bf3.gos.ea.com сервер получает через подмену DNS активатора (BF3ACT_DNSREDIR -> 127.0.0.1), поэтому слушаем
// только 127.0.0.1:80 (порт < 1024 в Windows не требует прав). По решению пользователя 20.10 из файла убираем
// <ScoreMultiplier> (в копии BF-Portable — очки ×2 до 2098 года): очки обычные, лицензии (<Licenses>) остаются.

using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.RegularExpressions;

namespace EVBots.Backend.Http;

internal sealed class EditorialHttp(string? file, Log log)
{
    public const string ServerXmlPath = "/easo/editorial/Battlefield/3/config/pc/server.xml";
    private const int Port = 80;

    private readonly CancellationTokenSource _stop = new();
    private TcpListener? _tcp;

    /// <summary>server.xml из BF-Portable: Instance -> Battlefield_3_Server -> BF-Portable\htdocs (или Xampp\htdocs).</summary>
    public static string? FindServerXml(string instanceDir)
    {
        if (instanceDir.Length == 0) return null;
        var rel = Path.Combine("easo", "editorial", "Battlefield", "3", "config", "pc", "server.xml");
        var portable = Path.GetFullPath(Path.Combine(instanceDir, "..", ".."));
        foreach (var root in new[] { Path.Combine(portable, "htdocs"), Path.Combine(portable, "Xampp", "htdocs") })
        {
            var p = Path.Combine(root, rel);
            if (File.Exists(p)) return p;
        }
        return null;
    }

    /// <summary>Не бросает: занят порт или нет файла — пишем предупреждение, бэкенд работает дальше.</summary>
    public void Start()
    {
        if (file is null)
        {
            log.Warn("editorial: server.xml в BF-Portable\\htdocs не найден — сервер напишет «Failed to download server configuration»");
            return;
        }
        try
        {
            _tcp = new TcpListener(IPAddress.Loopback, Port);
            _tcp.Start();
        }
        catch (SocketException ex)
        {
            log.Warn($"editorial: порт 127.0.0.1:{Port} занят ({ex.SocketErrorCode}) — server.xml не отдаём");
            _tcp = null;
            return;
        }
        _ = Task.Run(() => LoopAsync(_stop.Token));
        log.Ok($"editorial: server.xml на 127.0.0.1:{Port} (bf3.gos.ea.com), из {file}, без ScoreMultiplier");
    }

    public void Stop()
    {
        try { _stop.Cancel(); _tcp?.Stop(); } catch { }
    }

    /// <summary>Файл BF-Portable без блоков ScoreMultiplier.</summary>
    public static string Prepare(string xml) =>
        Regex.Replace(xml, @"[ \t]*<ScoreMultiplier>.*?</ScoreMultiplier>[ \t]*\r?\n?", "", RegexOptions.Singleline);

    private async Task LoopAsync(CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            TcpClient c;
            try { c = await _tcp!.AcceptTcpClientAsync(ct); }
            catch (OperationCanceledException) { return; }
            catch (ObjectDisposedException) { return; }
            catch (SocketException) { continue; }
            _ = Task.Run(() => HandleAsync(c));
        }
    }

    private async Task HandleAsync(TcpClient c)
    {
        using (c)
        {
            try
            {
                c.ReceiveTimeout = 5000;
                var s = c.GetStream();
                var buf = new byte[8192];
                var n = 0;
                while (n < buf.Length)
                {
                    var r = await s.ReadAsync(buf.AsMemory(n, buf.Length - n));
                    if (r <= 0) break;
                    n += r;
                    if (Encoding.ASCII.GetString(buf, 0, n).Contains("\r\n\r\n")) break;
                }
                var head = Encoding.ASCII.GetString(buf, 0, n);
                var line = head.Split("\r\n")[0];
                var parts = line.Split(' ');
                var path = parts.Length > 1 ? parts[1].Split('?')[0] : "";
                byte[] body;
                string status;
                if (parts.Length > 1 && parts[0] == "GET" && path.Equals(ServerXmlPath, StringComparison.OrdinalIgnoreCase))
                {
                    body = Encoding.UTF8.GetBytes(Prepare(File.ReadAllText(file!, Encoding.UTF8)));
                    status = "200 OK";
                    log.Info($"[editorial] {line} -> 200, {body.Length} Б");
                }
                else
                {
                    body = Encoding.ASCII.GetBytes("not found");
                    status = "404 Not Found";
                    log.Info($"[editorial] {line} -> 404");
                }
                var hdr = $"HTTP/1.1 {status}\r\nContent-Type: text/xml\r\nContent-Length: {body.Length}\r\nConnection: close\r\n\r\n";
                await s.WriteAsync(Encoding.ASCII.GetBytes(hdr));
                await s.WriteAsync(body);
            }
            catch (Exception ex)
            {
                log.Warn($"[editorial] ошибка ответа: {ex.Message}");
            }
        }
    }
}
