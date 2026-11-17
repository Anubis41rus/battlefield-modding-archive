using System.Diagnostics;

namespace EVBots.Core;

/// <summary>
/// Работа с процессами. Остановка — только по Process-объекту, который лаунчер получил
/// от Process.Start (а не по имени и не по сохранённому PID: PID может переиспользоваться).
/// </summary>
public static class ProcessUtil
{
    /// <summary>Сколько процессов с таким именем сейчас запущено (чужие тоже считаются).</summary>
    public static int CountByName(string name)
    {
        try
        {
            var list = Process.GetProcessesByName(name);
            var count = list.Length;
            foreach (var p in list)
            {
                p.Dispose();
            }

            return count;
        }
        catch (Exception)
        {
            return 0;
        }
    }

    public static bool IsAlive(Process? p)
    {
        if (p is null)
        {
            return false;
        }

        try
        {
            return !p.HasExited;
        }
        catch (Exception)
        {
            return false;
        }
    }

    /// <summary>Снимает процесс вместе с дочерними (загрузчик с --wait держит bf3_server_Final).</summary>
    public static void KillTree(Process? p)
    {
        if (!IsAlive(p))
        {
            return;
        }

        try
        {
            p!.Kill(entireProcessTree: true);
            p.WaitForExit(15000);
        }
        catch (Exception)
        {
            // уже завершился
        }
    }

    /// <summary>Самый свежий RuntimeLog_<ПК>_server.log (без ротационных .1, .2 …).</summary>
    public static string? NewestRuntimeLog(string dir) => NewestFile(dir, "RuntimeLog_*_server.log");

    /// <summary>Самый свежий файл по маске в папке (или null).</summary>
    public static string? NewestFile(string dir, string pattern)
    {
        try
        {
            if (!Directory.Exists(dir))
            {
                return null;
            }

            return new DirectoryInfo(dir)
                .EnumerateFiles(pattern)
                .OrderByDescending(f => f.LastWriteTimeUtc)
                .Select(f => f.FullName)
                .FirstOrDefault();
        }
        catch (Exception)
        {
            return null;
        }
    }
}
