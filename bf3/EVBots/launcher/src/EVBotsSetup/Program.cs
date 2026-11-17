// Установщик EV Bots: ставит папку EVBots (из вшитого kit.zip) рядом с «Battlefield 3» и «BF-Portable»
// и создаёт ярлык «EV Launcher» на рабочем столе. Установщик лежит в той же папке, что и обе игры:
// путь берётся от его собственного места, выбирать папку не нужно. Это комплект локального Battle Royale; другие моды не меняет.
// Русский язык игры установщик не трогает. Журнал: %TEMP%\evbots-install.log.
// 23.10: поверх старой установки — обновление (бэкап, уборка устаревшего, данные игрока целы; см. InstallFiles).
// 20.10: удаление — «Удалить EVBots.exe» в папке EVBots или «Установленные приложения» Windows (Uninstall.cs).
using System;
using System.Drawing;
using System.IO;
using System.IO.Compression;
using System.Linq;
using EVBots.Installation;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace EVBots.Setup;

internal static class Program
{
    [STAThread]
    private static int Main(string[] args)
    {
        // 20.10: удаление — ключ /uninstall или запуск копии «Удалить EVBots.exe» из папки EVBots
        if (args.Any(a => a.Equals("/uninstall", StringComparison.OrdinalIgnoreCase)) ||
            string.Equals(Path.GetFileName(Application.ExecutablePath), Uninstall.UninstallerName, StringComparison.OrdinalIgnoreCase))
        {
            Application.EnableVisualStyles();
            return Uninstall.Run(args);
        }
        // Тихий режим для проверки: EVBots-Setup.exe /quiet — только распаковка папки EVBots, без окон, реестра, фаервола и ярлыка.
        if (args.Any(a => a.Equals("/quiet", StringComparison.OrdinalIgnoreCase)))
        {
            try
            {
                // 01.11: строки установки — в %TEMP%\evbots-install-quiet.log (проверка русского меню без окон)
                var qlog = Path.Combine(Path.GetTempPath(), "evbots-install-quiet.log");
                SetupForm.InstallFiles(SetupForm.OwnFolder, new Progress<int>(_ => { }),
                    s => File.AppendAllText(qlog, s + Environment.NewLine));
                return 0;
            }
            catch (Exception ex)
            {
                File.AppendAllText(Path.Combine(Path.GetTempPath(), "evbots-install-quiet.log"), "ОШИБКА: " + ex + Environment.NewLine);
                return 1;
            }
        }
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Application.Run(new SetupForm());
        return 0;
    }
}

internal sealed class SetupForm : Form
{
    private static readonly string LogFile = Path.Combine(Path.GetTempPath(), "evbots-install.log");

    /// <summary>Папка, где лежит сам установщик: рядом с ней должны быть «Battlefield 3» и «BF-Portable».</summary>
    public static string OwnFolder => Path.GetDirectoryName(Application.ExecutablePath) ?? "";

    private readonly Label _state = new() { AutoSize = true, ForeColor = Color.DimGray, Location = new Point(20, 92) };
    private readonly CheckBox _shortcut = new() { Text = "Создать ярлык «EV Launcher» на рабочем столе", Checked = true, AutoSize = true };
    private readonly CheckBox _launch = new() { Text = "Запустить EV Launcher после установки", Checked = true, AutoSize = true };
    private readonly CheckBox _firewall = new() { Text = "Настроить брандмауэр Windows для игры по сети (окно UAC)", Checked = true, AutoSize = true };
    private readonly ProgressBar _progress = new SetupProgressBar() { Width = 520, Height = 18 };
    private readonly ListBox _log = new() { Width = 520, Height = 150, HorizontalScrollbar = true };
    private readonly Button _install = new() { Text = "Установить", Width = 120, Height = 30 };
    private readonly Button _cancel = new() { Text = "Отмена", Width = 100, Height = 30 };
    private readonly SetupPresentation _presentation;

    public SetupForm()
    {
        StartPosition = FormStartPosition.CenterScreen;
        try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch (Exception) { }
        _presentation = new SetupPresentation(this, _state, _shortcut, _launch, _firewall, _progress, _log, _install, _cancel);

        _install.Click += async (_, _) => await InstallAsync();
        _cancel.Click += (_, _) => Close();

        UpdateState();
    }

    private static byte[] ReadAll(Stream s)
    {
        using var ms = new MemoryStream();
        s.CopyTo(ms);
        return ms.ToArray();
    }

    private static string GameExe(string dir)
    {
        var p = Path.Combine(dir, "Battlefield 3", "bf3.exe");
        return File.Exists(p) ? p : "";
    }

    private static string ServerExe(string dir)
    {
        foreach (var name in new[] { "BF-Portable", "Battlefield Portable" })
        {
            var p = Path.Combine(dir, name, "Battlefield_3_Server", "bf3_server_Final.exe");
            if (File.Exists(p)) return p;
        }
        return "";
    }

    // Что найдено рядом с установщиком. Без игры установка не начинается.
    private void UpdateState()
    {
        var dir = OwnFolder;
        var game = GameExe(dir);
        var server = ServerExe(dir);
        _presentation.UpdateDetectedPaths(dir, game, server);
        _install.Enabled = game.Length > 0;
    }

    /// <summary>
    /// 20.10: все IPv4 включённых адаптеров (кроме loopback). Раньше брался первый адаптер со шлюзом — на ПК с VPN это
    /// был VPN (10.6.7.1), а не домашняя сеть (192.168.0.10), и гости не входили. Бэкенд слушает каждый адрес отдельно.
    /// </summary>
    internal static System.Collections.Generic.List<string> LanIps()
    {
        var res = new System.Collections.Generic.List<string>();
        try
        {
            foreach (var ni in NetworkInterface.GetAllNetworkInterfaces())
            {
                if (ni.OperationalStatus != OperationalStatus.Up) continue;
                if (ni.NetworkInterfaceType is NetworkInterfaceType.Loopback) continue;
                foreach (var u in ni.GetIPProperties().UnicastAddresses)
                {
                    if (u.Address.AddressFamily != AddressFamily.InterNetwork) continue;
                    var s = u.Address.ToString();
                    if (s.StartsWith("127.") || s.StartsWith("169.254.") || res.Contains(s)) continue;
                    res.Add(s);
                }
            }
        }
        catch (Exception) { }
        return res;
    }

    /// <summary>Брандмауэр — одной командой с UAC. Те же правила, что кнопка «Фаервол (UAC)» лаунчера.</summary>
    private void ApplySystemSettings(string target)
    {
        var cmds = new System.Collections.Generic.List<string>();
        if (_firewall.Checked)
        {
            var user = Environment.UserName;
            var ips = LanIps();
            foreach (var ip in ips)
            {
                cmds.Add($"netsh http delete urlacl url=http://{ip}:8080/ >nul 2>&1");
                cmds.Add($"netsh http add urlacl url=http://{ip}:8080/ user=\"{user}\"");
            }
            cmds.Add("netsh http delete urlacl url=http://127.0.0.1:8080/ >nul 2>&1");
            cmds.Add($"netsh http add urlacl url=http://127.0.0.1:8080/ user=\"{user}\"");
            // 20.10: запись http://+:8080/ НЕ добавлять: у «+» в http.sys приоритет выше, чем у адресов, на которых слушает
            // бэкенд (127.0.0.1, LAN IP), — все запросы уходили в пустую запись, HTTP 503, стенд и лаунчер не стартовали.
            // Удаляем её, если осталась от установщика 20.10 ≈02:15.
            cmds.Add("netsh http delete urlacl url=http://+:8080/ >nul 2>&1");
            // старые правила прежнего названия проекта
            foreach (var old in new[] { "Legacy-TCP", "Legacy-UDP", "Legacy client", "Legacy server" })
                cmds.Add($"netsh advfirewall firewall delete rule name=\"{old}\" >nul 2>&1");
            cmds.Add("netsh advfirewall firewall delete rule name=\"EVBots-TCP\" >nul 2>&1");
            cmds.Add("netsh advfirewall firewall add rule name=\"EVBots-TCP\" dir=in action=allow protocol=TCP localport=8080,42127,10041,7948");  // 7948 — канал модов (как Harmony у VU)
            cmds.Add("netsh advfirewall firewall delete rule name=\"EVBots-UDP\" >nul 2>&1");
            cmds.Add("netsh advfirewall firewall add rule name=\"EVBots-UDP\" dir=in action=allow protocol=UDP localport=25300,17554,47990");
            var game = GameExe(target);
            if (game.Length > 0)
            {
                cmds.Add("netsh advfirewall firewall delete rule name=\"EVBots client\" >nul 2>&1");
                cmds.Add($"netsh advfirewall firewall add rule name=\"EVBots client\" dir=in action=allow program=\"{game}\" enable=yes");
            }
            var server = ServerExe(target);
            if (server.Length > 0)
            {
                cmds.Add("netsh advfirewall firewall delete rule name=\"EVBots server\" >nul 2>&1");
                cmds.Add($"netsh advfirewall firewall add rule name=\"EVBots server\" dir=in action=allow program=\"{server}\" enable=yes");
            }
            Say("Брандмауэр: порты TCP 8080, 42127, 10041; UDP 25300, 17554, 47990; разрешение bf3.exe" +
                (server.Length > 0 ? " и сервера" : "") + (ips.Count > 0 ? "; HTTP для гостей " + string.Join(", ", ips) : ""));
        }
        if (cmds.Count == 0) return;
        var psi = new System.Diagnostics.ProcessStartInfo("cmd.exe", "/d /c \"" + string.Join(" & ", cmds) + "\"")
        {
            Verb = "runas",
            UseShellExecute = true,
            WindowStyle = System.Diagnostics.ProcessWindowStyle.Hidden,
        };
        try
        {
            using var pr = System.Diagnostics.Process.Start(psi);
            pr?.WaitForExit();
            Say("Настройки Windows применены.");
        }
        catch (System.ComponentModel.Win32Exception)
        {
            Say("UAC отклонён: брандмауэр не настроен. Позже: кнопка «Фаервол (UAC)» в лаунчере.");
        }
    }

    private static bool HasDesktopRuntime10()
    {
        try
        {
            var dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86), "dotnet", "shared", "Microsoft.WindowsDesktop.App");
            return Directory.Exists(dir) && Directory.GetDirectories(dir).Any(d => Path.GetFileName(d).StartsWith("10.", StringComparison.Ordinal));
        }
        catch (Exception) { return false; }
    }

    private void Say(string text)
    {
        _log.Items.Add(text);
        _log.TopIndex = _log.Items.Count - 1;
        try { File.AppendAllText(LogFile, DateTime.Now.ToString("HH:mm:ss") + "  " + text + Environment.NewLine); } catch (Exception) { }
    }

    private async Task InstallAsync()
    {
        var target = OwnFolder;
        if (GameExe(target).Length == 0)
        {
            MessageBox.Show(this, "Установщик должен лежать в той же папке, что «Battlefield 3» и «BF-Portable».\nСейчас он в:\n" + target,
                Text, MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return;
        }

        // 23.10: обновление поверх — файлы не должны быть заняты
        var busy = new[] { "EV Launcher", "EV Backend", "bf3_server_Final", "bf3", "evbots-loader", "activation-loader" }
            .Where(n => System.Diagnostics.Process.GetProcessesByName(n).Length > 0).ToList();
        if (busy.Count > 0)
        {
            MessageBox.Show(this, "Закрой перед установкой: " + string.Join(", ", busy) + ".\nПотом нажми «Установить» ещё раз.",
                Text, MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return;
        }

        _install.Enabled = _shortcut.Enabled = _launch.Enabled = _firewall.Enabled = false;
        _state.Text = "Устанавливаем EVBots…\nПодробности — в журнале выше.";
        var dst = Path.Combine(target, "EVBots");
        try
        {
            Say("Папка установки: " + target);
            Say("Ставлю EVBots в " + dst + " …");
            var progress = new Progress<int>(p => _progress.Value = p);
            var lines = new System.Collections.Concurrent.ConcurrentQueue<string>();
            await Task.Run(() => InstallFiles(target, progress, lines.Enqueue));
            while (lines.TryDequeue(out var line)) Say(line);
            Say("EVBots установлен: " + dst);
            try
            {
                Uninstall.Register(dst, Version);
                Say("Добавлен в «Установленные приложения» (удалить можно оттуда или «Удалить EVBots.exe» в папке).");
            }
            catch (Exception ex) { Say("Запись в список программ не удалась: " + ex.Message); }

            if (_shortcut.Checked)
            {
                var lnk = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), "EV Launcher.lnk");
                CreateShortcut(lnk, Path.Combine(dst, "EV Launcher.exe"), dst);
                Say("Ярлык создан: " + lnk);
            }

            ApplySystemSettings(target);

            var netOk = HasDesktopRuntime10();
            Say(netOk ? ".NET 10 Desktop Runtime (x86) найден." : "ВНИМАНИЕ: не найден .NET 10 Desktop Runtime (x86).");
            _progress.Value = 100;
            _state.Text = "Установка завершена.";

            var msg = "Battle Royale установлен в папку:\n" + dst + "\n\nОткрой EV Launcher и войди на локальный сервер. Проверка файлов доступна в разделе «Настройки и диагностика».\nПолный игровой матч пока проходит проверку.";
            if (!netOk)
                msg += "\n\nВАЖНО: не найден .NET 10 Desktop Runtime (x86). Без него лаунчер не запустится.\nСкачай: https://dotnet.microsoft.com/download/dotnet/10.0";
            MessageBox.Show(this, msg, "Готово", MessageBoxButtons.OK, netOk ? MessageBoxIcon.Information : MessageBoxIcon.Warning);

            if (_launch.Checked && netOk)
                System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(Path.Combine(dst, "EV Launcher.exe")) { WorkingDirectory = dst, UseShellExecute = true });
            Close();
        }
        catch (Exception ex)
        {
            Say("ОШИБКА: " + ex.Message);
            _state.Text = "Установка не завершена.\nПодробности — в журнале выше.";
            MessageBox.Show(this, "Установка не завершена:\n" + ex.Message + "\n\nЖурнал: " + LogFile, Text, MessageBoxButtons.OK, MessageBoxIcon.Error);
            _install.Enabled = _shortcut.Enabled = _launch.Enabled = _firewall.Enabled = true;
        }
    }

    /// <summary>
    /// Папка EVBots из вшитого kit.zip + пустые logs и profiles.
    /// 23.10 (обновление): если EVBots уже стоит — заменяемые файлы копируются в EVBots\backup\update-&lt;время&gt;, а файлы,
    /// которых нет в новом комплекте (например, старый activation-loader.exe), переносятся туда же — из установки они
    /// уходят. Данные игрока не трогаем: profiles\ (аккаунты, статистика, прогресс, отчёты матчей), logs\, progress\ (кроме
    /// файлов комплекта), friend-settings.json (настройки лаунчера). Хранятся два последних бэкапа обновления.
    /// </summary>
    internal static void InstallFiles(string target, IProgress<int> progress, Action<string> say = null)
    {
        // Validate the complete archive before moving any previous installation.
        var kit = KitFiles();
        var dst = Path.Combine(target, "EVBots");
        var portable = ServerExe(target);
        var portableInstance = portable.Length == 0 ? null : Path.Combine(Path.GetDirectoryName(portable), "Instance");
        if (portableInstance != null) RoyaleInstanceConfig.ValidatePaths(portableInstance);
        CheckDestinationLinks(dst, kit.Concat(RoyaleInstanceConfig.InstallationPaths()));
        if (Directory.Exists(dst) && Directory.EnumerateFileSystemEntries(dst).Any())
            PrepareUpdate(dst, kit, say ?? (_ => { }));
        Extract(dst, progress);
        foreach (var d in new[] { "logs", "profiles" })
            Directory.CreateDirectory(Path.Combine(dst, d));
        MigrateLegacy(target, dst, say ?? (_ => { }));
        var royaleInstance = RoyaleInstanceConfig.InstanceDirectory(dst);
        RoyaleInstanceConfig.Write(royaleInstance);
        say?.Invoke("Конфигурация Battle Royale подготовлена: " + royaleInstance);
        if (portableInstance != null)
        {
            RoyaleInstanceConfig.Write(portableInstance);
            say?.Invoke("Файлы Instance в BF Portable настроены для Battle Royale: " + portableInstance);
        }
        // Комплект Royale не меняет файлы других модов и не устанавливает меню F12.
        // 20.10: деинсталлятор — копия установщика под именем «Удалить EVBots.exe»; метка — удаление трогает только эту папку
        File.Copy(Application.ExecutablePath, Path.Combine(dst, Uninstall.UninstallerName), true);
        File.WriteAllText(Path.Combine(dst, Uninstall.Marker),
            "EVBots " + Version + " " + DateTime.Now.ToString("yyyy-MM-dd HH:mm") + Environment.NewLine);
    }

    internal static string Version =>
        typeof(SetupForm).Assembly.GetName().Version?.ToString(3) ?? "1.0.0";

    // данные игрока и служебное — обновление их не трогает
    private static readonly string[] KeepDirs = { "profiles\\", "logs\\", "backup\\", "progress\\", "activation\\logs\\",
        "mod-settings\\", "activation\\mod-settings\\", "cache\\", "runtime\\", "license\\" };
    private static readonly string[] KeepFiles = { "friend-settings.json" };

    internal static void CheckDestinationLinks(string dst, System.Collections.Generic.IEnumerable<string> files)
    {
        foreach (var relative in files)
        {
            var path = dst;
            if ((Directory.Exists(path) || File.Exists(path)) && (File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0)
                throw new IOException("Папка установки является ссылкой: " + path);
            foreach (var part in relative.Split('\\'))
            {
                path = Path.Combine(path, part);
                if ((Directory.Exists(path) || File.Exists(path)) && (File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0)
                    throw new IOException("Файл комплекта проходит через ссылку: " + path);
            }
        }
    }

    /// <summary>Относительные пути файлов вшитого kit.zip (внутри EVBots\).</summary>
    internal static System.Collections.Generic.HashSet<string> KitFiles()
    {
        using var res = typeof(SetupForm).Assembly.GetManifestResourceStream("kit.zip")
            ?? throw new InvalidOperationException("в установщике нет вшитого kit.zip");
        using var zip = new ZipArchive(res, ZipArchiveMode.Read);
        return ValidateKit(zip);
    }

    internal static System.Collections.Generic.HashSet<string> ValidateKit(ZipArchive zip)
    {
        const string prefix = "EVBots\\";
        var set = new System.Collections.Generic.HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var e in zip.Entries)
        {
            var full = e.FullName.Replace('/', '\\');
            if (e.Name.Length == 0 || !full.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)) continue;
            var rel = full.Substring(prefix.Length);
            if (Path.IsPathRooted(rel) || rel.IndexOf(':') >= 0 ||
                rel.Split('\\').Any(part => part.Length == 0 || part == "." || part == ".." ||
                    part.EndsWith(".", StringComparison.Ordinal) || part.EndsWith(" ", StringComparison.Ordinal) ||
                    part.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0))
                throw new InvalidDataException("Недопустимый путь в комплекте: " + e.FullName);
            if (!set.Add(rel)) throw new InvalidDataException("Повторный файл в комплекте: " + e.FullName);
            if (e.Length == 0) throw new InvalidDataException("Пустой файл в комплекте: " + e.FullName);
            // The catalog is program data; accounts and player statistics are not.
            // Never let an accidentally packaged user file overwrite it during Extract.
            var isCatalog = rel.Equals("progress\\bf3-stats-catalog.json", StringComparison.OrdinalIgnoreCase);
            if (!isCatalog && (KeepDirs.Any(d => rel.StartsWith(d, StringComparison.OrdinalIgnoreCase)) ||
                KeepFiles.Contains(rel, StringComparer.OrdinalIgnoreCase)))
                throw new InvalidDataException("В комплект попали пользовательские данные: " + e.FullName);
        }
        foreach (var required in new[] { "EV Launcher.exe", "activation\\EVBots.dll", "activation\\lua.dll",
            "activation\\ev-core.dll", "backend\\EV Backend.exe" })
            if (!set.Contains(required)) throw new InvalidDataException("Неполный комплект: отсутствует " + required);
        return set;
    }

    /// <summary>Бэкап заменяемых файлов и уборка лишних перед распаковкой нового комплекта поверх старого.</summary>
    internal static void PrepareUpdate(string dst, System.Collections.Generic.HashSet<string> kit, Action<string> say)
    {
        var root = Path.GetFullPath(dst).TrimEnd('\\') + "\\";
        var backupRoot = Path.Combine(dst, "backup");
        var backup = Path.Combine(backupRoot, "update-" + DateTime.Now.ToString("yyyyMMdd-HHmmss"));
        int saved = 0, removed = 0;
        foreach (var file in UpdateFiles(dst, root))
        {
            var rel = Path.GetFullPath(file).Substring(root.Length);
            if (KeepDirs.Any(d => rel.StartsWith(d, StringComparison.OrdinalIgnoreCase))) continue;
            if (KeepFiles.Any(f => rel.Equals(f, StringComparison.OrdinalIgnoreCase))) continue;
            // Другие моды в этой папке принадлежат пользователю. Три каталога
            // Royale устанавливаются целиком, включая удаление старых файлов из активного набора.
            if (rel.StartsWith("Mods\\", StringComparison.OrdinalIgnoreCase) && !kit.Contains(rel) &&
                !RoyaleInstanceConfig.Mods.Any(name => rel.StartsWith("Mods\\" + name + "\\", StringComparison.OrdinalIgnoreCase))) continue;
            if (rel.Equals(Uninstall.Marker, StringComparison.OrdinalIgnoreCase) ||
                rel.Equals(Uninstall.UninstallerName, StringComparison.OrdinalIgnoreCase)) continue;  // перезапишутся ниже
            var to = Path.Combine(backup, rel);
            Directory.CreateDirectory(Path.GetDirectoryName(to)!);
            if (kit.Contains(rel))
            {
                File.Copy(file, to, true);  // будет заменён новым
                saved++;
            }
            else
            {
                File.Move(file, to);        // в новом комплекте его нет — убираем из установки
                removed++;
                say("убран устаревший файл: " + rel);
            }
        }
        // пустые папки, оставшиеся после уборки (кроме данных игрока)
        PruneEmptyUpdateDirs(dst, root);
        say($"обновление: в бэкап {saved} заменяемых файлов, убрано устаревших {removed}; бэкап — {backup}");
        // два последних бэкапа обновления
        if (Directory.Exists(backupRoot))
            foreach (var old in Directory.GetDirectories(backupRoot, "update-*").OrderByDescending(d => d).Skip(2))
                try { Uninstall.DeleteTreeWithoutLinks(old); } catch (Exception) { }
    }

    private static bool SkipUpdateDirectory(string dir, string root)
    {
        if ((File.GetAttributes(dir) & FileAttributes.ReparsePoint) != 0) return true;
        var rel = Path.GetFullPath(dir).TrimEnd('\\').Substring(root.Length) + "\\";
        return KeepDirs.Any(d => rel.StartsWith(d, StringComparison.OrdinalIgnoreCase));
    }

    // Do not even enumerate the game's Data junction or preserved accounts.
    private static System.Collections.Generic.IEnumerable<string> UpdateFiles(string dir, string root)
    {
        foreach (var file in Directory.GetFiles(dir))
            if ((File.GetAttributes(file) & FileAttributes.ReparsePoint) == 0) yield return file;
        foreach (var child in Directory.GetDirectories(dir))
            if (!SkipUpdateDirectory(child, root))
                foreach (var file in UpdateFiles(child, root)) yield return file;
    }

    private static void PruneEmptyUpdateDirs(string dir, string root)
    {
        foreach (var child in Directory.GetDirectories(dir))
        {
            if (SkipUpdateDirectory(child, root)) continue;
            PruneEmptyUpdateDirs(child, root);
            if (!Directory.EnumerateFileSystemEntries(child).Any()) Directory.Delete(child);
        }
    }

    // 26.10: папки установок до переименования (Legacy → EVBots, 20.10) рядом с играми
    private static readonly string[] LegacyDirs = { "LegacyFriend", "BF3LegacyLan", "Legacy" };

    /// <summary>
    /// 26.10: старая установка под прежним названием (LegacyFriend и др., свой активатор activation-loader/legacyext) рядом с
    /// играми. Профили (аккаунты, статистика, прогресс) переносятся в EVBots\profiles, если там ещё нет accounts.json; сама
    /// папка уходит целиком в EVBots\backup\legacy-&lt;имя&gt;-&lt;время&gt; (не удаляется), ярлыки рабочего стола на неё — туда же.
    /// Папку считаем старой установкой, только если в ней есть profiles\ и activation\ или payload\.
    /// </summary>
    internal static void MigrateLegacy(string target, string dst, Action<string> say)
    {
        foreach (var name in LegacyDirs)
        {
            var old = Path.Combine(target, name);
            if (!Directory.Exists(old)) continue;
            var oldProfiles = Path.Combine(old, "profiles");
            if (!Directory.Exists(oldProfiles) ||
                !(Directory.Exists(Path.Combine(old, "activation")) || Directory.Exists(Path.Combine(old, "payload"))))
            {
                say($"папка «{name}» не похожа на старую установку — не трогаю");
                continue;
            }
            var profiles = Path.Combine(dst, "profiles");
            if (File.Exists(Path.Combine(profiles, "accounts.json")))
                say($"старая установка «{name}»: в EVBots уже есть свои аккаунты — профили оттуда не переношу (лежат в бэкапе)");
            else
            {
                var n = CopyTree(oldProfiles, profiles);
                var progress = Path.Combine(old, "progress");
                if (Directory.Exists(progress)) n += CopyTree(progress, Path.Combine(dst, "progress"));
                say($"старая установка «{name}»: перенесено профилей (файлов) {n} в {profiles}");
            }
            var oldSettings = Path.Combine(old, "friend-settings.json");
            var newSettings = Path.Combine(dst, "friend-settings.json");
            if (File.Exists(oldSettings) && !File.Exists(newSettings) &&
                (File.GetAttributes(oldSettings) & FileAttributes.ReparsePoint) == 0)
                File.Copy(oldSettings, newSettings);
            var oldLicense = Path.Combine(old, "license");
            if (Directory.Exists(oldLicense)) CopyTree(oldLicense, Path.Combine(dst, "license"));
            var stamp = DateTime.Now.ToString("yyyyMMdd-HHmmss");
            var to = Path.Combine(dst, "backup", "legacy-" + name + "-" + stamp);
            Directory.CreateDirectory(Path.Combine(dst, "backup"));
            try
            {
                Directory.Move(old, to);
                say($"старая установка «{name}» убрана из папки игр → {to}");
            }
            catch (Exception ex)
            {
                say($"старую папку «{name}» убрать не вышло ({ex.Message}) — закрой её программы и удали вручную");
                continue;
            }
            RemoveShortcutsTo(old, Path.Combine(to, "desktop-shortcuts"), say);
        }
    }

    private static int CopyTree(string from, string to)
    {
        var root = Path.GetFullPath(from).TrimEnd('\\') + "\\";
        int n = 0;
        if ((File.GetAttributes(from) & FileAttributes.ReparsePoint) != 0) return 0;
        foreach (var file in UpdateFiles(from, root))
        {
            var relative = Path.GetFullPath(file).Substring(root.Length);
            CheckDestinationLinks(to, new[] { relative });
            var dest = Path.Combine(to, relative);
            if (File.Exists(dest)) continue;  // своё в EVBots не перезаписываем
            Directory.CreateDirectory(Path.GetDirectoryName(dest)!);
            File.Copy(file, dest);
            n++;
        }
        return n;
    }

    /// <summary>Ярлыки рабочего стола, ведущие в старую папку, — в бэкап (через WScript.Shell, как CreateShortcut).</summary>
    private static void RemoveShortcutsTo(string oldDir, string backup, Action<string> say)
    {
        var prefix = Path.GetFullPath(oldDir).TrimEnd('\\') + "\\";
        try
        {
            var type = Type.GetTypeFromProgID("WScript.Shell");
            if (type is null) return;
            var shell = Activator.CreateInstance(type);
            var desktop = Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory);
            foreach (var lnk in Directory.GetFiles(desktop, "*.lnk"))
            {
                var sc = type.InvokeMember("CreateShortcut", System.Reflection.BindingFlags.InvokeMethod, null, shell, new object[] { lnk });
                var path = sc!.GetType().InvokeMember("TargetPath", System.Reflection.BindingFlags.GetProperty, null, sc, null) as string;
                if (string.IsNullOrEmpty(path) || !path!.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)) continue;
                Directory.CreateDirectory(backup);
                File.Move(lnk, Path.Combine(backup, Path.GetFileName(lnk)));
                say("убран старый ярлык: " + Path.GetFileName(lnk));
            }
        }
        catch (Exception ex) { say("ярлыки старой установки не проверены: " + ex.Message); }
    }

    /// <summary>Распаковка вшитого kit.zip: берём только записи внутри папки EVBots\… (разделители / и \ оба понимаем).</summary>
    internal static void Extract(string dst, IProgress<int> progress)
    {
        using var res = typeof(SetupForm).Assembly.GetManifestResourceStream("kit.zip")
            ?? throw new InvalidOperationException("в установщике нет вшитого kit.zip");
        using var zip = new ZipArchive(res, ZipArchiveMode.Read);
        ValidateKit(zip);
        const string prefix = "EVBots\\";
        var files = zip.Entries
            .Where(e => e.Name.Length > 0 && e.FullName.Replace('/', '\\').StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
            .ToList();
        var done = 0;
        foreach (var e in files)
        {
            var rel = e.FullName.Replace('/', '\\').Substring(prefix.Length);
            var to = Path.GetFullPath(Path.Combine(dst, rel));
            var root = Path.GetFullPath(dst).TrimEnd('\\') + "\\";
            if (!to.StartsWith(root, StringComparison.OrdinalIgnoreCase) || rel.IndexOf(':') >= 0)
                throw new InvalidDataException("Недопустимый путь в комплекте: " + e.FullName);
            Directory.CreateDirectory(Path.GetDirectoryName(to)!);
            using (var src = e.Open())
            using (var fs = File.Create(to))
                src.CopyTo(fs);
            progress.Report(++done * 95 / files.Count);
        }
    }

    /// <summary>Ярлык через WScript.Shell (без лишних ссылок: вызовы по имени).</summary>
    private static void CreateShortcut(string lnk, string target, string workDir)
    {
        var type = Type.GetTypeFromProgID("WScript.Shell") ?? throw new InvalidOperationException("нет WScript.Shell");
        var shell = Activator.CreateInstance(type);
        var sc = type.InvokeMember("CreateShortcut", System.Reflection.BindingFlags.InvokeMethod, null, shell, new object[] { lnk });
        var st = sc!.GetType();
        st.InvokeMember("TargetPath", System.Reflection.BindingFlags.SetProperty, null, sc, new object[] { target });
        st.InvokeMember("WorkingDirectory", System.Reflection.BindingFlags.SetProperty, null, sc, new object[] { workDir });
        st.InvokeMember("IconLocation", System.Reflection.BindingFlags.SetProperty, null, sc, new object[] { target + ",0" });
        st.InvokeMember("Description", System.Reflection.BindingFlags.SetProperty, null, sc, new object[] { "EV Launcher: Battlefield 3 Battle Royale LAN" });
        st.InvokeMember("Save", System.Reflection.BindingFlags.InvokeMethod, null, sc, null);
    }
}
