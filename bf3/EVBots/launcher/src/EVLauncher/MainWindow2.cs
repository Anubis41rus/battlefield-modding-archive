using System.IO;
using System.Text.Json;
using System.Windows.Controls;

namespace EVBots.Launcher;

public partial class MainWindow
{
    private sealed record Row(string Title, string Ip, string Map, string Mode, string Players, ulong GameId) { public override string ToString() => Title; }

    /// <summary>Тик: серверы на IP, где вошли + хвост лога сервера.</summary>
    private async Task TickAsync()
    {
        if (_authIp.Length == 0) return; // ещё не вошли — нечего опрашивать
        try
        {
            var mine = _authIp == "127.0.0.1";
            var rows = new List<Row>();
            foreach (var g in await ServersAt(_authIp))
                rows.Add(new Row(mine ? g.Title + " (мой)" : g.Title, _authIp, g.Map, g.Mode, g.Players, g.GameId));
            Dispatcher.Invoke(() =>
            {
                ServerBox.ItemsSource = rows;
                ServerBox.DisplayMemberPath = "Title";
                if (ServerBox.SelectedIndex < 0 && rows.Count > 0) ServerBox.SelectedIndex = 0;
            });
            // Хвост лога сервера (только своя машина — лог чужого ПК недоступен)
            if (mine)
                foreach (var l in ReadTail(Path.Combine(_p.StandDir, "logs", "EVBots.log")))
                    Log(l);
            await SyncProgressAsync(false);
            await ReadRemoteProfileLabelAsync();
        }
        catch (Exception) { }
    }

    private static async Task<List<Row>> ServersAt(string ip)
    {
        var out_ = new List<Row>();
        try
        {
            var txt = await Http.GetStringAsync($"http://{ip}:8080/servers");
            using var doc = JsonDocument.Parse(txt);
            if (!doc.RootElement.TryGetProperty("servers", out var arr)) return out_;
            foreach (var s in arr.EnumerateArray())
            {
                string Str(string n) => s.TryGetProperty(n, out var v) ? (v.ValueKind == JsonValueKind.String ? v.GetString() ?? "" : v.GetRawText()) : "";
                ulong U64(string n) => s.TryGetProperty(n, out var v) && v.TryGetUInt64(out var x) ? x : 0;
                var name = Str("name");
                if (name.Length == 0) name = "Сервер";
                var players = Str("players") + "/" + Str("maxPlayers");
                out_.Add(new Row($"{name} · {Str("map")} {Str("mode")} · {players}", ip,
                    Str("map"), Str("mode"), players, U64("gameId")));
            }
        }
        catch (Exception) { }
        return out_;
    }

    private List<string> ReadTail(string path)
    {
        var res = new List<string>();
        try
        {
            if (!File.Exists(path)) return res;
            using var fs = new FileStream(path, FileMode.Open, FileAccess.Read,
                FileShare.ReadWrite | FileShare.Delete);
            if (fs.Length < _logOffset) _logOffset = 0;
            if (fs.Length == _logOffset) return res;
            if (fs.Length - _logOffset > 512 * 1024) _logOffset = fs.Length - 512 * 1024;
            fs.Seek(_logOffset, SeekOrigin.Begin);
            using var sr = new StreamReader(fs, System.Text.Encoding.UTF8);
            var text = sr.ReadToEnd();
            _logOffset = fs.Position;
            foreach (var l in text.Split('\n'))
            {
                var t = l.TrimEnd('\r');
                if (t.Contains("LUA-MODE READY") || t.Contains("LUA-MODE FAILED") || t.Contains("VEH #") || t.Contains("Heartbeat") || t.Contains("heartbeat"))
                    res.Add(t);
            }
        }
        catch (Exception) { }
        return res;
    }
}
