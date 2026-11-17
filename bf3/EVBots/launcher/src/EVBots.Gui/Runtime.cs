using System.Diagnostics;
using System.IO;
using System.Text;
using EVBots.Core;

namespace EVBots.Gui;

/// <summary>
/// Процессы, которые запустил сам лаунчер. Бэкенд и сервер стартуют через cmd.exe с выводом в файл
/// (cmd /d /s /c "exe args >> log 2>&amp;1"): так они не держат пайпы лаунчера и переживают его закрытие.
/// Вход в игру — напрямую, без перенаправления (игра не должна зависеть от лаунчера).
/// Остановка — только по Process-объектам, которые вернул Process.Start здесь.
/// </summary>
public sealed class Runtime
{
    public Process? Backend { get; private set; }
    public Process? ServerLoader { get; private set; }
    public Process? Client { get; private set; }
    public string? BackendLogPath { get; private set; }
    public string? ServerLogPath { get; private set; }

    public bool BackendOwned => ProcessUtil.IsAlive(Backend);
    public bool ServerOwned => ProcessUtil.IsAlive(ServerLoader);

    public (bool Ok, string Message) StartBackend(LauncherSettings settings, string advertiseIp)
    {
        try
        {
            var plan = ServerPlan.Backend(settings, advertiseIp);
            BackendLogPath = NewLogPath("backend");
            Backend = SpawnToFile(plan, BackendLogPath);
            return (true, $"Бэкенд запущен, PID {Backend.Id}, адрес для клиентов {advertiseIp}.");
        }
        catch (Exception ex)
        {
            return (false, "Бэкенд не запущен: " + ex.Message);
        }
    }

    public (bool Ok, string Message) StartServer(LauncherSettings settings)
    {
        try
        {
            var plan = ServerPlan.Server(settings, NewLogPath("loader"));
            ServerLogPath = NewLogPath("server");
            ServerLoader = SpawnToFile(plan, ServerLogPath);
            return (true, $"Сервер запускается, PID загрузчика {ServerLoader.Id}.");
        }
        catch (Exception ex)
        {
            return (false, "Сервер не запущен: " + ex.Message);
        }
    }

    public (bool Ok, string Message) StartClient(ProcessPlan plan)
    {
        if (ProcessUtil.CountByName("bf3") > 0)
        {
            return (false, "Игра bf3 уже запущена — второй клиент не запускаю.");
        }

        try
        {
            Client = Spawn(plan);
            return (true, "Вход в игру запущен (evbots-loader --client).");
        }
        catch (Exception ex)
        {
            return (false, "Вход не запущен: " + ex.Message);
        }
    }

    /// <summary>Останавливает сервер (и бэкенд, если includeBackend). Чужие процессы не трогает.</summary>
    public List<string> Stop(bool includeBackend)
    {
        var messages = new List<string>();
        if (ServerOwned)
        {
            ProcessUtil.KillTree(ServerLoader);
            messages.Add("сервер остановлен");
        }
        else
        {
            messages.Add("сервер не запущен лаунчером");
        }

        ServerLoader = null;

        if (includeBackend)
        {
            if (BackendOwned)
            {
                ProcessUtil.KillTree(Backend);
                messages.Add("бэкенд остановлен");
            }
            else
            {
                messages.Add("бэкенд не запущен лаунчером");
            }

            Backend = null;
        }

        return messages;
    }

    private static string NewLogPath(string tag) =>
        Path.Combine(Paths.LogsDir, $"{tag}-{DateTime.Now:yyyyMMdd-HHmmss}.log");

    private static Process SpawnToFile(ProcessPlan plan, string logPath)
    {
        Directory.CreateDirectory(Paths.LogsDir);
        File.AppendAllText(logPath, "# " + plan.Display() + Environment.NewLine, Encoding.UTF8);

        var inner = new StringBuilder();
        inner.Append(CmdQuote(plan.FileName));
        foreach (var arg in plan.Args)
        {
            inner.Append(' ').Append(CmdQuote(arg));
        }

        inner.Append(" >> ").Append(CmdQuote(logPath)).Append(" 2>&1");

        var psi = new ProcessStartInfo
        {
            FileName = "cmd.exe",
            Arguments = "/d /s /c \"" + inner + "\"",
            WorkingDirectory = plan.WorkDir,
            UseShellExecute = false,
            CreateNoWindow = true,
        };

        ApplyEnv(psi, plan);
        return Process.Start(psi) ?? throw new InvalidOperationException("процесс не стартовал");
    }

    private static Process Spawn(ProcessPlan plan)
    {
        var psi = new ProcessStartInfo
        {
            FileName = plan.FileName,
            WorkingDirectory = plan.WorkDir,
            UseShellExecute = false,
            CreateNoWindow = true,
        };

        foreach (var arg in plan.Args)
        {
            psi.ArgumentList.Add(arg);
        }

        ApplyEnv(psi, plan);
        return Process.Start(psi) ?? throw new InvalidOperationException("процесс не стартовал");
    }

    private static void ApplyEnv(ProcessStartInfo psi, ProcessPlan plan)
    {
        foreach (var kv in plan.Env)
        {
            if (kv.Value is null)
            {
                psi.Environment.Remove(kv.Key);
            }
            else
            {
                psi.Environment[kv.Key] = kv.Value;
            }
        }
    }

    /// <summary>
    /// Аргумент в двойных кавычках для cmd.exe. Кавычек внутри аргументов нет (их отсекает валидация),
    /// хвостовой обратный слэш удваиваем, чтобы он не съел закрывающую кавычку.
    /// </summary>
    private static string CmdQuote(string arg)
    {
        var tail = arg.EndsWith('\\') ? "\\" : "";
        return "\"" + arg + tail + "\"";
    }
}
