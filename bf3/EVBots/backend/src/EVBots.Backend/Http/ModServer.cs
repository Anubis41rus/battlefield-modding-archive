// ModServer.cs — раздача модов клиентам по HTTP.
//
// У VU это работает автоматически: «All VU mods are hosted on the server-side
// and automatically downloaded by joining clients» (modding/structure.md).
// У нас та же схема: моды лежат в Admin\Mods\ инстанса, клиент при входе
// забирает их по этому маршруту.
//
// Раскладка как у VU:
//   <instance>\Admin\ModList.txt        список модов (по имени папки)
//   <instance>\Admin\Mods\<мод>\mod.json
//                              \ext\{client,server,shared}\__init__.lua
//
// Безопасность: путь не выходит за пределы Admin\Mods\ (проверка на ..).

using System.Net;
using System.Text;
using System.Text.Json;
using EVBots.Backend.Data;
using EVBots.Backend.Tdf;

namespace EVBots.Backend.Http;

internal static class ModServer
{
    public static void Handle(HttpListenerContext ctx, ServeConfig cfg,
        InstanceCatalog catalog, string[] parts, string method)
    {
        var modsDir = cfg.ModsDir;
        Directory.CreateDirectory(modsDir);

        // GET /mods
        if (parts.Length == 1 && method == "GET")
        {
            SendJson(ctx, 200, new
            {
                mods = ListMods(modsDir),
                modList = catalog.ReadModList(),
                debug = catalog.DebugMode,
                modsDir = modsDir,
                modListPath = catalog.ModListPath,
            });
            return;
        }

        // GET /mods/{name}/... — отдача файла
        if (parts.Length >= 2 && method == "GET")
        {
            var modName = parts[1];
            var modDir = Path.Combine(modsDir, modName);

            // защита от выхода за пределы каталога модов
            var full = Path.GetFullPath(modDir);
            var root = Path.GetFullPath(modsDir);
            if (!full.StartsWith(root, StringComparison.OrdinalIgnoreCase))
            {
                WebHost.SendError(ctx, 403, "выход за пределы Admin/Mods запрещён");
                return;
            }

            if (!Directory.Exists(full))
            {
                WebHost.SendError(ctx, 404, $"мод '{modName}' не найден");
                return;
            }

            // /mods/{name} — описание мода
            if (parts.Length == 2)
            {
                SendJson(ctx, 200, DescribeMod(full, modName));
                return;
            }

            // /mods/{name}/{файл...}
            var rel = string.Join('/', parts.Skip(2));
            var filePath = Path.GetFullPath(Path.Combine(full,
                rel.Replace('/', Path.DirectorySeparatorChar)));

            if (!filePath.StartsWith(full, StringComparison.OrdinalIgnoreCase))
            {
                WebHost.SendError(ctx, 403, "выход за пределы мода запрещён");
                return;
            }

            if (!File.Exists(filePath))
            {
                WebHost.SendError(ctx, 404, "файл не найден");
                return;
            }

            ServeFile(ctx, filePath);
            return;
        }

        WebHost.SendError(ctx, 405, "метод не поддерживается");
    }

    private static void ServeFile(HttpListenerContext ctx, string path)
    {
        var bytes = File.ReadAllBytes(path);
        ctx.Response.StatusCode = 200;
        ctx.Response.ContentType = ContentType(path);
        ctx.Response.ContentLength64 = bytes.Length;
        ctx.Response.OutputStream.Write(bytes, 0, bytes.Length);
    }

    private static string ContentType(string path) =>
        Path.GetExtension(path).ToLowerInvariant() switch
        {
            ".json" => "application/json; charset=utf-8",
            ".lua" => "text/x-lua; charset=utf-8",
            ".vuic" => "application/octet-stream",
            ".txt" => "text/plain; charset=utf-8",
            ".html" or ".htm" => "text/html; charset=utf-8",
            ".css" => "text/css; charset=utf-8",
            ".js" => "text/javascript; charset=utf-8",
            ".png" => "image/png",
            ".jpg" or ".jpeg" => "image/jpeg",
            _ => "application/octet-stream",
        };

    public static IReadOnlyList<string> ListMods(string modsDir) =>
        Directory.Exists(modsDir)
            ? Directory.EnumerateDirectories(modsDir).Select(Path.GetFileName)
                .Where(n => !string.IsNullOrEmpty(n)).Select(n => n!).Order()
                .ToList()
            : new List<string>();

    private static object DescribeMod(string dir, string name)
    {
        var modJson = Path.Combine(dir, "mod.json");

        object? descriptor = null;
        if (File.Exists(modJson))
        {
            try
            {
                using var doc = JsonDocument.Parse(
                    File.ReadAllText(modJson, Encoding.UTF8));
                descriptor = doc.RootElement.Clone();
            }
            catch
            {
                descriptor = new { error = "mod.json не разобран" };
            }
        }

        return new
        {
            name,
            hasModJson = File.Exists(modJson),
            modJson = descriptor,
            files = ModFiles(dir),
            hasExt = Directory.Exists(Path.Combine(dir, "ext")),
            hasWebUi = File.Exists(Path.Combine(dir, "ui.vuic")),
        };
    }

    /// <summary>
    /// Файлы мода, которые имеет смысл отдавать клиенту. Служебные каталоги
    /// (.git, node_modules) и мусор (.md, .ps1, .bat) клиенту не нужны, а
    /// .git вообще не должен уезжать по сети.
    /// </summary>
    private static List<string> ModFiles(string dir)
    {
        var skipDirs = new[] { ".git", ".svn", ".vs", "node_modules", "obj", "bin" };
        var keepExt = new[]
        {
            ".lua", ".json", ".vuic", ".png", ".jpg", ".jpeg", ".gif", ".svg",
            ".css", ".js", ".html", ".htm", ".txt", ".woff", ".woff2", ".ttf",
        };

        var result = new List<string>();

        foreach (var f in Directory.EnumerateFiles(dir, "*", SearchOption.AllDirectories))
        {
            var rel = Path.GetRelativePath(dir, f).Replace('\\', '/');

            var parts = rel.Split('/');
            if (parts.Any(p => skipDirs.Contains(p, StringComparer.OrdinalIgnoreCase)))
            {
                continue;
            }

            if (parts.Any(p => p.StartsWith('.') && p.Length > 1))
            {
                continue; // .gitignore, .luacheckrc и прочее
            }

            if (!keepExt.Contains(Path.GetExtension(f),
                    StringComparer.OrdinalIgnoreCase))
            {
                continue;
            }

            result.Add(rel);
        }

        result.Sort(StringComparer.Ordinal);
        return result;
    }

    private static readonly JsonSerializerOptions JsonOut = new()
    {
        WriteIndented = true,
        Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
    };

    private static void SendJson(HttpListenerContext ctx, int status, object body)
    {
        var json = JsonSerializer.Serialize(body, JsonOut);

        var bytes = Encoding.UTF8.GetBytes(json);
        ctx.Response.StatusCode = status;
        ctx.Response.ContentType = "application/json; charset=utf-8";
        ctx.Response.ContentLength64 = bytes.Length;
        ctx.Response.OutputStream.Write(bytes, 0, bytes.Length);
    }
}