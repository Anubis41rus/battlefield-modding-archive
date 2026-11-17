// Поиск игры и сервера: exe лежит рядом с ними в одной папке.
// Проверяем: рядом с exe -> корень выше -> весь диск/буквы дисков.
// Ничего не зашито: у друга своя буква диска и своя папка.
using System.IO;

namespace EVBots.Launcher;

internal sealed class FoundPaths
{
    public string Root = "";
    public string KitDir = "";
    public string ClientGameDir = "";
    public string ServerDir = "";
    public string InstanceDir = "";
    public string StandDir = "";
}

internal static class PathFinder
{
    /// <summary>Папка EVBots лежит рядом с «Battlefield 3» и «BF-Portable». Не нашлась — друг выбирает её кнопкой.</summary>
    public static readonly string[] StandNames = { "EVBots" };

    /// <summary>Папка сервера: «BF-Portable» (через дефис), как у всех.</summary>
    public static readonly string[] ServerNames = { "BF-Portable", "Battlefield Portable" };

    public static string ExeDir => AppContext.BaseDirectory.TrimEnd(Path.DirectorySeparatorChar);

    public static FoundPaths Find()
    {
        // Кандидаты корней: папка exe, выше до 3 уровней, корни всех дисков.
        var roots = new List<string>();
        var d = ExeDir;
        for (var i = 0; i < 4 && d is not null; i++)
        {
            roots.Add(d);
            d = Directory.GetParent(d)?.FullName;
        }
        foreach (var drive in DriveInfo.GetDrives().Where(x => x.IsReady))
            if (!roots.Contains(drive.RootDirectory.FullName.TrimEnd('\\')))
                roots.Add(drive.RootDirectory.FullName);

        foreach (var root in roots)
        {
            var hit = ScanRoot(root, 3);
            if (hit is not null) return hit;
        }
        return new FoundPaths { Root = ExeDir, KitDir = ExeDir };
    }

    // Ищем пару "Battlefield 3\bf3.exe" + "BF-Portable\Battlefield_3_Server\bf3_server_Final.exe"
    // не глубже depth уровней от root.
    private static FoundPaths? ScanRoot(string root, int depth)
    {
        try
        {
            var bf3 = Directory.GetFiles(root, "bf3.exe", SearchOption.TopDirectoryOnly).FirstOrDefault();
            // bf3.exe может лежать в "Battlefield 3\" или прямо в корне
            string? gameDir = null;
            if (bf3 is not null) gameDir = Path.GetDirectoryName(bf3);
            else
            {
                foreach (var sub in SafeDirs(root))
                {
                    var c = Path.Combine(sub, "bf3.exe");
                    if (File.Exists(c)) { gameDir = sub; break; }
                    // вложенность "Battlefield 3\..." на уровень ниже
                    foreach (var sub2 in SafeDirs(sub))
                    {
                        if (File.Exists(Path.Combine(sub2, "bf3.exe"))) { gameDir = sub2; break; }
                    }
                    if (gameDir is not null) break;
                }
            }
            if (gameDir is null) return null;

            var baseDir = Directory.GetParent(gameDir)?.FullName ?? root;
            // 20.10: без обхода всего дерева (AllDirectories падал на закрытой папке и долго шёл по C:\Games) —
            // только известные имена папки сервера рядом с игрой.
            string? serverExe = null;
            {
                foreach (var cand in ServerNames.SelectMany(n => new[]
                {
                    Path.Combine(baseDir, n, "Battlefield_3_Server", "bf3_server_Final.exe"),
                    Path.Combine(root, n, "Battlefield_3_Server", "bf3_server_Final.exe"),
                }))
                    if (File.Exists(cand)) { serverExe = cand; break; }
            }
            if (serverExe is null)
            {
                // Сервера может не быть — это нормально для гостя (у друга своя машина).
                return new FoundPaths
                {
                    Root = baseDir,
                    KitDir = ExeDir,
                    ClientGameDir = gameDir,
                    ServerDir = "",
                    InstanceDir = "",
                    StandDir = FindStand(baseDir, root),
                };
            }
            var serverDir = Path.GetDirectoryName(serverExe) ?? "";

            return new FoundPaths
            {
                Root = baseDir,
                KitDir = ExeDir,
                ClientGameDir = gameDir,
                ServerDir = serverDir,
                InstanceDir = Path.Combine(serverDir, "Instance"),
                StandDir = FindStand(baseDir, root),
            };
        }
        catch (Exception) { return null; }
    }

    /// <summary>Папка стенда: первая существующая из кандидатов рядом с игрой, корнем или exe.</summary>
    private static string FindStand(string baseDir, string root) =>
        // лаунчер лежит в самой папке EVBots — она и есть стенд
        (EVBots.Core.Paths.FindLoader(Path.Combine(ExeDir, "activation")) is not null ? ExeDir : null) ??
        StandNames
            .SelectMany(n => new[] { Path.Combine(baseDir, n), Path.Combine(root, n), Path.Combine(ExeDir, n) })
            .FirstOrDefault(Directory.Exists)
        ?? Path.Combine(baseDir, StandNames[0]);

    private static IEnumerable<string> SafeDirs(string dir)
    {
        try { return Directory.GetDirectories(dir); }
        catch (Exception) { return Enumerable.Empty<string>(); }
    }
}
