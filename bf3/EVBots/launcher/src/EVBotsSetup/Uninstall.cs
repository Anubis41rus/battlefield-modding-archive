// Удаление EV Bots (20.10.2026). Запуск: «Удалить EVBots.exe» в папке EVBots или «Удалить» в «Установленных приложениях»
// (EVBots-Setup.exe /uninstall "<папка EVBots>"). Удаляет только папку с меткой установки (.evbots-install), ярлык,
// запись в списке программ и правила брандмауэра (одно окно UAC). Аккаунты и прогресс (profiles, progress) — по вопросу:
// у хоста там аккаунты друзей. Игру, BF-Portable и мод не трогает.
// Тихий режим для проверки: /uninstall "<папка>" /quiet [/all] — без окон и без UAC (брандмауэр не трогает).
using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Windows.Forms;
using Microsoft.Win32;

namespace EVBots.Setup;

internal static class Uninstall
{
    public const string Marker = ".evbots-install";
    public const string UninstallerName = "Удалить EVBots.exe";
    public const string RegKey = @"Software\Microsoft\Windows\CurrentVersion\Uninstall\EVBots";
    private static readonly string[] KeepDirs = { "profiles", "progress" };

    /// <summary>Запись «EV Bots» в «Установленные приложения» (раздел текущего пользователя — без прав админа).</summary>
    public static void Register(string dir, string version)
    {
        using var k = Registry.CurrentUser.CreateSubKey(RegKey);
        if (k == null) return;
        var un = Path.Combine(dir, UninstallerName);
        k.SetValue("DisplayName", "EV Bots — Battlefield 3 с ботами");
        k.SetValue("DisplayVersion", version);
        k.SetValue("Publisher", "EVBots");
        k.SetValue("InstallLocation", dir);
        k.SetValue("DisplayIcon", Path.Combine(dir, "EV Launcher.exe"));
        k.SetValue("UninstallString", "\"" + un + "\" /uninstall \"" + dir + "\"");
        k.SetValue("QuietUninstallString", "\"" + un + "\" /uninstall \"" + dir + "\" /quiet");
        k.SetValue("NoModify", 1, RegistryValueKind.DWord);
        k.SetValue("NoRepair", 1, RegistryValueKind.DWord);
        try
        {
            var kb = Directory.GetFiles(dir, "*", SearchOption.AllDirectories).Sum(f => new FileInfo(f).Length) / 1024;
            k.SetValue("EstimatedSize", (int)Math.Min(kb, int.MaxValue), RegistryValueKind.DWord);
        }
        catch (Exception) { }
    }

    public static int Run(string[] args)
    {
        var quiet = args.Any(a => a.Equals("/quiet", StringComparison.OrdinalIgnoreCase));
        var all = args.Any(a => a.Equals("/all", StringComparison.OrdinalIgnoreCase));
        var i = Array.FindIndex(args, a => a.Equals("/uninstall", StringComparison.OrdinalIgnoreCase));
        var dir = i >= 0 && i + 1 < args.Length && !args[i + 1].StartsWith("/") ? args[i + 1] : SetupForm.OwnFolder;
        dir = Path.GetFullPath(dir.TrimEnd('\\', '/'));

        if (!File.Exists(Path.Combine(dir, Marker)))
        {
            Say(quiet, "Это не папка установки EV Bots (нет метки " + Marker + "):\n" + dir + "\n\nНичего не удалено.", true);
            return 2;
        }

        // Из самой папки себя не удалить — копия во временную папку и перезапуск оттуда.
        var self = Path.GetFullPath(Application.ExecutablePath);
        if (self.StartsWith(dir + "\\", StringComparison.OrdinalIgnoreCase))
        {
            var tmp = Path.Combine(Path.GetTempPath(), "evbots-uninstall-" + Guid.NewGuid().ToString("N").Substring(0, 8) + ".exe");
            File.Copy(self, tmp, true);
            var rest = "/uninstall \"" + dir + "\"" + (quiet ? " /quiet" : "") + (all ? " /all" : "");
            // не ждём: этот процесс сам лежит в папке и держит свой exe
            Process.Start(new ProcessStartInfo(tmp, rest) { UseShellExecute = false, WorkingDirectory = Path.GetTempPath() });
            return 0;
        }

        try { return RunFromTemp(dir, quiet, all); }
        finally
        {
            // временная копия удаляет себя после выхода
            if (Path.GetFileName(self).StartsWith("evbots-uninstall-", StringComparison.OrdinalIgnoreCase))
            {
                try
                {
                    Process.Start(new ProcessStartInfo("cmd.exe", "/d /c ping -n 3 127.0.0.1 >nul & del /f /q \"" + self + "\"")
                        { WindowStyle = ProcessWindowStyle.Hidden, CreateNoWindow = true, UseShellExecute = false });
                }
                catch (Exception) { }
            }
        }
    }

    private static int RunFromTemp(string dir, bool quiet, bool all)
    {
        // Запущенные из этой папки лаунчер, бэкенд или сервер держат файлы. Свой деинсталлятор (он только что вышел) не считаем.
        System.Threading.Thread.Sleep(500);
        var busy = Process.GetProcesses().Where(pr =>
        {
            try
            {
                var f = pr.MainModule?.FileName;
                return f != null && f.StartsWith(dir + "\\", StringComparison.OrdinalIgnoreCase) &&
                       !string.Equals(Path.GetFileName(f), UninstallerName, StringComparison.OrdinalIgnoreCase);
            }
            catch (Exception) { return false; }
        }).Select(pr => pr.ProcessName).Distinct().ToList();
        if (busy.Count > 0)
        {
            Say(quiet, "Сначала закрой: " + string.Join(", ", busy) + ".\nПотом запусти удаление ещё раз.", true);
            return 3;
        }

        var keep = !all;
        if (!quiet)
        {
            if (MessageBox.Show("Удалить EV Bots из папки\n" + dir + " ?\n\nИгра, BF-Portable и мод fun-bots останутся.",
                    "Удаление EV Bots", MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes)
                return 1;
            keep = MessageBox.Show("Сохранить аккаунты и прогресс (папки profiles и progress)?\n\n" +
                                   "«Да» — оставить: при новой установке всё вернётся.\n" +
                                   "«Нет» — удалить совсем (у хоста там и аккаунты друзей).",
                       "Удаление EV Bots", MessageBoxButtons.YesNo, MessageBoxIcon.Question) == DialogResult.Yes;
        }

        var errors = 0;
        foreach (var d in Directory.GetDirectories(dir))
        {
            if (keep && KeepDirs.Contains(Path.GetFileName(d), StringComparer.OrdinalIgnoreCase)) continue;
            if (!Retry(() => DeleteTreeWithoutLinks(d))) errors++;
        }
        foreach (var f in Directory.GetFiles(dir))
        {
            if (!Retry(() => { File.SetAttributes(f, FileAttributes.Normal); File.Delete(f); })) errors++;
        }
        if (!keep) { try { Directory.Delete(dir, false); } catch (Exception) { } }

        // ярлык на рабочем столе — только если он ведёт в эту папку
        try
        {
            var lnk = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), "EV Launcher.lnk");
            if (File.Exists(lnk) && ShortcutTarget(lnk).StartsWith(dir + "\\", StringComparison.OrdinalIgnoreCase)) File.Delete(lnk);
        }
        catch (Exception) { }

        try
        {
            using var k = Registry.CurrentUser.OpenSubKey(RegKey);
            var loc = k?.GetValue("InstallLocation") as string;
            if (k != null && string.Equals(loc?.TrimEnd('\\'), dir, StringComparison.OrdinalIgnoreCase))
            {
                k.Dispose();
                Registry.CurrentUser.DeleteSubKeyTree(RegKey, false);
            }
        }
        catch (Exception) { }

        var fw = quiet ? "" : RemoveFirewall();

        Say(quiet, "EV Bots удалён." +
                   (keep ? "\nАккаунты и прогресс оставлены в:\n" + dir : "") +
                   (errors > 0 ? "\n\nНе удалось удалить файлов: " + errors + " (заняты?). Удали папку вручную:\n" + dir : "") +
                   (fw.Length > 0 ? "\n\n" + fw : ""), errors > 0);
        return errors > 0 ? 4 : 0;
    }

    // Royale links to the already installed game. Remove the link itself,
    // never enumerate or change attributes on the external game directory.
    internal static void DeleteTreeWithoutLinks(string directory)
    {
        if ((File.GetAttributes(directory) & FileAttributes.ReparsePoint) != 0)
        {
            Directory.Delete(directory, false);
            return;
        }
        foreach (var child in Directory.GetDirectories(directory)) DeleteTreeWithoutLinks(child);
        foreach (var file in Directory.GetFiles(directory)) File.Delete(file);
        Directory.Delete(directory, false);
    }

    /// <summary>До 5 с попыток: только что вышедший деинсталлятор ещё может держать свой exe.</summary>
    private static bool Retry(Action a)
    {
        for (var i = 0; i < 25; i++)
        {
            try { a(); return true; }
            catch (Exception) { System.Threading.Thread.Sleep(200); }
        }
        return false;
    }

    /// <summary>Правила брандмауэра и URL-ACL, которые ставил установщик. Одно окно UAC.</summary>
    private static string RemoveFirewall()
    {
        var cmds = new[]
        {
            "netsh advfirewall firewall delete rule name=\"EVBots-TCP\"",
            "netsh advfirewall firewall delete rule name=\"EVBots-UDP\"",
            "netsh advfirewall firewall delete rule name=\"EVBots client\"",
            "netsh advfirewall firewall delete rule name=\"EVBots server\"",
            "netsh http delete urlacl url=http://+:8080/",
            "netsh http delete urlacl url=http://127.0.0.1:8080/",
        }.Select(c => c + " >nul 2>&1").ToList();
        foreach (var ip in SetupForm.LanIps()) cmds.Add($"netsh http delete urlacl url=http://{ip}:8080/ >nul 2>&1");
        try
        {
            using var pr = Process.Start(new ProcessStartInfo("cmd.exe", "/d /c \"" + string.Join(" & ", cmds) + "\"")
                { Verb = "runas", UseShellExecute = true, WindowStyle = ProcessWindowStyle.Hidden });
            pr?.WaitForExit();
            return "Правила брандмауэра EVBots удалены.";
        }
        catch (System.ComponentModel.Win32Exception)
        {
            return "UAC отклонён: правила брандмауэра EVBots остались (они безвредны).";
        }
    }

    private static string ShortcutTarget(string lnk)
    {
        var type = Type.GetTypeFromProgID("WScript.Shell");
        if (type == null) return "";
        var shell = Activator.CreateInstance(type);
        var sc = type.InvokeMember("CreateShortcut", System.Reflection.BindingFlags.InvokeMethod, null, shell, new object[] { lnk });
        return sc?.GetType().InvokeMember("TargetPath", System.Reflection.BindingFlags.GetProperty, null, sc, null) as string ?? "";
    }

    private static void Say(bool quiet, string text, bool warn)
    {
        if (quiet) return;
        MessageBox.Show(text, "Удаление EV Bots", MessageBoxButtons.OK, warn ? MessageBoxIcon.Warning : MessageBoxIcon.Information);
    }
}
