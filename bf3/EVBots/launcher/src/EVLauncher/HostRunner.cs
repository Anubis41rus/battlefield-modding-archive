// Бэкенд и сервер запускаем напрямую (без скриптов комплекта).
// Аргументы и env — как в scripts\host.ps1 комплекта друга.
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace EVBots.Launcher;

internal sealed class HostRunner
{
    private Process? _backend;
    private Process? _loader;

    public bool BackendAlive => _backend is not null && !_backend.HasExited;
    public bool ServerAlive => _loader is not null && !_loader.HasExited;

    public static bool ClientRunning() => ProcessesRunning("bf3", "zbf3");
    public static bool AnyGameRunning() => ProcessesRunning("bf3", "zbf3", "bf3_server_Final");

    private static bool ProcessesRunning(params string[] names)
    {
        foreach (var name in names)
        {
            var processes = Process.GetProcessesByName(name);
            var found = processes.Length > 0;
            foreach (var process in processes) process.Dispose();
            if (found) return true;
        }
        return false;
    }

    public void StopBackend(FoundPaths paths, Action<string> log)
    {
        if (ServerAlive || AnyGameRunning())
            throw new InvalidOperationException("Сначала закрой игру и останови сервер. Работающий матч не будет прерван.");
        var expected = Path.GetFullPath(Path.Combine(paths.StandDir, "backend", "EV Backend.exe"));
        foreach (var process in Process.GetProcessesByName("EV Backend"))
            using (process)
            {
                const uint processQueryLimitedInformation = 0x1000;
                using var handle = OpenProcess(processQueryLimitedInformation, false, process.Id);
                if (handle.IsInvalid) continue;
                // Лаунчер x86, бэкенд x64. Путь exe доступен без перечисления модулей процесса.
                var imagePath = new StringBuilder(32768);
                var length = (uint)imagePath.Capacity;
                if (!QueryFullProcessImageName(handle, 0, imagePath, ref length) ||
                    !string.Equals(imagePath.ToString(), expected, StringComparison.OrdinalIgnoreCase)) continue;
                try { process.Kill(); }
                catch (Win32Exception) when (process.HasExited) { continue; }
                if (!process.WaitForExit(5000)) throw new IOException("Бэкенд ещё останавливается.");
                log("Бэкенд остановлен перед новым запуском.");
            }
        _backend?.Dispose();
        _backend = null;
    }

    [DllImport("kernel32.dll", SetLastError = true, ExactSpelling = true)]
    private static extern SafeProcessHandle OpenProcess(uint desiredAccess,
        [MarshalAs(UnmanagedType.Bool)] bool inheritHandle, int processId);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, EntryPoint = "QueryFullProcessImageNameW",
        SetLastError = true, ExactSpelling = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool QueryFullProcessImageName(SafeProcessHandle process, uint flags,
        StringBuilder imagePath, ref uint length);

    public static string? PortBusy(int port, bool tcp)
    {
        try
        {
            if (tcp) { using var l = new TcpListener(IPAddress.Any, port); l.Start(); l.Stop(); }
            else { using var u = new UdpClient(port); }
            return null;
        }
        catch (SocketException) { return "занят"; }
    }

    /// <summary>Папка с profiles и progress: стенд разработки (EVBots-dev\stand рядом с корнем), если он есть, иначе StandDir.</summary>
    private static string AccountsHome(FoundPaths p)
    {
        foreach (var root in new[] { p.Root, Path.GetDirectoryName(p.StandDir) ?? "" })
        {
            if (root.Length == 0) continue;
            var dev = Path.Combine(root, "EVBots-dev", "stand");
            if (Directory.Exists(Path.Combine(dev, "profiles")) && File.Exists(Path.Combine(dev, "progress", "bf3-stats-catalog.json")))
                return dev;
        }
        return p.StandDir;
    }

    public void StartBackend(FoundPaths p, string advertiseIp, string serverName, Action<string> log,
        int frequency = 30, string? modsDirectory = null, string? instanceDirectory = null, string? sourceServerDir = null)
    {
        var backend = Path.Combine(p.StandDir, "backend", "EV Backend.exe");
        var psi = new ProcessStartInfo
        {
            FileName = backend,
            WorkingDirectory = Path.Combine(p.StandDir, "backend"),
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8,
        };
        psi.ArgumentList.Add("serve");
        psi.ArgumentList.Add("--redirector-port"); psi.ArgumentList.Add("42127");
        psi.ArgumentList.Add("--blaze-port"); psi.ArgumentList.Add("10041");
        psi.ArgumentList.Add("--http-port"); psi.ArgumentList.Add("8080");
        // 19.10: на машине разработки (рядом есть EVBots-dev\stand\profiles) аккаунты и прогресс — общие со стендом
        // (run\start.ps1). Иначе лаунчер брал старый BF3NativeBots\profiles: тот же логин, но уровень от 10.10.
        var data = AccountsHome(p);
        if (!string.Equals(data, p.StandDir, StringComparison.OrdinalIgnoreCase))
            log("Аккаунты и прогресс — общие со стендом: " + data);
        psi.ArgumentList.Add("--profiles-dir"); psi.ArgumentList.Add(Path.Combine(data, "profiles"));
        psi.ArgumentList.Add("--progress-dir"); psi.ArgumentList.Add(Path.Combine(data, "progress"));
        psi.ArgumentList.Add("--stats-catalog"); psi.ArgumentList.Add(Path.Combine(data, "progress", "bf3-stats-catalog.json"));
        psi.ArgumentList.Add("--instance"); psi.ArgumentList.Add(instanceDirectory ?? p.InstanceDir);
        if (!string.IsNullOrWhiteSpace(modsDirectory))
        { psi.ArgumentList.Add("--mods-dir"); psi.ArgumentList.Add(modsDirectory); }
        var portable = Path.GetDirectoryName(sourceServerDir ?? p.ServerDir);
        if (!string.IsNullOrEmpty(portable))
        {
            var config = Path.Combine(portable, @"htdocs\easo\editorial\Battlefield\3\config\pc\server.xml");
            if (File.Exists(config))
            { psi.ArgumentList.Add("--editorial-file"); psi.ArgumentList.Add(config); }
        }
        psi.ArgumentList.Add("--evbots-log"); psi.ArgumentList.Add(Path.Combine(p.StandDir, "logs", "EVBots.log"));
        psi.ArgumentList.Add("--server-name"); psi.ArgumentList.Add(serverName);
        // 26.10: частота сервера — в списке /servers, клиенты друзей ставят ту же (как у VU)
        psi.ArgumentList.Add("--frequency"); psi.ArgumentList.Add(NormFrequency(frequency).ToString());
        if (advertiseIp.Length > 0)
        {
            psi.ArgumentList.Add("--advertise-ip"); psi.ArgumentList.Add(advertiseIp);
            psi.ArgumentList.Add("--game-server-address"); psi.ArgumentList.Add(advertiseIp + ":25300");
        }
        _backend = new Process { StartInfo = psi, EnableRaisingEvents = true };
        _backend.OutputDataReceived += (_, e) => { if (e.Data is not null) log("[backend] " + e.Data); };
        _backend.ErrorDataReceived += (_, e) => { if (e.Data is not null) log("[backend-err] " + e.Data); };
        _backend.Start();
        _backend.BeginOutputReadLine();
        _backend.BeginErrorReadLine();
        log($"backend запущен, PID {_backend.Id}");
    }

    /// <summary>Пароль RCON сервера (только 127.0.0.1:47900); им же входит RconRules.</summary>
    public const string RconPassword = "change-this-rcon-password";

    /// <summary>26.10: частота движка — только 30, 60 или 120 Гц (режимы VU regular/high60/high120), иначе 30.</summary>
    public static int NormFrequency(int hz) => hz is 60 or 120 ? hz : 30;

    public void StartServer(FoundPaths p, string serverName, string map, string mode, Action<string> log, int frequency, string activeMods, string modsDirectory)
    {
        if (ServerAlive || AnyGameRunning())
            throw new InvalidOperationException("Уже запущена игра или сервер. Новый сервер не запущен.");
        // 23.10: свой evbots-loader.exe; в старой установке без него — activation-loader.exe
        var loader = EVBots.Core.Paths.FindLoader(Path.Combine(p.StandDir, "activation"))
                     ?? Path.Combine(p.StandDir, "activation", EVBots.Core.Paths.LoaderName);
        var psi = new ProcessStartInfo
        {
            FileName = loader,
            WorkingDirectory = p.StandDir,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8,
        };
        psi.Environment["BF3ACT_MODE"] = "dedicated";
        psi.Environment["BF3ACT_DRM"] = "0";
        psi.Environment["BF3ACT_LOG"] = Path.Combine(p.StandDir, "logs", "EVBots.log");
        // 20.10: + bf3.gos.ea.com — server.xml отдаёт наш бэкенд на 127.0.0.1:80 (как Apache у BF-Portable)
        psi.Environment["BF3ACT_DNSREDIR"] = EVBots.Core.Ports.ServerSinkhole;
        psi.Environment["EVBOTS_BRIDGE"] = "1";
        psi.Environment["EVBOTS_BRIDGE_ENGINE"] = "1";
        psi.Environment["EVBOTS_SPM_PROBE"] = "0";
        psi.Environment["EVBOTS_SPM_MGR"] = "1";
        psi.Environment["EVBOTS_SPM_TICK"] = "1";
        psi.Environment["EVBOTS_SPM_SOLDIER"] = "1";
        psi.Environment["EVBOTS_SPM_SQUAD"] = "0";
        psi.Environment["EVBOTS_SPM_ONCREATE"] = "0";
        psi.Environment["EVBOTS_SPM_CREATE"] = "0";
        psi.Environment["EVBOTS_LIVE_SEARCH"] = "1";
        psi.Environment["EVBOTS_TESTS"] = "0";
        psi.Environment["EVBOTS_MODS"] = modsDirectory;
        psi.Environment["EVBOTS_ACTIVE_MODS"] = activeMods;
        psi.Environment["EVBOTS_SETTINGS_DIR"] = Path.Combine(p.StandDir, "mod-settings");
        // 26.10: частота движка (ev-core, EvFreq.cpp): 30 — без изменений BF3, 60/120 — как -high60/-high120 у VU
        psi.Environment["EVBOTS_FREQUENCY"] = NormFrequency(frequency).ToString();
        // 18.10: EVBOTS_LEVEL_NAME/GAMEMODE не задаём — с ними мост шлёт Level:Loaded сразу, до загрузки уровня и его
        // флагов (у мода 0 флагов, авиация без цели). Имя уровня и режим мост читает из движка, как стенд run\start.ps1.
        var ebx = Path.Combine(p.StandDir, "activation", "ebx_index.bin");
        if (File.Exists(ebx)) psi.Environment["EVBOTS_EBX_INDEX"] = ebx;
        // 23.10: ev-core.dll (ядро, аналог vu-core: окно сервера, канал модов 7948) — перед EVBots.dll; lua.dll EVBots.dll
        // берёт сама из своей папки. В старой установке без ev-core — только EVBots.dll.
        var hasCore = File.Exists(Path.Combine(p.StandDir, "activation", "ev-core.dll"));
        foreach (var a in new[]
        {
            "--dedicated", "--game", p.ServerDir, "--exe", "bf3_server_Final.exe",
        }.Concat(hasCore ? new[] { "--dll", "ev-core.dll" } : Array.Empty<string>()).Concat(new[]
        {
            "--dll", "EVBots.dll",
            "--log", Path.Combine(p.StandDir, "logs", "server-loader.log"),
            "--wait", "--",
            "-serverInstancePath", p.InstanceDir,
            "-Server.ServerName", serverName, "-GamePort", "25300",
            "-Network.MaxClientCount", "64", "-RemoteAdminPort", "127.0.0.1:47900",
            "-CpuTiming.Enable", "false",
            "+admin.password", RconPassword, "-PingSite", "ams", "-Region", "EU",
            "-Login", "bf3_1", "-Password", "password",
            "-Server.ActiveLicenses", "premium_xp4,xp2_ks,xp3_ks,xp4_ks,xp5_ks,ea_nfs12,le,premium,pwp",
        })) psi.ArgumentList.Add(a);
        _loader = new Process { StartInfo = psi, EnableRaisingEvents = true };
        _loader.OutputDataReceived += (_, e) => { if (e.Data is not null) log("[server] " + e.Data); };
        _loader.ErrorDataReceived += (_, e) => { if (e.Data is not null) log("[server-err] " + e.Data); };
        _loader.Start();
        _loader.BeginOutputReadLine();
        _loader.BeginErrorReadLine();
        log($"сервер запускается, PID загрузчика {_loader.Id}");
    }

    public void StopAll(Action<string> log)
    {
        foreach (var n in new[] { "bf3_server_Final", "EV Backend" })
            foreach (var pr in Process.GetProcessesByName(n))
                try { pr.Kill(); log($"остановлен {n} (PID {pr.Id})"); }
                catch (Exception ex) { log($"не остановился {n}: {ex.Message}"); }
        _backend = null;
        _loader = null;
    }
}
