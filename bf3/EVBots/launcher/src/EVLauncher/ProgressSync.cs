// Синхронизация статистики учётки между ПК друзей (18.10).
// Своя копия статистики лежит рядом с лаунчером (progress\<логин>.json). После входа и раз в минуту лаунчер
// отправляет её хосту (POST /accounts/{логин}/progress), хост сливает максимумом по каждому имени и отвечает итогом,
// итог сохраняется в копию. Так звание и статистика ходят за игроком, кто бы ни хостил.
using System.IO;
using System.Net.Http;
using System.Text;
using System.Text.Json;

namespace EVBots.Launcher;

public partial class MainWindow
{
    private DateTime _lastSync = DateTime.MinValue;
    private bool _syncBusy;

    private string ProgressFile()
    {
        var safe = new string(_s.Login.Select(c => char.IsLetterOrDigit(c) || c is '_' or '-' ? c : '_').ToArray());
        return Path.Combine(PathFinder.ExeDir, "progress", (safe.Length > 0 ? safe : "_") + ".json");
    }

    private async Task SyncProgressAsync(bool force)
    {
        if (_syncBusy || _authIp.Length == 0 || _s.Login.Length == 0) return;
        if (!force && DateTime.UtcNow - _lastSync < TimeSpan.FromSeconds(60)) return;
        _syncBusy = true;
        _lastSync = DateTime.UtcNow;
        try
        {
            var file = ProgressFile();
            var local = new Dictionary<string, double>(StringComparer.Ordinal);
            if (File.Exists(file))
            {
                try { local = JsonSerializer.Deserialize<Dictionary<string, double>>(File.ReadAllText(file)) ?? local; }
                catch (Exception) { }
            }

            var body = JsonSerializer.Serialize(new { password = _s.Password, stats = local });
            using var resp = await Http.PostAsync(
                $"http://{_authIp}:8080/accounts/{Uri.EscapeDataString(_s.Login)}/progress",
                new StringContent(body, Encoding.UTF8, "application/json"));
            var txt = await resp.Content.ReadAsStringAsync();
            if (!resp.IsSuccessStatusCode)
            {
                if (force) Log($"Статистика: хост не принял ({(int)resp.StatusCode}) — у хоста старый бэкенд?");
                return;
            }

            using var doc = JsonDocument.Parse(txt);
            var merged = new Dictionary<string, double>(local, StringComparer.Ordinal);
            var fromHost = 0;
            if (doc.RootElement.TryGetProperty("stats", out var st) && st.ValueKind == JsonValueKind.Object)
            {
                foreach (var kv in st.EnumerateObject())
                {
                    if (kv.Value.ValueKind != JsonValueKind.Number) continue;
                    var v = kv.Value.GetDouble();
                    if (!merged.TryGetValue(kv.Name, out var old) || v > old) { merged[kv.Name] = v; fromHost++; }
                }
            }

            var toHost = doc.RootElement.TryGetProperty("changed", out var ch) && ch.TryGetInt32(out var c) ? c : 0;
            Directory.CreateDirectory(Path.GetDirectoryName(file)!);
            File.WriteAllText(file, JsonSerializer.Serialize(merged, new JsonSerializerOptions { WriteIndented = true }));
            if (force || fromHost > 0 || toHost > 0)
            {
                merged.TryGetValue("sc_general", out var g);
                Log($"Статистика синхронизирована: на хост {toHost}, с хоста {fromHost}, всего {merged.Count} (очки {g:0})");
            }
        }
        catch (Exception ex)
        {
            if (force) Log("Статистика: синхронизация не удалась: " + ex.Message);
        }
        finally { _syncBusy = false; }
    }
}
